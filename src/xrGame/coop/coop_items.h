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
//   pushed to that player (it may be stale there: NPC guns, stash contents).
//
// Wire (Msg::ItemState, ids in host numbering):
//   u8 op = 1 State      u16 item, str section, u8 flags (1 = host authority), varu n, n bytes blob
//   u8 op = 2 SpawnAmmo  u32 stream, u32 seq, u16 weapon, u16 parent, str ammo section, u16 rounds
// Blob: u8 version(1), then tags { u8 tag, varu len, payload }, unknown tags are skipped:
//   1 condition u16 (q 1/65535)   2 upgrades u8 n + n str (install order)   3 ammo box u16
//   4 addons u8 flags   5 scope index u8   6 main magazine u16 count + u8 ammo type
//   7 launcher magazine u16 count + u8 type (weapons with a grenade launcher)   8 portions u8
// Main/launcher magazines are stored independent of the launcher mode (the mode itself is not
// synced: it is cosmetic and switching it has side effects).
#pragma once

#include "xrCoop/coop_session.h"

#include <vector>

class CInventoryItem;
class CWeapon;
class CWeaponAmmo;
class CSE_Abstract;

namespace coop
{
namespace items
{
void install(Session& s);
void reset();
void frame(); // main thread, from the coop pump: capture/send, host tracking, pending applies
void print_status();

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
} // namespace items
} // namespace coop
