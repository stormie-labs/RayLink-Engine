// Anomaly Coop — player money lives on the host (F2 T1, review R4 7). Rules and wire: xrCoop/coop_ledger.h.
//
// Host: a player's balance is the money of his character — the proxy's CInventoryOwner money while it is online, the
// server entity's m_dwMoney while it is parked or its player loads the world; kHostPeer is the host's own actor. A
// ReqMoney is checked with ledger::apply_client_delta and every one is answered with MoneyBalance{balance, acked_seq =
// its seq}. Trades, transfers and Lua grants change balances on the host (host_adjust / host_set), which send the new
// balance to the player.
// Client: every change of the actor's money (GE_MONEY through the router, requests::client_money) becomes a ReqMoney
// with a cause: Dialog while we hold an NPC's talk lock, else Script. The actor shows the host's balance plus the
// changes the host has not answered yet (ledger::client_view); a MoneyBalance sets the actor's money without an echo
// (entities::ClientApply). Changes are queued (the router may run on the game thread) and sent by frame(), or at once
// by client_flush() before a trade deal and before a talk lock is released.
// Settings (host): configs\coop_money.ltx [coop_money], read once; console coop_money_gain_burst,
// coop_money_gain_per_min, coop_money_dialog_cap, coop_money_max (Ruling 21).
// Lua (coop.*):
//   money_give(peer, amount) -> ok, why    any machine: our money to another player (the host checks it)
//   money_grant(peer, amount) -> ok, why   host: adds (or, negative, takes) money, never below 0
//   money_set(peer, amount) -> ok, why     host: sets the balance
//   money_of(peer) -> RU | nil             host
//   talk_lock_of(peer) -> npc id | nil     host: the NPC whose talk lock the player holds (host numbering)
// The host-only ones also work for a character without a game object (its player is loading the world).
#pragma once

#include "xrCoop/coop_session.h"

struct lua_State;

namespace coop
{
namespace money
{
void install(Session& s);
void reset(); // session started / ended / world replaced
void frame(); // main thread, every frame: the client sends its queued changes
void on_peer_left(PeerId id);
// Client: live in the host's world. The actor's money came from the host's save (role swap): it is the balance.
void on_client_live();
void print_status();

// Policy::max_money of this machine (the host's settings on the host).
u32 max_money();
// configs\coop_money.ltx upgrade_min_percent (for the upgrade price check, F2 T5).
float upgrade_min_percent();

// --- host ---
// The balance of `peer`'s character, 0 if he has none.
u32 host_balance(PeerId peer);
// Changes the balance of `peer`'s character by `delta` (never below 0, never above max_money unless it already is)
// and sends it to its player. `why`: a literal for the log ("trade", "grant", ...). False if he has no character.
bool host_adjust(PeerId peer, s64 delta, const char* why);
// Sets the balance (up to max_money) and sends it to its player. False if he has no character.
bool host_set(PeerId peer, u32 amount, const char* why);

// --- client ---
// GE_MONEY of the local actor (any thread). Skipped while host commands are applied (entities::client_applying).
void client_on_actor_money(u32 money);
// Main thread: sends the queued changes now.
void client_flush();
// While alive, decreases of the actor's money are added up here and not sent: the request they belong to carries
// the sum (F2 T5: an upgrade paid by one request). Main thread; scopes nest (the innermost one counts).
struct ClientSpendScope
{
	ClientSpendScope();
	~ClientSpendScope();
	ClientSpendScope(const ClientSpendScope&) = delete;
	ClientSpendScope& operator=(const ClientSpendScope&) = delete;
	u32 spent() const { return m_spent; }

private:
	friend void client_on_actor_money(u32 money);
	ClientSpendScope* m_outer = nullptr;
	u32 m_spent = 0;
};

// Adds the Lua functions above to the `coop` table (coop_script.cpp register_raw).
void register_lua(lua_State* L);
} // namespace money
} // namespace coop
