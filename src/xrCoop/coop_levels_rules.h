// Anomaly Coop — split levels, the rules (docs/plans/2026-10-24-f3a-split-levels.md §1.1, §1.5, §1.10, §3). Each player
// may be on a different level. The host's PC leads them all: the host's process leads its own level, a background map
// server (Mk) leads every other level that has players. This header is the pure part of that, with no engine in it:
//   - Gate / effective_gate   how a level is entered (free, better together, together) and how the two game rules
//                             coop_split_levels and coop_story_together fold into it;
//   - Table                   who sits on which level: who leads a level, whom its messages reach, who sees what;
//   - decide_move             the verdict on a player's move: go, wait at the board, refused (the map-server cap);
//   - ServerLife              the life of one map server: when to launch it, when to stop it (the pause, "linger");
//   - FlushRound              the round in which every map server sends a full snapshot before a save.
// The barrier of the simultaneous load is in coop_load_barrier.h.
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop/coop_levels*.cpp) and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace coop
{
namespace levels
{
// A seat that is on no level (choosing a character, joining) and the level id nobody has.
constexpr uint16_t kNoLevel = 0xffff;

// ---------------------------------------------------------------- gates

// How a level is entered (docs/research/2026-10-24-story-levels-together.md §5). The numbers are the enum of the wire
// (LevelMap: u8 gate); the words are those of configs/coop_levels_together.ltx and of coop.levels.set_gate.
enum class Gate : uint8_t
{
	Free = 0, // nothing: a player goes where he wants, alone
	BetterTogether = 1, // "suggest": a notice on the exit, nobody waits
	Together = 2, // the waiting point: entering waits for every player (leaving is never held)
};

inline const char* gate_name(Gate g)
{
	switch (g)
	{
	case Gate::Free: return "free";
	case Gate::BetterTogether: return "suggest";
	case Gate::Together: return "together";
	}
	return "";
}

// Exactly one of the three words (lower case, nothing around it). `out` is left as it was on false.
inline bool parse_gate(std::string_view word, Gate& out)
{
	for (Gate g : {Gate::Free, Gate::BetterTogether, Gate::Together})
		if (word == gate_name(g))
		{
			out = g;
			return true;
		}
	return false;
}

// The gate that rules a move once the two game rules are known:
//   split levels off     the group moves as one through the waiting point at every exit (Phase 3 behaviour): Together;
//   story maps together off  a Together level is only a suggestion: BetterTogether;
//   otherwise            the gate stands. With split levels on, a value that is none of the three passes through and
//                        decide_move refuses it; with split levels off every value, valid or not, becomes Together.
inline Gate effective_gate(Gate g, bool split, bool story_together)
{
	if (!split)
		return Gate::Together;
	if (g == Gate::Together && !story_together)
		return Gate::BetterTogether;
	return g;
}

// ---------------------------------------------------------------- who is where

// One player of the session as the world server sees him. plays = false: a seat that is not a player - the ghost actor of
// a map server, a future console host that has no actor - it leads nothing, receives nothing, keeps no level served.
struct Seat
{
	uint32_t peer = 0;
	uint16_t level = kNoLevel; // the level he is on, or the one he is loading into
	bool in_world = false; // false while he loads (the level is already his, map messages are not yet)
	bool plays = true;
};

// What leads a level.
enum class Server : uint8_t
{
	None, // no player is on it: it is not loaded anywhere (the world server holds it offline)
	HostProcess, // the host plays on it: the host's own process is its online
	MapServer, // players are on it and the host is not: a background map server must lead it
};

// The seats of the session, in the order they came. The host's seat is the one whose peer is `host_peer` (0, kHostPeer).
class Table
{
public:
	explicit Table(uint32_t host_peer = 0) : host_peer_(host_peer) {}

	// Adds a seat, or replaces the one of the same peer (its place in the order stays).
	void set(const Seat& s)
	{
		for (Seat& e : seats_)
			if (e.peer == s.peer)
			{
				e = s;
				return;
			}
		seats_.push_back(s);
	}

	void remove(uint32_t peer)
	{
		seats_.erase(std::remove_if(seats_.begin(), seats_.end(), [peer](const Seat& s) { return s.peer == peer; }),
		    seats_.end());
	}

	void clear() { seats_.clear(); }

	const Seat* find(uint32_t peer) const
	{
		for (const Seat& s : seats_)
			if (s.peer == peer)
				return &s;
		return nullptr;
	}

	const std::vector<Seat>& seats() const { return seats_; }
	uint32_t host_peer() const { return host_peer_; }

	// Who leads `level`: the host's process when the host plays on it (even if a client came to it first), else a map
	// server when any other player is on it or loading into it, else nobody. The host that is still loading his level
	// leads it already: the level is his process's.
	Server server_of(uint16_t level) const
	{
		if (level == kNoLevel)
			return Server::None;
		bool other = false;
		for (const Seat& s : seats_)
		{
			if (!s.plays || s.level != level)
				continue;
			if (s.peer == host_peer_)
				return Server::HostProcess;
			other = true;
		}
		return other ? Server::MapServer : Server::None;
	}

	// Players who are on `level` or loading into it (a player on his way already needs the level's server).
	uint32_t population(uint16_t level) const
	{
		if (level == kNoLevel)
			return 0;
		uint32_t n = 0;
		for (const Seat& s : seats_)
			if (s.plays && s.level == level)
				++n;
		return n;
	}

	// The count a map server of `level` is told with ServerLife::players: its players, 0 when the level has no map server
	// (the host's process leads it, or nobody does).
	uint32_t map_players(uint16_t level) const { return server_of(level) == Server::MapServer ? population(level) : 0; }

	// Every level that needs a map server now, ascending.
	std::vector<uint16_t> map_server_levels() const
	{
		std::vector<uint16_t> out;
		for (const Seat& s : seats_)
			if (s.level != kNoLevel && server_of(s.level) == Server::MapServer &&
			    std::find(out.begin(), out.end(), s.level) == out.end())
				out.push_back(s.level);
		std::sort(out.begin(), out.end());
		return out;
	}

	// The players who are in the world on `level` (host included), in the order of the seats.
	std::vector<uint32_t> on(uint16_t level) const
	{
		std::vector<uint32_t> out;
		if (level == kNoLevel)
			return out;
		for (const Seat& s : seats_)
			if (s.plays && s.in_world && s.level == level)
				out.push_back(s.peer);
		return out;
	}

	// Whom a message of `level` reaches: on(level) without the sender (a peer who is not on the level excepts nobody).
	std::vector<uint32_t> receivers(uint16_t level, uint32_t except) const
	{
		std::vector<uint32_t> out = on(level);
		out.erase(std::remove(out.begin(), out.end(), except), out.end());
		return out;
	}

	// Does the player see the events of `level`: only his own level's, and only when he is in the world.
	bool observes(uint32_t peer, uint16_t level) const
	{
		if (level == kNoLevel)
			return false;
		const Seat* s = find(peer);
		return s && s->plays && s->in_world && s->level == level;
	}

private:
	uint32_t host_peer_;
	std::vector<Seat> seats_;
};

// ---------------------------------------------------------------- the verdict on a move

enum class MoveVerdict : uint8_t
{
	Go, // the player goes by himself, now
	Board, // the move waits at the waiting point and goes with the group (wire: LevelMoveResult Together)
	RefuseCap, // a new map server is needed and coop_max_map_servers are running already
	RefuseInvalid, // the gate is none of the three
};

// `effective` = effective_gate(...) of the level he goes to; `needs_new_server` = nobody leads that level yet and the
// mover is not the host (the host's process would lead it); `running` = map servers running now; `max_servers` =
// coop_max_map_servers. A Together move is the whole group going to one map: it starts no server of its own, no cap.
inline MoveVerdict decide_move(Gate effective, bool needs_new_server, uint32_t running, uint32_t max_servers)
{
	switch (effective)
	{
	case Gate::Together: return MoveVerdict::Board;
	case Gate::Free:
	case Gate::BetterTogether:
		return (needs_new_server && running >= max_servers) ? MoveVerdict::RefuseCap : MoveVerdict::Go;
	}
	return MoveVerdict::RefuseInvalid;
}

// ---------------------------------------------------------------- the life of one map server

enum class ServerState : uint8_t
{
	Off, // no process
	Starting, // launched, loading the map (at most kStartTimeoutMs)
	Ready, // loaded, nobody on it yet (the players it was started for are gone): the pause runs
	Serving, // loaded, at least one player on it
	Draining, // loaded, the last player has left: the pause runs, a returning player cancels it
	Stopping, // the Stop order is out: it hands everything to the world server and exits (at most kStopTimeoutMs)
	Lost, // the process died or hung: the world server resolves its journal, a new one starts if players wait
};

constexpr uint32_t kStartTimeoutMs = 120000; // a process that is not Ready by then is given up on
constexpr uint32_t kStopTimeoutMs = 15000; // a process that has not exited by then after Stop is killed
constexpr uint32_t kDefaultLingerMs = 60000; // the pause: coop_mapserver_linger = 60 s
constexpr uint32_t kFlushTimeoutMs = 3000; // a map server that has not sent its snapshot by then is left to its stream
// A process that dies again and again before it serves anybody must not be started for ever (the players would wait on the
// loading screen for good): this many deaths in a row without a healthy run, and ServerLife gives up (gave_up()).
constexpr uint32_t kMaxStartFailures = 3;
// A process that has lived this long after Ready has proved itself: its failures so far are forgotten, and a later death is a
// crash (a new start is wanted), not a failed start.
constexpr uint32_t kHealthyRunMs = 30000;

// One map server of one level, as the world server keeps it. It decides nothing by itself: the caller feeds it what he
// sees and does what it asks. One tick of the caller:
//     life.players(table.map_players(level), now);              // the count, every tick
//     if (life.gave_up()) { give the moves to this level up; return the movers to where they were; }
//     else if (life.want_start()) { launch(level); life.started(now); }
//     else if (life.want_stop(now))
//         switch (life.state())
//         {
//         case ServerState::Starting:                            // it hangs while loading
//         case ServerState::Stopping: kill(); life.lost(); break; // it hangs while leaving
//         default: send_stop_order(); life.stopping(now); break; // the pause is over
//         }
// and, as the reports come: ready() when the process says Ready, stopped() when it says Stopped or exits, lost() when
// the bridge to it breaks. A report that does not fit the state (late, twice, from an old process) is ignored.
// The clock is the caller's: `now` in ms, any origin; a value older than one seen before is taken as that one. The reports
// ready(), stopped() and lost() have no clock of their own: they use the latest `now` that players() or started() saw, so
// players() must be called every tick (a death is judged young, the careful way, if the clock was not told for long).
//
// Failed starts. A process that is lost while it loads (Starting), or that is lost fewer than kHealthyRunMs after it was
// Ready, is a failed start; so is one that exits by itself in those cases. kMaxStartFailures of them in a row and the server
// "gave up": want_start() is false, the state stays Lost, and the caller must not leave the movers on the loading screen.
// The series ends (failures() back to 0) when a process has lived kHealthyRunMs after Ready (seen in players()), when one is
// stopped cleanly, when the players of the level are gone (players(0): the movers went back; the next player who tries gets
// a fresh series, so a retry always needs a player's action) or when the caller says retry_allowed(). Ready alone does not
// end it: a map that loads and dies at the first player would otherwise be restarted for ever.
struct ServerLife
{
	uint32_t linger_ms = kDefaultLingerMs; // how long a server with no players stays

	ServerState state() const { return state_; }
	uint32_t count() const { return n_; }

	// Failed starts in a row (see above).
	uint32_t failures() const { return failures_; }
	// Too many failed starts in a row: no process is launched for these players (want_start() is false).
	bool gave_up() const { return failures_ >= kMaxStartFailures; }
	// Forget the failed starts: the next want_start() may launch again (the caller knows something changed).
	void retry_allowed() { failures_ = 0; }

	// Does this server count against coop_max_map_servers (decide_move's `running`)? Yes while a process is there to serve
	// or to be served: Starting, Ready, Serving, Draining. No for Off and Lost (no process) and for Stopping (it is on its
	// way out, its place is free for the next map).
	bool running() const
	{
		return state_ == ServerState::Starting || state_ == ServerState::Ready || state_ == ServerState::Serving ||
		    state_ == ServerState::Draining;
	}

	// The players of this server now (Table::map_players). Counts that come back to zero start the pause once; the same
	// zero again does not restart it; a player in the pause cancels it. Does nothing to a process that is going down.
	void players(uint32_t n, uint64_t now)
	{
		tick(now);
		n_ = n;
		if (gave_up() && n_ == 0)
			failures_ = 0; // everybody is gone: the next player to try starts a fresh series
		if (loaded() && now_ - ready_at_ >= kHealthyRunMs)
			failures_ = 0; // it has lived: whatever failed before is forgotten
		if (!loaded())
			return;
		if (n_ > 0)
			state_ = ServerState::Serving;
		else if (state_ == ServerState::Serving)
		{
			state_ = ServerState::Draining;
			since_ = now_;
		}
	}

	// Players want a level that no process leads: launch one, then say started(). Not once the server gave up.
	bool want_start() const { return (state_ == ServerState::Off || state_ == ServerState::Lost) && n_ > 0 && !gave_up(); }

	// The process was launched. Only an Off or a Lost server is (re)started.
	void started(uint64_t now)
	{
		if (state_ != ServerState::Off && state_ != ServerState::Lost)
			return;
		tick(now);
		state_ = ServerState::Starting;
		since_ = now_;
	}

	// The process loaded its map (MapServerState Ready).
	void ready()
	{
		if (state_ != ServerState::Starting)
			return;
		ready_at_ = now_;
		if (n_ > 0)
			state_ = ServerState::Serving;
		else
		{
			state_ = ServerState::Ready;
			since_ = now_;
		}
	}

	// Time to end this process: the pause is over, or a start or a stop took too long. For Starting and Stopping it means
	// "kill it" (whatever the number of players: a start or a stop that hangs is ended); for Ready and Draining "order Stop".
	// Never for a Serving server: one with players is stopped only by the caller's own decision (stopping()).
	bool want_stop(uint64_t now) const
	{
		switch (state_)
		{
		case ServerState::Starting: return elapsed(now) >= kStartTimeoutMs;
		case ServerState::Ready:
		case ServerState::Draining: return elapsed(now) >= linger_ms;
		case ServerState::Stopping: return elapsed(now) >= kStopTimeoutMs;
		default: return false;
		}
	}

	// The Stop order was sent (after the pause, or at once when the host moves: then the server has players too).
	void stopping(uint64_t now)
	{
		if (state_ != ServerState::Ready && state_ != ServerState::Serving && state_ != ServerState::Draining)
			return;
		tick(now);
		state_ = ServerState::Stopping;
		since_ = now_;
	}

	// The process ended. After a Stop that is the normal end; at any other time it was not asked to, and it is a loss.
	void stopped()
	{
		if (state_ == ServerState::Stopping)
		{
			state_ = ServerState::Off;
			failures_ = 0; // it came up and left cleanly
		}
		else if (state_ != ServerState::Off && state_ != ServerState::Lost)
		{
			note_death();
			state_ = ServerState::Lost;
		}
	}

	// The process died, hung (killed by the caller) or the bridge to it broke.
	void lost()
	{
		if (state_ == ServerState::Off || state_ == ServerState::Lost)
			return;
		note_death();
		state_ = ServerState::Lost;
	}

private:
	void tick(uint64_t now)
	{
		if (now > now_)
			now_ = now;
	}
	uint64_t elapsed(uint64_t now) const { return now > since_ ? now - since_ : 0; }
	bool loaded() const { return state_ == ServerState::Ready || state_ == ServerState::Serving || state_ == ServerState::Draining; }

	// A process that nobody asked to end is gone. While loading, or not yet healthy, that is a failed start; after a healthy
	// run it is a crash and the series starts over. A death while Stopping is the end it was going to have anyway.
	void note_death()
	{
		if (state_ == ServerState::Starting)
			++failures_;
		else if (loaded())
		{
			if (now_ - ready_at_ >= kHealthyRunMs)
				failures_ = 0;
			else
				++failures_;
		}
	}

	ServerState state_ = ServerState::Off;
	uint32_t n_ = 0;
	uint32_t failures_ = 0; // failed starts in a row
	uint64_t now_ = 0; // the latest clock value seen
	uint64_t since_ = 0; // when the state that runs a timer was entered
	uint64_t ready_at_ = 0; // when the process said Ready
};

// ---------------------------------------------------------------- the flush round before a save

// Why the world server asks every map server for a full snapshot (LevelFlush.why on the wire).
enum class FlushWhy : uint8_t
{
	Save = 1,
	Move = 2,
	Stop = 3,
	HostMove = 4,
};

// One round: the servers asked, which of them answered with a full snapshot of that round, and the 3 s clock. A server
// that does not answer holds the save for kFlushTimeoutMs; then the save goes by its last stream and a line in the log.
struct FlushRound
{
	// Starts round `round` (>= 1; 0 is the snapshot stream, not a round - refused, the round before stays). A round that
	// is running is replaced: what was answered to it counts for nothing here. A server listed twice is one server.
	bool start(uint32_t round, FlushWhy why, std::vector<uint32_t> servers, uint64_t now)
	{
		if (round == 0)
			return false;
		round_ = round;
		why_ = why;
		started_ = now;
		pending_.clear();
		for (uint32_t s : servers)
			if (std::find(pending_.begin(), pending_.end(), s) == pending_.end())
				pending_.push_back(s);
		active_ = true;
		return true;
	}

	// A full snapshot of `round` came from `server`. Another round, round 0, a server not asked, a repeat: ignored.
	void reply(uint32_t server, uint32_t round)
	{
		if (!active_ || round == 0 || round != round_)
			return;
		pending_.erase(std::remove(pending_.begin(), pending_.end(), server), pending_.end());
	}

	void clear()
	{
		active_ = false;
		pending_.clear();
	}

	bool active() const { return active_; }
	bool complete() const { return active_ && pending_.empty(); }
	bool timed_out(uint64_t now) const { return active_ && !pending_.empty() && elapsed(now) >= kFlushTimeoutMs; }
	// Everybody answered, or the time is up: the save may be written.
	bool done(uint64_t now) const { return complete() || timed_out(now); }

	uint32_t round() const { return round_; }
	FlushWhy why() const { return why_; }
	// The servers that have not answered, in the order they were asked: the laggards of the log line.
	std::vector<uint32_t> missing() const { return pending_; }

private:
	uint64_t elapsed(uint64_t now) const { return now > started_ ? now - started_ : 0; }

	bool active_ = false;
	uint32_t round_ = 0;
	FlushWhy why_ = FlushWhy::Save;
	uint64_t started_ = 0;
	std::vector<uint32_t> pending_;
};
} // namespace levels
} // namespace coop
