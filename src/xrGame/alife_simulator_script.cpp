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
#include "coop/coop_items.h"
#include "coop/coop_callgate.h" // Anomaly Coop
#include "coop/coop_story.h" // Anomaly Coop: alife():give_info of our actor
#include "coop/coop_manager.h" // Anomaly Coop
#include "xrCoop/coop_mode.h" // Anomaly Coop: an object online elsewhere
#include "coop/coop_levels.h" // Anomaly Coop: the map of a create's new root
#include "coop/coop_router.h" // Anomaly Coop: the writer gate of a script write
#include "Actor.h" // Anomaly Coop: g_actor
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

// Anomaly Coop: the call gate of a spawning alife() call on a client in the host's world (Run elsewhere).
// own: calls::own_spawn - the new object's parent is our actor or under it (ReqSpawn only creates items in our own
// inventory); a temporary object (create/clone_weapon with register = false) passes as own, its alife():register is
// gated. A refusal returns nil, as the request path does. position (may be null) must be finite; section names the log
// line. While a host command is applied the class rules still hold (never the stock create).
static coop::calls::Gate coop_spawn_gate(const char* name, bool own, const Fvector* position, LPCSTR section)
{
	if (!coop::requests::lua_mirror())
		return coop::calls::Gate::Run;
	coop::callgate::Args args;
	args.pos = position;
	args.what = section;
	args.runs_while_applying = false;
	return coop::callgate::client_gate_own(name, own, args);
}

// Anomaly Coop: alife():give_info / disable_info write the info registry directly,
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

// Anomaly Coop: the writer gate of a script write (coop_router.h writer_refuses), after the call gate above. On the world server and a
// map server it counts what it would refuse, ask or order (coop_writer_gate log, the census in coop_status) and refuses under enforce;
// the co-op host, a player's machine and single player never.
// A release or an info: the object it changes.
static bool coop_writer_refuses(const char* call, coop::writer::WriteOp op, ALife::_OBJECT_ID id, LPCSTR what)
{
	coop::calls::WriteTarget target;
	target.id = id;
	target.what = what;
	return coop::router::writer_refuses(call, op, target);
}

// A create: under its parent, else a new root of `kind` standing on the map of `game_vertex`.
static bool coop_writer_refuses(const char* call, coop::writer::WriteOp op, ALife::_OBJECT_ID parent, LPCSTR section,
                                coop::calls::Target kind, GameGraph::_GRAPH_ID game_vertex)
{
	coop::calls::WriteTarget target;
	target.what = section;
	if (parent != ALife::_OBJECT_ID(-1))
		target.id = parent;
	else
	{
		target.kind = kind;
		target.level = coop::levels::level_of_vertex(game_vertex);
	}
	return coop::router::writer_refuses(call, op, target);
}

// The kind of a new root that is an object already (a spawn-graph object, a temporary one being registered): a unit as the writer rule
// names one (writer::is_unit: a squad or a living monster), anything else a resident of its map.
static coop::calls::Target coop_new_root_kind(CSE_Abstract* object)
{
	const CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(object);
	const bool unit = coop::writer::is_unit(smart_cast<CSE_ALifeOnlineOfflineGroup*>(object) != nullptr,
	    smart_cast<CSE_ALifeMonsterAbstract*>(object) != nullptr, creature != nullptr && creature->g_Alive());
	return unit ? coop::calls::Target::NewUnit : coop::calls::Target::NewResident;
}

// The game vertex of an object (0xffff: none, its map unknown).
static GameGraph::_GRAPH_ID coop_vertex_of(CSE_Abstract* object)
{
	CSE_ALifeObject* o = smart_cast<CSE_ALifeObject*>(object);
	return o ? o->m_tGraphID : GameGraph::_GRAPH_ID(-1);
}

