// Anomaly Coop — protocol constants, message types, handshake data.
// Pure C++17, no engine dependencies.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "coop_ids.h"
#include "coop_wire.h"

namespace coop
{
// Bump on any incompatible change of any message layout. Hello takes only this number, so an older peer never reaches a reader
// (no branch for one); what each version changed: docs/PROTOCOL_HISTORY.md.
constexpr uint32_t kProtocolVersion = 35;
// The most seats that play the wire carries: a server's 16 clients (its own seat plays no character, SessionConfig::host_plays;
// limits::client_capacity). How many a session has is its mode's: the start says
// SessionConfig::max_players (no default: co-op 8, the host + 7 with the Steam lobby unchanged; the server 16) and the Welcome
// tells a client (Session::max_players). Counts of players, the cap of SessionConfig/Welcome max_players and of the lists by seat.
constexpr uint32_t kMaxPlayers = 16;
// The slot ids: seat 0 (the host) and the client slots 1 .. kMaxPlayers. Every check of a player's peer id
// (`peer < kMaxPeerSlots`), every array indexed by one and every list that may have a row per slot uses this, not kMaxPlayers.
constexpr uint32_t kMaxPeerSlots = kMaxPlayers + 1;

// Transport peer slot: the host is always 0, clients get 1..kMaxPeerSlots-1 (a co-op host gives at most 1..7, a server 1..16).
using PeerId = uint32_t;
constexpr PeerId kHostPeer = 0;
constexpr PeerId kInvalidPeer = 0xFFFFFFFFu;

using PlayerKey = std::array<uint8_t, 16>;

// Delivery class of a message (ENet channel).
enum class Lane : uint8_t
{
	Reliable = 0, // ordered, reliable: spawns, destroys, events, requests, RPC
	Unreliable = 1, // sequenced, unreliable: snapshots (latest wins)
	Bulk = 2, // ordered, reliable, low priority: world snapshot blobs, join bulk
	Count
};

// First u16 of every packet. Values are part of the protocol: append only.
enum class Msg : uint16_t
{
	// --- core (handled inside Session) ---
	Hello = 0x0001, // client -> host
	Welcome = 0x0002, // host -> client
	Reject = 0x0003, // host -> client, then disconnect
	Bye = 0x0004, // either side, graceful leave with reason text
	BlobBegin = 0x0005, // bulk transfer header: id, kind, size, crc, world epoch of the sender
	BlobChunk = 0x0006,
	BlobEnd = 0x0007, // also ends a cancelled blob early: the receiver drops what it got
	PeerList = 0x0008, // host -> all: joined players (slot, nick, key hash)
	Chat = 0x0009,
	WorldEpoch = 0x000A, // client -> host: u32 world epoch the client rejoins (Session::set_world_epoch)
	// The operator -> the server: OperatorCommandMsg, one console line (xrCoop/coop_operator.h; Session checks the
	// sender, the rate, the length and the list, then on_operator_command)
	OperatorCommand = 0x000B,

	// --- game (dispatched to registered handlers) ---
	GameBase = 0x0100,

	// session / join / world lifecycle
	JoinRequest = 0x0100, // client -> host: ready to receive the world
	WorldBlob = 0x0101, // host -> client: blob id of the world save follows
	WorldLoaded = 0x0102, // client -> host: save loaded, local actor ready
	JoinComplete = 0x0103, // host -> client: catch-up done, live sync on
	SessionReset = 0x0104, // host -> clients: world replaced (host load): str reason, u32 new world epoch; rejoin
	LevelChange = 0x0105, // host -> clients: group transition, rejoin after
	TransitionVote = 0x0106, // client -> host: u8 op (ready / cancel / consent / route), u16 level changer (host numbering), [u16 route], coop_transition.h VoteMsg
	AvatarList = 0x0107, // host -> clients: AvatarListMsg, who plays each player character in the world
	CharOffer = 0x0108, // host -> client: CharOfferMsg, his key has no character here: choose one
	CharChoice = 0x0109, // client -> host: CharChoiceMsg, faction and bought gear; the host answers with the world
	TransitionState = 0x010A, // host -> all: coop_transition.h StateMsg, who waits at which exit for the group

