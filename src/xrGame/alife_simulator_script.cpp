////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_simulator_script.cpp
//	Created 	: 25.12.2002
//  Modified 	: 13.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "alife_simulator.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_story_registry.h"
#include "script_engine.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "restriction_space.h"
#include "alife_graph_registry.h"
#include "alife_spawn_registry.h"
#include "alife_registry_container.h"
#include "xrServer.h"
#include "level.h"
#include "coop/coop_requests.h" // Anomaly Coop
#include "coop/coop_callgate.h" // Anomaly Coop (F2 T12)
#include "coop/coop_story.h" // Anomaly Coop (F2 T12 round 1): alife():give_info of our actor
#include "coop/coop_manager.h" // Anomaly Coop (F2 T12 round 1)
#include "Actor.h" // Anomaly Coop (F2 T12 round 1): g_actor
#include "InventoryOwner.h"

#include <luabind/iterator_policy.hpp>
#include <luabind/iterator_pair_policy.hpp>

using namespace luabind;

typedef xr_vector<std::pair<shared_str, int>> STORY_PAIRS;
typedef STORY_PAIRS SPAWN_STORY_PAIRS;
LPCSTR _INVALID_STORY_ID = "INVALID_STORY_ID";
LPCSTR _INVALID_SPAWN_STORY_ID = "INVALID_SPAWN_STORY_ID";
STORY_PAIRS story_ids;
SPAWN_STORY_PAIRS spawn_story_ids;

CALifeSimulator* alife()
{
	return (const_cast<CALifeSimulator*>(ai().get_alife()));
}

// Anomaly Coop (F2 T12): the call gate of a spawning alife() call on a client in the host's world (Run elsewhere).
// own: calls::own_spawn - the new object's parent is our actor or under it (ReqSpawn only creates items in our own
// inventory); a temporary object (create/clone_weapon with register = false) passes as own, its alife():register is
// gated. A refusal returns nil, as the request path does. position (may be null) must be finite; section names the log
// line. While a host command is applied the class rules still hold (round 1: as before T12, never the stock create).
static coop::calls::Gate coop_spawn_gate(const char* name, bool own, const Fvector* position, LPCSTR section)
{
	if (!coop::requests::lua_client())
		return coop::calls::Gate::Run;
	coop::callgate::Args args;
	args.pos = position;
	args.what = section;
	args.runs_while_applying = false;
	return coop::callgate::client_gate_own(name, own, args);
}

// Anomaly Coop (F2 T12 round 1 gate, review I2): alife():give_info / disable_info write the info registry directly,
// past the story hook, so a client's info of its own actor never reached the host and the story resync took it back.
// On a client in the host's world: our actor (id 0) takes the story path of db.actor:give_info_portion
// (story::on_info: sent to the host, or held, or applied here when it is this machine's), another's registry is the
// host's (refused). Without our actor's game object (a condlist read while loading: xr_logic pick_section_from_condlist
// with db.actor == nil) the story path cannot be asked: while loading the registry is written here as the story path
// itself does then (the resync at live heals it); live, it is refused with a log line.
// True = handled here; false = the stock registry write runs.
static bool coop_alife_info(const char* name, ALife::_OBJECT_ID id, LPCSTR info_id, bool give)
{
	if (!coop::requests::lua_client())
		return false;
	coop::callgate::Args args;
	args.runs_while_applying = false; // the story path itself knows the host's story being applied
	switch (coop::callgate::client_gate_own(name, id == 0, args))
	{
	case coop::calls::Gate::Run: return false;
	case coop::calls::Gate::Refuse: return true;
	case coop::calls::Gate::Request: break;
	}
	CInventoryOwner* actor = smart_cast<CInventoryOwner*>(g_actor);
	if (actor)
		return coop::story::on_info(actor, shared_str(info_id), give);
	if (!coop::mgr().is_live_client())
		return false;
	coop::callgate::note_refused(name, "our actor has no game object: the story path cannot be asked");
	return true;
}

CSE_ALifeDynamicObject* alife_object(const CALifeSimulator* self, ALife::_OBJECT_ID object_id)
{
	VERIFY(self);
	if (object_id == 0xffff)
	{
		Msg("alife():object(id) ! invalid id specified");
        ai().script_engine().print_stack();
		return (0);
	}
	return (self->objects().object(object_id, true));
}

