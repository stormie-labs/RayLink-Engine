// Anomaly Coop — trade with NPCs and shared containers. See coop_trade.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_trade.h"
#include "coop_dialog.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_requests.h"
#include "coop_script.h"
#include "coop_money.h"
#include "coop_items.h"
#include "xrCoop/coop_talks.h" // R4 2: may_trade_with
#include "xrCoop/coop_search_view.h"
#include "xrCoop/coop_trade_window_rules.h" // TZ R90: the Lua trade window of a player's copy
#include "xrCoop/coop_reconcile.h"
#include "xrCoop/coop_trade_permit.h" // G6-3: remote trade through a trading computer

#include "Level.h"
#include "Actor.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryBox.h"
#include "entity_alive.h"
#include "trade.h"
#include "trade_parameters.h"
#include "eatable_item.h"
#include "ai/trader/ai_trader.h"
#include "ai/stalker/ai_stalker.h"
#include "ai_space.h"
#include "script_engine.h" // G6-3: the grants of coop_mods_trade_computer.script
#include "alife_simulator.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <string>
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "script_callback_ex.h"
#include "script_game_object.h"
#include "game_object_space.h"
#include "../xrServerEntities/xrMessages.h"
#include "UIGameCustom.h"
#include "UIGameSP.h" // O2-partner: the talk window
#include "ui/UITalkWnd.h"
#include "xrCoop/coop_partner_rules.h"
#include "ui/UIActorMenu.h"
#include "ui/UIDragDropListEx.h"
#include "ui/UICellItem.h"
#include "ui/UI3tButton.h"

namespace coop
{
namespace trade
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u16 kMaxDealItems = 1024;
constexpr u64 kMoveCheckMs = 2500; // container move not applied after this -> rebuild the window
constexpr u64 kRefreshMinMs = 150; // at most one list rebuild per this period (take-all bursts)
constexpr u64 kPendingWarnMs = 15000; // client: a deal without answer is logged
constexpr s64 kMaxMoney = 0xffffffffll;
using trade_window::kDealDonate; // TradeOp Deal flags (xrCoop/coop_trade_window_rules.h)
using trade_window::kDealLuaPrices; // TZ R90: the Lua window's deal; TZ R92: its shown sums are the price
// kTradeReach: coop_trade.h (F2 T5: also a mechanic's reach)

enum class Op : u8
{
	Request = 1, // retired: single-direction request (before the ledger); ignored
	Result = 2,
	TraderMoney = 3,
	Deal = 4,
};

enum TradeResult : u8
{
	Ok = 0,
	NotReady = 1, // the player is still joining
	NoPlayer = 2, // the player's proxy is not online on the host
	NoTrader = 3, // trader missing, dead, not a trader or not trading now
	ItemGone = 4, // an item is no longer where the client saw it
	NoMoney = 5, // the player cannot pay, after what the trader pays him
	TraderNoMoney = 6, // the trader cannot pay, after what the player pays him
	Malformed = 7,
	TooFar = 8, // the player's character is not by the trader on the host (R4 2)
	NotTraded = 9, // the trader does not buy / sell one of the items (R4 2)
	PriceChanged = 10, // TZ R92: the decider's formula and the window's sum differ beyond the tolerance, either way (settle_side)
};
static_assert(PriceChanged == trade_window::kPriceChanged, "xrCoop/coop_trade_window_rules.h, scripts\\coop_trade_window.script");

const char* result_name(u8 r)
{
	switch (r)
	{
	case Ok: return "ok";
	case NotReady: return "not ready";
	case NoPlayer: return "no player";
	case NoTrader: return "no trader";
	case ItemGone: return "item gone";
	case NoMoney: return "no money";
	case TraderNoMoney: return "trader has no money";
	case Malformed: return "malformed";
	case TooFar: return "too far from the trader";
	case NotTraded: return "item not traded";
	case PriceChanged: return "the price is not the one the window showed";
	default: return "?";
	}
}

enum ContainerKind : u8
{
	ContNone = 0,
	ContBox = 1,
	ContCorpse = 2,
	ContTrader = 3,
};

static_assert(ContBox == kViewBox && ContCorpse == kViewCorpse && ContTrader == kViewTrader, "xrCoop/coop_search_view.h kinds");

const char* container_name(u8 k)
{
	switch (k)
	{
	case ContBox: return "box";
	case ContCorpse: return "corpse";
	case ContTrader: return "trader";
	default: return "none";
	}
}

// ---------------------------------------------------------------- shared helpers

u64 now_ms() { return mgr().now_ms(); }

CObject* find_object(u16 id) { return (g_pGameLevel && id != kNoId) ? Level().Objects.net_Find(id) : nullptr; }

CInventoryOwner* find_owner(u16 id)
{
	CObject* o = find_object(id);
	return o ? smart_cast<CInventoryOwner*>(o) : nullptr;
}

bool alive(CInventoryOwner* io)
{
	CEntityAlive* e = io ? smart_cast<CEntityAlive*>(io) : nullptr;
	return e && e->g_Alive();
}

u16 id_of(CInventoryOwner* io)
{
	CGameObject* go = io ? smart_cast<CGameObject*>(io) : nullptr;
	return go ? go->ID() : kNoId;
}

bool within_reach(CInventoryOwner* a, CInventoryOwner* b, float reach)
{
	CGameObject* ga = a ? smart_cast<CGameObject*>(a) : nullptr;
	CGameObject* gb = b ? smart_cast<CGameObject*>(b) : nullptr;
	return ga && gb && ga->Position().distance_to(gb->Position()) <= reach;
}

// Host: parent of an entity in the server state (updated synchronously by every ownership event,
// unlike game objects, which follow one event round later).
u16 server_parent(u16 id)
{
	if (!ai().get_alife())
		return kNoId;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	return e ? e->ID_Parent : kNoId;
}

// Parent of a game object on this machine, kNoId if none or unknown.
u16 local_parent(u16 id)
{
	CObject* o = find_object(id);
	return (o && o->H_Parent()) ? o->H_Parent()->ID() : kNoId;
}

void move_item(u16 from_id, u16 to_id, u16 item_id)
{
	NET_Packet P;
	if (from_id != kNoId)
	{
		CGameObject::u_EventGen(P, GE_TRADE_SELL, from_id);
		P.w_u16(item_id);
		CGameObject::u_EventSend(P);
	}
	CGameObject::u_EventGen(P, GE_TRADE_BUY, to_id);
	P.w_u16(item_id);
	CGameObject::u_EventSend(P);
}

void write_trader_money(Writer& w, u16 trader_host_id, u32 money)
{
	w.u8(u8(Op::TraderMoney));
	w.u16(trader_host_id);
	w.u32(money);
}

int clamp_int(s64 v) { return v > INT_MAX ? INT_MAX : (v < -INT_MAX ? -INT_MAX : int(v)); }

u32 clamp_money(s64 v) { return v < 0 ? 0u : (v > kMaxMoney ? u32(kMaxMoney) : u32(v)); }

// ---------------------------------------------------------------- host state
struct LastResult
{
	bool have = false;
	u32 seq = 0;
	u16 trader = kNoId;
	u8 code = Ok;
	s32 delta = 0;
	u32 trader_money = 0;
	// TZ R90, R92: PriceChanged only, the decider's price of each side (for the player's log and text; nothing agrees to it)
	u32 decider_sell = 0;
	u32 decider_buy = 0;
};
struct PeerTrade
{
	LastResult last; // last answered deal (repeats, P6)
};
xr_map<PeerId, PeerTrade> g_peers; // host, per player
u32 g_host_world = 0; // join::world_generation() the answers belong to

// A new world (load, level change): the answers of the old one are void.
void host_sync_world()
{
	const u32 gen = join::world_generation();
	if (gen == g_host_world)
		return;
	g_host_world = gen;
	g_peers.clear();
}

struct Deal
{
	u32 seq = 0;
	u16 trader = kNoId;
	bool donate = false;
	bool lua_prices = false; // TZ R90: from Anomaly's Lua window; TZ R92: at the sums it showed (lua_price_deal)
	xr_vector<u16> sell; // player -> trader
	xr_vector<u16> buy; // trader -> player
	// What the deal moves, set by the decider (host_apply): the shown sums once the decider's formula agrees with them (TZ R92).
	u32 sell_total = 0; // the trader pays the player
	u32 buy_total = 0; // the player pays the trader
	// TZ R90 (protocol 35): what the player's window showed for each side; TZ R92: the deal's price, checked against the decider's
	// formula (settle_side). A Lua window sends them; for the engine window's deal they are the CTrade sums the decider computes for the
	// player's character, the engine window's own formula, which that window showed (host_apply, R4 2).
	u32 sell_shown = 0;
	u32 buy_shown = 0;
	// Review of 2026-11-25: the decider's formula's sums (lua_price_deal); TZ R92: for the log, and sent with a refusal for its price.
	u32 sell_decider = 0;
	u32 buy_decider = 0;
};

struct OpenView
{
	u8 kind = ContNone;
	u16 id = kNoId;
};
xr_map<PeerId, OpenView> g_open; // host: containers open per player (the host itself at kHostPeer)

u64 g_host_deals = 0, g_host_ok = 0, g_host_rejected = 0, g_host_items = 0;

// ---------------------------------------------------------------- G6-3: remote trade through a trading computer
// Review m23:3 (Gadgets for Hideout Furniture): the computer lists traders up to 150 m away and opens the stock trade. The
// decider's permits (xrCoop/coop_trade_permit.h), one per player, for the world they were issued in. Its Lua issues a grant
// (coop_mods_trade_computer.script, request mods.tcomp_permit: a real computer, the player at it, a trader it lists); the
// player's trade window with that trader opening here makes it his permit, its close or a new window drops it. Without one
// a deal keeps the stock rules (the talk and kTradeReach): remote trade with any NPC stays impossible.
trade_permit::Book g_permits;
u32 g_permit_world = 0; // join::world_generation() of the permits
u32 g_permit_errors = 0;
u64 g_permits_issued = 0, g_permits_dropped = 0, g_permit_deals = 0;

trade_permit::Book& permits()
{
	const u32 gen = join::world_generation();
	if (gen != g_permit_world)
	{
		g_permit_world = gen; // a new world: the computers and the traders of the old one are gone
		g_permits.clear();
	}
	return g_permits;
}

// Metres from a character (or the actor) to an object here; -1 when either is missing or going.
float distance_to(CGameObject* who, CObject* what)
{
	if (!who || !what || what->getDestroy())
		return -1.f;
	return who->Position().distance_to(what->Position());
}

// Under lua_pcall: arg 1 the name of a function of coop_mods_trade_computer.script, the rest its arguments. Returns what it
// returns (lua_pcall keeps three values); nothing when the script or the function is missing (no Gadgets: no grants).
int lua_trade_computer(lua_State* L)
{
	const int args = lua_gettop(L) - 1;
	lua_getglobal(L, "coop_mods_trade_computer"); // _G.__index loads the module if it was not yet
	if (!lua_istable(L, -1))
		return 0;
	lua_pushvalue(L, 1);
	lua_gettable(L, -2);
	if (!lua_isfunction(L, -1))
		return 0;
	for (int i = 2; i <= args + 1; ++i)
		lua_pushvalue(L, i);
	lua_call(L, args, 3);
	return 3;
}

// coop_mods_trade_computer.<fn>(a, b), protected. out: the three values it returned, NaN where it gave no number (the
// caller bounds them with trade_permit::object_of / ttl_of / reach_of). False: no Lua here, or a Lua error (logged a few times).
bool call_trade_computer(const char* fn, double a, double b, double out[3])
{
	out[0] = out[1] = out[2] = std::numeric_limits<double>::quiet_NaN();
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 12))
		return false;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &lua_trade_computer);
	lua_pushstring(L, fn);
	lua_pushnumber(L, lua_Number(a));
	lua_pushnumber(L, lua_Number(b));
	if (lua_pcall(L, 3, 3, 0) != 0)
	{
		if (g_permit_errors++ < 5)
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] coop_mods_trade_computer.%s failed (the stock rules stand): %s", fn, err ? err : "?");
		}
		lua_settop(L, top);
		return false;
	}
	for (int i = 0; i < 3; ++i)
		if (lua_type(L, top + 1 + i) == LUA_TNUMBER)
			out[i] = double(lua_tonumber(L, top + 1 + i));
	lua_settop(L, top);
	return true;
}