	// players
	PlayerState = 0x0110, // unreliable movement snapshot (owner -> host -> others)
	PlayerVitals = 0x0111, // health, bleeding, radiation, satiety; optional tail: the layout of the owner's things
	                       // (u8 1, varu n, n x {u16 item, u16 tag, u8 place<<6|slot}, xrCoop/coop_gear.h)
	PlayerEquip = 0x0112, // visual, active slot item, torch, NVG, helmet
	PlayerAnim = 0x0113, // motion ids by partition
	PlayerShot = 0x0114, // shot event: weapon, origin, direction; what a host creature or machine does: coop_presence.h
	PlayerGrenade = 0x0115,
	PlayerHit = 0x0116, // host -> owner: damage dealt to that player's character
	PlayerWounded = 0x0117, // owner -> host -> all: life state (alive / wounded / dead / spectating), coop_survival
	PlayerRevive = 0x0118, // revive request / host orders (revive, respawn, spectate, refused)
	PlayerDied = 0x0119, // engine death of a player character (coop_events relay)
	PlayerRules = 0x011A, // host -> all: survival rules (death mode, bleed-out, revive health, respawn delay); ends
	                      // with u8 share_pct (coop_death_share); drop 3 = keep (nothing is lost)

	// entities (host -> clients)
	EntitySpawn = 0x0120, // full CSE state with fixed id
	EntityDestroy = 0x0121,
	EntitySwitch = 0x0122, // online/offline switch with position
	EntitySnapshot = 0x0123, // batch of object snapshots (unreliable)
	EntityAnim = 0x0124, // motion ids for NPC/monster partitions
	EntityEvent = 0x0125, // engine GE_* event relay
	EntityHealth = 0x0126,
	EntityDied = 0x0127,
	EntityParent = 0x0128, // ownership change: item -> new parent
	EntityState = 0x0129, // generic CSE state update (doors, physics, zones)
	EntityLocal = 0x012A, // non-ALife child entity (fake missile, rocket): spawn/destroy with a fixed id
	ItemState = 0x012B, // owned item state (ammo, condition, addons, upgrades, uses) and unload ammo spawn
	EntitySound = 0x012C, // host -> one client: sounds of NPCs/monsters within its hearing range, a long line reliable
	EntityVisual = 0x012D, // host -> all: EntityVisualMsg, a host creature's new visual

	// requests (client -> host) and results
	ReqSpawn = 0x0140, // section, parent, position; host spawns with its id
	ReqDestroy = 0x0141,
	ReqTake = 0x0142, // pick up / take from container
	ReqDrop = 0x0143,
	ReqTransfer = 0x0144, // move item between owners/containers, gifts
	ReqUse = 0x0145, // eat / use: host destroys, owner applies effect
	ReqHit = 0x0146, // hit claim on an NPC / object
	ReqResult = 0x0147, // host -> client: accepted/rejected with request seq
	ReqMoney = 0x0148, // client -> host: u32 seq, i32 delta, u8 cause, u32 ref (coop_ledger.h MoneyReq)
	ReqContainer = 0x0149, // open/close container (shared view)
	ReqItemEvent = 0x014A, // GE_* event on an owned item the host must repeat (upgrade install)
	ReqLaunch = 0x014B, // rocket / launcher grenade fired: launcher, rocket, launch transform and velocity

	// world state
	TimeSync = 0x0160,
	WeatherSync = 0x0161,
	Emission = 0x0162,
	ZoneState = 0x0163,
	TimeRequest = 0x0164, // client -> host: u32 minutes the player wants to sleep / skip (host decides)

	// quests, story, PDA
	InfoPortion = 0x0170,
	TaskList = 0x0171, // full list, host-computed texts/targets
	TaskUpdate = 0x0172,
	MapSpot = 0x0173,
	Relation = 0x0174, // goodwill / community relation change
	Progress = 0x0175, // rank, reputation, stats
	StoryHash = 0x0176, // host -> clients every 10 s: count + hash per story domain (P5 drift check)
	StoryResync = 0x0177, // client -> host: send full lists for a domain mask
	// Leased object ids (xrCoop/coop_id_lease.h IdsMsg: u32 epoch, varu n, n x u16 id)
	IdLease = 0x0178, // host -> client: ids added to his lease (he creates his own items under them at once)
	IdLeaseAck = 0x0179, // client -> host: ids he gives back (busy on his machine, or his create was refused)

