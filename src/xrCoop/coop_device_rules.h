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

inline bool prefer_matching_startup_monitor(bool exclusive_fullscreen, bool cursor_matches_mode, unsigned matching_monitors)
{
	return exclusive_fullscreen && !cursor_matches_mode && matching_monitors == 1;
}

// vid_monitor Auto before the device exists (xrEngine InitMonitor -> ResolveAutoStartupMonitor). Stock Auto is the display
// under the cursor. The owner's start (2026-11-21 20:46): fullscreen 2560x1440 saved, the cursor on a portrait display
// without that mode, DXGI fell back to its first mode and the world loaded at 800x1280. In exclusive fullscreen the one
// display whose desktop is the saved size is taken when the cursor's display cannot show that size; none or several keep
// the cursor's, and so do borderless and windowed. The cursor's display shows the size when its desktop is that size or
// its mode list has it: 1920x1080 fullscreen on a 2560x1440 desktop is an ordinary choice, and a 1080p second display
// must not take the game then (gate R38, 2026-11-22).
struct startup_display
{
	unsigned desktop_width;
	unsigned desktop_height;
	bool lists_mode; // the display's own mode list has the saved size (read for the cursor's display)
};

// Index into `displays` of the display to start on; -1 keeps the cursor's display.
inline int auto_startup_display(bool exclusive_fullscreen, unsigned width, unsigned height, const startup_display& cursor,
                                const startup_display* displays, unsigned count)
{
	if (width == 0 || height == 0)
		return -1;
	const bool cursor_shows_mode = cursor.lists_mode || (cursor.desktop_width == width && cursor.desktop_height == height);
	int match = -1;
	unsigned matches = 0;
	for (unsigned i = 0; i < count; ++i)
	{
		if (displays[i].desktop_width == width && displays[i].desktop_height == height)
		{
			match = static_cast<int>(i);
			++matches;
		}
	}
	return prefer_matching_startup_monitor(exclusive_fullscreen, cursor_shows_mode, matches) ? match : -1;
}

inline bool defer_monitor_change(bool level_present, bool level_can_pause)
{
	return level_present && !level_can_pause;
}
} // namespace device
} // namespace coop
