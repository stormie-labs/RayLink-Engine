#include "stdafx.h"
#include "coop_remote.h"
#include "coop_manager.h"
#include "coop_sync.h"
#include "coop_world.h"
#include "coop_entities.h"
#include "coop_players.h"
#include "coop_ai.h"
#include "xrCoop/coop_rules.h"

#include "Level.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "EntityCondition.h"
#include "CustomMonster.h"
#include "movement_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager_smart_cover.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/trader/ai_trader.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "step_manager.h"
#include "script_binder.h"
#include "PhysicsShellHolder.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "ai_space.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "game_graph.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

namespace coop
{
namespace remote
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u16 kPartUnknown = u16(-2); // legs partition not looked up yet
constexpr float kMinAliveHealth = 0.001f;

// Per-puppet state, keyed by local id. An entry exists exactly while this module holds the
// object as an engine Remote object.
struct Puppet
{
	const IKinematicsAnimated* visual = nullptr; // legs_part belongs to this skeleton (avatars change visuals)
	u16 legs_part = kPartUnknown;
	u32 legs_motion = 0;
	const CBlend* legs_blend = nullptr;
};
xr_map<u16, Puppet> g_puppets;
u64 g_made_remote = 0, g_made_local = 0, g_deaths = 0, g_driven = 0, g_holstered = 0;

// W9: nobody talks to a player's character, on any machine: talk off means no "talk" use tip (CActor looks at
// IsTalkEnabled), OfferTalk refuses, Lua is_talk_enabled() is false. Its Lua binder update is suspended (no
// meet logic turns it back on); update_cl re-asserts it every frame anyway. A use tip a script sets on purpose
// (e.g. "help" on a wounded teammate) is left alone.
void mute_player_character(CGameObject* obj)
{
	if (!world::is_player_character(obj))
		return;
	CInventoryOwner* io = obj->cast_inventory_owner();
	if (io && io->IsTalkEnabled())
		io->DisableTalk();
}

// Yaw `a` with XFORM rotation == Fmatrix().rotateY(a), the convention of CCustomMonster's
// m_body (rotateY(a) has k = (sin a, 0, cos a)).
float body_yaw(const Fmatrix& m) { return angle_normalize_signed(atan2f(m.k.x, m.k.z)); }

CPHMovementControl* capsule(CGameObject* obj)
{
	CPhysicsShellHolder* holder = obj->cast_physics_shell_holder();
	CCharacterPhysicsSupport* support = holder ? holder->character_physics_support() : nullptr;
	CPHMovementControl* control = support ? support->movement() : nullptr;
	return control && control->CharacterExist() ? control : nullptr;
}

// Keep the engine's own orientation state equal to the driven XFORM: eye matrix, bone callbacks,
// Lua direction queries and a later switch back to Local all read it.
void sync_orientation(CCustomMonster* m, float yaw)
{
	MonsterSpace::SBoneRotation& body = m->movement().m_body;
	body.current.yaw = body.target.yaw = yaw;
	body.current.pitch = body.target.pitch = 0.f;
	if (CAI_Stalker* s = m->cast_stalker())
	{
		MonsterSpace::SBoneRotation& head = s->movement().m_head;
		head.current = body.current;
		head.target = body.current;
	}
}

// Back to Local: restart CCustomMonster's network state from where the puppet is now, otherwise
// its UpdateCL would snap it to the stale spawn sample.
void restart_net_state(CCustomMonster* m)
{
	const float yaw = body_yaw(m->XFORM());
	sync_orientation(m, yaw);
	CCustomMonster::net_update n;
	n.dwTimeStamp = Level().timeServer();
	n.o_model = yaw;
	n.o_torso = m->movement().m_body.current;
	n.p_pos = m->Position();
	n.fHealth = m->GetfHealth();
	m->NET.clear();
	m->NET.push_back(n);
	m->NET.push_back(n);
	m->NET_Last = n;
}

void make_remote(CGameObject* obj)
{
	obj->setLocal(FALSE);
	if (!obj->getVisible())
		obj->setVisible(TRUE); // a Remote CEntity is invisible by default
	if (!obj->getEnabled())
		obj->setEnabled(TRUE);
	if (CEntityAlive* alive = obj->cast_entity_alive())
		alive->conditions().SetCanBeHarmedState(false); // health comes from the host
	if (obj->animation_movement())
		obj->destroy_anim_mov_ctrl();
	if (CPHMovementControl* control = capsule(obj))
		control->SetVelocity(0.f, 0.f, 0.f);
	g_puppets[obj->ID()] = Puppet();
	++g_made_remote;
}

void make_local(CGameObject* obj)
{
	obj->setLocal(TRUE);
	if (CEntityAlive* alive = obj->cast_entity_alive())
		alive->conditions().SetCanBeHarmedState(true);
	if (CCustomMonster* m = obj->cast_custom_monster())
		restart_net_state(m);
	g_puppets.erase(obj->ID());
	++g_made_local;
}

// Footsteps and leg IK follow the blend reported by the engine's animation selectors through
// CStepManager::on_animation_start; puppets have no selector, so report the dominant blend of the
// legs partition ourselves whenever it changes.
void track_legs(CGameObject* obj, IKinematicsAnimated* ka, Puppet& p)
{
	CStepManager* steps = nullptr;
	if (CAI_Stalker* s = obj->cast_stalker())
		steps = s;
	else if (CBaseMonster* m = obj->cast_base_monster())
		steps = m;
	if (!steps)
		return;
	if (p.visual != ka)
	{
		p = Puppet();
		p.visual = ka;
	}
	if (p.legs_part == kPartUnknown)
	{
		p.legs_part = ka->LL_PartID("legs");
		if (p.legs_part == BI_NONE)
			p.legs_part = ka->LL_PartID("default");
	}
	if (p.legs_part == BI_NONE)
		return;
	CBlend* best = nullptr;
	for (u32 i = 0, n = ka->LL_PartBlendsCount(p.legs_part); i < n; ++i)
	{
		CBlend* b = ka->LL_PartBlend(p.legs_part, i);
		if (b && b->blend_state() == CBlend::eAccrue && (!best || b->blendAmount >= best->blendAmount))
			best = b;
	}
	if (!best || (best == p.legs_blend && best->motionID.val == p.legs_motion))
		return;
	p.legs_blend = best;
	p.legs_motion = best->motionID.val;
	steps->on_animation_start(best->motionID, best);
}

// Host: a remote object never exports to its server entity, so keep the player proxy's CSE where
// the player is (ALife switching around players, saves, AI that targets the proxy).
void sync_server_entity(CGameObject* obj)
{
	if (!ai().get_alife())
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(obj->ID(), true);
	if (!e)
		return;
	e->o_Position = obj->Position();
	const u32 node = obj->ai_location().level_vertex_id();
	if (ai().get_level_graph() && ai().level_graph().valid_vertex_id(node))
		e->m_tNodeID = node;
	const GameGraph::_GRAPH_ID graph = obj->ai_location().game_vertex_id();
	if (ai().get_game_graph() && ai().game_graph().valid_vertex_id(graph))
		e->m_tGraphID = graph;
	if (CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(e))
	{
		// What net_Export would have written for a Local stalker (o_model/o_torso = body/head yaw).
		const float yaw = body_yaw(obj->XFORM());
		creature->o_model = yaw;
		creature->o_torso.yaw = yaw;
		creature->o_torso.pitch = 0.f;
	}
}
} // namespace

