// Anomaly Coop — what a host takes from a client: message rates per type (F2 T3) and new player characters per
// session (Ruling F2-10 b). The game installs the rates (coop_join install) and keeps the character count (coop_join).
// F2 T14: a client paces itself by the same rates (Pacer; coop_script.cpp script events, coop_sync_api.cpp).
// F2 T3b: the story, shot, item, trade, object and request messages got rows too (Ruling F2-15); F2 T3c: the player stream,
// the life states and the chat (a core message, taken by Session::handle_chat) did too, and the story outbox of a client
// (Outbox) merges relations and keeps them in a lane of their own. Still without a row: the join lifecycle, TimeRequest,
// the dialog choice, PlayerAnim / PlayerGrenade (no handler) and the blobs (their own limits).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>

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

	// F2 T3c (T3b review I-1, I-2): the player stream, the life states and the chat. The host relays every one of them to the
	// other players (broadcast), so a modified client's flood is multiplied by the players there are.
	// The movement: strictly one per kSendIntervalMs = 50 ms, time based, never a catch-up after a hitch (coop_players.cpp:26,
	// :283, :294-296; the only sender), so 20 a second; the network may bunch them (a lag spike of 2 s: 40). The row is 4 x the
	// rate and 3.2 s of the stream in a row. A dropped one costs nothing (Unreliable lane, the newest wins), but a log line
	// "rate limit: PlayerState" for an honest player would send us looking for a fault that is not there.
	{Msg::PlayerState, 64, 12},
	// send_my_state (coop_survival.cpp:651): at every change of the life state (:729 down, :743 dead, :765 / :851 up or
	// respawning, :893 spectating), when the character changes (:2236) and as a refresh every 5 s (:65, :1840). A fall and its end
	// is 4 in a row (estimate); a fall lasts at least the bleed-out of 5 s (coop_death_rules.h:104), so the honest rate is
	// below one a second (estimate). 4 x both: 16 in a row, 4 a second. The host decides on each one and broadcasts it
	// reliably (coop_survival.cpp:1563).
	{Msg::PlayerWounded, 16, 250},
	// A revive request is sent when the USE hold on a wounded teammate has lasted kReviveHoldMs = 3000 (coop_survival.cpp:61,
	// :1639, :1664): one in 3 s at the most (estimate: 2 in a row, 1 in 3 s). 4 x that: 8 in a row, 1.33 a second.
	// HUD v2 part 2 (revive::Announcer, coop_revive_hold.h): the same row now also carries Begin and Cancel of the hold, and it
	// was not raised: a reviver announces a hold after 250 ms of USE, at most one Begin per 2 s, and a Cancel only for an
	// announced hold, so a Begin + Cancel (or Begin + Request) is a pair per 2 s at the most, one a second, below the row's
	// 1.33 a second even for a player who mashes the button (tests/test_revive_hold.cpp runs that against this row).
	{Msg::PlayerRevive, 8, 750},
	// One per death of the own actor (coop_events.cpp:312 -> coop_requests.cpp:2416, :518); a death is followed by a respawn or
	// a spectator's wait of seconds (estimate: 2 in a row, one in 2 s). 4 x that: 8 in a row, 2 a second. The host logs each
	// one and broadcasts it reliably (coop_events.cpp:817-835).
	{Msg::PlayerDied, 8, 500},
	// A human (estimate: a paste of 5 lines at once, 2 lines a second at the most), 4 x that: 20 in a row, 8 a second. Chat is a
	// core message (Session::dispatch, not game_message): Session::handle_chat takes a token of this row itself. Over it: the
	// line is dropped, counted, logged like the other rows, not logged as chat, not given to Lua (on_chat), not relayed.
	{Msg::Chat, 20, 125},

	// F2 T12b: ids a client gives back (coop_id_lease.h): the ones an object of his holds when a lease comes, and the id of
	// each create the host refused. The client sends at most one ack per kAckGapMs = 250 ms with everything gathered
	// meanwhile (coop_requests.cpp client_pump_leases), so 4 a second; 8 in a row for a lag spike.
	{Msg::IdLeaseAck, 8, 250},
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

