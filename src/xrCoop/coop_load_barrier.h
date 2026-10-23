// Anomaly Coop — the barrier of the simultaneous load (docs/plans/2026-10-24-f3a-split-levels.md §1.5). When players
// change level together (the group at the waiting point, the host's own move, a lone player whose map server is started
// for him), the world server prepares the world and everybody who leaves goes to the loading screen at once. Each says
// LoadReady {epoch} when his world is up; the host's own player is ready when his process's world is up (through the
// local link, world_up); a new map server is ready when it reports Ready. When all are ready the world server sends
// LoadWait {Enter} and the screens come off together. A laggard does not hold the others for ever: kLoadWaitMs after the
// start the barrier opens for those who are ready, and he enters alone when he is ready. An answer of an older
// transition (another epoch) is not an answer to this one.
// Pure C++17, header-only, no engine dependencies: used by xrGame and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coop
{
namespace levels
{
constexpr uint32_t kLoadWaitMs = 60000; // the longest the players who are ready wait for one who is not

class LoadBarrier
{
public:
	// host_peer: the seat of the host's own player (0, kHostPeer); world_up() is his LoadReady.
	explicit LoadBarrier(uint32_t host_peer = 0) : host_peer_(host_peer) {}

	// A new transition: `epoch` of the new world, `movers` = the peers that leave (the host's player too when his process
	// reloads), `wait_server` = a new map server loads for it and must report Ready. Replaces the barrier that was.
	void start(uint32_t epoch, std::vector<uint32_t> movers, bool wait_server, uint64_t now)
	{
		active_ = true;
		epoch_ = epoch;
		started_ = now;
		wait_server_ = wait_server;
		server_ready_ = false;
		movers_.clear();
		for (uint32_t p : movers)
			if (std::find_if(movers_.begin(), movers_.end(), [p](const Mover& m) { return m.peer == p; }) == movers_.end())
				movers_.push_back({p, false});
	}

	// LoadReady {epoch} from `peer`. Another epoch, a peer who is not leaving, a barrier that is not started: ignored.
	void ready(uint32_t peer, uint32_t epoch)
	{
		if (!active_ || epoch != epoch_)
			return;
		for (Mover& m : movers_)
			if (m.peer == peer)
				m.ready = true;
	}

	// The world of the host's process is up: the host's player is ready (no message: it is the same process). Another
	// epoch, or a transition where the host stays on his map: ignored.
	void world_up(uint32_t epoch) { ready(host_peer_, epoch); }

	// The new map server reported Ready. Not needed by this barrier: ignored.
	void server_ready()
	{
		if (active_ && wait_server_)
			server_ready_ = true;
	}

	// A mover left the session (dropped, kicked): he is not waited for. The map server asked for him still is.
	void drop(uint32_t peer)
	{
		movers_.erase(std::remove_if(movers_.begin(), movers_.end(), [peer](const Mover& m) { return m.peer == peer; }),
		    movers_.end());
	}

	// Everything is over: the screens are off, the barrier is not needed.
	void clear()
	{
		active_ = false;
		movers_.clear();
	}

	// May the screens come off: every mover is ready, or kLoadWaitMs have passed since the start - and the new map server
	// is up in either case (a map nobody leads cannot be entered, the wait of the players does not open it). Once true it
	// stays true; after that each mover who is ready (is_ready) enters, a laggard when he becomes ready.
	bool enter(uint64_t now) const
	{
		if (!active_ || (wait_server_ && !server_ready_))
			return false;
		return waiting().empty() || elapsed(now) >= kLoadWaitMs;
	}

	bool active() const { return active_; }
	uint32_t epoch() const { return epoch_; }

	bool is_ready(uint32_t peer) const
	{
		for (const Mover& m : movers_)
			if (m.peer == peer)
				return m.ready;
		return false;
	}

	// A new map server is still being waited for.
	bool waiting_server() const { return active_ && wait_server_ && !server_ready_; }

	// The movers who are not ready, in the order they were given: the line "Waiting for: ..." of the loading screen.
	std::vector<uint32_t> waiting() const
	{
		std::vector<uint32_t> out;
		if (!active_)
			return out;
		for (const Mover& m : movers_)
			if (!m.ready)
				out.push_back(m.peer);
		return out;
	}

private:
	struct Mover
	{
		uint32_t peer;
		bool ready;
	};

	uint64_t elapsed(uint64_t now) const { return now > started_ ? now - started_ : 0; }

	uint32_t host_peer_;
	bool active_ = false;
	uint32_t epoch_ = 0;
	uint64_t started_ = 0;
	bool wait_server_ = false;
	bool server_ready_ = false;
	std::vector<Mover> movers_;
};
} // namespace levels
} // namespace coop