bool is_puppet_type(CGameObject* obj)
{
	if (!obj)
		return false;
	if (obj->cast_stalker() || obj->cast_base_monster())
		return true;
	return obj->cast_entity_alive() && smart_cast<CAI_Trader*>(obj);
}

bool is_remote_creature(const CGameObject* obj)
{
	if (!obj || !mgr().active())
		return false;
	return is_puppet_type(const_cast<CGameObject*>(obj)) && sync::is_remote_driven(obj);
}

void on_net_spawn(CObject* o)
{
	CGameObject* obj = o ? smart_cast<CGameObject*>(o) : nullptr;
	if (!obj)
		return;
	g_puppets.erase(obj->ID()); // a previous object with this id is gone
	mute_player_character(obj);
	if (is_remote_creature(obj))
		make_remote(obj);
}

bool update_cl(CGameObject* obj)
{
	mute_player_character(obj);
	if (!obj || (!mgr().active() && g_puppets.empty()))
		return false;
	if (!is_remote_creature(obj))
	{
		if (!obj->Local() && g_puppets.count(obj->ID()))
			make_local(obj); // session over or no longer remote-driven: back to its own AI
		return false;
	}
	if (obj->Local())
		make_remote(obj); // became remote-driven after it spawned (e.g. proxy registered later)
	apply(obj);
	return true;
}