// Anomaly Coop: what a script create asks after its call gate: the writer of the new object's root (under `parent`, else a new root of
// `kind` on the map of `game_vertex`), then a free id of this process's own pool (xrServer::CoopMayCreate). True: refused, the create
// returns nil.
static bool coop_create_refused(CALifeSimulator* self, const char* call, ALife::_OBJECT_ID parent, LPCSTR section,
                                coop::calls::Target kind, GameGraph::_GRAPH_ID game_vertex)
{
	return coop_writer_refuses(call, coop::writer::WriteOp::Create, parent, section, kind, game_vertex) ||
	       !self->server().CoopMayCreate(call);
}

// Anomaly Coop: every gate of a script create, in order: the call gate (coop_spawn_gate), then coop_create_refused. Run: the stock
// create runs; Request: the call site's request path; Refuse: nil.
static coop::calls::Gate coop_create_gate(CALifeSimulator* self, const char* call, bool own, const Fvector& position, LPCSTR section,
                                          ALife::_OBJECT_ID parent, coop::calls::Target kind, GameGraph::_GRAPH_ID game_vertex)
{
	const coop::calls::Gate gate = coop_spawn_gate(call, own, &position, section);
	if (gate != coop::calls::Gate::Run)
		return gate;
	return coop_create_refused(self, call, parent, section, kind, game_vertex) ? coop::calls::Gate::Refuse : coop::calls::Gate::Run;
}

// Anomaly Coop: a temporary object that will not be registered: its id goes back to the generator and the object is destroyed.
static void coop_discard(CALifeSimulator* self, CSE_Abstract* object)
{
	self->server().FreeID(object->ID, 0);
	F_entity_Destroy(object);
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
	if (coop::requests::lua_client_refuse("alife:kill_entity", monster ? monster->ID : ALife::_OBJECT_ID(-1))) return; // Anomaly Coop: deaths are the host's
	alife->kill_entity(monster, game_vertex_id, 0);
}

void kill_entity1(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster)
{
	if (coop::requests::lua_client_refuse("alife:kill_entity", monster ? monster->ID : ALife::_OBJECT_ID(-1))) return; // Anomaly Coop
	alife->kill_entity(monster, monster->m_tGraphID, 0);
}

// Anomaly Coop: wrappers so a coop client can refuse these (bound as members before)
void kill_entity2(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, const GameGraph::_GRAPH_ID& game_vertex_id, CSE_ALifeSchedulable* schedulable)
{
	if (coop::requests::lua_client_refuse("alife:kill_entity", monster ? monster->ID : ALife::_OBJECT_ID(-1))) return;
	alife->kill_entity(monster, game_vertex_id, schedulable);
}

void CALifeSimulator__set_switch_online(CALifeSimulator* self, ALife::_OBJECT_ID id, bool value)
{
	if (coop::requests::lua_client_refuse("alife:set_switch_online", id)) return; // the host switches objects
	self->set_switch_online(id, value);
}

void CALifeSimulator__set_switch_offline(CALifeSimulator* self, ALife::_OBJECT_ID id, bool value)
{
	if (coop::requests::lua_client_refuse("alife:set_switch_offline", id)) return;
	self->set_switch_offline(id, value);
}

// Anomaly Coop: restrictions and set_interactive change the host's ALife (a
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

// Anomaly Coop: wrappers so a coop client can refuse these (bound as members before)
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
	if (coop::requests::lua_mirror() && coop::callgate::client_gate_own("alife:create", false) != coop::calls::Gate::Run) return nullptr; // Anomaly Coop: a spawn-graph object is a world spawn
	const CALifeSpawnRegistry::SPAWN_GRAPH::CVertex* vertex = ai().alife().spawns().spawns().vertex(spawn_id);
	THROW2(vertex, "Invalid spawn id!");

	CSE_ALifeDynamicObject* spawn = smart_cast<CSE_ALifeDynamicObject*>(&vertex->data()->object());
	THROW(spawn);

	// Anomaly Coop: the writer of the spawn-graph object's new root; an id of this process's own pool
	if (coop_create_refused(self, "alife:create", spawn->ID_Parent, spawn->s_name.c_str(), coop_new_root_kind(spawn), spawn->m_tGraphID))
		return nullptr;
	CSE_ALifeDynamicObject* object;
	self->create(object, spawn, spawn_id);

	return (object);
}

