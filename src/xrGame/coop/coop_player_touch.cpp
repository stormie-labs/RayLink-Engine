// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop - collision between players where they appear and at level exits (coop_player_touch.h; the
// rule and the thresholds: xrCoop/coop_player_touch_rules.h).
//
// Who decides: the machine that plays the actor, for its own actor (its CPHActorCharacter is the only body here that the
// physics pushes on a player's behalf; the other players are avatars placed from their snapshots). What it uses: its actor, the
// avatars (characters of the player section; each one's distance counts only once players::drive_avatar has placed it from
// its player's snapshots since it spawned here, players::placed), the level's exits as this machine has them: the online
// level changers (g_lchangers) and the route restrictors coop_transition_route.script lists (coop.player_touch_zones), and
// the arrival places of the level: the destination points (m_tNextPosition) of every level changer of the game that
// leads here, read from this machine's ALife (all.spawn), the same on every machine. Nothing goes over the network: each
// player's machine decides for its own actor from facts both machines of a pair hold alike (the places, the positions, the
// appearance). Single player: no session, nothing here runs (on_here).
#include "stdafx.h"
#include "pch_script.h"
#include "coop_player_touch.h"
#include "coop_manager.h"
#include "coop_players.h"
#include "coop_world.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_player_touch_rules.h"
#include "xrCoop/coop_shapes.h"
#include "coop_lua.h" // lua_api::fill

#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "level_changer.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "game_graph.h"
#include "xrServer_Objects_ALife.h"
#include "space_restrictor.h"
#include "PhysicsShellHolder.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "../../xrPhysics/PHCharacter.h"
#include "../../xrPhysics/PHCollideValidator.h"
#include "../../xrEngine/xr_collide_form.h"

extern xr_vector<CLevelChanger*> g_lchangers; // level_changer.cpp: the exits of the level that are online

