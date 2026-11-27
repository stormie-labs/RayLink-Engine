// Anomaly Coop — trade: what the deals (coop_trade.cpp), the views (coop_trade_views.cpp) and the Lua trade window with the
// trading computer's permits (coop_trade_window_lua.cpp) share. Private to those three files.
#pragma once

#include "coop_trade.h"
#include "coop_world.h"
#include "xrCoop/coop_ids.h"
#include "xrCoop/coop_search_view.h"
#include "xrCoop/coop_trade_window_rules.h"
#include "xrCoop/coop_trade_permit.h"

#include <atomic>
#include <string>

class CObject;
class CGameObject;

namespace coop
{
namespace trade
{
using world::alive;
using world::find_owner;
using world::id_of;

enum ContainerKind : u8
{
	ContNone = 0,
	ContBox = 1,
	ContCorpse = 2,
	ContTrader = 3,
};

static_assert(ContBox == kViewBox && ContCorpse == kViewCorpse && ContTrader == kViewTrader, "xrCoop/coop_search_view.h kinds");

// ---------------------------------------------------------------- the deals (coop_trade.cpp)

// The object `id` on the loaded level, one being destroyed included; nullptr for none, no level, or kNoId.
CObject* find_object(u16 id);
// Parent of a game object on this machine, kNoId if none or unknown.
u16 local_parent(u16 id);
// Host: a view a player may really have open (a box, a dead body, or the trader he talks or trades with).
bool may_see_contents(PeerId from, u8 kind, u16 id);
// Host: the money of trader `trader_id` as it is here, to player `to` (TradeOp TraderMoney); nothing when the trader is not here.
void host_send_trader_money(PeerId to, u16 trader_id);

struct Deal
{
	u32 seq = 0;
	u16 trader = kNoId;
	bool donate = false;
	bool lua_prices = false; // from Anomaly's Lua window, at the sums it showed (lua_price_deal)
	xr_vector<u16> sell; // player -> trader
	xr_vector<u16> buy; // trader -> player
	// What the deal moves, set by the decider (host_apply): the shown sums once the decider's formula agrees with them.
	u32 sell_total = 0; // the trader pays the player
	u32 buy_total = 0; // the player pays the trader
	// What the player's window showed for each side: the deal's price, checked against the decider's formula (settle_side). A Lua
	// window sends them; for the engine window's deal they are the CTrade sums the decider computes for the player's character, the
	// engine window's own formula, which that window showed (host_apply).
	u32 sell_shown = 0;
	u32 buy_shown = 0;
	// The decider's formula's sums (lua_price_deal): for the log, and sent with a refusal for its price.
	u32 sell_decider = 0;
	u32 buy_decider = 0;
};

// ---------------------------------------------------------------- the views (coop_trade_views.cpp)

struct OpenView
{
	u8 kind = ContNone;
	u16 id = kNoId;
};
extern xr_map<PeerId, OpenView> g_open; // host: containers open per player (the host itself at kHostPeer)

// The open window (both roles).
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
	trade_permit::Window remote; // this machine's grant for the trader of this window through a trading computer
};
extern ViewState g_view;
// What the Lua inventory shows, local numbering, in the low 16 bits; above them the view the script named (kViewTrader: a trade
// window). Lua may run on the game worker thread.
extern std::atomic<u32> g_search_now;
extern u16 g_search_sent;
extern u64 g_rebuilds, g_moves_failed;

void request_rebuild(u64 at);
void host_on_container(PeerId from, Reader& r);
// Tells the host what the Lua inventory searches now (g_search_now), as on_actor_menu_shown tells it of the engine's window.
void search_frame();

// ---------------------------------------------------------------- the Lua window and the computer's permits (coop_trade_window_lua.cpp)

extern trade_permit::Book g_permits;
extern u64 g_permits_issued, g_permits_dropped, g_permit_deals;
// The decider's permits, for the world they were issued in (a new world drops them).
trade_permit::Book& permits();
// Metres from a character (or the actor) to an object here; -1 when either is missing or going.
float distance_to(CGameObject* who, CObject* what);
void host_take_permit(PeerId from, u16 trader_id);
trade_permit::Verdict permit_verdict(PeerId from, CInventoryOwner* proxy, u16 trader_id);
void client_take_window(CInventoryOwner* partner);
trade_window::Answer ask_host_price(u16 proxy_id, u16 trader_id, const Deal& d, double raw[2], std::string& why);
// Client: the answer to a deal of the Lua window, to coop_trade_window.on_result.
void window_on_result(u32 seq, u8 code, s32 delta, const char* why, u32 decider_sell, u32 decider_buy);
} // namespace trade
} // namespace coop
