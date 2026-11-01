// Anomaly Coop — presence of other players (WP8, SPEC R-3.2, R-3.3): their shots and reloads are seen
// and heard, and their names float over their heads.
//
// Shots: the owner's weapon fire (WP9 choke point items::on_weapon_fired -> on_weapon_shot) is recorded and
//   sent once per frame as PlayerShot. Everybody else plays it on the shooter's avatar: shot sound, flame
//   and smoke particles at the third-person muzzle, and cosmetic bullets (tracers, impacts) that never
//   deal damage (SendHit off). Damage is decided by the hit claims (WP2), not here.
//   The host's NPCs too (live 2026-10-29: puppets never fire): their shots go as PlayerShot with shooter = the NPC to
//   the players within 500 m; the clients play sound, flame and smoke on the puppet, and bullets that deal no damage when
//   the fight is near them (audit W3: tracers, marks, whine; budget per client, xrCoop/coop_relay_rules.h).
// The rest of what the host's creatures and machines do (audits 2026-10-31) rides on the same event: an NPC's other weapon
//   sounds (reload, jam, empty click, a knife's swing, F6), the flinch of a hit (F7), a helicopter's gun, rockets, blast
//   and state (rotor, lights, smoke, health, H2, W8).
// Reload: the owner's active weapon entering the reload state goes out as PlayerShot kind Reload; the
//   others play the weapon's reload sound at the avatar (the reload body motion comes with the owner's
//   MotionIDs in PlayerState already).
// Name tags (HUD v2.1, xrCoop/coop_nametag.h): over every other player, from the head up, an icon (a chevron in his
//   colour; wounded: the red ring with a cross, pulsing; dead: a grey skull), a line (the distance; wounded: the
//   bleed-out timer and the distance in red) and the nick in white with a dark outline; the alpha follows the distance,
//   steps aside at the crosshair, a wounded one shows through walls dimmed (console coop_nametags 0/1,
//   coop_nametags_dist 0/1; numbers: tag_* lines of [coop_hud_layout]).
//
// Wire (ids in host numbering), Lane::Unreliable (cosmetic, latest wins), Lane::Reliable where a lost one is a silence
// nothing covers (audit M5: the shot that opens a burst, a reload, a launcher, a rocket):
//   PlayerShot  u16 shooter, u16 weapon, u8 kind (0 shot, 1 reload, 2 grenade launcher; 3.. host -> clients only, the
//               Kind enum says what each field means), u8 count, u8 ammo type index, u8 flags (1 silenced, 2 show bullets),
//               vec3 muzzle, vec3 direction
//   owner -> host -> others; the host replaces the shooter id with the sender's proxy.
#pragma once

#include "xrCoop/coop_session.h"

class CWeapon;
class CGameObject;

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

// --- host hooks (one line each, "Anomaly Coop"; any game thread): what the clients are told, frame() sends it.
// CHudItem::PlaySound: an item in an NPC's hands made a sound.
void on_item_sound(CGameObject* item, LPCSTR alias, const Fvector& pos);
// CCharacterPhysicsSupport::in_Hit, CBaseMonster::HitSignal: a creature flinched from a hit.
void on_flinch(CGameObject* creature, const Fvector& dir, const Fvector& bone_pos, u16 bone);
void on_monster_flinch(CGameObject* monster, u8 side);
// CHelicopter: fired, launched, blew up; on_heli_state every frame and at its death (flags: relay::HeliFlags).
void on_heli_shot(CGameObject* heli, const Fvector& pos, const Fvector& dir);
void on_heli_rocket(CGameObject* heli, const Fvector& pos);
void on_heli_blast(CGameObject* heli);
void on_heli_state(CGameObject* heli, u8 flags, float health, const Fvector& aim); // aim: a direction, read with relay::HeliAiming
} // namespace presence
} // namespace coop