bool apply(CGameObject* obj)
{
	if (!is_remote_creature(obj) || obj->getDestroy())
		return false;
	CEntityAlive* alive = obj->cast_entity_alive();
	if (!alive || !alive->g_Alive())
		return false;

	IKinematicsAnimated* ka = obj->Visual() ? obj->Visual()->dcast_PKinematicsAnimated() : nullptr;
	if (ka)
		ka->UpdateTracks(); // the skipped engine selectors used to advance the tracks

	bool driven = false;
	if (world::is_player_proxy(obj->ID()))
	{
		if (CAI_Stalker* avatar = obj->cast_stalker())
			driven = players::drive_avatar(avatar);
	}
	else if (mgr().is_client())
		driven = entities::drive_mirror(obj);
	if (!driven)
		return false;

	if (CCustomMonster* m = obj->cast_custom_monster())
		sync_orientation(m, body_yaw(obj->XFORM()));
	if (CPHMovementControl* control = capsule(obj))
		control->SetPosition(obj->Position()); // collision capsule follows the puppet
	obj->validate_ai_locations(true);
	if (ka)
	{
		auto it = g_puppets.find(obj->ID());
		if (it != g_puppets.end())
			track_legs(obj, ka, it->second);
	}
	if (mgr().is_host() && world::is_player_proxy(obj->ID()))
	{
		sync_server_entity(obj);
		npc_ai::on_proxy_frame(obj); // W11: light and view for the NPCs that look at this player
	}
	++g_driven;
	return true;
}

bool suspend_binder(CScriptBinder* binder)
{
	if (!binder || !mgr().active())
		return false;
	CGameObject* obj = smart_cast<CGameObject*>(binder);
	if (!is_remote_creature(obj))
		return false;
	CEntityAlive* body = obj->cast_entity_alive();
	return !rules::puppet_binder_runs(obj->cast_base_monster() != nullptr, body && body->g_Alive());
}

void on_host_death(CGameObject* obj, u16 killer_local_id)
{
	if (!obj || obj->getDestroy())
		return;
	CEntityAlive* alive = obj->cast_entity_alive();
	if (!alive || !alive->g_Alive() || alive->AlreadyDie())
		return; // dead already, or GE_DIE is on its way
	if (!is_remote_creature(obj))
	{
		::Msg("! [coop] host death for %u [%s] ignored: not a remote creature here", obj->ID(), obj->cName().c_str());
		return;
	}
	// CEntityAlive::Die dereferences the killer: fall back to the victim itself.
	const bool killer_here = killer_local_id != kNoId && Level().Objects.net_Find(killer_local_id);
	alive->KillEntity(killer_here ? killer_local_id : obj->ID());
	++g_deaths;
}

// Live 2026-10-29 (build 29, owner's screenshot): on the client a dead NPC kept its gun in its hand; the gun went into
// the ground and "screamed". Stock empties a dead stalker's hands in its death planner (CStalkerActionDead drops the
// held item), which a puppet never runs (Remote: no Think), and its snapshots stop at death. A host drop comes as
// EntityParent; GAMMA's keep_guns_on_bodies keeps the gun with GE_TRADE_SELL + GE_TRADE_BUY in one frame, which
// events::host_flush relays as nothing ("back where it was"). So the gun stayed on the hand bone and its collision
// went into the hand element of the ragdoll (AddActiveWeaponCollision), a fixed 1e8 kg body the host's poses place:
// pushed into the terrain, it made contact sounds every step. The item goes into the corpse's inventory before the
// ragdoll is built; a drop from the host takes it out of there.
void on_puppet_die(CGameObject* obj)
{
	if (!obj || !mgr().is_client() || world::is_player_proxy(obj->ID()) || !is_remote_creature(obj))
		return;
	CInventoryOwner* owner = obj->cast_inventory_owner();
	PIItem item = owner ? owner->inventory().ActiveItem() : nullptr;
	if (!item)
		return;
	entities::ClientApply scope; // the host's word applied here: its weapon state events stay local
	if (owner->inventory().Ruck(item))
		++g_holstered;
}

void set_health(CGameObject* obj, float health)
{
	if (!is_remote_creature(obj))
		return;
	CEntityAlive* alive = obj->cast_entity_alive();
	if (!alive || !alive->g_Alive() || alive->AlreadyDie())
		return;
	alive->SetfHealth(_max(health, kMinAliveHealth));
}

void print_status()
{
	::Msg("[coop]   puppets: %u now, %llu made remote, %llu made local, %llu driven updates, %llu host deaths applied, "
	    "%llu held items put away at death",
	    (u32)g_puppets.size(), g_made_remote, g_made_local, g_driven, g_deaths, g_holstered);
	npc_ai::print_status();
}
} // namespace remote
} // namespace coop