// F2 T3c (T3b review I-3): the outbox of a client's story messages (coop_story.cpp push_out / flush_outbox) as a pure queue.
// Two lanes with a cap each:
//  - story: InfoPortion, MapSpot, TaskUpdate, in order, kMaxStoryOutbox (F2 T14 fix 1). What the pacer holds holds the rest
//    of the lane, as before: one lost or reordered info breaks a quest;
//  - relations: Msg::Relation, kMaxRelationOutbox. GAMMA's Fluid Relations answers every stalker killed by a stalker with 8-15
//    set_community_relation calls (game_relations.script:232-236, :361-455), sometimes dozens, and the pacer lets a relation
//    out at 10 a second. In one queue they held the infos, spots and tasks behind them for seconds and, at the cap, took
//    the room of every type. Now a relation waits in a lane of its own: it holds back no story message and a full lane
//    drops only relations.
// A relation that sets the relation of a pair of communities (RelReqCommRelation) is a fact "from -> to is v": a queued one of
// the same pair is replaced by the newer value and keeps its place, so the lane holds at most one per pair (32 x 32 in
// GAMMA) however many calls a fight makes, and the host ends with the value the game last set. Other relations (a goodwill
// to a player set or changed by a delta) are not facts of a pair: the order and the sum of deltas matter, they are not merged.
constexpr size_t kMaxStoryOutbox = 4096;
constexpr size_t kMaxRelationOutbox = 2048; // all 32 x 32 community pairs of GAMMA at once and room for the goodwill changes

struct CommunityPair
{
	int32_t from;
	int32_t to;
};

// What drops again and again (a full queue) is logged for the first and then for every 100th drop, not for each.
inline bool drop_logged(uint64_t drops)
{
	return drops == 1 || (drops != 0 && drops % 100 == 0);
}

template <class Item>
class Outbox
{
public:
	enum class Push : uint8_t
	{
		Queued, // waits in its lane
		Merged, // replaced the queued relation of the same pair
		Full // its lane is full: dropped and counted (dropped_story, dropped_relation)
	};

	Outbox()
	{
		m_story.cap = kMaxStoryOutbox;
		m_relations.cap = kMaxRelationOutbox;
	}

	// `pair` is for Msg::Relation only (the pair a "set the community relation" fact is about); other messages ignore it.
	Push push(Msg type, Item&& item, const CommunityPair* pair = nullptr)
	{
		const bool keyed = pair != nullptr && type == Msg::Relation;
		Lane& lane = type == Msg::Relation ? m_relations : m_story;
		if (keyed)
			for (Entry& e : lane.q)
				if (e.keyed && e.pair.from == pair->from && e.pair.to == pair->to)
				{
					e.item = std::move(item);
					++m_merged;
					return Push::Merged;
				}
		if (lane.q.size() >= lane.cap)
		{
			++lane.dropped;
			return Push::Full;
		}
		lane.q.push_back(Entry{type, keyed, keyed ? *pair : CommunityPair{0, 0}, std::move(item)});
		return Push::Queued;
	}

	// One frame of sending. In each lane, in order, `take(type)` says whether the pacer lets the next one out: if not, it and
	// everything behind it in the lane wait for a later drain (the other lane goes on); if so, `send(item)` gets it. Only what
	// was queued when the drain began is sent: what `send` queues meanwhile waits for the next one. Returns how many went.
	template <class Take, class Send>
	size_t drain(Take&& take, Send&& send)
	{
		Lane* lanes[2] = {&m_story, &m_relations};
		size_t budget[2] = {m_story.q.size(), m_relations.q.size()};
		size_t sent = 0;
		bool held = false;
		for (int i = 0; i < 2; ++i)
		{
			Lane& lane = *lanes[i];
			while (budget[i] > 0 && !lane.q.empty())
			{
				if (!take(lane.q.front().type))
				{
					held = true;
					break;
				}
				Entry e = std::move(lane.q.front());
				lane.q.pop_front();
				--budget[i];
				send(e.item);
				++sent;
			}
		}
		if (held)
			++m_held;
		return sent;
	}

	void clear() // the counters stay
	{
		m_story.q.clear();
		m_relations.q.clear();
	}

	bool empty() const { return m_story.q.empty() && m_relations.q.empty(); }
	size_t size() const { return m_story.q.size() + m_relations.q.size(); }
	size_t story_size() const { return m_story.q.size(); }
	size_t relation_size() const { return m_relations.q.size(); }
	uint64_t held() const { return m_held; } // drains in which the pacer held something back
	uint64_t merged() const { return m_merged; } // relations replaced by a newer value of their pair
	uint64_t dropped_story() const { return m_story.dropped; }
	uint64_t dropped_relation() const { return m_relations.dropped; }
	uint64_t dropped_of(Msg type) const { return type == Msg::Relation ? m_relations.dropped : m_story.dropped; }

private:
	struct Entry
	{
		Msg type;
		bool keyed; // a "set the relation of this pair" fact: merged by `pair`
		CommunityPair pair;
		Item item;
	};
	struct Lane
	{
		std::deque<Entry> q;
		size_t cap = 0;
		uint64_t dropped = 0;
	};
	Lane m_story, m_relations;
	uint64_t m_held = 0, m_merged = 0;
};
} // namespace limits
} // namespace coop
