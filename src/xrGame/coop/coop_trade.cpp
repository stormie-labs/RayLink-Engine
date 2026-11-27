// Anomaly Coop — trade: the deals with NPC traders. See coop_trade.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_trade_impl.h"
#include "coop_dialog.h"
#include "coop_hud_text.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_sync.h"
#include "coop_requests.h"
#include "coop_money.h"
#include "coop_items.h"
#include "xrCoop/coop_talks.h"
#include "xrCoop/coop_reconcile.h"

#include "Level.h"
#include "Actor.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryBox.h"
#include "trade.h"
#include "trade_parameters.h"
#include "eatable_item.h"
#include "ai/trader/ai_trader.h"
#include "ai/stalker/ai_stalker.h"
#include "ai_space.h"
#include "alife_simulator.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "script_callback_ex.h"
#include "script_game_object.h"
#include "game_object_space.h"
#include "../xrServerEntities/xrMessages.h"
#include "ui/UIDragDropListEx.h"
#include "ui/UICellItem.h"

namespace coop
{
namespace trade
{
namespace
{
constexpr u16 kMaxDealItems = 1024;
constexpr u64 kPendingWarnMs = 15000; // client: a deal without answer is logged
constexpr s64 kMaxMoney = 0xffffffffll;
using trade_window::kDealDonate; // TradeOp Deal flags (xrCoop/coop_trade_window_rules.h)
using trade_window::kDealLuaPrices; // the Lua window's deal: its shown sums are the price

// The TradeOp sub-operations. Value 1 is not reused.
enum class Op : u8
{
	Result = 2,
	TraderMoney = 3,
	Deal = 4,
};

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

void write_trader_money(Writer& w, u16 trader_host_id, u32 money)
{
	w.u8(u8(Op::TraderMoney));
	w.u16(trader_host_id);
	w.u32(money);
}
} // namespace

CObject* find_object(u16 id) { return (g_pGameLevel && id != kNoId) ? Level().Objects.net_Find(id) : nullptr; }

u16 local_parent(u16 id)
{
	CObject* o = find_object(id);
	return (o && o->H_Parent()) ? o->H_Parent()->ID() : kNoId;
}

// A living character's items and an id out of thin air are nobody's business.
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
	        permits().covers(from, id, mgr().now_ms())); // the trader of his trading computer's permit
}

void host_send_trader_money(PeerId to, u16 trader_id)
{
	if (CInventoryOwner* trader = find_owner(trader_id))
	{
		Writer w(8);
		write_trader_money(w, trader_id, trader->get_money());
		net::host_send(to, Msg::TradeOp, w);
	}
}

namespace
{
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
	TooFar = 8, // the player's character is not by the trader on the host
	NotTraded = 9, // the trader does not buy / sell one of the items
	PriceChanged = 10, // the decider's formula and the window's sum differ beyond the tolerance, either way (settle_side)
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
	// PriceChanged only: the decider's price of each side (for the player's log and text; nothing agrees to it)
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

u64 g_host_deals = 0, g_host_ok = 0, g_host_rejected = 0, g_host_items = 0;

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
	bool lua = false; // sent by the Lua window (coop.trade_deal): its answer goes to coop_trade_window.on_result
	bool lua_prices = false; // flag kDealLuaPrices
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
	// A refusal for its price carries the decider's prices, for the player's log and text
	if (r.code == PriceChanged)
	{
		w.u32(r.decider_sell);
		w.u32(r.decider_buy);
	}
	net::host_send(to, Msg::TradeOp, w);
}

// May the player offer `item` to the trader? As the stock window decides (CUIActorMenu::CanMoveToPartner):
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

// The price of a list with the stock formula, as the player's own machine shows it to its actor:
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

