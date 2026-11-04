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

#include <algorithm>
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

// Owner 2026-10-24: "frames must not affect the ping". The helper keeps the link prompt between the owner's frames:
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
		m_inbox.clear(); // their peers are gone with the host
		m_inbox_bytes = 0;
		m_relay.reset();
		m_relaying.store(false);
	}
	if (m_enet_ref)
	{
		m_enet_ref = false;
		enet_release();
	}
}

// True when ENet's next event is a DISCONNECT: the first peer in the dispatch queue with something to hand out
// (enet_protocol_dispatch_incoming_commands' order) is a zombie.
static bool disconnect_next(ENetHost* host)
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
static bool link_alive(const ENetPeer* peer, uint32_t link)
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

// The relay must not stop at a goodbye (disconnect_next): the zombies go behind everybody else in the dispatch queue, where they wait for
// the owner, and what the others sent comes out past them. A peer stays queued, so ENet's NEEDS_DISPATCH flag stays true.
static void zombies_last(ENetHost* host)
{
	ENetPeer* found[64];
	size_t n = 0;
	for (ENetListIterator it = enet_list_begin(&host->dispatchQueue); it != enet_list_end(&host->dispatchQueue) && n < 64;
		it = enet_list_next(it))
	{
		ENetPeer* peer = reinterpret_cast<ENetPeer*>(it);
		if (peer->state == ENET_PEER_STATE_ZOMBIE)
			found[n++] = peer;
	}
	for (size_t i = 0; i < n; ++i)
	{
		enet_list_remove(&found[i]->dispatchList);
		enet_list_insert(enet_list_end(&host->dispatchQueue), &found[i]->dispatchList);
	}
}

// An ENet event as the owner gets it (a packet's bytes are copied and the packet freed).
static void take_event(ENetEvent& e, Event& ev)
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
		while (!m_inbox.empty()) // what the helper took out of ENet comes first: the order stays ENet's
		{
			Taken t = std::move(m_inbox.front());
			m_inbox.pop_front();
			m_inbox_bytes -= t.ev.bytes.size();
			// A packet of a link the owner has reset (its slot may be someone else's now) or is closing itself is
			// dropped, as ENet drops what it still holds then. One from a peer that has left since comes before its
			// DISCONNECT: the owner gets the last word (a Reject, a Bye) the goodbye would have wiped in ENet.
			if (t.ev.type == Event::Type::Receive && !link_alive(t.ev.peer, t.link))
				continue;
			ev = std::move(t.ev);
			return 1;
		}
		for (;;)
		{
			rc = enet_host_service(m_host, &e, wait_ms);
			if (rc <= 0 || !m_relay)
				break;
			// What the owner takes out of ENet goes the way of what the helper takes: the order of a connection is ENet's.
			Taken t;
			t.link = e.peer ? e.peer->connectID : 0;
			take_event(e, t.ev);
			if (relay_take(t))
				continue;
			ev = std::move(t.ev);
			return 1;
		}
	}
	if (rc <= 0)
		return rc;
	take_event(e, ev);
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

