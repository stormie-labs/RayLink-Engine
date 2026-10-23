// Anomaly Coop — game messages for a world that is not there.
//
// Live crash 2026-10-23 20:00 (build 9, GAMMA): a client at a level exit asked the group to change level; while the host
// built the new level (CLevel::CLevel() in the log) the session kept pumping (CGamePersistent::OnFrame -> Session::update
// -> run_pending), a PlayerVitals reached coop_equip.cpp store_vitals and ai().alife() had no simulator: access violation.
// The same window opens at a load, at a client's entry (a client in the main menu, choosing a character, gets what the
// host sends everybody) and in the main menu. 100+ call sites of ai().alife() / Level() / Actor() cannot each carry the
// right check, so the session carries one rule for all game messages:
//
//  - every game message type has a NEED (need_of): Free (its handler works without a world: it keeps books, or queues for
//    a frame that checks the world, or a join step that has its own guard), Drop (it needs the world, and a missing world
//    makes it void: a snapshot the sender repeats, an event that is stale later, a report of a join that is gone), Hold
//    (it needs the world and changes it: a request, a report of a change, an ordered delta);
//  - the game tells the WORLD of this machine at the moment a handler is due (Session::set_world_gate): Up (the level
//    stands and the simulator and the local actor exist), Hole (the host's world is not there right now: it is being
//    built or torn down), Absent (a client that is not in the host's world: connecting, choosing a character, loading, or
//    its level gone; what the host sends then is of a world it does not have, the host sends the state again at its join);
//  - decide(need, world): Free always runs; in an Up world everything runs; a Drop message is dropped when the world is
//    not Up; a Hold message waits in a queue on the host (Hole) and is dropped on a client (Absent).
//
// Held messages are stamped with the world epoch they were received in (coop_session.h set_world_epoch, R2 I1). When the
// world is back they run in arrival order, before anything newer, if the epoch is the same; if it moved on they are void
// (the world they were made in is gone: it was replaced, and every client rejoins), so a request made for the old level
// never touches the new one. In every flow known today the world IS replaced after a Hole, so the queue ends up void; it
// keeps a request that arrives in a hole of the SAME world (the local actor re-created, a pause between two steps of one
// load) instead of failing it. The queue is bounded (entries, bytes, age): the oldest entry goes first.
//
// Pure C++17, header-only, no engine dependencies: used by the session and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "coop_protocol.h" // Msg

