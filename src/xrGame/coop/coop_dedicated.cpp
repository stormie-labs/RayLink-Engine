#include "stdafx.h"
#include "pch_script.h"
#include "coop_dedicated.h"
#include "coop_autosave.h"
#include "coop_levels.h"
#include "coop_manager.h"
#include "coop_mapserver.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/coop_host_console.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "date_time.h"
#include "../../xrEngine/Environment.h"
#include "game_graph.h"
#include "game_sv_single.h"
#include "Level.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife.h"
#include "xrCoop/coop_world_server_rules.h"
#include "xrCoop/coop_operator.h"
#include "xrCoop/coop_world_stage.h"
#include "coop_net.h"
#include "script_engine.h"

#include <cstdio>

#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// See coop_dedicated.h.

namespace coop
{
namespace debug
{
// coop_debug.cpp: coop_dbg of the operator, as his peer's request (run_for is its one caller).
void operator_dbg(PeerId from, const std::string& rest);
} // namespace debug

namespace dedicated
{
namespace
{
constexpr u32 kTitleEveryMs = 2000;

struct State
{
	bool parsed = false;
	Args args;
	bool world_asked = false;
	bool new_world = false;
	bool quitting = false;
	bool quit_sent = false;
	u64 title_at = 0;
	bool usage_said = false;
	// plan 2026-11-12 stage 2: what the parent of a -coop_hidden server was last told
	u64 counts_at = 0;
	u32 players_said = u32(-1);
	std::map<u16, world_server::MapState> mapservers_said;
} g;

// An @rl line for the parent (coop_world_server_rules.h); nothing without -coop_hidden and a redirected output.
void rl(const world_server::Record& r) { coop_console::rl_line(world_server::format(r).c_str()); }

// The answer to the operator's command that runs now (run_for): its lines, and whether a command runs. A vector of this file, not a
// pointer to one of run_for's own: xrGame is built without unwinding, a command that throws would leave a pointer to a dead frame
// behind, and the next line anybody says would be written there (security review 2026-11-12).
std::vector<std::string> g_answer;
bool g_answering = false;

// An answer to the operator: in the log, and in the window whatever its log level.
void say(const char* line)
{
	::Msg("%s", line);
	if (coop_console::log_level() < 1)
		coop_console::print(line);
	answer_line(line);
}

// A line of the log that is an answer too (a refusal of a command): the log as before, and the operator of run_for hears it.
void tell(const char* line)
{
	::Msg("%s", line);
	answer_line(line);
}
void tell(const std::string& line) { tell(line.c_str()); }

void list_saves(std::vector<SaveFile>& out)
{
	string_path mask;
	FS.update_path(mask, "$game_saves$", "*" SAVE_EXTENSION);
	WIN32_FIND_DATAA found;
	const HANDLE h = FindFirstFileA(mask, &found);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		std::string name = found.cFileName;
		name.resize(name.size() - xr_strlen(SAVE_EXTENSION));
		SaveFile f;
		f.name = name;
		f.time = (u64(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
		out.push_back(f);
	} while (FindNextFileA(h, &found));
	FindClose(h);
}

bool save_exists(const std::string& name)
{
	string_path file;
	FS.update_path(file, "$game_saves$", (name + SAVE_EXTENSION).c_str());
	return GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES;
}

bool load_allowed(const std::string& save, std::string& why); // below: asks coop_dedicated.load_allowed

void start_world(World world, std::string save)
{
	if (world == World::Load)
	{
		if (save == kLastSave)
		{
			std::vector<SaveFile> files;
			list_saves(files);
			save = newest_save(files);
			if (save.empty())
			{
				::Msg("! [coop] dedicated: no saves in this folder, nothing to load: coop_world new");
				rl(world_server::Record::of_error("no_save", "no saves in this folder, nothing to load"));
				return;
			}
		}
		if (!save_name_ok(save) || !save_exists(save))
		{
			::Msg("! [coop] dedicated: no save '%s': coop_world list", save.c_str());
			rl(world_server::Record::of_error("no_save", "no save '" + save + "'"));
			return;
		}
		// owner 2026-11-13 (the old split mode stays): never the world of a host who plays in it
		std::string why;
		if (!load_allowed(save, why))
		{
			::Msg("! [coop] dedicated: %s", why.c_str());
			rl(world_server::Record::of_error("bad_world", why));
			return;
		}
	}
	g.world_asked = true;
	g.new_world = world == World::New;
	::Msg("[coop] dedicated: %s", world == World::New ? "a new world" : (std::string("the world of the save ") + save).c_str());
	rl(world_server::Record::of_phase(world_server::Phase::World));
	Console->Execute(start_line(world, save).c_str());
}

// ---------------------------------------------------------------- plan 2026-11-12 stage 5: a new world from coop_new_world.ltx (-coop_stage)

// debug.traceback of an error in coop_dedicated.stage_new_world.
int stage_traceback(lua_State* L)
{
	const char* msg = lua_tostring(L, 1);
	if (!msg)
		msg = "(error object is not a string)";
	lua_getglobal(L, "debug");
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_pushstring(L, msg);
		return 1;
	}
	lua_getfield(L, -1, "traceback");
	if (!lua_isfunction(L, -1))
	{
		lua_pop(L, 2);
		lua_pushstring(L, msg);
		return 1;
	}
	lua_pushstring(L, msg);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

// coop_dedicated.stage_new_world, or nil. Protected (call_stage): the first lookup loads the script through _G.__index, which may raise.
int stage_lookup(lua_State* L)
{
	lua_getglobal(L, "coop_dedicated");
	if (!lua_istable(L, -1))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_getfield(L, -1, "stage_new_world");
	return 1;
}

void stage_text(lua_State* L, const char* field, const std::string& v)
{
	lua_pushlstring(L, v.data(), v.size());
	lua_setfield(L, -2, field);
}

void stage_flag(lua_State* L, const char* field, world_stage::Flag f)
{
	if (f == world_stage::Flag::Absent)
		return; // nil: the game's default
	lua_pushboolean(L, f == world_stage::Flag::On ? 1 : 0);
	lua_setfield(L, -2, field);
}

// The parsed file as the table coop_dedicated.stage_new_world takes (its comment): one parser (coop_world_stage.h), not a second one in Lua.
void push_stage(lua_State* L, const world_stage::Stage& s)
{
	lua_createtable(L, 0, 3);
	lua_createtable(L, 0, 8);
	stage_text(L, "id", s.world.id);
	stage_text(L, "name", s.world.name);
	stage_text(L, "title", s.world.title);
	stage_text(L, "faction", s.world.faction);
	stage_flag(L, "story", s.world.story);
	stage_flag(L, "routes", s.world.routes);
	if (s.world.difficulty > 0)
	{
		lua_pushnumber(L, lua_Number(s.world.difficulty));
		lua_setfield(L, -2, "difficulty");
	}
	if (s.world.economy > 0)
	{
		lua_pushnumber(L, lua_Number(s.world.economy));
		lua_setfield(L, -2, "economy");
	}
	lua_setfield(L, -2, "world");
	lua_createtable(L, 0, int(s.values.size()));
	for (const auto& kv : s.values)
	{
		lua_pushlstring(L, kv.first.data(), kv.first.size());
		lua_pushlstring(L, kv.second.data(), kv.second.size());
		lua_settable(L, -3);
	}
	lua_setfield(L, -2, "values");
	if (s.creator.present)
	{
		lua_createtable(L, 0, 6);
		stage_text(L, "name", s.creator.name);
		stage_text(L, "icon", s.creator.icon);
		stage_text(L, "map", s.creator.map);
		lua_createtable(L, int(s.creator.loadout.size()), 0);
		for (size_t i = 0; i < s.creator.loadout.size(); ++i)
		{
			lua_pushlstring(L, s.creator.loadout[i].data(), s.creator.loadout[i].size());
			lua_rawseti(L, -2, int(i + 1));
		}
		lua_setfield(L, -2, "loadout");
		lua_pushnumber(L, lua_Number(s.creator.money));
		lua_setfield(L, -2, "money");
		lua_pushboolean(L, s.creator.bare ? 1 : 0);
		lua_setfield(L, -2, "bare");
		lua_setfield(L, -2, "creator");
	}
}

// coop_dedicated.stage_new_world(path, stage) in a protected call. False with `why`.
bool call_stage(const std::string& path, const world_stage::Stage& s, std::string& why)
{
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 24))
	{
		why = "no Lua state";
		return false;
	}
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &stage_traceback);
	const int errfunc = lua_gettop(L);
	lua_pushcfunction(L, &stage_lookup);
	if (lua_pcall(L, 0, 1, errfunc) != 0 || !lua_isfunction(L, -1))
	{
		const char* err = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
		why = std::string("coop_dedicated.stage_new_world is missing") + (err ? std::string(": ") + err : std::string());
		lua_settop(L, top);
		return false;
	}
	lua_pushlstring(L, path.data(), path.size());
	push_stage(L, s);
	if (lua_pcall(L, 2, 2, errfunc) != 0)
	{
		const char* err = lua_tostring(L, -1);
		why = std::string("lua error in coop_dedicated.stage_new_world: ") + (err ? err : "?");
		lua_settop(L, top);
		return false;
	}
	const bool ok = lua_toboolean(L, -2) != 0;
	if (!ok)
		why = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "refused by coop_dedicated.stage_new_world";
	lua_settop(L, top);
	return ok;
}

// coop_dedicated.load_allowed, or nil; protected as stage_lookup.
int load_lookup(lua_State* L)
{
	lua_getglobal(L, "coop_dedicated");
	if (!lua_istable(L, -1))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_getfield(L, -1, "load_allowed");
	return 1;
}

// May this world server load the save? coop_dedicated.load_allowed(save) in a protected call: false with `why` only when the script
// says no (the save is a world of a host who plays in it: its actor is his character, and this process would make it its ghost). A
// missing script or a Lua error lets the save through with a line in the log: the guard is not a gate the server depends on.
bool load_allowed(const std::string& save, std::string& why)
{
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 16))
		return true;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &stage_traceback);
	const int errfunc = lua_gettop(L);
	lua_pushcfunction(L, &load_lookup);
	if (lua_pcall(L, 0, 1, errfunc) != 0 || !lua_isfunction(L, -1))
	{
		::Msg("! [coop] dedicated: coop_dedicated.load_allowed is missing: the save '%s' is not checked", save.c_str());
		lua_settop(L, top);
		return true;
	}
	lua_pushlstring(L, save.data(), save.size());
	if (lua_pcall(L, 1, 2, errfunc) != 0)
	{
		const char* err = lua_tostring(L, -1);
		::Msg("! [coop] dedicated: lua error in coop_dedicated.load_allowed, the save '%s' is not checked: %s", save.c_str(), err ? err : "?");
		lua_settop(L, top);
		return true;
	}
	const bool ok = lua_toboolean(L, -2) != 0;
	if (!ok)
		why = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "refused by coop_dedicated.load_allowed";
	lua_settop(L, top);
	return ok;
}

