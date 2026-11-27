// Anomaly Coop - a client catches up by itself: the host's roster of entities .
//
// A change of the world reaches a client once (EntitySpawn / Destroy / Switch / Parent / Died, ItemState). A client that
// misses one (its world was not Up: world gate, Hold on a client drops; a death that waited too long for its object; an
// id that was taken here) stayed wrong until it rejoined. Now the host sends, one bucket per kStepMs, the digest of a
// bucket of its entities (Msg::EntityHash, the sums of xfer::LevelHash); a whole round takes kCycleMs and no frame walks
// the whole world. The client builds the same digest when the message arrives: it is on the reliable lane of the
// changes, so everything the host said before it is here.
//
// A difference may be a change in flight, so it has to last to the next round (kStreak, Watch). Then the client asks for
// the rows of that bucket (Msg::EntityResync, answered with Msg::EntityRoster) and compares them with its own (diff). For
// each entity that differs it asks the host for a repair: StateDumpRequest, u16 id, u8 what (Repair). Every repair is a
// command of the host, one of its usual idempotent world messages sent to that client alone: the client never decides
// what the world is. A repair that does not take is asked again later and later (Watch), so a difference nobody can
// heal costs almost nothing.
//
// A row: id, holder, online (a free object only: a child goes with its holder), alive (monsters and stalkers), kind
// (the section: another object under the same id) and a state digest of what has no message of its own: an item nobody
// alive holds (itemtags::state_digest), an NPC's community, rank and reputation (traits_digest), a squad's members
// (members_digest). Items in living hands are not compared: their magazine and condition change with every shot and
// hit, and they travel when they are opened, taken or their holder dies.
//
// Ids are in host numbering on the wire and in a row.
// Wire:
//   EntityHash    u8 bucket, u16 n, u32 sum
//   EntityRoster  u8 bucket, u16 n, n x {u16 id, u16 parent, u8 flags (1 online, 2 alive), u16 kind, u32 state}
//   EntityResync  u8 bucket
//   EntityTraits  u16 object, i32 community, i32 rank, i32 reputation
//   EntitySquad   u16 squad, u16 n, n x u16 member
// At the end: what every copy does with a spawn of an id it has (spawn_of_known), when a squad from the wire is whole
// (squad_whole), and the echo of a fact a map server reported (merged_echo, echo_reaches; tests/test_relay_echo.cpp):
// a stale spawn never replaces the writer's own object (Known::Keep), a repeat's item state goes into an offline copy only
// (repeat_takes_state), a stalled move asks Spawn only for what is missing (stalled_ask), a map server compares its world's digest
// mapless (mapless), coop_squad_complete is raised where the squad's map is led (squad_complete_here).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "coop_ids.h"
#include "coop_level_hash.h"
#include "coop_hash.h"
#include "coop_wire.h"