CSE_Abstract* CALifeSimulator__spawn_item(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id)
{
	// coop: An unparented spawn is a world write, except for personal map zones created on the client alone.
	switch (coop_spawn_gate("alife:create", coop::requests::local_zone(section), &position, section))
	{
	case coop::calls::Gate::Request: return coop::requests::lua_create(section, position, level_vertex_id, game_vertex_id, ALife::_OBJECT_ID(-1), true);
	case coop::calls::Gate::Refuse: return nullptr;
	case coop::calls::Gate::Run: break;
	}
	THROW(self);
	// coop: Check the writer and ID pool for a new root on the vertex's map.
	if (coop_create_refused(self, "alife:create", ALife::_OBJECT_ID(-1), section, coop::calls::Target::NewOfSection, game_vertex_id))
		return nullptr;
	return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, ALife::_OBJECT_ID(-1)));
}

CSE_Abstract* CALifeSimulator__spawn_item2(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent)
{
	// Anomaly Coop: player items via the host; the writer of the parent's root (no parent: a new root on the map of the vertex)
	switch (coop_create_gate(self, "alife:create", coop::calls::own_spawn(true, coop::callgate::is_own(id_parent)), position, section,
	            id_parent, coop::calls::Target::NewOfSection, game_vertex_id))
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

	if (!object->m_bOnline || coop::mode().online_elsewhere(object->ID)) // Anomaly Coop: no game object here, the client who holds it gets it as any spawn
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
	if (reg == true)
		return CALifeSimulator__spawn_item2(self, section, position, level_vertex_id, game_vertex_id, id_parent); // Anomaly Coop: gated there

	// Anomaly Coop: no id until the host creates it; a temporary object takes its id now (its alife():register is asked again)
	switch (coop_create_gate(self, "alife:create", coop::calls::own_spawn(false, coop::callgate::is_own(id_parent)), position, section,
	            id_parent, coop::calls::Target::NewOfSection, game_vertex_id))
	{
	case coop::calls::Gate::Request: return coop::requests::lua_create(section, position, level_vertex_id, game_vertex_id, id_parent, false);
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

	if (!object->m_bOnline || coop::mode().online_elsewhere(object->ID)) // Anomaly Coop
		return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent));

	CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);

	return (item);
}