// The whole file, or false. Reading stops past the limit: parse refuses the size.
bool read_stage_file(const std::string& path, std::string& text, std::string& why)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
	{
		why = "it cannot be opened";
		return false;
	}
	char buf[4096];
	size_t n = 0;
	while (text.size() <= world_stage::kMaxFileBytes && (n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	const bool failed = std::ferror(f) != 0;
	std::fclose(f);
	if (failed)
	{
		why = "it cannot be read";
		return false;
	}
	return true;
}

// The record of the new world from the staging file, staged with the engine (coop.game.stage_new) before the world starts; the file becomes
// <file>.used so that a restart cannot make a second world of it. False: refused with the reason in the log and "@rl error bad_args", and
// no world starts (never the default world instead).
bool stage_world(const std::string& path)
{
	std::string text, why;
	world_stage::Stage stage;
	if (!read_stage_file(path, text, why) || !world_stage::parse(text, stage, why) || !call_stage(path, stage, why))
	{
		const std::string line = "the staging file " + path + ": " + why;
		::Msg("! [coop] dedicated: %s; no world starts (coop_world new starts the default world)", line.c_str());
		rl(world_server::Record::of_error("bad_args", line));
		return false;
	}
	const std::string used = path + world_stage::kUsedSuffix;
	if (MoveFileExA(path.c_str(), used.c_str(), MOVEFILE_REPLACE_EXISTING))
		::Msg("[coop] dedicated: the new world is staged from %s (faction %s, %u value(s), %s); the file is now %s", path.c_str(),
		    stage.world.faction.c_str(), u32(stage.values.size()), stage.creator.present ? "a creator" : "no creator", used.c_str());
	else
		::Msg("! [coop] dedicated: the new world is staged, but %s could not be renamed (error %u): a restart would stage it again", path.c_str(),
		    u32(GetLastError()));
	return true;
}

// The first frame: what the keys ask for. A wrong key is said in the window, which stays open for the operator.
void first_frame()
{
	std::string why;
	if (!parse_args(Core.Params, g.args, why))
	{
		::Msg("! [coop] dedicated: %s", why.c_str());
		rl(world_server::Record::of_error("bad_args", why));
	}
	::Msg("[coop] dedicated host, port %u, the build '%s'", u32(g.args.port), Manager::build_id());
	// stage 5: Core.Params is lower-cased; the path keeps the letters of the raw command line where it can
	if (g.args.world == World::New && !g.args.stage.empty())
	{
		std::string stage = stage_original_case(Core.Params, GetCommandLineA(), g.args.stage);
		if (stage_is_name(stage))
		{
			// a file of this server's own data folder: its parent wrote it there and named it without a path (stage_is_name)
			string_path full;
			FS.update_path(full, "$app_data_root$", stage.c_str());
			stage = full;
		}
		if (!stage_world(stage))
			return;
	}
	if (g.args.world != World::None)
		start_world(g.args.world, g.args.save);
	else
		::Msg("[coop] no world yet. coop_world new | coop_world last | coop_world <save> | coop_world list");
}

void run_line(const char* text)
{
	const Parsed line = parse_line(text);
	switch (line.kind)
	{
	case Line::Empty: break;
	case Line::Quit: g.quitting = true; break;
	case Line::Chat: mgr().send_chat(line.text.c_str()); break;
	case Line::Unknown: ::Msg("! [coop] '%s' is no command: help lists them", line.text.c_str()); break;
	case Line::Command: Console->Execute(line.text.c_str()); break;
	}
}

void update_title()
{
	const u64 now = mgr().now_ms();
	if (now < g.title_at)
		return;
	g.title_at = now + kTitleEveryMs;
	string256 title;
	if (mgr().active())
		xr_sprintf(title, "RayLink: dedicated host, port %u, %u player(s), %u map server(s)", u32(mgr().session().listen_port()),
		    u32(mgr().session().peers().size()), mapserver::host_running());
	else
		xr_sprintf(title, "RayLink: dedicated host, port %u, no session", u32(g.args.port));
	coop_console::set_title(title);
}

// The session goes first: its end writes the world's last save (Manager::stop, autosave::host_on_session_end) while the level still runs.
void quit_now()
{
	rl(world_server::Record::of_phase(world_server::Phase::Saving));
	::Msg("[coop] dedicated: the world is saved, the host goes down");
	if (mgr().active())
		mgr().stop("the server is shutting down");
	rl(world_server::Record::of_phase(world_server::Phase::Down));
	coop_console::flush_output(2000); // the parent waits for "down": it must leave before the process does
	coop_console::quit_done();
	Console->Execute("quit");
}

// The parent's header: the players of the session, the state of every map server; a line when one changes. The session's peers without the
// map servers' bridges (stage 1's coop_roster.h will say who plays; until then a bridge is the one peer here that is not a player).
void report_counts()
{
	if (!coop_console::output_on())
		return;
	const u64 now = mgr().now_ms();
	if (now < g.counts_at)
		return;
	g.counts_at = now + kTitleEveryMs;
	u32 players = 0;
	if (mgr().active())
	{
		const std::vector<PeerId>& bridges = mapserver::host_bridges();
		for (const PeerInfo& p : mgr().session().peers())
			players += std::find(bridges.begin(), bridges.end(), p.id) == bridges.end() ? 1u : 0u;
	}
	if (players != g.players_said)
	{
		g.players_said = players;
		rl(world_server::Record::of_players(players));
	}
	if (!mgr().active() || !ai().get_alife())
		return;
	std::map<u16, world_server::MapState> now_states;
	for (const auto& it : ai().game_graph().header().levels())
	{
		const u16 level = u16(it.first);
		if (!mapserver::host_serves(level))
			continue;
		const levels::wire::ServerState s = mapserver::host_state_of(level);
		world_server::MapState state = world_server::MapState::Loading;
		if (s == levels::wire::ServerState::Ready)
			state = levels::players_on(level) ? world_server::MapState::Serving : world_server::MapState::Ready;
		else if (s == levels::wire::ServerState::Draining || s == levels::wire::ServerState::Stopped)
			state = world_server::MapState::Gone; // Stopping: it hands its level back and exits, nobody enters it any more
		now_states[level] = state;
	}
	for (const auto& it : g.mapservers_said)
		if (it.second != world_server::MapState::Gone && !now_states.count(it.first))
			now_states[it.first] = world_server::MapState::Gone;
	for (const auto& it : now_states)
	{
		const auto said = g.mapservers_said.find(it.first);
		if (said != g.mapservers_said.end() && said->second == it.second)
			continue;
		if (said == g.mapservers_said.end() && it.second == world_server::MapState::Gone)
			continue;
		const char* name = levels::level_name(it.first);
		rl(world_server::Record::of_mapserver(name ? name : std::to_string(it.first), it.second));
		g.mapservers_said[it.first] = it.second;
	}
}

const char* level_name(PeerId peer)
{
	const char* name = levels::level_name(levels::level_of_player(peer));
	return name ? name : "no level";
}

// --- the commands

class CCC_CoopPlayers : public IConsole_Command
{
public:
	CCC_CoopPlayers(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		if (!mgr().is_host())
		{
			::Msg("! [coop] coop_players: only the host of a session has the list");
			return;
		}
		const std::vector<PeerInfo> peers = mgr().session().peers();
		string128 line;
		xr_sprintf(line, "[coop] %u player(s) in the session", u32(peers.size()));
		say(line);
		for (const PeerInfo& p : peers)
		{
			string256 row;
			xr_sprintf(row, "[coop]   #%u %-16s %-16s rtt %4u ms loss %4.1f%% %s", p.id, p.nick.c_str(), level_name(p.id), p.rtt_ms, p.packet_loss * 100.f,
			    p.state == PeerState::Joined ? "joined" : "connecting");
			say(row);
		}
	}
	void Info(TInfo& I) override { xr_strcpy(I, "host: the players of the session: id, nick, level, rtt, loss"); }
};

// coop_kick <id|nick> [reason]. `by`: the operator who asked (run_for), kInvalidPeer for the server's console. The operator never kicks
// himself (he would lose the rights he asked with); false when nobody was kicked.
bool kick_line(const std::string& args, PeerId by)
{
	if (!mgr().is_host())
	{
		tell("! [coop] coop_kick: only the host of a session can do this");
		return false;
	}
	std::string who = args;
	std::string why = "kicked by the host";
	const size_t blank = who.find(' ');
	if (blank != std::string::npos)
	{
		why = who.substr(blank + 1);
		who.erase(blank);
	}
	const std::vector<PeerInfo> peers = mgr().session().peers();
	// The players only: a map server's bridge is none (a kicked bridge breaks its level for everybody on it), nor is a seat that plays no
	// character. An id wins over a nick that reads as the number; a nick two players have is nobody (op::kick_pick, security review
	// 2026-11-12).
	std::vector<op::KickRow> rows;
	std::vector<const PeerInfo*> of_row;
	for (const PeerInfo& p : peers)
		if (!mapserver::is_bridge(p.id) && p.plays)
		{
			rows.push_back({p.id, p.nick});
			of_row.push_back(&p);
		}
	const int pick = op::kick_pick(rows, who);
	if (pick == op::kKickAmbiguous)
	{
		tell("! [coop] coop_kick: more than one player is '" + who + "': name him by his id (coop_players)");
		return false;
	}
	if (pick < 0)
	{
		tell("! [coop] coop_kick: nobody is '" + who + "': coop_players");
		return false;
	}
	const PeerInfo* target = of_row[size_t(pick)];
	if (by != kInvalidPeer && target->id == by)
	{
		tell("! [coop] coop_kick: not yourself");
		return false;
	}
	tell("[coop] kicked #" + std::to_string(target->id) + " '" + target->nick + "': " + why);
	mgr().session().kick(target->id, RejectReason::Kicked, why);
	return true;
}

class CCC_CoopKick : public IConsole_Command
{
public:
	CCC_CoopKick(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }
	void Execute(LPCSTR args) override { kick_line(args ? args : "", kInvalidPeer); }
	void Info(TInfo& I) override { xr_strcpy(I, "host: coop_kick <id|nick> [reason]: the player leaves the session, his character stays in the world"); }
};

// The clock of the world. Forward only: the stock clock has no way back, and neither do its timers.
void change_time(u32 minutes)
{
	game_sv_Single* single = g_pGameLevel && Level().Server ? smart_cast<game_sv_Single*>(Level().Server->game) : nullptr;
	if (!single || !ai().get_alife())
	{
		tell("! [coop] coop_time: no world");
		return;
	}
	const u32 seconds = minutes * 60;
	g_pGamePersistent->Environment().ChangeGameTime(float(seconds));
	single->alife().time_manager().change_game_time(seconds * 1000);
	worldstate::note_time_jump(s64(seconds) * 1000); // the sky's sections follow at once (no level_weathers forces it here)
}

class CCC_CoopTime : public IConsole_Command
{
public:
	CCC_CoopTime(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		if (!ai().get_alife() || !mgr().is_host())
		{
			tell("! [coop] coop_time: only the host of a world can do this");
			return;
		}
		if (args && *args)
		{
			u32 hour = 0, minute = 0;
			if (sscanf(args, "%u:%u", &hour, &minute) < 1 || hour > 23 || minute > 59)
			{
				tell("! usage: coop_time [hh[:mm]]");
				return;
			}
			u32 y, mo, d, h, mi, s, ms;
			split_time(ai().alife().time_manager().game_time(), y, mo, d, h, mi, s, ms);
			const u32 now = h * 60 + mi;
			const u32 target = hour * 60 + minute;
			change_time(target > now ? target - now : target + 24 * 60 - now);
		}
		u32 y, mo, d, h, mi, s, ms;
		split_time(ai().alife().time_manager().game_time(), y, mo, d, h, mi, s, ms);
		string128 line;
		xr_sprintf(line, "[coop] game time %04u.%02u.%02u %02u:%02u, factor %.1f", y, mo, d, h, mi, ai().alife().time_manager().time_factor());
		say(line);
	}
	void Info(TInfo& I) override { xr_strcpy(I, "host: the game time; coop_time hh[:mm] moves the world forward to that hour"); }
};

// The local time of a save's file (SaveFile::time, a FILETIME) as "YYYY-MM-DD HH:MM"; "0000-00-00 00:00" when it cannot be read.
std::string save_time_text(u64 time)
{
	FILETIME utc, local;
	utc.dwLowDateTime = DWORD(time & 0xFFFFFFFFu);
	utc.dwHighDateTime = DWORD(time >> 32);
	SYSTEMTIME st;
	if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
		return "0000-00-00 00:00";
	char text[32];
	std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", unsigned(st.wYear), unsigned(st.wMonth), unsigned(st.wDay),
	    unsigned(st.wHour), unsigned(st.wMinute));
	return text;
}