	// dialogs and trade
	DialogLock = 0x0180, // client -> host: request talk lock on npc
	DialogLockResult = 0x0181,
	DialogUnlock = 0x0182,
	DialogPhrases = 0x0183, // host -> client: available phrases
	DialogSelect = 0x0184, // client -> host: chosen phrase
	TradeOp = 0x0185,

	// Mod layer (0x0190-0x019F)
	MoneyBalance = 0x0190, // host -> the player it belongs to: u32 balance, u32 acked seq, a note (coop_ledger.h)
	// coop.sync. All of them on Lane::Reliable: one ordered lane.
	SyncSet = 0x0191, // client -> host: a change of his own player / object data; host -> clients: every change, answers
	SyncFull = 0x0192, // host -> client: the full copy of what goes to him, in parts, before his world (join)
	SyncRequest = 0x0193, // client -> host: coop.sync.request to a handler of the host's Lua
	SyncResult = 0x0194, // host -> client: its answer (ok, result)
	SyncCast = 0x0195, // host -> clients: coop.sync.broadcast (effects: visuals and sound)

	// Checkpoints, shared sleep and death (0x01C0-0x01CF; docs/CHECKPOINTS_DEATH.md). Characters in host numbering.
	CheckpointState = 0x01C0, // host -> client(s): u16 character, u8 flags (1 rollback, 2 cleared), checkpoint::write(Record)
	                          // or flags 4 alone: u8 what, u8 reason (his own claim refused)
	CheckpointClaim = 0x01C1, // client -> host: u8 what (1 tent, 2 sleeping bag, 3 pack up), vec3 pos
	RestRequest = 0x01C2, // client -> host: u8 op (1 lie down, 2 get up), u8 hours, u8 forced
	RestState = 0x01C3, // host -> all: u8 block, u8 n, n x {u16 character, u8 state, u8 hours}, u8 refused
	RestSleep = 0x01C4, // host -> every sleeper: u8 hours
	DeathMarker = 0x01C5, // host -> all: u16 character, u16 marker object, u8 drop mode
	NewCharacter = 0x01C6, // client -> host: hardcore, the dead player starts a new character
	// A monster's special ability hits a remote player: the host decides, the player's machine plays
	// it on its own actor (camera / PP effectors, impulse, psy damage). u16 monster (host id), u8 kind, u8 phase, f32 value.
	MonsterEffect = 0x01C7, // host -> the targeted player

	// Split levels (0x01D0-0x01DF; docs/SPLIT_LEVELS.md; layouts: xrCoop/srv/coop_levels_wire.h, scopes: srv/coop_scope.h).
	// "World server" is the host's process, "map server" a background process of the host's PC
	// that leads one map for the players on it. Level numbers are game graph level ids.
	LevelMap = 0x01D0, // world server -> players and map servers: who is on which level, which server leads it, the gates
	LevelMove = 0x01D1, // player -> world server: leave the level (u8 kind: 1 exit, 2 trip), seq answered by LevelMoveResult
	LevelMoveResult = 0x01D2, // world server -> player: go / together / refused
	LevelFlush = 0x01D3, // world server -> map server: send the full state (before a save, a move, a stop)
	LevelSnapshot = 0x01D4, // map server -> world server: its objects' state (stream, or a full round)
	LevelEntity = 0x01D5, // map server -> world server: destroy / parent / died / state / item of an object it leads
	LevelXferUp = 0x01D6, // map server -> world server: the map server's half of an ownership transfer (two-phase)
	LevelXferDown = 0x01D7, // world server -> map server: the world server's half (the world server repeats it)
	LevelBrain = 0x01D8, // world server -> map server: assignments of squads to smarts for the squads it leads
	LevelHash = 0x01D9, // map server -> world server: hash of its set of object ids and of 32 buckets (auditor)
	LevelResync = 0x01DA, // world server -> map server: send the full state of the buckets in the mask
	Tunnel = 0x01DB, // host's process <-> map server: a game packet of a player of that map, carried over the trunk
	MapServerState = 0x01DC, // map server -> world server: loading / ready / draining / stopped, frame times, memory
	MapServerOrder = 0x01DD, // world server -> map server: serve / drain / stop
	LoadWait = 0x01DE, // world server -> player on the loading screen: prepared / waiting / enter / cancel, who is ready
	LoadReady = 0x01DF, // player -> world server: my loading screen is up for this epoch (simultaneous load)

