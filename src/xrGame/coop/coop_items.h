// Anomaly Coop — state of owned items (WP9).
//
// Who owns what: an item in a player's inventory is that player's (owner-authoritative). Its
// internal state — rounds in the magazine(s) and their type, ammo box count, condition (also
// artefact charge and device battery), attached addons and scope index, installed upgrades,
// remaining portions of multi-use items — changes only on the owner's machine. Ownership itself
// (who holds the item) is the host's business (WP2).
//
// Owner (client): every 500 ms, on any inventory change and right after reload / unload /
//   attach / detach / upgrade / eat, the state of every item of the local actor is captured
//   into a small tagged blob and compared with what was last sent; a difference goes to the
//   host as ItemState. Unloading on a client does not spawn ammo locally: the exact count goes
//   to the host (ItemState op SpawnAmmo) and the host creates the boxes. An emptied box stays
//   in the client's inventory until the host destroys it (the host does that when the owner
//   reports 0 rounds), so no drop/destroy race can leave a full box in the host world.
// Host: validates (sender's item by parent chain, section, sane values), applies the state to
//   its server entity AND to the online game object of the item (otherwise the host's own net
//   export would overwrite it with stale values), keeps re-applying it if the host copy drifts,
//   and relays the result to the other clients. The host's own actor items are broadcast too,
//   so everybody's copies match. An item entering a player's inventory gets the host's state
//   pushed to that player (it may be stale there: NPC guns, stash contents); what the player's own
//   take callbacks changed on it meanwhile stays his and goes to the host (client_before_take).
//
// Wire (Msg::ItemState, ids in host numbering):
//   u8 op = 1 State      u16 item, str section, u8 flags (1 = host authority), varu n, n bytes blob
//   u8 op = 2 SpawnAmmo  u32 stream, u32 seq, u16 weapon, u16 parent, str ammo section, u16 rounds
// Blob: u8 version(1), then tags { u8 tag, varu len, payload }, unknown tags are skipped:
//   1 condition u16 (q 1/65535)   2 upgrades u8 n + n str (install order)   3 ammo box u16
//   4 addons u8 flags   5 scope index u8   6 main magazine u16 count + u8 ammo type
//   7 launcher magazine u16 count + u8 type (weapons with a grenade launcher)   8 portions u8
// Replacement snapshots also carry 9 active launcher mode, 10 fire mode index, 11 per-round main magazine types.
// Their spawn restoration applies these after native weapon initialization, including both launcher magazines.
#pragma once

#include "xrCoop/coop_session.h"
#include "xrCoop/coop_owner_rules.h" // TZ R32 W8: owner::Setter, itemtags::FieldVersions
#include "coop_money.h" // F2 T5: UpgradePurchase holds a money::ClientSpendScope

#include <string>
#include <vector>

class CInventoryItem;
class CWeapon;
class CWeaponAmmo;
class CSE_Abstract;

