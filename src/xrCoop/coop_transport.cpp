// Anomaly Coop — private ENet wrapper implementation.
#include "coop_transport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <cstring>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

namespace coop
{
namespace net
{
namespace
{
constexpr size_t kChannels = size_t(Lane::Count);

std::mutex g_enet_mutex;
int g_enet_refs = 0;

bool enet_acquire()
{
	std::lock_guard<std::mutex> lock(g_enet_mutex);
	if (g_enet_refs == 0 && enet_initialize() != 0)
		return false;
	++g_enet_refs;
	return true;
}

void enet_release()
{
	std::lock_guard<std::mutex> lock(g_enet_mutex);
	if (g_enet_refs > 0 && --g_enet_refs == 0)
		enet_deinitialize();
}

void ENET_CALLBACK packet_free_cb(ENetPacket* p)
{
	if (!p || !p->userData)
		return;
	ByteCounterPtr* box = static_cast<ByteCounterPtr*>(p->userData);
	p->userData = nullptr;
	if (*box)
	{
		ByteCounter& c = **box;
		c.bytes = c.bytes >= p->dataLength ? c.bytes - p->dataLength : 0;
	}
	delete box;
}
} // namespace

// ---------------------------------------------------------------- Packet

Packet& Packet::operator=(Packet&& o) noexcept
{
	if (this != &o)
	{
		if (m_p)
			enet_packet_destroy(m_p);
		m_p = o.m_p;
		o.m_p = nullptr;
	}
	return *this;
}

Packet::~Packet()
{
	if (m_p)
		enet_packet_destroy(m_p);
}

size_t Packet::size() const
{
	return m_p ? m_p->dataLength : 0;
}

// ---------------------------------------------------------------- Transport

bool Transport::listen(uint16_t port, uint32_t max_peers, std::string* err)
{
	if (m_host)
	{
		if (err)
			*err = "transport already started";
		return false;
	}
	if (!enet_acquire())
	{
		if (err)
			*err = "enet_initialize failed";
		return false;
	}
	m_enet_ref = true;
	if (max_peers == 0)
		max_peers = 1;
	if (max_peers > ENET_PROTOCOL_MAXIMUM_PEER_ID)
		max_peers = ENET_PROTOCOL_MAXIMUM_PEER_ID;

	ENetAddress addr;
	addr.host = ENET_HOST_ANY;
	addr.port = port;
	m_host = enet_host_create(&addr, max_peers, kChannels, 0, 0);
	if (!m_host)
	{
		if (err)
			*err = "cannot bind UDP port " + std::to_string(unsigned(port)) + " (already in use?)";
		destroy();
		return false;
	}
	return true;
}

PeerHandle Transport::connect(const std::string& address, uint16_t port, std::string* err)
{
	if (m_host)
	{
		if (err)
			*err = "transport already started";
		return nullptr;
	}
	if (!enet_acquire())
	{
		if (err)
			*err = "enet_initialize failed";
		return nullptr;
	}
	m_enet_ref = true;

	ENetAddress addr;
	addr.port = port;
	if (enet_address_set_host(&addr, address.c_str()) != 0)
	{
		if (err)
			*err = "cannot resolve host '" + address + "'";
		destroy();
		return nullptr;
	}
	m_host = enet_host_create(nullptr, 1, kChannels, 0, 0);
	if (!m_host)
	{
		if (err)
			*err = "cannot create client socket";
		destroy();
		return nullptr;
	}
	ENetPeer* peer = enet_host_connect(m_host, &addr, kChannels, 0);
	if (!peer)
	{
		if (err)
			*err = "no free transport peer slot";
		destroy();
		return nullptr;
	}
	return peer;
}

void Transport::destroy()
{
	if (m_host)
	{
		enet_host_destroy(m_host);
		m_host = nullptr;
	}
	if (m_enet_ref)
	{
		m_enet_ref = false;
		enet_release();
	}
}

int Transport::service(Event& ev, uint32_t wait_ms)
{
	if (!m_host)
		return 0;
	ev = Event();
	ENetEvent e;
	const int rc = enet_host_service(m_host, &e, wait_ms);
	if (rc <= 0)
		return rc;

	ev.peer = e.peer;
	ev.channel = e.channelID;
	ev.data = e.data;
	switch (e.type)
	{
	case ENET_EVENT_TYPE_CONNECT:
		ev.type = Event::Type::Connect;
		break;
	case ENET_EVENT_TYPE_DISCONNECT:
		ev.type = Event::Type::Disconnect;
		break;
	case ENET_EVENT_TYPE_RECEIVE:
		ev.type = Event::Type::Receive;
		if (e.packet)
		{
			ev.bytes.assign(e.packet->data, e.packet->data + e.packet->dataLength);
			enet_packet_destroy(e.packet);
		}
		break;
	default:
		ev.type = Event::Type::None;
		break;
	}
	return 1;
}

void Transport::flush()
{
	if (m_host)
		enet_host_flush(m_host);
}

Packet Transport::make_packet(Lane lane, uint16_t type, const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
	const ByteCounterPtr& counter)
{
	Packet out;
	// Unreliable: sequenced per channel (no UNSEQUENCED flag). UNRELIABLE_FRAGMENT keeps packets larger than the MTU
	// unreliable too (the whole packet is dropped if one fragment is lost) instead of silently promoting them to reliable.
	const enet_uint32 flags = (lane == Lane::Unreliable) ? ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT : ENET_PACKET_FLAG_RELIABLE;
	ENetPacket* p = enet_packet_create(nullptr, 2 + na + nb, flags);
	if (!p)
		return out;
	p->data[0] = uint8_t(type);
	p->data[1] = uint8_t(type >> 8);
	if (na && a)
		std::memcpy(p->data + 2, a, na);
	if (nb && b)
		std::memcpy(p->data + 2 + na, b, nb);
	if (counter)
	{
		ByteCounterPtr* box = new ByteCounterPtr(counter);
		counter->bytes += p->dataLength;
		p->userData = box;
		p->freeCallback = packet_free_cb;
	}
	out.m_p = p;
	return out;
}

bool Transport::send(PeerHandle peer, Lane lane, Packet&& packet)
{
	ENetPacket* p = packet.m_p;
	packet.m_p = nullptr;
	if (!p)
		return false;
	if (!m_host || !peer || peer->state != ENET_PEER_STATE_CONNECTED
		|| enet_peer_send(peer, enet_uint8(lane), p) < 0)
	{
		enet_packet_destroy(p);
		return false;
	}
	return true;
}

void Transport::set_timeouts(PeerHandle peer, uint32_t timeout_ms)
{
	if (!peer)
		return;
	// ENet's congestion throttle drops a share of unreliable packets whenever the measured RTT spikes. In a game that
	// pumps the network from the main thread a frame hitch (level load, GC) looks exactly like that, and snapshot
	// streams would die for seconds on a perfectly good link. Deceleration 0 switches the throttle off: the
	// Unreliable lane then behaves like plain UDP and the game decides how much to send.
	enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0);
	if (!timeout_ms)
		return;
	// limit 4: after ~2 retransmit doublings only the minimum wall-clock silence decides.
	enet_peer_timeout(peer, 4, timeout_ms, timeout_ms * 2);
}

void Transport::disconnect_later(PeerHandle peer)
{
	if (peer)
		enet_peer_disconnect_later(peer, 0);
}

void Transport::disconnect_now(PeerHandle peer)
{
	if (peer)
		enet_peer_disconnect_now(peer, 0);
}

void Transport::reset(PeerHandle peer)
{
	if (peer)
		enet_peer_reset(peer);
}

void* Transport::user(PeerHandle peer)
{
	return peer ? peer->data : nullptr;
}

void Transport::set_user(PeerHandle peer, void* p)
{
	if (peer)
		peer->data = p;
}

bool Transport::outgoing_idle(PeerHandle peer)
{
	if (!peer)
		return true;
	return enet_list_empty(&peer->outgoingCommands) && enet_list_empty(&peer->outgoingSendReliableCommands);
}

bool Transport::all_acked(PeerHandle peer)
{
	if (!peer)
		return true;
	return outgoing_idle(peer) && enet_list_empty(&peer->sentReliableCommands);
}

uint32_t Transport::rtt_ms(PeerHandle peer)
{
	return peer ? peer->roundTripTime : 0;
}

float Transport::packet_loss(PeerHandle peer)
{
	return peer ? float(peer->packetLoss) / float(ENET_PEER_PACKET_LOSS_SCALE) : 0.f;
}

std::string Transport::address_of(PeerHandle peer)
{
	if (!peer)
		return std::string();
	char ip[64] = {};
	if (enet_address_get_host_ip(&peer->address, ip, sizeof(ip)) != 0)
		std::strcpy(ip, "?");
	return std::string(ip) + ":" + std::to_string(unsigned(peer->address.port));
}
} // namespace net
} // namespace coop
