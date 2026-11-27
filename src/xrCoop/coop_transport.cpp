// Anomaly Coop — private ENet wrapper implementation.
#include "coop_transport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>
#include <enet/time.h> // ENET_TIME_DIFFERENCE

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

std::mutex g_enet_mutex; // the library's reference count (enet_initialize / enet_deinitialize)
int g_enet_refs = 0;

// Frames must not affect the ping. The helper keeps the link prompt between the owner's frames:
// acks and pings go out within ~1 ms of a packet (the engine runs the Windows timer at 1 ms), and what the owner sent
// leaves at once instead of at its next frame. RTT is the network's again, not network + both sides' frame times.
constexpr uint32_t kKeepAliveTickMs = 1; // helper thread wake-up period
constexpr uint64_t kOwnerAliveMs = 1000; // the owner pumped this recently: it is alive, the helper keeps the link prompt
constexpr size_t kMaxInbox = 4096; // events waiting for the owner; past it the helper only acks (ENet keeps the rest)

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

// Every queued command was transmitted at least once. Under the I/O lock.
bool idle(const ENetPeer* peer)
{
	return enet_list_empty(&peer->outgoingCommands) && enet_list_empty(&peer->outgoingSendReliableCommands);
}

// True when ENet's next event is a DISCONNECT: the first peer in the dispatch queue with something to hand out
// (enet_protocol_dispatch_incoming_commands' order) is a zombie.
bool disconnect_next(ENetHost* host)
{
	for (ENetListIterator it = enet_list_begin(&host->dispatchQueue); it != enet_list_end(&host->dispatchQueue);
		it = enet_list_next(it))
	{
		const ENetPeer* peer = reinterpret_cast<const ENetPeer*>(it); // dispatchList is ENetPeer's first member
		switch (peer->state)
		{
		case ENET_PEER_STATE_ZOMBIE:
			return true;
		case ENET_PEER_STATE_CONNECTION_PENDING:
		case ENET_PEER_STATE_CONNECTION_SUCCEEDED:
			return false;
		case ENET_PEER_STATE_CONNECTED:
			if (!enet_list_empty(&peer->dispatchedCommands))
				return false;
			break;
		default:
			break;
		}
	}
	return false;
}

// The connection `link` (its ENet connectID) is still the peer's, and the owner has not reset or closed it.
bool link_alive(const ENetPeer* peer, uint32_t link)
{
	if (peer->connectID != link)
		return false;
	switch (peer->state)
	{
	case ENET_PEER_STATE_CONNECTED:
	case ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT: // the remote's goodbye
	case ENET_PEER_STATE_ZOMBIE:
		return true;
	default: // reset, or closing on the owner's word (disconnect, disconnect_later)
		return false;
	}
}

// An ENet event as the owner gets it (a packet's bytes are copied and the packet freed).
void take_event(ENetEvent& e, Event& ev)
{
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
	{
		IoLock io(m_io);
		m_host = enet_host_create(&addr, max_peers, kChannels, 0, 0);
		if (m_host)
			m_traffic = std::make_unique<TrafficSlot[]>(m_host->peerCount);
	}
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
	ENetPeer* peer = nullptr;
	{
		IoLock io(m_io);
		m_host = enet_host_create(nullptr, 1, kChannels, 0, 0);
		if (m_host)
			peer = enet_host_connect(m_host, &addr, kChannels, 0);
		if (peer)
			m_traffic = std::make_unique<TrafficSlot[]>(m_host->peerCount);
	}
	if (!m_host)
	{
		if (err)
			*err = "cannot create client socket";
		destroy();
		return nullptr;
	}
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
		IoLock io(m_io);
		enet_host_destroy(m_host);
		m_host = nullptr;
		m_traffic.reset();
		m_inbox.clear(); // their peers are gone with the host
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
		IoLock io(m_io);
		m_last_service_ms.store(steady_ms(), std::memory_order_relaxed);
		while (!m_inbox.empty()) // what the helper took out of ENet comes first: the order stays ENet's
		{
			Taken t = std::move(m_inbox.front());
			m_inbox.pop_front();
			// A packet of a link the owner has reset (its slot may be someone else's now) or is closing itself is
			// dropped, as ENet drops what it still holds then. One from a peer that has left since comes before its
			// DISCONNECT: the owner gets the last word (a Reject, a Bye) the goodbye would have wiped in ENet.
			if (t.ev.type == Event::Type::Receive && !link_alive(t.ev.peer, t.link))
				continue;
			ev = std::move(t.ev);
			return 1;
		}
		rc = enet_host_service(m_host, &e, wait_ms);
		if (rc > 0)
			count_traffic_event(e);
	}
	if (rc <= 0)
		return rc;
	take_event(e, ev);
	return 1;
}