bool valid_object_id(const CALifeSimulator* self, ALife::_OBJECT_ID object_id)
{
	VERIFY(self);
	return (object_id != 0xffff);
}

#ifdef DEBUG
CSE_ALifeDynamicObject *alife_object		(const CALifeSimulator *self, LPCSTR name)
{
	VERIFY			(self);
	
	for (CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator it = self->objects().objects().begin(); it != self->objects().objects().end(); it++) {
		CSE_ALifeDynamicObject	*obj = it->second;
		if (xr_strcmp(obj->name_replace(),name) == 0)
			return	(it->second);
	}
	
	return			(0);
}
#endif // #ifdef DEBUG

CSE_ALifeDynamicObject* alife_object(const CALifeSimulator* self, ALife::_OBJECT_ID id, bool no_assert)
{
	VERIFY(self);
	return (self->objects().object(id, no_assert));
}

CSE_ALifeDynamicObject* alife_story_object(const CALifeSimulator* self, ALife::_STORY_ID id)
{
	return (self->story_objects().object(id, true));
}

template <typename _id_type>
void generate_story_ids(
	STORY_PAIRS& result,
	_id_type INVALID_ID,
	LPCSTR section_name,
	LPCSTR INVALID_ID_STRING,
	LPCSTR invalid_id_description,
	LPCSTR invalid_id_redefinition,
	LPCSTR duplicated_id_description
)
{
	result.clear();

    const CInifile* Ini = pGameIni;

	LPCSTR N, V;
    u32 k = 0;
    R_ASSERT(Ini->section_exist(section_name));

    result.reserve(Ini->line_count(section_name) + 1);
    while (Ini->r_line(section_name, k, &N, &V))
	{
        const shared_str& temp = Ini->r_string_wb(section_name, N);

		R_ASSERT3(!strchr(*temp,' '), invalid_id_description, *temp);
		R_ASSERT2(xr_strcmp(*temp,INVALID_ID_STRING), invalid_id_redefinition);

        for (const auto& story : result)
            R_ASSERT3(story.first != temp, duplicated_id_description, *temp);

		result.push_back(std::make_pair(*temp, atoi(N)));
        ++k;
	}

	result.push_back(std::make_pair(INVALID_ID_STRING, INVALID_ID));
}

void kill_entity0(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, const GameGraph::_GRAPH_ID& game_vertex_id)
{
	if (coop::requests::lua_client_refuse("alife:kill_entity", monster ? monster->ID : ALife::_OBJECT_ID(-1))) return; // Anomaly Coop (F2 T12 gate): deaths are the host's
	alife->kill_entity(monster, game_vertex_id, 0);
}

void kill_entity1(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster)
{
	if (coop::requests::lua_client_refuse("alife:kill_entity", monster ? monster->ID : ALife::_OBJECT_ID(-1))) return; // Anomaly Coop (F2 T12 gate)
	alife->kill_entity(monster, monster->m_tGraphID, 0);
}

// Anomaly Coop: wrappers so a coop client can refuse these (bound as members before)
void kill_entity2(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, const GameGraph::_GRAPH_ID& game_vertex_id, CSE_ALifeSchedulable* schedulable)
{
	if (coop::requests::lua_client_refuse("alife:kill_entity", monster ? monster->ID : ALife::_OBJECT_ID(-1))) return; // F2 T12 gate
	alife->kill_entity(monster, game_vertex_id, schedulable);
}

void CALifeSimulator__set_switch_online(CALifeSimulator* self, ALife::_OBJECT_ID id, bool value)
{
	if (coop::requests::lua_client_refuse("alife:set_switch_online", id)) return; // the host switches objects (F2 T12 gate)
	self->set_switch_online(id, value);
}

void CALifeSimulator__set_switch_offline(CALifeSimulator* self, ALife::_OBJECT_ID id, bool value)
{
	if (coop::requests::lua_client_refuse("alife:set_switch_offline", id)) return; // F2 T12 gate
	self->set_switch_offline(id, value);
}

// Anomaly Coop (F2 T12 round 1 gate, review list item 5): restrictions and set_interactive change the host's ALife (a
// client's copy is frozen; an id it does not have crashed it): refused on a client, run while a host command is applied.
void add_in_restriction(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, ALife::_OBJECT_ID id)
{
	if (coop::requests::lua_client_refuse("alife:add_in_restriction", monster ? monster->ID : ALife::_OBJECT_ID(-1), true)) return;
	alife->add_restriction(monster->ID, id, RestrictionSpace::eRestrictorTypeIn);
}