	// The journal of a map server (0x01E0-0x01EF; layouts: srv/coop_journal_wire.h, scopes: srv/coop_scope.h): one journal up. Every fact of a
	// map server's frame (a create, a release, a death, a parent, a membership, an info, an item's state, a module's carry, a numbered
	// op done, an ask, the answer to a flush) goes up in order on the reliable lane; the world server reads the batch whole before it
	// applies any entry, and answers it.
	LevelJournal = 0x01E0, // map server -> world server: u32 seq, u16 n, n x {u8 op, varu len, body}
	LevelJournalAck = 0x01E1, // world server -> that map server: u32 upto, the refused entries, an id block

	// scripting and diagnostics
	ScriptEvent = 0x01A0, // Lua RPC
	// The server's debug (docs/DEBUG_DESIGN.md §B.3), its state and the players' personal flags; layout and rules:
	// xrCoop/coop_debug_rules.h StateMsg. World server -> every player (the host player over the LocalLink) and every map server.
	DebugState = 0x01A1,
	StateDumpRequest = 0x01B0, // client -> host: u16 id[, u8 what]: an entity it misses, a repair (coop_reconcile.h Repair)
	StateDump = 0x01B1,

	// a client catches up by itself (coop_reconcile.h)
	EntityHash = 0x0130, // host -> clients: the digest of one bucket of the roster of entities, a round every 10 s
	EntityRoster = 0x0131, // host -> the client that asked: the rows of a bucket
	EntityResync = 0x0132, // client -> host: u8 bucket: my digest of it differs, send its rows
	EntityTraits = 0x0133, // host -> the client that asked: an NPC's community, rank and reputation
	EntitySquad = 0x0134, // host -> the client that asked: a squad's members

	// hanging lamps: the host's word on the lamps that changed (xrCoop/coop_relay_rules.h)
	LampState = 0x0138, // host -> clients: varu n, n x {u16 lamp, u8 LampBits}

	// a bolt of lightning (xrCoop/coop_relay_rules.h)
	Thunderbolt = 0x0139, // host -> clients: str collection, f32 period, f32 duration, u32 seed