class CCC_CoopRestore : public IConsole_Command
{
public:
	CCC_CoopRestore(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = true;
		bLowerCaseArgs = false;
	}
	void Execute(LPCSTR args) override
	{
		if (!mgr().is_host())
		{
			tell("! [coop] coop_restore: only the host of a session can do this");
			return;
		}
		std::vector<SaveFile> files;
		list_saves(files);
		const std::vector<SaveFile> list = restore_list(std::move(files));
		const std::string what = args ? args : "";
		if (what.empty())
		{
			// one row per save, "[coop]   <n> <name> <YYYY-MM-DD HH:MM>" (local time of the file): the console reads it, and the operator's
			// Co-op window parses it (coop_menu.parse_restore_lines). A name is a file name: no blank in an autosave's (coop_auto_*).
			u32 n = 0;
			for (const SaveFile& f : list)
			{
				string256 row;
				xr_sprintf(row, "[coop]   %2u %s %s", ++n, f.name.c_str(), save_time_text(f.time).c_str());
				say(row);
			}
			if (list.empty())
				say("[coop] no autosaves in this folder");
			return;
		}
		const std::string name = pick_restore(list, what);
		if (name.empty())
		{
			tell("! [coop] coop_restore: no autosave '" + what + "': coop_restore lists them");
			return;
		}
		// the session stays: a new world is a new epoch, everybody loads it (join::on_world_created)
		tell("[coop] the world goes back to " + name);
		Console->Execute(start_line(World::Load, name).c_str());
	}
	void Info(TInfo& I) override { xr_strcpy(I, "host: coop_restore lists the autosaves, coop_restore <n|name> loads one: everybody follows"); }
};