void add_out_restriction(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, ALife::_OBJECT_ID id)
{
	if (coop::requests::lua_client_refuse("alife:add_out_restriction", monster ? monster->ID : ALife::_OBJECT_ID(-1), true)) return;
	alife->add_restriction(monster->ID, id, RestrictionSpace::eRestrictorTypeOut);
}

void remove_in_restriction(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, ALife::_OBJECT_ID id)
{
	if (coop::requests::lua_client_refuse("alife:remove_in_restriction", monster ? monster->ID : ALife::_OBJECT_ID(-1), true)) return;
	alife->remove_restriction(monster->ID, id, RestrictionSpace::eRestrictorTypeIn);
}

void remove_out_restriction(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, ALife::_OBJECT_ID id)
{
	if (coop::requests::lua_client_refuse("alife:remove_out_restriction", monster ? monster->ID : ALife::_OBJECT_ID(-1), true)) return;
	alife->remove_restriction(monster->ID, id, RestrictionSpace::eRestrictorTypeOut);
}

// Anomaly Coop (F2 T12 round 1): wrappers so a coop client can refuse these (bound as members before)
void CALifeSimulator__remove_all_restrictions(CALifeSimulator* self, ALife::_OBJECT_ID id, const RestrictionSpace::ERestrictorTypes& type)
{
	if (coop::requests::lua_client_refuse("alife:remove_all_restrictions", id, true)) return;
	self->remove_all_restrictions(id, type);
}

void CALifeSimulator__set_interactive(CALifeSimulator* self, ALife::_OBJECT_ID id, bool value)
{
	if (coop::requests::lua_client_refuse("alife:set_interactive", id, true)) return;
	self->set_interactive(id, value);
}

u32 get_level_id(CALifeSimulator* self)
{
	return (self->graph().level().level_id());
}

CSE_ALifeDynamicObject* CALifeSimulator__create(CALifeSimulator* self, ALife::_SPAWN_ID spawn_id)
{
	if (coop::requests::lua_client() && coop::callgate::client_gate_own("alife:create", false) != coop::calls::Gate::Run) return nullptr; // Anomaly Coop (F2 T12 gate): a spawn-graph object is a world spawn
	const CALifeSpawnRegistry::SPAWN_GRAPH::CVertex* vertex = ai().alife().spawns().spawns().vertex(spawn_id);
	THROW2(vertex, "Invalid spawn id!");

	CSE_ALifeDynamicObject* spawn = smart_cast<CSE_ALifeDynamicObject*>(&vertex->data()->object());
	THROW(spawn);

	CSE_ALifeDynamicObject* object;
	self->create(object, spawn, spawn_id);

	return (object);
}

CSE_Abstract* CALifeSimulator__spawn_item(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id)
{
	switch (coop_spawn_gate("alife:create", false, &position, section)) // Anomaly Coop (F2 T12 gate): no parent, a world spawn
	{
	case coop::calls::Gate::Request: return coop::requests::lua_create(section, position, level_vertex_id, game_vertex_id, ALife::_OBJECT_ID(-1), true);
	case coop::calls::Gate::Refuse: return nullptr;
	case coop::calls::Gate::Run: break;
	}
	THROW(self);
	return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, ALife::_OBJECT_ID(-1)));
}

CSE_Abstract* CALifeSimulator__spawn_item2(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent)
{
	switch (coop_spawn_gate("alife:create", coop::calls::own_spawn(true, coop::callgate::is_own(id_parent)), &position, section)) // Anomaly Coop (F2 T12 gate): player items via the host
	{
	case coop::calls::Gate::Request: return coop::requests::lua_create(section, position, level_vertex_id, game_vertex_id, id_parent, true);
	case coop::calls::Gate::Refuse: return nullptr;
	case coop::calls::Gate::Run: break;
	}
	if (id_parent == ALife::_OBJECT_ID(-1))
		return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent));

	CSE_ALifeDynamicObject* object = ai().alife().objects().object(id_parent, true);
	if (!object)
	{
		Msg("! invalid parent id [%d] specified", id_parent);
		return (0);
	}

	if (!object->m_bOnline)
		return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent));

	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(section);

	CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);
	item->Spawn_Write(packet,FALSE);
	self->server().FreeID(item->ID, 0);
	F_entity_Destroy(item);

	ClientID clientID;
	clientID.set(0xffff);

	u16 dummy;
	packet.r_begin(dummy);
	VERIFY(dummy == M_SPAWN);
	return (self->server().Process_spawn(packet, clientID));
}

