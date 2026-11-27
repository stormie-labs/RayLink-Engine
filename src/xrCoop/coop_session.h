// Anomaly Coop — network session: host or client, handshake, routing, stats, bulk blobs.
// Pure C++17 on top of ENet, no engine dependencies. Single-threaded: every call,
// including all callbacks, happens on the thread that calls update() (the game main thread).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "coop_protocol.h"
#include "coop_wire.h"
#include "coop_world_gate.h"

namespace coop
{
// The client side of the protocol: where a process takes the world server's messages and runs its client-side handlers (each
// module's install_client). A remote client's endpoint is its Session (connected to the host); the co-op host's process has the host
// player's end of its LocalLink (coop_local_link.h, co-op layer). Ядро: both modes have it, it answers no question of a mode.
class Endpoint
{
public:
	using Handler = std::function<void(PeerId from, Reader& payload)>;
	virtual ~Endpoint() = default;
	// Game message handlers (type >= Msg::GameBase): one per type, the last wins, an empty one removes it.
	virtual void on(Msg type, Handler h) = 0;
};

class LocalLink; // the co-op's internal channel (coop_local_link.h, included at the end of this file)

// R1 I3: what a host takes from one client in blobs (a client announces any size it likes; the host buffers it).
constexpr uint64_t kMaxClientBlobBytes = 1024 * 1024; // one blob
constexpr uint64_t kMaxClientBlobBytesInFlight = 4 * 1024 * 1024; // announced sizes of its unfinished blobs together

enum class Role : uint8_t
{
	None,
	Host,
	Client
};

// How a client's session ended (Session::end_code), or the name of the RejectReason of a kick after joining.
constexpr const char* kEndLost = "lost"; // no word from the host: the link went silent or closed
constexpr const char* kEndHostLeft = "host_left"; // the host's Bye: it stopped the session itself

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
	bool local = false; // F3a T4: the host player over the LocalLink (in players() of a host with one attached)
	// Protocol 31 (xrCoop/coop_roster.h): false only for seat 0 of a world server, whose seat plays no character (a client
	// learns it from Welcome flags, the host from SessionConfig::host_plays). Such a seat is in no list of players.
	bool plays = true;
	// Protocol 31: this player is the operator of a world server and holds the host's rights (xrCoop/coop_operator.h; a client
	// learns it from the PeerList flag Operator). Seat 0 of an ordinary host has the rights without this flag.
	bool host_rights = false;
};

struct SessionConfig
{
	uint16_t port = kDefaultPort; // host: listen port
	// Host only: the seats that play, the host's own one included when it plays (limits::client_capacity). No default (F5-SEAT-1,
	// docs/MODES.md rule 5): the mode's start says it (Mode::configure: co-op 8, the server 16); host() refuses 0 and cuts what is
	// over kMaxPlayers. A carried host ignores it (its seats are its carrier's).
	uint32_t max_players = 0;
	std::string build_id; // must match between host and client
	Manifest manifest; // local content manifest
	PlayerKey key{};
	std::string nick;
	uint32_t connect_timeout_ms = 10000; // client: transport + handshake deadline
	uint32_t peer_timeout_ms = 15000; // silence before a peer is dropped
	// F3a: who connects (client): a player, or a map server of the host's PC with the token of its command line.
	// Protocol 31, a player (role Player) presents the operator token here (-coop_operator <hex> of the creator's game; zeros
	// for everybody else).
	uint8_t hello_role = kHelloRolePlayer;
	std::array<uint8_t, kHelloTokenSize> hello_token{};
	// Protocol 31, host only. host_plays: the host's own seat is a player (an ordinary host). A world server (-coop_dedicated)
	// sets false: its seat is in no list, the Welcome says so (HostSeatPlays off) and max_players counts clients only
	// (limits::client_capacity). Which player holds the host's rights is the session's part's to say (SessionPart::rights_of): no
	// config holds the token that decides it (A-S4Sb).
	bool host_plays = true;
	// Protocol 33: the session's mode, said in every Welcome of a listening host (WelcomeMsg::mode). The game's start sets it
	// (Mode::configure): host() refuses None, a carried host's too, and a listening host's mode that contradicts its seat. A carried
	// host's session answers its carrier's mode (SessionPart::carrier: a map server's level is part of its world's session, whose
	// Welcome says it).
	SessionMode mode = SessionMode::None;
};

