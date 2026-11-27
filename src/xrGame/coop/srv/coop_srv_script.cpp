// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the Lua table srv.* (docs/MODES.md section 4 "Lua"): the
// predicates of the server's mode, for the server's layer (scripts\coop_srv_mode.script) and the server's scripts:
//   srv.is_dedicated()          this process was started with -coop_dedicated: the world server, from its start
//   srv.dedicated_new_world()   that world server makes a new world
//   srv.is_map_server()         this process is a map server (-coop_mapserver), in its session
//   srv.is_world_server()       this process is the world server (-coop_dedicated) and decides the world of its session; a co-op host
//                               decides its world too (its roles hold WorldServer) and is answered false
//   srv.split()                 the session is the server's: its players stand on different maps
//   srv.kill_record(id, killer) the world server kills a record whose body stands nowhere (l_kill_record below)
//   srv.journal_done(origin, seq, answer)  a map server records a numbered op's answer with its effect (l_journal_done below)
// They answer as their copies in coop.* (coop.is_dedicated, coop.dedicated_new_world, coop.is_map_server, coop.is_world_server,
// coop.levels.split: coop_levels.cpp), which go once no script reads them; is_world_server is the copy's
// answer in a -coop_dedicated process only. In a co-op process and in single player the table is there too and answers false: the
// server's layer asks it in every process.
// The predicates read no argument: an addon that passes one gets the same answer. Plain C functions, as in coop_levels.cpp: no luabind
// overload errors, which are fatal in this engine.
// Made every time the script engine makes its Lua state, with the Core's coop.* (coop_script.cpp), by the server's layer
// (srv/coop_srv_mode.cpp ServerLayer::register_lua, through the switchboard: coop_mode.cpp register_layers_lua).
#include "stdafx.h"
#include "pch_script.h"
#include "coop/coop_manager.h"
#include "coop/srv/coop_dedicated.h"
#include "coop/srv/coop_levels_alife.h" // levels::journal_done
#include "coop/srv/coop_srv_layer.h"
#include "coop/coop_levels.h"
#include "coop/coop_events.h"
#include "coop/coop_world.h"
#include "coop/coop_lua.h" // lua_api: the registrar, the argument readers
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <vector>

namespace coop
{
namespace srv
{
namespace
{
int l_is_dedicated(lua_State* L)
{
	lua_pushboolean(L, dedicated::on());
	return 1;
}
int l_dedicated_new_world(lua_State* L)
{
	lua_pushboolean(L, dedicated::new_world());
	return 1;
}
int l_is_map_server(lua_State* L)
{
	lua_pushboolean(L, is_map_server());
	return 1;
}
int l_is_world_server(lua_State* L)
{
	lua_pushboolean(L, dedicated::on() && is_world_server());
	return 1;
}
int l_split(lua_State* L)
{
	lua_pushboolean(L, levels::split());
	return 1;
}

// srv.kill_record(id, killer) -> bool. The world server kills a record whose body stands nowhere
// (scripts\coop_srv_quests_far.script: an order "kill" carried out on the record, its map server gone or silent) the way it applies a
// death a map server reports (coop_levels_alife.cpp, the journal's Died entry): levels::host_apply_death (health 0, alife().on_death,
// the tasks' relay), then events::host_on_death, so the players and the map servers hear of it. Never CALifeCombatManager::kill_entity:
// it detaches the inventory, which the stock does not do for a stalker or a monster, and its R_ASSERT(children.empty()) stopped the
// world server on a corpse with the quest item inside. The body keeps what it holds. killer: an id, anything else is nobody (0xffff).
// true: the record was a living creature and is dead now. Only the world server kills: in a co-op process, in single player, on a map
// server and on a player's machine nothing changes and the answer is false. A player's character dies on his own map, never here.
CSE_ALifeCreatureAbstract* living_record(u16 id)
{
	if (id == 0xffff || !world_up())
		return nullptr;
	CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(ai().alife().objects().object(id, true));
	return c && c->g_Alive() && !world::is_player_character(c) ? c : nullptr;
}

int l_kill_record(lua_State* L)
{
	u16 victim = 0xffff, killer = 0xffff; // anything but an object id is nobody
	lua_api::arg_object_id(L, 1, victim);
	lua_api::arg_object_id(L, 2, killer);
	bool dead = false;
	if (dedicated::on() && is_world_server() && living_record(victim))
	{
		levels::host_apply_death(victim, killer, std::vector<u8>());
		events::host_on_death(victim, killer);
		dead = !living_record(victim);
	}
	lua_pushboolean(L, dead);
	return 1;
}

// srv.journal_done(origin, seq, answer) -> bool. A map server's numbered op that consumes or pays
// (scripts\coop_ops_done.script record -> coop_mode.op_done -> coop_srv_mode.script op_done) ran here with `answer` (coop_main.encode of
// the executor's answer): a Done entry of its level journal, after the facts of the op's effect, in the same batch (levels::journal_done,
// srv/coop_levels_xfer.cpp). The world keeps it for the level and hands it to the level's next map server. origin: the character's world
// id; seq: the op's sequence (from 1). true: queued. false, nothing recorded, and the ledger keeps the answer in this process only: on any
// other process (the world server's ledger is its own save; a co-op process and a player's machine have no journal), for arguments that
// are no op's number or no string, an answer over journal::done::kMaxAnswer bytes, and when the journal does not take it (its holder of
// this world not made yet, the hand-over over).
int l_journal_done(lua_State* L)
{
	u32 origin = 0, seq = 0;
	size_t len = 0;
	const char* answer = lua_type(L, 3) == LUA_TSTRING ? lua_tolstring(L, 3, &len) : nullptr;
	const bool queued = answer && lua_api::arg_whole_u32(L, 1, double(0xffffffffu), origin) &&
	    lua_api::arg_whole_u32(L, 2, double(0xffffffffu), seq) && is_map_server() &&
	    levels::journal_done(origin, seq, std::string(answer, len));
	lua_pushboolean(L, queued);
	return 1;
}
} // namespace

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"is_dedicated", &l_is_dedicated},
		{"dedicated_new_world", &l_dedicated_new_world},
		{"is_map_server", &l_is_map_server},
		{"is_world_server", &l_is_world_server},
		{"split", &l_split},
		{"kill_record", &l_kill_record},
		{"journal_done", &l_journal_done},
		{nullptr, nullptr},
	};
	lua_api::fill_global(L, "srv", fns);
}
} // namespace srv
} // namespace coop