//Alundaio: Allows to call alife():register(se_obj) manually afterward so that packet editing can be done safely when spawning object with a parent
CSE_Abstract* CALifeSimulator__spawn_item3(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, bool reg = true)
{
	switch (coop_spawn_gate("alife:create", coop::calls::own_spawn(reg, coop::callgate::is_own(id_parent)), &position, section)) // Anomaly Coop (F2 T12 gate): no id until the host creates it
	{
	case coop::calls::Gate::Request: return coop::requests::lua_create(section, position, level_vertex_id, game_vertex_id, id_parent, reg);
	case coop::calls::Gate::Refuse: return nullptr;
	case coop::calls::Gate::Run: break;
	}
	if (reg == true)
		return CALifeSimulator__spawn_item2(self, section, position, level_vertex_id, game_vertex_id, id_parent);

	if (id_parent == ALife::_OBJECT_ID(-1))
		return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent));

	CSE_ALifeDynamicObject* object = ai().alife().objects().object(id_parent, true);
	if (!object)
	{
		Msg("! invalid parent id [%d] specified", id_parent);
		return (0);
	}

	if (!object->m_bOnline)
		return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent));

	CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);

	return (item);
}

CSE_Abstract* CALifeSimulator__spawn_ammo(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, int ammo_to_spawn)
{
	switch (coop_spawn_gate("alife:create_ammo", coop::calls::own_spawn(true, coop::callgate::is_own(id_parent)), &position, section)) // Anomaly Coop (F2 T12 gate)
	{
	case coop::calls::Gate::Request: return coop::requests::lua_create(section, position, level_vertex_id, game_vertex_id, id_parent, true, ammo_to_spawn);
	case coop::calls::Gate::Refuse: return nullptr;
	case coop::calls::Gate::Run: break;
	}
	//	if (id_parent == ALife::_OBJECT_ID(-1))
	//		return							(self->spawn_item(section,position,level_vertex_id,game_vertex_id,id_parent));
	CSE_ALifeDynamicObject* object = 0;
	if (id_parent != ALife::_OBJECT_ID(-1))
	{
		object = ai().alife().objects().object(id_parent, true);
		if (!object)
		{
			Msg("! invalid parent id [%d] specified", id_parent);
			return (0);
		}
	}

	if (!object || !object->m_bOnline)
	{
		CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent);

		CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(item);
		THROW(ammo);
		THROW(ammo->m_boxSize >= ammo_to_spawn);
		ammo->a_elapsed = (u16)ammo_to_spawn;

		return (item);
	}

	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(section);

	CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);

	CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(item);
	THROW(ammo);
	THROW(ammo->m_boxSize >= ammo_to_spawn);
	ammo->a_elapsed = (u16)ammo_to_spawn;

	item->Spawn_Write(packet,FALSE);
	self->server().FreeID(item->ID, 0);
	F_entity_Destroy(item);

	ClientID clientID;
	clientID.set(0xffff);

	u16 dummy;
	packet.r_begin(dummy);
	VERIFY(dummy == M_SPAWN);
	return (self->server().Process_spawn(packet, clientID));
}

ALife::_SPAWN_ID CALifeSimulator__spawn_id(CALifeSimulator* self, ALife::_SPAWN_STORY_ID spawn_story_id)
{
	return (((const CALifeSimulator *)self)->spawns().spawn_id(spawn_story_id));
}

void CALifeSimulator__release(CALifeSimulator* self, CSE_Abstract* object, bool)
{
	if (coop::requests::lua_client() && coop::requests::lua_release(object)) return; // Anomaly Coop: the host releases (F2 T12 gate inside)
	VERIFY(self);
	//	self->release						(object,true);

	THROW(object);
	if (!object) return;
	CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(object);
	THROW(alife_object);
	if (!alife_object) return;
	if (!alife_object->m_bOnline)
	{
		self->release(object, true);
		return;
	}

	// awful hack, for stohe only
	NET_Packet packet;
	packet.w_begin(M_EVENT);
	packet.w_u32(Level().timeServer());
	packet.w_u16(GE_DESTROY);
	packet.w_u16(object->ID);
	Level().Send(packet, net_flags(TRUE,TRUE));
}

