// Anomaly Coop — private ENet wrapper implementation.
#include "coop_transport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <chrono>
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

// One lock for all ENet state of the process: the owner thread and keep-alive threads take turns.
// Recursive: a few wrappers call each other.
std::recursive_mutex g_io;
using IoLock = std::lock_guard<std::recursive_mutex>;

constexpr uint32_t kKeepAliveTickMs = 50; // helper thread wake-up period
constexpr uint64_t kOwnerStallMs = 150; // the owner is "busy" when it has not serviced for this long

uint64_t steady_ms()
{
	using namespace std::chrono;
	return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

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
		std::atomic<size_t>& bytes = (*box)->bytes;
		size_t cur = bytes.load();
		size_t next;
		do
			next = cur >= p->dataLength ? cur - p->dataLength : 0;
		while (!bytes.compare_exchange_weak(cur, next));
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
	IoLock io(g_io);
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
	keepalive_start();
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
	if (enet_address_set_host(&addr, address.c_str()) != 0) // may block on DNS: outside the ENet lock
	{
		if (err)
			*err = "cannot resolve host '" + address + "'";
		destroy();
		return nullptr;
	}
	IoLock io(g_io);
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
	keepalive_start();
	return peer;
}

void Transport::destroy()
{
	keepalive_stop(); // before the host goes: the helper must not service a destroyed host
	if (m_host)
	{
		IoLock io(g_io);
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
	int rc;
	{
		IoLock io(g_io);
		m_last_service_ms.store(steady_ms(), std::memory_order_relaxed);
		rc = enet_host_service(m_host, &e, wait_ms);
	}
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
	IoLock io(g_io);
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
		counter->bytes.fetch_add(p->dataLength);
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
	IoLock io(g_io);
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
	IoLock io(g_io);
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
	IoLock io(g_io);
	if (peer)
		enet_peer_disconnect_later(peer, 0);
}

void Transport::disconnect_now(PeerHandle peer)
{
	IoLock io(g_io);
	if (peer)
		enet_peer_disconnect_now(peer, 0);
}

void Transport::reset(PeerHandle peer)
{
	IoLock io(g_io);
	if (peer)
		enet_peer_reset(peer);
}

void* Transport::user(PeerHandle peer)
{
	IoLock io(g_io);
	return peer ? peer->data : nullptr;
}

void Transport::set_user(PeerHandle peer, void* p)
{
	IoLock io(g_io);
	if (peer)
		peer->data = p;
}

bool Transport::outgoing_idle(PeerHandle peer)
{
	if (!peer)
		return true;
	IoLock io(g_io);
	return enet_list_empty(&peer->outgoingCommands) && enet_list_empty(&peer->outgoingSendReliableCommands);
}

bool Transport::all_acked(PeerHandle peer)
{
	if (!peer)
		return true;
	IoLock io(g_io);
	return outgoing_idle(peer) && enet_list_empty(&peer->sentReliableCommands);
}

uint32_t Transport::rtt_ms(PeerHandle peer)
{
	IoLock io(g_io);
	return peer ? peer->roundTripTime : 0;
}

float Transport::packet_loss(PeerHandle peer)
{
	IoLock io(g_io);
	return peer ? float(peer->packetLoss) / float(ENET_PEER_PACKET_LOSS_SCALE) : 0.f;
}

std::string Transport::address_of(PeerHandle peer)
{
	if (!peer)
		return std::string();
	IoLock io(g_io);
	char ip[64] = {};
	if (enet_address_get_host_ip(&peer->address, ip, sizeof(ip)) != 0)
		std::strcpy(ip, "?");
	return std::string(ip) + ":" + std::to_string(unsigned(peer->address.port));
}

// ---------------------------------------------------------------- keep-alive

void Transport::hold(uint32_t ms)
{
	m_hold_until_ms.store(ms ? steady_ms() + ms : 0, std::memory_order_relaxed);
}

void Transport::keepalive_start()
{
	if (m_ka_thread.joinable())
		return;
	{
		std::lock_guard<std::mutex> lk(m_ka_mutex);
		m_ka_stop = false;
	}
	m_hold_until_ms.store(0, std::memory_order_relaxed);
	m_last_service_ms.store(steady_ms(), std::memory_order_relaxed);
	m_ka_thread = std::thread(&Transport::keepalive_loop, this);
}

void Transport::keepalive_stop()
{
	if (!m_ka_thread.joinable())
		return;
	{
		std::lock_guard<std::mutex> lk(m_ka_mutex);
		m_ka_stop = true;
	}
	m_ka_cv.notify_all();
	m_ka_thread.join();
	m_hold_until_ms.store(0, std::memory_order_relaxed);
}

void Transport::keepalive_loop()
{
	for (;;)
	{
		{
			std::unique_lock<std::mutex> lk(m_ka_mutex);
			if (m_ka_cv.wait_for(lk, std::chrono::milliseconds(kKeepAliveTickMs), [this] { return m_ka_stop; }))
				return;
		}
		const uint64_t now = steady_ms();
		const uint64_t until = m_hold_until_ms.load(std::memory_order_relaxed);
		if (!until || now >= until)
			continue; // not held: a stalled owner must look dead to its peers
		if (m_last_service_ms.load(std::memory_order_relaxed) + kOwnerStallMs > now)
			continue; // the owner pumps by itself
		// Never wait for the lock: the owner (or another transport's helper) is using ENet right now.
		std::unique_lock<std::recursive_mutex> io(g_io, std::try_to_lock);
		if (!io.owns_lock() || !m_host)
			continue;
		// No event structure: ENet sends acks and pings, checks timeouts and queues whatever it receives (and any
		// state change) for the owner's next enet_host_service.
		enet_host_service(m_host, nullptr, 0);
		m_ka_services.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace net
} // namespace coop