	// Vehicles for players (0x01F0-0x01FF; docs/VEHICLES.md). Layouts and
	// bounds: xrCoop/coop_vehicle_rules.h (ReqMsg, SeatMsg, InputMsg, StateMsg); the module: xrGame/coop/coop_vehicles.h. Scope::Map
	// (coop_scope.h): the decider of the sender's level handles them, the co-op host or the map server of that level (the world
	// server tunnels them). Car and character ids in host numbering.
	VehicleReq = 0x01F0, // player -> decider: u8 op (enter / exit / switch seat), u16 car, u8 seat (Reliable)
	VehicleSeat = 0x01F1, // decider -> the players of the level (refused: to the asker only): u16 car, u8 seat, u16 character,
	                      // u8 op (granted / released / refused), u8 reason, [3 x f32 exit position] when released (Reliable)
	VehicleInput = 0x01F2, // driver -> decider: u16 car, u8 seq, u16 held, u8 edges, [i16 yaw, i16 pitch] (Unreliable, sequenced)
	VehicleState = 0x01F3, // decider -> the players of the level: u16 car, u8 flags, u8 rpm, i8 gear, u8 health, u8 fuel, u8 damage
	                       // (Reliable)
};

// Human-readable name for logs and stats ("Hello", "PlayerState", ...).
// Unknown values return "Msg_0xNNNN" from a static buffer per call site table.
const char* msg_name(Msg type);
inline const char* msg_name(uint16_t type) { return msg_name(static_cast<Msg>(type)); }

// Who may send each game message, checked by the Session on receipt: the table is msg_sender in coop_protocol.cpp.
enum class MsgSender : uint8_t
{
	Either,
	HostOnly, // from a client: kicked as malformed before any handler
	ClientOnly, // from the host: a client logs and ignores it
	TrunkOnly, // sent by the bridge of a map server to the host's process; from anybody else: kicked as malformed
	           // (Session::game_message refuses it from everybody, a client of the host included, until the bridge peer
	           // exists)
};
MsgSender msg_sender(Msg type);

enum class RejectReason : uint8_t
{
	None = 0,
	ProtocolMismatch = 1, // different kProtocolVersion
	BuildMismatch = 2, // different engine build id
	ContentMismatch = 3, // different gamedata / mod list; host manifest attached
	DuplicateKey = 4, // player key equals host's or a connected player's (copied game folder)
	SessionFull = 5,
	HostNotReady = 6, // host is not in a level
	Kicked = 7,
	Timeout = 8,
	Malformed = 9,
};
const char* reject_reason_name(RejectReason r);

// One file of the content manifest: relative path (lowercase, '\\' separators) and content hash.
struct ManifestEntry
{
	std::string path;
	uint64_t hash = 0;
};
using Manifest = std::vector<ManifestEntry>;

// Order-independent fingerprint of a manifest (FNV-1a over sorted entries).
uint64_t manifest_fingerprint(const Manifest& m);

// A loose gamedata file (path relative to gamedata, any case, '\\' or '/') that only changes how the world looks or sounds:
// everything under textures\ and sounds\. Left out of the manifest, so a texture pack or another version of one does not
// refuse a join.
bool manifest_visual_only(const std::string& gamedata_rel);

// A text file (same path rules): its size in the manifest is manifest_lf_size. The same file comes with CRLF from a PC with Git
// for Windows and with LF from any other, and nothing the game reads differs.
bool manifest_text_file(const std::string& gamedata_rel);
// The size of `data` with every CRLF counted as one byte.
uint64_t manifest_lf_size(const uint8_t* data, size_t size);

// A loose gamedata file (same path rules) that the game or its addons write at run time: user options, caches, logs,
// `*_user.ltx`, `*.flag`, `*_cache.ltx`, anything under a `cached\` folder. Two players with identical content have
// different copies of them, so they are not content.
bool manifest_game_written(const std::string& gamedata_rel);

struct ManifestDiff
{
	std::vector<std::string> missing_here; // host has, we don't
	std::vector<std::string> extra_here; // we have, host doesn't
	std::vector<std::string> differ; // both have, different hash
	bool empty() const { return missing_here.empty() && extra_here.empty() && differ.empty(); }
};
// Compares local (`here`) against the host's manifest.
ManifestDiff diff_manifests(const Manifest& here, const Manifest& host);

void write_manifest(Writer& w, const Manifest& m);
bool read_manifest(Reader& r, Manifest& out);

// Who is connecting. A map server is a process of the host's PC that connects to the host's process over
// the loopback; it takes no player slot and proves itself with the token the host's process put on its
// command line.
constexpr uint8_t kHelloRolePlayer = 0;
constexpr uint8_t kHelloRoleMapServer = 1;
constexpr size_t kHelloTokenSize = 16; // 128 bits

// Client -> host, first message after transport connect.
// Wire: u32 protocol, str build_id, u64 content_fp, u8 key[16], str nick, u8 role, u8 token[16]. The host
// reads the protocol first (Session::host_hello), so a Hello of another protocol, whatever its length, still gets its
// ProtocolMismatch reject; read() of the whole message wants the tail.
struct HelloMsg
{
	uint32_t protocol = kProtocolVersion;
	std::string build_id; // engine build stamp, must match exactly
	uint64_t content_fp = 0; // manifest_fingerprint of client content
	PlayerKey key{}; // persistent random player identity
	std::string nick;
	uint8_t role = kHelloRolePlayer; // kHelloRolePlayer or kHelloRoleMapServer
	uint8_t token[kHelloTokenSize] = {}; // map server only: the token of its command line; zeros for a player

