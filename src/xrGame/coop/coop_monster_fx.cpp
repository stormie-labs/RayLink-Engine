// Anomaly Coop — monster abilities against remote players. See coop_monster_fx.h.
#include "stdafx.h"
#include "coop_monster_fx.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_world.h"
#include "coop_join.h"
#include "coop_ai.h"
#include "xrCoop/coop_local_link.h"
#include "xrCoop/coop_enemy_rules.h"
#include "Level.h"
#include "Actor.h"
#include "Hit.h"
#include "PhysicsShellHolder.h"
#include "../xrphysics/PhysicsShell.h"
#include "../xrServerEntities/xrMessages.h"
#include "../Include/xrRender/Kinematics.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/monsters/pseudogigant/pseudo_gigant.h"
#include "ai/monsters/controller/controller.h"
#include "ai/monsters/controller/controller_psy_hit.h"
#include "ai/monsters/burer/burer.h"
#include "ai/monsters/anti_aim_ability.h"
#include "ai/monsters/bloodsucker/bloodsucker.h"
#include "ai/monsters/pseudodog/psy_dog.h"
#include "ai/monsters/poltergeist/poltergeist.h"

namespace coop
{
namespace monster_fx
{
namespace
{
u64 g_sent = 0, g_played = 0, g_dropped = 0, g_late = 0, g_thrown_hits = 0;
constexpr u32 kPendingMs = 1500;
constexpr size_t kMaxPending = 32;

struct Hold
{
	u16 target;
	u16 monster;
};
xr_vector<Hold> g_holds;

struct Pending
{
	u16 id; // local numbering
	u8 kind;
	u8 phase;
	float value;
	u32 until;
	Fvector at, dir; // kPolterFlame
};
xr_vector<Pending> g_pending;
bool g_playing = false;

// Audit F4: an object a monster threw, followed until it hits a remote player or its flight ends.
struct Throw
{
	u16 object, monster;
	float power, mass;
	Fvector last; // where it was at last_ms
	u32 last_ms, until;
};
constexpr size_t kMaxThrows = 32;
xrCriticalSection g_thrown_lock; // AI think, which throws, may run on the game thread
xr_vector<Throw> g_thrown; // under the lock, frame() takes them
xr_vector<Throw> g_flights;

struct Playing
{
	Playing() { g_playing = true; }
	~Playing() { g_playing = false; }
};

// This machine's actor takes what the host decided for it.
void play(CBaseMonster* monster, u8 kind, u8 phase, float value, const Fvector& at, const Fvector& dir)
{
	CActor* actor = g_actor;
	if (!actor || actor->getDestroy())
		return;
	const bool alive = actor->g_Alive(); // a dead actor still has an ability's grip on it let go (the tube's end)
	Playing playing;
	switch (kind)
	{
	case kGiantKick:
		if (!alive)
			break;
		if (CPseudoGigant* giant = smart_cast<CPseudoGigant*>(monster))
		{
			giant->kick_actor(actor, value);
			++g_played;
			return;
		}
		break;
	case kControllerTube:
		if (CController* controller = smart_cast<CController*>(monster))
		{
			controller->m_psy_hit->coop_play(phase, value);
			++g_played;
			return;
		}
		break;
	case kControllerControl:
		if (!alive)
			break;
		if (CController* controller = smart_cast<CController*>(monster))
		{
			controller->control_fx_on_actor();
			++g_played;
			return;
		}
		break;
	case kControllerStamina:
		if (!alive)
			break;
		if (CController* controller = smart_cast<CController*>(monster))
		{
			controller->stamina_hit_actor();
			++g_played;
			return;
		}
		break;
	case kAntiAim:
		if (!alive)
			break;
		if (anti_aim_ability* ability = monster->get_anti_aim())
		{
			ability->coop_play(phase);
			++g_played;
			return;
		}
		break;
	case kBloodsuckerVampire:
		if (phase == 0 && !alive)
			break; // the letting go plays on a dead actor too
		if (CAI_Bloodsucker* bloodsucker = smart_cast<CAI_Bloodsucker*>(monster))
		{
			bloodsucker->coop_vampire(phase);
			++g_played;
			return;
		}
		break;
	case kPsyDogAppear:
		if (!alive)
			break;
		if (CPsyDogPhantom* phantom = smart_cast<CPsyDogPhantom*>(monster))
		{
			phantom->appear_fx_on_actor();
			++g_played;
			return;
		}
		break;
	case kBurerGravi:
		if (!alive)
			break;
		if (CBurer* burer = smart_cast<CBurer*>(monster))
		{
			burer->coop_gravi_at_actor();
			++g_played;
			return;
		}
		break;
	case kMeleeHit:
		if (!alive)
			break;
		{
			// rebuilt here: the blow's own direction stays on the host
			Fvector to_actor;
			to_actor.sub(actor->Position(), monster->Position());
			to_actor.normalize_safe();
			Fvector shove = to_actor;
			monster->melee_fx_on_actor(value, to_actor, shove);
		}
		++g_played;
		return;
	case kPolterFlame:
		if (CPoltergeist* polter = smart_cast<CPoltergeist*>(monster))
		{
			polter->coop_flame(at, dir);
			++g_played;
			return;
		}
		break;
	default: break;
	}
	(void)phase;
	++g_dropped;
}

void client_on_effect(PeerId, Reader& r)
{
	const u16 id = join::to_local(r.u16());
	const u8 kind = r.u8();
	const u8 phase = r.u8();
	const float value = r.f32();
	Fvector at{}, dir{};
	if (kind == kPolterFlame)
	{
		float v[3] = {};
		r.vec3(v);
		at.set(v[0], v[1], v[2]);
		r.vec3(v);
		dir.set(v[0], v[1], v[2]);
	}
	if (!r.ok() || !mgr().is_live_client() || !g_pGameLevel || kind >= kKinds)
	{
		++g_dropped;
		return;
	}
	CBaseMonster* monster = smart_cast<CBaseMonster*>(Level().Objects.net_Find(id));
	if (!monster || monster->getDestroy())
	{
		// a monster spawned this instant (a psy dog's phantom) may come after its effect: it waits a moment
		if (g_pending.size() < kMaxPending)
			g_pending.push_back({id, kind, phase, value, Device.dwTimeGlobal + kPendingMs, at, dir});
		else
			++g_dropped;
		return;
	}
	play(monster, kind, phase, value, at, dir);
}

enemy_rules::Point point_of(const Fvector& v)
{
	return enemy_rules::Point{v.x, v.y, v.z};
}

// What the stock collision would have dealt a character of the player's kind (the actor's section) at `speed`.
float throw_power(const Throw& t, float speed)
{
	const float contact = enemy_rules::contact_speed(speed, t.mass, pSettings->r_float("actor", "ph_mass"));
	return enemy_rules::thrown_power(contact, pSettings->r_float("actor", "ph_crash_speed_min"),
	    pSettings->r_float("actor", "ph_crash_speed_max"), t.power);
}

// As the monster's own blow (HitEntity): named after a creature the hit goes to the player as PlayerHit.
void hit_player(CGameObject* monster, CEntityAlive* player, const Fvector& dir, float power)
{
	NET_Packet P;
	SHit hit;
	hit.GenHeader(GE_HIT, player->ID());
	hit.whoID = monster->ID();
	hit.weaponID = monster->ID();
	hit.dir = dir;
	hit.power = power;
	IKinematics* k = smart_cast<IKinematics*>(player->Visual());
	hit.boneID = k ? k->LL_GetBoneRoot() : u16(BI_NONE);
	hit.p_in_bone_space = Fvector().set(0.f, 0.f, 0.f);
	hit.impulse = 0.f;
	hit.hit_type = ALife::eHitTypeStrike;
	hit.Write_Packet(P);
	monster->u_EventSend(P);
	++g_thrown_hits;
}

// True: the flight is over. One step of the object against every remote player: at tens of m/s it covers a metre and more
// between two samples, and its Position() moves only when it is updated (far from the host: not every frame).
bool flight_over(Throw& t, u32 now)
{
	if (now > t.until)
		return true;
	CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(t.object));
	CGameObject* monster = smart_cast<CGameObject*>(Level().Objects.net_Find(t.monster));
	if (!object || !monster || object->getDestroy() || monster->getDestroy())
		return true;
	const Fvector pos = object->Position();
	if (now == t.last_ms || pos.similar(t.last))
		return false;
	const float step = pos.distance_to(t.last);
	const float speed = step / (float(now - t.last_ms) * 0.001f);
	CEntityAlive* near_players[npc_ai::kMaxNearPlayers];
	const u32 count = npc_ai::players_near(pos, step + enemy_rules::kThrowReach + enemy_rules::kThrowBodyHeight, near_players);
	for (u32 i = 0; i < count; ++i)
	{
		CEntityAlive* player = near_players[i];
		if (!npc_ai::is_remote_player(player))
			continue; // the host's actor takes the stock contact
		Fvector chest = player->Position();
		chest.y += enemy_rules::kThrowBodyHeight;
		const enemy_rules::Point closest = enemy_rules::closest_on_segment(point_of(t.last), point_of(pos), point_of(chest));
		if (enemy_rules::distance(closest, point_of(chest)) > enemy_rules::kThrowReach)
			continue;
		const float power = throw_power(t, speed);
		if (power <= 0.f)
			continue;
		Fvector from, dir;
		from.set(closest.x, closest.y, closest.z);
		dir.sub(chest, from);
		const float range = dir.magnitude();
		if (range > EPS_L)
		{
			dir.div(range);
			if (Level().ObjectSpace.RayTest(from, dir, range, collide::rqtStatic, nullptr, nullptr))
				continue; // it flew by behind a wall
		}
		else
			dir.set(0.f, 0.f, 1.f);
		hit_player(monster, player, dir, power);
		return true;
	}
	t.last = pos;
	t.last_ms = now;
	return false;
}

void host_flights(bool world_up)
{
	{
		xrCriticalSectionGuard guard(&g_thrown_lock);
		g_flights.insert(g_flights.end(), g_thrown.begin(), g_thrown.end());
		g_thrown.clear();
	}
	if (g_flights.empty())
		return;
	if (!world_up || !mgr().is_host())
	{
		g_flights.clear();
		return;
	}
	const u32 now = Device.dwTimeGlobal;
	g_flights.erase(std::remove_if(g_flights.begin(), g_flights.end(), [now](Throw& t) { return flight_over(t, now); }),
	    g_flights.end());
}
} // namespace

bool client_playing() { return g_playing; }

void host_hold(u16 target, u16 monster)
{
	g_holds.push_back({target, monster});
}

void host_unhold(u16 target, u16 monster)
{
	g_holds.erase(std::remove_if(g_holds.begin(), g_holds.end(),
	                   [&](const Hold& h) { return h.target == target && h.monster == monster; }),
	    g_holds.end());
}

bool host_held(u16 target)
{
	if (!g_pGameLevel)
	{
		g_holds.clear();
		return false;
	}
	g_holds.erase(std::remove_if(g_holds.begin(), g_holds.end(),
	                   [](const Hold& h) {
	                       const CEntityAlive* m = smart_cast<const CEntityAlive*>(Level().Objects.net_Find(h.monster));
	                       return !m || m->getDestroy() || !m->g_Alive();
	                   }),
	    g_holds.end());
	for (const Hold& h : g_holds)
		if (h.target == target)
			return true;
	return false;
}

void frame(bool world_up)
{
	host_flights(world_up);
	if (g_pending.empty())
		return;
	if (!world_up || !mgr().is_live_client())
	{
		g_dropped += g_pending.size();
		g_pending.clear();
		return;
	}
	const u32 now = Device.dwTimeGlobal;
	for (size_t i = 0; i < g_pending.size();)
	{
		const Pending p = g_pending[i];
		CBaseMonster* monster = smart_cast<CBaseMonster*>(Level().Objects.net_Find(p.id));
		if (monster && !monster->getDestroy())
		{
			g_pending.erase(g_pending.begin() + i);
			++g_late;
			play(monster, p.kind, p.phase, p.value, p.at, p.dir);
			continue;
		}
		if (now > p.until)
		{
			g_pending.erase(g_pending.begin() + i);
			++g_dropped;
			continue;
		}
		++i;
	}
}

bool host_send(const CObject* target, const CObject* monster, u8 kind, u8 phase, float value)
{
	if (!target || !monster || !mgr().is_host() || !npc_ai::is_remote_player(target))
		return false;
	const PeerId peer = world::avatar_peer(target->ID());
	if (peer == kInvalidPeer)
		return false;
	Writer w(12);
	w.u16(monster->ID());
	w.u8(kind);
	w.u8(phase);
	w.f32(value);
	net::host_send(peer, Msg::MonsterEffect, w);
	++g_sent;
	return true;
}

void host_flame(const CObject* monster, const Fvector& at, const Fvector& dir)
{
	if (!monster || !mgr().is_host())
		return;
	Writer w(36);
	w.u16(monster->ID());
	w.u8(kPolterFlame);
	w.u8(0);
	w.f32(0.f);
	const float position[3] = {at.x, at.y, at.z}, direction[3] = {dir.x, dir.y, dir.z};
	w.vec3(position);
	w.vec3(direction);
	net::host_broadcast(Msg::MonsterEffect, w, Lane::Unreliable); // a picture of a moment: no queue for a client that loads
	++g_sent;
}

void host_throw(CPhysicsShellHolder* object, const CObject* monster, float power)
{
	if (!object || !monster || !object->PPhysicsShell() || !mgr().is_host())
		return;
	Throw t;
	t.object = object->ID();
	t.monster = monster->ID();
	t.power = power;
	t.mass = object->PPhysicsShell()->getMass();
	t.last = object->Position();
	t.last_ms = Device.dwTimeGlobal;
	t.until = t.last_ms + enemy_rules::kThrowFlightMs;
	xrCriticalSectionGuard guard(&g_thrown_lock);
	if (g_thrown.size() < kMaxThrows)
		g_thrown.push_back(t);
}

void install_server(Session&) {}

void install_client(Endpoint& e)
{
	e.on(Msg::MonsterEffect, client_on_effect);
}

void print_status()
{
	::Msg("[coop]   monster abilities on remote players: %llu sent, %llu played here (%llu after their monster came), "
	      "%llu dropped, %u waiting; %u players held; thrown objects: %u flying, %llu hit",
	    g_sent, g_played, g_late, g_dropped, (u32)g_pending.size(), (u32)g_holds.size(), (u32)g_flights.size(), g_thrown_hits);
}
} // namespace monster_fx
} // namespace coop