// Per message type counters (P9 / R-12.4).
struct MsgStats
{
	uint64_t sent_count = 0, sent_bytes = 0;
	uint64_t recv_count = 0, recv_bytes = 0;
	uint64_t rate_dropped = 0; // host: received but dropped by limit_rate, from all clients (T12 review, Ruling 18)
	uint64_t world_held = 0; // game message that came while the world was missing and waited for it (coop_world_gate.h)
	uint64_t world_dropped = 0; // ... and was dropped (void without the world, or a client that is not in the host's world)
};

// Lifetime and last five completed seconds of ENet payload on one joined physical link.
struct PeerTraffic
{
	PeerId id;
	uint64_t out_bytes, in_bytes;
	uint64_t out_5s, in_5s;
};

// Network condition emulation for tests (applied on send and receive inside Session).
struct NetSim
{
	uint32_t lag_ms = 0; // added one-way delay
	uint32_t jitter_ms = 0; // +- random extra delay
	float loss = 0.f; // 0..1 drop probability, Unreliable lane only
	bool enabled() const { return lag_ms || jitter_ms || loss > 0.f; }
};

// Transport slots a host keeps beyond its players for connections of another Hello role than a player's (SessionPart::admit).
constexpr uint32_t kTrunkSlots = 7;

class Session;

// The part of a Session that one mode's layer adds (docs/MODES.md section 4, critique H10). The Session runs every connection and
// calls its part at fixed points; it never asks what the part is. A part decides:
//   - the connections of another Hello role than a player's (trunks): who comes in, under which id. A trunk takes no player slot, is
//     in no peers() and no players(), may send MsgSender::TrunkOnly (nobody else may), and no rate limit or in-filter applies to it.
//     send(), send_blob() and kick() take its id like a player's; broadcast() reaches it with what trunk_takes admits;
//   - a host carried by another session: no transport of its own, its peers have none either (Owner::carried_*), what is sent to
//     one the part carries; send(kHostPeer) and what a session does as a client (the epoch, the hold of the link, the silence of
//     the host) go to the carrier, and self() is the carrier's;
//   - which player holds the host's rights (PeerInfo::host_rights), and what a console line of a player (Msg::OperatorCommand) does.
// Without a part a session is what both modes have: it refuses every Hello of another role, gives no player the host's rights and
// runs no console line. A part is made by its layer (Session::part), owned by the Session and kept for its life, across its
// sessions, like the handlers. Every call on the thread of Session::update; what reaches the game goes through Owner::emit.
class SessionPart
{
public:
	// What the Session gives its part to act on it: handed over once, in attached(). Nobody else gets one.
	class Owner
	{
	public:
		virtual Session& session() = 0;
		// One line of the session's log, "[coop] " in front (Session::set_log). The text is a line, never a format.
		virtual void log(const char* line) = 0;
		// `fn` runs at the next safe point of Session::update, after what is queued before it (as the session's callbacks do).
		virtual void emit(std::function<void()> fn) = 0;
		// Host: the joined trunks, by id; is `id` one of them.
		virtual std::vector<PeerId> trunks() const = 0;
		virtual bool trunk(PeerId id) const = 0;
		// A carried host: its peers. join: `id` (the carrier's host's numbering) is one from now on, on_peer_joined follows; nothing
		// if he is one already or this is no carried host. leave: he is not, on_peer_left follows. receive: a game message of his (the
		// 2-byte type first), run here as a remote client's (the same checks, the same world gate); dropped if he is none.
		virtual void carried_join(PeerId id, const std::string& nick) = 0;
		virtual void carried_leave(PeerId id, const std::string& reason) = 0;
		virtual void carried_receive(PeerId from, Lane lane, const uint8_t* packet, size_t size) = 0;

	protected:
		~Owner() = default;
	};

