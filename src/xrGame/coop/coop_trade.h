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
//   4 Deal         client -> host  u32 seq, u16 trader, u8 flags (1 donate),
//                                  u16 n, n x u16 item   (player -> trader)
//                                  u16 n, n x u16 item   (trader -> player)
//   2 Result       host -> client  u32 seq, u16 trader, u8 TradeResult (0 ok), i32 money_delta
//                                  (sell_total - buy_total), u32 trader_money
//   3 TraderMoney  host -> all     u16 trader, u32 money   (after any trade; answer to an open)
//   1 (retired single-direction request; ignored)
// A repeated Deal (same seq) gets the stored answer again and changes nothing.
// Prices (review R4 2): the host prices every item with the stock CTrade::GetItemPrice for the
// player's character (its relations, rank, reputation, faction) as for the actor, and checks that
// the trader trades now, stands by that character and trades each item; the client's own prices
// only fill its window.
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

class CTrade;
class CUIActorMenu;
class CUIDragDropListEx;
class CInventoryItem;
class CInventoryOwner;

namespace coop
{
class Session;

namespace trade
{
// R4 2: player character <-> trader on the host. The stock trade window closes beyond 3 m
// (CUIActorMenu::CheckDistance); twice that for the lag of the player's character on the host. Also the reach of a
// mechanic who sells a client an upgrade (F2 T5, coop_requests).
constexpr float kTradeReach = 6.f;

void install(Session& s);
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
// For CUIGameSP::StartTrade (not wired yet, see the W3b report): false on a coop client, which must
// trade in the engine window. Anomaly's Lua inventory (ui_inventory.script) replaces that window and
// trades with transfer_item + give_money, which bypasses this module (no money check on the host,
// trader money changed on the client only, items moved one by one).
bool lua_trade_window_allowed();

// --- engine hooks: actor menu (ui/UIActorMenu.cpp, ui/UIActorMenuDeadBodySearch.cpp) ---
// CUIActorMenu::Show(status), first line (mode, partner and box are already set when opening).
void on_actor_menu_shown(CUIActorMenu& menu, bool shown);
// CUIActorMenu::Update, first line. Rebuilds the lists through the menu's public API when a shared
// container changed elsewhere, a trade answer arrived or a container move was not applied.
// True: the caller refreshes prices, weights and money labels (UpdateItemsPlace).
bool refresh_actor_menu(CUIActorMenu& menu);
// move_item_from_to (container windows): the move is checked after a while; not applied -> rebuild.
void on_container_move(u16 from_id, u16 to_id, u16 item_id);

// For the item layer (WP2): a take/put request was rejected by the host; rebuild the open window.
void invalidate_views();

// --- engine hooks: trader NPC (ai/trader/ai_trader.cpp) ---
// False on a coop client: the trader's own world changes (picking items up, dropping, rejecting an
// item it was given) are the host's; the client only mirrors them. True otherwise.
bool trader_local_logic();
} // namespace trade
} // namespace coop
