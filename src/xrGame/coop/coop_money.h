// Anomaly Coop — player money lives on the host (F2 T1, review R4 7). Rules and wire: xrCoop/coop_ledger.h.
//
// Host: a player's balance is the money of his character — the proxy's CInventoryOwner money while it is online, the
// server entity's m_dwMoney while it is parked or its player loads the world; kHostPeer is the host's own actor. A
// ReqMoney is checked with ledger::HostBook (allowances per character for the whole session, Ruling F2-1) and every one
// is answered with MoneyBalance{balance, acked_seq = its seq}. Trades, transfers and Lua grants change balances on the
// host (host_adjust / host_set), which send the new balance to the player; so does the end of his join.
// Client: every change of the actor's money (GE_MONEY through the router, requests::client_money) becomes a ReqMoney
// with a cause: Dialog while we hold an NPC's talk lock, else Script. The actor shows the host's balance plus the
// changes the host has not answered yet (ledger::client_view); a MoneyBalance sets the actor's money without an echo
// (entities::ClientApply). Changes are queued (the router may run on the game thread) and sent by frame(), or at once
// by client_flush() before a trade deal and before a talk lock is released.
// Settings (host): configs\coop_money.ltx [coop_money], read once; console coop_money_gain_burst,
// coop_money_gain_per_min, coop_money_dialog_cap, coop_money_dialog_per_min, coop_money_max (Ruling 21, F2-1).
// F2 T6b (Ruling F2-19): the session's rules for money between players (money_transfer, max_money_transfer) come from the
// host's Lua, coop.money_set_rules; the ledger refuses a transfer against them (a client's Lua checks them too, but only
// the host's answer counts). Until the Lua has said, the options' defaults hold (on, 1 000 000 at a time). T6b review I1: a
// character also gives money away at a bounded rate (transfer_burst in a row, transfer_per_min a minute; coop_money.ltx,
// console coop_money_transfer_*), so that nobody can fill another player's news with 1 RU gifts.
// A MoneyBalance carries a note (ledger::MoneyNote): a refusal of the host's goes to the player
// whose change it was, a gift to the receiver. Each is told by the Lua callback coop_money_note on the machine of that
// player (the host's own too, for a gift to him): ("received", giver_peer, amount) or
// ("refused", receiver_peer | -1, change, why, cause).
// F3a T7: the host player is a client of the world server over the internal channel for the transfers: his money_give is
// a ReqMoney (Transfer) from peer 0, settled by the same ledger, rules and rates as a client's, and every answer to him is
// a MoneyBalance over the link that tells him its note. His actor's money stays the world's balance of kHostPeer (one
// object in the process): his other changes are not sent, and a MoneyBalance does not set it.
// Lua (coop.*):
//   money_give(peer, amount) -> ok, why    any machine: our money to another player (the host checks it, the answer comes
//                                          later as a note)
//   money_grant(peer, amount) -> ok, why   host: adds (or, negative, takes) money, never below 0
//   money_grant_to(character, amount) -> added | nil, why  host: adds money to a player's character by id, also while
//                                          its player is away (a group reward); `added` is what really came
//   money_set(peer, amount) -> ok, why     host: sets the balance
//   money_set_rules(enabled, max) -> ok, why  host: the transfer rules between players (F2 T6b); `max` per transfer
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
void init(); // F3a T4: once per process (sync::init): the money settings and their console commands
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void reset(); // session ended, client's world replaced: nothing kept
// Host started a session (sync::on_session_started): reset, and the player characters in the world now count as "from
// the save" — they keep full allowances; one made later in this session starts empty (Ruling F2-10).
void on_session_started();
void frame(); // main thread, every frame: the client sends its queued changes
void on_peer_left(PeerId id); // the connection's numbering goes; the character keeps its allowances
// Host (sync::host_end_join): player `id` has loaded the world and is live. Sends him his balance once: the client
// drops MoneyBalance until it is live, and the save it loaded may be older than its balance. His character's
// allowances start refilling here, not at its first income (Ruling F2-20): a new character starts empty, a returning one
// keeps its frozen tokens, a saved one that was not here yet this session is full at its first income.
void host_on_peer_in_world(PeerId id);
// Client: live in the host's world. The actor's money came from the host's save (role swap): it is the balance.
void on_client_live();
void print_status();

// Policy::max_money of this machine (the host's settings on the host).
u32 max_money();
// configs\coop_money.ltx upgrade_min_percent (for the upgrade price check, F2 T5).
float upgrade_min_percent();
// configs\coop_money.ltx scripted_upgrades (F2 T5): the host takes a client's upgrade that names no mechanic (a script
// installed it). Default false.
bool scripted_upgrades();

// --- host ---
// The balance of `peer`'s character, 0 if he has none.
u32 host_balance(PeerId peer);
// Changes the balance of `peer`'s character by `delta` (never below 0, never above max_money unless it already is)
// and sends it to its player. `why`: a literal for the log ("trade", "grant", ...); "trade" is counted as a trade in
// coop_status, anything else as a grant. False if he has no character.
bool host_adjust(PeerId peer, s64 delta, const char* why);
// Sets the balance (up to max_money) and sends it to its player. False if he has no character.
bool host_set(PeerId peer, u32 amount, const char* why);
// Host: adds `amount` to a player's character, whether or not its player is here; `added` is what really came. False: not
// a player's character.
bool host_add_to_character(u16 character, u32 amount, const char* why, u32& added);
// Sends `peer` his balance as it is (F2 T5: after the answer to an upgrade he paid for on his machine, which the host
// refused or did not charge). Nothing if he has no character.
void host_send_balance(PeerId peer);

// --- client ---
// GE_MONEY of the local actor (any thread). Skipped while host commands are applied (entities::client_applying).
void client_on_actor_money(u32 money);
// Main thread: sends the queued changes now.
void client_flush();
// While alive, decreases of the actor's money are added up here and not sent: the request they belong to carries
// the sum (F2 T5: an upgrade paid by one request, items::UpgradePurchase). Main thread; scopes nest (the innermost one
// counts).
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
// F2 T5 (T1 concern 3), main thread. A payment a request carries to the host (an upgrade's price): until the host
// answers that request it is held out of the money shown (ledger::UpgradeHolds), so a balance the host sent before it
// charged the payment does not show the money from before it; the host sends the balance after its answer. Returns the
// key to release it with (0: nothing held — not a live client, or nothing paid).
u32 client_hold(u32 amount);
// The host answered the request of `key` (or it timed out): the hold goes; the money shown follows the next balance.
void client_release(u32 key);
// A spend a ClientSpendScope summed that no request carried after all: it goes to the host as an ordinary change.
void client_spend(u32 amount);

// Adds the Lua functions above to the `coop` table (coop_script.cpp register_raw).
void register_lua(lua_State* L);
} // namespace money
} // namespace coop