LPCSTR get_level_name(const CALifeSimulator* self, int level_id)
{
	const GameGraph::LEVEL_MAP& levels = ai().game_graph().header().levels();
	GameGraph::LEVEL_MAP::const_iterator I = levels.find((GameGraph::_LEVEL_ID)level_id);
	if (I == levels.end())
	{
		return NULL;
	}
	LPCSTR result = *ai().game_graph().header().level((GameGraph::_LEVEL_ID)level_id).name();
	return (result);
}

CSE_ALifeCreatureActor* get_actor(const CALifeSimulator* self)
{
	THROW(self);
	return (self->graph().actor());
}

KNOWN_INFO_VECTOR* registry(const CALifeSimulator* self, const ALife::_OBJECT_ID& id)
{
	THROW(self);
	return (self->registry().get<CInfoPortionRegistry>().object(id, true));
}

bool has_info(const CALifeSimulator* self, const ALife::_OBJECT_ID& id, LPCSTR info_id)
{
	const KNOWN_INFO_VECTOR* known_info = registry(self, id);
	if (!known_info)
		return (false);

	static shared_str has_info;
	has_info = info_id;

	if (std::find_if(known_info->begin(), known_info->end(), CFindByIDPred(has_info)) == known_info->end())
		return (false);

	return (true);
}

bool dont_has_info(const CALifeSimulator* self, const ALife::_OBJECT_ID& id, LPCSTR info_id)
{
	THROW(self);
	// absurdly, but only because of scriptwriters needs
	return (!has_info(self, id, info_id));
}

void AlifeGiveInfo(const CALifeSimulator *alife, const ALife::_OBJECT_ID &id, LPCSTR info_id)
{
	if (coop_alife_info("alife:give_info", id, info_id, true)) return; // Anomaly Coop (F2 T12 round 1 gate)
	KNOWN_INFO_VECTOR *known_info = alife->registry().get<CInfoPortionRegistry>().object(id, true);
	if (!known_info)
		return;

	static shared_str AlifeGiveInfo;
	AlifeGiveInfo = info_id;

	if (std::find_if(known_info->begin(), known_info->end(), CFindByIDPred(AlifeGiveInfo)) == known_info->end())
	{
		known_info->push_back(info_id);
	}

	return;
}

void AlifeRemoveInfo(const CALifeSimulator *alife, const ALife::_OBJECT_ID &id, LPCSTR info_id)
{
	if (coop_alife_info("alife:disable_info", id, info_id, false)) return; // Anomaly Coop (F2 T12 round 1 gate)
	KNOWN_INFO_VECTOR	*known_info = alife->registry().get<CInfoPortionRegistry>().object(id, true);
	if (!known_info)
		return;

	static shared_str AlifeRemoveInfo;
	AlifeRemoveInfo = info_id;

	known_info->erase(std::find_if(known_info->begin(), known_info->end(), CFindByIDPred(AlifeRemoveInfo)),known_info->end());
}

//Alundaio: teleport object
void teleport_object(CALifeSimulator* alife, ALife::_OBJECT_ID id, GameGraph::_GRAPH_ID game_vertex_id,
                     u32 level_vertex_id, const Fvector& position)
{
	if (coop::requests::lua_client_refuse("alife:teleport_object", id)) return; // Anomaly Coop: the host moves objects (F2 T12 gate)
	alife->teleport_object(id, game_vertex_id, level_vertex_id, position);
}

void IterateInfo(const CALifeSimulator* alife, const ALife::_OBJECT_ID& id, const ::luabind::functor<void>& functor)
{
	const KNOWN_INFO_VECTOR* known_info = registry(alife, id);
	if (!known_info)
		return;

	xr_vector<shared_str>::const_iterator I = known_info->begin();
	xr_vector<shared_str>::const_iterator E = known_info->end();
	for (; I != E; ++I)
		functor(id, (LPCSTR)(*I).c_str());
}

