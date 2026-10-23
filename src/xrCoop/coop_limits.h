// Anomaly Coop — what a host takes from a client: message rates per type (F2 T3) and new player characters per
// session (Ruling F2-10 b). The game installs the rates (coop_join install) and keeps the character count (coop_join).
// F2 T14: a client paces itself by the same rates (Pacer; coop_script.cpp script events, coop_sync_api.cpp).
// F2 T3b: the story, shot, item, trade, object and request messages got rows too (Ruling F2-15); still without one: the
// join lifecycle, the player stream and life states, TimeRequest and the dialog choice (task-3b-report.md).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "coop_protocol.h"

namespace coop
{
namespace limits
{
// Host, per client: `type` passes `burst` times in a row, then once more every `refill_ms` (Session::limit_rate, a
// token bucket per client). What exceeds it is dropped before its handler and counted (stats(type).rate_dropped,
// coop_status); the sender is not kicked: a burst may be honest. Every value leaves an honest client room.
struct RateRule
{
	Msg type;
	uint32_t burst;
	uint32_t refill_ms;
};

constexpr RateRule kClientRateRules[] = {
	// R2 I4: a client asks for an entity it misses (P3); a modified one could ask for the whole world in a loop, each
	// answer a full spawn packet. 20 per second after a burst.
	{Msg::StateDumpRequest, 64, 50},
	// T12 review (Ruling 18): an honest client votes when it touches a level exit (once, or each time the player says
	// yes to the stock dialog, which comes back every 5 s while he stays inside). One per 2 s after a burst.
	{Msg::TransitionVote, 4, 2000},
	// F2 T3: sent on a change and every 5 s, checked every 500 ms (coop_equip.cpp kEquipCheckMs, kEquipRefreshMs).
	{Msg::PlayerEquip, 8, 400},
	// F2 T3: every 2 s (coop_equip.cpp kVitalsMs).
	{Msg::PlayerVitals, 4, 1000},
	// F2 T3: map spots a player's game adds or removes (coop_story); a quest step adds a few at once.
	{Msg::MapSpot, 32, 250},
	// F2 T3: info portions a player's game gives (coop_story); a dialog or quest step gives a batch.
	{Msg::InfoPortion, 64, 50},
	// F2 T3: a full story resend for the client: after a hash mismatch (StoryHash every 10 s), at join, console.
	{Msg::StoryResync, 4, 5000},
	// F2 T3: Lua RPC of mods (coop_script), 50 per second after a burst.
	{Msg::ScriptEvent, 64, 20},
	// F2 T3: one per money change of the player's own game (coop_money queue_change), 10 per second after a burst.
	{Msg::ReqMoney, 32, 100},
	// F2 T1 review: a talk lock is asked once per talk window opened (a request waits for its answer up to 10 s) and
	// refreshed every 30 s while the talk lasts (coop_dialog.cpp kPendingTimeoutMs, kKeepAliveMs). 10 covers a player
	// clicking an NPC that is busy, then one per second.
	{Msg::DialogLock, 10, 1000},
	// Protocol 8: a player with a new key sends his choice of character when he presses Done (the window waits for the
	// answer); a refused choice is sent again after a change. Each one runs the host's Lua check and, when accepted, a
	// world save. One per 2 s after a burst; a choice nobody asked for is ignored anyway (coop_join, newchar::Choosers).
	{Msg::CharChoice, 4, 2000},
	// F3: one per tent the player places (the stock use of a sleeping bag, an animation, then picking the tent up again
	// before the next one); the host keeps each claim open 15 s (tent_claim_ms) to find the tent. One per 2 s after a
	// burst.
	{Msg::CheckpointClaim, 4, 2000},
	// F3: the player lies down at his rest spot or gets up (the doze window, any time); the plan's host limit is one
	// per second. 4 covers a quick lie down / get up / lie down.
	{Msg::RestRequest, 4, 1000},
	// F3: hardcore, one per death: the dead player holds USE 3 s (or coop_new_character) and the host stashes all his
	// gear and sends him the world again. A double press, then one per 5 s.
	{Msg::NewCharacter, 2, 5000},
	// F2 T14: coop.sync changes of a player's own data and of his items (a mod writes them on its events: a shot, an
	// item used, a perk; Lua protocols over coop.send_event move here), as many as script events.
	{Msg::SyncSet, 64, 20},
	// F2 T14: coop.sync.request runs a handler of the host's Lua; per handler name the host limits it again (20 in a row,
	// 10 per second by default, coop.sync.limit). 20 per second in all after a burst.
	{Msg::SyncRequest, 32, 50},

	// F2 T3b (Ruling F2-15): the rest of what a client sends. An honest sender is never dropped: a row is above the honest
	// cadence of the sender's code, or, where the code does not bound it, at 4 x the best estimate (marked "estimate"
	// here and in task-3b-report.md). The story outbox goes by Pacer (T14), so its rows only need to take what the
	// pacer lets out: a backlog waits in the outbox (coop_story.cpp flush_outbox).
	// A quest step gives a few tasks at once and completes some (estimate: 8, about one a second), 4 x that.
	{Msg::TaskUpdate, 32, 250},
	// A fight: every kill changes a few relations (estimate: 3 a second, a grenade killing five at once: 15), 4 x that.
	{Msg::Relation, 64, 80},
	// progress_frame sends at most once per 1000 ms, and only when rank or reputation changed (coop_story.cpp:47, :1833).
	// 2 a second after a burst of 4 (a lag spike of 3 s delivers three together).
	{Msg::Progress, 4, 500},
	// One PlayerShot per frame and weapon, shots of one frame merged (coop_presence.cpp:583), so the weapon's rate of fire
	// bounds it (estimate: 1200 rpm = 20 a second), plus one per reload. 4 x that, 32 in a row (1.6 s of full fire).
	{Msg::PlayerShot, 32, 12},
	// Entering the world sends the state of every item of the inventory in one frame, unpaced (coop_items.cpp:1725-1745,
	// scan_local(true)), and a drop here is a silent loss of the inventory's state. Ruling F2-33: at least 4096 in a row.
	// The weight does not bound a GAMMA inventory: the base max_weight is 35 kg, one carried item can add up to 124.8 kg
	// (additional_inventory_weight), and GAMMA has 41 item lines of 0.01 kg and 38 of weight 0 (inv_weight in
	// E:\GAMMA\mods, checked 2026-10-23). No real maximum is known (estimate: a hoarder 400-1000 items), so 4096 is a
	// reserve, not a measure.
	// The rate stays that of the honest stream after the join: the changed items per scan, every 100 ms after a change
	// (kModifyScanMs, :50), a weapon and its ammo box in a reload, estimate 20 a second, 100 a second here. A Pacer or
	// batches in scan_local (T20) would let the burst come down.
	{Msg::ItemState, 4096, 10},
	// One deal per press of the Sell, Buy or donate control (coop_trade.cpp:645): a human, estimate 8 presses in the first
	// second then 5 a second, 4 x that.
	{Msg::TradeOp, 32, 50},
	// Pushes of bodies in contact: one per body per 100 ms (kPushIntervalMs, coop_objects.cpp:68, :897), estimate 3 bodies =
	// 30 a second and a pile of 8 in a check, 4 x the rate. Uses, artefact activations and throws are single presses.
	{Msg::EntityEvent, 64, 8},

	// Requests. The host has its own buckets for them (coop_requests.cpp kRate / kBurst per client, :772-773 at engine
	// 4df7bd6: spawns 20 a second after 40, moves and destroys 30 after 60, hits 80 after 160, the rest 20 after 40;
	// launches one per 100 ms, :1863) and answers a request over one with a ReqResult RateLimited: the client logs it and
	// shows the real content of the container again (:699). A request dropped here gets no answer at all: the client
	// only waits out kResultTimeoutMs (:74). So these rows are an outer guard at twice the host's bucket, a flood stopper
	// before the handler, and an honest client runs into the host's answer first. Take, Drop and Transfer share the
	// host's moves bucket (Destroy and Use the destroys one), but each type has its own bucket here.
	{Msg::ReqSpawn, 80, 25},
	{Msg::ReqDestroy, 120, 16},
	{Msg::ReqUse, 120, 16},
	{Msg::ReqTake, 120, 16},
	{Msg::ReqDrop, 120, 16},
	{Msg::ReqTransfer, 120, 16},
	{Msg::ReqHit, 320, 6},
	{Msg::ReqItemEvent, 80, 25},
	// A launch needs a rocket the launcher holds: at most the host's 10 a second, a six-shot launcher in a row.
	{Msg::ReqLaunch, 16, 50},
	// One on showing and one on hiding a trade or loot window (coop_trade.cpp:772): a human, estimate 4 a second, 8 in a
	// row, 4 x that. Each opening runs a callback of the host's Lua.
	{Msg::ReqContainer, 32, 60},
};

// Ruling F2-10 b: a client picks its own player key, and a key the host's world has no character for gets a new one:
// new keys farmed the starting kit and money, and every such character stays in the save for good. A session makes
// at most the host setting coop_max_new_characters of them (g_coop_max_new_characters, coop_join; F2 T3 fix round 1:
// a host whose session ran out raises it without a restart). A player whose key has a character in the world costs
// nothing. Ruling F3-9: the limit is for keys that never had a character; a key that had one in this session, or whose
// character retired in this world (hardcore, F3 Task 10: the same player starting again), neither checks nor spends it
// (coop_join, death::new_key_charge).
constexpr uint32_t kNewCharactersDefault = 8;
constexpr uint32_t kNewCharactersMin = 1;
constexpr uint32_t kNewCharactersMax = 64;
// The refusal of a new key over the limit: the nearest reason of the protocol (no room in this session for one more
// new character), and why.
constexpr RejectReason kNoNewCharacterReason = RejectReason::SessionFull;
inline std::string no_new_character_text(uint32_t max)
{
	return "this session has made all " + std::to_string(max) +
		" new characters the host allows (coop_max_new_characters); players who have a character in the host's world "
		"can still join";
}

// Host: the player keys (digests) a session made a new character for. A key that got one already costs nothing
// again: a load of a save from before he came takes his character away, and he gets a new one. Ruling F3-9: keys that
// had a character before are not added here (coop_join, death::new_key_charge).
class NewCharacters
{
public:
	// A player asks for the world, which has no character for his key: true if he may get one while the session
	// allows `max` new characters (the setting now: lowering it refuses new keys only).
	bool admits(const std::string& key, uint32_t max) const { return m_keys.count(key) != 0 || m_keys.size() < max; }
	// The host made a character for `key`.
	void add(const std::string& key) { m_keys.insert(key); }
	size_t count() const { return m_keys.size(); }

private:
	std::set<std::string> m_keys; // at most kNewCharactersMax (admits)
};

// F2 T14 (Ruling F2-14): the client's side of kClientRateRules. The host drops what exceeds a rule and the sender never
// learns of it (a dropped InfoPortion or script event is lost for good), so a client holds such a message back in its
// queue and sends it later instead. The host measures when a message arrives, and the network can bunch up messages
// that left evenly (a lag spike delivers them together): the client goes slower than the rule, half its burst in a row,
// then one per 5/4 of its refill.
inline const RateRule* client_rule(Msg type)
{
	for (const RateRule& r : kClientRateRules)
		if (r.type == type)
			return &r;
	return nullptr;
}

struct Pace
{
	uint32_t burst;
	uint32_t interval_ms;
};
inline Pace client_pace(const RateRule& r)
{
	return {r.burst / 2 ? r.burst / 2 : 1, r.refill_ms + (r.refill_ms / 4 ? r.refill_ms / 4 : 1)};
}

// A token bucket per message type, as the host's (Session::limit_rate) at client_pace. One per client machine.
class Pacer
{
public:
	// True if one more `type` may go now (it is counted); false: hold it and ask again later. A type without a rule
	// always goes.
	bool take(Msg type, uint64_t now_ms)
	{
		const RateRule* rule = client_rule(type);
		if (!rule)
			return true;
		const Pace pace = client_pace(*rule);
		Bucket& b = m_buckets[uint16_t(type)]; // at most one per rule
		if (!b.started)
		{
			b.started = true;
			b.tokens = double(pace.burst);
			b.at = now_ms;
		}
		else if (now_ms > b.at)
		{
			const double refilled = b.tokens + double(now_ms - b.at) / double(pace.interval_ms);
			b.tokens = refilled < double(pace.burst) ? refilled : double(pace.burst);
			b.at = now_ms;
		}
		if (b.tokens < 1.)
			return false;
		b.tokens -= 1.;
		return true;
	}
	void reset() { m_buckets.clear(); }

private:
	struct Bucket
	{
		bool started = false;
		double tokens = 0.;
		uint64_t at = 0;
	};
	std::map<uint16_t, Bucket> m_buckets;
};
} // namespace limits
} // namespace coop
