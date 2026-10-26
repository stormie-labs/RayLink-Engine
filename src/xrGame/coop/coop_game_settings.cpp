// Anomaly Coop — "Game settings", the engine's part. See coop_game_settings.h and docs/GAME_SETTINGS.md §3.4, §3.5.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_game_settings.h"
#include "xrCoop/coop_game_values.h"

#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "ai_space.h"
#include "script_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace coop
{
namespace game
{
namespace
{
// The Lua side: scripts\coop_game_settings.script, function before_world_start(mode, save).
constexpr const char* kHookScript = "coop_game_settings";
constexpr const char* kHookFunction = "before_world_start";

// One set_cvars call carries at most this many names (the white list has 17): a table with more is a script's mistake and
// the rest is not looked at.
constexpr size_t kMaxCvarsPerCall = 64;

Pending g_pending; // the new game that waits for its world
CvarLayer g_layer; // the game's console values over the machine's
std::string g_world_faction; // the world's group; "" none

// ---------------------------------------------------------------- the console

// The engine's console behind the layer's interface.
class EngineConsole final : public CvarConsole
{
public:
	bool get(const std::string& name, std::string& value) override
	{
		if (!Console)
			return false;
		IConsole_Command* command = Console->GetCommand(name.c_str());
		if (!command)
			return false;
		IConsole_Command::TStatus status;
		status[0] = 0;
		command->Status(status);
		status[sizeof(status) - 1] = 0;
		value = status;
		return true;
	}

	void set(const std::string& name, const std::string& value) override
	{
		if (!Console)
			return;
		const std::string line = name + " " + value;
		Console->Execute(line.c_str()); // not recorded in the console's history
	}
};

EngineConsole g_console;

// ---------------------------------------------------------------- the Lua call of the hook

// debug.traceback of the error that came out of the hook.
int hook_traceback(lua_State* L)
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

// coop_game_settings.before_world_start, or nil. Runs in a protected call (call_hook): the first lookup loads the script
// through _G.__index (gamedata/scripts/<name>.script), and a script that fails to load, or an addon's strict _G metatable,
// raises there. No C++ object lives here: a Lua error leaves this frame without running destructors.
int hook_lookup(lua_State* L)
{
	lua_getglobal(L, kHookScript);
	if (!lua_istable(L, -1))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_getfield(L, -1, kHookFunction);
	return 1;
}

enum class HookCall
{
	Done,
	Missing, // no Lua state, no script or no function: nothing to do
	Failed, // it raised an error (logged)
};

HookCall call_hook(const char* mode, const char* save)
{
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 8))
		return HookCall::Missing;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &hook_traceback);
	const int errfunc = lua_gettop(L);
	lua_pushcfunction(L, &hook_lookup);
	if (lua_pcall(L, 0, 1, errfunc) != 0)
	{
		const char* err = lua_tostring(L, -1);
		::Msg("! [coop] game settings: lua error looking up %s.%s: %s", kHookScript, kHookFunction, err ? err : "?");
		lua_settop(L, top);
		return HookCall::Failed;
	}
	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, top);
		return HookCall::Missing;
	}
	lua_pushstring(L, mode);
	lua_pushstring(L, save);
	if (lua_pcall(L, 2, 0, errfunc) != 0)
	{
		const char* err = lua_tostring(L, -1);
		::Msg("! [coop] game settings: lua error in %s.%s(%s, %s), the world starts without the game's values: %s",
		    kHookScript, kHookFunction, mode, save, err ? err : "?");
		lua_settop(L, top);
		return HookCall::Failed;
	}
	lua_settop(L, top);
	return HookCall::Done;
}

// ---------------------------------------------------------------- Lua API

// A plain C function each: a wrong argument from a script returns false / nil instead of a luabind overload error, which
// is fatal in this engine.

