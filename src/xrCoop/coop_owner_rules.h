// Anomaly Coop — who wins a field of a player's item, the decider (its writer) or the owner (the player).
// The decider of a player's item keeps a baseline of it (coop_items.cpp Owned.data) and puts it back on its copy every 500 ms
// (host_enforce): engine drift on that copy (an avatar firing for show) never becomes the item's state. A script's deliberate
// setter on the decider (set_condition, set_remaining_uses) is not drift: it is the new baseline, its field versions move on
// (coop_item_tags.h bump) and the owner gets it in the same frame. An owner's report then counts field by field: a field made on
// the decider's latest version is his (his own eating, his gun's wear); a field made on an older one is stale and refused, and
// the decider pushes its own value of it again so that the owner catches up (a report never restores a debit). The uses are a
// counter and travel as changes (CounterHistory below): a stale report of them is rebased, so a dose the owner eats inside the
// round trip of a decider's debit stays spent too.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstdint>

#include "coop_item_tags.h"

namespace coop
{
namespace owner
{
// The script setters that write a field of an item (script_game_object.cpp, script_game_object3.cpp).
enum class Setter : std::uint8_t
{
	Condition, // set_condition: the condition (also a device's charge, an artefact's)
	Uses, // set_remaining_uses: the portions of an eatable
};

inline std::uint32_t tags_of(Setter s)
{
	return s == Setter::Condition ? std::uint32_t(itemtags::Cond) : std::uint32_t(itemtags::Uses);
}

// A setter call is the decider's deliberate write when this process writes the item (`decides_item`), the item is a remote
// player's (`owned_by_player`: the decider enforces his reports on it) and it is not the apply of that player's own state
// (`applying_owner_state`: the engine's apply of a report, or a callback run inside it).
inline bool deliberate(bool decides_item, bool owned_by_player, bool applying_owner_state)
{
	return decides_item && owned_by_player && !applying_owner_state;
}

// The decider's new baseline after its deliberate write of the tags in `mask`: those tags take `now` (its copy right after the
// setter), every other tag keeps the baseline (the owner's word). The upgrade list is never taken here (coop_ledger.h).
template <class D>
D adopt(D baseline, const D& now, std::uint32_t mask)
{
	return itemtags::take_push(baseline, now, mask & itemtags::kOwnerTakeTags);
}

// Decider: the tags of an owner's report it takes. `carried`: the tags the report has. A tag is taken when the report was made
// on the decider's latest version of it (coop_item_tags.h fresh); fields outside the versioned tags (launcher mode, fire mode,
// cartridges) are not filtered here.
inline std::uint32_t report_takes(
    const itemtags::FieldVersions& latest, const itemtags::FieldVersions& report, std::uint32_t carried)
{
	return itemtags::fresh(latest, report, carried & itemtags::kAll);
}

// Decider: the tags of an owner's report it refuses (stale): it pushes its own value of them back to the owner.
inline std::uint32_t report_refuses(
    const itemtags::FieldVersions& latest, const itemtags::FieldVersions& report, std::uint32_t carried)
{
	return (carried & itemtags::kAll) & ~report_takes(latest, report, carried);
}

// Owner: the tags of a decider's push (`carried` tags, versions `push`) he applies: those whose version he has not applied yet,
// whatever else he changed meanwhile. A repeated or late push of a version he has already applied changes nothing (his own
// change on top of it stays).
inline std::uint32_t push_applies(
    const itemtags::FieldVersions& applied, const itemtags::FieldVersions& push, std::uint32_t carried)
{
	std::uint32_t out = 0;
	for (std::size_t i = 0; i < itemtags::kTagCount; ++i)
	{
		const std::uint32_t bit = 1u << i;
		if ((carried & bit) && applied.of[i] != push.of[i])
			out |= bit;
	}
	return out;
}

// The versions a state carries on the wire: only the tags whose version is not 0 (a tag that is missing reads as 0,
// coop_item_tags.h read_versions), so an item nobody wrote on purpose costs nothing.
inline std::uint32_t versions_mask(const itemtags::FieldVersions& v)
{
	std::uint32_t m = 0;
	for (std::size_t i = 0; i < itemtags::kTagCount; ++i)
		if (v.of[i] != 0)
			m |= 1u << i;
	return m;
}

// the decider's latest versions travel with the item (its spawn's initial state, a map server's ItemFacts). The
// process the item arrives at (a new decider after a level move or a restart; the owner whose copy is built from that spawn)
// takes them whole: the travelling state was made on exactly those versions.
inline void arrive(itemtags::FieldVersions& here, const itemtags::FieldVersions& carried, std::uint32_t mask)
{
	here = itemtags::FieldVersions();
	itemtags::take(here, carried, mask & itemtags::kAll);
}

// ---------------------------------------------------------------- counters (the portions of an eatable)
// The uses are a counter: the decider takes doses off (a combat heal), the owner eats his own. Compared as absolute values by
// version, a dose the owner ate inside the round trip of a decider's debit was lost: his report (made on the old version) was
// refused and the push then overwrote his value with the decider's. So the uses travel as changes against the version they were
// made on:
// - the decider keeps the change of each of its recent deliberate writes of the uses (CounterHistory, by version);
// - an owner's stale report of the uses is rebased, not refused: his value plus every decider change after the version he made it
//   on (his own doses stay spent, the debit stays);
// - the owner applies a push of the version right after the one he has applied as its change onto his own value (his doses since
//   stay); any other push (a heal after a decider change, a decider with no history) as its value.
// The condition stays absolute (a decider's set is a set: an FDDA extraction to c_zero, a repair).
constexpr std::size_t kCounterHistory = 8;

struct CounterHistory
{
	std::uint16_t version[kCounterHistory] = {};
	std::int16_t delta[kCounterHistory] = {};
	std::uint8_t used = 0; // entries in use
	std::uint8_t next = 0; // the entry the next record takes (the oldest once all are used)

