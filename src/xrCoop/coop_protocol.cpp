// Anomaly Coop — protocol helpers: names, manifest fingerprint/diff, handshake structs.
#include "coop_protocol.h"

#include <algorithm>
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

	// Msg::GameBase aliases Msg::JoinRequest (same value) and is reported under that name.
	case Msg::JoinRequest: return "JoinRequest";
	case Msg::WorldBlob: return "WorldBlob";
	case Msg::WorldLoaded: return "WorldLoaded";
	case Msg::JoinComplete: return "JoinComplete";
	case Msg::SessionReset: return "SessionReset";
	case Msg::LevelChange: return "LevelChange";
	case Msg::TransitionVote: return "TransitionVote";

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
}

bool WelcomeMsg::read(Reader& r)
{
	slot = r.u32();
	session_token = r.u64();
	host_nick = r.str();
	max_players = r.u32();
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
} // namespace coop