// ---------------------------------------------------------------- TZ R90: Anomaly's Lua trade window (scripts\coop_trade_window.script)
// Under lua_pcall: arg 1 the name of a function of coop_trade_window.script, the rest its arguments. Returns what it returns (two
// values kept); nothing when the script or the function is missing (an older gamedata: a copy keeps the engine window).
int lua_trade_window_fn(lua_State* L)
{
	const int args = lua_gettop(L) - 1;
	lua_getglobal(L, "coop_trade_window"); // _G.__index loads the module if it was not yet
	if (!lua_istable(L, -1))
		return 0;
	lua_pushvalue(L, 1);
	lua_gettable(L, -2);
	if (!lua_isfunction(L, -1))
		return 0;
	for (int i = 2; i <= args + 1; ++i)
		lua_pushvalue(L, i);
	lua_call(L, args, 2);
	return 2;
}

u32 g_window_errors = 0;

// coop_trade_window.<fn>(...), protected: `push(L)` pushes the arguments and returns their number, `read(L, at)` reads the two results
// at `at` and `at + 1` (nil when the script gave none). False: no Lua here, or a Lua error (logged a few times).
template <class Push, class Read>
bool call_trade_window(const char* fn, Push push, Read read)
{
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 16))
		return false;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &lua_trade_window_fn);
	lua_pushstring(L, fn);
	const int n = push(L);
	if (lua_pcall(L, 1 + n, 2, 0) != 0)
	{
		if (g_window_errors++ < 5)
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] coop_trade_window.%s failed: %s", fn, err ? err : "?");
		}
		lua_settop(L, top);
		return false;
	}
	read(L, top + 1);
	lua_settop(L, top);
	return true;
}

// A Lua array of ids.
void push_ids(lua_State* L, const xr_vector<u16>& ids)
{
	lua_createtable(L, int(ids.size()), 0);
	for (size_t i = 0; i < ids.size(); ++i)
	{
		lua_pushnumber(L, lua_Number(ids[i]));
		lua_rawseti(L, -2, int(i + 1));
	}
}

// Player `from`'s trade window with `trader_id` opened here: the grant the decider's Lua kept for it (take_grant, taken once)
// becomes his permit when the computer it names is here and his character stands at it. Any older permit of his goes: one
// window, one permit.
void host_take_permit(PeerId from, u16 trader_id)
{
	trade_permit::Book& book = permits();
	book.revoke(from);
	double g[3];
	if (!call_trade_computer("take_grant", double(from), double(trader_id), g))
		return;
	const u16 computer = trade_permit::object_of(g[0]);
	if (computer == trade_permit::kNoObject)
		return; // no grant for this window: the stock rules
	trade_permit::Facts f;
	f.player = from;
	f.trader = trader_id;
	f.now = now_ms();
	f.to_computer = distance_to(smart_cast<CGameObject*>(find_object(join::proxy_of_peer(from))), find_object(computer));
	trade_permit::Verdict v = trade_permit::Verdict::None;
	if (!world::is_player_proxy(computer) && !world::is_player_proxy(trader_id) &&
	    book.issue(from, computer, trader_id, f.now, trade_permit::ttl_of(g[1]), trade_permit::reach_of(g[2])))
		v = book.judge(f);
	if (!trade_permit::granted(v))
	{
		book.revoke(from);
		++g_permits_dropped;
		::Msg("! [coop] trade: player #%u's computer grant for %u through %u is not kept (%s)", from, u32(trader_id),
		    u32(computer), trade_permit::verdict_name(v));
		return;
	}
	++g_permits_issued;
	::Msg("[coop] trade: player #%u may trade with %u through computer %u", from, u32(trader_id), u32(computer));
}

// The deal of player `from` with `trader_id` through his permit: Granted, or why not (None: he holds none).
trade_permit::Verdict permit_verdict(PeerId from, CInventoryOwner* proxy, u16 trader_id)
{
	trade_permit::Book& book = permits();
	const trade_permit::Permit* p = book.find(from);
	if (!p)
		return trade_permit::Verdict::None;
	trade_permit::Facts f;
	f.player = from;
	f.trader = trader_id;
	f.now = now_ms();
	f.to_computer = distance_to(smart_cast<CGameObject*>(proxy), find_object(p->computer));
	return book.judge(f);
}

// ---------------------------------------------------------------- client state
struct DealItem
{
	u16 id; // local numbering
	u32 price;
};
struct PendingDeal
{
	u16 trader = kNoId; // host numbering
	bool donate = false;
	bool lua = false; // TZ R90: sent by the Lua window (coop.trade_deal): its answer goes to coop_trade_window.on_result
	bool lua_prices = false; // TZ R90: flag kDealLuaPrices
	xr_vector<DealItem> sell; // player -> trader
	xr_vector<DealItem> buy; // trader -> player
	u32 sell_total = 0;
	u32 buy_total = 0;
	u64 sent_at = 0;
	bool warned = false;
};
xr_map<u32, PendingDeal> g_pending; // client: seq -> deal
u32 g_seq = 0;
u32 g_client_world = 0; // join::world_generation() of the pending deals

