// Anomaly Coop: the game UI is never laid out on a degenerate window size (owner 2026-11-08: "the minimap sometimes sits
// top-left, over the party panel"). Pure functions, no engine dependencies (the unit-test exe includes this file as it
// is). Rule table: tests/test_ui_size_policy.cpp.
//
// How a UI file is chosen: ui_core::get_xml_name adds a suffix by layout_family(Device.dwWidth, Device.dwHeight) at the
// moment the file is loaded: 0 -> file.xml (aspect up to 1.343: 4:3, 5:4), 1 -> file_16.xml, 2 -> file_21.xml (aspect
// over 1.8; a missing _21 falls back to _16). The game UI (HUD, minimap) is built on level load and rebuilt by
// CHUDManager::OnScreenResolutionChanged when CRenderDevice::Reset changes the device size.
//
// Where a size comes from (CHW::selectResolution): borderless and fullscreen take vid_mode; a window (rs_screenmode
// windowed) takes its client rectangle, which is 0x0 while it is minimised. A reset in that state (vid_restart from a
// settings menu, a driver's DXGI_ERROR_DEVICE_RESET) made the device 0x0, the HUD was rebuilt on it, layout_family
// fell to 0 (zone_map.xml, top-left on G.A.M.M.A.) and nothing reset it again on the restore. Now such a reset waits
// (reset_action), and the frame loop runs it once the window is back (resume_deferred_reset).
#pragma once

#include <cstdint>

namespace ui_size_policy
{
// rs_screenmode values (xr_ioc_cmd.cpp screen_mode_tokens)
constexpr std::uint32_t kWindowed = 0;
constexpr std::uint32_t kBorderless = 1;
constexpr std::uint32_t kFullscreen = 2;

// The smallest size the UI is laid out on. A minimised window's client rectangle is 0x0 (Windows parks the window
// itself at about 160x28); a game window is far larger than this.
constexpr std::uint32_t kMinWidth = 320;
constexpr std::uint32_t kMinHeight = 240;

inline bool usable(std::uint32_t w, std::uint32_t h)
{
	return w >= kMinWidth && h >= kMinHeight;
}

// ui_core::screenmode(): the stock thresholds and the stock float arithmetic (1024/768 + 0.01 = 1.34333).
// A 0-high size has no aspect: the stock code divided by zero (0x0 gave NaN -> 0); here it is 0 without the division.
inline std::uint8_t layout_family(std::uint32_t w, std::uint32_t h)
{
	if (h == 0)
		return 0;
	const float aspect = float(w) / float(h);
	if (aspect > 1.8f)
		return 2;
	if (aspect > (1024.0f / 768.0f + 0.01f))
		return 1;
	return 0;
}

// The file suffix of a family, for the log.
inline const char* layout_suffix(std::uint8_t family)
{
	switch (family)
	{
	case 0: return "";
	case 1: return "_16";
	case 2: return "_21";
	default: return "?";
	}
}

enum class ResetAction
{
	run,   // reset now
	defer, // the window gives no size to reset to: keep the device (and the UI) as they are, reset on the restore
};

// CRenderDevice::Reset. Only a window reads its client rectangle; borderless and fullscreen take vid_mode (and
// CHW::Reset shows a minimised fullscreen window itself).
inline ResetAction reset_action(std::uint32_t rs_screenmode, bool iconic, std::uint32_t client_w, std::uint32_t client_h)
{
	if (rs_screenmode != kWindowed)
		return ResetAction::run;
	if (iconic || !usable(client_w, client_h))
		return ResetAction::defer;
	return ResetAction::run;
}

// The frame loop: run a deferred reset as soon as it would no longer be deferred.
inline bool resume_deferred_reset(bool deferred, std::uint32_t rs_screenmode, bool iconic, std::uint32_t client_w,
	std::uint32_t client_h)
{
	return deferred && reset_action(rs_screenmode, iconic, client_w, client_h) == ResetAction::run;
}

// CHUDManager::OnScreenResolutionChanged (also level.reload_hud_xml and a language change): the game UI is rebuilt
// only on a usable device size; the next size change rebuilds it.
inline bool hud_rebuild_allowed(std::uint32_t w, std::uint32_t h)
{
	return usable(w, h);
}
} // namespace ui_size_policy