namespace coop
{
namespace items
{
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
// Host (the world server, a map server): `peer`'s join is over (sync::host_end_join). The states of his items that came while he
// was still joining are taken now, in the order they came.
void host_on_peer_in_world(PeerId peer);
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void reset();
void frame(); // main thread, from the coop pump: capture/send, host tracking, pending applies
void print_status();
void print_inventory(); // local diagnostic; never changes or requests inventory state

// --- engine hooks (any game thread; they only record, frame() sends) ---

// The state of `item` changed (reload, unload, attach, detach, upgrade, eat). Own items are
// rescanned next frame; an item of a corpse / stash / the ground the player unloaded is sent too.
void touch(CInventoryItem* item);
// CWeapon::SpawnAmmo on a live coop client: the host spawns the boxes with exactly this many
// rounds. Returns true if handled (caller must not spawn locally).
bool client_spawn_ammo(CWeapon* weapon, u32 box_curr, const char* ammo_sect, u32 parent_id);
// CWeaponMagazined::ReloadMagazine emptied `box`. Returns true on a live coop client: keep the
// empty box, the host destroys it after the 0 count arrives (caller skips SetDropManual).
bool client_keep_empty_box(CWeaponAmmo* box);

// Shot choke point (CWeaponMagazined::OnShot, launcher shot in CWeaponMagazinedWGrenade::OnShot).
// Calls the handler set by set_weapon_fired_handler (WP8 player shot events); no-op without one.
// May run on the game thread: the handler must only record.
void on_weapon_fired(CWeapon* weapon);
using WeaponFiredFn = void (*)(CWeapon* weapon);
void set_weapon_fired_handler(WeaponFiredFn fn);

// --- ReqSpawn (WP2, R3 В6): the state of an item a client asks the host to create travels as the blob above,
// never as raw spawn bytes; the host builds the entity from its own section and applies the blob. ---
// Client: the blob of an entity that is not created yet. False = not an inventory item (nothing to carry).
bool capture_spawn_state(CSE_Abstract* entity, std::vector<u8>& blob);
bool replacement_state(CSE_Abstract* source, const u8* data, size_t size, std::vector<u8>& out);
void stage_spawn_state(u16 id, const std::vector<u8>& blob);
void discard_spawn_state(u16 id);
void forget_item(u16 id); // hook queues now, owner/host tracking at the next main-thread pass
void prepare_clone_state(u16 source, u16 target);
void restore_spawn_state(CWeapon* weapon, CSE_Abstract* entity, bool launcher_ready = false);
void restore_item_state(CSE_Abstract* entity, const std::vector<u8>& blob, CInventoryItem* object = nullptr);
bool client_pending(CWeapon* weapon);
// Client (live report 2026-11-08, GAMMA): the host's take is about to put item `local_item` into our character (WP2 parent
// relay, before GE_OWNERSHIP_TAKE runs our take callbacks). Its state now is the baseline: what our callbacks change on it
// (GAMMA pins a looted gun at 0.83) is the owner's, goes to the host with the next scan and survives the host's push of
// its copy; the tags we did not touch take the host's (fresher) values. Main thread.
void client_before_take(u16 local_item);
// Client (F2 T12b): the host has just created our item `local_id` that we created at once under a leased id; what we
// changed on it since (portions, a condition a mod set on the object) goes to the host with the next scan, as if the
// host knew nothing of it. Main thread.
void client_resend_state(u16 local_id);
void client_send_state_now(u16 local_id);
void host_silencer_detached(u16 id, PeerId owner);
// Client (live report 2026-11-08, G.A.M.M.A.: a scope fitted to an SR-3M showed 7 %): we made our item `local_id` ourselves
// under a leased id (a clone of our weapon for a scope, a kit, a detach). The host builds its copy from its own view of the
// source and pushes it into our hands; that push keeps all our tags (but the upgrade list, the host's) and ours goes to the
// host. Main thread.
void client_made_here(u16 local_id);
// Host: applies a client's blob to a fresh entity (not spawned yet) with the ItemState checks: kind, counts,
// known upgrades and ammo types. False = refused (`why` says why); the entity may be half changed: discard it.
bool host_apply_spawn_state(CSE_Abstract* entity, const u8* data, size_t size, const char*& why);
// Host (R3 В6, task 10b): the section's own installed_upgrades break its upgrade tree, so any new entity of it is
// FATAL in Manager::init_install (broken data). True for a section without upgrades.
bool section_defaults_ok(const shared_str& section);

// Host (R3 В6, task 10b): a client's GE_INSTALL_UPGRADE `upgrade` on the item whose server entity is `entity`,
// checked the way the next net_Spawn from scratch installs (section defaults, the entity's list, then it; a loading
// install is FATAL on a missing parent group or a taken group). Refused: `why` says why.
enum class AddedUpgrade : u8
{
	Relay, // the entity may keep it (CSE add_upgrade)
	AlreadyThere, // on the entity already or a section default: nothing to do (add_upgrade is FATAL on a duplicate)
	Refused,
};
AddedUpgrade host_check_added_upgrade(CSE_Abstract* entity, const shared_str& upgrade, const char*& why);
// Parity audit F1 (xrCoop/coop_reconcile.h): the digest of the item `e` as this machine sees it; `with_uses`: its top
// holder is online, so the portions are in its game object at both ends.
u32 state_digest(CSE_Abstract* e, bool with_uses);
// Host: the host's state of world item `id`, or of everything `holder` holds, to one client: a repair, a loot or trade
// window that opens (the client's copy keeps its spawn values, which GAMMA overwrites on the host right after).
void host_send_state(PeerId to, u16 id);
void host_send_contents(PeerId to, u16 holder);
// Split levels (testers 2026-11-13: loot of a map server's NPCs was 100 % on the world server and in the players' hands). A map
// server: its state of world item `id` (local numbering), the ItemState blob, for LevelEntity Item. False: no inventory item.
bool level_item_state(u16 id, std::string& section, std::vector<u8>& blob);
// A map server: the same state to the players of its level (ItemState, no authority flag: their own items keep theirs), so that
// their copy has the wear before a take or a tooltip rolls the parts of it.
void level_item_tell(u16 id, const std::string& section, const std::vector<u8>& blob);
// The world server: the authority of a level says world item `id` (host numbering) is `blob`. Only the condition is taken, into
// the entity (and the object, if there is one). The caller checked that the item lies on that level and no player holds it;
// this checks the holder again by the world's tree, not by the owner table (it lags a frame behind the move). False:
// refused, `why` says why.
bool host_take_level_state(u16 id, const char* section, const u8* data, size_t size, const char*& why);

// --- TZ R32 W8: one value of an item's state, which travels with the item ---
// The decider (this process writes the item: the co-op host, a map server) ran a script setter on its copy (script_game_object.cpp
// SetCondition, script_game_object3.cpp SetRemainingUses). Outside the apply of an owner's state, on a remote player's item, it is
// the new baseline (host_enforce keeps it), its field versions move on and the owner gets it in this frame; the other players' copies
// get the decider's view. A map server also tells the world (LevelSide::level_item_facts_up, source Decider). Main thread.
void decider_wrote(CInventoryItem* item, owner::Setter what);
// A map server's journal, writing the ItemFacts of `id` (level_item_state gives the blob, with the decider's versions and, for a held
// eatable, its object's own client_data): the source of the change since the last read (journal::ItemFactsBody: 0 Loot, 1 Decider,
// 2 Owner), then forgotten. The world treats Decider and Owner alike (host_take_item_facts); only Loot takes the wear-only path.
u8 level_facts_source(u16 id);
// The world server: a map server's ItemFacts of item `id` from its writer (source Decider or Owner; the world's acceptance checked
// the writer). The condition, the addon flags and the scope go into the entity (and the object, if there is one), the uses into
// client_data (with no object here: the writer's own bytes of its object when they came, else the stock format patched or made,
// coop_initial_rules.h uses_store), the decider's versions into this process's table, the upgrade list (the writer's own copy's: the
// blob is its capture, a player's claim never reaches it). False: refused, `why` says why, and nothing was changed (uses that cannot
// be stored refuse the whole entry: no Applied for a half-applied fact).
bool host_take_item_facts(u16 id, const char* section, const u8* data, size_t size, const char*& why);
// The uses of an eatable with no game object here, into its entity's client_data in exactly the stock CEatableItem::save format
// (xrCoop/coop_initial_rules.h). False: not an eatable section (is_eatable_section), more than its max_uses, an item on the ground
// (physics state in its client_data) or a section with a script binding and no client_data yet (stage_uses keeps them then).
bool entity_write_uses(CSE_Abstract* e, u8 uses);
// WP6 fix round (finding 1): uses an entity cannot keep, staged for the item's next game object here (set the first frame it exists:
// a deliberate write on its writer). staged_uses: the staged value, for a spawn's initial state. Main thread.
// discard_staged_uses: a newer value went into the object itself (set_initial's object path).
void stage_uses(u16 id, u8 uses);
bool staged_uses(u16 id, u8& uses);
void discard_staged_uses(u16 id);
// WP6 fix round (finding 5): the items of `section` have portions (its class is an eatable's, coop_initial_rules.h
// is_eatable_class; cached), and the most they have (CEatableItem::Load: max_uses, 1 when missing). Main thread.
bool is_eatable_section(const shared_str& section);
u32 section_max_uses(const shared_str& section);
// §8a A12: the decider's versions of item `id` as this process knows them (a decider: its latest; an owner: those he has applied),
// and their arrival with the item's state (a spawn's initial state): the decider takes them as its latest, the owner as applied.
itemtags::FieldVersions item_versions(u16 id);
void item_versions_arrive(u16 id, const itemtags::FieldVersions& v, u32 mask);
// Host (F2 T5 fix round 1): the host sold an upgrade of item `id` to player `owner` (ReqItemEvent accepted, the entity
// holds it). The host's game object of the item takes the entity's list (a loading install: no event, no Lua price) and
// the other players' copies get the host's state of it. The owner's own ItemState never adds an upgrade on the host.
void host_upgrade_sold(u16 id, PeerId owner);

// --- F2 T5: an upgrade a client buys at a mechanic ---
// requests::client_item_event, the client's GE_INSTALL_UPGRADE of `local_item`: inside an UpgradePurchase of that item,
// its mechanic (local numbering) and what the actor paid since the last claim, which the client now holds out of the
// money it shows until the host answers (money::client_hold; `hold` is the key, 0 if nothing was paid). False outside
// a purchase: mechanic 0xffff, nothing paid (a script's upgrade). Main thread.
bool claim_upgrade_payment(u16 local_item, u16& mechanic, u32& paid, u32& hold);
// Client: the actor's talk partner (local numbering), 0xffff if he talks to nobody. The mechanic of an upgrade Lua
// installs (Anomaly's upgrade window, ui_inventory.script, runs inside the mechanic's dialog).
u16 actor_talk_partner();

// RAII around Manager::upgrade_install on the client's upgrade paths: the engine window
// (CUIInventoryUpgradeWnd::OnMesBoxYes; the mechanic is the menu's partner) and Lua obj:install_upgrade
// (CScriptGameObject::InstallUpgrade, used by ui_inventory.script; the mechanic is actor_talk_partner()). What the actor
// pays meanwhile (inventory_upgrades.effect_functor_a: give_money(-price)) is summed by a money::ClientSpendScope, not
// sent as a money change; the upgrade's GE_INSTALL_UPGRADE inside it takes the mechanic and the sum to the host
// (ReqItemEvent, claim_upgrade_payment), which checks the mechanic and the price and charges the player. A sum no
// upgrade event took goes to the host as an ordinary change at the end. Main thread; purchases nest (the innermost one
// takes the events). Off a live client nothing is summed or sent.
struct UpgradePurchase
{
	UpgradePurchase(CInventoryItem* item, u16 mechanic); // mechanic: local numbering, 0xffff: none
	~UpgradePurchase();
	UpgradePurchase(const UpgradePurchase&) = delete;
	UpgradePurchase& operator=(const UpgradePurchase&) = delete;

private:
	friend bool claim_upgrade_payment(u16 local_item, u16& mechanic, u32& paid, u32& hold);
	money::ClientSpendScope m_scope; // first: alive for the whole purchase
	UpgradePurchase* m_outer = nullptr;
	u16 m_item = 0xffff; // local numbering
	u16 m_mechanic = 0xffff;
	u32 m_carried = 0; // what upgrade events took of m_scope.spent()
};
} // namespace items
} // namespace coop
