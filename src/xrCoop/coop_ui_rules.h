// Anomaly Coop — what the UI and the input keep across a level. A session runs the world behind every window and moves the
// group whatever a player has open, so a level goes down under windows a single-player level change never had.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstdint>

namespace coop
{
namespace ui_rules
{
// The UI cursor is one object of the process. A level's dialogs die with it without closing (CUIGameSP::HideShownDialogs
// closes the inventory and a talk only: never a PDA, a script's window or a message box), so the "cursor on" of the last one
// stayed on over the next level (live 2026-10-31). A main menu that is open is the one thing it still points at.
inline bool cursor_outlives_level(bool main_menu_open) { return main_menu_open; }

// The main menu closing: the cursor comes back for a dialog of the level that still wants it. The level it was shown for may
// be gone by now (a load runs "main_menu off"), and a stale one has nothing to come back for.
inline bool cursor_back_after_menu(bool shown_at_open, bool top_dialog_needs_it) { return shown_at_open && top_dialog_needs_it; }

// Device.seqRender runs from high to low, the last one drawn is on top. A loading screen a group holds stays registered at 0
// (CLoadScreenRenderer), so a menu opened with Esc lay under it: the player saw nothing, his clicks went to buttons he could
// not see, and the menu was simply there when the screen came off (live 2026-10-31: "pressed Esc and fell to the main menu").
constexpr int kMenuRenderPriority = 4; // the stock place: 1 console, 3 a combo box's list
constexpr int kLoadScreenRenderPriority = 0;
constexpr int kMenuOverLoadScreen = -1; // above the loading screen, under a tutorial (-2) and the cursor
constexpr int kCursorRenderPriority = -3;

inline int menu_render_priority(bool load_screen_on) { return load_screen_on ? kMenuOverLoadScreen : kMenuRenderPriority; }

// The held screen takes the level's input and gives back what a script had set. That word belongs to the world it was said
// in: the flag is one global, the script that would clear it dies with its Lua state, and the new actor's net_spawn has
// already cleared it. Given back into a new world it left a player with Esc as the only key that works.
inline bool input_off_after_hold(bool off_before, uint32_t world_before, uint32_t world_now)
{
	return off_before && world_before == world_now;
}
} // namespace ui_rules
} // namespace coop
