// Anomaly Coop — the pause stock puts on a single player level that finished loading in a window without the focus
// (xrEngine/device.cpp, "device pause on: application start"; it holds until the window is activated).
// b57j stand, sp_smoke (2026-11-21): the new game loaded behind another window and stood paused for 2.5 minutes with
// g_always_active on. Always-active keeps a window without the focus running everywhere else (CRenderDevice::OnWM_Activate:
// not in fullscreen, g_screenmode 2), and now at the start too, with the same exception.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe (tests/test_device_rules.cpp).
#pragma once

namespace coop
{
namespace device
{
inline bool pauses_at_start(bool single_player, bool window_has_focus, bool always_active, int screen_mode)
{
	if (!single_player || window_has_focus)
		return false;
	return !(always_active && screen_mode != 2);
}
} // namespace device
} // namespace coop
