// Anomaly Coop (live 2026-11-09, a diagnostic: it only logs): the health ledger of this machine's own actor. See
// coop_health_ledger.h here and xrCoop/coop_health_ledger.h (the rules, the line, the unit tests). The condition update and
// the scripts may run on the GameThread worker (mt_Scheduler) and the line is written on the main thread: every touch of
// the ledger is under g_lock, never held across a call out.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_health_ledger.h"
#include "coop_manager.h"
#include "xrCoop/coop_health_ledger.h"

#include "ai_space.h"
#include "script_engine.h"
#include "Actor.h"
#include "ActorCondition.h"
#include "entity_alive.h"

#include <cmath>
#include <string>

namespace coop
{
namespace health_ledger
{
std::atomic<const CEntityCondition*> g_own_condition{nullptr};

namespace
{
xrCriticalSection g_lock;
Ledger g_ledger;

bool on() { return mgr().active() && !ghost_actor(); }

bool own(const CEntity* who) { return who && g_actor && who == static_cast<const CEntity*>(g_actor) && on(); }

// The Lua function that called into the engine: its file and line. The first frame that is not C (luabind's own are).
std::string lua_caller()
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return "?";
	lua_Debug ar;
	for (int level = 0; level <= 8 && lua_getstack(L, level, &ar); ++level)
	{
		if (!lua_getinfo(L, "Sl", &ar))
			break;
		if (ar.what && ar.what[0] == 'C')
			continue;
		return caller_key(ar.short_src, ar.currentline);
	}
	return "?";
}

Vitals vitals_of(CActorCondition& c)
{
	Vitals v;
	v.bleeding = c.BleedingSpeed();
	v.radiation = c.GetRadiation();
	v.satiety = c.GetSatiety(); // CActorCondition's own: the base class always says 1
	v.psy = c.GetPsyHealth();
	const CEntityCondition::BOOSTER_MAP boosters = c.GetCurBoosterInfluences();
	for (const auto& b : boosters)
	{
		const int type = int(b.first);
		std::string name = type >= 0 && type < int(eBoostMaxCount) ? ef_boosters_section_names[type] : "?";
		if (name.compare(0, 6, "boost_") == 0)
			name.erase(0, 6);
		v.boosters.push_back({name, b.second.fBoostValue});
	}
	return v;
}
} // namespace

void script_set(const CEntity* who, float was)
{
	if (!own(who))
		return;
	const float delta = who->GetfHealth() - was;
	if (delta == 0.f || !std::isfinite(delta))
		return;
	const std::string caller = lua_caller();
	xrCriticalSectionGuard guard(g_lock);
	g_ledger.script(caller, delta);
}

void script_change(const CEntityAlive* who, float value)
{
	if (!own(who))
		return;
	const float queued = (who->conditions().CanBeHarmed() || value > 0) ? value : 0.f; // as CEntityCondition::ChangeHealth
	if (queued == 0.f || !std::isfinite(queued))
		return;
	const std::string caller = lua_caller();
	xrCriticalSectionGuard guard(g_lock);
	g_ledger.script_queued(caller, queued);
}

void coop_set_health(CEntityCondition& c, float value)
{
	const float was = c.GetHealth();
	c.SetHealth(value);
	if (!g_actor || &c != static_cast<CEntityCondition*>(&g_actor->conditions()) || !on())
		return;
	const float delta = c.GetHealth() - was;
	if (delta == 0.f || !std::isfinite(delta))
		return;
	xrCriticalSectionGuard guard(g_lock);
	g_ledger.coop(delta);
}

void condition_update(const CActorCondition& c, float was)
{
	if (&c.object() != g_actor || !on())
		return;
	const float delta = c.GetHealth() - was;
	if (!std::isfinite(delta))
		return;
	// at the maximum after the update: what it would have added was clamped away (CEntityCondition::UpdateCondition)
	const bool at_full = c.GetHealth() >= c.GetMaxHealth();
	xrCriticalSectionGuard guard(g_lock);
	g_ledger.condition(delta, was > 0.f, at_full); // at health <= 0 CEntityCondition::UpdateCondition returns at once, its queue stays
}

namespace
{
bool own_condition(const CEntityCondition& c)
{
	return g_actor && &c == static_cast<const CEntityCondition*>(&g_actor->conditions()) && on();
}

void part(const CEntityCondition& c, Part p, float amount)
{
	if (amount == 0.f || !own_condition(c) || !std::isfinite(amount))
		return;
	xrCriticalSectionGuard guard(g_lock);
	g_ledger.condition_part(p, amount);
}
} // namespace

void condition_bleeding(const CEntityCondition& c, float amount) { part(c, Part::Bleeding, amount); }
void condition_restore(const CEntityCondition& c, float amount) { part(c, Part::Restore, amount); }
void condition_radiation(const CEntityCondition& c, float amount) { part(c, Part::Radiation, amount); }
void condition_satiety(const CEntityCondition& c, float amount) { part(c, Part::Satiety, amount); }

void worn(const CActor* actor, const char* section, float value)
{
	if (!actor || actor != g_actor || !on())
		return;
	const float queued = (g_actor->conditions().CanBeHarmed() || value > 0) ? value : 0.f; // as CEntityCondition::ChangeHealth
	if (queued == 0.f || !std::isfinite(queued))
		return;
	const std::string key = section && *section ? section : "?";
	xrCriticalSectionGuard guard(g_lock);
	g_ledger.worn(key, queued);
}

HitScope::HitScope(const CActor* actor, float power) : on_(actor && actor == g_actor && on())
{
	if (!on_)
		return;
	xrCriticalSectionGuard guard(g_lock);
	g_ledger.hit_begin(power);
}

HitScope::~HitScope()
{
	if (!on_)
		return;
	xrCriticalSectionGuard guard(g_lock);
	g_ledger.hit_end();
}

void frame()
{
	if (ghost_actor())
	{
		g_own_condition.store(nullptr, std::memory_order_relaxed);
		return;
	}
	CActor* a = world_up() ? g_actor : nullptr;
	g_own_condition.store(a && on() ? static_cast<const CEntityCondition*>(&a->conditions()) : nullptr,
	    std::memory_order_relaxed);
	std::string line;
	bool report = false;
	{
		xrCriticalSectionGuard guard(g_lock);
		if (!a)
		{
			g_ledger.stop();
			return;
		}
		CActorCondition& c = a->conditions();
		report = g_ledger.frame(mgr().now_ms(), reinterpret_cast<uintptr_t>(a), c.GetHealth(), line, [&c] { return vitals_of(c); });
	}
	if (report)
		::Msg("[coop] %s", line.c_str());
}
} // namespace health_ledger
} // namespace coop