void Transport::flush()
{
	IoLock io(m_io);
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

bool Transport::owns(PeerHandle peer) const
{
	return m_host && peer && peer->host == m_host;
}

bool Transport::submit(PeerHandle peer, Lane lane, ENetPacket* packet)
{
	if (!owns(peer) || peer->state != ENET_PEER_STATE_CONNECTED || enet_peer_send(peer, enet_uint8(lane), packet) < 0)
		return false;
	count_traffic(peer, packet->dataLength, 0);
	return true;
}

bool Transport::send(PeerHandle peer, Lane lane, Packet&& packet)
{
	ENetPacket* p = packet.m_p;
	packet.m_p = nullptr;
	if (!p)
		return false;
	IoLock io(m_io);
	if (submit(peer, lane, p))
		return true;
	enet_packet_destroy(p);
	return false;
}

Transport::Fanout::Fanout(Transport& transport, Lane lane, Packet&& packet) : m_tp(transport), m_lane(lane), m_p(packet.m_p)
{
	packet.m_p = nullptr;
	if (m_p)
		++m_p->referenceCount; // not shared yet: no ENet call has seen it
}

Transport::Fanout::~Fanout()
{
	if (!m_p)
		return;
	IoLock io(m_tp.m_io); // ENet drops its references under the same lock
	if (--m_p->referenceCount == 0)
		enet_packet_destroy(m_p);
}

bool Transport::Fanout::send(PeerHandle peer)
{
	if (!m_p)
		return false;
	IoLock io(m_tp.m_io);
	return m_tp.submit(peer, m_lane, m_p);
}

void Transport::count_traffic(PeerHandle peer, uint64_t out, uint64_t in)
{
	TrafficSlot& slot = m_traffic[peer->incomingPeerID];
	if (out)
		slot.out_bytes.fetch_add(out, std::memory_order_relaxed);
	if (in)
		slot.in_bytes.fetch_add(in, std::memory_order_relaxed);

	const uint64_t tick = steady_ms() / kTrafficBucketMs;
	auto& bucket = slot.buckets[tick % kTrafficBuckets];
	if (bucket.tick != tick)
		bucket = {tick, 0, 0};
	bucket.out_bytes += out;
	bucket.in_bytes += in;
}

void Transport::count_traffic_event(const ENetEvent& event)
{
	if (event.type == ENET_EVENT_TYPE_CONNECT)
	{
		TrafficSlot& slot = m_traffic[event.peer->incomingPeerID];
		slot.out_bytes.store(0, std::memory_order_relaxed);
		slot.in_bytes.store(0, std::memory_order_relaxed);
		slot.buckets.fill({});
	}
	else if (event.type == ENET_EVENT_TYPE_RECEIVE && event.packet)
		count_traffic(event.peer, 0, event.packet->dataLength);
}

std::vector<Transport::Traffic> Transport::traffic(const std::vector<PeerHandle>& peers) const
{
	std::vector<Traffic> out(peers.size());
	IoLock io(m_io);
	if (!m_host)
		return out;
	const uint64_t tick = steady_ms() / kTrafficBucketMs;
	for (size_t i = 0; i < peers.size(); ++i)
	{
		const ENetPeer* peer = peers[i];
		if (!peer || peer->host != m_host || peer->state == ENET_PEER_STATE_DISCONNECTED)
			continue;
		const TrafficSlot& slot = m_traffic[peer->incomingPeerID];
		Traffic& stats = out[i];
		stats.out_bytes = slot.out_bytes.load(std::memory_order_relaxed);
		stats.in_bytes = slot.in_bytes.load(std::memory_order_relaxed);
		for (const auto& bucket : slot.buckets)
		{
			if (bucket.tick < tick && tick - bucket.tick < kTrafficBuckets)
			{
				stats.out_5s += bucket.out_bytes;
				stats.in_5s += bucket.in_bytes;
			}
		}
	}
	return out;
}

void Transport::set_timeouts(PeerHandle peer, uint32_t timeout_ms)
{
	if (!peer)
		return;
	IoLock io(m_io);
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
	IoLock io(m_io);
	if (peer)
		enet_peer_disconnect_later(peer, 0);
}

void Transport::disconnect_now(PeerHandle peer)
{
	IoLock io(m_io);
	if (peer)
		enet_peer_disconnect_now(peer, 0);
}

void Transport::reset(PeerHandle peer)
{
	IoLock io(m_io);
	if (peer)
		enet_peer_reset(peer);
}

void* Transport::user(PeerHandle peer) const
{
	IoLock io(m_io);
	return peer ? peer->data : nullptr;
}

void Transport::set_user(PeerHandle peer, void* p)
{
	IoLock io(m_io);
	if (peer)
		peer->data = p;
}

bool Transport::outgoing_idle(PeerHandle peer) const
{
	if (!peer)
		return true;
	IoLock io(m_io);
	return idle(peer);
}

bool Transport::all_acked(PeerHandle peer) const
{
	if (!peer)
		return true;
	IoLock io(m_io);
	return idle(peer) && enet_list_empty(&peer->sentReliableCommands);
}

uint32_t Transport::rtt_ms(PeerHandle peer) const
{
	IoLock io(m_io);
	return peer ? peer->roundTripTime : 0;
}

float Transport::packet_loss(PeerHandle peer) const
{
	IoLock io(m_io);
	return peer ? float(peer->packetLoss) / float(ENET_PEER_PACKET_LOSS_SCALE) : 0.f;
}

uint32_t Transport::ms_since_receive(PeerHandle peer) const
{
	IoLock io(m_io);
	// lastReceiveTime is stamped on every acknowledgement the peer sends (0 = none yet).
	if (!peer || !peer->lastReceiveTime)
		return UINT32_MAX;
	return ENET_TIME_DIFFERENCE(enet_time_get(), peer->lastReceiveTime);
}

std::string Transport::address_of(PeerHandle peer) const
{
	if (!peer)
		return std::string();
	IoLock io(m_io);
	char ip[64] = {};
	if (enet_address_get_host_ip(&peer->address, ip, sizeof(ip)) != 0)
		std::strcpy(ip, "?");
	return std::string(ip) + ":" + std::to_string(unsigned(peer->address.port));
}

// ---------------------------------------------------------------- keep-alive

void Transport::hold(uint32_t ms)
{
	m_hold.arm(ms ? steady_ms() + ms : 0); // refused after seal_hold()
}

void Transport::keepalive_start()
{
	if (m_ka_thread.joinable())
		return;
	{
		std::lock_guard<std::mutex> lk(m_ka_mutex);
		m_ka_stop = false;
	}
	m_hold.release(); // a seal stays
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
	m_hold.release(); // a seal stays
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
		if (m_hold.sealed())
			continue; // a crash: the link dies with the process
		const bool alive = m_last_service_ms.load(std::memory_order_relaxed) + kOwnerAliveMs > now;
		if (!alive && !m_hold.held(now))
			continue; // a stalled owner without a hold (released, expired) must look dead to its peers
		// Never wait for the lock: the owner is using this host right now.
		std::unique_lock<std::mutex> io(m_io, std::try_to_lock);
		if (!io.owns_lock() || !m_host)
			continue;
		// Acks, pings, sends, receives, timeouts. With no event structure ENet queues every state change (a timeout
		// too) for dispatch instead of handing it out here.
		enet_host_service(m_host, nullptr, 0);
		// Then the queued events in order into the inbox, up to the first goodbye: ENet frees a peer's slot when it
		// hands out its DISCONNECT, and the owner may still send to that peer until it has seen it. The goodbye and
		// what follows wait in ENet for the owner's own enet_host_service. A full inbox (an owner held by a long
		// load) leaves the rest in ENet too.
		ENetEvent e;
		while (m_inbox.size() < kMaxInbox && !disconnect_next(m_host) && enet_host_check_events(m_host, &e) > 0)
		{
			count_traffic_event(e);
			m_inbox.emplace_back();
			m_inbox.back().link = e.peer ? e.peer->connectID : 0;
			take_event(e, m_inbox.back().ev);
		}
		m_ka_services.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace net
} // namespace coop
