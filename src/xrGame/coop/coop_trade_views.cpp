// Anomaly Coop — trade: the windows on shared containers and traders, and the partner windows. See coop_trade.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_trade_impl.h"
#include "coop_join.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_items.h"
#include "coop_script.h"
#include "coop_sync.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_partner_rules.h"

#include "Actor.h"
#include "Inventory.h"
#include "InventoryBox.h"
#include "inventory_item.h"
#include "UIGameCustom.h"
#include "UIGameSP.h" // the talk window
#include "ui/UIActorMenu.h"
#include "ui/UI3tButton.h"
#include "ui/UITalkWnd.h"

namespace coop
{
namespace trade
{
namespace
{
constexpr u64 kMoveCheckMs = 2500; // container move not applied after this -> rebuild the window
constexpr u64 kRefreshMinMs = 150; // at most one list rebuild per this period (take-all bursts)

u8 g_search_kind = ContNone;

// Order-independent fingerprint of a container's content.
u64 container_signature(CInventoryOwner* owner, CInventoryBox* box)
{
	u64 sum = 0, mix = 0, n = 0;
	auto add = [&](u16 id)
	{
		const u64 h = (u64(id) + 1) * 0x9E3779B97F4A7C15ull;
		sum += h;
		mix ^= (h >> 29) | (h << 35);
		++n;
	};
	if (owner)
	{
		for (PIItem it : owner->inventory().m_all)
			if (it)
				add(it->object_id());
	}
	else if (box)
	{
		for (u16 id : box->m_items)
			add(id);
	}
	return sum ^ (mix * 31) ^ (n << 48);
}

// Index of the selected sort tab (the disabled button), -1 if none.
int selected_tab(CUIActorMenu& menu)
{
	for (u32 i = 0; i < menu.m_sort_buttons.size(); ++i)
		if (menu.m_sort_buttons[i] && !menu.m_sort_buttons[i]->IsEnabled())
			return int(i);
	return -1;
}

void send_container(bool open, u8 kind, u16 local_id)
{
	if (mode().leads_level())
	{
		if (open)
		{
			OpenView& v = g_open[kHostPeer];
			v.kind = kind;
			v.id = local_id;
		}
		else
			g_open.erase(kHostPeer);
		return;
	}
	if (!is_live_mirror())
		return;
	Writer w(6);
	w.u8(open ? 1 : 0);
	w.u8(kind);
	w.u16(join::to_host(local_id));
	net::client_send(Msg::ReqContainer, w);
}
} // namespace

xr_map<PeerId, OpenView> g_open;

ViewState g_view;

std::atomic<u32> g_search_now{kNoId};
u16 g_search_sent = kNoId;
u64 g_rebuilds = 0, g_moves_failed = 0;

void request_rebuild(u64 at)
{
	if (!g_view.rebuild_at || at < g_view.rebuild_at)
		g_view.rebuild_at = at;
}

void host_on_container(PeerId from, Reader& r)
{
	const bool open = r.u8() != 0;
	const u8 kind = r.u8();
	const u16 id = r.u16();
	if (!r.ok() || from == kHostPeer)
		return;
	if (!open)
	{
		g_open.erase(from);
		permits().revoke(from); // any close clears the player's open view
		return;
	}
	const auto was = g_open.find(from);
	const bool same_view = was != g_open.end() && was->second.kind == kind && was->second.id == id;
	OpenView& v = g_open[from];
	v.kind = kind;
	v.id = id;
	if (!same_view)
	{
		// A new window: his computer permit follows it (the decider's grant for this trader, else none)
		if (kind == ContTrader && !sync::host_peer_joining(from))
			host_take_permit(from, id);
		else
			permits().revoke(from);
	}
	const bool loot = kind == ContBox || kind == ContCorpse;
	// May he see it: asked once for the contents a new view gets and for the loot hook (a box across the level is no window on it)
	const bool visible = (!same_view || loot) && !sync::host_peer_joining(from) && may_see_contents(from, kind, id);
	if (!same_view && visible)
		items::host_send_contents(from, id); // what his copy of it shows may be the spawn values
	if (loot && visible)
		script::on_container_opened(from, id); // the host's Lua hears of it (coop_on_container_opened)
	if (kind == ContTrader)
		host_send_trader_money(from, id); // the client's copy of the trader's money may be old
}

void search_frame()
{
	const u32 said = g_search_now.load();
	u16 now = u16(said & 0xffff);
	const u8 asked = u8(said >> 16);
	if (now == kNoId && g_search_sent == kNoId)
		return;
	u8 kind = ContNone;
	if (now != kNoId)
	{
		CObject* o = find_object(now);
		CInventoryOwner* owner = o ? smart_cast<CInventoryOwner*>(o) : nullptr;
		const bool box = o && smart_cast<CInventoryBox*>(o);
		kind = search_kind(o != nullptr, box, owner != nullptr, owner && alive(owner), asked); // a trader of a trade window
		if (kind == ContNone)
			now = kNoId; // a companion's bag, or the box went
	}
	const SearchStep step = search_step_kind(g_search_sent, g_search_kind, now, kind, kNoId);
	if (step.close)
		send_container(false, g_search_kind, g_search_sent);
	if (step.open)
		send_container(true, kind, now);
	g_search_sent = now;
	g_search_kind = kind;
}

int lua_search_window(lua_State* L)
{
	const lua_Number id = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : -1;
	const u16 now = id >= 0 && id < lua_Number(kNoId) ? u16(id) : kNoId;
	// Kind 3: a trade window with that trader (the decider sends his money, his items' states and a computer's permit)
	const u32 kind = lua_isnumber(L, 2) && lua_tonumber(L, 2) == lua_Number(kViewTrader) ? u32(kViewTrader) : u32(kViewNone);
	g_search_now.store(now == kNoId ? u32(kNoId) : (u32(now) | (kind << 16)));
	return 0;
}

void on_actor_menu_shown(CUIActorMenu& menu, bool shown)
{
	if (!net::live())
	{
		g_view = ViewState();
		return;
	}
	if (!shown)
	{
		if (g_view.shown && g_view.kind != ContNone)
			send_container(false, g_view.kind, g_view.id);
		g_view = ViewState();
		return;
	}
	g_view = ViewState();
	g_view.shown = true;
	CInventoryOwner* partner = menu.GetPartner();
	CInventoryBox* box = menu.GetInvBox();
	switch (menu.GetMenuMode())
	{
	case mmTrade:
		g_view.kind = partner ? ContTrader : ContNone;
		g_view.id = id_of(partner);
		if (partner)
			client_take_window(partner); // a trading computer's grant
		break;
	case mmDeadBodySearch:
		g_view.kind = partner ? ContCorpse : (box ? ContBox : ContNone);
		g_view.id = partner ? id_of(partner) : (box ? box->ID() : kNoId);
		g_view.sig = container_signature(partner, box);
		break;
	default:
		break;
	}
	if (g_view.kind != ContNone)
		send_container(true, g_view.kind, g_view.id);
}

bool refresh_actor_menu(CUIActorMenu& menu)
{
	if (!g_view.shown || !net::live() || !menu.IsShown())
		return false;
	const EMenuMode mode = menu.GetMenuMode();
	if (mode != mmTrade && mode != mmDeadBodySearch)
		return false;
	const u64 now = mgr().now_ms();

	bool rebuild = g_view.rebuild_at && now >= g_view.rebuild_at;
	if (mode == mmDeadBodySearch)
	{
		// The content changed (another player, a host answer): rebuild from the real inventory.
		const u64 sig = container_signature(menu.GetPartner(), menu.GetInvBox());
		if (sig != g_view.sig)
		{
			g_view.sig = sig;
			rebuild = true;
		}
	}
	for (auto it = g_view.moves.begin(); it != g_view.moves.end();)
	{
		if (now < it->check_at)
		{
			++it;
			continue;
		}
		if (local_parent(it->item) != it->to)
		{
			++g_moves_failed; // refused, overtaken or destroyed meanwhile: the cells are stale
			rebuild = true;
		}
		it = g_view.moves.erase(it);
	}

	const bool labels = g_view.labels;
	g_view.labels = false;
	if (rebuild)
	{
		if (now - g_view.last_rebuild < kRefreshMinMs)
		{
			request_rebuild(g_view.last_rebuild + kRefreshMinMs); // burst: once more, a bit later
			return labels;
		}
		const int tab = selected_tab(menu);
		if (tab < 0)
			return labels;
		g_view.rebuild_at = 0;
		g_view.last_rebuild = now;
		++g_rebuilds;
		menu.SelectInventoryTab(tab); // public: refilters the actor bag and the container/trader lists
	}
	return rebuild || labels;
}

void client_on_item_state(u16 holder)
{
	if (g_view.shown && g_view.id == holder)
		request_rebuild(mgr().now_ms() + kRefreshMinMs); // a window's contents come in a burst
}

void on_container_move(u16 from_id, u16 to_id, u16 item_id)
{
	if (!net::live() || !g_view.shown)
		return;
	(void)from_id;
	if (g_view.moves.size() >= 512)
		g_view.moves.erase(g_view.moves.begin());
	g_view.moves.push_back({item_id, to_id, mgr().now_ms() + kMoveCheckMs});
}

void on_inv_box_destroy(CInventoryBox& box)
{
	CUIGameCustom* ui = CurrentGameUI();
	if (!ui)
		return;
	CUIActorMenu& menu = ui->GetActorMenu();
	if (menu.GetInvBox() != &box)
		return;
	if (menu.IsShown())
	{
		// On a client the host released the box (an emptied death bag) or a script did, with the window
		// open. Show(false) runs inside (CDialogHolder::RemoveDialogToRender): our close goes to the host, the menu's
		// DeInit clears the box's in_use - all while the box is still whole.
		ui->HideActorMenu();
		::Msg("[coop] box %u goes away while it is searched: its window is closed", u32(box.ID()));
	}
	if (menu.IsShown())
	{
		// Cannot happen while the menu has its holder; SetInvBox would assert on a shown menu.
		::Msg("! [coop] box %u goes away and its window would not close: the menu still points at it", u32(box.ID()));
		return;
	}
	menu.SetInvBox(nullptr); // a hidden menu keeps the pointer until the next search: never a freed box
}

namespace
{
CUITalkWnd* talk_window(CUIGameCustom* ui)
{
	CUIGameSP* sp = ui ? smart_cast<CUIGameSP*>(ui) : nullptr;
	return sp ? sp->TalkMenu : nullptr;
}

// What this machine's windows show of `owner` (xrCoop/coop_partner_rules.h). Without a UI (a server's process) only
// the actor's talk.
partner_rules::Windows partner_windows(CUIGameCustom* ui, CInventoryOwner& owner)
{
	partner_rules::Windows w;
	if (ui)
	{
		CUIActorMenu& menu = ui->GetActorMenu();
		w.menu_shown = menu.IsShown();
		switch (menu.GetMenuMode())
		{
		case mmTrade: w.menu = partner_rules::Menu::Trade; break;
		case mmUpgrade: w.menu = partner_rules::Menu::Upgrade; break;
		case mmDeadBodySearch: w.menu = partner_rules::Menu::Search; break;
		default: w.menu = partner_rules::Menu::Other; break;
		}
		w.menu_partner = menu.GetPartner() == &owner;
		if (CUITalkWnd* talk = talk_window(ui))
		{
			w.talk_shown = talk->IsShown();
			w.talk_partner = talk->OthersInvOwner() == &owner;
		}
	}
	CActor* a = g_actor ? Actor() : nullptr;
	w.actor_talks = a && a->IsTalking() && a->GetTalkPartner() == &owner;
	return w;
}

// Does `c` to the windows that show `owner`, in the rules' order: the menu (a trade or an upgrade opened from a talk
// lies over the talk window), the menu's memory of it, the actor's talk, the talk window. `why` follows "<id> (<section>)" in the log.
void close_partner_windows(CUIGameCustom* ui, CInventoryOwner& owner, const partner_rules::Closes& c, const char* why)
{
	if (!c.hide_menu && !c.forget_menu_partner && !c.stop_talk && !c.hide_talk)
		return; // every NPC that goes comes here: almost always nobody shows it
	CGameObject* go = smart_cast<CGameObject*>(&owner);
	const u32 id = go ? u32(go->ID()) : u32(kNoId);
	const char* name = go ? go->cName().c_str() : "?";
	if (ui && c.hide_menu)
	{
		// Show(false) runs inside (CDialogHolder::RemoveDialogToRender): our close goes to the decider, DeInit*Mode stops the
		// partner's trading (StopTrading), Lua's hide hook runs - all while the partner is still whole
		ui->HideActorMenu();
		::Msg("[coop] %u (%s) %s: the window that shows it is closed", id, name, why);
	}
	if (ui && c.forget_menu_partner)
	{
		CUIActorMenu& menu = ui->GetActorMenu();
		if (menu.IsShown()) // cannot happen while the menu has its holder; SetPartner would assert on a shown menu
			::Msg("! [coop] %u (%s) %s and its window would not close: the menu still points at it", id, name, why);
		else
			menu.SetPartner(nullptr); // a hidden menu keeps the pointer until the next trade: never a freed partner
	}
	if (c.stop_talk)
	{
		if (CActor* a = g_actor ? Actor() : nullptr)
			a->StopTalk(); // the actor's UpdateInventoryOwner reads its talk partner every frame
	}
	CUITalkWnd* talk = (ui && c.hide_talk) ? talk_window(ui) : nullptr;
	if (talk)
		talk->HideDialog(); // Show(false) runs at once: its StopSnd hands the partner to OnDialogSoundHandlerStop while it is whole
	if (c.stop_talk || c.hide_talk)
		::Msg("[coop] %u (%s) %s: the talk with it ends", id, name, why);
	if (talk && talk->IsShown())
		::Msg("! [coop] %u (%s) %s and the talk window would not close: it still points at it", id, name, why);
}
} // namespace

void on_inv_owner_destroy(CInventoryOwner& owner)
{
	CUIGameCustom* ui = CurrentGameUI();
	close_partner_windows(ui, owner, partner_rules::on_destroy(partner_windows(ui, owner)), "goes away while a window shows it");
}

bool on_talk_lock_revoked(CInventoryOwner& partner)
{
	CUIGameCustom* ui = CurrentGameUI();
	const partner_rules::Closes c = partner_rules::on_lock_revoked(partner_windows(ui, partner));
	close_partner_windows(ui, partner, c, "is no longer ours to talk to (the talk lock was taken back)");
	return c.hide_menu || c.stop_talk;
}

void invalidate_views()
{
	if (g_view.shown)
		request_rebuild(mgr().now_ms());
}

bool host_container_open(u16 id)
{
	if (!mode().leads_level() || id == kNoId)
		return false;
	for (const auto& it : g_open)
		if (it.second.kind != ContNone && it.second.id == id)
			return true;
	return false;
}

// For CUIActorMenu::CheckDistance (the stock window closes 3 m from the trader). True: the trade window with `partner` stays open
// because this machine holds the grant for that trader through a trading computer, it has not run out and the actor stands within
// the stock 3 m of the computer (xrCoop/coop_trade_permit.h window_keeps). False: the stock rule.
bool keeps_trade_window(CInventoryOwner* partner)
{
	if (!partner || !g_view.shown || g_view.kind != ContTrader || g_view.id != id_of(partner))
		return false;
	const float to_computer = distance_to(Actor(), find_object(g_view.remote.computer));
	return trade_permit::window_keeps(g_view.remote, g_view.id, mgr().now_ms(), to_computer);
}
} // namespace trade
} // namespace coop