	// What admit() says of a Hello of another role than a player's.
	struct Admission
	{
		bool accept = false;
		PeerId id = kInvalidPeer; // accepted: its peer id, no player slot (kMaxPeerSlots or more)
		std::string nick; // accepted: its name when its Hello has none
		std::string replaced; // accepted: what a joined trunk of the same id hears when this one takes its place
		RejectReason reason = RejectReason::Kicked; // refused: its Reject
		std::string text;
	};

	virtual ~SessionPart() = default;
	virtual void attached(Owner& owner) = 0;
	// host(): the session that carries this host (nullptr: a host with a transport of its own). Asked once by each host() that goes
	// ahead; host_starts follows with the answer once the config is in, before the transport.
	virtual Session* carrier() = 0;
	virtual void host_starts(Session* carrier) = 0;
	// The session ended (shutdown, a refusal, a lost connection): what was given out for it goes.
	virtual void ended() = 0;
	// Host: a Hello of another role than a player's, its protocol, build and content checked; `address` is its "ip:port".
	virtual Admission admit(const std::string& address, const HelloMsg& hello) = 0;
	// Host: the trunk `id` has its Welcome; it is gone (Bye, kick, lost: no trunk() any more).
	virtual void trunk_joined(PeerId id, const std::string& address) = 0;
	virtual void trunk_left(PeerId id, const std::string& reason) = 0;
	// Host: does a broadcast of `type` reach the trunks?
	virtual bool trunk_takes(Msg type) const = 0;
	// Host, a player's Hello before his place is given: is the last place of a full session kept for a holder of the host's rights who
	// has not come (`holder_in`: a joined player holds them)? Nothing is bound and nothing logged.
	virtual bool holds_place(const std::string& address, const HelloMsg& hello, bool holder_in) const = 0;
	// Host, a player's Hello: does `player` (his slot, nick and address set) hold the host's rights?
	virtual bool rights_of(const PeerInfo& player, const HelloMsg& hello) = 0;
	// Host: a console line of a player that passed the rate limit (kClientRateRules OperatorCommand); `may`: he holds the host's
	// rights over a connection of his own. The part judges it; the session never runs it.
	virtual void console_line(const PeerInfo& from, bool may, const std::string& line) = 0;
	// A carried host: a game message to its peer `to` (the 2-byte type, then a, then b), through the carrier. The bytes it sent,
	// counted in stats(type); 0 when it dropped it.
	virtual size_t carry(PeerId to, Lane lane, uint16_t type, const uint8_t* a, size_t na, const uint8_t* b, size_t nb) = 0;
};

// F3a T4: a session connected to a host is the Endpoint of a remote client (its install_client handlers are registered
// with on()); a hosting session runs the server's handlers (install_server), and the host player's end is its LocalLink.
class Session final : public Endpoint
{
public:
	using Handler = Endpoint::Handler;
	using LogFn = std::function<void(const char* line)>;

	Session();
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	// Starts listening with the configured mode, or hosts through its carrier. Returns false and sets err on failure.
	bool host(const SessionConfig& cfg, std::string* err);
	// Starts connecting; result arrives via on_welcome / on_rejected / on_disconnected.
	bool connect(const SessionConfig& cfg, const std::string& address, uint16_t port, std::string* err);
	// Graceful stop: sends Bye(reason) to everyone, flushes, closes. Safe to call twice.
	void shutdown(const char* reason);

	// Pumps the transport, handshake, timeouts, blobs and delayed (NetSim) packets.
	void update(uint64_t now_ms);