CSE_Abstract* reprocess_spawn(CALifeSimulator* self, CSE_Abstract* object)
{
	switch (coop_spawn_gate("alife:register", object && coop::callgate::is_own(object->ID_Parent), nullptr, object ? object->s_name.c_str() : nullptr)) // Anomaly Coop (F2 T12 gate)
	{
	case coop::calls::Gate::Request: return coop::requests::lua_register_spawn(object);
	case coop::calls::Gate::Refuse: return nullptr;
	case coop::calls::Gate::Run: break;
	}
	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(object->s_name);

	object->Spawn_Write(packet, FALSE);
	self->server().FreeID(object->ID, 0);
	F_entity_Destroy(object);

	ClientID clientID;
	clientID.set(0xffff);

	u16 dummy;
	packet.r_begin(dummy);

	return (self->server().Process_spawn(packet, clientID));
}

CSE_Abstract* try_to_clone_object(CALifeSimulator* self, CSE_Abstract* object, LPCSTR section, const Fvector& position,
                                  u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent,
                                  bool bRegister = true)
{
	switch (coop_spawn_gate("alife:clone_weapon", coop::calls::own_spawn(bRegister, coop::callgate::is_own(id_parent)), &position, section)) // Anomaly Coop (F2 T12 gate)
	{
	case coop::calls::Gate::Request: return coop::requests::lua_clone(object, section, position, level_vertex_id, game_vertex_id, id_parent, bRegister);
	case coop::calls::Gate::Refuse: return nullptr;
	case coop::calls::Gate::Run: break;
	}
	CSE_ALifeItemWeaponMagazined* wpnmag = smart_cast<CSE_ALifeItemWeaponMagazined*>(object);
	if (wpnmag)
	{
		CSE_Abstract* absClone = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);
		if (!absClone)
			return (0);

		CSE_ALifeItemWeaponMagazined* clone = smart_cast<CSE_ALifeItemWeaponMagazined*>(absClone);
		if (!clone)
			return (0);

		clone->wpn_flags = wpnmag->wpn_flags;
		clone->m_addon_flags = wpnmag->m_addon_flags;
		clone->m_fCondition = wpnmag->m_fCondition;
		clone->ammo_type = wpnmag->ammo_type;
		clone->m_upgrades = wpnmag->m_upgrades;
		clone->set_ammo_elapsed(wpnmag->get_ammo_elapsed());

		return bRegister ? reprocess_spawn(self, absClone) : absClone;
		// (self->server().Process_spawn(packet, clientID));
	}

	return (0);
}

void set_objects_per_update(CALifeSimulator* self, u32 count)
{
	self->objects_per_update(count);
}

void set_process_time(CALifeSimulator* self, int micro)
{
	self->set_process_time(micro);
}

void force_update(CALifeSimulator* self)
{
	self->update_scheduled(true);
}

// demonized: iterate alife objects, functor style
void CALifeSimulator__iterate_objects(const CALifeSimulator* self, const luabind::functor<bool>& functor)
{
	const CALifeObjectRegistry& objects = self->objects();
	for (const auto& se_obj : objects.objects()) {
		if (functor(se_obj.second)) break;
	}
}

void CALifeSimulator__iterate_objects_without_actor(const CALifeSimulator* self, const luabind::functor<bool>& functor)
{
	const CALifeObjectRegistry& objects = self->objects();
	for (const auto& se_obj : objects.objects()) {
		if (se_obj.first != 0 && functor(se_obj.second)) break;
	}
}

// iterate alife objects on actor's current level filtered by script clsid;
// functor must not release iterated object (would invalidate iterator)
void CALifeSimulator__iterate_level_objects_of_clsid(
	const CALifeSimulator* self, int script_clsid_val, const luabind::functor<bool>& functor)
{
	if (!self->graph().level_exists()) return;
	const auto& level_reg = self->graph().level();
	for (const auto& entry : level_reg.objects()) {
		if (entry.second->m_script_clsid != script_clsid_val) continue;
		if (functor(entry.second)) break;
	}
}

struct alife_object_iterator {
	const CALifeObjectRegistry::OBJECT_REGISTRY* container;
	CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator it;

	alife_object_iterator(const CALifeObjectRegistry::OBJECT_REGISTRY* c)
		: container(c), it(c ? c->begin() : CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator()) {}

	CSE_ALifeDynamicObject* next() {
		if (!container || it == container->end())
			return nullptr;
		auto obj = it->second;
		++it;
		return obj;
	}
};

struct alife_object_without_actor_iterator {
	const CALifeObjectRegistry::OBJECT_REGISTRY* container;
	CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator it;

	alife_object_without_actor_iterator(const CALifeObjectRegistry::OBJECT_REGISTRY* c)
		: container(c), it(c ? c->begin() : CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator()) {}

