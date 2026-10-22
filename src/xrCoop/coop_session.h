// Anomaly Coop — network session: host or client, handshake, routing, stats, bulk blobs.
// Pure C++17 on top of ENet, no engine dependencies. Single-threaded: every call,
// including all callbacks, happens on the thread that calls update() (the game main thread).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coop_protocol.h"
#include "coop_wire.h"

namespace coop
{
// R1 I3: what a host takes from one client in blobs (a client announces any size it likes; the host buffers it).
constexpr uint64_t kMaxClientBlobBytes = 1024 * 1024; // one blob
constexpr uint64_t kMaxClientBlobBytesInFlight = 4 * 1024 * 1024; // announced sizes of its unfinished blobs together

enum class Role : uint8_t
{
	None,
	Host,
	Client
};

enum class PeerState : uint8_t
{
	Connecting, // transport connected, waiting for Hello / Welcome
	Joined, // handshake accepted
	Leaving
};

struct PeerInfo
{
	PeerId id = kInvalidPeer;
	PeerState state = PeerState::Connecting;
	std::string nick;
	PlayerKey key{}; // known only on the host
	uint32_t rtt_ms = 0;
	float packet_loss = 0.f; // 0..1 as reported by the transport
	std::string address; // "ip:port"
};

struct SessionConfig
{
	uint16_t port = kDefaultPort; // host: listen port
	uint32_t max_players = kMaxPlayers; // host only, including the host itself
	std::string build_id; // must match between host and client
	Manifest manifest; // local content manifest
	PlayerKey key{};
	std::string nick;
	uint32_t connect_timeout_ms = 10000; // client: transport + handshake deadline
	uint32_t peer_timeout_ms = 15000; // silence before a peer is dropped
};

// Per message type counters (P9 / R-12.4).
struct MsgStats
{
	uint64_t sent_count = 0, sent_bytes = 0;
	uint64_t recv_count = 0, recv_bytes = 0;
};

// Network condition emulation for tests (applied on send and receive inside Session).
struct NetSim
{
	uint32_t lag_ms = 0; // added one-way delay
	uint32_t jitter_ms = 0; // +- random extra delay
	float loss = 0.f; // 0..1 drop probability, Unreliable lane only
	bool enabled() const { return lag_ms || jitter_ms || loss > 0.f; }
};

class Session
{
public:
	using Handler = std::function<void(PeerId from, Reader& payload)>;
	using LogFn = std::function<void(const char* line)>;

	Session();
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	// Starts listening. false + err on failure (port busy, ENet init).
	bool host(const SessionConfig& cfg, std::string* err);
	// Starts connecting; result arrives via on_welcome / on_rejected / on_disconnected.
	bool connect(const SessionConfig& cfg, const std::string& address, uint16_t port, std::string* err);
	// Graceful stop: sends Bye(reason) to everyone, flushes, closes. Safe to call twice.
	void shutdown(const char* reason);

	// Pumps the transport, handshake, timeouts, blobs and delayed (NetSim) packets.
	void update(uint64_t now_ms);

	Role role() const;
	PeerId self() const; // host: 0; client: slot from Welcome, kInvalidPeer before
	bool is_active() const; // host listening, or client joined
	std::vector<PeerInfo> peers() const; // host: all clients; client: host + list from PeerList
	const PeerInfo* peer(PeerId id) const;

	// Game message handlers (type >= Msg::GameBase). One handler per type, last wins.
	void on(Msg type, Handler h);

	// Sends payload as message `type`. Host: to any client. Client: only to kHostPeer.
	void send(PeerId to, Lane lane, Msg type, const Writer& payload);
	// Host: to every joined client except `except`. Client: same as send to host.
	void broadcast(Lane lane, Msg type, const Writer& payload, PeerId except = kInvalidPeer);
	// Disconnects one client with a reason (host only).
	void kick(PeerId id, RejectReason reason, const std::string& text);

	// The owner thread is about to block for long (a level load does not call update() for tens of seconds):
	// keep the connection alive from a helper thread for up to `ms` from now; 0 releases. Without a hold a
	// silent process is dropped by its peers after peer_timeout_ms (a crash dialog must not look alive).
	// Thread-safe (the only Session call that is).
	void hold_connection(uint32_t ms);
	uint64_t keepalive_services() const; // helper services so far (diagnostics)

	// Bulk transfer of an arbitrary byte blob in chunks over Lane::Bulk, with CRC32 check.
	// Returns the blob id. R1 I4: a blob of the same kind still queued or streaming to that peer is cancelled, the
	// newer one replaces it (a streaming one ends early, its receiver drops it). The blob carries the world epoch of
	// the moment it was queued.
	uint32_t send_blob(PeerId to, uint32_t kind, std::vector<uint8_t> data);

	// R1 I3, host: a blob kind a client may send (none by default). A client that announces another kind, a blob
	// over kMaxClientBlobBytes or more than kMaxClientBlobBytesInFlight of unfinished blobs is kicked as malformed.
	void accept_client_blob(uint32_t kind);

	// R2 I4, host: a client may send `type` at most `burst` times in a row, and once more every `refill_ms` (a token
	// bucket per client). What exceeds it is dropped before its handler (logged once per client and type, the
	// sender is not kicked: a burst may be honest). burst or refill_ms 0 removes the limit.
	void limit_rate(Msg type, uint32_t burst, uint32_t refill_ms);