	void write(Writer& w) const;
	// False on overrun or a role that is none of the two.
	bool read(Reader& r);
};

// What a session is, said once by its host in the
// Welcome. A client keeps it with the host's row (Session::session_mode) from the Welcome on, before any game message of the
// session runs, so that its start installs the mode of its process by it instead of guessing it from the first LevelMap.
// The values are the wire's: append only.
enum class SessionMode : uint8_t
{
	None = 0, // never on the wire: no session, or a Welcome nobody filled (a client refuses it)
	Coop = 1, // the co-op: the host plays, everybody is on its map
	Server = 2, // the server: a world server, whose own seat plays no character, and the map servers it bridges
};
// "coop", "server"; "none" for None and for a value no protocol has.
const char* session_mode_name(SessionMode mode);
// The rule a Welcome is held to: a co-op host's seat plays, a world server's never does (WelcomeMsg::HostSeatPlays,
// SessionConfig::host_plays). Session::host holds a listening host to it and refuses a host that names no mode (SESSION-MODE).
inline SessionMode session_mode_of_seat(bool host_seat_plays)
{
	return host_seat_plays ? SessionMode::Coop : SessionMode::Server;
}

// Host -> client on acceptance.
// Wire: u32 slot, u64 session_token, str host_nick, u32 max_players, u32 world_epoch, u8 flags, u8 mode
// (SessionMode).
struct WelcomeMsg
{
	enum Flags : uint8_t
	{
		HostSeatPlays = 1, // seat 0 is a player (an ordinary host); off on a world server (SessionConfig::host_plays)
		Known = HostSeatPlays,
	};

	PeerId slot = kInvalidPeer; // client's own peer id
	uint64_t session_token = 0; // random per session; used to rejoin after level change
	std::string host_nick;
	uint32_t max_players = kMaxPlayers;
	uint32_t world_epoch = 0; // the world the host serves now (Session::set_world_epoch)
	uint8_t flags = HostSeatPlays;
	SessionMode mode = SessionMode::None; // the session's (Session fills it); None is never sent and never read

	void write(Writer& w) const;
	// False on overrun, an unknown flag, a mode that is not Coop or Server, or a mode the seat contradicts
	// (session_mode_of_seat of HostSeatPlays).
	bool read(Reader& r);
};

// Host -> client on refusal. For ContentMismatch carries the host manifest.
struct RejectMsg
{
	RejectReason reason = RejectReason::None;
	std::string text; // human-readable explanation with a hint how to fix
	Manifest host_manifest; // only for ContentMismatch

	void write(Writer& w) const;
	bool read(Reader& r);
};

// One row of Msg::PeerList (host -> all: varu n, n x PeerListEntry, then varu d, d x {u32 slot, str reason} of who left).
// Wire: u32 slot, str nick, u32 key_hash, u8 flags.
struct PeerListEntry
{
	enum Flags : uint8_t
	{
		Operator = 1, // this player holds the host's rights on a world server (xrCoop/coop_operator.h)
		Known = Operator,
	};
	static constexpr size_t kMinWire = 4 + 1 + 4 + 1; // an empty nick

	PeerId slot = kInvalidPeer;
	std::string nick;
	uint32_t key_hash = 0; // short hash of the key (never send other players' keys)
	uint8_t flags = 0;

	void write(Writer& w) const;
	// False on overrun or an unknown flag.
	bool read(Reader& r);
};

// The operator -> the server (core, Lane::Reliable). Wire: str line. The server takes at most
// op::kMaxLine bytes of it and only the lines of op::allowed (coop_operator.h); read() checks the layout only.
struct OperatorCommandMsg
{
	std::string line;

