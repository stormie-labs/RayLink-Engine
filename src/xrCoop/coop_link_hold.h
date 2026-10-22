// Anomaly Coop — W9 link hold: when the frame pump keeps the session link held (the transport's keep-alive thread
// answers the peers for a main thread that is busy loading) and when it lets the link go.
// Pure C++17, header-only, no engine dependencies: used by coop::Manager::frame and by the unit-test exe.
#pragma once

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
} // namespace link_hold
} // namespace coop