	CSE_ALifeDynamicObject* next() {
		if (!container || it == container->end())
			return nullptr;
		if (it->first == 0) {
			++it;
			return next();
		}
		auto obj = it->second;
		++it;
		return obj;
	}
};

// demonized: iterate alife objects, for loop
alife_object_iterator alife_object_iter(const CALifeSimulator* self)
{
	VERIFY(self);
	const CALifeObjectRegistry& objects = self->objects();
	return alife_object_iterator(&objects.objects());
}
// demonized: iterate alife objects without actor, for loop
alife_object_without_actor_iterator alife_object_without_actor_iter(const CALifeSimulator* self)
{
	VERIFY(self);
	const CALifeObjectRegistry& objects = self->objects();
	return alife_object_without_actor_iterator(&objects.objects());
}

ALife::_OBJECT_ID alife_max_id(const CALifeSimulator* self)
{
	return self->objects().max_id;
}

ALife::_OBJECT_ID alife_object_count(const CALifeSimulator* self)
{
    return self->objects().objects().size();
}

::luabind::object alife_object_ids(const CALifeSimulator* self, const bool keytable = false, const bool withActor = false)
{
	VERIFY(self);

	::luabind::object result = ::luabind::newtable(ai().script_engine().lua());
	const CALifeObjectRegistry& objects = self->objects();
	int i = 1;
	for (const auto& [id, obj] : objects.objects())
	{
		if (!withActor && id == 0)
		{
			continue;
		}
		const int index = keytable ? id : i;
		result[index] = keytable ? true : id;
		i++;
	}
	return result;
}

::luabind::object alife_objects(const CALifeSimulator *self, const bool keytable = false, const bool withActor = false)
{
	VERIFY(self);
	
	::luabind::object result = ::luabind::newtable(ai().script_engine().lua());
	const CALifeObjectRegistry& objects = self->objects();
	int i = 1;
	for (const auto& [id, obj] : objects.objects())
	{
		if (!withActor && id == 0)
		{
			continue;
		}
		const int index = keytable ? id : i;
		result[index] = obj;
		i++;
	}
	return result;
}

xr_vector<u16>& get_children(const CALifeSimulator *self, CSE_Abstract *object)
{
	VERIFY(self);
	return object->children;
}