class CCC_CoopWorld : public IConsole_Command
{
public:
	CCC_CoopWorld(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }
	void Execute(LPCSTR args) override
	{
		const std::string what = args ? args : "";
		if (what == "list")
		{
			std::vector<SaveFile> files;
			list_saves(files);
			for (const SaveFile& f : files)
				say(("[coop]   " + f.name).c_str());
			return;
		}
		if (g.world_asked || g_pGameLevel)
		{
			::Msg("! [coop] coop_world: this process has its world, to go to another use coop_restore");
			return;
		}
		if (what == "new")
			start_world(World::New, "");
		else
			start_world(World::Load, what);
	}
	void Info(TInfo& I) override { xr_strcpy(I, "dedicated host: coop_world new | last | <save> | list"); }
};

class CCC_CoopConsoleLog : public IConsole_Command
{
public:
	CCC_CoopConsoleLog(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		if (args && *args)
			coop_console::set_log_level(std::atoi(args));
		::Msg("[coop] the window shows level %d of the log: 0 errors, 1 the session, 2 everything", coop_console::log_level());
	}
	void Info(TInfo& I) override { xr_strcpy(I, "dedicated host: what of the log the window shows: 0 errors, 1 the session, 2 everything"); }
};
} // namespace

bool on() { return g_coop_dedicated; }