	Role role() const;
	// Protocol 33, what this session is (docs/MODES.md section 4: start_client installs the mode of its process by it). Host: what
	// its Welcome says. A carried host (a map server's level, SessionPart::carrier): its carrier's. Client: what the host's Welcome
	// said, kept with the host's row from the Welcome on - set before on_welcome and before any game message of the session runs.
	// None before the Welcome, outside a session, and from the end of a session on.
	SessionMode session_mode() const;
	// F5-SEAT-1: the seats that play in this session (the mode's number; coop.session_info().max_players). Host: its config's (cut to
	// kMaxPlayers). A carried host: its carrier's. Client: what the host's Welcome said (cut to kMaxPlayers), from the Welcome on.
	// 0 before the Welcome, outside a session and from the end of a session on.
	uint32_t max_players() const;
	uint16_t listen_port() const; // host: the port it listens on
	PeerId self() const; // host: 0; client: slot from Welcome, kInvalidPeer before
	bool is_active() const; // host listening, or client joined
	std::vector<PeerInfo> peers() const; // host: all clients; client: host + list from PeerList
	// The peers of peers(), in its order, without the copy: f(const PeerInfo&) for each. f must not end the session or change who
	// is joined (no kick, no disconnect): the walk reads the session's own table.
	template <class F> void for_each_joined(F&& f) const
	{
		visit_joined([](void* fn, const PeerInfo& p) { (*static_cast<std::remove_reference_t<F>*>(fn))(p); },
			const_cast<void*>(static_cast<const void*>(std::addressof(f))));
	}
	size_t joined_count() const; // peers().size() without the copy
	const PeerInfo* peer(PeerId id) const; // one of peers(): the host player (local) is not one
	// F3a T4. Host: the host player (peer 0, `local`, joined, the host's nick) while a LocalLink is attached, then every
	// joined client. Client: the same as peers() (the host, who is a player, and the others). peers() stays the remote
	// ones, so that old code does not count the host twice; modules move to players() one by one.
	std::vector<PeerInfo> players() const;

	// Game message handlers (type >= Msg::GameBase). One handler per type, last wins.
	void on(Msg type, Handler h) override;
	// F3a T4: every game message handler goes (a process installs the handler table of its role anew when it starts to
	// host or to connect: the server's on a hosting session, the client's on a connecting one).
	void clear_handlers();

	// F3a T4, the internal channel of the host's process (coop_local_link.h): while this session hosts, the host player is
	// peer 0 (kHostPeer) flagged local. send(0, ...) and broadcast(...) put the message into `link` (it takes only what its
	// gate admits); what the host player pushes runs the handlers registered here with from = 0, after the same sender
	// check (he is a client: HostOnly and TrunkOnly are refused, logged) and the same world gate as a remote client's game
	// message, with no rate limit and always in this world epoch. What the session sends him runs his handlers on the link
	// (from = 0) after the client's sender check (ClientOnly and TrunkOnly refused) and the same world gate. update() pumps
	// the link: see coop_local_link.h. Kept across sessions like the handlers; nullptr detaches; a session that is not
	// hosting leaves it alone; a session that ends empties its queues. The link must outlive the session or be detached.
	void attach_local(LocalLink* link);
	LocalLink* local() const;

	// H10 (A-S4Sb): the part `kind` names (SessionPart; `kind` is the address of a tag of the layer that owns it), made by `make` the
	// first time it is asked for. nullptr: the Session holds a part of another kind, or none and `make` is null. For that layer's own
	// code: the server's part is srv::SessionSrv's (xrCoop/srv/coop_session_srv.h). The Core never asks for a part: whether there is
	// one would tell it the mode.
	SessionPart* part(const void* kind, std::unique_ptr<SessionPart> (*make)());
	// Host: `id` is a joined peer of this session (a player, a trunk, a carried host's peer). Client: `id` is the host's and this
	// session is joined. What send(id) reaches (the host player's link aside).
	bool joined(PeerId id) const;

	// F3a: the host's last word on a game message that passed everything else (sender, epoch, rate): false drops it, quietly (the
	// filter may have sent it on, coop_trunk.cpp). `packet` is the whole message, the 2-byte type first. Null: every message goes
	// on. Kept across sessions, like the handlers.
	using InFilter = bool (*)(PeerId from, Msg type, Lane lane, const uint8_t* packet, size_t size);
	void set_in_filter(InFilter filter);

	// Sends payload as message `type`. Host: to any client, and to the host player (0) over an attached LocalLink.
	// Client: only to kHostPeer.
	void send(PeerId to, Lane lane, Msg type, const Writer& payload);
	// Host: to every joined client except `except`, and to the host player over an attached LocalLink unless `except` is
	// 0. Client: same as send to host.
	void broadcast(Lane lane, Msg type, const Writer& payload, PeerId except = kInvalidPeer);
	// Disconnects one client with a reason (host only).
	void kick(PeerId id, RejectReason reason, const std::string& text);

