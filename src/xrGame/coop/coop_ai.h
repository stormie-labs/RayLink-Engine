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
// Protocol 29 (the server's debug, work package 4): debug_unseen and player_god answer for a player by the personal flags of the state
// this process acts on (coop_debug.h); the rules are xrCoop/coop_debug_rules.h (ai_ignores, blow_spares).
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
// Live 2026-10-25 (owner): "when somebody died, why do NPCs go on shooting the corpse". A player who is down stays alive for
// the engine (he can be revived); one dead for good, out of the run or being respawned is no enemy (CEnemyManager::useful).
// A wounded one still is: an enemy may finish him.
bool player_out_of_fight(const CObject* obj);
// Live 2026-10-30 (owner): "monsters keep at you after you go down". For a monster a wounded player is out too: it leaves
// him and turns to whoever is standing (death::monster_leaves; CEnemyManager::useful, CMonsterEnemyManager, the poltergeist).
bool player_down(const CObject* obj);
// What CEnemyManager::useful asks: is this player no enemy for a creature of that kind? A monster leaves one who is down (as
// player_down); a stalker leaves him too unless the host switched the game rule coop_npc_finish_downed on.
bool player_out_for(const CObject* obj, bool stalker);
// The game rule coop_npc_finish_downed; a map server has no game record, it takes it from the world's PlayerRules.
bool finish_downed();
void set_finish_downed(bool on);

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
// Audit A4 (2026-10-24): the nearest player a monster can go for within `max_dist` of `pos` - this machine's actor or (host)
// the character of a player who is in the world and not down (player_down); nullptr when none. Engine code that only knew
// Actor() (the poltergeist's senses) asks this.
CEntityAlive* nearest_player(const Fvector& pos, float max_dist);
// F3a: the actor of a map server: no creature sees or hears it (plan §1.6).
bool ghost(const CObject* obj);
// Protocol 29, the server's debug (docs/DEBUG_DESIGN.md §B.9, work package 4): `obj` is a player with the personal flag "invisible" and
// this process runs the AI of his level (the world server: the host player's actor and the clients' proxies; a map server: the proxies
// of its players). The AI leaves him out as the stock ai_ignore_actor leaves out the actor: the memory managers do not take him (vision,
// hearing, hits, a smart cover's hit callback), he is no enemy (player_out_for) and nobody's nearest player. False on a player's
// machine (puppets), out of a session, while the debug is off; cheap while nobody has the flag. Lua: coop.debug.unseen(id).
bool debug_unseen(const CObject* obj);
// A.2 p. 7: does a monster's stamina blow (a burer's anti-aim, a controller's melee) spare `who` - this machine's actor, or a remote
// player's character the authority would send the blow to? In a session the server's flag god of his player decides, not this process's
// AF_GODMODE (a g_god of before the session); the actor's scripted god (AF_GODMODE_RT) still spares; a ghost is spared. No session: the
// stock GodMode().
bool player_god(const CObject* who);
// Audit F10: who a phantom born at `pos` flies at: the nearest player who is on his feet, of all the players, this machine's
// view of it. A phantom has no snapshot, every machine's copy picks by itself. Stock (no session, nobody): this machine's entity.
CObject* phantom_target(const Fvector& pos);
// Audit A6: every such player within `max_dist` of `pos` (this machine's actor first); returns how many were written.
constexpr u32 kMaxNearPlayers = 8;
u32 players_near(const Fvector& pos, float max_dist, CEntityAlive* out[kMaxNearPlayers]);
// Does `player` (this machine's actor or a remote player's character) see `monster` now? The actor: its visual memory;
// a remote player: his reported view and a static ray from his head (audit F1). Unlike player_sees it does not ask what
// `monster` sees: CMonsterEnemyMemory asks this to alert a monster that sees nothing.
bool player_sees_now(const CEntityAlive* player, const CCustomMonster* monster);
// Audit F3c: a remote player's character reported the start of a reload (coop_presence); player_reloading: a moment ago.
void on_player_reload(u16 player);
bool player_reloading(const CEntityAlive* player);
// Audit F3d: `player` is in a jump - this machine's actor, or a remote player by the move state his machine reports.
bool player_jumping(const CObject* player);
// Audit A5: where `player` looks - this machine's actor: its camera; a remote player's character: the view his
// machine reports. False for anything else or a view never reported.
bool player_view_dir(const CEntityAlive* player, Fvector& dir);
void viewers_clear();
// Console: coop_viewer_range (m, 0 = viewers off), coop_viewer_frame_radius (m, 0 = scale only). Host settings.
void register_console();

void print_status();
} // namespace npc_ai
} // namespace coop
