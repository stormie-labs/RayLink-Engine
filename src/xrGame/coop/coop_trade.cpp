// Anomaly Coop — trade with NPCs and shared containers. See coop_trade.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_trade.h"
#include "coop_dialog.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_requests.h"
#include "coop_script.h"
#include "coop_money.h"

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
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "script_callback_ex.h"
#include "script_game_object.h"
#include "game_object_space.h"
#include "../xrServerEntities/xrMessages.h"
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
constexpr u64 kSettleMs = 1500; // after a successful deal, one consistency rebuild
constexpr u64 kPendingWarnMs = 15000; // client: a deal without answer is logged
constexpr s64 kMaxMoney = 0xffffffffll;
constexpr u8 kDealDonate = 1;
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
};

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
	CGameObject::u_EventGen(P, GE_TRADE_SELL, from_id);
	P.w_u16(item_id);
	CGameObject::u_EventSend(P);
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
	xr_vector<u16> sell; // player -> trader
	xr_vector<u16> buy; // trader -> player
	// Priced by the host (R4 2), not sent by the client.
	u32 sell_total = 0; // the trader pays the player
	u32 buy_total = 0; // the player pays the trader
};

struct OpenView
{
	u8 kind = ContNone;
	u16 id = kNoId;
};
xr_map<PeerId, OpenView> g_open; // host: containers open per player (the host itself at kHostPeer)

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
};
xr_vector<TradeCallback> g_callbacks;
constexpr u64 kCallbackWaitMs = 3000;
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
};
ViewState g_view;
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
	Writer w(20);
	w.u8(u8(Op::Result));
	w.u32(r.seq);
	w.u16(r.trader);
	w.u8(r.code);
	w.i32(r.delta);
	w.u32(r.trader_money);
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

// One deal, everything or nothing. Money: the trader's and the player's both live on the host (the
// player's is his balance, coop_money; F2 T1). Both sides are checked after the whole deal, so what
// the player sells pays for what he buys and the other way round. The host prices the deal
// (d.sell_total, d.buy_total); the client sends item ids only (R4 2).
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
	// R4 2: the counterparty is one the player trades with now: its trade flag is on (the stock talk window
	// offers trade only then; wounded NPCs and meet logic turn it off) and it stands by the player's character.
	if (!trader->IsTradeEnabled())
		return NoTrader;
	if (!trader->NeedOsoznanieMode() && !within_reach(proxy, trader, kTradeReach))
		return TooFar;
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

	// R4 2: the host's prices. A temporary CTrade: the trader's own one may hold the host actor's trade with him.
	// A gift is free, as the stock DonateCurrentItem (TransferItem with b_free).
	d.sell_total = d.buy_total = 0;
	if (!d.donate)
	{
		CTrade pricing(trader);
		pricing.SetActorPartner(proxy);
		if (const u8 c = price_list(pricing, trader, d.sell, true, d.sell_total))
			return c;
		if (const u8 c = price_list(pricing, trader, d.buy, false, d.buy_total))
			return c;
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

	for (u16 id : d.sell)
	{
		CInventoryItem* item = smart_cast<CInventoryItem*>(find_object(id));
		proxy->on_before_sell(item);
		trader->on_before_buy(item);
		move_item(proxy_id, trader_id, id);
	}
	for (u16 id : d.buy)
	{
		CInventoryItem* item = smart_cast<CInventoryItem*>(find_object(id));
		trader->on_before_sell(item);
		proxy->on_before_buy(item);
		move_item(trader_id, proxy_id, id);
	}
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

void host_on_deal(PeerId from, Reader& r)
{
	Deal d;
	d.seq = r.u32();
	d.trader = r.u16();
	const u8 flags = r.u8();
	if (!r.ok())
		return; // no seq to answer
	d.donate = (flags & kDealDonate) != 0;
	const bool parsed = read_list(r, d.sell) && read_list(r, d.buy);

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
	host_send_result(from, last);

	const PeerInfo* info = mgr().session().peer(from);
	::Msg("[coop] trade #%u <%s> with %u: %s %u item(s) for %u RU, buys %u item(s) for %u RU: %s", d.seq,
	    info ? info->nick.c_str() : "?", d.trader, d.donate ? "gives" : "sells", (u32)d.sell.size(), d.sell_total,
	    (u32)d.buy.size(), d.buy_total, result_name(code));
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
		return;
	}
	OpenView& v = g_open[from];
	v.kind = kind;
	v.id = id;
	if (kind == ContBox || kind == ContCorpse)
	{
		// Host Lua (W5, e.g. loot addons): SendScriptCallback("coop_on_container_opened", peer, id).
		CObject* o = find_object(id);
		const bool real = o && (kind == ContBox ? smart_cast<CInventoryBox*>(o) != nullptr
		                                        : (smart_cast<CInventoryOwner*>(o) && !alive(smart_cast<CInventoryOwner*>(o))));
		if (real && !sync::host_peer_joining(from))
			script::on_container_opened(from, id);
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

void client_send_deal(u16 trader_local, PendingDeal& deal)
{
	client_sync_world();
	// Item moves and money changes (ReqMoney, F2 T1) made before the deal go out first: on the one
	// reliable lane the host checks the deal against them.
	requests::client_flush_outbox();
	// Our own prices stay here (the money pre-checks of the trade buttons); the host prices the deal (R4 2).
	deal.sell_total = list_total(deal.sell);
	deal.buy_total = list_total(deal.buy);
	const u32 seq = ++g_seq;
	deal.trader = join::to_host(trader_local);
	deal.sent_at = now_ms();

	// F2 T1: no money of ours in the deal; the host checks it against our balance there.
	Writer w(32 + (deal.sell.size() + deal.buy.size()) * 2);
	w.u8(u8(Op::Deal));
	w.u32(seq);
	w.u16(deal.trader);
	w.u8(deal.donate ? kDealDonate : 0);
	write_list(w, deal.sell);
	write_list(w, deal.buy);
	net::client_send(Msg::TradeOp, w);
	g_pending[seq] = std::move(deal);
	++g_cl_deals;
}

void client_on_result(Reader& r)
{
	const u32 seq = r.u32();
	const u16 trader_host = r.u16();
	const u8 code = r.u8();
	const s32 delta = r.i32();
	const u32 trader_money = r.u32();
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
		// R4 2: the money moved is the host's price; ours only showed it. A difference means the trader's
		// profile or the relations differ between the machines.
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
			for (const DealItem& di : deal.sell)
				g_callbacks.push_back({di.id, trader_local, true, di.price, deadline});
			for (const DealItem& di : deal.buy)
				g_callbacks.push_back({di.id, a->ID(), false, di.price, deadline});
		}
		else
			::Msg("! [coop] trade #%u accepted while there is no actor here", seq);
		request_rebuild(now_ms() + kSettleMs);
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
		else
			dialog::hud_message("st_coop_trade_failed", "Trade failed: the goods are no longer available");
		request_rebuild(now_ms()); // undo the optimistic move of the cells
	}
	g_view.labels = true;
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
		if (mgr().is_host() && from != kHostPeer)
			host_on_deal(from, r);
		break;
	case Op::Result:
		if (mgr().is_client())
			client_on_result(r);
		break;
	case Op::TraderMoney:
		if (mgr().is_client())
			client_on_trader_money(r);
		break;
	default:
		break;
	}
}