	// The owner thread is about to block for long (a level load does not call update() for tens of seconds):
	// keep the connection alive from a helper thread for up to `ms` from now; 0 releases. Without a hold a
	// silent process is dropped by its peers after peer_timeout_ms (a crash dialog must not look alive).
	// Thread-safe (the only Session calls that are: this and seal_connection_hold).
	void hold_connection(uint32_t ms);
	// This process crashed: the hold goes at once and no hold_connection takes any more, for the life of this Session
	// (atomic with hold_connection: one racing with it cannot re-arm). Thread-safe, also mid-crash.
	void seal_connection_hold();
	uint64_t keepalive_services() const; // helper services so far (diagnostics)
	// A client: ms since anything (an acknowledgement too) came from the host; 0 when not a connected client.
	uint32_t ms_since_host_heard() const;
	// A client that has waited for a silent host long enough: the session ends here and now (on_disconnected, before this
	// returns), as if the transport had lost it. ENet alone takes up to twice peer_timeout_ms. Not from a callback.
	void drop_silent_host();
	// A client: how the last session ended, valid from on_disconnected on (see kEndLost).
	const std::string& end_code() const;

	// Bulk transfer of an arbitrary byte blob in chunks over Lane::Bulk, with CRC32 check.
	// Returns the blob id. R1 I4: a blob of the same kind still queued or streaming to that peer is cancelled, the
	// newer one replaces it (a streaming one ends early, its receiver drops it). The blob carries the world epoch of
	// the moment it was queued.
	uint32_t send_blob(PeerId to, uint32_t kind, std::vector<uint8_t> data);

	// R1 I3, host: a blob kind a client may send (none by default). A client that announces another kind, a blob
	// over kMaxClientBlobBytes or more than kMaxClientBlobBytesInFlight of unfinished blobs is kicked as malformed.
	void accept_client_blob(uint32_t kind);

	// R2 I4, host: a client may send `type` at most `burst` times in a row, and once more every `refill_ms` (a token
	// bucket per client). What exceeds it is dropped before its handler (logged once per client and type, counted in
	// stats(type).rate_dropped; the sender is not kicked: a burst may be honest). burst or refill_ms 0 removes the limit.
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

	// A game message is for a world, and the world is not always there (a level
	// is built or torn down, a client is in the menu or loading). `gate` tells the world of this machine whenever a
	// handler is due; with none every message runs (the library's tests, a session before the game sets it). The rule
	// and the table are in coop_world_gate.h: a message whose handler needs a world waits (host) or is dropped (client)
	// until `gate` says Up; waiting messages made in an older world epoch are void. Held messages run in arrival order,
	// before any newer one.
	void set_world_gate(std::function<wgate::World()> gate);
	struct WorldGateStats
	{
		wgate::HoldStats hold;
		size_t queued = 0; // waiting now
		size_t queued_bytes = 0;
		uint64_t gaps = 0; // times the world went missing with a game message due
	};
	WorldGateStats world_gate_stats() const;

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

	// A console line of a player for the host.
	// Client: sends one console line to the host as Msg::OperatorCommand. false if this is not a joined client. It does not check
	// the rights or the line: the host is the judge (Lua checks has_host_rights before, stage 3). The host's side is its part's
	// (SessionPart::console_line); a host without one refuses every line.
	bool send_operator_command(const std::string& line);
	// Client: the host's PeerList gave this client the flag Operator. false on a host (its own rights are is_host()).
	bool self_host_rights() const;

	void set_log(LogFn fn);
	void set_netsim(const NetSim& sim);
	const NetSim& netsim() const;

	// Links of this transport only, including trunks. Independent of the message-type counters and reset_stats().
	std::vector<PeerTraffic> peer_traffic() const;

	// Stats indexed by message type value.
	const MsgStats& stats(uint16_t type) const;
	std::vector<uint16_t> stats_types() const; // types with any traffic
	void reset_stats();

private:
	void visit_joined(void (*visit)(void* fn, const PeerInfo& p), void* fn) const;

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

// The co-op's LocalLink for whoever includes this file and owns one by value (coop_manager.h keeps the host player's link): the line
// goes when the co-op layer owns the link (A-S4C's co-op mode). After the namespace: coop_local_link.h includes this file for Endpoint.
#include "coop_local_link.h"