// Client: Lua actor_on_trade for an accepted deal, fired when the item has arrived locally
// (the ownership mirror follows the answer) so scripts see it where single player has it.
struct TradeCallback
{
	u16 item; // local
	u16 expect_parent; // local
	bool actor_sells;
	u32 price;
	u64 deadline;
	bool repair_asked = false;
};
xr_vector<TradeCallback> g_callbacks;
constexpr u64 kCallbackWaitMs = 3000;
constexpr u64 kCallbackRepairWaitMs = 45000; // includes the ownership mirror's retry window
u64 g_cl_deals = 0, g_cl_ok = 0, g_cl_rejected = 0;

// The host's save of a new world holds the outcome of every deal sent so far: nothing is pending.
void client_sync_world()
{
	const u32 gen = join::world_generation();
	if (gen == g_client_world)
		return;
	g_client_world = gen;
	g_pending.clear();
	g_callbacks.clear();
}

// ---------------------------------------------------------------- open window state (both roles)
struct PendingMove
{
	u16 item;
	u16 to;
	u64 check_at;
};
struct ViewState
{
	bool shown = false;
	u8 kind = ContNone;
	u16 id = kNoId; // local numbering
	u64 sig = 0;
	bool labels = false; // refresh money/weight labels
	u64 rebuild_at = 0; // rebuild the lists at this time (0: no)
	u64 last_rebuild = 0;
	xr_vector<PendingMove> moves;
	trade_permit::Window remote; // G6-3: this machine's grant for the trader of this window through a trading computer
};
ViewState g_view;

// G6-3: a trade window with `partner` opens on this machine: the grant its Lua got for that trader through a trading
// computer (coop_mods_trade_computer.window_grant, taken once), else none (the stock window). The co-op host's own and a
// single player's trade never ask, so there is none there.
void client_take_window(CInventoryOwner* partner)
{
	g_view.remote = trade_permit::Window();
	const u16 trader = id_of(partner);
	if (trader == kNoId)
		return;
	double g[3];
	if (call_trade_computer("window_grant", double(trader), 0.0, g))
		g_view.remote = trade_permit::window_of(trader, g[0], g[1], now_ms());
}

// What the Lua inventory shows, local numbering, in the low 16 bits; above them the view the script named (kViewTrader: a trade window,
// TZ R90). Lua may run on the game worker thread.
std::atomic<u32> g_search_now{kNoId};
u16 g_search_sent = kNoId;
u8 g_search_kind = ContNone;
u64 g_rebuilds = 0, g_moves_failed = 0;

void request_rebuild(u64 at)
{
	if (!g_view.rebuild_at || at < g_view.rebuild_at)
		g_view.rebuild_at = at;
}

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

// ---------------------------------------------------------------- host

void host_broadcast_trader_money(CInventoryOwner* trader, PeerId except)
{
	const u16 id = id_of(trader);
	if (id == kNoId)
		return;
	Writer w(8);
	write_trader_money(w, id, trader->get_money());
	net::host_broadcast(Msg::TradeOp, w, Lane::Reliable, except);
}

void host_send_result(PeerId to, const LastResult& r)
{
	Writer w(28);
	w.u8(u8(Op::Result));
	w.u32(r.seq);
	w.u16(r.trader);
	w.u8(r.code);
	w.i32(r.delta);
	w.u32(r.trader_money);
	// TZ R90, review of 2026-11-25 (protocol 35, the same Dev build): a refusal for its price carries the decider's prices (TZ R92:
	// for the player's log and text)
	if (r.code == PriceChanged)
	{
		w.u32(r.decider_sell);
		w.u32(r.decider_buy);
	}
	net::host_send(to, Msg::TradeOp, w);
}

// R4 2: may the player offer `item` to the trader? As the stock window decides (CUIActorMenu::CanMoveToPartner):
// tradable at all, and in a state the trader takes. CInventoryItem::CanTrade() does not fit here: on the host the
// item's owner is the player's character, a stalker whose AllowItemToTrade keeps the weapons it would use itself.
bool trader_takes(CInventoryOwner* trader, CInventoryItem* item)
{
	LPCSTR section = item->object().cNameSect().c_str();
	if (item->IsQuestItem() || !READ_IF_EXISTS(pSettings, r_bool, section, "can_trade", TRUE))
		return false;
	const CEatableItem* eatable = item->cast_eatable_item();
	if (eatable && eatable->GetMaxUses())
		return true;
	return item->GetCondition() >= trader->trade_parameters().buy_item_condition_factor;
}

// R4 2: the price of a list with the stock formula, as the player's own machine shows it to its actor:
// `pricing` is the trader's CTrade with the player's character as the actor (the trader's trade parameters, its
// attitude to that character, the scripted discounts). `trader_buys`: the player sells the list. A price of 0 is an
// item the trader's trade parameters do not allow in that direction. What the trader sells is what the stock trade
// window offers (CInventory::AddAvailableItems: CanTrade, with the trader as the owner).
u8 price_list(CTrade& pricing, CInventoryOwner* trader, const xr_vector<u16>& list, bool trader_buys, u32& total)
{
	u64 sum = 0;
	for (u16 id : list)
	{
		CInventoryItem* item = smart_cast<CInventoryItem*>(find_object(id));
		if (!item)
			return ItemGone;
		if (trader_buys ? !trader_takes(trader, item) : !item->CanTrade())
			return NotTraded;
		const u32 price = pricing.GetItemPrice(item, trader_buys);
		if (!price)
			return NotTraded;
		sum += price;
	}
	if (sum > u64(kMaxMoney))
		return Malformed;
	total = u32(sum);
	return Ok;
}

// TZ R90: coop_trade_window.host_price here, under lua_pcall: the sums of both lists of a player's deal by the Lua window's formula, the
// host's own window's formula. TZ R92: run as for the decider itself, no player character put in (G.A.M.M.A.'s trader lists and
// discounts do not depend on the character); the player's items are held by his character here, and the Weapon Parts Overhaul
// compatibility script treats any player as the holder its checks look for. Priced: two numbers. Refused: nil and why (`why`: what the
// script said), or a Lua error, or no Lua. NoFormula (review of 2026-11-25): nothing at all, the decider's gamedata has no such function.
trade_window::Answer ask_host_price(u16 proxy_id, u16 trader_id, const Deal& d, double raw[2], std::string& why)
{
	trade_window::Answer answer = trade_window::Answer::Refused;
	const bool called = call_trade_window("host_price", [&](lua_State* L)
	{
		lua_pushnumber(L, lua_Number(proxy_id));
		lua_pushnumber(L, lua_Number(trader_id));
		push_ids(L, d.sell);
		push_ids(L, d.buy);
		return 4;
	}, [&](lua_State* L, int at)
	{
		if (lua_type(L, at) == LUA_TNUMBER && lua_type(L, at + 1) == LUA_TNUMBER)
		{
			answer = trade_window::Answer::Priced;
			raw[0] = double(lua_tonumber(L, at));
			raw[1] = double(lua_tonumber(L, at + 1));
		}
		else if (lua_isnoneornil(L, at) && lua_isnoneornil(L, at + 1))
			answer = trade_window::Answer::NoFormula; // lua_trade_window_fn gave nothing: no script, no host_price
		else if (lua_type(L, at + 1) == LUA_TSTRING)
			why = lua_tostring(L, at + 1);
	});
	return called ? answer : trade_window::Answer::Refused;
}