// ---------------------------------------------------------------- the operator (plan 2026-11-12 §2.3)

void answer_line(const char* line)
{
	if (g_answering && line && g_answer.size() < 64)
		g_answer.push_back(line);
}

namespace
{
// The answer as the ScriptEvent "coop.op.answer" to the operator: { cmd = string, ok = bool, text = string } (coop_main.decode).
void send_answer(PeerId to, const std::string& cmd, bool ok, const std::vector<std::string>& lines)
{
	// 8 KiB of text counted as it is sent (op::answer_text): a script event carries 16 KiB at most (coop_script.cpp kMaxPayload), and
	// a longer one is dropped by the client without a word
	constexpr size_t kMaxText = 8 * 1024;
	const std::string payload = "cmd=s" + op::escape_value(cmd) + ";ok=" + (ok ? "b1" : "b0") + ";text=s" + op::answer_text(lines, kMaxText);
	Writer w(payload.size() + 64);
	w.u32(kHostPeer); // the origin: the world server
	w.i32(s32(to));
	w.str("coop.op.answer");
	w.str(payload);
	net::host_send(to, Msg::ScriptEvent, w);
}

// coop_game_settings.operator_set looked up inside the protected call (the first use of the script loads it through _G.__index).
int game_set_lookup(lua_State* L)
{
	lua_getglobal(L, "coop_game_settings");
	if (!lua_istable(L, -1))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_getfield(L, -1, "operator_set");
	return 1;
}

// coop_game_set <key> <value> of the operator (plan §2.4): the key and the value go to coop_game_settings.operator_set(key, raw, peer)
// as Lua strings in a protected call, never as a console line; it finds the key in the catalog, checks that it may change on an existing
// world and that the value is one of the item's, and applies it as the host's own change. `text`: its answer (the key and the canonical
// value) or the reason of a refusal.
bool game_set(PeerId from, const std::string& rest, std::string& text)
{
	const size_t blank = rest.find(' ');
	if (blank == std::string::npos || blank == 0 || blank + 1 >= rest.size())
	{
		text = "usage: coop_game_set <key> <value>";
		return false;
	}
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 8))
	{
		text = "no Lua state";
		return false;
	}
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &stage_traceback);
	const int errfunc = lua_gettop(L);
	lua_pushcfunction(L, &game_set_lookup);
	if (lua_pcall(L, 0, 1, errfunc) != 0 || !lua_isfunction(L, -1))
	{
		lua_settop(L, top);
		text = "coop_game_settings.operator_set is missing";
		return false;
	}
	lua_pushlstring(L, rest.data(), blank);
	lua_pushlstring(L, rest.data() + blank + 1, rest.size() - blank - 1);
	lua_pushnumber(L, lua_Number(from));
	if (lua_pcall(L, 3, 2, errfunc) != 0)
	{
		const char* err = lua_tostring(L, -1);
		::Msg("! [coop] coop_game_set: lua error in coop_game_settings.operator_set: %s", err ? err : "?");
		lua_settop(L, top);
		text = "the server's script failed (see its log)";
		return false;
	}
	const bool ok = lua_toboolean(L, -2) != 0;
	size_t len = 0;
	const char* s = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : nullptr;
	text = s ? std::string(s, len > 512 ? size_t(512) : len) : std::string(ok ? "done" : "refused");
	lua_settop(L, top);
	return ok;
}
} // namespace