namespace coop
{
namespace reconcile
{
constexpr uint32_t kCycleMs = 10000; // a round over all buckets
constexpr uint32_t kStepMs = kCycleMs / xfer::kBuckets; // one bucket: about 3 small messages a second
constexpr uint32_t kStreak = 2; // rounds a bucket must differ before its rows are asked for
constexpr uint32_t kMaxWaitMs = 320000; // the longest wait between two rosters of a bucket that stays different
constexpr uint32_t kRepairWaitMs = 15000; // the first wait before a repair of one entity is asked again; it doubles
constexpr uint32_t kMaxRepairWaitMs = 320000;
constexpr uint32_t kMaxRepairs = 4096; // entities with a repair under way
constexpr uint32_t kMaxQueued = 2048; // repairs waiting for the rate of StateDumpRequest
constexpr uint32_t kRosterGapMs = 100; // host: one roster per client in this time
constexpr uint32_t kMaxRows = 65536 / xfer::kBuckets; // ids of one bucket
constexpr size_t kRowBytes = 11;
constexpr uint8_t kRowOnline = 1, kRowAlive = 2;

// What a client asks the host to say about one entity (StateDumpRequest, u16 id, u8 what).
enum Repair : uint8_t
{
	Spawn = 1, // EntitySpawn: it is missing here
	Switch = 2, // EntitySwitch: its online state and place
	Parent = 4, // EntityParent: its holder
	Died = 8, // EntityDied: it is dead on the host
	State = 16, // ItemState, EntityTraits or EntitySquad, by what it is
	Gone = 32, // EntityDestroy, if the host really has no such id
	Replace = 64, // EntityDestroy, then EntitySpawn: another object has this id on the host
};
// A StateDumpRequest without `what`: an entity a client's handler found missing. No Switch: an EntitySwitch of an entity
// that still cannot be made here (its id is taken) would make that handler ask again, for ever.
constexpr uint8_t kAskMissing = Spawn | Died;

struct Row
{
	uint16_t id = 0;
	uint16_t parent = xfer::kNoParent;
	bool online = false;
	bool alive = true;
	uint16_t kind = 0; // kind_of(section); 0: a player's character, whose section differs between the machines
	uint32_t state = 0;
};

// A section as a kind: never 0 (a row without one).
inline uint16_t kind_of(const char* section)
{
	const uint16_t folded = fold16(fnv1a32(section));
	return folded ? folded : uint16_t(1);
}

inline xfer::Entry entry_of(const Row& r)
{
	xfer::Entry e;
	e.id = r.id;
	e.gen = r.online ? 1 : 0;
	e.parent = r.parent;
	e.alive = r.alive;
	e.digest = (uint64_t(r.kind) << 32) | r.state;
	return e;
}

struct BucketHash
{
	uint8_t bucket = 0;
	uint16_t n = 0;
	uint32_t sum = 0;
};
inline bool operator==(const BucketHash& a, const BucketHash& b)
{
	return a.bucket == b.bucket && a.n == b.n && a.sum == b.sum;
}

// The digest of the rows of one bucket (any order; the same sum as a bucket of xfer::hash_of).
inline BucketHash hash_bucket(uint8_t bucket, const std::vector<Row>& rows)
{
	BucketHash h;
	h.bucket = bucket;
	h.n = uint16_t(rows.size());
	uint64_t sum = 0;
	for (const Row& r : rows)
		sum += xfer::entry_hash(entry_of(r));
	h.sum = uint32_t(sum ^ (sum >> 32));
	return h;
}

inline void write(Writer& w, const BucketHash& h)
{
	w.u8(h.bucket);
	w.u16(h.n);
	w.u32(h.sum);
}

inline bool read(Reader& r, BucketHash& h)
{
	h.bucket = r.u8();
	h.n = r.u16();
	h.sum = r.u32();
	return r.ok() && h.bucket < xfer::kBuckets;
}

inline void write_roster(Writer& w, uint8_t bucket, const std::vector<Row>& rows)
{
	w.u8(bucket);
	w.u16(uint16_t(rows.size()));
	for (const Row& r : rows)
	{
		w.u16(r.id);
		w.u16(r.parent);
		w.u8(uint8_t((r.online ? kRowOnline : 0) | (r.alive ? kRowAlive : 0)));
		w.u16(r.kind);
		w.u32(r.state);
	}
}

inline bool read_roster(Reader& r, uint8_t& bucket, std::vector<Row>& rows)
{
	rows.clear();
	bucket = r.u8();
	const uint16_t n = r.u16();
	if (!r.ok() || bucket >= xfer::kBuckets || n > kMaxRows || r.remaining() < n * kRowBytes)
		return false;
	rows.resize(n);
	for (Row& row : rows)
	{
		row.id = r.u16();
		row.parent = r.u16();
		const uint8_t flags = r.u8();
		row.online = (flags & kRowOnline) != 0;
		row.alive = (flags & kRowAlive) != 0;
		row.kind = r.u16();
		row.state = r.u32();
	}
	return r.ok();
}

struct Action
{
	uint16_t id = 0;
	uint8_t what = 0;
};

// What to ask for an entity both sides have (0: nothing).
inline uint8_t compare(const Row& host, const Row& local)
{
	if (host.kind != local.kind || (host.alive && !local.alive))
		return Replace; // another object under this id: a death is not undone
	uint8_t what = 0;
	if (host.parent != local.parent)
		what |= Parent; // the answer says online too
	else if (host.parent == xfer::kNoParent && host.online != local.online)
		what |= Switch;
	if (!host.alive && local.alive)
		what |= Died;
	if (host.state != local.state)
		what |= State;
	return what;
}

// The repairs that make `local` equal to `host`, free objects first: a holder is fetched before what it holds.
inline std::vector<Action> diff(std::vector<Row> host, std::vector<Row> local)
{
	const auto by_id = [](const Row& a, const Row& b) { return a.id < b.id; };
	std::sort(host.begin(), host.end(), by_id);
	std::sort(local.begin(), local.end(), by_id);
	std::vector<Action> free_ones, held;
	size_t h = 0, l = 0;
	while (h < host.size() || l < local.size())
	{
		if (l == local.size() || (h < host.size() && host[h].id < local[l].id))
		{
			(host[h].parent == xfer::kNoParent ? free_ones : held).push_back({host[h].id, uint8_t(Spawn)});
			++h;
		}
		else if (h == host.size() || local[l].id < host[h].id)
		{
			free_ones.push_back({local[l].id, uint8_t(Gone)});
			++l;
		}
		else
		{
			if (const uint8_t what = compare(host[h], local[l]))
				(host[h].parent == xfer::kNoParent ? free_ones : held).push_back({host[h].id, what});
			++h;
			++l;
		}
	}
	free_ones.insert(free_ones.end(), held.begin(), held.end());
	return free_ones;
}

// A client: when to believe a difference, and how often to ask.
class Watch
{
public:
	// The digest of `bucket` came: `same` as this machine's or not. True: ask for its rows now.
	bool on_hash(uint8_t bucket, bool same, uint64_t now_ms)
	{
		Bucket& b = m_buckets[bucket % xfer::kBuckets];
		if (same)
		{
			if (b.streak >= kStreak)
				++m_healed;
			b = Bucket();
			for (auto it = m_repairs.begin(); it != m_repairs.end();)
				it = xfer::bucket_of(it->first) == bucket ? m_repairs.erase(it) : std::next(it);
			return false;
		}
		if (b.streak < 255)
			++b.streak;
		if (b.streak < kStreak || now_ms < b.next)
			return false;
		b.wait = b.wait ? (std::min)(b.wait * 2, kMaxWaitMs) : kCycleMs * 2;
		b.next = now_ms + b.wait;
		return true;
	}
	// A repair of `id` may be asked now (it is counted).
	bool allow(uint16_t id, uint64_t now_ms)
	{
		const auto it = m_repairs.find(id);
		if (it == m_repairs.end())
		{
			if (m_repairs.size() >= kMaxRepairs && !make_room(now_ms))
				return false;
			m_repairs[id] = {now_ms + kRepairWaitMs, kRepairWaitMs};
			return true;
		}
		if (now_ms < it->second.next)
			return false;
		it->second.wait = (std::min)(it->second.wait * 2, kMaxRepairWaitMs);
		it->second.next = now_ms + it->second.wait;
		return true;
	}
	uint32_t watched() const
	{
		uint32_t n = 0;
		for (const Bucket& b : m_buckets)
			n += b.streak ? 1 : 0;
		return n;
	}
	size_t repairing() const { return m_repairs.size(); }
	uint64_t healed() const { return m_healed; } // buckets that matched again after a roster was asked for
	void reset()
	{
		*this = Watch();
	}

private:
	struct Bucket
	{
		uint32_t streak = 0;
		uint32_t wait = 0;
		uint64_t next = 0;
	};
	struct Repairing
	{
		uint64_t next = 0;
		uint32_t wait = 0;
	};
	// A row that never heals keeps its entry: one whose wait is over gives way to a repair that may still work.
	bool make_room(uint64_t now_ms)
	{
		for (auto it = m_repairs.begin(); it != m_repairs.end(); ++it)
			if (now_ms >= it->second.next)
			{
				m_repairs.erase(it);
				return true;
			}
		return false;
	}
	Bucket m_buckets[xfer::kBuckets];
	std::map<uint16_t, Repairing> m_repairs;
	uint64_t m_healed = 0;
};

// A client: repairs waiting for the rate of StateDumpRequest, one entry per entity.
class Queue
{
public:
	void push(uint16_t id, uint8_t what)
	{
		const auto it = m_what.find(id);
		if (it != m_what.end())
		{
			it->second |= what;
			return;
		}
		if (m_order.size() >= kMaxQueued)
		{
			++m_refused;
			return;
		}
		m_what[id] = what;
		m_order.push_back(id);
	}
	bool pop(Action& out)
	{
		if (m_order.empty())
			return false;
		out.id = m_order.front();
		m_order.pop_front();
		const auto it = m_what.find(out.id);
		out.what = it->second;
		m_what.erase(it);
		return true;
	}
	size_t size() const { return m_order.size(); }
	uint64_t refused() const { return m_refused; }
	void clear()
	{
		m_order.clear();
		m_what.clear();
	}

private:
	std::deque<uint16_t> m_order;
	std::map<uint16_t, uint8_t> m_what;
	uint64_t m_refused = 0;
};

// An EntitySpawn that found its id taken here (a local object, our id generator). The id frees by itself in a moment
// (a bolt in flight, a lease given back): the entity is asked for again a few times; the roster takes it from there.
constexpr uint32_t kRetryGapMs = 2000;
constexpr uint32_t kRetryForMs = 20000;

class Retry
{
public:
	void add(uint16_t id, uint64_t now_ms)
	{
		if (m_ids.size() < kMaxQueued)
			m_ids.emplace(id, Entry{now_ms + kRetryGapMs, now_ms + kRetryForMs}); // a repeat keeps its first end
	}
	void done(uint16_t id) { m_ids.erase(id); }
	// The ids to ask for now.
	void due(uint64_t now_ms, std::vector<uint16_t>& out)
	{
		for (auto it = m_ids.begin(); it != m_ids.end();)
		{
			if (now_ms > it->second.until)
			{
				it = m_ids.erase(it);
				continue;
			}
			if (now_ms >= it->second.next)
			{
				out.push_back(it->first);
				it->second.next = now_ms + kRetryGapMs;
			}
			++it;
		}
	}
	size_t size() const { return m_ids.size(); }
	void clear() { m_ids.clear(); }

private:
	struct Entry
	{
		uint64_t next = 0;
		uint64_t until = 0;
	};
	std::map<uint16_t, Entry> m_ids;
};

// What a client's copy of an NPC should say about its community, rank and reputation.
inline uint32_t traits_digest(int32_t community, int32_t rank, int32_t reputation)
{
	const uint64_t a = xfer::mix64(uint64_t(uint32_t(community)) + 0x2545F4914F6CDD1Dull);
	const uint64_t b = xfer::mix64(a ^ ((uint64_t(uint32_t(rank)) << 32) | uint32_t(reputation)));
	return uint32_t(b ^ (b >> 32)) | 1u; // never 0: a row without a state
}

struct TraitsMsg
{
	uint16_t object = kNoId;
	int32_t community = 0;
	int32_t rank = 0;
	int32_t reputation = 0;
};

inline void write(Writer& w, const TraitsMsg& m)
{
	w.u16(m.object);
	w.i32(m.community);
	w.i32(m.rank);
	w.i32(m.reputation);
}

inline bool read(Reader& r, TraitsMsg& m)
{
	m.object = r.u16();
	m.community = r.i32();
	m.rank = r.i32();
	m.reputation = r.i32();
	return r.ok();
}

// The set of a squad's members (order-independent: a squad's list is a map here and there).
inline uint32_t members_digest(const std::vector<uint16_t>& members)
{
	uint64_t sum = 0x9E3779B97F4A7C15ull;
	for (uint16_t id : members)
		sum += xfer::mix64(uint64_t(id) + 0x632BE59BD9B4E019ull);
	return uint32_t(sum ^ (sum >> 32)) | 1u;
}

struct SquadMsg
{
	static constexpr size_t kMaxMembers = 256;