// TZ R92: the decider's check of a player's deal against its own window's formula (ask_host_price), each side floored once as TMode_Sell
// does and compared with what the player's window showed (xrCoop/coop_trade_window_rules.h settle_side): the sums a Lua window sent
// (protocol 35), or for the engine window's deal (flag 0) the CTrade sums host_apply put there, which that window showed. Within the
// tolerance (tolerance_of: rounding and the condition's u16 steps; for CTrade also its floors) the side goes at the shown sum, as in single
// player; beyond it, either way, the deal is refused (PriceChanged) with the decider's sums in d.sell_decider / buy_decider, for the log.
// Review of 2026-11-25: the formula is the decider's, not the flag's (check_of): flag 0 is no way around the G.A.M.M.A. formula. No
// fallback price for a Lua window's deal: no answer refuses it; the engine window's deal keeps its CTrade sums only where this decider
// has no Lua formula at all (its gamedata is older than R90).
u8 lua_price_deal(u16 proxy_id, u16 trader_id, Deal& d)
{
	double raw[2] = {0.0, 0.0};
	std::string why;
	const trade_window::Answer answer = ask_host_price(proxy_id, trader_id, d, raw, why);
	const trade_window::Check check = trade_window::check_of(d.lua_prices, answer);
	if (check == trade_window::Check::EngineOnly)
		return Ok; // d.sell_total, d.buy_total: the engine formula's, as before R90
	const char* window = d.lua_prices ? "a Lua window" : "the engine window";
	const bool priced = check == trade_window::Check::Compare;
	u32 decider[2] = {0, 0};
	const trade_window::PriceVerdict vs = trade_window::host_total(!d.sell.empty(), priced, raw[0], u32(kMaxMoney), decider[0]);
	const trade_window::PriceVerdict vb = trade_window::host_total(!d.buy.empty(), priced, raw[1], u32(kMaxMoney), decider[1]);
	trade_window::PriceVerdict v = vs != trade_window::PriceVerdict::Ok ? vs : vb;
	if (v == trade_window::PriceVerdict::Ok)
	{
		d.sell_decider = decider[0];
		d.buy_decider = decider[1];
		const u32 sell_tolerance = trade_window::tolerance_of(d.lua_prices, d.sell.size());
		const u32 buy_tolerance = trade_window::tolerance_of(d.lua_prices, d.buy.size());
		const trade_window::PriceVerdict ss =
		    trade_window::settle_side(!d.sell.empty(), decider[0], d.sell_shown, sell_tolerance, d.sell_total);
		const trade_window::PriceVerdict sb =
		    trade_window::settle_side(!d.buy.empty(), decider[1], d.buy_shown, buy_tolerance, d.buy_total);
		v = ss != trade_window::PriceVerdict::Ok ? ss : sb;
		if (v == trade_window::PriceVerdict::Ok)
		{
			if (d.sell_total != decider[0] || d.buy_total != decider[1])
				::Msg("[coop] trade #%u from %s: the formula here says %u RU for the sale and %u RU for the purchase, the window "
				      "showed %u and %u RU (within %u and %u RU): sold for %u, bought for %u RU, as shown",
				    d.seq, window, decider[0], decider[1], d.sell_shown, d.buy_shown, sell_tolerance, buy_tolerance, d.sell_total,
				    d.buy_total);
			return Ok;
		}
		if (v == trade_window::PriceVerdict::Changed)
		{
			d.sell_total = d.buy_total = 0;
			::Msg("[coop] trade #%u from %s refused for its price: the formula here says %u RU for the sale and %u RU for the "
			      "purchase, the window showed %u and %u RU, beyond %u and %u RU; the player's window takes the trader's data again",
			    d.seq, window, decider[0], decider[1], d.sell_shown, d.buy_shown, sell_tolerance, buy_tolerance);
			return PriceChanged;
		}
		why = "the window showed nothing to pay";
	}
	d.sell_total = d.buy_total = 0;
	d.sell_decider = d.buy_decider = 0;
	::Msg("! [coop] trade #%u from %s: %s%s%s", d.seq, window, trade_window::price_verdict_name(v), why.empty() ? "" : ": ",
	    why.c_str());
	return v == trade_window::PriceVerdict::TooMuch ? u8(Malformed) : u8(NotTraded);
}

// One deal, everything or nothing. Money: the trader's and the player's both live on the host (the
// player's is his balance, coop_money; F2 T1). Both sides are checked after the whole deal, so what
// the player sells pays for what he buys and the other way round. The decider sets what the deal moves
// (d.sell_total, d.buy_total): the engine window's deal carries item ids only and goes at the CTrade sums
// here (R4 2), a Lua window's deal at the sums it showed; either only once the formula here agrees (TZ R92).
bool transfer_ready(u16 item_id, u16 from, CInventoryOwner* to)
{
	CGameObject* go = smart_cast<CGameObject*>(find_object(item_id));
	CInventoryItem* item = go ? smart_cast<CInventoryItem*>(go) : nullptr;
	// SELL can detach an object that is leaving the level, or whose previous
	// ownership event has not reached its game object. BUY may then reject it.
	return item && !go->getDestroy() && !go->object_removed() && server_parent(item_id) == from &&
	    local_parent(item_id) == from && to->inventory().CanTakeItem(item);
}

bool transfer_deal(const Deal& d, CInventoryOwner* proxy, CInventoryOwner* trader)
{
	struct Move { u16 item, from, to; };
	xr_vector<Move> moved;
	moved.reserve(d.sell.size() + d.buy.size());
	const auto transfer = [&](u16 id, CInventoryOwner* from, CInventoryOwner* to)
	{
		CInventoryItem* item = smart_cast<CInventoryItem*>(find_object(id));
		from->on_before_sell(item);
		to->on_before_buy(item);
		moved.push_back({id, id_of(from), id_of(to)});
		move_item(id_of(from), id_of(to), id);
		// Local-server ownership is synchronous; the inventory events follow next frame.
		return server_parent(id) == id_of(to);
	};
	bool complete = true;
	for (u16 id : d.sell)
		if (!(complete = transfer(id, proxy, trader))) break;
	if (complete)
		for (u16 id : d.buy)
			if (!(complete = transfer(id, trader, proxy))) break;
	if (complete) return true;

	// No money has moved. Return both halves of a barter, including a last
	// item detached by SELL but refused by BUY. Never duplicate/recreate it.
	for (auto it = moved.rbegin(); it != moved.rend(); ++it)
	{
		const u16 parent = server_parent(it->item);
		if (parent == it->from) continue;
		if (parent == it->to || parent == kNoId)
			move_item(parent, it->from, it->item);
		if (server_parent(it->item) != it->from)
			::Msg("! [coop] trade #%u rollback: item %u could not return to %u (holder %u); no money charged",
			    d.seq, it->item, it->from, server_parent(it->item));
	}
	return false;
}

u8 host_apply(PeerId from, Deal& d, s64& delta)
{
	delta = 0;
	if (sync::host_peer_joining(from) || Device.dwPrecacheFrame) // precaching: a deal from the old world
		return NotReady;
	const u16 proxy_id = join::proxy_of_peer(from);
	CInventoryOwner* proxy = find_owner(proxy_id);
	if (!proxy)
		return NoPlayer;
	const u16 trader_id = d.trader;
	CInventoryOwner* trader = find_owner(trader_id);
	if (!trader || trader == proxy || trader_id == 0 || world::is_player_proxy(trader_id) || !alive(trader))
		return NoTrader;
	if (!smart_cast<CAI_Trader*>(trader) && !smart_cast<CAI_Stalker*>(trader))
		return NoTrader; // CTrade (pricing below) knows traders and stalkers only
	// R4 2: the counterparty is one the player trades with now (talks::may_trade_with): its stock trade flag is on, or,
	// on Anomaly 1.5.3 where xr_meet keeps that flag off and trade opens from the dialog, the player talked to it; and it
	// stands by the player's character.
	// G6-3: or the player holds the decider's permit for this trader through a trading computer he stands at
	// (xrCoop/coop_trade_permit.h): it stands for the talk and for the reach to the trader, nothing else.
	const trade_permit::Verdict permit = permit_verdict(from, proxy, trader_id);
	const bool remote = trade_permit::granted(permit);
	if (permit != trade_permit::Verdict::None && !remote)
		::Msg("[coop] trade #%u: the computer permit does not hold (%s), the stock rules apply", d.seq,
		    trade_permit::verdict_name(permit));
	const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(trader);
	if (!talks::may_trade_with(trader->IsTradeEnabled(),
	        dialog::host_talked_recently(from, trader_id, u32(talks::kTradeAfterTalkMs)) || remote, stalker && stalker->wounded()))
		return NoTrader;
	if (!remote && !trader->NeedOsoznanieMode() && !within_reach(proxy, trader, kTradeReach))
		return TooFar;
	if (remote)
		++g_permit_deals;
	const size_t n = d.sell.size() + d.buy.size();
	if (!n || n > kMaxDealItems)
		return Malformed;
	if (d.donate && !d.buy.empty())
		return Malformed;

	// Every item once, and still where the client saw it.
	xr_vector<u16> seen;
	seen.reserve(n);
	const auto check = [&](const xr_vector<u16>& list, u16 holder) -> u8
	{
		for (u16 id : list)
		{
			if (std::find(seen.begin(), seen.end(), id) != seen.end())
				return Malformed;
			seen.push_back(id);
			CObject* o = find_object(id);
			if (!o || !smart_cast<CInventoryItem*>(o) || server_parent(id) != holder)
				return ItemGone;
		}
		return Ok;
	};
	if (const u8 c = check(d.sell, proxy_id))
		return c;
	if (const u8 c = check(d.buy, trader_id))
		return c;
	for (u16 id : d.sell)
		if (!transfer_ready(id, proxy_id, trader)) return ItemGone;
	for (u16 id : d.buy)
		if (!transfer_ready(id, trader_id, proxy)) return ItemGone;

	// A gift is free, as the stock DonateCurrentItem (TransferItem with b_free).
	// TZ R92: a deal goes at what the player's window showed, as in single player, once the decider's own window's formula (the
	// G.A.M.M.A. one, computed as for itself) agrees with it within the tolerance (lua_price_deal, settle_side). A Lua window's deal
	// carries its sums. The engine window's deal (flag 0) carries item ids only: what it showed is the CTrade sums for the player's
	// character (R4 2: its own formula; a temporary CTrade, the trader's own one may hold the host actor's trade with him), and they meet
	// the formula the same way, with that window's tolerance. The player's choice of window chooses no formula.
	d.sell_total = d.buy_total = 0;
	switch (trade_window::pricing_of(d.donate, d.lua_prices))
	{
	case trade_window::Pricing::Free:
		break;
	case trade_window::Pricing::Lua:
		if (const u8 c = lua_price_deal(proxy_id, trader_id, d))
			return c;
		break;
	case trade_window::Pricing::Engine:
	{
		CTrade pricing(trader);
		pricing.SetActorPartner(proxy);
		if (const u8 c = price_list(pricing, trader, d.sell, true, d.sell_total))
			return c;
		if (const u8 c = price_list(pricing, trader, d.buy, false, d.buy_total))
			return c;
		d.sell_shown = d.sell_total; // what the engine window showed (R4 2: its formula for this character, as here)
		d.buy_shown = d.buy_total;
		if (const u8 c = lua_price_deal(proxy_id, trader_id, d))
			return c;
		break;
	}
	}

	// F2 T1: the player's money is his balance on the host, never a figure from the client.
	const s64 net = s64(d.sell_total) - s64(d.buy_total); // to the player
	const s64 player_after = s64(money::host_balance(from)) + net;
	const s64 trader_after = s64(trader->get_money()) - net;
	if (player_after < 0)
		return NoMoney;
	if (trader_after < 0 && !trader->InfinitiveMoney())
		return TraderNoMoney;
	if ((net > 0 && player_after > s64(money::max_money())) || trader_after > kMaxMoney || net > INT_MAX ||
	    net < -INT_MAX)
		return Malformed;

	// The trader's own reaction (scripted trader animations / phrases), as OnPerformTrade(get, put).
	if (!d.donate)
		if (CAI_Trader* ai_trader = smart_cast<CAI_Trader*>(trader))
			ai_trader->callback(GameObject::eTradePerformTradeOperation)(d.buy_total, d.sell_total);

	if (!transfer_deal(d, proxy, trader))
		return ItemGone;
	g_host_items += n;

	if (net != 0)
	{
		trader->set_money(clamp_money(trader_after), true); // an infinite-money trader keeps max(old, new), as in SP
		money::host_adjust(from, net, "trade"); // F2 T1: MoneyBalance reaches the player before the Result
	}
	delta = net;
	return Ok;
}