// coop.game.stage_new(payload, faction) -> bool
int l_stage_new(lua_State* L)
{
	bool ok = false;
	if (lua_type(L, 1) == LUA_TSTRING && lua_type(L, 2) == LUA_TSTRING)
	{
		size_t payload_size = 0;
		size_t faction_size = 0;
		const char* payload = lua_tolstring(L, 1, &payload_size);
		const char* faction = lua_tolstring(L, 2, &faction_size);
		// The sizes are checked before a byte is copied: a script's mistake must not cost a copy of what it passed.
		if (payload_size <= kMaxPendingPayload && faction_size <= kMaxCharName)
			ok = g_pending.stage(std::string(payload, payload_size), std::string(faction, faction_size));
	}
	if (ok)
		::Msg("[coop] game settings: a new game is staged (group '%s')", g_pending.faction().c_str());
	else
	{
		g_pending.clear(); // see Pending::stage: a refused stage leaves nothing staged
		::Msg("! [coop] game settings: coop.game.stage_new refused (payload 1..%u bytes, faction a character name)",
		    u32(kMaxPendingPayload));
	}
	lua_pushboolean(L, ok ? 1 : 0);
	return 1;
}

// coop.game.take_new() -> payload|nil, faction|nil
int l_take_new(lua_State* L)
{
	std::string payload;
	std::string faction;
	if (!g_pending.take(payload, faction))
	{
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}
	lua_pushlstring(L, payload.data(), payload.size());
	lua_pushlstring(L, faction.data(), faction.size());
	return 2;
}

// The value at `index` as the console line takes it. A string longer than a value may be is cut to one byte over the
// limit: it fails the check all the same and a script's huge string is not copied. A number must be finite.
bool cvar_text_at(lua_State* L, int index, std::string& out)
{
	switch (lua_type(L, index))
	{
	case LUA_TSTRING:
	{
		size_t size = 0;
		const char* s = lua_tolstring(L, index, &size);
		out.assign(s, (std::min)(size, kMaxCvarValue + 1));
		return true;
	}
	case LUA_TNUMBER:
	{
		const double v = lua_tonumber(L, index);
		if (!std::isfinite(v))
			return false;
		char text[40];
		std::snprintf(text, sizeof(text), "%.9g", v);
		out = text;
		return true;
	}
	case LUA_TBOOLEAN:
		out = lua_toboolean(L, index) ? "1" : "0";
		return true;
	default:
		return false;
	}
}

const char* verdict_reason(CvarVerdict v)
{
	switch (v)
	{
	case CvarVerdict::NotAllowed: return "not_allowed";
	case CvarVerdict::BadValue: return "bad_value";
	case CvarVerdict::Unknown: return "unknown";
	case CvarVerdict::Applied: break;
	}
	return "";
}

// coop.game.set_cvars({name = value}) -> applied, refused
int l_set_cvars(lua_State* L)
{
	// Everything is read out of the table first (no console command runs while it is being walked), in name order.
	std::vector<std::pair<std::string, std::string>> wanted; // name, value ("" = not a usable value)
	if (lua_type(L, 1) == LUA_TTABLE)
	{
		lua_pushnil(L);
		while (lua_next(L, 1) != 0)
		{
			// Only a string key is read: lua_tolstring on a number would turn the key into a string and confuse lua_next.
			if (lua_type(L, -2) == LUA_TSTRING && wanted.size() < kMaxCvarsPerCall)
			{
				size_t name_size = 0;
				const char* name = lua_tolstring(L, -2, &name_size);
				std::string value;
				if (!cvar_text_at(L, -1, value))
					value.clear();
				wanted.emplace_back(std::string(name, (std::min)(name_size, kMaxCvarValue + 1)), std::move(value));
			}
			lua_pop(L, 1);
		}
	}
	std::sort(wanted.begin(), wanted.end());

	u32 applied = 0;
	std::vector<std::pair<std::string, CvarVerdict>> refused;
	for (const auto& w : wanted)
	{
		const CvarVerdict v = g_layer.apply(g_console, w.first, w.second);
		if (v == CvarVerdict::Applied)
			++applied;
		else
			refused.emplace_back(w.first, v);
	}

	if (applied || !refused.empty())
		::Msg("[coop] game settings: %u console value(s) set, %u refused (%u held over the machine's)", applied,
		    u32(refused.size()), u32(g_layer.held()));
	for (const auto& r : refused)
		::Msg("! [coop] game settings: console rule '%.64s' refused: %s", r.first.c_str(), verdict_reason(r.second));

	lua_pushnumber(L, lua_Number(applied));
	lua_createtable(L, 0, int(refused.size()));
	for (const auto& r : refused)
	{
		lua_pushlstring(L, r.first.data(), r.first.size());
		lua_pushstring(L, verdict_reason(r.second));
		lua_rawset(L, -3);
	}
	return 2;
}