void run_for(PeerId from, const std::string& line)
{
	if (!on() || !mgr().is_host())
		return; // only a world server has an operator
	// The session checked him when the line came; check again now (a callback of the same update, but the rule is the rule).
	const PeerInfo* p = mgr().session().peer(from);
	if (!p || p->state != PeerState::Joined || !p->host_rights)
	{
		::Msg("! [coop] operator: a command of #%u refused: not the operator now", from);
		return;
	}
	if (op::command_verdict(true, line) != op::Command::Run)
	{
		::Msg("! [coop] operator #%u: a command refused (not_allowed)", from);
		return;
	}
	const std::string nick = p->nick; // p is gone after a kick or a leave
	const size_t blank = line.find(' ');
	const std::string cmd = line.substr(0, blank);
	const std::string rest = blank == std::string::npos ? std::string() : line.substr(blank + 1);
	::Msg("[coop] operator #%u '%s': %s", from, nick.c_str(), cmd.c_str());

	g_answer.clear();
	g_answering = true;
	bool ok = true;
	if (cmd == "coop_kick")
		ok = kick_line(rest, from);
	else if (cmd == "coop_dbg")
		debug::operator_dbg(from, rest);
	else if (cmd == "coop_game_set")
	{
		// plan §2.4: the value goes into the game's record through its table of values (coop_game_settings.operator_set). Never a
		// console line.
		std::string text;
		ok = game_set(from, rest, text);
		tell((ok ? "[coop] coop_game_set: " : "! [coop] coop_game_set: refused: ") + text);
	}
	else
	{
		// the way of a line of the server's own pipe (parse_line, run_line): the line as op::allowed saw it. Console->Execute does not
		// record it in the console's history.
		const Parsed parsed = parse_line(line);
		if (parsed.kind == Line::Command || parsed.kind == Line::Chat)
			run_line(line.c_str());
		else
		{
			tell("! [coop] " + cmd + ": not a command of the operator");
			ok = false;
		}
	}
	g_answering = false;
	std::vector<std::string> answer;
	answer.swap(g_answer);
	if (answer.empty())
		answer.push_back(ok ? "done (see the server's log)" : "refused");
	send_answer(from, cmd, ok, answer);
}