bool read_list(Reader& r, xr_vector<u16>& out)
{
	const u16 n = r.u16();
	if (!r.ok() || n > kMaxDealItems)
		return false;
	out.reserve(n);
	for (u16 i = 0; i < n; ++i)
		out.push_back(r.u16());
	return r.ok();
}

// A view a player may really have open: a box, a dead body, or the trader he holds the talk lock of. A living character's
// items and an id out of thin air are nobody's business.
bool may_see_contents(PeerId from, u8 kind, u16 id)
{
	CObject* o = find_object(id);
	if (kind == ContBox)
		return o && smart_cast<CInventoryBox*>(o) != nullptr;
	if (kind == ContCorpse)
	{
		CInventoryOwner* owner = o ? smart_cast<CInventoryOwner*>(o) : nullptr;
		return owner && !alive(owner);
	}
	return kind == ContTrader &&
	    (dialog::host_lock_of(from) == id || dialog::host_talked_recently(from, id, u32(talks::kTradeAfterTalkMs)) ||
	        permits().covers(from, id, now_ms())); // G6-3: the trader of his computer permit
}

// TZ R92 (review of 2026-11-25, lua_flow#1): after a refusal for its price the player's window takes the trader's data again without
// closing its view: a close revokes a trading computer's permit (host_on_container), and the grant behind it is taken once
// (coop_mods_trade_computer.script take_grant), so every later deal in that window would be TooFar. The decider sends that data with
// the refusal instead, ahead of the Result and on the same reliable lane: the states of the trader's items (as a new view gets them)
// and his money (as host_on_container sends it). They are on the copy when the Result reaches its window, which draws itself anew.
// Nothing while the player joins, nothing of a trader he may not see. No new message (ItemState, TradeOp TraderMoney; protocol 35).
void host_send_trader_view(PeerId to, u16 trader_id)
{
	if (sync::host_peer_joining(to) || !may_see_contents(to, ContTrader, trader_id))
		return;
	items::host_send_contents(to, trader_id);
	if (CInventoryOwner* trader = find_owner(trader_id))
	{
		Writer w(8);
		write_trader_money(w, trader_id, trader->get_money());
		net::host_send(to, Msg::TradeOp, w);
	}
}

void host_on_deal(PeerId from, Reader& r)
{
	Deal d;
	d.seq = r.u32();
	d.trader = r.u16();
	const u8 flags = r.u8();
	if (!r.ok())
		return; // no seq to answer
	d.donate = (flags & kDealDonate) != 0;
	d.lua_prices = (flags & kDealLuaPrices) != 0; // TZ R90
	bool parsed = read_list(r, d.sell) && read_list(r, d.buy);
	if (parsed && d.lua_prices) // TZ R90, protocol 35: the sums the player's window showed follow the lists (TZ R92: the price)
	{
		d.sell_shown = r.u32();
		d.buy_shown = r.u32();
		parsed = r.ok();
	}

	host_sync_world();
	PeerTrade& p = g_peers[from];
	LastResult& last = p.last;
	if (last.have && d.seq == last.seq)
	{
		host_send_result(from, last); // repeat: same answer, nothing changes
		return;
	}
	if (last.have && s32(d.seq - last.seq) < 0)
		return; // older than the last answered deal

	++g_host_deals;
	s64 delta = 0;
	const u8 code = parsed ? host_apply(from, d, delta) : u8(Malformed);
	CInventoryOwner* trader = find_owner(d.trader);

	last.have = true;
	last.seq = d.seq;
	last.trader = d.trader;
	last.code = code;
	last.delta = s32(delta);
	last.trader_money = trader ? trader->get_money() : 0;
	last.decider_sell = d.sell_decider; // read by the player only with PriceChanged
	last.decider_buy = d.buy_decider;
	if (code == PriceChanged)
		host_send_trader_view(from, d.trader); // TZ R92 (lua_flow#1): the trader's items and money first, then the answer
	host_send_result(from, last);

	const PeerInfo* info = mgr().session().peer(from);
	::Msg("[coop] trade #%u <%s> with %u: %s %u item(s) for %u RU, buys %u item(s) for %u RU%s: %s", d.seq,
	    info ? info->nick.c_str() : "?", d.trader, d.donate ? "gives" : "sells", (u32)d.sell.size(), d.sell_total,
	    (u32)d.buy.size(), d.buy_total, d.lua_prices && !d.donate ? " (Lua window prices)" : "", result_name(code));
	if (code == Ok)
	{
		++g_host_ok;
		if (trader)
			host_broadcast_trader_money(trader, from);
	}
	else
		++g_host_rejected;
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
		permits().revoke(from); // G6-3: any close clears the player's open view
		return;
	}
	const auto was = g_open.find(from);
	const bool same_view = was != g_open.end() && was->second.kind == kind && was->second.id == id;
	OpenView& v = g_open[from];
	v.kind = kind;
	v.id = id;
	if (!same_view)
	{
		// G6-3: a new window: his computer permit follows it (the decider's grant for this trader, else none)
		if (kind == ContTrader && !sync::host_peer_joining(from))
			host_take_permit(from, id);
		else
			permits().revoke(from);
	}
	if (!same_view && !sync::host_peer_joining(from) && may_see_contents(from, kind, id))
		items::host_send_contents(from, id); // F1: what his copy of it shows may be the spawn values
	if (kind == ContBox || kind == ContCorpse)
	{
		// Host Lua (W5, e.g. loot addons): SendScriptCallback("coop_on_container_opened", peer, id).
		CObject* o = find_object(id);
		const bool real = o && (kind == ContBox ? smart_cast<CInventoryBox*>(o) != nullptr
		                                        : (smart_cast<CInventoryOwner*>(o) && !alive(smart_cast<CInventoryOwner*>(o))));
		if (real && !sync::host_peer_joining(from) && may_see_contents(from, kind, id))
			script::on_container_opened(from, id); // a box across the level is no window on it: no roll for a modified client
	}
	if (kind == ContTrader)
	{
		// The client's copy of the trader's money may be old: send the current one.
		if (CInventoryOwner* trader = find_owner(id))
		{
			Writer w(8);
			write_trader_money(w, id, trader->get_money());
			net::host_send(from, Msg::TradeOp, w);
		}
	}
}