// The decider's check of a player's deal against its own window's formula (ask_host_price), each side floored once as TMode_Sell
// does and compared with what the player's window showed (xrCoop/coop_trade_window_rules.h settle_side): the sums a Lua window sent,
// or for the engine window's deal (flag 0) the CTrade sums host_apply put there, which that window showed. Within the tolerance
// (tolerance_of: rounding and the condition's u16 steps; for CTrade also its floors) the side goes at the shown sum, as in single
// player; beyond it, either way, the deal is refused (PriceChanged) with the decider's sums in d.sell_decider / buy_decider, for the log.
// The formula is the decider's, not the flag's (check_of): flag 0 is no way around the Lua window's formula. No fallback price for a
// Lua window's deal: no answer refuses it; the engine window's deal keeps its CTrade sums only where this decider has no Lua formula.
u8 lua_price_deal(u16 proxy_id, u16 trader_id, Deal& d)
{
	double raw[2] = {0.0, 0.0};
	std::string why;
	const trade_window::Answer answer = ask_host_price(proxy_id, trader_id, d, raw, why);
	const trade_window::Check check = trade_window::check_of(d.lua_prices, answer);
	if (check == trade_window::Check::EngineOnly)
		return Ok; // d.sell_total, d.buy_total: the engine formula's
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
// player's is his balance, coop_money). Both sides are checked after the whole deal, so what
// the player sells pays for what he buys and the other way round. The decider sets what the deal moves
// (d.sell_total, d.buy_total): the engine window's deal carries item ids only and goes at the CTrade sums
// here, a Lua window's deal at the sums it showed; either only once the formula here agrees.
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
	// The counterparty is one the player trades with now (talks::may_trade_with): its stock trade flag is on, or,
	// on Anomaly 1.5.3 where xr_meet keeps that flag off and trade opens from the dialog, the player talked to it; and it
	// stands by the player's character.
	// Or the player holds the decider's permit for this trader through a trading computer he stands at
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

	// Every item once, and still where the client saw it, in deal order (the sold list, then the bought one): the first item that
	// is a repeat (Malformed) or gone (ItemGone) answers. The repeat is found by sorting (id, place): one deal may name 1024 ids.
	struct Place
	{
		u16 id;
		u32 at;
	};
	xr_vector<Place> places;
	places.reserve(n);
	for (u16 id : d.sell)
		places.push_back({id, u32(places.size())});
	for (u16 id : d.buy)
		places.push_back({id, u32(places.size())});
	std::sort(places.begin(), places.end(), [](const Place& a, const Place& b) { return a.id != b.id ? a.id < b.id : a.at < b.at; });
	u32 first_repeat = u32(n);
	for (size_t i = 1; i < places.size(); ++i)
		if (places[i].id == places[i - 1].id && places[i].at < first_repeat)
			first_repeat = places[i].at;
	u32 at = 0;
	const auto check = [&](const xr_vector<u16>& list, u16 holder) -> u8
	{
		for (u16 id : list)
		{
			if (at++ == first_repeat)
				return Malformed;
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
	// A deal goes at what the player's window showed, as in single player, once the decider's own window's formula (computed as for
	// itself) agrees with it within the tolerance (lua_price_deal, settle_side). A Lua window's deal carries its sums. The engine
	// window's deal (flag 0) carries item ids only: what it showed is the CTrade sums for the player's character (its own formula; a
	// temporary CTrade, the trader's own one may hold the host actor's trade with him), and they meet the formula the same way, with
	// that window's tolerance. The player's choice of window chooses no formula.
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
		d.sell_shown = d.sell_total; // what the engine window showed (its formula for this character, as here)
		d.buy_shown = d.buy_total;
		if (const u8 c = lua_price_deal(proxy_id, trader_id, d))
			return c;
		break;
	}
	}

	// The player's money is his balance on the host, never a figure from the client.
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
		money::host_adjust(from, net, "trade"); // MoneyBalance reaches the player before the Result
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

// After a refusal for its price the player's window takes the trader's data again without closing its view: a close revokes a
// trading computer's permit (host_on_container), and the grant behind it is taken once (coop_mods_trade_computer.script take_grant),
// so every later deal in that window would be TooFar. The decider sends that data with the refusal instead, ahead of the Result and
// on the same reliable lane: the states of the trader's items (as a new view gets them) and his money (as host_on_container sends
// it). They are on the copy when the Result reaches its window, which draws itself anew. Nothing while the player joins, nothing of
// a trader he may not see.
void host_send_trader_view(PeerId to, u16 trader_id)
{
	if (sync::host_peer_joining(to) || !may_see_contents(to, ContTrader, trader_id))
		return;
	items::host_send_contents(to, trader_id);
	host_send_trader_money(to, trader_id);
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
	d.lua_prices = (flags & kDealLuaPrices) != 0;
	bool parsed = read_list(r, d.sell) && read_list(r, d.buy);
	if (parsed && d.lua_prices) // the sums the player's window showed follow the lists: the price
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
		host_send_trader_view(from, d.trader); // the trader's items and money first, then the answer
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

// coop.trade_deal may run on the game's worker thread (Lua does, see lua_search_window): a deal of the Lua window is queued with
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
	// Item moves and money changes (ReqMoney) made before the deal go out first: on the one
	// reliable lane the host checks the deal against them.
	requests::client_flush_outbox();
	// The engine window's prices stay here (the money pre-checks of the trade buttons); the decider prices its deal with CTrade.
	// The Lua window's sums are the ones it showed, floored once (lua_trade_deal): the deal's price.
	if (!deal.lua)
	{
		deal.sell_total = list_total(deal.sell);
		deal.buy_total = list_total(deal.buy);
	}
	deal.trader = join::to_host(trader_local);
	deal.sent_at = mgr().now_ms();

	// No money of ours in the deal (a Lua window's shown sums are prices); the host checks it against our balance there.
	Writer w(32 + (deal.sell.size() + deal.buy.size()) * 2);
	w.u8(u8(Op::Deal));
	w.u32(seq);
	w.u16(deal.trader);
	w.u8(u8((deal.donate ? kDealDonate : 0) | (deal.lua_prices ? kDealLuaPrices : 0)));
	write_list(w, deal.sell);
	write_list(w, deal.buy);
	if (deal.lua_prices) // the sums the window showed: the price, which the decider checks
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
	u32 decider_sell = 0, decider_buy = 0; // the decider's prices of a deal refused for its price (log and text)
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
		// The money moved is the decider's: what a Lua window showed, the CTrade sums there for the engine
		// window's deal. A difference means the trader's profile or the relations differ between the machines.
		const s64 shown = s64(deal.sell_total) - s64(deal.buy_total);
		if (shown != delta)
			::Msg("[coop] trade #%u: the host priced it %d RU, this window showed %lld RU", seq, delta, shown);
		if (a)
		{
			// The money came before this answer (MoneyBalance from the host's balance), nothing to set.
			// Lua sees the trade of the local actor as in single player (actor_on_trade), see frame(),
			// with the item prices this window showed.
			const u64 deadline = mgr().now_ms() + kCallbackWaitMs;
			const u16 trader_local = join::to_local(trader_host);
			// The window shows the deal already (client_transfer moved the cells), and the stock trader-inventory watch
			// (CUIActorMenu::Update) refills the trader's list when the items leave him: the lists are rebuilt again only
			// if an item has not arrived where the deal put it (refresh_actor_menu, kMoveCheckMs). A settle rebuild of both
			// lists would be a second hitch after every purchase.
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
			request_rebuild(mgr().now_ms());
		}
	}
	else
	{
		++g_cl_rejected;
		::Msg("[coop] trade #%u rejected by the host: %s", seq, result_name(code));
		if (code == NoMoney)
			hud_text::say("st_coop_trade_no_money", "Trade failed: not enough money");
		else if (code == TraderNoMoney)
			hud_text::say("st_coop_trade_trader_no_money", "Trade failed: the trader has not enough money");
		else if (code == TooFar)
			hud_text::say("st_coop_trade_too_far", "Trade failed: you are too far from the trader");
		else if (code == NoTrader)
			hud_text::say("st_coop_trade_no_trader", "Trade failed: the trader does not trade now");
		else if (code == NotTraded)
			hud_text::say("st_coop_trade_not_traded", "Trade failed: the trader does not trade this item");
		else if (code == PriceChanged)
		{
			// The decider's formula and this window's sums differ beyond the tolerance; nothing agrees to the decider's price.
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
					hud_text::say_n("st_coop_trade_price_buy",
					    "Trade cancelled: the trader asks %u RU, more than the window showed", decider_buy);
					break;
				case trade_window::PriceTell::Sale:
					hud_text::say_n("st_coop_trade_price_sell",
					    "Trade cancelled: the trader pays %u RU, less than the window showed", decider_sell);
					break;
				case trade_window::PriceTell::Stale:
					hud_text::say("st_coop_trade_price_stale",
					    "Trade cancelled: the prices in the window are out of date, the window is refreshed");
					break;
				}
		}
		else
			hud_text::say("st_coop_trade_failed", "Trade failed: the goods are no longer available");
		request_rebuild(mgr().now_ms()); // undo the optimistic move of the cells
	}
	g_view.labels = true;
	if (deal.lua) // the Lua window's buttons wait for this answer; it follows the items that come
		window_on_result(seq, code, delta, result_name(code), decider_sell, decider_buy);
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

void frame()
{
	search_frame();
	if (g_have_queued.load()) // the Lua window's deals, sent on the main thread
	{
		std::lock_guard<std::mutex> lock(g_deal_lock);
		send_queued_locked();
	}
	if (!is_mirror())
		return;
	client_sync_world();
	if (!is_live_mirror())
		return;
	const u64 now = mgr().now_ms();
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
	g_permits.clear();
	g_search_now.store(kNoId);
	g_search_sent = kNoId;
	{
		std::lock_guard<std::mutex> lock(g_deal_lock);
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
	g_permits.revoke(id);
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
		request_rebuild(mgr().now_ms());
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
	// The sums the window shows (arguments 7 and 8: TMode_UpdatePrice's GetCellCost sums), else the item prices added up. They fill
	// the log line "this window showed" and, with flag 2, travel with the deal: its price.
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

bool trader_local_logic() { return !is_mirror(); }

void print_status()
{
	if (mode().leads_level())
	{
		::Msg("[coop]   trade: %llu deals (%llu ok, %llu rejected), %llu items moved", g_host_deals, g_host_ok,
		    g_host_rejected, g_host_items);
		::Msg("[coop]   trade computer: %u permits held, %llu issued, %llu not kept, %llu deals let through",
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
