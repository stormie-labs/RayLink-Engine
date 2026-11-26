// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the map server's level journal, its batches and the world's answer (TZ Р32, temp/r30/design/R32-final.md §1 W3
// "Up", W2b refusal answers, W5 repair, §5, §8a A11). Pure C++17, header-only: the engine (srv/coop_levels_xfer.cpp) and the
// unit-test exe use it.
//   Batcher     a map server: the registry facts of a frame, in order, cut into LevelJournal batches under 1 MiB (A11: the world
//               gate's hold queue evicts a bigger message) and numbered from 1;
//   Receiver    the world server: the last batch it applied of each bridge (a repeat is answered again, never applied twice);
//   acks_of     the world server: the Ack(s) of one batch, 255 refusals each (a u8 count), the id block in the last one;
//   repair_of   a map server: what it does about an entry the world refused (W2b, W5): it releases a create the world refused,
//               keeping the id reserved (W9), and asks the world for an object whose change or removal it refused; the world's
//               answer is an ordinary EntitySpawn / EntityDestroy (StateDumpRequest Replace). No new message.
//   refused_id  the object a refusal row names;
//   HeldMemory, creature_place, ParentNet   a map server: the head of a move, a death, a release names the root the world still sees
//               when it applies the entry (A6; review WP4 #1, #2), see below.
//   ReleasesUnderWay, adopts_at_left   a map server: who its Holder adopts at a squad's Left (§8a A17; fix round 2 of package A).
//   done::Store     the world server: the done-records of numbered ops, per level (R32 W7.2, §8a A21 E-1), see below;
//   done::Loaded    the records of a save just loaded, until the world server adopts them or a map server raises its level's.
//   done::ReplayGate  a map server's Ready waits until the records it raised reached its Lua (§8a A22 open item a, O6).
//   uses_refusal_of, object_note   what a refusal line says (O6, reports #1644-#1649).
#pragma once