namespace coop
{
namespace wgate
{
enum class Need : uint8_t
{
	Unclassified, // no row below: treated as Hold (safe), and tests/test_world_gate.cpp fails until it has one
	Free, // its handler works without a world
	Drop, // needs the world; void without it
	Hold, // needs the world and changes it; kept for the world's return on the host
};

// One row per game message. A new message (coop_protocol.h) needs its row here: test_world_gate.cpp
// every_named_game_message_is_classified fails without it.
inline Need need_of(Msg type)
{
	switch (type)
	{
	// --- the join lifecycle (coop_join.cpp): its handlers answer a missing world themselves (host_can_start kicks with a
	// reason, a client's steps run before its world exists, the host's SessionReset comes when its level is up)
	case Msg::JoinRequest:
	case Msg::WorldBlob:
	case Msg::JoinComplete:
	case Msg::SessionReset:
	case Msg::LevelChange:
	case Msg::AvatarList: // a client that is not live ignores it itself
	case Msg::CharOffer:
	case Msg::CharChoice:
	case Msg::TransitionState: // the host's board, a loading client ignores it

	// --- world-free by design: the handler keeps the newest value or queues, a frame applies it when the world is there
	case Msg::TimeSync: // client: kept while loading, applied at live (coop_worldstate.cpp, P6)
	case Msg::WeatherSync:
	case Msg::TimeRequest: // host: a log line
	case Msg::Emission:
	case Msg::InfoPortion: // coop_story.cpp defers its own (dispatch -> g_deferred until ready())
	case Msg::TaskList:
	case Msg::TaskUpdate:
	case Msg::MapSpot:
	case Msg::Relation:
	case Msg::Progress:
	case Msg::StoryHash:
	case Msg::StoryResync:
	case Msg::SyncSet: // coop.sync: the store is C++, its Lua runs from a frame that checks lua_ready()
	case Msg::SyncFull: // arrives BEFORE the client's world (join)
	case Msg::SyncRequest:
	case Msg::SyncResult:
	case Msg::SyncCast:
	case Msg::ScriptEvent: // queued for coop_script.cpp frame(), Lua only when lua_ready()
	case Msg::CheckpointState: // the client's mirror (C++) and queued Lua callbacks
	case Msg::RestState:
	case Msg::RestSleep:
	case Msg::PlayerRules: // survival rules: stored
	case Msg::PlayerDied: // relay and a log line

	// --- F3a: the tables and the barrier keep their own books (coop_levels, coop_join), a map server's state is a number
	case Msg::LevelMap: // the newest table is kept; a loading client applies it when its world is there
	case Msg::LevelMoveResult: // the answer to a request of a player who is in a world
	case Msg::LevelFlush: // a map server answers from its own world; a hole there means it is loading
	case Msg::Tunnel: // the inner packet passes its own need_of when it is taken out
	case Msg::MapServerState:
	case Msg::MapServerOrder:
	case Msg::LoadWait: // the loading screen is exactly the time there is no world: the barrier must run in the hole
	case Msg::StateDump: return Need::Free;

	// --- snapshots and events: the sender sends the next one, or the news is stale when the world is back
	case Msg::WorldLoaded: // a client's load of a world the host is leaving
	case Msg::TransitionVote: // a claim refreshed every few seconds (coop_transition.h kClaimRefreshMs)
	case Msg::PlayerState: // 20 Hz
	case Msg::PlayerVitals: // periodic
	case Msg::PlayerEquip: // periodic refresh and a resend at every join
	case Msg::PlayerAnim:
	case Msg::PlayerShot:
	case Msg::PlayerGrenade:
	case Msg::PlayerHit: // a hit of a world that is gone must not hurt the next one
	case Msg::EntitySnapshot: // unreliable lane
	case Msg::EntityAnim:
	case Msg::EntityHealth:
	case Msg::EntitySound: // unreliable lane
	case Msg::EntityState: // samples and rests repeat, destroyed / broken / explosions are stale visuals
	case Msg::ZoneState: // the host sends the zones again when a client is live (host_on_peer_ready)
	case Msg::MoneyBalance: // the host sends the balance again when its player is live (host_on_peer_in_world)
	case Msg::DeathMarker:
	case Msg::LevelXferDown: // F3a: the world server repeats it until it is acknowledged
	case Msg::LevelBrain: // 1 Hz, the next one replaces it
	case Msg::LevelHash: // every 10 s
	case Msg::LevelResync: return Need::Drop;

	// --- requests, reports of a change and the host's ordered deltas
	case Msg::PlayerWounded:
	case Msg::PlayerRevive:
	case Msg::NewCharacter:
	case Msg::EntitySpawn:
	case Msg::EntityDestroy:
	case Msg::EntitySwitch:
	case Msg::EntityEvent:
	case Msg::EntityParent:
	case Msg::EntityDied:
	case Msg::EntityLocal:
	case Msg::ItemState:
	case Msg::ReqSpawn:
	case Msg::ReqDestroy:
	case Msg::ReqTake:
	case Msg::ReqDrop:
	case Msg::ReqTransfer:
	case Msg::ReqUse:
	case Msg::ReqHit:
	case Msg::ReqResult:
	case Msg::ReqMoney:
	case Msg::ReqContainer:
	case Msg::ReqItemEvent:
	case Msg::ReqLaunch:
	case Msg::IdLease: // carry their world epoch and are checked against it again
	case Msg::IdLeaseAck:
	case Msg::DialogLock:
	case Msg::DialogLockResult:
	case Msg::DialogUnlock:
	case Msg::DialogPhrases:
	case Msg::DialogSelect:
	case Msg::TradeOp:
	case Msg::CheckpointClaim:
	case Msg::RestRequest:
	case Msg::LevelMove: // F3a: a player's leave is a request that changes the world; it waits in a hole, not dropped
	case Msg::LevelSnapshot: // F3a: a map server's state and its halves of a transfer are deltas, in order
	case Msg::LevelEntity:
	case Msg::LevelXferUp:
	case Msg::LoadReady: // F3a: the same epoch is played after the world is up (plan §1.5 step 7)
	case Msg::StateDumpRequest: return Need::Hold;

	default: break;
	}
	return Need::Unclassified;
}

// The world of THIS machine at the moment a handler is due (the game decides: coop_manager.cpp world_state).
enum class World : uint8_t
{
	Up, // the level stands, the simulator and the local actor exist; for a client also: it is live in the host's world
	Hole, // host: no world right now (a level is being built or torn down); a world comes back
	Absent, // client: not in the host's world (menu, character choice, receiving, loading, or its level gone)
};

enum class Act : uint8_t
{
	Run, // call the handler now
	Drop, // never call it; counted
	Hold, // keep it for the world's return (host only), counted
};

inline Act decide(Need need, World world)
{
	if (need == Need::Free || world == World::Up)
		return Act::Run;
	if (need == Need::Drop)
		return Act::Drop;
	// Hold, and an unclassified message (safe: it never touches a missing world)
	return world == World::Hole ? Act::Hold : Act::Drop;
}

struct Limits
{
	size_t max_entries = 1024; // a hole lasts a level load (tens of seconds): hundreds of requests at the worst
	size_t max_bytes = 1024 * 1024;
	uint32_t max_age_ms = 120000; // as long as one load holds the link (coop_link_hold.h kLoadHoldMs)
};

// A game message kept for the world's return: the whole packet (the 2-byte type header included) and who sent it.
struct Held
{
	uint64_t conn = 0; // the session's connection serial of the sender
	uint16_t type = 0;
	uint32_t epoch = 0; // the world epoch it was received in
	uint64_t at_ms = 0;
	std::shared_ptr<std::vector<uint8_t>> bytes;
};

struct HoldStats
{
	uint64_t held = 0; // entered the queue
	uint64_t replayed = 0; // ran when the world came back
	uint64_t void_epoch = 0; // the world they were made in was replaced
	uint64_t expired = 0; // waited longer than Limits::max_age_ms
	uint64_t evicted = 0; // pushed out by the entry or byte limit
	uint64_t gone = 0; // its sender left meanwhile
};

class HoldQueue
{
public:
	HoldQueue() = default;
	explicit HoldQueue(const Limits& limits) : m_limits(limits) {}