// Gives the machine's values back and says so; returns how many.
size_t release_cvars(const char* why)
{
	const size_t restored = g_layer.release(g_console);
	if (restored)
		::Msg("[coop] game settings: %u console value(s) back to the machine's (%s)", u32(restored), why);
	return restored;
}

// coop.game.release_cvars() -> restored
int l_release_cvars(lua_State* L)
{
	lua_pushnumber(L, lua_Number(release_cvars("released by a script")));
	return 1;
}

// coop.game.machine_cvar(name) -> string|nil
int l_machine_cvar(lua_State* L)
{
	const std::string* machine = nullptr;
	if (lua_type(L, 1) == LUA_TSTRING)
	{
		size_t size = 0;
		const char* name = lua_tolstring(L, 1, &size);
		machine = g_layer.machine(std::string(name, (std::min)(size, kMaxCvarValue + 1)));
	}
	if (machine)
		lua_pushlstring(L, machine->data(), machine->size());
	else
		lua_pushnil(L);
	return 1;
}

// coop.game.set_world_faction(f) -> bool
int l_set_world_faction(lua_State* L)
{
	bool ok = false;
	if (lua_type(L, 1) == LUA_TSTRING)
	{
		size_t size = 0;
		const char* faction = lua_tolstring(L, 1, &size);
		if (size <= kMaxCharName)
		{
			const std::string f(faction, size);
			ok = world_faction_valid(f);
			if (ok)
				g_world_faction = f;
		}
	}
	lua_pushboolean(L, ok ? 1 : 0);
	return 1;
}

// coop.game.world_faction() -> string
int l_world_faction(lua_State* L)
{
	lua_pushlstring(L, g_world_faction.data(), g_world_faction.size());
	return 1;
}
} // namespace

// ---------------------------------------------------------------- public

void before_world_start()
{
	if (!g_pGamePersistent)
		return;
	const IGame_Persistent::params& p = g_pGamePersistent->m_game_params;
	const StartMode mode = start_mode(p.m_new_or_load);
	if (mode == StartMode::Other)
	{
		::Msg("[coop] game settings: world start '%s' is neither new nor load, left alone", p.m_new_or_load);
		return;
	}
	on_world_start(mode, g_pending, g_world_faction);
	::Msg("[coop] game settings: world start (%s, '%s'), group '%s', %s", start_mode_name(mode), p.m_game_or_spawn,
	    g_world_faction.c_str(), g_pending.staged() ? "a new game is staged" : "no game staged");

	switch (call_hook(start_mode_name(mode), p.m_game_or_spawn))
	{
	case HookCall::Done:
	case HookCall::Failed: // logged by call_hook
		break;
	case HookCall::Missing:
	{
		static bool logged = false; // once per process: every world start would say it again
		if (!logged)
		{
			logged = true;
			::Msg("[coop] game settings: %s.%s is not there (gamedata/scripts/%s.script not installed?), nothing to do",
			    kHookScript, kHookFunction, kHookScript);
		}
		break;
	}
	}
}

void on_level_exit() { release_cvars("the level is left"); }

const std::string& world_faction() { return g_world_faction; }

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"stage_new", &l_stage_new},
		{"take_new", &l_take_new},
		{"set_cvars", &l_set_cvars},
		{"release_cvars", &l_release_cvars},
		{"machine_cvar", &l_machine_cvar},
		{"set_world_faction", &l_set_world_faction},
		{"world_faction", &l_world_faction},
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	lua_createtable(L, 0, 8);
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_setfield(L, -2, "game"); // coop.game
	lua_pop(L, 1); // coop
}
} // namespace game
} // namespace coop