namespace coop
{
namespace player_touch
{
namespace
{
constexpr u16 kNone = 0xffff;
constexpr size_t kMaxZones = 256; // route restrictors of one level (a level has a dozen); more is not a list of exits
constexpr u32 kLogLines = 32; // pair switches logged per session
constexpr size_t kMaxPlaces = 256; // arrival places of one level (the game has 124 exits in all)

xr_map<u16, Pair> g_avatars; // this machine's numbering: the other players' characters in this world, the actor's pair with each
// the actor's flags of its last frame with a body (where it stood then, no memory beyond it: near_exit, near_spawn)
bool g_me_near = false; // the actor is at a level exit
bool g_me_at_place = false; // the actor is at an arrival place
xr_vector<float> g_places; // the arrival places of this level (x, y, z each), the same on every machine
bool g_places_read = false; // ... read for this world (the actor's spawn reads them again)
u16 g_me = kNone; // the actor whose body carries the player class
xr_vector<u16> g_zones; // the route restrictors of the level (coop.player_touch_zones), this machine's numbering
u32 g_logged = 0;
u64 g_switches = 0, g_spawns = 0;

bool on_here() { return mgr().active() && mode().actor_is_player(); }

// The character body of a creature (the capsule the physics moves), nullptr when it has none now (dead, not built yet).
CPHObject* body_of(CGameObject* obj)
{
	CPhysicsShellHolder* holder = obj ? obj->cast_physics_shell_holder() : nullptr;
	CCharacterPhysicsSupport* support = holder ? holder->character_physics_support() : nullptr;
	CPHMovementControl* control = support ? support->movement() : nullptr;
	if (!control || !control->CharacterExist())
		return nullptr;
	return control->PHCharacter();
}

void mark(CPHObject* body, bool player, bool passes_players)
{
	if (!body)
		return;
	_flags<CLClassBits>& f = body->collide_class_bits();
	f.set(CPHCollideValidator::cbClassPlayer, player ? TRUE : FALSE);
	f.set(CPHCollideValidator::cbNCClassPlayer, passes_players ? TRUE : FALSE);
}

CGameObject* object_here(u16 id)
{
	if (id == kNone || !g_pGameLevel)
		return nullptr;
	CGameObject* obj = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return obj && !obj->getDestroy() ? obj : nullptr;
}

void unmark(u16 id) { mark(body_of(object_here(id)), false, false); }

// Metres from p to the shapes of a level changer or a space restrictor (its CCF_Shape, placed as CCF_Shape::Contact places it).
float shape_distance(CGameObject* obj, const Fvector& p)
{
	ICollisionForm* form = obj ? obj->CFORM() : nullptr;
	if (!form || form->Type() != cftShape)
		return shapes::kFar;
	const Fmatrix& xf = obj->XFORM();
	const float at[3] = {p.x, p.y, p.z};
	float best = shapes::kFar;
	for (const CCF_Shape::shape_def& s : static_cast<CCF_Shape*>(form)->Shapes())
	{
		float d = shapes::kFar;
		if (s.type == 0) // CCF_Shape::add_sphere
		{
			Fvector center;
			xf.transform_tiny(center, s.data.sphere.P);
			const float c[3] = {center.x, center.y, center.z};
			d = shapes::to_sphere(at, c, s.data.sphere.R);
		}
		else if (s.type == 1) // CCF_Shape::add_box
		{
			Fmatrix box;
			box.mul_43(xf, s.data.box);
			const float i[3] = {box.i.x, box.i.y, box.i.z};
			const float j[3] = {box.j.x, box.j.y, box.j.z};
			const float k[3] = {box.k.x, box.k.y, box.k.z};
			const float c[3] = {box.c.x, box.c.y, box.c.z};
			d = shapes::to_box(at, i, j, k, c);
		}
		if (d < best)
			best = d;
	}
	return best;
}

// Metres from p to the nearest level exit of this level: an enabled level changer (the stock exit, as coop_join.cpp
// exit_touched counts them) or a route restrictor of the list.
float exit_distance(const Fvector& p)
{
	float best = shapes::kFar;
	for (CLevelChanger* lc : g_lchangers)
		if (lc && lc->IsLevelChangerEnabled())
			best = _min(best, shape_distance(lc, p));
	for (const u16 id : g_zones)
	{
		CGameObject* obj = object_here(id);
		if (obj && obj->cast_restrictor())
			best = _min(best, shape_distance(obj, p));
	}
	return best;
}

void log_switch(const CGameObject* obj, bool off, bool at_exit, bool at_spawn, float apart)
{
	++g_switches;
	if (++g_logged > kLogLines)
		return;
	const char* why = !off ? "apart" : at_exit ? "at a level exit" : at_spawn ? "at a spawn place" : "appeared";
	::Msg("[coop] touch: player character %u '%s' %s my actor (%s, %.1f m)", obj->ID(), obj->cName().c_str(),
	    off ? "passes through" : "collides with", why, apart);
}

void to_floats(const Fvector& v, float out[3])
{
	out[0] = v.x;
	out[1] = v.y;
	out[2] = v.z;
}

// The arrival places of this level, from this machine's ALife: the destination point of every level changer of the game
// whose next vertex is on this level (the stock exit's m_tNextPosition: where a single player's level change, the host's ring of a
// group transition and Server's arrival put who comes). all.spawn is the same on every machine, so are the places. No ALife
// (not up yet): none, read again next frame.
void read_places()
{
	g_places.clear();
	g_places_read = false;
	if (!ai().get_alife() || !ai().get_game_graph())
		return;
	const CGameGraph& gg = ai().game_graph();
	const GameGraph::_LEVEL_ID here = ai().alife().graph().level().level_id();
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(it.second);
		if (!lc || !gg.valid_vertex_id(lc->m_tNextGraphID) || gg.vertex(lc->m_tNextGraphID)->level_id() != here)
			continue;
		if (g_places.size() >= 3 * kMaxPlaces)
			break;
		g_places.push_back(lc->m_tNextPosition.x);
		g_places.push_back(lc->m_tNextPosition.y);
		g_places.push_back(lc->m_tNextPosition.z);
	}
	g_places_read = true;
	if (++g_logged <= kLogLines)
		::Msg("[coop] touch: %u arrival place(s) on this level: players pass through each other within %.0f m of them",
		    u32(g_places.size() / 3), kSpawnFar);
}

float place_distance(const float p[3]) { return to_places(g_places.data(), g_places.size() / 3, p); }

// A player character other than the actor: the pair starts off (new or appeared again) and the body passes players.
void track(CGameObject* obj, bool appeared_now)
{
	auto it = g_avatars.find(obj->ID());
	const bool start = it == g_avatars.end() || appeared_now;
	if (it == g_avatars.end())
		it = g_avatars.emplace(obj->ID(), Pair()).first;
	if (start)
		pair_start(it->second);
	mark(body_of(obj), true, it->second.off);
	if (appeared_now && ++g_logged <= kLogLines)
		::Msg("[coop] touch: player character %u '%s' appeared here: passes through my actor until both are %.0f m from the "
		      "level's arrival places and %.0f m apart",
		    obj->ID(), obj->cName().c_str(), kSpawnFar, kApart);
}
} // namespace

void on_spawn(CGameObject* obj)
{
	if (!obj || !on_here())
		return;
	if (obj->cast_actor())
	{
		// the actor came into this world (a load, an arrival): its spot may be any player's
		++g_spawns;
		for (auto& it : g_avatars)
			pair_start(it.second);
		g_me_near = false;
		g_me_at_place = false;
		g_places_read = false; // a world of its own (another level): its arrival places are read on its first frame
		g_me = obj->ID();
		mark(body_of(obj), true, false);
		for (auto& it : g_avatars)
			mark(body_of(object_here(it.first)), true, true);
		return;
	}
	if (!world::is_player_character(obj))
		return;
	++g_spawns;
	track(obj, true);
}

void on_body(CGameObject* obj)
{
	if (!obj || !on_here())
		return;
	if (obj->cast_actor())
		mark(body_of(obj), true, false);
	else if (world::is_player_character(obj))
		track(obj, false);
}

void frame()
{
	if (!on_here() || !g_pGameLevel)
	{
		reset();
		return;
	}
	if (!g_places_read)
		read_places();
	CGameObject* me = smart_cast<CGameObject*>(g_actor);
	CPHObject* mine = body_of(me);
	if (!mine)
		return; // no actor here now, or no body (dead, in a vehicle): the pairs keep their state; the actor's flags are taken
		        // anew from where it stands on its next frame with a body
	if (g_me != me->ID())
	{
		unmark(g_me);
		g_me = me->ID();
	}
	mark(mine, true, false);
	const Fvector at = me->Position();
	float my_pos[3];
	to_floats(at, my_pos);
	// where the actor stands now (no band, no memory: the other machines take its avatar's flags from its position alike)
	g_me_near = near_exit(g_me_near, exit_distance(at));
	g_me_at_place = near_spawn(g_me_at_place, place_distance(my_pos));
	for (auto it = g_avatars.begin(); it != g_avatars.end();)
	{
		CGameObject* obj = object_here(it->first);
		if (!obj || !world::is_player_character(obj))
		{
			it = g_avatars.erase(it); // gone from this world: its next spawn is a new pair
			continue;
		}
		Pair& a = it->second;
		const Fvector& his = obj->Position();
		float his_pos[3];
		to_floats(his, his_pos);
		const bool was_off = a.off;
		// his avatar counts from where his own snapshots put it, not from where it spawned here (the host's blind ring spot): the
		// distance, and whether he is at an arrival place
		pair_frame(a, my_pos, g_me_near || g_me_at_place, his_pos, players::placed(obj->ID()), exit_distance(his),
		    place_distance(his_pos));
		mark(body_of(obj), true, a.off);
		if (a.off != was_off)
			log_switch(obj, a.off, g_me_near || a.at_exit, g_me_at_place || a.at_place, at.distance_to(his));
		++it;
	}
}

void reset()
{
	// nothing of a session left (single player every frame); the arrival places of the last level too (a session whose
	// actor never got a body and saw nobody would leave them for the next one, on another level)
	if (g_me == kNone && g_avatars.empty() && g_zones.empty() && g_places.empty() && !g_places_read)
		return;
	if (g_pGameLevel)
	{
		unmark(g_me);
		for (const auto& it : g_avatars)
			unmark(it.first);
	}
	g_avatars.clear();
	g_me = kNone;
	g_me_near = false;
	g_me_at_place = false;
	g_places.clear();
	g_places_read = false;
	g_zones.clear();
	g_logged = 0;
}

void print_status()
{
	u32 off = 0, unplaced = 0, at_place = 0;
	for (const auto& it : g_avatars)
	{
		off += it.second.off ? 1 : 0;
		unplaced += players::placed(it.first) ? 0 : 1;
		at_place += it.second.at_place ? 1 : 0;
	}
	::Msg("[coop]   touch (TZ P50, P75): %u player character(s) here (%u not placed by their player yet, %u at an arrival place), "
	      "%u passing through my actor, actor at an exit %s, at an arrival place %s, %u route exit(s), %u arrival place(s)%s, "
	      "%llu spawns, %llu switches",
	    u32(g_avatars.size()), unplaced, at_place, off, g_me_near ? "yes" : "no", g_me_at_place ? "yes" : "no",
	    u32(g_zones.size()), u32(g_places.size() / 3), g_places_read ? "" : " (not read yet)", g_spawns, g_switches);
}

namespace
{
// coop.player_touch_zones({id, ...}) -> the number taken: the route restrictors of the level, this machine's numbering. The list
// replaces the last one; what is not a whole id is skipped.
int l_zones(lua_State* L)
{
	g_zones.clear();
	if (lua_istable(L, 1))
	{
		const int n = int(lua_objlen(L, 1));
		for (int i = 1; i <= n && g_zones.size() < kMaxZones; ++i)
		{
			lua_rawgeti(L, 1, i);
			if (lua_isnumber(L, -1))
			{
				const lua_Number v = lua_tonumber(L, -1);
				if (v >= 0 && v < lua_Number(kNone) && v == lua_Number(s64(v)))
					g_zones.push_back(u16(v));
			}
			lua_pop(L, 1);
		}
	}
	lua_pushinteger(L, lua_Integer(g_zones.size()));
	return 1;
}
} // namespace

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"player_touch_zones", &l_zones},
		{nullptr, nullptr},
	};
	lua_api::fill(L, nullptr, fns); // coop_script logs a missing coop
}
} // namespace player_touch
} // namespace coop