// ---------------------------------------------------------------- client

u32 list_total(const xr_vector<DealItem>& list)
{
	u64 sum = 0;
	for (const DealItem& it : list)
		sum += it.price;
	return u32(sum > u64(kMaxMoney) ? u64(kMaxMoney) : sum);
}

void write_list(Writer& w, const xr_vector<DealItem>& list)
{
	w.u16(u16(list.size()));
	for (const DealItem& it : list)
		w.u16(join::to_host(it.id));
}

// TZ R90: coop.trade_deal may run on the game's worker thread (Lua does, see lua_search_window): a deal of the Lua window is queued with
// its seq under this lock and frame() sends it on the main thread. client_send_deal takes the lock too: the wire carries the deals in
// the order of their seq (the host ignores a deal older than its last answer).
struct QueuedDeal
{
	u32 seq;
	u16 trader; // local numbering
	PendingDeal deal;
};
std::mutex g_deal_lock;
xr_vector<QueuedDeal> g_queued; // under g_deal_lock
std::atomic<bool> g_have_queued{false};

// One deal with its seq (the caller holds g_deal_lock).
void send_deal_locked(u32 seq, u16 trader_local, PendingDeal& deal)
{
	client_sync_world();
	// Item moves and money changes (ReqMoney, F2 T1) made before the deal go out first: on the one
	// reliable lane the host checks the deal against them.
	requests::client_flush_outbox();
	// The engine window's prices stay here (the money pre-checks of the trade buttons); the decider prices its deal with CTrade (R4 2).
	// The Lua window's sums are the ones it showed, floored once (lua_trade_deal): TZ R92, the deal's price.
	if (!deal.lua)
	{
		deal.sell_total = list_total(deal.sell);
		deal.buy_total = list_total(deal.buy);
	}
	deal.trader = join::to_host(trader_local);
	deal.sent_at = now_ms();

	// F2 T1: no money of ours in the deal (a Lua window's shown sums are prices); the host checks it against our balance there.
	Writer w(32 + (deal.sell.size() + deal.buy.size()) * 2);
	w.u8(u8(Op::Deal));
	w.u32(seq);
	w.u16(deal.trader);
	w.u8(u8((deal.donate ? kDealDonate : 0) | (deal.lua_prices ? kDealLuaPrices : 0)));
	write_list(w, deal.sell);
	write_list(w, deal.buy);
	if (deal.lua_prices) // TZ R90, protocol 35: the sums the window showed; TZ R92: the price, which the decider checks
	{
		w.u32(deal.sell_total);
		w.u32(deal.buy_total);
	}
	net::client_send(Msg::TradeOp, w);
	g_pending[seq] = std::move(deal);
	++g_cl_deals;
}

// The Lua window's deals queued since the last frame, in seq order (the caller holds g_deal_lock).
void send_queued_locked()
{
	if (g_queued.empty())
		return;
	const bool live = is_live_mirror();
	for (QueuedDeal& q : g_queued)
	{
		if (live)
			send_deal_locked(q.seq, q.trader, q.deal);
		else
			::Msg("! [coop] trade #%u of the Lua window dropped: this machine is no longer in the world", q.seq);
	}
	g_queued.clear();
	g_have_queued.store(false);
}

void client_send_deal(u16 trader_local, PendingDeal& deal)
{
	std::lock_guard<std::mutex> lock(g_deal_lock);
	send_queued_locked();
	send_deal_locked(++g_seq, trader_local, deal);
}

// A Lua array of ids at `at` (nil: none). False: neither, or an element that is no id.
bool read_lua_ids(lua_State* L, int at, std::vector<uint16_t>& out)
{
	if (lua_isnoneornil(L, at))
		return true;
	if (!lua_istable(L, at))
		return false;
	const int n = int(lua_objlen(L, at));
	for (int i = 1; i <= n; ++i)
	{
		lua_rawgeti(L, at, i);
		uint16_t id = kNoId;
		const bool ok = lua_type(L, -1) == LUA_TNUMBER && trade_window::id_of(double(lua_tonumber(L, -1)), kNoId, id);
		lua_pop(L, 1);
		if (!ok)
			return false;
		out.push_back(id);
	}
	return true;
}

// The prices the window showed for a list of `n` items (none, or no number: 0).
void read_lua_prices(lua_State* L, int at, size_t n, std::vector<double>& out)
{
	out.assign(n, 0.0);
	if (!lua_istable(L, at))
		return;
	for (size_t i = 0; i < n; ++i)
	{
		lua_rawgeti(L, at, int(i + 1));
		if (lua_type(L, -1) == LUA_TNUMBER)
			out[i] = double(lua_tonumber(L, -1));
		lua_pop(L, 1);
	}
}

void client_on_result(Reader& r)
{
	const u32 seq = r.u32();
	const u16 trader_host = r.u16();
	const u8 code = r.u8();
	const s32 delta = r.i32();
	const u32 trader_money = r.u32();
	u32 decider_sell = 0, decider_buy = 0; // TZ R90, R92: the decider's prices of a deal refused for its price (log and text)
	if (code == PriceChanged)
	{
		decider_sell = r.u32();
		decider_buy = r.u32();
	}
	if (!r.ok())
		return;
	client_sync_world(); // an answer from a replaced world is already in the new world's money
	auto it = g_pending.find(seq);
	if (it == g_pending.end())
		return; // repeat or unknown: applied already
	const PendingDeal deal = std::move(it->second);
	g_pending.erase(it);

	if (CInventoryOwner* trader = find_owner(join::to_local(trader_host)))
		trader->set_money(trader_money, false);

	CActor* a = Actor();
	if (code == Ok)
	{
		++g_cl_ok;
		// The money moved is the decider's: what a Lua window showed (TZ R92), the CTrade sums there for the engine
		// window's deal (R4 2). A difference means the trader's profile or the relations differ between the machines.
		const s64 shown = s64(deal.sell_total) - s64(deal.buy_total);
		if (shown != delta)
			::Msg("[coop] trade #%u: the host priced it %d RU, this window showed %lld RU", seq, delta, shown);
		if (a)
		{
			// F2 T1: the money came before this answer (MoneyBalance from the host's balance), nothing to set.
			// Lua sees the trade of the local actor as in single player (actor_on_trade), see frame(),
			// with the item prices this window showed.
			const u64 deadline = now_ms() + kCallbackWaitMs;
			const u16 trader_local = join::to_local(trader_host);
			// The window shows the deal already (client_transfer moved the cells), and the stock trader-inventory watch
			// (CUIActorMenu::Update) refills the trader's list when the items leave him: the lists are rebuilt again only
			// if an item has not arrived where the deal put it (refresh_actor_menu, kMoveCheckMs). The unconditional
			// settle rebuild of both lists was a second hitch after every purchase (live 2026-10-29, G.A.M.M.A.).
			for (const DealItem& di : deal.sell)
			{
				g_callbacks.push_back({di.id, trader_local, true, di.price, deadline});
				on_container_move(kNoId, trader_local, di.id);
			}
			for (const DealItem& di : deal.buy)
			{
				g_callbacks.push_back({di.id, a->ID(), false, di.price, deadline});
				on_container_move(kNoId, a->ID(), di.id);
			}
		}
		else
		{
			::Msg("! [coop] trade #%u accepted while there is no actor here", seq);
			request_rebuild(now_ms());
		}
	}
	else
	{
		++g_cl_rejected;
		::Msg("[coop] trade #%u rejected by the host: %s", seq, result_name(code));
		if (code == NoMoney)
			dialog::hud_message("st_coop_trade_no_money", "Trade failed: not enough money");
		else if (code == TraderNoMoney)
			dialog::hud_message("st_coop_trade_trader_no_money", "Trade failed: the trader has not enough money");
		else if (code == TooFar)
			dialog::hud_message("st_coop_trade_too_far", "Trade failed: you are too far from the trader");
		else if (code == NoTrader)
			dialog::hud_message("st_coop_trade_no_trader", "Trade failed: the trader does not trade now");
		else if (code == NotTraded)
			dialog::hud_message("st_coop_trade_not_traded", "Trade failed: the trader does not trade this item");
		else if (code == PriceChanged)
		{
			// TZ R92: the decider's formula and this window's sums differ beyond the tolerance; nothing agrees to the decider's price.
			// The Lua window tells it itself and takes the trader's data again (coop_trade_window.on_result); the engine window is
			// told here, and request_rebuild below reads the trader again.
			::Msg("[coop] trade #%u: the trader's prices are %u RU for the sale and %u RU for the purchase, this window showed %u and "
			      "%u RU",
			    seq, decider_sell, decider_buy, deal.sell_total, deal.buy_total);
			if (!deal.lua)
				switch (trade_window::price_tell(deal.lua_prices, deal.sell.size(), deal.buy.size(), decider_sell, decider_buy,
				    deal.sell_total, deal.buy_total))
				{
				case trade_window::PriceTell::Purchase:
					dialog::hud_message_n("st_coop_trade_price_buy",
					    "Trade cancelled: the trader asks %u RU, more than the window showed", decider_buy);
					break;
				case trade_window::PriceTell::Sale:
					dialog::hud_message_n("st_coop_trade_price_sell",
					    "Trade cancelled: the trader pays %u RU, less than the window showed", decider_sell);
					break;
				case trade_window::PriceTell::Stale:
					dialog::hud_message("st_coop_trade_price_stale",
					    "Trade cancelled: the prices in the window are out of date, the window is refreshed");
					break;
				}
		}
		else
			dialog::hud_message("st_coop_trade_failed", "Trade failed: the goods are no longer available");
		request_rebuild(now_ms()); // undo the optimistic move of the cells
	}
	g_view.labels = true;
	if (deal.lua) // TZ R90: the Lua window's buttons wait for this answer; it follows the items that come
		call_trade_window("on_result", [&](lua_State* L)
		{
			lua_pushnumber(L, lua_Number(seq));
			lua_pushnumber(L, lua_Number(code));
			lua_pushnumber(L, lua_Number(delta));
			lua_pushstring(L, result_name(code));
			lua_pushnumber(L, lua_Number(decider_sell)); // review of 2026-11-25: PriceChanged only, else 0
			lua_pushnumber(L, lua_Number(decider_buy));
			return 6;
		}, [](lua_State*, int) {});
}

