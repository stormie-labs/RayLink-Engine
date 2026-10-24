// Anomaly Coop — every player is a real participant for the NPC AI (W11).
//
// On the host every other player is a stalker proxy (section coop_player_avatar) driven by network
// snapshots (coop_remote). The stock AI perceives it like any stalker, but a few engine pieces only
// know the local actor or read state a puppet never has:
//   - vision: CVisualMemoryManager::object_luminocity reads the object's render lighting (ROS), which
//     the renderer refreshes every frame only for the camera entity and, round robin, for one object in
//     the camera view. A proxy outside the host's view stays at 0 (seen ~180 times slower than lit).
//     Here: a light estimate from the weather (ambient, sun and sky visibility by ray, headlamp).
//   - "the enemy sees me" (stalker combat planner, monster enemy manager, enemy priority bias) reads
//     the enemy's own visual memory, empty on a puppet. Here: the player's reported view direction
//     plus the line of sight the NPC itself has to the player.
//   - "player on my path" (stalker combat) looks at the actor only. Here: every remote player.
//   - NPC head turning to the player (CAI_Stalker::LookAtActor) on a client: the NPC there is a puppet
//     without perception. Here: distance and a line-of-sight ray to this machine's player.
//   - how often the host updates an NPC: by its distance to the HOST'S CAMERA only (shedule_Scale, crow radius), so an NPC
//     far from the host but next to a client thinks every 625 ms and walks at 1/6 of its speed (owner, build 10).
//     Here: the characters of the other players are "viewers" of the scheduler (viewers_frame).
//
// Threading: on_proxy_frame runs on the main thread (UpdateCL, FrameMove); the queries run on the
// game thread (schedule) or in bone callbacks. The game thread never runs during FrameMove, so the
// per-proxy records written there are read without locks.
#pragma once

class CObject;
class CGameObject;
class CEntityAlive;
class CCustomMonster;
class CAI_Stalker;

namespace coop
{
namespace npc_ai
{
// The local actor or any player character (proxy / avatar, section coop_player_avatar).
bool is_player(const CObject* obj);
// A player character that is not this machine's actor, while a session runs (on the host: a client's proxy).
bool is_remote_player(const CObject* obj);

// Host, main thread, every frame of a driven player proxy (remote::apply): refreshes its light estimate
// (twice a second) and its view direction.
void on_proxy_frame(CGameObject* proxy);

// Vision: true (and `lum` 0..1 set) for a remote player's character with a fresh light estimate; the
// caller then uses it instead of the render lighting.
bool player_luminocity(const CGameObject* obj, float& lum);
// Audit A3 (2026-10-24): the same light estimate for any other creature near a player who is not the host (the
// render refreshes the light of objects on the host's screen only: off it a creature stays dark and is seen ~30 s
// late, so fights by a far client went on half blind). True and `lum` when a fresh estimate exists.
bool creature_luminocity(const CGameObject* obj, float& lum);

// "Does player `player` see `observer` now?" for a remote player's character (false = not one, use the
// stock answer). `sees`: `observer` sees the player (line of sight is mutual) and is inside the player's
// view cone; an unknown view direction counts as seeing.
bool player_sees(const CEntityAlive* player, const CCustomMonster* observer, bool& sees);

// Stalker combat "player on my path" for the remote players: a hostile player the NPC sees right now
// stands within 2 m of its path.
bool player_on_the_path(CAI_Stalker* npc);

// Client side of CAI_Stalker::LookAtActor for an NPC puppet (game thread): this machine's player is
// alive, closer than `max_distance`, the Lua hook allows it and no static geometry is in between.
// Always false for player characters (their heads follow their players).
bool puppet_may_look_at_actor(CAI_Stalker* npc, float max_distance);

// Viewers of the scheduler (xrEngine/sheduler_viewers.h, rule and numbers in xrCoop/coop_viewers.h). Host, main thread,
// once a frame (sync::frame): lists where the characters of the other players are (alive or down, in the world, not
// parked), so the objects around them are updated as often as the ones around the host's camera. Anywhere else (a client,
// no session, a world that is not up) it empties the list, which is the stock behaviour.
void viewers_frame();
// Audit A4 (2026-10-24): the nearest living player to `pos` within `max_dist` - this machine's actor or (host) the
// character of a player who is in the world, alive or downed; nullptr when none. Engine code that only knew Actor()
// (the poltergeist's senses) asks this.
CEntityAlive* nearest_player(const Fvector& pos, float max_dist);
void viewers_clear();
// Console: coop_viewer_range (m, 0 = viewers off), coop_viewer_frame_radius (m, 0 = scale only). Host settings.
void register_console();

void print_status();
} // namespace npc_ai
} // namespace coop
