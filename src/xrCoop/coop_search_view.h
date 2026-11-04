// Anomaly Coop - the search window of a player when it is Anomaly's Lua inventory (coop_trade.cpp). ui_inventory takes every search, the
// engine's window never opens, so CUIActorMenu::Show never told the host (ReqContainer): no contents for the player's copy, no
// coop_on_container_opened, a death bag freed under an open window. The script says what the window shows (GUI_on_show, GUI_on_hide, with a
// delay of FDDA's search animation or from a mod's ui_inventory.start), a frame compares that with what the host was told.
#pragma once

#include <cstdint>

namespace coop
{
namespace trade
{
struct SearchStep
{
	bool close = false;
	bool open = false;
};

// `sent`: the id the host was told of, `now`: what the window shows, `none`: no window. Another window takes the place of the last on the
// host (one view per player), so it needs no close first.
inline SearchStep search_step(uint16_t sent, uint16_t now, uint16_t none)
{
	SearchStep s;
	if (sent == now)
		return s;
	s.close = now == none;
	s.open = now != none;
	return s;
}
} // namespace trade
} // namespace coop