	// The decider's deliberate write that made version `v` changed the uses by `d`.
	void record(std::uint16_t v, int d)
	{
		const std::int16_t c = std::int16_t(d < -32768 ? -32768 : (d > 32767 ? 32767 : d));
		for (std::size_t i = 0; i < used; ++i)
			if (version[i] == v)
			{
				delta[i] = c; // the version came round again (it wraps after 65535)
				return;
			}
		version[next] = v;
		delta[next] = c;
		next = std::uint8_t((next + 1) % kCounterHistory);
		if (used < kCounterHistory)
			++used;
	}
	// The change that made version `v`; false for version 0 (no write) and for a version this history does not hold.
	bool delta_of(std::uint16_t v, int& d) const
	{
		if (v == 0)
			return false;
		for (std::size_t i = 0; i < used; ++i)
			if (version[i] == v)
			{
				d = delta[i];
				return true;
			}
		return false;
	}
};

inline int clamp_counter(int value, int max_value)
{
	return value < 0 ? 0 : (value > max_value ? max_value : value);
}

// Decider: the owner's value `reported`, made on version `made_on`, rebased onto its latest version `latest`: plus the change of every
// version after `made_on` up to `latest`, clamped to 0 .. max_value. False: a change of that chain is not in the history (older than
// kCounterHistory writes, another decider's numbering): the field is refused as stale then.
inline bool rebase_counter(
    const CounterHistory& h, std::uint16_t made_on, std::uint16_t latest, int reported, int max_value, int& out)
{
	int sum = 0;
	std::uint16_t v = made_on;
	for (std::size_t steps = 0; v != latest; ++steps)
	{
		if (steps >= kCounterHistory)
			return false;
		v = itemtags::next_version(v);
		int d = 0;
		if (!h.delta_of(v, d))
			return false;
		sum += d;
	}
	out = clamp_counter(reported + sum, max_value);
	return true;
}

// Decider: what it does with an owner's report (carrying the tags `carried`, made on the versions `report`). `refused`: the stale tags,
// not taken (its own value of them stays; it pushes them back while the owner still holds the item). `rebased`: the uses were stale
// and are taken as `uses` (rebase_counter), so they are not refused. The same judgement for a report of an item the owner holds and
// of one that has just left his hands (the version check of a late report keeps a debit in place).
struct ReportVerdict
{
	std::uint32_t refused = 0;
	bool rebased = false;
	int uses = 0;
};

inline ReportVerdict judge_report(const itemtags::FieldVersions& latest, const itemtags::FieldVersions& report, std::uint32_t carried,
    const CounterHistory& uses_history, int reported_uses, int max_uses)
{
	ReportVerdict v;
	v.refused = report_refuses(latest, report, carried);
	int out = 0;
	if ((v.refused & itemtags::Uses) &&
	    rebase_counter(uses_history, report.get(itemtags::Uses), latest.get(itemtags::Uses), reported_uses, max_uses, out))
	{
		v.refused &= ~std::uint32_t(itemtags::Uses);
		v.rebased = true;
		v.uses = out;
	}
	return v;
}

// Owner: his value of the uses after a decider's push whose uses he applies (push_applies took them). The push's change onto his
// own value `mine` when it is the version right after the one he has applied (`applied`) and carries its change; else the push's
// value (a heal after a decider change, a push with no change known).
inline int owner_takes_uses(std::uint16_t applied, std::uint16_t pushed, bool has_delta, int delta, int push_value, bool has_mine,
    int mine, int max_uses)
{
	if (has_delta && has_mine && pushed != 0 && itemtags::next_version(applied) == pushed)
		return clamp_counter(mine + delta, max_uses);
	return push_value;
}

// Who holds an item at the end of its chain, as one view of this machine sees it: its game object's chain or its server entity's.
enum class Holder : std::uint8_t
{
	Nobody, // the chain is the item alone (on the ground; or a game object its parent has not taken yet)
	Me, // this machine's own character
	Living, // a living creature: an NPC, a companion, another player's character
	Other, // a corpse, a box, a stash
};

// A player's report of a world item he changed (coop_items.cpp send_foreign: a corpse's gun he unloaded). What his character holds
// goes with his own scan; a living creature's things are its decider's (they change with every shot, the decider refuses them).
// Both views are asked: a game object spawned a moment ago has no parent yet (its holder takes it through an event of a later
// frame), while its server entity already names the holder.
inline bool reports_foreign(Holder by_object, Holder by_entity)
{
	const auto keeps = [](Holder h) { return h == Holder::Me || h == Holder::Living; };
	return !keeps(by_object) && !keeps(by_entity);
}
} // namespace owner
} // namespace coop
