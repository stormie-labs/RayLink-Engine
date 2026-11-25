// Anomaly Coop — trade with NPCs and shared containers (SPEC R-8.2, R-6.1, R-6.5, P1, P6).
//
// Trade: a trader's stock and money live on the host (P1). On a client the trade window works as
// usual, but the deal goes to the host as one request; the host moves the items between the trader
// and the player's proxy (same ids) with the engine's own ownership events, which reach every
// machine through the ownership mirror, moves the trader's money, and answers. Everything or
// nothing: one deal carries what the player sells and what he buys; if one item is no longer where
// the client saw it or one side cannot pay after the other side's payment, nothing happens, so of
// two players buying the last item only one gets it. Player money lives on the host too (F2 T1,
// review R4 7; coop_money): the deal is checked against the player's balance there, never against
// a figure from the client, and the host changes that balance (money::host_adjust), which reaches
// the player as MoneyBalance before the deal's Result. The money changes and item moves the client
// made before a deal leave first (the requests and money outboxes are flushed, one reliable lane),
// so "sell A, then buy B" before the sale is answered is checked against the money after the sale.
//
// TradeOp payload, first byte is the sub-operation (ids in host numbering, reliable lane):
//   4 Deal         client -> host  u32 seq, u16 trader, u8 flags (1 donate, 2 the Lua window's prices: TZ R90),
//                                  u16 n, n x u16 item   (player -> trader)
//                                  u16 n, n x u16 item   (trader -> player)
//                                  flag 2 only (protocol 35): u32 sell_shown, u32 buy_shown   (what the window showed)
//   2 Result       host -> client  u32 seq, u16 trader, u8 TradeResult (0 ok), i32 money_delta
//                                  (sell_total - buy_total), u32 trader_money
//                                  code 10 only: u32 decider_sell, u32 decider_buy   (PriceChanged, TZ R90, review of
//                                  2026-11-25; protocol 35, the same Dev build: the decider's prices, which the window shows)
//   3 TraderMoney  host -> all     u16 trader, u32 money   (after any trade; answer to an open)
//   1 (retired single-direction request; ignored)
// A repeated Deal (same seq) gets the stored answer again and changes nothing.
// Prices (review R4 2): the host prices every item with the stock CTrade::GetItemPrice for the
// player's character (its relations, rank, reputation, faction) as for the actor, and checks that
// the trader trades now, stands by that character and trades each item; the client's own prices
// only fill its window. A deal from Anomaly's Lua window (flag 2, TZ R90) is priced with that window's
// formula instead, as the host's own window prices his trades: scripts\coop_trade_window.script
// host_price under the player's character (the trader's lists and discounts for him, get_item_cost),
// then each side is settled against the sum the player's window showed (review of 2026-11-24,
// xrCoop/coop_trade_window_rules.h settle_side): a sale pays the smaller sum, a purchase costs the
// larger, so the player never gets more nor pays less than his window showed or the formula here says.
// Review of 2026-11-25: nor does he pay more or get less than his window showed without seeing it: a
// side the decider prices worse for him than shown beyond rounding (1 RU; for the engine window's
// CTrade sums 1 RU + 2 RU an item, trade_window::slack_of) is refused with TradeResult 10
// (PriceChanged) and the decider's prices; his Lua window shows them, its next press agrees. Within
// the rounding a purchase costs what was shown and a sale pays the decider's price.
// The formula is the decider's, not the player's choice of window: an engine window's deal (flag 0) is
// priced with CTrade (what that window showed) and then settled against the Lua formula the same way;
// CTrade alone only where the decider has no coop_trade_window.host_price. Host_price prices the
// player's items as his own window sees them, held by the actor (id 0): Weapon Parts Overhaul's checks
// of the owner's id give the worn part's price and the busted-parts refusal there too.
// Flag 2, the shown sums and the tail of PriceChanged are protocol 35: a reader of 34 never meets them (Hello).
//
// Containers (boxes, stashes, corpses) have one content for all players. Takes and puts are
// ownership events that the item layer (WP2) turns into host requests. Here: the open window
// follows the content. When the container's content changes for any reason (another player, a
// host result, a rejected request), the lists of the open window are rebuilt from the real
// inventory, so two players in one box always see the same thing.
//   ReqContainer   client -> host  u8 open, u8 kind (1 box, 2 corpse, 3 trader), u16 id
// The host keeps who has which container open (coop_status) and answers a trader open with
// TraderMoney. No locks: the host resolves conflicting takes.
//
// Included from UI and AI files: keep it free of engine includes.
#pragma once

