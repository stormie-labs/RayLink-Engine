// Anomaly Coop — monster abilities against remote players. See coop_monster_fx.h.
#include "stdafx.h"
#include "coop_monster_fx.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_world.h"
#include "coop_join.h"
#include "coop_ai.h"
#include "xrCoop/coop_local_link.h"
#include "Level.h"
#include "Actor.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/monsters/pseudogigant/pseudo_gigant.h"
#include "ai/monsters/controller/controller.h"
#include "ai/monsters/controller/controller_psy_hit.h"
#include "ai/monsters/burer/burer.h"
#include "ai/monsters/anti_aim_ability.h"
#include "ai/monsters/bloodsucker/bloodsucker.h"
#include "ai/monsters/pseudodog/psy_dog.h"

namespace coop
{
namespace monster_fx
{
namespace
{
u64 g_sent = 0, g_played = 0, g_dropped = 0, g_late = 0;
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
};
xr_vector<Pending> g_pending;
bool g_playing = false;

struct Playing
{
	Playing() { g_playing = true; }
	~Playing() { g_playing = false; }
};

// This machine's actor takes what the host decided for it.
void play(CBaseMonster* monster, u8 kind, u8 phase, float value)
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
			g_pending.push_back({id, kind, phase, value, Device.dwTimeGlobal + kPendingMs});
		else
			++g_dropped;
		return;
	}
	play(monster, kind, phase, value);
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
			play(monster, p.kind, p.phase, p.value);
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

void install_server(Session&) {}

void install_client(Endpoint& e)
{
	e.on(Msg::MonsterEffect, client_on_effect);
}

void print_status()
{
	::Msg("[coop]   monster abilities on remote players: %llu sent, %llu played here (%llu after their monster came), "
	      "%llu dropped, %u waiting; %u players held",
	    g_sent, g_played, g_late, g_dropped, (u32)g_pending.size(), (u32)g_holds.size());
}
} // namespace monster_fx
} // namespace coop
