// Anomaly Coop — W9 link hold: when the frame pump keeps the session link held (the transport's keep-alive thread
// answers the peers for a main thread that is busy loading) and when it lets the link go; the hold word of the
// transport (sealed for good by a crash); how long the host's joins wait for its own level to come up.
// Pure C++17, header-only, no engine dependencies: used by coop::Manager::frame, net::Transport, coop_join.cpp and by
// the unit-test exe.
#pragma once

#include <atomic>
#include <cstdint>

namespace coop
{
namespace link_hold
{
// How long one arm holds the link: the longest single blocking stretch we wait for (a load stage, the texture upload
// of a big level on a slow disk). A process that stops arming (hang, crash dialog) goes silent this long after the
// last arm at most; a crash releases the hold at once (coop_manager.cpp on_crash).
constexpr uint32_t kLoadHoldMs = 120000;

// What the frame pump (Manager::frame, once per frame) sees.
struct FrameState
{
	bool load_steps_queued = false; // the engine runs a level load step by step (g_loading_events)
	bool precache_frames = false; // the first frames after a load: precache, the first update of every script
	bool joining_world = false; // a client between "world received" and live
	// This process crashed (coop_manager.cpp on_crash, any thread). The main thread may still run a frame or a load
	// stage before the crash dialog stops it; a hold armed there kept the crashed player "connected" for kLoadHoldMs
	// (review of the live fixes, 2026-10-23). For good: nothing holds the link after a crash.
	bool crashed = false;
};

// A load stage (on_blocking_work) may arm the hold: not after a crash.
constexpr bool may_arm(bool crashed) { return !crashed; }

// True: arm the hold again for kLoadHoldMs. False: release it: this machine plays normally, its frames pump the
// network, and from now on a stall (a hang, a crash) must look like silence to the peers.
// The last load step is not the end of a load: the first frames after it (precache, the first update of every
// script, a client's role swap until it is live) can each stall longer than the silence timeout. Live check
// 2026-10-23: a GAMMA client was dropped by the host there, after its hold was released by its first frame.
inline bool keep_held(const FrameState& s)
{
	return may_arm(s.crashed) && (s.load_steps_queued || s.precache_frames || s.joining_world);
}

// The hold of one transport (net::Transport) as one atomic word: 0 = released, kSealed = released for good (this
// process crashed), anything else = the steady-clock ms until which the link is held. Review of protocol 8: the frame
// pump and the load stages read the crash flag and then armed the hold, and a crash on another thread in between left
// it armed for kLoadHoldMs. Here the check and the arm are one compare-exchange: an arm either lands before the seal
// (which wipes it) or sees the seal and does nothing, whatever its caller read before. Any thread.
class HoldWord
{
public:
	static constexpr uint64_t kSealed = UINT64_MAX;

	// Holds until `until_ms` (0 releases). false: sealed, nothing changed.
	bool arm(uint64_t until_ms)
	{
		if (until_ms == kSealed)
			until_ms = kSealed - 1; // a deadline, never the seal
		uint64_t cur = m_word.load();
		do
		{
			if (cur == kSealed)
				return false;
		} while (!m_word.compare_exchange_weak(cur, until_ms));
		return true;
	}
	// Released, unless sealed (a seal stays: the transport may start again, a crashed process holds nothing).
	void release() { arm(0); }
	// A crash: released at once and for good.
	void seal() { m_word.store(kSealed); }
	bool sealed() const { return m_word.load() == kSealed; }
	bool held(uint64_t now_ms) const
	{
		const uint64_t w = m_word.load();
		return w != 0 && w != kSealed && now_ms < w;
	}

private:
	std::atomic<uint64_t> m_word{0};
};

// Host: a JoinRequest that comes while the host's own level is coming up waits for it (coop_join.cpp precache_wait):
// a world save then would stall those frames further. The engine's precache (Device.dwPrecacheFrame) is no end signal
// on its own: it counts down only in the focused window (xrEngine/device.cpp End) and starts again on a device reset
// (Device_destroy.cpp), so an unfocused host never served those joins and the client dropped after 5 min (review of
// protocol 8). The wait ends with the precache or kLimitMs after the first frame of it the join code saw (load steps
// run without frames, so that is after the level is up). Main thread; any monotonic ms clock.
class PrecacheWait
{
public:
	static constexpr uint64_t kLimitMs = 20000;

	// True: joins wait. `precaching`: the host's level precaches now.
	bool waiting(bool precaching, uint64_t now_ms)
	{
		if (!precaching)
		{
			m_on = false;
			return false;
		}
		if (!m_on)
		{
			m_on = true;
			m_since = now_ms;
			m_gave_up = m_told = false;
		}
		if (now_ms < m_since + kLimitMs) // a clock that went back keeps waiting: the limit still holds from m_since
			return true;
		m_gave_up = true;
		return false;
	}
	// True once per precache that outlasted kLimitMs (the log says so once).
	bool take_gave_up()
	{
		if (!m_on || !m_gave_up || m_told)
			return false;
		m_told = true;
		return true;
	}

private:
	bool m_on = false;
	bool m_gave_up = false;
	bool m_told = false;
	uint64_t m_since = 0;
};
} // namespace link_hold
} // namespace coop