void client_on_trader_money(Reader& r)
{
	const u16 trader_host = r.u16();
	const u32 money = r.u32();
	if (!r.ok())
		return;
	const u16 local = join::to_local(trader_host);
	if (CInventoryOwner* trader = find_owner(local))
		trader->set_money(money, false);
	if (g_view.shown && g_view.id == local)
		g_view.labels = true;
}

void on_trade_op(PeerId from, Reader& r)
{
	const Op op = Op(r.u8());
	if (!r.ok())
		return;
	switch (op)
	{
	case Op::Deal:
		if (mode().leads_level() && from != kHostPeer)
			host_on_deal(from, r);
		break;
	case Op::Result:
		if (is_mirror())
			client_on_result(r);
		break;
	case Op::TraderMoney:
		if (is_mirror())
			client_on_trader_money(r);
		break;
	default:
		break;
	}
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

// ---------------------------------------------------------------- API

void install_server(Session& s)
{
	s.on(Msg::TradeOp, &on_trade_op);
	s.on(Msg::ReqContainer, [](PeerId from, Reader& r)
	{
		if (mode().leads_level())
			host_on_container(from, r);
	});
}

void install_client(Endpoint& e) { e.on(Msg::TradeOp, &on_trade_op); }

static void search_frame()
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
		kind = search_kind(o != nullptr, box, owner != nullptr, owner && alive(owner), asked); // TZ R90: a trader of a trade window
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

void frame()
{
	search_frame();
	if (g_have_queued.load()) // TZ R90: the Lua window's deals, sent on the main thread
	{
		std::lock_guard<std::mutex> lock(g_deal_lock);
		send_queued_locked();
	}
	if (!is_mirror())
		return;
	client_sync_world();
	if (!is_live_mirror())
		return;
	const u64 now = now_ms();
	for (auto it = g_callbacks.begin(); it != g_callbacks.end();)
	{
		if (local_parent(it->item) != it->expect_parent)
		{
			if (now >= it->deadline)
			{
				if (it->repair_asked)
				{
					::Msg("! [coop] trade: item %u did not reach holder %u locally; purchase callback skipped",
					    join::to_host(it->item), join::to_host(it->expect_parent));
					request_rebuild(now);
					it = g_callbacks.erase(it);
					continue;
				}
				Writer w(4);
				w.u16(join::to_host(it->item));
				w.u8(reconcile::Spawn | reconcile::Parent | reconcile::State);
				net::client_send(Msg::StateDumpRequest, w);
				it->repair_asked = true;
				it->deadline = now + kCallbackRepairWaitMs;
			}
			++it;
			continue;
		}
		CActor* a = Actor();
		CGameObject* go = smart_cast<CGameObject*>(find_object(it->item));
		if (a && go)
			a->callback(GameObject::eTradeSellBuyItem)(go->lua_game_object(), it->actor_sells, it->price);
		it = g_callbacks.erase(it);
	}
	for (auto& it : g_pending)
	{
		if (!it.second.warned && now - it.second.sent_at > kPendingWarnMs)
		{
			it.second.warned = true;
			::Msg("! [coop] trade #%u: no answer from the host for %llus", it.first, (now - it.second.sent_at) / 1000);
			request_rebuild(now);
		}
	}
}

void reset()
{
	g_peers.clear();
	g_open.clear();
	g_permits.clear(); // G6-3
	g_search_now.store(kNoId);
	g_search_sent = kNoId;
	{
		std::lock_guard<std::mutex> lock(g_deal_lock); // TZ R90
		g_queued.clear();
		g_have_queued.store(false);
	}
	g_pending.clear();
	g_callbacks.clear();
	g_view.labels = false;
	g_view.rebuild_at = 0;
	g_view.moves.clear();
}

void on_peer_left(PeerId id)
{
	g_peers.erase(id); // his money stays on his character (P4, coop_money)
	g_open.erase(id);
	g_permits.revoke(id); // G6-3
}

bool client_transfer(CUIDragDropListEx* sell_list, CUIDragDropListEx* buy_list, CTrade* trade, bool trader_buys)
{
	if (!is_live_mirror() || !sell_list || !buy_list || !trade)
		return false;
	CInventoryOwner* trader = trade->pThis.inv_owner;
	CActor* a = Actor();
	if (!trader || !a || trade->pPartner.inv_owner != smart_cast<CInventoryOwner*>(a))
		return false;

	PendingDeal deal;
	// The trade window has a Sell and a Buy button (Call of Chernobyl split CoP's single "trade"):
	// each press is one deal with one list filled. A deal can carry both lists (one atomic barter).
	xr_vector<DealItem>& items = trader_buys ? deal.sell : deal.buy;
	// Same cell moves as CUIActorMenu::TransferItems: the window shows the deal at once; the host's
	// answer confirms it (ownership events) or the lists are rebuilt from the real inventories.
	while (sell_list->ItemsCount())
	{
		CUICellItem* cell = sell_list->RemoveItem(sell_list->GetItemIdx(0), false);
		PIItem item = (PIItem)cell->m_pData;
		items.push_back({item->object_id(), trade->GetItemPrice(item, trader_buys)});
		if (trader_buys)
		{
			SInvItemPlace pl;
			pl.type = eItemPlaceRuck;
			if (trader->CInventoryOwner::AllowItemToTrade(item, pl))
				buy_list->SetItem(cell);
		}
		else
			buy_list->SetItem(cell);
	}
	if (items.empty())
		return true;
	if (items.size() > kMaxDealItems)
	{
		::Msg("! [coop] trade: %u items in one deal, the limit is %u", (u32)items.size(), (u32)kMaxDealItems);
		request_rebuild(now_ms());
		return true;
	}
	client_send_deal(id_of(trader), deal);
	return true;
}

bool client_donate(CTrade* trade, CInventoryItem* item)
{
	if (!is_live_mirror() || !trade || !item)
		return false;
	CInventoryOwner* trader = trade->pThis.inv_owner;
	if (!trader)
		return false;
	PendingDeal deal;
	deal.donate = true;
	deal.sell.push_back({item->object_id(), 0});
	client_send_deal(id_of(trader), deal);
	return true;
}

int client_money_in_flight()
{
	if (!is_live_mirror())
		return 0;
	client_sync_world();
	s64 sum = 0;
	for (const auto& it : g_pending)
		sum += s64(it.second.sell_total) - s64(it.second.buy_total);
	return clamp_int(sum);
}

int client_trader_money_in_flight(CInventoryOwner* trader)
{
	if (!is_live_mirror() || !trader)
		return 0;
	client_sync_world();
	const u16 host_id = join::to_host(id_of(trader));
	s64 sum = 0;
	for (const auto& it : g_pending)
		if (it.second.trader == host_id)
			sum += s64(it.second.buy_total) - s64(it.second.sell_total);
	return clamp_int(sum);
}

bool lua_trade_window_allowed()
{
	// TZ R90: a copy gets the host's window once the Lua adapter has its Sell, Buy and Donate (coop_trade_window.ready()).
	const bool mirror = is_mirror();
	bool adapter = false;
	std::string said;
	if (mirror)
		call_trade_window("ready", [](lua_State*) { return 0; }, [&](lua_State* L, int at)
		{
			adapter = lua_toboolean(L, at) != 0;
			if (lua_type(L, at + 1) == LUA_TSTRING)
				said = lua_tostring(L, at + 1);
		});
	const bool lua = trade_window::choose(mirror, adapter) == trade_window::Window::Lua;
	if (net::live()) // which window a player saw, and why: the log answers "a different window than the host's"
	{
		const std::string tail = said.empty() ? std::string() : " (" + said + ")";
		::Msg("[coop] trade window: %s - %s%s", lua ? "Anomaly's Lua window, as the host's" : "the engine window",
		    trade_window::why(mirror, adapter), tail.c_str());
	}
	return lua;
}

int lua_trade_deal(lua_State* L)
{
	const auto refuse = [L](const char* why)
	{
		lua_pushnumber(L, 0);
		lua_pushstring(L, why);
		return 2;
	};
	if (!is_live_mirror())
		return refuse("not a player's copy in the world");
	u16 trader = kNoId;
	if (lua_type(L, 1) != LUA_TNUMBER || !trade_window::id_of(double(lua_tonumber(L, 1)), kNoId, trader))
		return refuse("no trader id");
	for (int at = 2; at <= 3; ++at)
		if (lua_istable(L, at) && lua_objlen(L, at) > kMaxDealItems)
			return refuse(trade_window::list_check_name(trade_window::ListCheck::TooMany));
	std::vector<uint16_t> sell, buy;
	if (!read_lua_ids(L, 2, sell) || !read_lua_ids(L, 3, buy))
		return refuse(trade_window::list_check_name(trade_window::ListCheck::BadId));
	const u8 flags = lua_type(L, 4) == LUA_TNUMBER ? u8(int(lua_tonumber(L, 4)) & trade_window::kDealFlags) : u8(0);
	const bool donate = (flags & kDealDonate) != 0;
	const trade_window::ListCheck c = trade_window::check_lists(sell, buy, donate, kMaxDealItems, kNoId);
	if (c != trade_window::ListCheck::Ok)
		return refuse(trade_window::list_check_name(c));
	std::vector<double> sell_prices, buy_prices;
	read_lua_prices(L, 5, sell.size(), sell_prices);
	read_lua_prices(L, 6, buy.size(), buy_prices);
	// Review of 2026-11-24: the sums the window shows (arguments 7 and 8: TMode_UpdatePrice's GetCellCost sums), else the item prices
	// added up. They fill the log line "this window showed" and, with flag 2, travel with the deal (protocol 35): TZ R92, its price.
	std::vector<double> sell_sum = sell_prices, buy_sum = buy_prices;
	if (lua_type(L, 7) == LUA_TNUMBER)
		sell_sum.assign(1, double(lua_tonumber(L, 7)));
	if (lua_type(L, 8) == LUA_TNUMBER)
		buy_sum.assign(1, double(lua_tonumber(L, 8)));

	PendingDeal deal;
	deal.donate = donate;
	deal.lua = true;
	deal.lua_prices = !donate && (flags & kDealLuaPrices) != 0;
	if (!donate && (!trade_window::total_of(sell_sum, u32(kMaxMoney), deal.sell_total) ||
	                   !trade_window::total_of(buy_sum, u32(kMaxMoney), deal.buy_total)))
		return refuse("a price of the window is no number");
	for (size_t i = 0; i < sell.size(); ++i)
		deal.sell.push_back({sell[i], donate ? 0u : trade_window::item_price(sell_prices[i])});
	for (size_t i = 0; i < buy.size(); ++i)
		deal.buy.push_back({buy[i], trade_window::item_price(buy_prices[i])});
	u32 seq = 0;
	{
		std::lock_guard<std::mutex> lock(g_deal_lock);
		seq = ++g_seq;
		g_queued.push_back({seq, trader, std::move(deal)});
		g_have_queued.store(true);
	}
	lua_pushnumber(L, lua_Number(seq));
	return 1;
}

void on_local_trade_done(CTrade* trade)
{
	if (!mode().leads_level() || !trade || !trade->pThis.inv_owner)
		return;
	host_broadcast_trader_money(trade->pThis.inv_owner, kInvalidPeer);
}

bool deal_list_stale(CUIDragDropListEx* deal_list, CInventoryOwner* owner)
{
	if (!net::live() || !deal_list || !owner)
		return false;
	const TIItemContainer& own = owner->inventory().m_all;
	auto owned = [&](void* data)
	{
		// Pointer comparison only: a stale cell may point to an item that is already destroyed.
		return std::find(own.begin(), own.end(), (PIItem)data) != own.end();
	};
	for (u32 i = 0, n = deal_list->ItemsCount(); i < n; ++i)
	{
		CUICellItem* cell = deal_list->GetItemIdx(i);
		if (!owned(cell->m_pData))
			return true;
		for (u32 k = 0; k < cell->ChildsCount(); ++k)
			if (!owned(cell->Child(k)->m_pData))
				return true;
	}
	return false;
}

int lua_search_window(lua_State* L)
{
	const lua_Number id = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : -1;
	const u16 now = id >= 0 && id < lua_Number(kNoId) ? u16(id) : kNoId;
	// TZ R90: kind 3 - a trade window with that trader (the decider sends his money, his items' states and a computer's permit)
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
			client_take_window(partner); // G6-3
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
	const u64 now = now_ms();

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
		request_rebuild(now_ms() + kRefreshMinMs); // a window's contents come in a burst
}

void on_container_move(u16 from_id, u16 to_id, u16 item_id)
{
	if (!net::live() || !g_view.shown)
		return;
	(void)from_id;
	if (g_view.moves.size() >= 512)
		g_view.moves.erase(g_view.moves.begin());
	g_view.moves.push_back({item_id, to_id, now_ms() + kMoveCheckMs});
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
		// L-1 round 2: on a client the host released the box (an emptied death bag) or a script did, with the window
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

// O2-partner: what this machine's windows show of `owner` (xrCoop/coop_partner_rules.h). Without a UI (a server's process) only
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

// O2-partner: does `c` to the windows that show `owner`, in the rules' order: the menu (a trade or an upgrade opened from a talk
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
		request_rebuild(now_ms());
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

bool trader_local_logic() { return !is_mirror(); }

// G6-3 (review m23:3): for CUIActorMenu::CheckDistance (the stock window closes 3 m from the trader). True: the trade window
// with `partner` stays open because this machine holds the grant for that trader through a trading computer, it has not run
// out and the actor stands within the stock 3 m of the computer (xrCoop/coop_trade_permit.h window_keeps). False: the stock
// rule. The hook in ui/UIActorMenu.cpp is the lead's (not this unit's file); without it the window keeps closing at 3 m.
bool keeps_trade_window(CInventoryOwner* partner)
{
	if (!partner || !g_view.shown || g_view.kind != ContTrader || g_view.id != id_of(partner))
		return false;
	const float to_computer = distance_to(Actor(), find_object(g_view.remote.computer));
	return trade_permit::window_keeps(g_view.remote, g_view.id, now_ms(), to_computer);
}

void print_status()
{
	if (mode().leads_level())
	{
		::Msg("[coop]   trade: %llu deals (%llu ok, %llu rejected), %llu items moved", g_host_deals, g_host_ok,
		    g_host_rejected, g_host_items);
		::Msg("[coop]   trade computer: %u permits held, %llu issued, %llu not kept, %llu deals let through", // G6-3
		    (u32)g_permits.size(), g_permits_issued, g_permits_dropped, g_permit_deals);
		for (const auto& it : g_open)
		{
			u32 sharing = 0;
			for (const auto& other : g_open)
				if (other.first != it.first && other.second.id == it.second.id)
					++sharing;
			::Msg("[coop]     #%u has %s %u open%s", it.first, container_name(it.second.kind), it.second.id,
			    sharing ? " (shared view)" : "");
		}
	}
	else if (is_mirror())
		::Msg("[coop]   trade: %llu deals sent (%llu ok, %llu rejected), %u waiting (%d RU)", g_cl_deals, g_cl_ok,
		    g_cl_rejected, (u32)g_pending.size(), client_money_in_flight());
	::Msg("[coop]   container view: %s %u, %llu rebuilds, %llu moves not applied", container_name(g_view.kind),
	    g_view.id, g_rebuilds, g_moves_failed);
}
} // namespace trade
} // namespace coop