const Args& args() { return g.args; }

bool new_world() { return g.new_world; }
bool quitting() { return g.quitting; }

void frame()
{
	if (!on())
		return;
	if (!g.parsed)
	{
		g.parsed = true;
		first_frame();
	}
	string1024 line;
	while (coop_console::next_line(line, sizeof(line)))
		run_line(line);
	if (coop_console::quit_asked())
		g.quitting = true;
	if (g.quit_sent)
		return;
	if (g.quitting)
	{
		g.quit_sent = true;
		quit_now();
		return;
	}
	if (world_up())
	{
		g.new_world = false; // the next world is a save (coop_restore)
		mapserver::ghost_frame();
	}
	update_title();
	report_counts();
}

bool place_ghost(CALifeObjectRegistry& objects)
{
	const CGameGraph& graph = ai().game_graph();
	GameGraph::_LEVEL_ID level = GameGraph::_LEVEL_ID(-1);
	for (const auto& it : graph.header().levels())
		if (!xr_strcmp(it.second.name(), kServiceLevel))
			level = it.first;
	CSE_ALifeCreatureActor* ghost = smart_cast<CSE_ALifeCreatureActor*>(objects.object(0, true));
	if (!ghost || level == GameGraph::_LEVEL_ID(-1))
	{
		::Msg("! [coop] dedicated: no ghost: actor %p, level '%s' %s", ghost, kServiceLevel, level == GameGraph::_LEVEL_ID(-1) ? "not in the game" : "found");
		return false;
	}
	for (u32 v = 0, n = graph.header().vertex_count(); v < n; ++v)
	{
		const auto* vertex = graph.vertex(GameGraph::_GRAPH_ID(v));
		if (vertex->level_id() != level)
			continue;
		ghost->o_Position = vertex->level_point();
		ghost->m_tNodeID = vertex->level_vertex_id();
		ghost->m_tGraphID = GameGraph::_GRAPH_ID(v);
		ghost->m_holderID = u16(-1);
		ghost->set_health(1.f);
		::Msg("[coop] dedicated: the ghost stands on '%s', vertex %u", kServiceLevel, v);
		return true;
	}
	::Msg("! [coop] dedicated: level '%s' has no vertex in the game graph", kServiceLevel);
	return false;
}

void register_console()
{
	CMD1(CCC_CoopPlayers, "coop_players");
	CMD1(CCC_CoopKick, "coop_kick");
	CMD1(CCC_CoopTime, "coop_time");
	CMD1(CCC_CoopRestore, "coop_restore");
	CMD1(CCC_CoopWorld, "coop_world");
	CMD1(CCC_CoopConsoleLog, "coop_console_log");
}
} // namespace dedicated
} // namespace coop
