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
#pragma once

#include "coop_levels_wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
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

// ---------------------------------------------------------------- R32 A6: the head of a move, a death, a release (review WP4 #1, #2)
//
// The world checks an entry against the object as the world still sees it when it applies that entry (coop_levels_accept.h: the
// writer and the keeper generation of the root before the change), and it applies the entries of a map server in their order. The
// stock hooks fire after the change on the map server: a Parent after ID_Parent is set (xrServer_process_event_ownership.cpp,
// xrServer_process_event_reject.cpp: the old parent's children lack the item already), a Died after the death chain took the member
// out of its squad (game->on_death before events::host_on_death: se_stalker:on_death -> squad:on_npc_death -> remove_npc ->
// unregister_member), a script's remove_npc unregisters a member before it releases it. So the map server keeps what it needs to
// name the root before the change:
//   HeldMemory      where the children of its held creatures and the members of its held squads stood at the end of the last frame,
//                   and every Parent hook since;
//   creature_place  where a Died or a Destroy of a creature goes among the frame's pending ups, and which squad its head names;
//   ParentNet       which Parent entries of one flush the world can take (the flush writes where an object stands now).

// The parent an object had before the hook that moves it: known (the snapshot or a move since says so; kNoObject: on the ground),
// or not known (no held creature held it at the end of the last frame and no hook moved it since: its old root is no unit this map
// server holds, the head names gen 0).
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

	Pairs m_children; // child -> its holder, of the held creatures
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
		return !before.known || before.parent != parent_now;
	}

private:
	std::set<uint16_t> m_placed;
	std::set<uint16_t> m_unborn;
};
} // namespace journal
} // namespace levels
} // namespace coop
