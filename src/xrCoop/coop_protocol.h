// Anomaly Coop — protocol constants, message types, handshake data.
// Pure C++17, no engine dependencies.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "coop_wire.h"

namespace coop
{
// Bump on any incompatible change of any message layout.
// 2: TradeOp Deal, ReqMoney with deal ledger
// 3: EntitySound (NPC voices on clients); the EntitySnapshot animation-count byte carries creature flags (W5 C2)
// 4: TradeOp Deal carries item ids only, the host prices the deal (review R4 2);
//    ReqSpawn carries the section and an ItemState blob instead of raw spawn bytes, marker 0xC6 (review R3 V6)
// 5: world epoch: Welcome and BlobBegin carry it, core WorldEpoch (client -> host), SessionReset carries the new one;
//    TransitionVote carries a level changer id instead of a raw M_CHANGE_LEVEL packet (review R1 I3-I4, R2 I1, I3, I4)
// 6: AvatarList (host -> clients): the owner of every player character (review R3 В3)
constexpr uint32_t kProtocolVersion = 6;
constexpr uint16_t kDefaultPort = 27960;
constexpr uint32_t kMaxPlayers = 8; // host + 7 clients

// Transport peer slot: the host is always 0, clients get 1..kMaxPlayers-1.
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

	// --- game (dispatched to registered handlers) ---
	GameBase = 0x0100,

	// session / join / world lifecycle
	JoinRequest = 0x0100, // client -> host: ready to receive the world
	WorldBlob = 0x0101, // host -> client: blob id of the world save follows
	WorldLoaded = 0x0102, // client -> host: save loaded, local actor ready
	JoinComplete = 0x0103, // host -> client: catch-up done, live sync on
	SessionReset = 0x0104, // host -> clients: world replaced (host load): str reason, u32 new world epoch; rejoin
	LevelChange = 0x0105, // host -> clients: group transition, rejoin after
	TransitionVote = 0x0106, // client -> host: u16 level changer (host numbering) the client stands in
	AvatarList = 0x0107, // host -> clients: AvatarListMsg, who plays each player character in the world (R3 В3)

	// players
	PlayerState = 0x0110, // unreliable movement snapshot (owner -> host -> others)
	PlayerVitals = 0x0111, // health, bleeding, radiation, satiety
	PlayerEquip = 0x0112, // visual, active slot item, torch, NVG, helmet
	PlayerAnim = 0x0113, // motion ids by partition
	PlayerShot = 0x0114, // shot event: weapon, origin, direction
	PlayerGrenade = 0x0115,
	PlayerHit = 0x0116, // host -> owner: damage dealt to that player's character
	PlayerWounded = 0x0117, // owner -> host -> all: life state (alive / wounded / dead / spectating), coop_survival
	PlayerRevive = 0x0118, // revive request / host orders (revive, respawn, spectate, refused)
	PlayerDied = 0x0119, // engine death of a player character (coop_events relay)
	PlayerRules = 0x011A, // host -> all: survival rules (death mode, bleed-out, revive health, respawn delay)

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
	ItemState = 0x012B, // owned item state (ammo, condition, addons, upgrades, uses) and unload ammo spawn (WP9)
	EntitySound = 0x012C, // host -> one client, unreliable: sounds of NPCs/monsters within its hearing range (W5 C2)

	// requests (client -> host) and results
	ReqSpawn = 0x0140, // section, parent, position; host spawns with its id
	ReqDestroy = 0x0141,
	ReqTake = 0x0142, // pick up / take from container
	ReqDrop = 0x0143,
	ReqTransfer = 0x0144, // move item between owners/containers, gifts
	ReqUse = 0x0145, // eat / use: host destroys, owner applies effect
	ReqHit = 0x0146, // hit claim on an NPC / object
	ReqResult = 0x0147, // host -> client: accepted/rejected with request seq
	ReqMoney = 0x0148,
	ReqContainer = 0x0149, // open/close container (shared view)
	ReqItemEvent = 0x014A, // GE_* event on an owned item the host must repeat (upgrade install)
	ReqLaunch = 0x014B, // rocket / launcher grenade fired: launcher, rocket, launch transform and velocity (W3a)

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

	// dialogs and trade
	DialogLock = 0x0180, // client -> host: request talk lock on npc
	DialogLockResult = 0x0181,
	DialogUnlock = 0x0182,
	DialogPhrases = 0x0183, // host -> client: available phrases
	DialogSelect = 0x0184, // client -> host: chosen phrase
	TradeOp = 0x0185,

	// scripting and diagnostics
	ScriptEvent = 0x01A0, // Lua RPC
	StateDumpRequest = 0x01B0,
	StateDump = 0x01B1,
};

// Human-readable name for logs and stats ("Hello", "PlayerState", ...).
// Unknown values return "Msg_0xNNNN" from a static buffer per call site table.
const char* msg_name(Msg type);
inline const char* msg_name(uint16_t type) { return msg_name(static_cast<Msg>(type)); }

