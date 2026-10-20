// Anomaly Coop — presence of other players (WP8, SPEC R-3.2, R-3.3): their shots and reloads are seen
// and heard, and their names float over their heads.
//
// Shots: the owner's weapon fire (WP9 choke point items::on_weapon_fired -> on_weapon_shot) is recorded and
//   sent once per frame as PlayerShot. Everybody else plays it on the shooter's avatar: shot sound, flame
//   and smoke particles at the third-person muzzle, and cosmetic bullets (tracers, impacts) that never
//   deal damage (SendHit off). Damage is decided by the hit claims (WP2), not here.
// Reload: the owner's active weapon entering the reload state goes out as PlayerShot kind Reload; the
//   others play the weapon's reload sound at the avatar (the reload body motion comes with the owner's
//   MotionIDs in PlayerState already).
// Name tags: nick, optional distance and the survival state over every other player, drawn by the HUD
//   (console coop_nametags 0/1, coop_nametags_dist 0/1).
//
// Wire (ids in host numbering), Lane::Unreliable (cosmetic, latest wins):
//   PlayerShot  u16 shooter, u16 weapon, u8 kind (0 shot, 1 reload, 2 grenade launcher), u8 count,
//               u8 ammo type index, u8 flags (1 silenced), vec3 muzzle, vec3 direction
//   owner -> host -> others; the host replaces the shooter id with the sender's proxy.
#pragma once

#include "xrCoop/coop_session.h"

class CWeapon;

namespace coop
{
namespace presence
{
void install(Session& s); // also registers on_weapon_shot as the WP9 weapon-fired handler
void register_console(); // from coop::register_console_commands
void frame(); // main thread, every frame
void reset(); // session started / ended (drops cached sounds and cartridges)
void print_status();
void render_hud(); // HUDManager::RenderUI: name tags (positions prepared by frame())

// Weapon fired (any game thread): records a shot of the local actor's weapon, frame() sends it.
void on_weapon_shot(CWeapon* weapon);
} // namespace presence
} // namespace coop
