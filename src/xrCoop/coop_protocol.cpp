// Anomaly Coop — protocol helpers: names, manifest fingerprint/diff, handshake structs.
#include "coop_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace coop
{
const char* msg_name(Msg type)
{
	switch (type)
	{
	case Msg::Hello: return "Hello";
	case Msg::Welcome: return "Welcome";
	case Msg::Reject: return "Reject";
	case Msg::Bye: return "Bye";
	case Msg::BlobBegin: return "BlobBegin";
	case Msg::BlobChunk: return "BlobChunk";
	case Msg::BlobEnd: return "BlobEnd";
	case Msg::PeerList: return "PeerList";
	case Msg::Chat: return "Chat";
	case Msg::WorldEpoch: return "WorldEpoch";

	// Msg::GameBase aliases Msg::JoinRequest (same value) and is reported under that name.
	case Msg::JoinRequest: return "JoinRequest";
	case Msg::WorldBlob: return "WorldBlob";
	case Msg::WorldLoaded: return "WorldLoaded";
	case Msg::JoinComplete: return "JoinComplete";
	case Msg::SessionReset: return "SessionReset";
	case Msg::LevelChange: return "LevelChange";
	case Msg::TransitionVote: return "TransitionVote";
	case Msg::AvatarList: return "AvatarList";
	case Msg::CharOffer: return "CharOffer";
	case Msg::CharChoice: return "CharChoice";

	case Msg::PlayerState: return "PlayerState";
	case Msg::PlayerVitals: return "PlayerVitals";
	case Msg::PlayerEquip: return "PlayerEquip";
	case Msg::PlayerAnim: return "PlayerAnim";
	case Msg::PlayerShot: return "PlayerShot";
	case Msg::PlayerGrenade: return "PlayerGrenade";
	case Msg::PlayerHit: return "PlayerHit";
	case Msg::PlayerWounded: return "PlayerWounded";
	case Msg::PlayerRevive: return "PlayerRevive";
	case Msg::PlayerDied: return "PlayerDied";

	case Msg::EntitySpawn: return "EntitySpawn";
	case Msg::EntityDestroy: return "EntityDestroy";
	case Msg::EntitySwitch: return "EntitySwitch";
	case Msg::EntitySnapshot: return "EntitySnapshot";
	case Msg::EntityAnim: return "EntityAnim";
	case Msg::EntityEvent: return "EntityEvent";
	case Msg::EntityHealth: return "EntityHealth";
	case Msg::EntityDied: return "EntityDied";
	case Msg::EntityParent: return "EntityParent";
	case Msg::EntityState: return "EntityState";
	case Msg::EntityLocal: return "EntityLocal";

	case Msg::ReqSpawn: return "ReqSpawn";
	case Msg::ReqDestroy: return "ReqDestroy";
	case Msg::ReqTake: return "ReqTake";
	case Msg::ReqDrop: return "ReqDrop";
	case Msg::ReqTransfer: return "ReqTransfer";
	case Msg::ReqUse: return "ReqUse";
	case Msg::ReqHit: return "ReqHit";
	case Msg::ReqResult: return "ReqResult";
	case Msg::ReqMoney: return "ReqMoney";
	case Msg::ReqContainer: return "ReqContainer";
	case Msg::ReqItemEvent: return "ReqItemEvent";
	case Msg::ReqLaunch: return "ReqLaunch";

	case Msg::TimeSync: return "TimeSync";
	case Msg::WeatherSync: return "WeatherSync";
	case Msg::Emission: return "Emission";
	case Msg::ZoneState: return "ZoneState";
	case Msg::TimeRequest: return "TimeRequest";
	case Msg::StoryHash: return "StoryHash";
	case Msg::StoryResync: return "StoryResync";
	case Msg::ItemState: return "ItemState";
	case Msg::EntitySound: return "EntitySound";
	case Msg::PlayerRules: return "PlayerRules";

	case Msg::InfoPortion: return "InfoPortion";
	case Msg::TaskList: return "TaskList";
	case Msg::TaskUpdate: return "TaskUpdate";
	case Msg::MapSpot: return "MapSpot";
	case Msg::Relation: return "Relation";
	case Msg::Progress: return "Progress";

	case Msg::DialogLock: return "DialogLock";
	case Msg::DialogLockResult: return "DialogLockResult";
	case Msg::DialogUnlock: return "DialogUnlock";
	case Msg::DialogPhrases: return "DialogPhrases";
	case Msg::DialogSelect: return "DialogSelect";
	case Msg::TradeOp: return "TradeOp";

	case Msg::MoneyBalance: return "MoneyBalance";

	case Msg::CheckpointState: return "CheckpointState";
	case Msg::CheckpointClaim: return "CheckpointClaim";
	case Msg::RestRequest: return "RestRequest";
	case Msg::RestState: return "RestState";
	case Msg::RestSleep: return "RestSleep";
	case Msg::DeathMarker: return "DeathMarker";
	case Msg::NewCharacter: return "NewCharacter";

	case Msg::ScriptEvent: return "ScriptEvent";
	case Msg::StateDumpRequest: return "StateDumpRequest";
	case Msg::StateDump: return "StateDump";
	}

	// Unknown value: format into a small rotating buffer so several calls can live in one log line.
	static thread_local char bufs[4][16];
	static thread_local unsigned idx = 0;
	char* buf = bufs[idx++ & 3u];
	std::snprintf(buf, sizeof(bufs[0]), "Msg_0x%04X", unsigned(static_cast<uint16_t>(type)));
	return buf;
}

// F2 T3: every game message has a direction, from its senders in the game layer (net::host_send / host_broadcast
// against net::client_send); one without a sender yet takes the direction of its comment in coop_protocol.h. A new
// message needs its row here: tests/test_session_limits.cpp game_messages_have_direction fails without it, and
// (F2 T3 fix round 1) MSVC does not build it: an enumerator the switch misses is error C4062 (off by default).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(error : 4062)
#endif
MsgSender msg_sender(Msg type)
{
	switch (type)
	{
	// the host's world and its answers
	case Msg::WorldBlob:
	case Msg::JoinComplete:
	case Msg::SessionReset:
	case Msg::LevelChange:
	case Msg::AvatarList:
	case Msg::CharOffer: // protocol 8: the host offers a new character before the world
	case Msg::PlayerHit:
	case Msg::PlayerRules:
	case Msg::EntitySpawn:
	case Msg::EntityDestroy:
	case Msg::EntitySwitch:
	case Msg::EntitySnapshot:
	case Msg::EntityAnim:
	case Msg::EntityHealth:
	case Msg::EntityDied:
	case Msg::EntityParent:
	case Msg::EntityState:
	case Msg::EntityLocal:
	case Msg::EntitySound:
	case Msg::ReqResult:
	case Msg::TimeSync:
	case Msg::WeatherSync:
	case Msg::Emission:
	case Msg::ZoneState:
	case Msg::TaskList:
	case Msg::StoryHash:
	case Msg::DialogLockResult:
	case Msg::DialogPhrases:
	case Msg::MoneyBalance: // F2 T1: only the host holds balances
	case Msg::CheckpointState: // F3: the host sets every checkpoint
	case Msg::RestState: // F3: the host decides the shared sleep
	case Msg::RestSleep:
	case Msg::DeathMarker: // F3: the host moves a dead player's things
	case Msg::StateDump: return MsgSender::HostOnly;

	// a client's requests and reports
	case Msg::JoinRequest:
	case Msg::CharChoice: // protocol 8: the player's choice, the host makes the character
	case Msg::WorldLoaded:
	case Msg::TransitionVote:
	case Msg::PlayerVitals:
	case Msg::EntityEvent: // use / push / throw / activate (coop_objects.h): the host applies it
	case Msg::ReqSpawn:
	case Msg::ReqDestroy:
	case Msg::ReqTake:
	case Msg::ReqDrop:
	case Msg::ReqTransfer:
	case Msg::ReqUse:
	case Msg::ReqHit:
	case Msg::ReqMoney: // F2 T1: the host changes its own money directly
	case Msg::ReqContainer:
	case Msg::ReqItemEvent:
	case Msg::ReqLaunch:
	case Msg::TimeRequest:
	case Msg::StoryResync:
	case Msg::DialogLock:
	case Msg::DialogSelect:
	case Msg::CheckpointClaim: // F3: a player reports his tent, the host checks it
	case Msg::RestRequest: // F3: a player lies down or gets up
	case Msg::NewCharacter: // F3: hardcore, a dead player asks for a new character
	case Msg::StateDumpRequest: return MsgSender::ClientOnly;

	// both: an owner's change the host relays, a client's request the host answers in kind; handlers check the sender
	case Msg::PlayerState:
	case Msg::PlayerEquip:
	case Msg::PlayerAnim:
	case Msg::PlayerShot:
	case Msg::PlayerGrenade:
	case Msg::PlayerWounded:
	case Msg::PlayerRevive:
	case Msg::PlayerDied:
	case Msg::ItemState:
	case Msg::InfoPortion:
	case Msg::TaskUpdate:
	case Msg::MapSpot:
	case Msg::Relation:
	case Msg::Progress:
	case Msg::DialogUnlock:
	case Msg::TradeOp:
	case Msg::ScriptEvent: return MsgSender::Either;

	// core messages: the Session's own dispatch checks them
	case Msg::Hello:
	case Msg::Welcome:
	case Msg::Reject:
	case Msg::Bye:
	case Msg::BlobBegin:
	case Msg::BlobChunk:
	case Msg::BlobEnd:
	case Msg::PeerList:
	case Msg::Chat:
	case Msg::WorldEpoch: return MsgSender::Either;
	}
	return MsgSender::Either; // not a message of the protocol: no handler takes it
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

const char* reject_reason_name(RejectReason r)
{
	switch (r)
	{
	case RejectReason::None: return "None";
	case RejectReason::ProtocolMismatch: return "ProtocolMismatch";
	case RejectReason::BuildMismatch: return "BuildMismatch";
	case RejectReason::ContentMismatch: return "ContentMismatch";
	case RejectReason::DuplicateKey: return "DuplicateKey";
	case RejectReason::SessionFull: return "SessionFull";
	case RejectReason::HostNotReady: return "HostNotReady";
	case RejectReason::Kicked: return "Kicked";
	case RejectReason::Timeout: return "Timeout";
	case RejectReason::Malformed: return "Malformed";
	}
	return "Unknown";
}

// ---------------------------------------------------------------- manifest

uint64_t manifest_fingerprint(const Manifest& m)
{
	// Sort pointers by (path, hash) so the result does not depend on entry order.
	std::vector<const ManifestEntry*> sorted;
	sorted.reserve(m.size());
	for (const ManifestEntry& e : m)
		sorted.push_back(&e);
	std::sort(sorted.begin(), sorted.end(), [](const ManifestEntry* a, const ManifestEntry* b) {
		const int c = a->path.compare(b->path);
		return c != 0 ? c < 0 : a->hash < b->hash;
	});

	constexpr uint64_t kPrime = 1099511628211ull;
	uint64_t h = 14695981039346656037ull;
	auto feed = [&](uint8_t byte) {
		h ^= byte;
		h *= kPrime;
	};
	auto feed64 = [&](uint64_t v) {
		for (int i = 0; i < 8; ++i)
			feed(uint8_t(v >> (8 * i)));
	};

	feed64(sorted.size());
	for (const ManifestEntry* e : sorted)
	{
		feed64(e->path.size()); // length prefix: "ab"+"c" never collides with "a"+"bc"
		for (char c : e->path)
			feed(uint8_t(c));
		feed64(e->hash);
	}
	return h;
}

ManifestDiff diff_manifests(const Manifest& here, const Manifest& host)
{
	std::map<std::string, uint64_t> mine, theirs;
	for (const ManifestEntry& e : here)
		mine[e.path] = e.hash;
	for (const ManifestEntry& e : host)
		theirs[e.path] = e.hash;

	ManifestDiff d;
	for (const auto& kv : theirs)
	{
		const auto it = mine.find(kv.first);
		if (it == mine.end())
			d.missing_here.push_back(kv.first);
		else if (it->second != kv.second)
			d.differ.push_back(kv.first);
	}
	for (const auto& kv : mine)
		if (theirs.find(kv.first) == theirs.end())
			d.extra_here.push_back(kv.first);
	return d;
}

void write_manifest(Writer& w, const Manifest& m)
{
	w.varu(m.size());
	for (const ManifestEntry& e : m)
	{
		w.str(e.path);
		w.u64(e.hash);
	}
}

bool read_manifest(Reader& r, Manifest& out)
{
	out.clear();
	const uint64_t count = r.varu();
	if (!r.ok())
		return false;
	// Every entry needs at least 1 (length) + 8 (hash) bytes: rejects absurd counts before allocating.
	if (count > r.remaining() / 9)
	{
		r.skip(r.remaining() + 1); // marks the reader failed
		return false;
	}
	out.reserve(size_t(count));
	for (uint64_t i = 0; i < count; ++i)
	{
		ManifestEntry e;
		e.path = r.str();
		e.hash = r.u64();
		if (!r.ok())
		{
			out.clear();
			return false;
		}
		out.push_back(std::move(e));
	}
	return true;
}

// ---------------------------------------------------------------- handshake messages

void HelloMsg::write(Writer& w) const
{
	w.u32(protocol);
	w.str(build_id);
	w.u64(content_fp);
	w.bytes(key.data(), key.size());
	w.str(nick);
}

bool HelloMsg::read(Reader& r)
{
	protocol = r.u32();
	build_id = r.str();
	content_fp = r.u64();
	r.bytes(key.data(), key.size());
	nick = r.str();
	return r.ok();
}

void WelcomeMsg::write(Writer& w) const
{
	w.u32(slot);
	w.u64(session_token);
	w.str(host_nick);
	w.u32(max_players);
	w.u32(world_epoch);
}

bool WelcomeMsg::read(Reader& r)
{
	slot = r.u32();
	session_token = r.u64();
	host_nick = r.str();
	max_players = r.u32();
	world_epoch = r.u32();
	return r.ok();
}

void RejectMsg::write(Writer& w) const
{
	w.u8(static_cast<uint8_t>(reason));
	w.str(text);
	if (reason == RejectReason::ContentMismatch)
		write_manifest(w, host_manifest);
}

bool RejectMsg::read(Reader& r)
{
	reason = static_cast<RejectReason>(r.u8());
	text = r.str();
	host_manifest.clear();
	if (reason == RejectReason::ContentMismatch && !read_manifest(r, host_manifest))
		return false;
	return r.ok();
}

// ---------------------------------------------------------------- R3 В3: who plays each player character

void AvatarListMsg::write(Writer& w) const
{
	const size_t count = std::min<size_t>(owners.size(), kMaxPlayers);
	w.u8(uint8_t(count));
	for (size_t i = 0; i < count; ++i)
	{
		w.u16(owners[i].avatar);
		w.u32(owners[i].peer);
	}
}

bool AvatarListMsg::read(Reader& r)
{
	owners.clear();
	const uint8_t count = r.u8();
	if (!r.ok() || count > kMaxPlayers)
		return false;
	std::vector<AvatarOwner> rows;
	rows.reserve(count);
	for (uint8_t i = 0; i < count; ++i)
	{
		AvatarOwner o;
		o.avatar = r.u16();
		o.peer = r.u32();
		if (!r.ok() || o.avatar == 0xFFFF || o.peer >= kMaxPlayers)
			return false;
		// id 0 is the host's own actor, and the host plays nothing else
		if ((o.avatar == 0) != (o.peer == kHostPeer))
			return false;
		for (const AvatarOwner& seen : rows)
			if (seen.avatar == o.avatar || seen.peer == o.peer)
				return false;
		rows.push_back(o);
	}
	owners = std::move(rows);
	return true;
}
// ---------------------------------------------------------------- protocol 8: a new character before the world

bool char_name_valid(const std::string& s, size_t max)
{
	if (s.empty() || s.size() > max)
		return false;
	for (const char ch : s)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		    c == '.' || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

void CharOfferMsg::write(Writer& w) const
{
	w.u8(eco);
	w.u32(points);
	w.u8(flags);
	w.str(level);
	w.str(refusal);
}

bool CharOfferMsg::read(Reader& r)
{
	eco = r.u8();
	points = r.u32();
	flags = r.u8();
	level = r.str();
	refusal = r.str();
	if (!r.ok() || eco < 1 || eco > 3 || (flags & ~uint8_t(Known)) != 0)
		return false;
	if (!level.empty() && !char_name_valid(level, kMaxLevel))
		return false;
	return refusal.empty() || char_name_valid(refusal, kMaxRefusal);
}

bool CharChoiceMsg::valid() const
{
	if (!char_name_valid(faction) || buy.size() > kMaxBuy)
		return false;
	for (const std::string& s : buy)
		if (!char_name_valid(s))
			return false;
	return true;
}

void CharChoiceMsg::write(Writer& w) const
{
	const size_t count = std::min(buy.size(), kMaxBuy);
	w.str(faction);
	w.u8(uint8_t(count));
	for (size_t i = 0; i < count; ++i)
		w.str(buy[i]);
}

bool CharChoiceMsg::read(Reader& r)
{
	buy.clear();
	faction = r.str();
	const uint8_t count = r.u8();
	if (!r.ok() || count > kMaxBuy || !char_name_valid(faction))
		return false;
	std::vector<std::string> items;
	items.reserve(count);
	for (uint8_t i = 0; i < count; ++i)
	{
		items.push_back(r.str());
		if (!r.ok() || !char_name_valid(items.back()))
			return false;
	}
	buy = std::move(items);
	return true;
}

// ---------------------------------------------------------------- W5 C2: creature sounds

bool sound_path_valid(const std::string& path)
{
	if (path.empty() || path.size() > NpcSound::kMaxPath)
		return false;
	if (path[0] == '\\' || path[0] == '.')
		return false;
	if (path.find("..") != std::string::npos)
		return false;
	for (const char ch : path)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '\\' || c == ' ' ||
		    c == '.' || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

void NpcSound::write(Writer& w) const
{
	w.u16(object);
	w.u8(flags);
	w.str(path);
	w.u16(delay_ms);
	if (flags & Bone)
		w.u16(bone);
	if (flags & Mask)
		w.u32(mask);
	if (flags & Offset)
		w.vec3(offset);
	if (flags & Volume)
		w.u8(volume);
}

bool NpcSound::read(Reader& r)
{
	object = r.u16();
	flags = r.u8();
	path = r.str();
	delay_ms = r.u16();
	bone = (flags & Bone) ? r.u16() : uint16_t(0xFFFF);
	mask = (flags & Mask) ? r.u32() : 0u;
	if (flags & Offset)
		r.vec3(offset);
	else
		offset[0] = offset[1] = offset[2] = 0.f;
	volume = (flags & Volume) ? r.u8() : uint8_t(100);
	if (!r.ok() || (flags & ~uint8_t(Known)) != 0 || !sound_path_valid(path))
		return false;
	for (const float v : offset)
		if (!std::isfinite(v) || std::fabs(v) > 1000.f)
			return false;
	return true;
}
} // namespace coop
