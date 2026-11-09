// Anomaly Coop (live 2026-11-09, a diagnostic: it only logs, nothing in the game changes): the health ledger of this machine's
// own actor. The rules and the line are xrCoop/coop_health_ledger.h; these hooks tell it what changed the health of the own
// actor (g_actor: the host's, a client's) and Manager::frame writes the line "[coop] health a -> b in 15 s: scripts ...".
// Only in a co-op session, never for the ghost actor of a map server or a dedicated host. Each hook first checks that the
// target is the own actor (a pointer compare), so every other object costs nothing.
#pragma once

class CEntity;
class CEntityAlive;
class CEntityCondition;
class CActorCondition;
class CActor;

namespace coop
{
namespace health_ledger
{
// game_object.health = v and game_object:set_health_ex(v): `was` is the health before the write, the ledger reads the one
// after it. Grouped by the Lua caller (file:line).
void script_set(const CEntity* who, float was);
// game_object:change_health(v): what CEntityCondition::ChangeHealth queued (the next condition update applies it).
void script_change(const CEntityAlive* who, float value);
// A co-op write of the own actor's health (downed, revive, respawn, the restore after a join): exactly c.SetHealth(value),
// and the ledger books the change as the co-op's.
void coop_set_health(CEntityCondition& c, float value);
// CActorCondition::UpdateCondition, right after CEntityCondition::UpdateCondition: `was` is the health before it.
void condition_update(const CActorCondition& c, float was);

// CActor::Hit, after co-op's own drop of the hit: the hit and its power as received (before scripts); while the scope lives,
// the condition update inside the hit (CEntityAlive::Hit -> CalcCondition -> UpdateCondition) is the hit's.
class HitScope
{
public:
	HitScope(const CActor* actor, float power);
	~HitScope();
	HitScope(const HitScope&) = delete;
	HitScope& operator=(const HitScope&) = delete;

private:
	bool on_;
};

// Manager::frame: the 15 s window and its line.
void frame();
} // namespace health_ledger
} // namespace coop
