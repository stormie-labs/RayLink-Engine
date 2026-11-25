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

// TZ R90: what the window shows of an object (ReqContainer kinds, coop_trade.cpp ContainerKind). A trade window says so
// (coop.search_window(id, kViewTrader)); a box or a body is what the object is; the bag of a living companion is nobody's business.
constexpr uint8_t kViewNone = 0;
constexpr uint8_t kViewBox = 1;
constexpr uint8_t kViewCorpse = 2;
constexpr uint8_t kViewTrader = 3;

// `exists`: the object is here; `box`: an inventory box; `owner`: an inventory owner, `alive` it lives; `asked`: what the script said
// the window shows (kViewTrader for a trade window, else kViewNone). A trade window shows a living trader only: a trader that died
// under it is no search of his body (no coop_on_container_opened for a body nobody opened).
inline uint8_t search_kind(bool exists, bool box, bool owner, bool alive, uint8_t asked)
{
	if (!exists)
		return kViewNone;
	if (asked == kViewTrader)
		return owner && alive && !box ? kViewTrader : kViewNone;
	if (box)
		return kViewBox;
	if (owner && !alive)
		return kViewCorpse;
	return kViewNone;
}

// search_step with the kind: the same id shown another way (a body searched, then traded with) is another window.
inline SearchStep search_step_kind(uint16_t sent, uint8_t sent_kind, uint16_t now, uint8_t now_kind, uint16_t none)
{
	SearchStep s = search_step(sent, now, none);
	if (!s.open && !s.close && now != none && sent_kind != now_kind)
		s.open = true;
	return s;
}
} // namespace trade
} // namespace coop
