// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop - TZ P50 (Dev 58): collision between players is off where they appear and at level exits, and comes back when
// they move apart (the rule: xrCoop/coop_player_touch_rules.h).
//
// Runs where a person plays this process's actor (mode().actor_is_player()), for that actor against the characters of the other
// players seen here; elsewhere it marks nothing. The marks are the collide class bits of the bodies (CPHCollideValidator::
// cbClassPlayer on every player's character here, cbNCClassPlayer on an avatar whose pair with the actor is off): the physics
// skips the contacts of such a pair and nothing else. Outside a session nothing is marked (single player stays stock).
#pragma once

class CGameObject;
struct lua_State;

namespace coop
{
namespace player_touch
{
// Level_network_spawn, after net_Spawn (coop_remote.cpp on_net_spawn): the actor or another player's character came into this
// world: every pair of the actor (or this one pair) starts off.
void on_spawn(CGameObject* obj);
// CPHMovementControl::CreateCharacter: a character body was built (at the spawn, or later: a stalker's body is made when it
// is needed). A player's body gets its marks before the physics' next step.
void on_body(CGameObject* obj);
// Each frame of a session whose world is up (coop_sync.cpp frame): the actor's pairs, one by one.
void frame();
// The session ends or its world is replaced (coop_sync.cpp reset): every mark is taken off, the state forgotten.
void reset();
// coop.player_touch_zones({id, ...}): the route restrictors of the level (scripts\coop_transition_route.script lists them):
// level exits as much as the level changers are.
void register_lua(lua_State* L);
void print_status();
} // namespace player_touch
} // namespace coop
