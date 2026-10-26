// Anomaly Coop: free cursor in windowed menus (owner, 2026-10-23)
// When does the game lock the mouse cursor inside its window? One pure function, no engine dependencies (the unit-test
// exe includes this file as it is): CRenderDevice::UpdateCursorClip feeds it the current state and turns the answer
// into ClipCursor calls. Rule table: tests/test_cursor_policy.cpp.
//
//   mode 0 (g_free_cursor 0)  stock: clipped for as long as the window is active.
//   mode 1 (g_free_cursor 1)  default:
//     - exclusive fullscreen (screenmode 2): as stock;
//     - windowed (0) and borderless (1): clipped only while the window is active AND no UI cursor is shown, so the
//       cursor leaves the window in the main menu, the inventory, the PDA, dialogs and on pause, and stays locked in
//       gameplay (mouse look: without the lock the cursor drifts over the edge and a click leaves the game).
//   Any other mode value behaves like 1, any screenmode other than 2 counts as a window.
#pragma once

#include <cstdint>

inline bool cursor_should_clip(bool window_active, std::uint32_t screenmode, bool ui_cursor_visible, std::uint32_t mode)
{
	if (!window_active)
		return false; // alt-tab, another application in front: never hold the cursor
	if (mode == 0)
		return true; // stock behaviour
	if (screenmode == 2)
		return true; // exclusive fullscreen keeps the stock lock whatever the UI shows
	return !ui_cursor_visible; // windowed / borderless: lock in gameplay, free while a UI cursor is shown
}
