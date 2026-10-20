// Anomaly Coop — creature sounds across machines (W5 C2).
//
// NPC voices and sounds (C2.4). On a client every NPC and monster is a puppet: it does not think, so the AI voice
// lines, Lua sound themes and monster growls that the host plays on it never happen there. The host records the
// sounds its creatures play and sends each client the ones within that client's hearing range:
//   - CSoundPlayer::play (stalker AI voices, Lua npc:play_sound themes, monster sounds): follows the bone, keeps
//     the synchro mask so a newer line of the same creature cuts an older one, as on the host;
//   - CScriptSound play / play_at_pos / play_no_feedback on a creature (Lua sound_object): played once where it
//     started (3D only, never 2D or looped sounds).
// Player characters (the host's actor, client proxies, avatars) are never relayed. On a client, CSoundPlayer::play
// on a host-driven creature is muted (the host is authoritative for creature sounds, no doubles from local paths
// such as Die() or Lua relays); avatars keep their own sounds.
// Wire: Msg::EntitySound, Lane::Unreliable, host -> one client: u8 count, count x NpcSound (xrCoop/coop_protocol.h).
// Limits: per client a token bucket (30/s, burst 40), the sound's max distance (default coop_npc_sound_dist, cap
// 300 m) from the client's character, one entry per object+file within 150 ms; on a client at most 48 relayed
// sounds play at once, 4 per creature.
//
// Footsteps (C2.2). Creatures driven from the network keep the engine's own footsteps (CStepManager follows the
// motions they play; avatars play the owner's exact motions). For stalker puppets and avatars the ground under the
// feet is picked by a ray (their physics capsule is placed, not simulated, so the engine's "last material" is
// stale). Steps, jumps and landings of other players' characters raise Lua
//   SendScriptCallback("coop_avatar_step", avatar_id, material, power, move_state)
// on the main thread (avatar_id in local numbering; material: game material name as actor_on_footstep gets it;
// move_state: "walk" | "run" | "sprint" | "crouch" | "jump" | "land"). Relayed NPC sounds raise
//   SendScriptCallback("coop_npc_sound", npc_id, path)            (client only; e.g. subtitles by file name)
// Both fire only if a Lua script registered the callback name (AddScriptCallback) before.
//
// Threading: the engine hooks may run on the game thread (AI think, Lua) and only record; frame() (main thread)
// sends, plays and calls Lua. Wiring: coop_presence calls install/reset/frame/print_status/register_console.
#pragma once

#include "xrCoop/coop_session.h"

class CObject;
class CGameObject;
class CEntityAlive;
struct ref_sound;
struct SGameMtlPair;

namespace coop
{
namespace sound
{
void install(Session& s);
void reset();
void frame();
void print_status();
void register_console();

// --- engine hooks (one line each, marked "Anomaly Coop") ---
// CSoundPlayer::play, first line. True on a client for a host-driven creature: the caller returns at once.
bool player_sound_blocked(CObject* owner);
// CSoundPlayer::play after the sound was chosen and queued. Host: records it for the clients nearby.
// start_time: Device.dwTimeGlobal at which the engine starts it (random start delay of the sound type).
void on_player_sound(CObject* owner, ref_sound& sound, u16 bone, u32 synchro_mask, u32 start_time);
// CScriptSound play paths after the sound started. Host: records a 3D, non-looped sound on a creature.
// `at` = nullptr: at the object's position.
void on_script_sound(CObject* owner, ref_sound& sound, const Fvector* at, u32 flags, float delay, float volume);
// CStepManager::update: the material pair for the current step. Puppet stalkers and avatars get the ground under
// their feet from a ray; everything else keeps `engine_pair`.
SGameMtlPair* step_pair(CEntityAlive* creature, SGameMtlPair* engine_pair);
// CStepManager::update after a non-actor footstep played: other players' characters raise coop_avatar_step.
void on_step(CGameObject* creature, SGameMtlPair* pair, float power);

// --- coop modules ---
// coop_players::drive_avatar: the avatar's movement flags (actor EMoveCommand) changed from `before` to `now`;
// jumps and landings raise coop_avatar_step.
void on_avatar_move(CGameObject* avatar, u32 before, u32 now);
} // namespace sound
} // namespace coop