#include "coop_levels_wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace coop
{
namespace levels
{
namespace journal
{
// A11: a LevelJournal stays under 1 MiB with its message header (the world gate's hold queue keeps 1 MiB at most,
// coop_world_gate.h max_bytes); the rest of a frame goes in the next batch, in order.
constexpr size_t kMaxSendBytes = 1000 * 1024;

// The map server's journal: what happened to the objects of its level, in order, until the frame sends it. A batch is cut where
// the next entry would take it over kMaxSendBytes or kMaxEntries; the order of the entries never changes, so a fact is applied
// on the world before every fact that came after it, whichever batch carries it.
class Batcher
{
public:
	// False, nothing kept, for an entry the world would refuse whole (body_ok): it would cost the world the whole batch.
	bool add(Entry e)
	{
		if (!body_ok(e.op, e.body))
		{
			++m_dropped;
			return false;
		}
		m_entries.push_back(std::move(e));
		return true;
	}

	bool empty() const { return m_entries.empty(); }
	size_t pending() const { return m_entries.size(); }
	const std::vector<Entry>& entries() const { return m_entries; }

	// The batches of what was added, numbered on from the last one; the journal is empty afterwards.
	std::vector<wire::LevelJournalMsg> take()
	{
		std::vector<wire::LevelJournalMsg> out;
		wire::LevelJournalMsg cur;
		size_t bytes = 4 + 2;
		for (Entry& e : m_entries)
		{
			const size_t n = 1 + wire::detail::varu_size(e.body.size()) + e.body.size();
			if (!cur.entries.empty() && (bytes + n > kMaxSendBytes || cur.entries.size() >= kMaxEntries))
			{
				cur.seq = next_seq();
				out.push_back(std::move(cur));
				cur = wire::LevelJournalMsg();
				bytes = 4 + 2;
			}
			cur.entries.push_back(std::move(e));
			bytes += n;
		}
		if (!cur.entries.empty())
		{
			cur.seq = next_seq();
			out.push_back(std::move(cur));
		}
		m_entries.clear();
		return out;
	}

	// The seq of the last batch taken (0: none yet).
	uint32_t last_seq() const { return m_seq; }
	// Entries add() refused.
	uint32_t dropped() const { return m_dropped; }

	// A new world or a new session: nothing queued counts, the seq starts at 1 again (the world's Receiver forgets the bridge too).
	void reset()
	{
		m_entries.clear();
		m_seq = 0;
	}

private:
	uint32_t next_seq()
	{
		if (++m_seq == 0)
			++m_seq; // 0 is no batch on the wire
		return m_seq;
	}

	std::vector<Entry> m_entries;
	uint32_t m_seq = 0;
	uint32_t m_dropped = 0;
};

// The world server's memory of each bridge's journal: the last seq it applied. The lane is reliable and ordered, so a seq at or
// below it is a repeat (answered again, applied never); a seq beyond the next one is a gap, applied and counted.
class Receiver
{
public:
	enum class Seen : uint8_t
	{
		Next, // the batch after the last one (or the first)
		Gap, // a later one: batches between were lost (a reconnect the bridge did not hear of)
		Repeat, // applied already
	};

	Seen look(uint32_t bridge, uint32_t seq) const
	{
		const auto it = m_last.find(bridge);
		const uint32_t last = it == m_last.end() ? 0 : it->second;
		if (seq <= last)
			return Seen::Repeat;
		return seq == last + 1 ? Seen::Next : Seen::Gap;
	}
	void applied(uint32_t bridge, uint32_t seq)
	{
		uint32_t& last = m_last[bridge];
		if (seq > last)
			last = seq;
	}
	uint32_t last(uint32_t bridge) const
	{
		const auto it = m_last.find(bridge);
		return it == m_last.end() ? 0 : it->second;
	}
	// The bridge is gone (its next connection numbers from 1 again).
	void forget(uint32_t bridge) { m_last.erase(bridge); }
	void clear() { m_last.clear(); }

private:
	std::map<uint32_t, uint32_t> m_last;
};

// R32 fix A3: does the world apply (and answer) a journal batch of this bridge? A map server the keeper has connected (it said
// Ready: it leads its map), and also the current bridge of its map that has not said Ready yet (Starting: in session, loading its
// world, where it may make objects before it says Ready). The acceptance decides each entry of it as of any other (the writer rule:
// a Starting map server writes no resident, so its creates and changes are refused and its repair releases or fetches them; its
// asks follow the ask rule), and the batch is answered, so its unacknowledged count falls. A bridge no map names any more (cut, lost)
// is dropped unanswered: nothing of a connection that is over counts.
inline bool applies_batch_of(bool keeper_connected, bool current_bridge_of_its_map)
{
	return keeper_connected || current_bridge_of_its_map;
}

// R32 fix A7: a Spawn of `id` waits among the pending ups of the frame (`ups`: op, a = the object; any allocator). Until the flush
// writes it (and Holder::spawned registers a unit there), a unit the map server just made is its own from birth all the same
// (writer::Hold::Born): levels::xfer_born asks this.
template <class Ups>
bool spawn_queued(const Ups& ups, uint16_t id)
{
	for (const auto& u : ups)
		if (u.op == wire::LevelEntityHead::Spawn && u.a == id)
			return true;
	return false;
}

// R32 §8a A17 (fix round 2 of package A, verifier finding 1): the creatures whose release is under way on a map server (local ids).
// The stock release (CALifeSimulatorBase::release) runs the Released hook first, then the releases of the children, then
// unregister_object -> on_unregister -> the squad's unregister_member -> the Left hook, with the creature still alive and in the
// registry, and only then takes it out of the registry. Its Destroy, queued at the Released hook while it was still a member, releases
// it on the world from its squad; so it must not become a unit of its own at that Left (its Destroy would go on the transfer lane,
// where the world has no record of it, and the world would adopt it from the Left after it), whenever the frame's flush runs. A mark
// is taken at the Released hook (a refused create's quiet release too), and goes when an object is made under the id again (spawned)
// or once no object stands under it (prune: `exists(id)` false).
class ReleasesUnderWay
{
public:
	void released(uint16_t id) { m_ids.insert(id); }
	void spawned(uint16_t id) { m_ids.erase(id); }
	bool under_way(uint16_t id) const { return m_ids.count(id) != 0; }
	template <class Exists>
	void prune(Exists exists)
	{
		for (auto it = m_ids.begin(); it != m_ids.end();)
			it = exists(*it) ? std::next(it) : m_ids.erase(it);
	}
	size_t size() const { return m_ids.size(); }
	void clear() { m_ids.clear(); }

private:
	std::set<uint16_t> m_ids;
};

// R32 §8a A17: does a map server's Holder adopt a creature at the Left of a squad (levels::client_adopt_left, at the hook and again at
// the flush)? Only one that leaves a squad it writes, that it does not write already (it keeps its own hold), a living creature on
// the ground that transfers (no player's character; a dead one is nobody's unit: its body is a resident, adopted by nobody), and
// whose release is not under way (fix round 2, finding 1: ReleasesUnderWay). A resident is not a root of gen 0 by its kind: the gen
// at a root is the sender's hold of it for any root (Р68-3, levels::root_gen in coop_levels_root_gen.h). A creature adopted here
// alive that dies later keeps that hold, and its body and loot are named with it on both sides; one that dies in its squad leaves
// it unheld, 0 on both.
struct LeftAdoptFacts
{
	bool squad_written = false; // its Holder authors the squad
	bool member_written = false; // ... and the creature already
	bool living_creature = false; // a living creature, no holder (ID_Parent none), no player's character, one that transfers
	bool releasing = false; // its release is under way here
};

inline bool adopts_at_left(const LeftAdoptFacts& f)
{
	return f.squad_written && !f.member_written && f.living_creature && !f.releasing;
}

// The object a refusal row names: the head's id, the member of Joined / Left, the object of Info and Ask; kNoObject for an entry
// that names none (Money, IdAsk, Carry, Done, Flushed) or one whose body does not read.
inline uint16_t refused_id(const Entry& e)
{
	using H = wire::LevelEntityHead;
	if (names_object(e.op))
	{
		ObjectHead h;
		return object_of(e, h) ? h.id : wire::kNoObject;
	}
	Reader r(e.body.data(), e.body.size());
	switch (e.op)
	{
	case H::Joined:
	case H::Left:
	{
		MemberBody b;
		return reads_whole(r, b) ? b.member : wire::kNoObject;
	}
	case H::Info:
	{
		InfoBody b;
		return reads_whole(r, b) ? b.id : wire::kNoObject;
	}
	case H::Ask:
	{
		AskBody b;
		return reads_whole(r, b) ? b.id : wire::kNoObject;
	}
	default: break;
	}
	return wire::kNoObject;
}

// The Ack(s) of batch `upto`: the refusals in rows of at most kMaxRefusals, every Ack with the same upto (an Ack never names a
// batch it does not acknowledge), the id block in the last one. One Ack with no rows when nothing was refused.
inline std::vector<wire::LevelJournalAckMsg> acks_of(uint32_t upto, const std::vector<wire::LevelJournalAckMsg::Refusal>& refusals,
    bool has_block, uint16_t first, uint16_t count)
{
	std::vector<wire::LevelJournalAckMsg> out;
	size_t at = 0;
	do
	{
		wire::LevelJournalAckMsg m;
		m.upto = upto;
		const size_t n = refusals.size() - at < wire::LevelJournalAckMsg::kMaxRefusals ? refusals.size() - at :
		                                                                                wire::LevelJournalAckMsg::kMaxRefusals;
		m.refusals.assign(refusals.begin() + ptrdiff_t(at), refusals.begin() + ptrdiff_t(at + n));
		at += n;
		out.push_back(std::move(m));
	} while (at < refusals.size());
	if (has_block && wire::LevelJournalAckMsg::block_ok(first, count))
	{
		out.back().has_block = true;
		out.back().first = first;
		out.back().count = count;
	}
	return out;
}

// What a map server does about an entry the world refused (W2b, W5): the non-writer repairs towards the writer.
enum class Repair : uint8_t
{
	None, // nothing to repair: the world's rule (a deal, a body report that crossed a move, an item word the item engine takes),
	      // an entry that names no object, an ask the map server did not apply first, a malformed entry (logged)
	ReleaseKeepId, // a create the world refused: released here, its id stays out of the slice's use (W9)
	Fetch, // a change or a removal the world refused: the world's copy replaces this one (StateDumpRequest Replace; the answer is
	       // an EntitySpawn, or an EntityDestroy when the world has no such object)
};

inline Repair repair_of(uint8_t op, uint8_t why)
{
	using H = wire::LevelEntityHead;
	switch (op)
	{
	case H::Spawn: return Repair::ReleaseKeepId; // the world has no such object from this map server: whatever the reason
	case H::Destroy:
	case H::Parent:
	case H::Died:
	case H::Item:
	case H::Tent:
	case H::Stash:
	case H::Logic:
	case H::Joined:
	case H::Left:
	case H::Info:
		// the copy here is wrong about who writes the object or what it is: the writer's word replaces it. The world's own rule
		// (Refused) and a malformed entry are no word about the copy.
		return why == NotWriter || why == Identity || why == PastFence || why == Unknown ? Repair::Fetch : Repair::None;
	default: break; // State (a body report: the seat decides), Money, Ask, IdAsk, Carry, Done, Flushed
	}
	return Repair::None;
}
// O6 (reports #1646/#1649: a Parent of an object the world does not have, so the item players see may vanish): a Parent + Unknown keeps
// its Fetch. Repair::None would lose no state of the world, but it is not shown to lose none at all: when the world released the object
// first its own EntityDestroy reaches the map server anyway (both rules end alike); when the world never took its Spawn, Fetch removes
// the item now, None keeps it on the map server alone until its hand-over or a player's move through the world loses it anyway; when
// the id is of the world's numbering and the world freed it, None leaves a copy under an id the world may give a new object (the
// duplicate W5 repairs). The refusal lines carry the section and the parent now (object_note); the rule waits for that data.

// O6 (reports #1644/#1645): the world's uses store refuses an ItemFacts (coop_initial_rules.h uses_store -> Refuse) in one of two ways,
// told apart by the client_data the world's entity has. None: a scripted section, whose bytes only the writer's object can make, and no
// usable bytes of the writer's came (none, other uses, or the world's entity has no holder). Some: bytes that are no held eatable's (an
// item that lies on the ground here keeps its physics state there). The item engine (coop_items.cpp) names both with one text, which
// starts with kUsesRefusedPrefix; the world's line says which.
constexpr const char* kUsesRefusedPrefix = "the uses cannot be stored";

enum class UsesRefusal : uint8_t
{
	NoBytes, // no client_data here: a scripted section and no usable bytes of the writer's object
	NotHeld, // client_data here that is no held eatable's: it lies on the ground in this world
};

inline bool is_uses_refusal(const char* why)
{
	return why && std::strncmp(why, kUsesRefusedPrefix, std::strlen(kUsesRefusedPrefix)) == 0;
}

inline UsesRefusal uses_refusal_of(bool client_data_here) { return client_data_here ? UsesRefusal::NotHeld : UsesRefusal::NoBytes; }

inline const char* uses_refusal_text(UsesRefusal r)
{
	return r == UsesRefusal::NoBytes ?
	    "the uses cannot be stored: a scripted section with no client_data here and no usable bytes of the writer's object" :
	    "the uses cannot be stored: the client_data here is no held eatable's (it lies on the ground in this world)";
}

// O6 (reports #1644-#1649): what a refusal line says of the object it names. `here`: the process has it (its section, its parent now;
// kNoObject: none); else the section crc its entry named (0: the entry names none). `move`: the entry moves it, to `to` (kNoObject: it
// is put down).
inline std::string object_note(bool here, const char* section, uint16_t crc, uint16_t parent, bool move, uint16_t to)
{
	char buf[192];
	int n = 0;
	if (here)
		n = parent == wire::kNoObject ? std::snprintf(buf, sizeof(buf), "here: section '%.64s', no parent", section ? section : "?") :
		                                std::snprintf(buf, sizeof(buf), "here: section '%.64s', parent %u", section ? section : "?",
		                                    unsigned(parent));
	else
		n = crc ? std::snprintf(buf, sizeof(buf), "not here, its entry names section crc 0x%04x", unsigned(crc)) :
		          std::snprintf(buf, sizeof(buf), "not here");
	const size_t len = n <= 0 ? 0 : size_t(n) < sizeof(buf) ? size_t(n) : sizeof(buf) - 1;
	std::string out(buf, len);
	if (move)
	{
		if (to == wire::kNoObject)
			out += ", put down";
		else
		{
			std::snprintf(buf, sizeof(buf), ", moved to %u", unsigned(to));
			out += buf;
		}
	}
	return out;
}

// ---------------------------------------------------------------- R32 A6: the head of a move, a death, a release (review WP4 #1, #2)
//
// The world checks an entry against the object as the world still sees it when it applies that entry (coop_levels_accept.h: the
// writer and the keeper generation of the root before the change), and it applies the entries of a map server in their order. The
// stock hooks fire after the change on the map server: a Parent after ID_Parent is set (xrServer_process_event_ownership.cpp,
// xrServer_process_event_reject.cpp: the old parent's children lack the item already), a Died after the death chain took the member
// out of its squad (game->on_death before events::host_on_death: se_stalker:on_death -> squad:on_npc_death -> remove_npc ->
// unregister_member), a script's remove_npc unregisters a member before it releases it. So the map server keeps what it needs to
// name the root before the change:
//   HeldMemory      where the children of its held creatures and of its players' characters, and the members of its held squads,
//                   stood at the end of the last frame, and every Parent hook since;
//   creature_place  where a Died or a Destroy of a creature goes among the frame's pending ups, and which squad its head names;
//   ParentNet       which Parent entries of one flush the world can take (the flush writes where an object stands now).

// The parent an object had before the hook that moves it: known (the snapshot or a move since says so; kNoObject: on the ground),
// or not known (no held creature and no player's character here held it at the end of the last frame and no hook moved it since: its
// old root is no unit this map server holds, the head names gen 0).
struct ParentBefore
{
	bool known = false;
	uint16_t parent = wire::kNoObject;
};

class HeldMemory
{
public:
	// A new snapshot: the end of a frame. What the hooks said before it is in it.
	void begin()
	{
		m_children.clear();
		m_members.clear();
		m_moved.clear();
		m_sorted = false;
	}
	void child(uint16_t child, uint16_t parent)
	{
		m_children.emplace_back(child, parent);
		m_sorted = false;
	}
	void member(uint16_t member, uint16_t squad)
	{
		m_members.emplace_back(member, squad);
		m_sorted = false;
	}

	// A Parent hook: where `item` stood before (its last move since the snapshot, else its holder in the snapshot); then the move.
	// A reject (new_parent kNoObject) never comes from the ground: a last move to the ground before it is stale (a holder that went
	// offline took the item back without a hook, add_offline_impl), so the move before that one, or the snapshot, says where it stood.
	ParentBefore moved(uint16_t item, uint16_t new_parent)
	{
		ParentBefore before;
		for (auto it = m_moved.rbegin(); it != m_moved.rend(); ++it)
			if (it->first == item && !(new_parent == wire::kNoObject && it->second == wire::kNoObject))
			{
				before.known = true;
				before.parent = it->second;
				break;
			}
		if (!before.known)
		{
			sort();
			const auto it = find(m_children, item);
			if (it != m_children.end())
			{
				before.known = true;
				before.parent = it->second;
			}
		}
		m_moved.emplace_back(item, new_parent);
		return before;
	}

	// The held squad `member` was a member of at the snapshot; kNoObject for none.
	uint16_t squad_before(uint16_t member)
	{
		sort();
		const auto it = find(m_members, member);
		return it == m_members.end() ? wire::kNoObject : it->second;
	}

	size_t children() const { return m_children.size(); }
	size_t members() const { return m_members.size(); }

private:
	using Pairs = std::vector<std::pair<uint16_t, uint16_t>>;
	static Pairs::const_iterator find(const Pairs& v, uint16_t key)
	{
		const auto it = std::lower_bound(v.begin(), v.end(), key,
		    [](const std::pair<uint16_t, uint16_t>& p, uint16_t k) { return p.first < k; });
		return it != v.end() && it->first == key ? it : v.end();
	}
	void sort()
	{
		if (m_sorted)
			return;
		const auto by_key = [](const std::pair<uint16_t, uint16_t>& a, const std::pair<uint16_t, uint16_t>& b) { return a.first < b.first; };
		std::stable_sort(m_children.begin(), m_children.end(), by_key); // a creature listed by two squads: the first one counts
		std::stable_sort(m_members.begin(), m_members.end(), by_key);
		m_sorted = true;
	}

	Pairs m_children; // child -> its holder, of the held creatures and of the players' characters here
	Pairs m_members; // member -> its squad, of the held squads
	Pairs m_moved; // item -> its new parent, every Parent hook since the snapshot, in order
	bool m_sorted = false;
};

// Where a Died or a Destroy of a creature goes among the pending ups of the frame (`ups`: op, a = the squad of a Joined / Left, b = its
// member), and the squad its head names. A creature in a squad now: the end, that squad. One whose last membership up of the frame is
// a Left of squad S (the hook of the stock unregister_member queued it before the death or release hook ran): just before that Left,
// naming S; the world then applies the death or the release while the creature is still in S, which this map server writes, and its
// own death chain or release takes the member out (a Left after it is a repeat). Otherwise the end, naming the squad of its last Joined
// of the frame, else the squad the snapshot knew it in (kNoObject: none, the head names the creature itself).
struct CreaturePlace
{
	size_t at = 0;
	uint16_t squad = wire::kNoObject;
	bool before_left = false; // `at` is the index of that Left
};

template <class Ups> // a vector of ups (any allocator: the engine's xr_vector)
CreaturePlace creature_place(const Ups& ups, uint16_t creature, uint16_t squad_now, uint16_t squad_at_snapshot)
{
	using H = wire::LevelEntityHead;
	CreaturePlace p;
	p.at = ups.size();
	if (squad_now != wire::kNoObject)
	{
		p.squad = squad_now;
		return p;
	}
	for (size_t i = ups.size(); i-- > 0;)
	{
		const auto& u = ups[i];
		if ((u.op != H::Joined && u.op != H::Left) || u.b != creature)
			continue;
		p.squad = u.a;
		if (u.op == H::Left)
		{
			p.at = i;
			p.before_left = true;
		}
		return p;
	}
	p.squad = squad_at_snapshot;
	return p;
}

// The Parent entries of one flush. The flush writes where an object stands now (a holder that goes offline rejects its children and
// takes them back without a word, add_offline_impl; the host's own flush nets the same way), so the world's copy goes straight from
// where it stood before the object's first move of the flush to where it stands now:
//   the first Parent of an object in the flush is sent, its head naming the root before that first move (the hook's head);
//   a later Parent of it, or one after its Spawn in the same flush (written where it stands now), is left out: the world has it
//     there already, and its head would name a root the world no longer has;
//   an object back where it stood before its first move (known) changes nothing: left out;
//   an object whose Spawn was left out (made and gone within the frame) is never heard of by the world: its later entries are left
//     out too.
// Server, the departure flood (2026-11-26, world log xray_banan4ik2.log: 23 of 23 printed refusals "parent X, moved to X"): a player
// who leaves the level (quits, drops, takes an exit) has his character switched offline here, every item rejected with a hook and
// taken back without one. The snapshot knows where those items stood only when it lists his character (client_remember_held); then
// each is back where it stood and left out here, and unchanged() counts it.
class ParentNet
{
public:
	void spawned(uint16_t id) { m_placed.insert(id); }
	void unborn(uint16_t id) { m_unborn.insert(id); }
	bool unborn_here(uint16_t id) const { return m_unborn.count(id) != 0; }

	bool send(uint16_t id, const ParentBefore& before, uint16_t parent_now)
	{
		if (m_unborn.count(id) || !m_placed.insert(id).second)
			return false;
		if (before.known && before.parent == parent_now)
		{
			++m_unchanged;
			return false;
		}
		return true;
	}

	// Objects of this flush whose first Parent was left out because they stand where they stood before it.
	uint32_t unchanged() const { return m_unchanged; }

private:
	std::set<uint16_t> m_placed;
	std::set<uint16_t> m_unborn;
	uint32_t m_unchanged = 0;
};

// ---------------------------------------------------------------- R32 W7.2 (§8a A21 E-1): the done-records of numbered ops
//
// A numbered op that consumes or pays (an exchange, a heal receipt, a give, a story share: coop_ops_done.script) runs once, also when
// its level moves to another map server. The map server that ran it records {op -> answer} with the effect: a Done entry (DoneBody)
// queued after the facts of the effect, so the same journal batch carries both (srv.journal_done). The world keeps the records of each
// level here and raises the Lua event coop_op_done(origin, seq, answer) for each new one (its ledger saves them in m_data.coop_ops_done).
// The store is a chunk of the world's ALife save, so the save a starting map server of that level is served (its snapshot) carries the
// level's records, and that map server raises coop_op_done for each before it serves its players. The first answer of an op stands. A
// level keeps kMaxPerLevel records and kMaxBytesPerLevel bytes of answers at most, the oldest going first: an op retried after its
// record went is the ledger's business (it keeps the highest retired number of each character, coop_ops_done.get).
namespace done
{
constexpr uint32_t kMaxOrigin = 0xfffe; // a character's world id (coop_ops_done.script key: 0 .. 65534)
constexpr size_t kMaxAnswer = 4096; // script::kMaxCallbackStr: the engine hands the answer to Lua as one callback argument
constexpr size_t kMaxPerLevel = 256; // records of one level
constexpr size_t kMaxBytesPerLevel = 128 * 1024; // ... and the bytes of their answers
constexpr size_t kMaxLevels = 64; // levels with records (the game has about 40)
constexpr uint32_t kVersion = 1; // of the snapshot's layout
// The snapshot (write / read): u32 version, u16 levels, per level {u16 level, u16 n, n x {u32 origin, u32 seq, str answer}}, levels
// in ascending order, records in the order the world kept them. Its size at most:
constexpr size_t kMaxSnapshotBytes = 4 + 2 + kMaxLevels * (2 + 2 + kMaxPerLevel * (4 + 4 + 3) + kMaxBytesPerLevel);

// An op's number and an answer the store takes: a character's id, a sequence from 1, an answer of at most kMaxAnswer bytes.
inline bool valid(uint32_t origin, uint32_t seq, size_t answer_size)
{
	return origin <= kMaxOrigin && seq != 0 && answer_size <= kMaxAnswer;
}

struct Record
{
	uint32_t origin = 0;
	uint32_t seq = 0;
	std::string answer;
};

enum class Put : uint8_t
{
	Kept, // a new record: the world raises coop_op_done for it
	Repeat, // the op has its record already: its first answer stands, nothing changes
	Refused, // no record: a number or an answer valid() refuses, no level (kNoLevel), or a new level past kMaxLevels
};

class Store
{
public:
	Put put(uint16_t level, uint32_t origin, uint32_t seq, std::string answer)
	{
		if (level == wire::kNoLevel || !valid(origin, seq, answer.size()))
			return Put::Refused;
		auto it = m_levels.find(level);
		if (it == m_levels.end())
		{
			if (m_levels.size() >= kMaxLevels)
				return Put::Refused;
			it = m_levels.emplace(level, Level()).first;
		}
		Level& l = it->second;
		if (!l.keys.insert(key(origin, seq)).second)
			return Put::Repeat;
		l.bytes += answer.size();
		l.rows.push_back(Record{origin, seq, std::move(answer)});
		while (l.rows.size() > kMaxPerLevel || l.bytes > kMaxBytesPerLevel)
		{
			const Record& old = l.rows.front();
			l.bytes -= old.answer.size();
			l.keys.erase(key(old.origin, old.seq));
			l.rows.pop_front();
			++m_evicted;
		}
		return Put::Kept;
	}

	// The record of op {origin, seq} on `level`; nullptr for none.
	const Record* find(uint16_t level, uint32_t origin, uint32_t seq) const
	{
		const auto it = m_levels.find(level);
		if (it == m_levels.end())
			return nullptr;
		for (const Record& r : it->second.rows)
			if (r.origin == origin && r.seq == seq)
				return &r;
		return nullptr;
	}

	// The records of `level`, in the order the world kept them.
	std::vector<Record> of(uint16_t level) const
	{
		const auto it = m_levels.find(level);
		return it == m_levels.end() ? std::vector<Record>() : std::vector<Record>(it->second.rows.begin(), it->second.rows.end());
	}

	size_t count(uint16_t level) const
	{
		const auto it = m_levels.find(level);
		return it == m_levels.end() ? 0 : it->second.rows.size();
	}
	size_t bytes(uint16_t level) const
	{
		const auto it = m_levels.find(level);
		return it == m_levels.end() ? 0 : it->second.bytes;
	}
	size_t levels() const { return m_levels.size(); }
	bool empty() const { return m_levels.empty(); }
	uint32_t evicted() const { return m_evicted; } // records that went for the bounds
	void clear() { m_levels.clear(); }

	void write(Writer& w) const
	{
		w.u32(kVersion);
		w.u16(uint16_t(m_levels.size()));
		for (const auto& it : m_levels)
		{
			w.u16(it.first);
			w.u16(uint16_t(it.second.rows.size()));
			for (const Record& r : it.second.rows)
			{
				w.u32(r.origin);
				w.u32(r.seq);
				w.str(r.answer);
			}
		}
	}

	// The whole store from a snapshot, replacing what is here. False, nothing changed, for a snapshot that does not read whole: another
	// version, a level twice or with no record, a record valid() refuses or met twice, more than the bounds keep, bytes after its end.
	bool read(Reader& r)
	{
		Store tmp;
		if (r.u32() != kVersion)
			return false;
		const uint16_t n_levels = r.u16();
		if (!r.ok() || n_levels > kMaxLevels)
			return false;
		for (uint16_t i = 0; i < n_levels; ++i)
		{
			const uint16_t level = r.u16();
			const uint16_t n = r.u16();
			if (!r.ok() || level == wire::kNoLevel || n == 0 || n > kMaxPerLevel || tmp.m_levels.count(level))
				return false;
			for (uint16_t k = 0; k < n; ++k)
			{
				const uint32_t origin = r.u32();
				const uint32_t seq = r.u32();
				std::string answer = r.str();
				if (!r.ok() || tmp.put(level, origin, seq, std::move(answer)) != Put::Kept)
					return false;
			}
		}
		if (!r.ok() || r.remaining() != 0 || tmp.m_evicted != 0)
			return false;
		m_levels = std::move(tmp.m_levels);
		return true;
	}

private:
	static uint64_t key(uint32_t origin, uint32_t seq) { return (uint64_t(origin) << 32) | seq; }

	struct Level
	{
		std::deque<Record> rows;
		std::set<uint64_t> keys;
		size_t bytes = 0;
	};
	std::map<uint16_t, Level> m_levels;
	uint32_t m_evicted = 0;
};

// Who looks for the done-records in a save being loaded: the world server, which takes them as its store, and a map server, which
// raises those of its level. A co-op process and a player's machine never do (the co-op has no journal; FE1).
inline bool loads_records(bool world_server, bool map_server) { return world_server || map_server; }

// The done-records of the save just loaded (its done chunk), until the frame that takes them, taken once (FE1: the engine's xfer_load,
// host_adopt_done and client_replay_done are thin glue around this). load: what the ALife load found. adopt: the world server's next
// frame makes them its store. replay: a map server's first frame of its world gets those of its level.
class Loaded
{
public:
	enum class Took : uint8_t
	{
		Nothing, // no load pending: a process that does not load records, a load taken already
		Taken, // the save's records (none: a save without the chunk)
		Unreadable, // the chunk is there and is no snapshot (cut, too big, another version): nothing taken from it
	};

	// `reads`: loads_records() of this process; false: nothing pending, whatever the save holds. `chunk`, `size`: the chunk's bytes
	// (size 0: the save has none; a null chunk with a size: the file is cut short inside it). A load not taken yet is replaced.
	void load(bool reads, const uint8_t* chunk, size_t size)
	{
		m_bytes.clear();
		m_pending = reads;
		m_bad = false;
		m_size = reads ? size : 0;
		if (!reads || size == 0)
			return;
		if (!chunk || size > kMaxSnapshotBytes)
		{
			m_bad = true;
			return;
		}
		m_bytes.assign(chunk, chunk + size);
	}

	bool pending() const { return m_pending; }
	size_t bytes() const { return m_size; } // the chunk's size of the load pending (also one that does not read), 0 for none

	// The world server: `live` becomes the save's store, what it held before going (a load is a world of its own; a save without the
	// chunk gives an empty store, one that does not read leaves it empty).
	Took adopt(Store& live)
	{
		if (!m_pending)
			return Took::Nothing;
		live.clear();
		return take(live); // Store::read takes a snapshot whole or not at all: one that does not read leaves `live` empty
	}

	// A map server: the records of `level` in the save, in the order the world kept them (`out` empty for none and for Nothing).
	Took replay(uint16_t level, std::vector<Record>& out)
	{
		out.clear();
		if (!m_pending)
			return Took::Nothing;
		Store loaded;
		const Took took = take(loaded);
		if (took == Took::Taken)
			out = loaded.of(level);
		return took;
	}

private:
	// The load pending, no more pending, read into `into` (an empty store) when it is a snapshot.
	Took take(Store& into)
	{
		m_pending = false;
		m_size = 0;
		const bool bad = m_bad;
		m_bad = false;
		std::vector<uint8_t> bytes;
		bytes.swap(m_bytes);
		if (bad)
			return Took::Unreadable;
		if (bytes.empty())
			return Took::Taken;
		Reader r(bytes.data(), bytes.size());
		return into.read(r) ? Took::Taken : Took::Unreadable;
	}

	std::vector<uint8_t> m_bytes;
	size_t m_size = 0;
	bool m_pending = false;
	bool m_bad = false;
};

// §8a A22 open item (a), O6: a starting map server says Ready only once the records its replay raised reached its Lua, so a player that
// Ready lets in retries an op only after the ledger has its record. The replay queues them in levels::frame of frame K
// (client_replay_done), after that frame's script delivery (coop_sync.cpp: script::frame, then mode().frame()); the engine's queue goes to
// Lua whole in the script delivery of the next frame whose Lua is up (coop_script.cpp frame: lua_ready(), the inbox, then the engine's
// events, no budget). So a frame other than K that the map server's step sees with Lua up comes after that delivery. `lua_up` reads what
// lua_ready() reads (the level, the actor, a live session), which no step between the script delivery and the map server's step changes.
// Not covered: a peer that was connected before Ready and whose retry sits in the inbox of that same script delivery (the inbox goes
// first there: coop_script.cpp, the Core's order).
class ReplayGate
{
public:
	void queued(uint32_t frame)
	{
		m_pending = true;
		m_frame = frame;
	}
	void frame(uint32_t frame, bool lua_up)
	{
		if (m_pending && lua_up && frame != m_frame)
			m_pending = false;
	}
	bool settled() const { return !m_pending; }
	void reset()
	{
		m_pending = false;
		m_frame = 0;
	}

private:
	bool m_pending = false;
	uint32_t m_frame = 0;
};
} // namespace done
} // namespace journal
} // namespace levels
} // namespace coop
