// Anomaly Coop — private ENet wrapper used by Session (not part of the public API).
// Keeps <enet/enet.h> and the Windows headers out of coop_session.cpp.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "coop_link_hold.h"
#include "coop_protocol.h"
#include "coop_soft_move.h"

struct _ENetHost;
struct _ENetPeer;
struct _ENetPacket;

namespace coop
{
namespace net
{
using PeerHandle = _ENetPeer*;

// Bytes of Bulk-lane packets that are alive (queued in Session or in ENet, not yet acknowledged).
// A packet holds a shared reference, so the counter outlives the connection that owns it.
// Atomic: ENet may free an acknowledged packet on the keep-alive thread (see Transport::hold).
struct ByteCounter
{
	std::atomic<size_t> bytes{0};
};
using ByteCounterPtr = std::shared_ptr<ByteCounter>;

// Owning, move-only handle of a not yet submitted ENet packet.
class Packet
{
public:
	Packet() noexcept = default;
	Packet(Packet&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }
	Packet& operator=(Packet&& o) noexcept;
	Packet(const Packet&) = delete;
	Packet& operator=(const Packet&) = delete;
	~Packet();

	explicit operator bool() const { return m_p != nullptr; }
	size_t size() const;

private:
	friend class Transport;
	_ENetPacket* m_p = nullptr;
};

struct Event
{
	enum class Type : uint8_t
	{
		None,
		Connect,
		Disconnect,
		Receive
	};
	Type type = Type::None;
	PeerHandle peer = nullptr;
	uint8_t channel = 0;
	uint32_t data = 0;
	std::vector<uint8_t> bytes; // Receive: full packet (u16 type + payload)
	bool relayed = false; // Receive: the relay sent it on too; what is left is the world's copy (softmove::Take::SentAndKept)
};

// What the relay did since it started (Transport::relay_start).
struct RelayStats
{
	uint64_t to_servers = 0; // packets of players that went to their map server
	uint64_t to_players = 0; // packets of map servers that went to their players
	uint64_t kept = 0; // left for the main thread
	uint64_t dropped = 0; // nobody to give them to
	size_t inbox_max = 0; // the most events that waited for the main thread at once
	size_t inbox_bytes_max = 0;
	bool overflowed = false; // the inbox outgrew its limit: the relay stopped there
};

// Thread safety: every call that touches ENet state takes one process-wide lock, so the owner thread and the
// keep-alive thread never run ENet at the same time. Everything else (events, Session) stays on the owner thread.
class Transport
{
public:
	Transport() = default;
	~Transport() { destroy(); }
	Transport(const Transport&) = delete;
	Transport& operator=(const Transport&) = delete;

	// Server socket on 0.0.0.0:port with room for max_peers simultaneous transport connections.
	bool listen(uint16_t port, uint32_t max_peers, std::string* err);
	// Client socket; starts connecting. Returns the (only) peer or nullptr.
	PeerHandle connect(const std::string& address, uint16_t port, std::string* err);
	void destroy();
	bool active() const { return m_host != nullptr; }

	// Non-blocking (wait_ms == 0) event poll: 1 = event filled, 0 = nothing, -1 = transport error.
	int service(Event& ev, uint32_t wait_ms = 0);
	void flush();

	// Builds [u16 type][a][b] with the right ENet flags for the lane. `counter` (optional) tracks live bytes.
	static Packet make_packet(Lane lane, uint16_t type, const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
		const ByteCounterPtr& counter);
	// Submits the packet. Ownership passes to ENet on success; on failure the packet is destroyed. false = not sent.
	bool send(PeerHandle peer, Lane lane, Packet&& packet);

	// Silence timeout for the peer; also switches ENet's unreliable-dropping congestion throttle off.
	static void set_timeouts(PeerHandle peer, uint32_t timeout_ms);
	static void disconnect_later(PeerHandle peer);
	static void disconnect_now(PeerHandle peer);
	static void reset(PeerHandle peer);