	void write(Writer& w) const;
	bool read(Reader& r);
};

// The player who plays a character, as Msg::AvatarList carries it (host numbering).
struct AvatarOwner
{
	uint16_t avatar = kNoId; // the character's object id; 0 = the host's own actor
	PeerId peer = kInvalidPeer;
};

// Host -> clients, sent whole whenever it changes (a player has loaded the world, a player left): the host's own
// actor (0, kHostPeer) and the character of every player in the world; one whose player is away or still loading is
// not listed. Nick and colour are not repeated: every machine has them by peer (PeerList nick, colour slot from the
// peer id).
// Wire: u8 count (<= kMaxPeerSlots), count x (u16 avatar, u32 peer).
struct AvatarListMsg
{
	std::vector<AvatarOwner> owners;

	void write(Writer& w) const;
	// False (owners cleared) on overrun, more rows than kMaxPeerSlots, avatar kNoId, a peer that is no player slot
	// (>= kMaxPeerSlots), a character or a player listed twice, id 0 for anybody but the host or the host with another id.
	bool read(Reader& r);
};

// ---------------------------------------------------------------- a new character before the world

// A player whose key has no character in the host's world chooses faction and gear in the
// main menu, before the world is sent, and enters the world as that faction at its start (coop_join.cpp,
// scripts\coop_menu_char.script, xrCoop/coop_newchar.h). The host answers his JoinRequest with Msg::CharOffer, he
// answers with Msg::CharChoice; the host checks it, makes the character and sends the world, or offers again with the
// reason of the refusal. A returning player (his key has a character) gets the world at once, as before.

// A name the join flow carries (faction, item section, level, refusal reason): 1..max characters of [A-Za-z0-9_.-].
// They pick ltx sections, string ids and textures on the other side: no separators, spaces or paths.
constexpr size_t kMaxCharName = 64;
bool char_name_valid(const std::string& s, size_t max = kMaxCharName);

// Host -> client. Wire: u8 eco, u32 points, u8 flags, str level, str refusal, str faction.
struct CharOfferMsg
{
	static constexpr size_t kMaxLevel = 64;
	static constexpr size_t kMaxRefusal = 32;
	enum Flags : uint8_t
	{
		StoryMode = 1, // the host's game has the story on: which faction starts the stock new game lists
		Known = StoryMode,
	};

	uint8_t eco = 1; // the host's economy tier 1..3: what can be bought and its price (new_game_loadouts.ltx)
	uint32_t points = 0; // points for gear of that tier
	uint8_t flags = 0;
	std::string level; // the host's level ("l01_escape"): every player is on it until split levels; may be empty
	std::string refusal; // reason code of the choice the host refused ("points", "busy", ...); empty in a first offer
	// The world's faction, which the host picked when it made the world (character_faction in
	// coop_newchar.h). The window shows it; the player does not choose. Always a name: the window picks a texture and a
	// string id by it.
	std::string faction;

	void write(Writer& w) const;
	// False on overrun, eco outside 1..3, unknown flags, a level, a refusal or a faction that is not a name or too long
	// (the faction is never empty).
	bool read(Reader& r);
};

// Client -> host. Wire: str faction, u8 count (<= kMaxBuy), count x str section.
struct CharChoiceMsg
{
	static constexpr size_t kMaxBuy = 64; // the window cannot buy more: a faction lists a few dozen items

	// The host ignores this field (stalker, bandit, ...). The new character gets the world's faction (CharOffer::faction,
	// newchar::character_faction). The wire layout reserves this field for a selectable faction.
	std::string faction;
	std::vector<std::string> buy; // sections bought with points, one entry per copy; the free kit is not listed

