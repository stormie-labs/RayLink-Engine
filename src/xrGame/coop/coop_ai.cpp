#include "stdafx.h"
#include "pch_script.h"
#include "coop_ai.h"
#include "coop_manager.h"
#include "coop_world.h"
#include "coop_players.h"
#include "coop_join.h"
#include "coop_survival.h"
#include "xrCoop/coop_viewers.h"

#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "CustomMonster.h"
#include "ai/stalker/ai_stalker.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "stalker_movement_manager_smart_cover.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "Torch.h"
#include "ai_space.h"
#include "../xrServerEntities/script_engine.h"
#include "../xrEngine/IGame_Persistent.h"
#include "../xrEngine/Environment.h"
#include "../xrEngine/sheduler_viewers.h"
#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"

namespace coop
{
namespace npc_ai
{
namespace
{
constexpr u32 kLightPeriodMs = 500; // light estimate refresh per proxy
constexpr u32 kLightFreshMs = 3000; // an older estimate is not used (proxy not driven any more)
constexpr u32 kForgetMs = 10000; // records of proxies not driven for this long are dropped
constexpr float kChestHeight = 1.2f; // CROS_impl samples at the visual's centre + 0.3 R (~1.2 m on a stalker)
constexpr float kSunRange = 500.f; // as CROS_impl::calc_sun_value
constexpr float kSkyRange = 50.f; // as CROS_impl::calc_sky_hemi_value
constexpr float kSkyScale = 0.08f; // r2_dhemi_sky_scale default: the sky adds little to the average
constexpr float kHeadlampLum = 1.f; // the own headlamp a few cm away saturates CROS_impl's light sum
constexpr float kViewCos = 0.57f; // half view cone of a player ~55 degrees (screen plus a little side vision)
constexpr float kAwareDistance = 2.f; // that close a player notices an NPC whatever he looks at
constexpr float kHeadHeight = 1.5f; // look-at target on this machine's actor (stock uses its camera)

struct Proxy
{
	u32 driven_ms = 0; // Device.dwTimeGlobal of the last drive
	u32 light_ms = 0; // of the last light estimate
	float lum = 0.f;
	bool lum_valid = false;
	bool headlamp = false;
	Fvector view{};
	bool view_valid = false;
};

xr_map<u16, Proxy> g_proxies; // host: client proxies driven recently, by local id

// Console settings of the scheduler's viewers (coop_viewers.h has the meaning and the defaults).
float g_viewer_range = viewers::kDefaultRange;
float g_viewer_frame_radius = viewers::kDefaultFrameRadius;

struct Stats
{
	u64 light_estimates = 0, light_used = 0, sun_lit = 0, headlamps = 0;
	u64 sees_asked = 0, sees_yes = 0, on_path = 0, look_checks = 0, look_yes = 0;
} g_stat;

float max3(const Fvector& v) { return _max(v.x, _max(v.y, v.z)); }

// The render's per-object light (CROS_impl::update) without the render: ambient + sky * scale +
// sun if the sun ray is free + own headlamp. Point lights of the level (fires, lamps) are left out.
float estimate_light(CGameObject* obj, Proxy& p)
{
	p.headlamp = false;
	if (CInventoryOwner* io = obj->cast_inventory_owner())
	{
		for (PIItem item : io->inventory().m_all)
		{
			const CTorch* torch = smart_cast<const CTorch*>(item);
			if (torch && torch->torch_active())
			{
				p.headlamp = true;
				break;
			}
		}
	}
	if (p.headlamp)
	{
		++g_stat.headlamps;
		return kHeadlampLum;
	}
	if (!g_pGamePersistent || !g_pGamePersistent->Environment().CurrentEnv)
		return 0.5f;
	const CEnvDescriptor& env = *g_pGamePersistent->Environment().CurrentEnv;
	Fvector at = obj->Position();
	at.y += kChestHeight;
	Fvector sum = env.ambient;
	Fvector up;
	up.set(0.f, 1.f, 0.f);
	if (!Level().ObjectSpace.RayTest(at, up, kSkyRange, collide::rqtStatic, nullptr, obj))
		sum.mad(Fvector().set(env.hemi_color.x, env.hemi_color.y, env.hemi_color.z), kSkyScale);
	Fvector to_sun = env.sun_dir; // the light's direction, from the sun down
	to_sun.invert();
	to_sun.normalize_safe();
	if (to_sun.y > 0.f && !Level().ObjectSpace.RayTest(at, to_sun, kSunRange, collide::rqtStatic, nullptr, obj))
	{
		sum.add(env.sun_color);
		++g_stat.sun_lit;
	}
	return clampr(max3(sum), 0.f, 1.f);
}

const Proxy* fresh_record(u16 id)
{
	auto it = g_proxies.find(id);
	if (it == g_proxies.end() || Device.dwTimeGlobal - it->second.driven_ms > kLightFreshMs)
		return nullptr;
	return &it->second;
}
} // namespace

bool is_player(const CObject* obj)
{
	if (!obj)
		return false;
	if (smart_cast<const CActor*>(obj))
		return true;
	return world::is_player_character(smart_cast<const CGameObject*>(obj));
}

bool is_remote_player(const CObject* obj)
{
	if (!obj || !mgr().active() || smart_cast<const CActor*>(obj))
		return false;
	return world::is_player_character(smart_cast<const CGameObject*>(obj));
}

void on_proxy_frame(CGameObject* proxy)
{
	if (!proxy || !mgr().is_host() || !is_remote_player(proxy))
		return;
	const u32 now = Device.dwTimeGlobal;
	Proxy& p = g_proxies[proxy->ID()];
	p.driven_ms = now;
	float yaw = 0.f, pitch = 0.f;
	p.view_valid = players::avatar_view(proxy->ID(), yaw, pitch);
	if (p.view_valid)
		p.view.setHP(yaw, -pitch);
	if (!p.lum_valid || now - p.light_ms >= kLightPeriodMs)
	{
		p.lum = estimate_light(proxy, p);
		p.lum_valid = true;
		p.light_ms = now;
		++g_stat.light_estimates;
	}
	// Drop the records of proxies that left (their objects may be gone: never dereferenced here).
	for (auto it = g_proxies.begin(); it != g_proxies.end();)
	{
		if (now - it->second.driven_ms > kForgetMs)
			it = g_proxies.erase(it);
		else
			++it;
	}
}

bool player_luminocity(const CGameObject* obj, float& lum)
{
	if (!obj || !is_remote_player(obj))
		return false;
	const Proxy* p = fresh_record(obj->ID());
	if (!p || !p->lum_valid)
		return false;
	lum = p->lum;
	++g_stat.light_used;
	return true;
}

bool player_sees(const CEntityAlive* player, const CCustomMonster* observer, bool& sees)
{
	if (!player || !is_remote_player(player))
		return false;
	++g_stat.sees_asked;
	sees = false;
	if (!observer || !player->g_Alive() || !observer->g_Alive())
		return true;
	// A player sees what sees him (the rays are the same) if it is in front of him.
	if (!observer->memory().visual().visible_now(player))
		return true;
	Fvector to;
	to.sub(observer->Position(), player->Position());
	const float distance = to.magnitude();
	const Proxy* p = fresh_record(player->ID());
	if (distance <= kAwareDistance || !p || !p->view_valid)
		sees = true;
	else
	{
		to.div(distance);
		sees = to.dotproduct(p->view) >= kViewCos;
	}
	if (sees)
		++g_stat.sees_yes;
	return true;
}

bool player_on_the_path(CAI_Stalker* npc)
{
	if (!npc || g_proxies.empty() || !mgr().is_host() || !g_pGameLevel)
		return false;
	const u32 now = Device.dwTimeGlobal;
	for (const auto& it : g_proxies)
	{
		if (now - it.second.driven_ms > kLightFreshMs)
			continue;
		CEntityAlive* player = smart_cast<CEntityAlive*>(Level().Objects.net_Find(it.first));
		if (!player || player->getDestroy() || !player->g_Alive())
			continue;
		if (!npc->is_relation_enemy(player) || !npc->memory().visual().visible_now(player))
			continue;
		if (npc->movement().is_object_on_the_way(player, 2.f))
		{
			++g_stat.on_path;
			return true;
		}
	}
	return false;
}

bool puppet_may_look_at_actor(CAI_Stalker* npc, float max_distance)
{
	if (!npc || !npc->g_Alive() || world::is_player_character(npc) || !g_pGameLevel)
		return false;
	CActor* actor = g_actor;
	if (!actor || !actor->g_Alive())
		return false;
	const float distance = actor->Position().distance_to(npc->Position());
	if (distance >= max_distance)
		return false;
	++g_stat.look_checks;
	// The stock Lua veto (CAI_Stalker::shedule_Update asks it for the actor the same way).
	::luabind::functor<bool> veto;
	if (npc->lua_game_object() && ::ai().script_engine().functor("_G.CNPCBeforeLookAtActor", veto) &&
	    !veto(npc->lua_game_object(), distance))
		return false;
	Fvector from = npc->eye_matrix.c;
	Fvector to = actor->Position();
	to.y += kHeadHeight;
	Fvector dir;
	dir.sub(to, from);
	const float range = dir.magnitude();
	if (range > EPS_L)
	{
		dir.div(range);
		if (Level().ObjectSpace.RayTest(from, dir, range, collide::rqtStatic, nullptr, npc))
			return false;
	}
	++g_stat.look_yes;
	return true;
}

void viewers_clear() { shedule_viewers::clear(); }

// Host: the character of `peer` if that player looks at the world from it (in the world, alive or downed), else nullptr.
CObject* viewer_body(u32 peer)
{
	const u16 proxy = join::proxy_of_peer(peer);
	if (proxy == 0xffff || proxy == 0 || !join::host_player_in_world(proxy) || world::is_parked(proxy))
		return nullptr; // no character, still loading, or gone: nobody looks at the world from there
	const survival::Life life = survival::life_of(proxy);
	if (life != survival::Life::Alive && life != survival::Life::Wounded)
		return nullptr; // dead, spectating or waiting for the respawn: the world is not seen from the body
	CObject* body = Level().Objects.net_Find(proxy);
	return body && !body->getDestroy() ? body : nullptr;
}

void viewers_frame()
{
	if (!mgr().is_host() || !world_up())
	{
		shedule_viewers::clear();
		return;
	}
	Fvector at[viewers::kMaxViewers];
	u32 n = 0;
	for (u32 peer = 1; peer < kMaxPlayers && n < viewers::kMaxViewers; ++peer)
		if (const CObject* body = viewer_body(peer))
			at[n++] = body->Position();
	shedule_viewers::set(at, n, g_viewer_range, g_viewer_frame_radius);
}

CEntityAlive* nearest_player(const Fvector& pos, float max_dist)
{
	CEntityAlive* best = nullptr;
	float best_d = max_dist;
	if (g_actor && g_actor->g_Alive() && !g_actor->getDestroy())
	{
		const float d = g_actor->Position().distance_to(pos);
		if (d <= best_d)
		{
			best = g_actor;
			best_d = d;
		}
	}
	if (!mgr().is_host() || !world_up())
		return best;
	for (u32 peer = 1; peer < kMaxPlayers; ++peer)
	{
		CEntityAlive* body = smart_cast<CEntityAlive*>(viewer_body(peer));
		if (!body || !body->g_Alive())
			continue;
		const float d = body->Position().distance_to(pos);
		if (d <= best_d)
		{
			best = body;
			best_d = d;
		}
	}
	return best;
}

void register_console()
{
	CMD4(CCC_Float, "coop_viewer_range", &g_viewer_range, 0.f, viewers::kMaxRange); // host setting
	CMD4(CCC_Float, "coop_viewer_frame_radius", &g_viewer_frame_radius, 0.f, viewers::kMaxFrameRadius); // host setting
}

void print_status()
{
	const shedule_viewers::Stats v = shedule_viewers::stats();
	static u64 s_prev_boosted = 0;
	static u32 s_prev_frame = 0;
	const u32 frames = Device.dwFrame - s_prev_frame;
	const double per_frame = frames ? double(v.frame_boosted - s_prev_boosted) / double(frames) : 0.0;
	s_prev_boosted = v.frame_boosted;
	s_prev_frame = Device.dwFrame;
	::Msg("[coop]   ai: scheduler viewers %u listed (range %.0f m, every-frame radius %.0f m; console coop_viewer_range, "
	      "coop_viewer_frame_radius) | shorter update interval for %llu of %llu asks | NPCs made crows by players: %llu of %llu "
	      "asks, %.1f NPCs per frame since the last status",
	    v.viewers, g_viewer_range, viewers::frame_radius(g_viewer_range, g_viewer_frame_radius), v.scale_boosted,
	    v.scale_queries, v.frame_boosted, v.frame_queries, per_frame);
	::Msg("[coop]   ai: %u proxies tracked | light: %llu estimates (%llu sun-lit, %llu headlamp), %llu used by vision | "
	      "player sees: %llu asked, %llu yes | player on path %llu | look-at checks %llu, %llu yes",
	    (u32)g_proxies.size(), g_stat.light_estimates, g_stat.sun_lit, g_stat.headlamps, g_stat.light_used,
	    g_stat.sees_asked, g_stat.sees_yes, g_stat.on_path, g_stat.look_checks, g_stat.look_yes);
	for (const auto& it : g_proxies)
		::Msg("[coop]   ai: proxy %u light %.2f%s, view %s", it.first, it.second.lum, it.second.headlamp ? " (headlamp)" : "",
		    it.second.view_valid ? "known" : "unknown");
}
} // namespace npc_ai
} // namespace coop
