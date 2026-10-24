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

namespace coop
{
namespace monster_fx
{
namespace
{
u64 g_sent = 0, g_played = 0, g_dropped = 0;
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
	if (!actor || !actor->g_Alive() || actor->getDestroy())
		return;
	Playing playing;
	switch (kind)
	{
	case kGiantKick:
		if (CPseudoGigant* giant = smart_cast<CPseudoGigant*>(monster))
		{
			giant->kick_actor(actor, value);
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
		++g_dropped;
		return;
	}
	play(monster, kind, phase, value);
}
} // namespace

bool client_playing() { return g_playing; }

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
	::Msg("[coop]   monster abilities on remote players: %llu sent, %llu played here, %llu dropped", g_sent, g_played,
	    g_dropped);
}
} // namespace monster_fx
} // namespace coop
