// Anomaly Coop — network session: host or client, handshake, routing, stats, bulk blobs.
// Pure C++17 on top of ENet, no engine dependencies. Single-threaded: every call,
// including all callbacks, happens on the thread that calls update() (the game main thread).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "coop_protocol.h"
#include "coop_wire.h"

namespace coop
{
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
	// Returns the blob id.
	uint32_t send_blob(PeerId to, uint32_t kind, std::vector<uint8_t> data);

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
} // namespace coop