	static void* user(PeerHandle peer);
	static void set_user(PeerHandle peer, void* p);
	static bool outgoing_idle(PeerHandle peer); // every queued command was transmitted at least once
	static bool all_acked(PeerHandle peer); // idle and no reliable command awaiting acknowledgement
	static uint32_t rtt_ms(PeerHandle peer);
	static float packet_loss(PeerHandle peer); // 0..1
	// Milliseconds since the peer last acknowledged anything; UINT32_MAX if it never did. ENet pings an idle link
	// every 500 ms, so a live peer stays under a second plus its RTT; a process that is gone or stuck goes silent.
	static uint32_t ms_since_receive(PeerHandle peer);
	static std::string address_of(PeerHandle peer); // "ip:port"

	// A helper thread services ENet every millisecond while the owner lives (it called service() within the last
	// 1 s) or while held: acks and pings go out at once, so the RTT is the network's, not the frame times'. Events it
	// takes wait for the owner's next service(), in ENet's order; it never hands out a DISCONNECT (ENet frees the
	// peer's slot then, and the owner must see the goodbye before a newcomer can take the slot).
	// Keep-alive for long blocking work on the owner thread (a level load does not pump the network for tens of
	// seconds): hold(ms) keeps the helper going for `ms` from now (a new call replaces the deadline), hold(0)
	// releases. A process that stops pumping without a hold (stuck on a crash dialog, hung) goes silent 1 s later
	// and its peers drop it after the silence timeout. Safe to call from any thread.
	void hold(uint32_t ms);
	// A crash: the hold is released at once and every later hold() is refused, for the life of this transport (a new
	// listen/connect does not lift it). Atomic with hold() (link_hold::HoldWord): a hold racing with it cannot re-arm.
	// Safe to call from any thread, also mid-crash.
	void seal_hold() { m_hold.seal(); }
	uint64_t keepalive_services() const { return m_ka_services.load(std::memory_order_relaxed); }

	// The host's move (xrCoop/coop_soft_move.h): while the owner loads a level, the helper thread is the road between the players and
	// their map servers. It runs by itself, with no owner and no hold, for kRelayRunsMs at most: a process that hangs goes silent then.
	// The inbox has no event limit meanwhile, only `inbox_bytes`: past that the relay stops by itself and everything stays in ENet, as
	// without it. What the inbox holds at the start goes through the relay first: the order of a connection stays. Owner's thread.
	void relay_start(std::shared_ptr<const softmove::Relay> relay, size_t inbox_bytes = kRelayInboxBytes);
	void relay_stop();
	bool relaying() const { return m_relaying.load(std::memory_order_relaxed); }
	bool inbox_empty() const; // nothing waits that the helper took out of ENet
	RelayStats relay_stats() const;
	static uint32_t link_of(PeerHandle peer); // the ENet connectID: the connection, where a handle is only its slot

	static constexpr uint32_t kRelayRunsMs = 180000;
	static constexpr size_t kRelayInboxBytes = size_t(64) * 1024 * 1024;

private:
	struct Taken;
	void keepalive_start();
	void keepalive_stop();
	void keepalive_loop();
	bool relay_take(Taken& t);
	void relay_pump();
	void inbox_push(Taken&& t);

	_ENetHost* m_host = nullptr;
	bool m_enet_ref = false;

	std::thread m_ka_thread;
	std::mutex m_ka_mutex; // guards m_ka_stop for the condition variable
	std::condition_variable m_ka_cv;
	bool m_ka_stop = false;
	std::atomic<uint64_t> m_last_service_ms{0}; // steady clock of the owner's last service()
	// Events the helper took out of ENet, in ENet's order (2026-10-24: the helper pumps between the owner's frames).
	// `link` is the peer's ENet connectID then: a packet of a link the owner has since reset is not handed out.
	// Guarded by the I/O lock.
	struct Taken
	{
		Event ev;
		uint32_t link = 0;
	};
	std::deque<Taken> m_inbox;
	size_t m_inbox_bytes = 0;
	link_hold::HoldWord m_hold; // steady clock deadline; released; or sealed by a crash
	std::atomic<uint64_t> m_ka_services{0};
	// The relay. Guarded by the I/O lock, but for what the helper asks before it takes that lock.
	std::shared_ptr<const softmove::Relay> m_relay;
	std::vector<softmove::Relay::Out> m_relay_out; // of one packet: kept for its capacity
	RelayStats m_relay_stats;
	size_t m_relay_cap = 0;
	std::atomic<bool> m_relaying{false};
	std::atomic<uint64_t> m_relay_until{0}; // steady clock
};
} // namespace net
} // namespace coop