	void set_limits(const Limits& limits) { m_limits = limits; }
	const Limits& limits() const { return m_limits; }

	bool empty() const { return m_q.empty(); }
	size_t size() const { return m_q.size(); }
	size_t bytes() const { return m_bytes; }
	const HoldStats& stats() const { return m_stats; }

	// Keeps `h` at the back. Over the entry or byte limit the OLDEST entries go (the newest are the likeliest to still be
	// meant). Returns how many went. One packet over the byte limit alone is not kept at all.
	size_t push(Held h)
	{
		++m_stats.held;
		const size_t size = h.bytes ? h.bytes->size() : 0;
		if (size > m_limits.max_bytes)
		{
			++m_stats.evicted;
			return 1;
		}
		m_bytes += size;
		m_q.push_back(std::move(h));
		size_t evicted = 0;
		while (!m_q.empty() && (m_q.size() > m_limits.max_entries || m_bytes > m_limits.max_bytes))
		{
			m_bytes -= m_q.front().bytes ? m_q.front().bytes->size() : 0;
			m_q.pop_front();
			++evicted;
		}
		m_stats.evicted += evicted;
		return evicted;
	}

	// The world is back: runs the entries in arrival order. `ready()` is asked before each one (a handler may start a
	// level change: the rest waits for the next return). An entry made in another epoch than `epoch` is void, one older
	// than the age limit expired; `run(h)` returns false for one whose sender is gone. Returns the entries run.
	template <class Ready, class Run>
	size_t drain(uint32_t epoch, uint64_t now_ms, Ready ready, Run run)
	{
		size_t ran = 0;
		while (!m_q.empty() && ready())
		{
			Held h = std::move(m_q.front());
			m_bytes -= h.bytes ? h.bytes->size() : 0;
			m_q.pop_front();
			if (h.epoch != epoch)
			{
				++m_stats.void_epoch;
				continue;
			}
			if (now_ms > h.at_ms && now_ms - h.at_ms > m_limits.max_age_ms)
			{
				++m_stats.expired;
				continue;
			}
			if (!run(h))
			{
				++m_stats.gone;
				continue;
			}
			++m_stats.replayed;
			++ran;
		}
		return ran;
	}

	// The world epoch moved on: what was made in another one is void now, not when the world comes back. Returns how many.
	size_t purge(uint32_t epoch)
	{
		size_t voided = 0;
		for (auto it = m_q.begin(); it != m_q.end();)
		{
			if (it->epoch == epoch)
			{
				++it;
				continue;
			}
			m_bytes -= it->bytes ? it->bytes->size() : 0;
			it = m_q.erase(it);
			++voided;
		}
		m_stats.void_epoch += voided;
		return voided;
	}

	// The session ended: nothing is kept.
	void clear()
	{
		m_q.clear();
		m_bytes = 0;
	}

private:
	Limits m_limits;
	std::deque<Held> m_q;
	size_t m_bytes = 0;
	HoldStats m_stats;
};
} // namespace wgate
} // namespace coop