	// What the host reads: a faction and at most kMaxBuy sections, every one a name. The client sends only a valid one.
	bool valid() const;
	void write(Writer& w) const;
	// False (buy cleared) on overrun, more than kMaxBuy entries, a faction or a section that is not a name.
	bool read(Reader& r);
};

// ---------------------------------------------------------------- creature sounds and flags

// One sound a host creature made, as sent to one client (Msg::EntitySound = u8 count, count x NpcSound).
// Wire: u16 object (host numbering), u8 flags, str path, u16 delay_ms,
//       [u16 bone]       if flags & Bone   (bone of the creature's visual the sound follows)
//       [u32 mask]       if flags & Mask   (CSoundPlayer synchro mask: a newer sound of the same object with an
//                                           overlapping mask cuts the older one, as on the host)
//       [vec3 offset]    if flags & Offset (position relative to the object's position when it started)
//       [u8 volume]      if flags & Volume (volume * 100, 0..255)
// path: file under $game_sounds$, lowercase, no extension ("characters_voice\\human_01\\...").
// A stop (flags == Stop | Mask, empty path, delay 0, mask != 0): the authority's CSoundPlayer removed the object's sounds
// whose synchro mask overlaps `mask` (set_sound_mask, remove_active_sounds, a death cry with mask -1); the client releases
// what it plays of that object with an overlapping mask. A reader before this flag rejects the entry as an unknown flag:
// the sender puts a stop last in its packet, so such a reader loses only the stop.
struct NpcSound
{
	enum Flags : uint8_t
	{
		Bone = 1,
		Mask = 2,
		Offset = 4,
		Volume = 8,
		Follow = 16, // keep the sound on the object (bone) while it plays: AI voices; else stays where it started
		Stop = 32,
		Known = Bone | Mask | Offset | Volume | Follow | Stop,
	};
	static constexpr size_t kMaxPath = 200;

	uint16_t object = kNoId;
	uint8_t flags = 0;
	std::string path;
	uint16_t delay_ms = 0;
	uint16_t bone = 0xFFFF;
	uint32_t mask = 0;
	float offset[3] = {0.f, 0.f, 0.f};
	uint8_t volume = 100;

	void write(Writer& w) const;
	// False on overrun, unknown flags, an invalid path or a non-finite offset; for a stop, anything but the stop's form.
	bool read(Reader& r);
};

// A relative sound file name a client may open: 1..kMaxPath chars of [a-z0-9_ .-] and '\\', no "..", does not
// start with a separator, no drive or alias ('$', ':').
bool sound_path_valid(const std::string& path);

// Msg::EntityVisual: a host creature took a new visual (Lua set_visual_name: disguises, mods that dress NPCs).
// Wire: u16 object (host numbering), str visual (a model name as in the spawn data, under $game_meshes$).
struct EntityVisualMsg
{
	static constexpr size_t kMaxVisual = 200;

	uint16_t object = kNoId;
	std::string visual;

	// 1..kMaxVisual chars of [A-Za-z0-9_ .-] and path separators, no "..", does not start with a separator or a dot, no
	// drive or alias (':', '$'): a relative name a client may load.
	bool valid() const;
	void write(Writer& w) const;
	// False on overrun or a name that is not valid().
	bool read(Reader& r);
};

// EntitySnapshot: the byte that holds the animation partition count (0..15) carries creature flags in its high
// nibble. A monster has no torch or strap: it uses those two bits for its look (coop_enemy_rules.h).
enum CreatureFlags : uint8_t
{
	CreatureTorchLit = 1, // the creature's head torch is on
	CreatureHolds = 2, // a stalker's active item: its u16 id follows the anims
	CreatureStrapped = 4, // that item is a weapon carried on the back, not in the hands
	CreatureAims = 8, // the stalker turns spine, shoulder and head to its aim: 3 quaternions follow
};
constexpr uint8_t pack_anim_byte(uint8_t anim_count, uint8_t creature_flags)
{
	return uint8_t((anim_count & 0x0F) | ((creature_flags & 0x0F) << 4));
}
constexpr uint8_t anim_count_of(uint8_t packed) { return uint8_t(packed & 0x0F); }
constexpr uint8_t creature_flags_of(uint8_t packed) { return uint8_t(packed >> 4); }

// A unit-range float (a quaternion component) as int16: x 32767, clamped, rounded to the nearest.
inline int16_t pack_unit(float v)
{
	const float c = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
	const float s = c * 32767.f;
	return int16_t(s >= 0.f ? s + 0.5f : s - 0.5f);
}
inline float unpack_unit(int16_t v)
{
	const float f = float(v) / 32767.f;
	return f < -1.f ? -1.f : f;
}
} // namespace coop
