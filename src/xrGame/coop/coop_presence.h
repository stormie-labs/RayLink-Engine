// Anomaly Coop — presence of other players (WP8, SPEC R-3.2, R-3.3): their shots and reloads are seen
// and heard, and their names float over their heads.
//
// Shots: the owner's weapon fire (WP9 choke point items::on_weapon_fired -> on_weapon_shot) is recorded and
//   sent once per frame as PlayerShot. Everybody else plays it on the shooter's avatar: shot sound, flame
//   and smoke particles at the third-person muzzle, and cosmetic bullets (tracers, impacts) that never
//   deal damage (SendHit off). Damage is decided by the hit claims (WP2), not here.
//   The host's NPCs too (live 2026-10-29: puppets never fire): their shots go as PlayerShot with shooter = the NPC to
//   the players within 500 m; the clients play sound, flame and smoke on the puppet, no bullets.
// Reload: the owner's active weapon entering the reload state goes out as PlayerShot kind Reload; the
//   others play the weapon's reload sound at the avatar (the reload body motion comes with the owner's
//   MotionIDs in PlayerState already).
// Name tags (HUD v2.1, xrCoop/coop_nametag.h): over every other player, from the head up, an icon (a chevron in his
//   colour; wounded: the red ring with a cross, pulsing; dead: a grey skull), a line (the distance; wounded: the
//   bleed-out timer and the distance in red) and the nick in white with a dark outline; the alpha follows the distance,
//   steps aside at the crosshair, a wounded one shows through walls dimmed (console coop_nametags 0/1,
//   coop_nametags_dist 0/1; numbers: tag_* lines of [coop_hud_layout]).
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
void init(); // F3a T4: once per process (sync::init): on_weapon_shot as the WP9 weapon-fired handler
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void register_console(); // from coop::register_console_commands
void frame(); // main thread, every frame
void reset(); // session started / ended (drops cached sounds and cartridges)
void print_status();
void render_hud(); // HUDManager::RenderUI: name tags (positions prepared by frame())

// Weapon fired (any game thread): records a shot of the local actor's weapon (host: of its NPCs too), frame() sends it.
void on_weapon_shot(CWeapon* weapon);
} // namespace presence
} // namespace coop
