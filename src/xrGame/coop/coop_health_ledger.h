// A diagnostic, off unless the console's coop_health_ledger is 1 (it only logs, nothing in the game changes): the health
// ledger of this machine's own actor. The rules and the line are xrCoop/coop_health_ledger.h; these hooks tell it what changed the health of the own
// actor (g_actor: the host's, a client's) and Manager::frame writes the line "[coop] health a -> b in 15 s: scripts ...".
// Only in a co-op session, never for the ghost actor of a map server or a dedicated host. Each hook first checks that the
// target is the own actor (a pointer compare), so every other object costs nothing.
#pragma once

#include <atomic>

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
// The own actor's condition as the last frame() saw it (null without one). A caller of the condition_* hooks below asks
// is_own_condition(this) FIRST, before it computes the amount, so that every other entity's update pays one pointer
// compare and not CanBeHarmed() and change_v() just to build the argument. The hooks check again in full (a stale pointer
// only costs that check). Null while the ledger is off.
extern std::atomic<const CEntityCondition*> g_own_condition;
inline bool is_own_condition(const CEntityCondition* c)
{
	return c && c == g_own_condition.load(std::memory_order_relaxed);
}
// What the condition update queued into its health change, as the engine adds it to m_fDeltaHealth (a slow drain no
// other line names). `c` is any entity's condition: only the own actor's is booked.
void condition_bleeding(const CEntityCondition& c, float amount); // CEntityCondition::UpdateHealth
void condition_restore(const CEntityCondition& c, float amount); // the same: m_fV_HealthRestore + the health_restore booster
void condition_radiation(const CEntityCondition& c, float amount); // CEntityCondition::UpdateRadiation
void condition_satiety(const CEntityCondition& c, float amount); // CActorCondition::UpdateSatiety
// CActor::UpdateArtefactsOnBeltAndOutfit: a worn item's (`section`) health restore, `value` as given to ChangeHealth.
void worn(const CActor* actor, const char* section, float value);

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
// coop_health_ledger 0/1 (0 by default).
void register_console();
} // namespace health_ledger
} // namespace coop