#include "xrCoop/coop_protocol.h"

struct lua_State;
class CTrade;
class CUIActorMenu;
class CUIDragDropListEx;
class CInventoryItem;
class CInventoryOwner;
class CInventoryBox;

namespace coop
{
class Session;
class Endpoint;

namespace trade
{
// R4 2: player character <-> trader on the host. The stock trade window closes beyond 3 m
// (CUIActorMenu::CheckDistance); twice that for the lag of the player's character on the host. Also the reach of a
// mechanic who sells a client an upgrade (F2 T5, coop_requests).
constexpr float kTradeReach = 6.f;

void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void frame(); // main thread, every frame
void reset(); // session started / ended / world replaced
void on_peer_left(PeerId id);
void print_status();

// --- engine hooks: trade window (ui/UIActorMenuTrade.cpp) ---
// CUIActorMenu::TransferItems. Client in a session: sends the deal to the host, moves the cells
// like the local trade would, returns true (the caller must not trade locally). Else false.
// `trade` is the partner's CTrade (pThis = trader, pPartner = our actor); `trader_buys` as there.
bool client_transfer(CUIDragDropListEx* sell_list, CUIDragDropListEx* buy_list, CTrade* trade, bool trader_buys);
// CUIActorMenu::DonateCurrentItem. Client: asks the host to give `item` to the trader; true = handled.
bool client_donate(CTrade* trade, CInventoryItem* item);
// End of CUIActorMenu::TransferItems on the host (a local trade): tells clients the trader's money.
void on_local_trade_done(CTrade* trade);
// CUIActorMenu::InitPartnerInventoryContents: true if the deal list holds an item that `owner`
// no longer has (bought by another player). The caller clears the deal list.
bool deal_list_stale(CUIDragDropListEx* deal_list, CInventoryOwner* owner);
// OnBtnPerformTradeBuy/Sell pre-checks: money that deals still waiting for the host's answer will
// move, as if they all succeed (the host checks each deal exactly). Player: sum of sold - bought
// over all waiting deals; trader: the opposite over the deals with `trader`. 0 unless a coop client.
int client_money_in_flight();
int client_trader_money_in_flight(CInventoryOwner* trader);
// CUIGameSP::StartTrade (TZ R90): may Anomaly's Lua inventory window (ui_inventory, G.A.M.M.A.'s copy) open this trade? Yes in a
// process that trades for itself (the host, single player), and on a copy (a co-op client, a server's player) once
// scripts\coop_trade_window.script has taken over the window's Sell, Buy and Donate (coop_trade_window.ready()): they send one deal
// to the decider (lua_trade_deal) instead of transfer_item + give_money on the copy, which would move the trader's money there only.
// Else the copy keeps the engine window. In a session the log says which window opened and why ("[coop] trade window: ...").
bool lua_trade_window_allowed();
// coop.trade_deal(trader, sell_ids, buy_ids, flags[, sell_prices, buy_prices, sell_shown, buy_shown]) -> seq, or 0 and why (TZ R90):
// the trade of the Lua window of a copy. Ids in local numbering; flags 1 a gift, 2 priced with the window's formula; the item prices
// are the ones the window showed (actor_on_trade only), the two sums what it showed for each side (GetCellCost: the log line and,
// with flag 2, the deal: the decider never pays more nor charges less than them; without them, the item prices added up). Queued, sent by frame() on the main thread (Lua may run on
// the game's worker thread). The answer goes to coop_trade_window.on_result(seq, code, money_delta, why, decider_sell, decider_buy)
// (the last two: PriceChanged only, else 0).
int lua_trade_deal(lua_State* L);

// --- engine hooks: actor menu (ui/UIActorMenu.cpp, ui/UIActorMenuDeadBodySearch.cpp) ---
// CUIActorMenu::Show(status), first line (mode, partner and box are already set when opening).
void on_actor_menu_shown(CUIActorMenu& menu, bool shown);
// coop.search_window([id[, kind]]): Anomaly's inventory, a Lua dialog, searches `id` now, no id: it closed (coop_mods_items.script, xrCoop/
// coop_search_view.h). Kind 3 (TZ R90, coop_trade_window.script): a trade window with the trader `id` (the decider sends his money,
// his items' states and a trading computer's permit). frame() tells the host as CUIActorMenu::Show tells it of the engine's window.
// Any thread.
int lua_search_window(lua_State* L);
// CUIActorMenu::Update, first line. Rebuilds the lists through the menu's public API when a shared
// container changed elsewhere, a trade answer arrived or a container move was not applied.
// True: the caller refreshes prices, weights and money labels (UpdateItemsPlace).
bool refresh_actor_menu(CUIActorMenu& menu);
// G6-3, CUIActorMenu::CheckDistance: true keeps the trade window of a trading computer's permit open beyond 3 m.
bool keeps_trade_window(CInventoryOwner* partner);
// Client: the host's state of a world item came (F1): a window open on `holder` (local numbering, the item's top holder)
// shows its bars and prices again. Teammates' items change with every shot: the rest is no business of a window.
void client_on_item_state(u16 holder);
// move_item_from_to (container windows): the move is checked after a while; not applied -> rebuild.
void on_container_move(u16 from_id, u16 to_id, u16 item_id);
// CInventoryBox::net_Destroy, first line, on every machine and in single player (L-1 round 2). The actor menu keeps a
// raw pointer to the box it searches (CUIActorMenu::m_pInvBox) and reads it while shown (refresh_actor_menu, the stock
// lists, its DeInit). If `box` is that box, a shown menu is closed now, while the box is still whole (HideActorMenu:
// the window and Lua's hide hook, as the stock item_backpack does before its release; Show(false) runs at once), and the
// menu forgets the box. A menu closed already (the stock path) only forgets it: no second close.
void on_inv_box_destroy(CInventoryBox& box);
// CGameObject::net_Destroy of an inventory owner (an NPC, a trader, a corpse, the actor), its first line, on every machine and in
// single player (O2-partner; crash reports RL-8CGN-EU7U and RL-AYXZ-JTV4 of build 56). The talk window, the actor menu and the
// actor's talk keep raw pointers to their partner and read them every frame and on their close (CUITalkWnd::StopSnd hands it to
// CActor::OnDialogSoundHandlerStop, whose smart_cast is a dynamic_cast; CUIActorMenu::Update reads the trader's inventory). If
// `owner` is that partner, a shown menu closes now (HideActorMenu, as on_inv_box_destroy) and forgets it, the actor stops talking to
// it and a shown talk window with it hides (its Show(false) runs at once): all while the owner is still whole
// (xrCoop/coop_partner_rules.h on_destroy). A process without windows (a world server, a map server) only stops its actor's talk.
void on_inv_owner_destroy(CInventoryOwner& owner);
// coop_dialog client_on_revoke: the decider took back the talk lock on `partner` (the local object). The talk stops, and a trade or
// an upgrade window with it, opened from that talk, closes (xrCoop/coop_partner_rules.h on_lock_revoked). True if either happened.
bool on_talk_lock_revoked(CInventoryOwner& partner);

// For the item layer (WP2): a take/put request was rejected by the host; rebuild the open window.
void invalidate_views();
// Host: container `id` (host numbering) is open in somebody's window now, a client's (his ReqContainer) or the host's
// own. Such a window keeps a raw pointer to the container (CUIActorMenu::m_pInvBox) and reads it every frame, so the
// host does not release it meanwhile (coop_death's empty death bags, L-1 round 1, I1). False on a client.
bool host_container_open(u16 id);

// --- engine hooks: trader NPC (ai/trader/ai_trader.cpp) ---
// False on a coop client: the trader's own world changes (picking items up, dropping, rejecting an
// item it was given) are the host's; the client only mirrors them. True otherwise.
bool trader_local_logic();
} // namespace trade
} // namespace coop