bool Transport::inbox_empty() const
{
	IoLock io(g_io);
	return m_inbox.empty();
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

uint32_t Transport::ms_since_receive(PeerHandle peer)
{
	IoLock io(g_io);
	// lastReceiveTime is stamped on every acknowledgement the peer sends (0 = none yet).
	if (!peer || !peer->lastReceiveTime)
		return UINT32_MAX;
	return ENET_TIME_DIFFERENCE(enet_time_get(), peer->lastReceiveTime);
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

void Transport::inbox_push(Taken&& t)
{
	m_inbox_bytes += t.ev.bytes.size();
	m_inbox.push_back(std::move(t));
	if (m_relay)
	{
		m_relay_stats.inbox_max = std::max(m_relay_stats.inbox_max, m_inbox.size());
		m_relay_stats.inbox_bytes_max = std::max(m_relay_stats.inbox_bytes_max, m_inbox_bytes);
	}
}

// One event through the relay: true when nothing of it is left for the owner. Under the I/O lock.
bool Transport::relay_take(Taken& t)
{
	if (t.ev.type != Event::Type::Receive)
		return false;
	m_relay_out.clear();
	const softmove::Take take = m_relay->on_packet(t.ev.peer, t.link, t.ev.channel, t.ev.bytes.data(), t.ev.bytes.size(), m_relay_out);
	for (softmove::Relay::Out& o : m_relay_out)
	{
		ENetPeer* to = static_cast<ENetPeer*>(const_cast<void*>(o.to));
		if (to->connectID != o.link || to->state != ENET_PEER_STATE_CONNECTED)
		{
			++m_relay_stats.dropped; // he left, or his slot is somebody else's now
			continue;
		}
		Packet p = make_packet(o.lane, o.type, o.body.data(), o.body.size(), nullptr, 0, nullptr);
		ENetPacket* raw = p.m_p;
		p.m_p = nullptr;
		if (!raw || enet_peer_send(to, enet_uint8(o.lane), raw) < 0)
		{
			if (raw)
				enet_packet_destroy(raw);
			++m_relay_stats.dropped;
			continue;
		}
		if (o.type == uint16_t(Msg::Tunnel))
			++m_relay_stats.to_servers;
		else
			++m_relay_stats.to_players;
	}
	switch (take)
	{
	case softmove::Take::Keep: ++m_relay_stats.kept; return false;
	case softmove::Take::Sent: return true;
	case softmove::Take::SentAndKept:
		++m_relay_stats.kept;
		t.ev.relayed = true;
		return false;
	case softmove::Take::Dropped: ++m_relay_stats.dropped; return true;
	}
	return false;
}

// The helper's pass while the relay runs. Under the I/O lock.
void Transport::relay_pump()
{
	ENetEvent e;
	for (;;)
	{
		zombies_last(m_host);
		if (disconnect_next(m_host))
			return; // nothing but goodbyes left: they wait for the owner
		if (m_inbox_bytes >= m_relay_cap)
		{
			// What the world has to hear after the load outgrew what we keep for it. Nothing is thrown away: the helper goes back to its
			// own rules, everything stays in ENet and the road is closed until the owner comes back.
			m_relay_stats.overflowed = true;
			m_relay.reset();
			m_relaying.store(false);
			return;
		}
		if (enet_host_check_events(m_host, &e) <= 0)
			return;
		Taken t;
		t.link = e.peer ? e.peer->connectID : 0;
		take_event(e, t.ev);
		if (!relay_take(t))
			inbox_push(std::move(t));
	}
}

void Transport::relay_start(std::shared_ptr<const softmove::Relay> relay, size_t inbox_bytes)
{
	IoLock io(g_io);
	if (!m_host || !relay)
		return;
	m_relay = std::move(relay);
	m_relay_stats = RelayStats();
	m_relay_cap = inbox_bytes;
	m_relay_until.store(steady_ms() + kRelayRunsMs);
	// What came before the relay stands in front of what comes after it: it goes first, or a player's stream would run out of order.
	for (auto it = m_inbox.begin(); it != m_inbox.end();)
	{
		const size_t size = it->ev.bytes.size();
		const bool alive = it->ev.type == Event::Type::Receive && link_alive(it->ev.peer, it->link);
		if (alive && relay_take(*it))
		{
			m_inbox_bytes -= size;
			it = m_inbox.erase(it);
		}
		else
			++it;
	}
	m_relaying.store(true);
}

void Transport::relay_stop()
{
	IoLock io(g_io);
	m_relay.reset();
	m_relaying.store(false);
}

RelayStats Transport::relay_stats() const
{
	IoLock io(g_io);
	return m_relay_stats;
}

uint32_t Transport::link_of(PeerHandle peer)
{
	IoLock io(g_io);
	return peer ? peer->connectID : 0;
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
		const bool road = m_relaying.load(std::memory_order_relaxed) && now < m_relay_until.load(std::memory_order_relaxed);
		if (!alive && !road && !m_hold.held(now))
			continue; // a stalled owner without a hold (released, expired) must look dead to its peers
		// Never wait for the lock: the owner (or another transport's helper) is using ENet right now.
		std::unique_lock<std::recursive_mutex> io(g_io, std::try_to_lock);
		if (!io.owns_lock() || !m_host)
			continue;
		// Acks, pings, sends, receives, timeouts. With no event structure ENet queues every state change (a timeout
		// too) for dispatch instead of handing it out here.
		enet_host_service(m_host, nullptr, 0);
		if (m_relay)
		{
			relay_pump();
			m_ka_services.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		// Then the queued events in order into the inbox, up to the first goodbye: ENet frees a peer's slot when it
		// hands out its DISCONNECT, and the owner may still send to that peer until it has seen it. The goodbye and
		// what follows wait in ENet for the owner's own enet_host_service. A full inbox (an owner held by a long
		// load) leaves the rest in ENet too.
		ENetEvent e;
		while (m_inbox.size() < kMaxInbox && !disconnect_next(m_host) && enet_host_check_events(m_host, &e) > 0)
		{
			Taken t;
			t.link = e.peer ? e.peer->connectID : 0;
			take_event(e, t.ev);
			inbox_push(std::move(t));
		}
		m_ka_services.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace net
} // namespace coop
