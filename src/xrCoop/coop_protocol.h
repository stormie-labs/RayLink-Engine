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
constexpr uint32_t kProtocolVersion = 1;
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
	BlobBegin = 0x0005, // bulk transfer header
	BlobChunk = 0x0006,
	BlobEnd = 0x0007,
	PeerList = 0x0008, // host -> all: joined players (slot, nick, key hash)
	Chat = 0x0009,

	// --- game (dispatched to registered handlers) ---
	GameBase = 0x0100,

	// session / join / world lifecycle
	JoinRequest = 0x0100, // client -> host: ready to receive the world
	WorldBlob = 0x0101, // host -> client: blob id of the world save follows
	WorldLoaded = 0x0102, // client -> host: save loaded, local actor ready
	JoinComplete = 0x0103, // host -> client: catch-up done, live sync on
	SessionReset = 0x0104, // host -> clients: world replaced (host load), rejoin
	LevelChange = 0x0105, // host -> clients: group transition, rejoin after
	TransitionVote = 0x0106, // client -> host: ready / not ready at level exit

	// players
	PlayerState = 0x0110, // unreliable movement snapshot (owner -> host -> others)
	PlayerVitals = 0x0111, // health, bleeding, radiation, satiety
	PlayerEquip = 0x0112, // visual, active slot item, torch, NVG, helmet
	PlayerAnim = 0x0113, // motion ids by partition
	PlayerShot = 0x0114, // shot event: weapon, origin, direction
	PlayerGrenade = 0x0115,
	PlayerHit = 0x0116, // host -> owner: damage dealt to that player's character
	PlayerWounded = 0x0117,
	PlayerRevive = 0x0118,
	PlayerDied = 0x0119,

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

	// world state
	TimeSync = 0x0160,
	WeatherSync = 0x0161,
	Emission = 0x0162,
	ZoneState = 0x0163,

	// quests, story, PDA
	InfoPortion = 0x0170,
	TaskList = 0x0171, // full list, host-computed texts/targets
	TaskUpdate = 0x0172,
	MapSpot = 0x0173,
	Relation = 0x0174, // goodwill / community relation change
	Progress = 0x0175, // rank, reputation, stats

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
} // namespace coop