// F0 final review: who may send a game message. The join lifecycle goes one way: a Session drops a message that
// comes the other way before its handler, and a host kicks the client that sent it as malformed (as it does for a
// core message the wrong way, Welcome from a client). Core messages are checked by the Session's own dispatch; the
// other game messages go both ways (requests, relays, events), their handlers check the sender.
enum class MsgSender : uint8_t
{
	Either,
	HostOnly, // host -> clients: WorldBlob, JoinComplete, SessionReset, LevelChange, AvatarList
	ClientOnly, // client -> host: JoinRequest, WorldLoaded, TransitionVote
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

// Client -> host, first message after transport connect.
struct HelloMsg
{
	uint32_t protocol = kProtocolVersion;
	std::string build_id; // engine build stamp, must match exactly
	uint64_t content_fp = 0; // manifest_fingerprint of client content
	PlayerKey key{}; // persistent random player identity
	std::string nick;

	void write(Writer& w) const;
	bool read(Reader& r);
};

// Host -> client on acceptance.
struct WelcomeMsg
{
	PeerId slot = kInvalidPeer; // client's own peer id
	uint64_t session_token = 0; // random per session; used to rejoin after level change
	std::string host_nick;
	uint32_t max_players = kMaxPlayers;
	uint32_t world_epoch = 0; // the world the host serves now (Session::set_world_epoch)

	void write(Writer& w) const;
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

struct PeerListEntry
{
	PeerId slot = kInvalidPeer;
	std::string nick;
	uint32_t key_hash = 0; // short hash of the key (never send other players' keys)
};

// R3 В3: the player who plays a character, as Msg::AvatarList carries it (host numbering).
struct AvatarOwner
{
	uint16_t avatar = 0xFFFF; // the character's object id; 0 = the host's own actor
	PeerId peer = kInvalidPeer;
};

// Host -> clients, sent whole whenever it changes (a player has loaded the world, a player left): the host's own
// actor (0, kHostPeer) and the character of every player in the world; one whose player is away or still loading is
// not listed. Nick and colour are not repeated: every machine has them by peer (PeerList nick, colour slot from the
// peer id).
// Wire: u8 count (<= kMaxPlayers), count x (u16 avatar, u32 peer).
struct AvatarListMsg
{
	std::vector<AvatarOwner> owners;

	void write(Writer& w) const;
	// False (owners cleared) on overrun, more rows than kMaxPlayers, avatar 0xFFFF, a peer that is no player slot
	// (>= kMaxPlayers), a character or a player listed twice, id 0 for anybody but the host or the host with another id.
	bool read(Reader& r);
};

// ---------------------------------------------------------------- W5 C2: creature sounds and flags

// One sound a host creature made, as sent to one client (Msg::EntitySound = u8 count, count x NpcSound).
// Wire: u16 object (host numbering), u8 flags, str path, u16 delay_ms,
//       [u16 bone]       if flags & Bone   (bone of the creature's visual the sound follows)
//       [u32 mask]       if flags & Mask   (CSoundPlayer synchro mask: a newer sound of the same object with an
//                                           overlapping mask cuts the older one, as on the host)
//       [vec3 offset]    if flags & Offset (position relative to the object's position when it started)
//       [u8 volume]      if flags & Volume (volume * 100, 0..255)
// path: file under $game_sounds$, lowercase, no extension ("characters_voice\\human_01\\...").
struct NpcSound
{
	enum Flags : uint8_t
	{
		Bone = 1,
		Mask = 2,
		Offset = 4,
		Volume = 8,
		Follow = 16, // keep the sound on the object (bone) while it plays: AI voices; else stays where it started
		Known = Bone | Mask | Offset | Volume | Follow,
	};
	static constexpr size_t kMaxPath = 200;

	uint16_t object = 0xFFFF;
	uint8_t flags = 0;
	std::string path;
	uint16_t delay_ms = 0;
	uint16_t bone = 0xFFFF;
	uint32_t mask = 0;
	float offset[3] = {0.f, 0.f, 0.f};
	uint8_t volume = 100;

	void write(Writer& w) const;
	// False on overrun, unknown flags, an invalid path or a non-finite offset.
	bool read(Reader& r);
};

// A relative sound file name a client may open: 1..kMaxPath chars of [a-z0-9_ .-] and '\\', no "..", does not
// start with a separator, no drive or alias ('$', ':').
bool sound_path_valid(const std::string& path);

// EntitySnapshot: the byte that holds the animation partition count (0..15) carries creature flags in its high
// nibble since protocol 3.
enum CreatureFlags : uint8_t
{
	CreatureTorchLit = 1, // the creature's head torch is on
};
constexpr uint8_t pack_anim_byte(uint8_t anim_count, uint8_t creature_flags)
{
	return uint8_t((anim_count & 0x0F) | ((creature_flags & 0x0F) << 4));
}
constexpr uint8_t anim_count_of(uint8_t packed) { return uint8_t(packed & 0x0F); }
constexpr uint8_t creature_flags_of(uint8_t packed) { return uint8_t(packed >> 4); }
} // namespace coop
