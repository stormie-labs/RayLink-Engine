// Anomaly Coop — a client's pause while the host is silent. In a session the clock refuses to stop (G18,
// CGamePersistent::CanBePaused), so the device reads as not paused while the sound is: the end follows what was taken here,
// not what the device says. Going by the device left every sound then playing paused for good (live 2026-10-31).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

namespace coop
{
struct SilencePause
{
	bool waiting = false;
	bool taken = false;

	// True when the caller has to pause the device now.
	bool begin(bool device_paused)
	{
		if (waiting) return false;
		waiting = true;
		taken = !device_paused;
		return taken;
	}

	// True when the caller has to lift the pause now.
	bool end()
	{
		const bool give_back = taken;
		waiting = taken = false;
		return give_back;
	}
};
}