#pragma optimize("s",on)
void CALifeSimulator::script_register(lua_State* L)
{
	module(L)
	[
		class_<alife_object_iterator>("alife_object_iterator")
		.def("next", &alife_object_iterator::next)
		.def("__call", &alife_object_iterator::next), // for Lua for-loop

		class_<alife_object_without_actor_iterator>("alife_object_without_actor_iterator")
		.def("next", &alife_object_iterator::next)
		.def("__call", &alife_object_iterator::next), // for Lua for-loop

		class_<CALifeSimulator>("alife_simulator")
		.def("valid_object_id", &valid_object_id)
		.def("level_id", &get_level_id)
		.def("level_name", &get_level_name)
		.def("object", (CSE_ALifeDynamicObject *(*)(const CALifeSimulator*, ALife::_OBJECT_ID))(alife_object))
		.def("object", (CSE_ALifeDynamicObject *(*)(const CALifeSimulator*, ALife::_OBJECT_ID, bool))(alife_object))
		.def("story_object", (CSE_ALifeDynamicObject *(*)(const CALifeSimulator*, ALife::_STORY_ID))(alife_story_object))
		.def("set_switch_online", &CALifeSimulator__set_switch_online) // Anomaly Coop: gated wrapper
		.def("set_switch_offline", &CALifeSimulator__set_switch_offline) // Anomaly Coop: gated wrapper
		.def("set_interactive", &CALifeSimulator__set_interactive) // Anomaly Coop (F2 T12 round 1): gated wrapper
		.def("kill_entity", &kill_entity2) // Anomaly Coop: gated wrapper
		.def("kill_entity", &kill_entity0)
		.def("kill_entity", &kill_entity1)
		.def("add_in_restriction", &add_in_restriction)
		.def("add_out_restriction", &add_out_restriction)
		.def("remove_in_restriction", &remove_in_restriction)
		.def("remove_out_restriction", &remove_out_restriction)
		.def("remove_all_restrictions", &CALifeSimulator__remove_all_restrictions) // Anomaly Coop (F2 T12 round 1): gated wrapper
		.def("create", &CALifeSimulator__create)
		.def("create", &CALifeSimulator__spawn_item2)
		.def("create", &CALifeSimulator__spawn_item)
		.def("create", &CALifeSimulator__spawn_item3)
		.def("create_ammo", &CALifeSimulator__spawn_ammo)
		.def("release", &CALifeSimulator__release)
		.def("spawn_id", &CALifeSimulator__spawn_id)
		.def("actor", &get_actor)
		.def("has_info", &has_info)
		.def("dont_has_info", &dont_has_info)
		.def("give_info", &AlifeGiveInfo)
		.def("disable_info", &AlifeRemoveInfo)
		.def("switch_distance", &CALifeSimulator::switch_distance)
		.def("set_switch_distance", &CALifeSimulator::set_switch_distance)
		//Alundaio: renamed to set_switch_distance from switch_distance
		//Alundaio: extend alife simulator exports
		.def("teleport_object", &::teleport_object)
		.def("iterate_info", &IterateInfo)
		.def("clone_weapon", &try_to_clone_object)
		.def("register", &reprocess_spawn)
		.def("set_objects_per_update", &set_objects_per_update)
		.def("set_process_time", &set_process_time)
		.def("force_update", &force_update)
		.def("get_children", &get_children, return_stl_iterator)
		//Alundaio: END

		// demonized: iterate alife objects
        .def("object_count", &alife_object_count)
		.def("object_ids", &alife_object_ids)
		.def("objects", &alife_objects)
		.def("iterate_objects", &CALifeSimulator__iterate_objects)
		.def("iterate_level_objects_of_clsid", &CALifeSimulator__iterate_level_objects_of_clsid)
		/*.def("iterate_objects_without_actor", &CALifeSimulator__iterate_objects_without_actor)
		.def("objects_iter", &alife_object_iter)
		.def("objects_without_actor_iter", &alife_object_without_actor_iter)*/
		.def("max_id", &alife_max_id)

		, def("alife", &alife)
	];

	{
		if (story_ids.empty())
			generate_story_ids(
				story_ids,
				INVALID_STORY_ID,
				"story_ids",
				"INVALID_STORY_ID",
				"Invalid story id description (contains spaces)!",
				"INVALID_STORY_ID redifinition!",
				"Duplicated story id description!"
			);

		::luabind::class_<class_exporter<CALifeSimulator>> instance("story_ids");

		for (const auto& pair : story_ids)
			instance = std::move(instance).enum_("_story_ids")[::luabind::value(*pair.first, pair.second)];

		::luabind::module(L)[std::move(instance)];
	}

	{
		if (spawn_story_ids.empty())
			generate_story_ids(
				spawn_story_ids,
				INVALID_SPAWN_STORY_ID,
				"spawn_story_ids",
				"INVALID_SPAWN_STORY_ID",
				"Invalid spawn story id description (contains spaces)!",
				"INVALID_SPAWN_STORY_ID redifinition!",
				"Duplicated spawn story id description!"
			);

		::luabind::class_<class_exporter<class_exporter<CALifeSimulator>>> instance("spawn_story_ids");

		for (const auto& pair : spawn_story_ids)
			instance = std::move(instance).enum_("_spawn_story_ids")[::luabind::value(*pair.first, pair.second)];

		::luabind::module(L)[std::move(instance)];
	}
}

#if 0//def DEBUG
struct dummy {
    int count;
    lua_State* state;
    int ref;
};

void CALifeSimulator::validate			()
{
	typedef CALifeSpawnRegistry::SPAWN_GRAPH::const_vertex_iterator	const_vertex_iterator;
	const_vertex_iterator		I = spawns().spawns().vertices().begin();
	const_vertex_iterator		E = spawns().spawns().vertices().end();
	for ( ; I != E; ++I) {
		::luabind::wrap_base		*base = smart_cast<::luabind::wrap_base*>(&(*I).second->data()->object());
		if (!base)
			continue;

		if (!base->m_self.m_impl)
			continue;

		dummy					*_dummy = (dummy*)((void*)base->m_self.m_impl);
		lua_State				**_state = &_dummy->state;
		VERIFY2					(
			base->m_self.state(),
			make_string(
				"0x%08x name[%s] name_replace[%s]",
				*(int*)&_state,
				(*I).second->data()->object().name(),
				(*I).second->data()->object().name_replace()
			)
		);
	}
}
#endif //DEBUG