	// R2 I1: the world epoch. A load or a level change on the host replaces the world under the session; nothing
	// made in the old world may be applied to the new one.
	// Host: the epoch of the world it serves, raised by the game as soon as a new world exists. Each client is in
	//   the epoch of its Welcome until it tells otherwise (core WorldEpoch, sent by its set_world_epoch). Game
	//   messages from a client in another epoch are dropped (logged once per client and epoch); game messages to it
	//   are not sent, except SessionReset, which tells it the new epoch. Blobs of another epoch are cancelled.
	// Client: the epoch of the world it is in or rejoins: from Welcome, then set by the game from SessionReset
	//   before its JoinRequest. A completed blob of another epoch is dropped.
	// host() and connect() start at 0.
	void set_world_epoch(uint32_t epoch);
	uint32_t world_epoch() const;

	// --- callbacks (all optional) ---
	std::function<void(const PeerInfo&)> on_peer_joined; // host: client accepted; client: other player joined
	std::function<void(PeerId, const std::string& reason)> on_peer_left;
	std::function<void(const WelcomeMsg&)> on_welcome; // client: accepted by host
	std::function<void(const RejectMsg&, const ManifestDiff&)> on_rejected; // client
	std::function<void(const std::string& reason)> on_disconnected; // client: lost host / closed
	std::function<void(PeerId, uint32_t blob_id, uint32_t kind, uint64_t done, uint64_t total)> on_blob_progress;
	std::function<void(PeerId, uint32_t blob_id, uint32_t kind, std::vector<uint8_t>& data)> on_blob_complete;
	std::function<void(PeerId, const std::string& text)> on_chat;

	void send_chat(const std::string& text); // client -> host -> all; host -> all

	void set_log(LogFn fn);
	void set_netsim(const NetSim& sim);
	const NetSim& netsim() const;

	// Stats indexed by message type value.
	const MsgStats& stats(uint16_t type) const;
	std::vector<uint16_t> stats_types() const; // types with any traffic
	void reset_stats();

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};

// 128-bit random player key (BCryptGenRandom).
PlayerKey generate_player_key();
// Short stable hash of a key for display and PeerList.
uint32_t player_key_hash(const PlayerKey& key);
// R1 I1: SHA-256 (BCrypt). A host keeps only the digest of a player's key in its saves: the world save goes to every
// client, and the key is all it takes to play that player's character. false (out zeroed) if BCrypt failed.
using Sha256 = std::array<uint8_t, 32>;
bool sha256(const void* data, size_t size, Sha256& out);

// R1 I1: the player key in the custom data of the player's character on the host, as text:
//   [coop]
//   key = <kKeyDigestHexDigits: sha256 of the key>
//   nick = <nick>
// Saves made before kept the key itself (kPlayerKeyHexDigits). Pure functions, no engine.
constexpr size_t kPlayerKeyHexDigits = sizeof(PlayerKey) * 2;
constexpr size_t kKeyDigestHexDigits = sizeof(Sha256) * 2;
// Lowercase hex of the bytes.
std::string to_hex(const uint8_t* data, size_t size);
// The digest of a key as the custom data keeps it. false if BCrypt failed.
bool key_digest_hex(const PlayerKey& key, std::string& out);
// The value of the "key" line of the [coop] section, by a strict parse of lines (not a search in the whole text: the
// nick line could hold another player's key). The line is "key = <value>", blanks around '=' optional; the value is
// kPlayerKeyHexDigits (legacy) or kKeyDigestHexDigits lowercase hex digits. Found at [begin, begin + len).
bool find_key_value(const std::string& ini, size_t& begin, size_t& len);
// Exactly kPlayerKeyHexDigits lowercase hex digits back to the key; false (out zeroed) otherwise.
bool key_from_hex(const std::string& hex, PlayerKey& out);
// A key line that still holds the key itself (a save from before) gets the key's digest in its place; every other
// line stays as it is. true if `ini` was changed.
bool migrate_key_value(std::string& ini);
// CRC32 (IEEE) used by blob transfer, exposed for tests.
uint32_t crc32(const void* data, size_t size, uint32_t seed = 0);

// R1 I4, R2 I4: the host's gate for JoinRequest. A join costs the host a full world save, written and read back on
// the main thread, and a blob of tens of MB: one join in flight per peer (from its admission to its WorldLoaded),
// and admissions of one peer at least kIntervalMs apart. A request that comes too soon is not lost: it waits and
// due() admits it. Times are any monotonic ms clock. Pure logic, no engine.
class JoinGate
{
public:
	static constexpr uint32_t kIntervalMs = 5000;
	enum class Verdict : uint8_t
	{
		Admit, // send the world now: the join is in flight
		Later, // too soon after the peer's last join: waits, due() admits it
		Refused, // a join of the peer is in flight or already waits: this request is dropped
	};

	Verdict request(PeerId peer, uint64_t now_ms);
	// Waiting requests whose time has come, admitted now (in flight).
	std::vector<PeerId> due(uint64_t now_ms);
	// WorldLoaded from the peer: true if its join was in flight (it ends here). false: nothing was asked of it (a
	// repeat, a forged one, one from a world that is gone) and the message is to be ignored.
	bool loaded(PeerId peer);
	// A load or a level change replaced the world: joins in flight and waiting requests are void, everybody rejoins
	// after SessionReset. The interval holds across it.
	void world_replaced();
	// The peer left: its join and its waiting request are gone. The interval stays with its slot.
	void forget(PeerId peer);
	bool in_flight(PeerId peer) const;
	bool waiting(PeerId peer) const;

private:
	struct Peer
	{
		bool in_flight = false;
		bool waiting = false;
		bool admitted_once = false;
		uint64_t last_admit_ms = 0;
	};
	std::map<PeerId, Peer> m_peers;
};
} // namespace coop