	uint16_t squad = kNoId;
	std::vector<uint16_t> members;
};

inline void write(Writer& w, const SquadMsg& m)
{
	w.u16(m.squad);
	const size_t n = (std::min)(m.members.size(), SquadMsg::kMaxMembers);
	w.u16(uint16_t(n));
	for (size_t i = 0; i < n; ++i)
		w.u16(m.members[i]);
}

inline bool read(Reader& r, SquadMsg& m)
{
	m.members.clear();
	m.squad = r.u16();
	const uint16_t n = r.u16();
	if (!r.ok() || n > SquadMsg::kMaxMembers || r.remaining() < n * sizeof(uint16_t))
		return false;
	m.members.resize(n);
	for (uint16_t& id : m.members)
		id = r.u16();
	return r.ok();
}

// ---------------------------------------------------------------- the receive of every copy

// What a copy does with an EntitySpawn of an id it already has. The same kind is a repeat (harmless, nothing
// changes); another kind is another object under that id now (the copy missed a release and the id was given again): the copy's
// object goes and the writer's comes (Replace), at once, not a roster round later. Kind 0 is a player's character (kind_of never
// gives 0): its section differs between the machines (a client's own actor, the proxies), it is never replaced.
// `writes_known`: this process writes the object it has under that id (writing::writes; a mirror never does). Then another kind is
// a stale word of its world (the echo of a spawn of its own from before it released the object and made another under the id, an
// answer to an older ask): the writer is the truth of its object, the stale spawn goes nowhere (Keep; verify-D finding 7).
enum class Known : uint8_t
{
	Repeat,
	Replace,
	Keep,
};

inline Known spawn_of_known(uint16_t kind_here, uint16_t kind_spawn, bool writes_known)
{
	if (kind_here == 0 || kind_spawn == 0 || kind_here == kind_spawn)
		return Known::Repeat;
	return writes_known ? Known::Keep : Known::Replace;
}

// The item state of a repeated spawn (the initial-state tail) goes into the copy's entity, and the decider's
// versions with it, only where both can be taken: this process does not write the object (`writes_it`: its own state stands) and has no
// game object for it (`object_here`: the object's own state stands, and versions without their values would claim a write it has not
// applied, so the decider's push of that write would be dropped as known).
inline bool repeat_takes_state(bool writes_it, bool object_here)
{
	return !writes_it && !object_here;
}

// A squad from the wire lists its members by id, and they may come after it. It is whole when it lists somebody and
// every listed id is here (`here(id)`). A copy that runs the world's logic (not a mirror) raises coop_squad_complete(squad) at the
// spawn that makes it whole: the squad's own (its members came first) or its last missing member's. A repeated spawn changes
// nothing, so it raises nothing.
template <class Here>
inline bool squad_whole(const std::vector<uint16_t>& members, Here here)
{
	if (members.empty())
		return false;
	for (const uint16_t id : members)
		if (!here(id))
			return false;
	return true;
}

// ---------------------------------------------------------------- the relay down

// A fact a map server reported reaches every other copy once. The world applies it under the EchoOf of the reporting level
// (levels::echo_now); what it records then (a holder change, a death, a release) keeps that level until the frame's flush sends it,
// when the scope is over. That level's players and its map server have it already: the broadcast skips them (the players: the
// passes_to of an EchoOf; the bridge: echo_reaches below, in the mode's delivery).
constexpr uint16_t kNoEcho = kNoLevel;

// Two records of one object in a frame keep the level only when both came from it: a change of the world's own in between (or one
// from another level) goes to everybody, the last state of the object is in the flush.
inline uint16_t merged_echo(uint16_t first, uint16_t then)
{
	return first == then ? first : kNoEcho;
}

// Does a broadcast made under the echo of level `echo` reach the copy that leads level `at` (a map server's bridge)?
inline bool echo_reaches(uint16_t echo, uint16_t at)
{
	return echo == kNoEcho || at != echo;
}

// ---------------------------------------------------------------- repairs

// A holder change (coop_events.cpp client_step_parents) that did not finish in time asks for the item and for its holder.
// An EntitySpawn answer is the object's whole subtree (a player's character with all he carries), so Spawn is asked only for
// an id this copy lacks (`here` false); in that case Gone lets the authority confirm the id was destroyed.
inline uint8_t stalled_ask(bool here)
{
	return here ? uint8_t(Parent | State) : uint8_t(Spawn | Parent | State | Gone);
}

// A row as a copy that leads a map of its own compares it with its world (a map server). What is online on its map is
// its own business: its world holds such an object offline (online_here is false there) and cannot say it. The online flag goes; a
// player's character (kind 0) keeps the flag row_of gives it at both ends. The other half of the mapless row, an item state without the
// uses only a game object shows, is the engine's (coop_entities.cpp mapless_row). The world sends its links the digest of mapless rows.
inline Row mapless(Row r)
{
	if (r.kind != 0)
		r.online = false;
	return r;
}

// coop_squad_complete makes the srv Lua put a squad in its smart, a write to the smart. It is raised where the
// smart is written: by the process that leads the squad's map apart from its world (`leads_apart`: Mode::level() is a level side, a map
// server), for a squad standing on that map (`squad_level` == `my_level`, both known; kNoEcho is no level).
inline bool squad_complete_here(bool leads_apart, uint16_t squad_level, uint16_t my_level)
{
	return leads_apart && my_level != kNoEcho && squad_level == my_level;
}

// An ItemState that came to a deciding process over its link to its own host: a map server's, from its world (no other process both
// decides and has that link). The world sends it one alone: its answer to the map server's repair of an item row (State, coop_roster.cpp
// host_on_entity_request -> items::host_send_state), for an object the map server does not write (client_on_roster asks nothing of
// what it writes). It is the writer's word: the map server's copy takes it as any mirror does (`mirror`, coop_items.cpp client_on_state).
// It is no player's report: taken as one (host_on_state) it was refused "no character", peer 0 having no character there (stand
// dev65b, dev66c). The object that has become the map server's own between the ask and the answer keeps its own state.
inline bool link_state_mirrors(bool is_state, bool writes_here)
{
	return is_state && !writes_here;
}
} // namespace reconcile
} // namespace coop