CSE_Abstract* CALifeSimulator__spawn_ammo(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, int ammo_to_spawn)
{
	// Anomaly Coop: the writer of the parent's root (no parent: a box of ammo on the map of the vertex)
	switch (coop_create_gate(self, "alife:create_ammo", coop::calls::own_spawn(true, coop::callgate::is_own(id_parent)), position, section,
	            id_parent, coop::calls::Target::NewResident, game_vertex_id))
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

	if (!object || !object->m_bOnline || coop::mode().online_elsewhere(object->ID)) // Anomaly Coop
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
	if (coop::requests::lua_mirror() && coop::requests::lua_release(object)) return; // Anomaly Coop: the host releases (gate inside)
	if (coop::requests::cancel_unregistered_clone(object))
	{
		coop_discard(self, object);
		return;
	}
	VERIFY(self);
	//	self->release						(object,true);

	THROW(object);
	if (!object) return;
	CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(object);
	THROW(alife_object);
	if (!alife_object) return;
	if (coop_writer_refuses("alife:release", coop::writer::WriteOp::Release, object->ID, object->s_name.c_str())) return; // Anomaly Coop
	if (coop::requests::held_by_a_client("alife:release", object->ID)) return; // Anomaly Coop
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
	if (coop_writer_refuses("alife:give_info", coop::writer::WriteOp::Info, id, info_id)) return; // Anomaly Coop: the ghost's too
	if (coop_alife_info("alife:give_info", id, info_id, true)) return; // Anomaly Coop
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
	if (coop_writer_refuses("alife:disable_info", coop::writer::WriteOp::Info, id, info_id)) return; // Anomaly Coop: the ghost's too
	if (coop_alife_info("alife:disable_info", id, info_id, false)) return; // Anomaly Coop
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
	if (coop::requests::lua_client_refuse("alife:teleport_object", id)) return; // Anomaly Coop: the host moves objects
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
	// Anomaly Coop: alife():register(nil) is the stock "nothing" back. alife():create(..., false) returns nil when the writer gate or an
	// empty id pool refuses it (coop_writer_gate enforce), and a script that hands that straight to register would otherwise have its
	// id read below: an access violation no pcall catches.
	if (!object)
		return nullptr;
	switch (coop_spawn_gate("alife:register", coop::callgate::is_own(object->ID_Parent), nullptr, object->s_name.c_str())) // Anomaly Coop
	{
	case coop::calls::Gate::Request: return coop::requests::lua_register_spawn(object);
	case coop::calls::Gate::Refuse: return nullptr;
	case coop::calls::Gate::Run: break;
	}
	// Anomaly Coop: an object the ALife holds already is registered. alife():create(..., false) hands a registered one back when
	// its parent has no game object in this process (CALifeSimulator__spawn_item3 above: an offline parent, or one online at a
	// map server), and the stock scripts call register on it all the same (dialogs_mlr.give_task_fanat_training_2: the
	// artefact for Fanatic's stash). Written to a packet and destroyed below while the ALife's registries still hold it, the
	// next walk over them would call a method of the dead object ("pure virtual function call"); a single-player game has the
	// same fault with an offline parent. There is nothing left to do for such an object.
	if (static_cast<CSE_Abstract*>(ai().alife().objects().object(object->ID, true)) == object)
		return object;
	const u16 clone_id = object->ID;
	// Anomaly Coop: the writer of the parent's root (no parent: a new root of the object's kind); its id was taken by create(..., false),
	// which asked for it. A refusal ends the temporary object as a failed register does.
	if (coop_writer_refuses("alife:register", coop::writer::WriteOp::Create, object->ID_Parent, object->s_name.c_str(),
	        coop_new_root_kind(object), coop_vertex_of(object)) ||
	    !coop::requests::before_clone_register(object))
	{
		coop::requests::after_clone_register(clone_id, false);
		coop_discard(self, object);
		return nullptr;
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

	CSE_Abstract* result = self->server().Process_spawn(packet, clientID);
	if (!coop::requests::after_clone_register(clone_id, result != nullptr) && result)
	{
		self->release(result, true);
		return nullptr;
	}
	return result;
}

CSE_Abstract* try_to_clone_object(CALifeSimulator* self, CSE_Abstract* object, LPCSTR section, const Fvector& position,
                                  u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent,
                                  bool bRegister = true)
{
	// Anomaly Coop: the writer of the parent's root (no parent: a weapon on the map of the vertex)
	switch (coop_create_gate(self, "alife:clone_weapon", coop::calls::own_spawn(bRegister, coop::callgate::is_own(id_parent)), position,
	            section, id_parent, coop::calls::Target::NewResident, game_vertex_id))
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
		{
			coop_discard(self, absClone);
			return nullptr;
		}

		clone->wpn_flags = wpnmag->wpn_flags;
		clone->m_addon_flags = wpnmag->m_addon_flags;
		clone->m_fCondition = wpnmag->m_fCondition;
		clone->ammo_type = wpnmag->ammo_type;
		clone->m_upgrades = wpnmag->m_upgrades;
		clone->set_ammo_elapsed(wpnmag->get_ammo_elapsed());
		if (!coop::requests::prepare_local_clone(object, absClone))
		{
			coop_discard(self, absClone);
			return nullptr;
		}

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
		.def("set_interactive", &CALifeSimulator__set_interactive) // Anomaly Coop: gated wrapper
		.def("kill_entity", &kill_entity2) // Anomaly Coop: gated wrapper
		.def("kill_entity", &kill_entity0)
		.def("kill_entity", &kill_entity1)
		.def("add_in_restriction", &add_in_restriction)
		.def("add_out_restriction", &add_out_restriction)
		.def("remove_in_restriction", &remove_in_restriction)
		.def("remove_out_restriction", &remove_out_restriction)
		.def("remove_all_restrictions", &CALifeSimulator__remove_all_restrictions) // Anomaly Coop: gated wrapper
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
