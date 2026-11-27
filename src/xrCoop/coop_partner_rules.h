// Anomaly Coop — which windows let go of an inventory owner, and when. The talk window (CUITalkWnd::m_pOthersInvOwner), the
// actor menu (CUIActorMenu::m_pPartnerInvOwner)
// and the actor's talk (CInventoryOwner::m_pTalkPartner) keep raw pointers to the NPC, the trader or the corpse they show, and read
// them every frame and on their close. Stock closes them when the level stops (CLevel::net_Stop, HideShownDialogs), but in a session
// a partner can go first: its decider releases it or takes it offline, or this machine's world is replaced. The windows let go of it
// on its net_Destroy, while it is still whole.
// Pure C++17, header-only, no engine dependencies: used by the engine (xrGame/coop/coop_trade.cpp) and by the unit-test exe.
#pragma once

#include <cstdint>

namespace coop
{
namespace partner_rules
{
// The mode of the actor menu (CUIActorMenu EMenuMode), as far as these rules care.
enum class Menu : uint8_t
{
	Other, // undefined (a hidden menu), the player's own inventory
	Trade,
	Upgrade,
	Search, // a corpse; a box has its own hook (coop::trade::on_inv_box_destroy)
};

// What the windows of this machine show of one inventory owner.
struct Windows
{
	bool menu_shown = false; // the actor menu is open
	Menu menu = Menu::Other; // in this mode
	bool menu_partner = false; // its partner (CUIActorMenu::GetPartner) is the owner, open or not
	bool talk_shown = false; // the talk window is open
	bool talk_partner = false; // its partner (CUITalkWnd::OthersInvOwner) is the owner
	bool actor_talks = false; // the actor talks to the owner (IsTalking, GetTalkPartner)
};

// What to do, in this order: the menu first (a trade or an upgrade opened from a talk lies over the talk window), then the talk.
struct Closes
{
	bool hide_menu = false; // CUIGameCustom::HideActorMenu: Show(false) runs at once, then Lua's hide hook
	bool forget_menu_partner = false; // CUIActorMenu::SetPartner(nullptr), on the menu hidden by then
	bool stop_talk = false; // CActor::StopTalk: the actor's pointer to its partner goes
	bool hide_talk = false; // CUITalkWnd::HideDialog: Show(false) runs at once (its StopSnd reads the partner)
};

// The owner is being destroyed (CGameObject::net_Destroy, first line): every window that points at it lets go now, whatever the
// mode of the menu (a trader, a mechanic, a corpse). A window of another partner stays.
constexpr Closes on_destroy(const Windows& w)
{
	Closes c{};
	c.hide_menu = w.menu_shown && w.menu_partner;
	c.forget_menu_partner = w.menu_partner;
	c.stop_talk = w.actor_talks;
	c.hide_talk = w.talk_shown && w.talk_partner;
	return c;
}

// The decider took back this client's talk lock on the owner (coop_dialog client_on_revoke): the talk stops (the talk window hides
// itself on its next update, as before) and a trade or an upgrade with that NPC, opened from the talk, closes with it. The owner
// stays whole: nothing forgets it.
constexpr Closes on_lock_revoked(const Windows& w)
{
	Closes c{};
	c.hide_menu = w.menu_shown && w.menu_partner && (w.menu == Menu::Trade || w.menu == Menu::Upgrade);
	c.stop_talk = w.actor_talks;
	return c;
}
} // namespace partner_rules
} // namespace coop