void send_container(bool open, u8 kind, u16 local_id)
{
	if (mgr().is_host())
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
	if (!mgr().is_live_client())
		return;
	Writer w(6);
	w.u8(open ? 1 : 0);
	w.u8(kind);
	w.u16(join::to_host(local_id));
	net::client_send(Msg::ReqContainer, w);
}
} // namespace

// ---------------------------------------------------------------- API

void install(Session& s)
{
	s.on(Msg::TradeOp, &on_trade_op);
	s.on(Msg::ReqContainer, [](PeerId from, Reader& r)
	{
		if (mgr().is_host())
			host_on_container(from, r);
	});
}

void frame()
{
	if (!mgr().is_client())
		return;
	client_sync_world();
	if (!mgr().is_live_client())
		return;
	const u64 now = now_ms();
	for (auto it = g_callbacks.begin(); it != g_callbacks.end();)
	{
		if (local_parent(it->item) != it->expect_parent && now < it->deadline)
		{
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
}

bool client_transfer(CUIDragDropListEx* sell_list, CUIDragDropListEx* buy_list, CTrade* trade, bool trader_buys)
{
	if (!mgr().is_live_client() || !sell_list || !buy_list || !trade)
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
	if (!mgr().is_live_client() || !trade || !item)
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
	if (!mgr().is_live_client())
		return 0;
	client_sync_world();
	s64 sum = 0;
	for (const auto& it : g_pending)
		sum += s64(it.second.sell_total) - s64(it.second.buy_total);
	return clamp_int(sum);
}

int client_trader_money_in_flight(CInventoryOwner* trader)
{
	if (!mgr().is_live_client() || !trader)
		return 0;
	client_sync_world();
	const u16 host_id = join::to_host(id_of(trader));
	s64 sum = 0;
	for (const auto& it : g_pending)
		if (it.second.trader == host_id)
			sum += s64(it.second.buy_total) - s64(it.second.sell_total);
	return clamp_int(sum);
}

bool lua_trade_window_allowed() { return !mgr().is_client(); }

void on_local_trade_done(CTrade* trade)
{
	if (!mgr().is_host() || !trade || !trade->pThis.inv_owner)
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

void on_container_move(u16 from_id, u16 to_id, u16 item_id)
{
	if (!net::live() || !g_view.shown)
		return;
	(void)from_id;
	if (g_view.moves.size() >= 512)
		g_view.moves.erase(g_view.moves.begin());
	g_view.moves.push_back({item_id, to_id, now_ms() + kMoveCheckMs});
}

void invalidate_views()
{
	if (g_view.shown)
		request_rebuild(now_ms());
}

bool host_container_open(u16 id)
{
	if (!mgr().is_host() || id == kNoId)
		return false;
	for (const auto& it : g_open)
		if (it.second.kind != ContNone && it.second.id == id)
			return true;
	return false;
}

bool trader_local_logic() { return !mgr().is_client(); }

void print_status()
{
	if (mgr().is_host())
	{
		::Msg("[coop]   trade: %llu deals (%llu ok, %llu rejected), %llu items moved", g_host_deals, g_host_ok,
		    g_host_rejected, g_host_items);
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
	else if (mgr().is_client())
		::Msg("[coop]   trade: %llu deals sent (%llu ok, %llu rejected), %u waiting (%d RU)", g_cl_deals, g_cl_ok,
		    g_cl_rejected, (u32)g_pending.size(), client_money_in_flight());
	::Msg("[coop]   container view: %s %u, %llu rebuilds, %llu moves not applied", container_name(g_view.kind),
	    g_view.id, g_rebuilds, g_moves_failed);
}
} // namespace trade
} // namespace coop
