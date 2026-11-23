// Кооп: только там, где хост играет сам (docs/MODES.md)
// Anomaly Coop — Steam, the co-op's only (docs/MODES.md section 8; INTEG-C, rv_A-S6-E-net fix 1). Moved here as it was from the Core:
// the invite polling of Manager::autostart, the two invite functions of coop_manager.cpp and the Lua exports of coop_script.cpp. The
// co-op layer asks them (coop_hostplay_mode.cpp CoopLayer: join_request, register_lua); the Core's autostart keeps its retry queue and
// its rules for an address a layer was asked to join (the same one while in that game changes nothing).
#include "stdafx.h"
#include "pch_script.h"
#include "coop_hostplay_steam.h"
#include "xrCoop/coop_steam_join.h"

#include <cstdio>

namespace coop
{
namespace hostplay
{
namespace steam
{
namespace
{
constexpr u32 kSteamJoinPollMs = 500;
u64 g_next_poll = 0;

// The launcher's Steam mode: an invite accepted while the game runs leaves the tunnel's address in the file
// RAYLINK_STEAM_JOIN names. False: nothing there.
bool invite_waits(u16& port)
{
	wchar_t path[1024];
	const DWORD length = GetEnvironmentVariableW(L"RAYLINK_STEAM_JOIN", path, 1024);
	if (length == 0 || length >= 1024)
		return false;
	FILE* file = _wfopen(path, L"rb");
	if (!file)
		return false;
	char text[32] = {};
	fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	_wremove(path);
	u16 tunnel = 0;
	if (!steam_join::parse_tunnel(text, tunnel))
	{
		::Msg("! [coop] Steam invite: not an address");
		return false;
	}
	port = tunnel;
	return true;
}

// coop.steam_invite_ready() -> true when the launcher's Steam mode started this game with a lobby
int l_steam_invite_ready(lua_State* L)
{
	lua_pushboolean(L, invite_ready() ? 1 : 0);
	return 1;
}

// coop.steam_invite() -> bool: Steam's "invite a friend" window over the game
int l_steam_invite(lua_State* L)
{
	lua_pushboolean(L, invite_window() ? 1 : 0);
	return 1;
}
} // namespace

bool poll(u64 now, std::string& address)
{
	if (now < g_next_poll)
		return false;
	g_next_poll = now + kSteamJoinPollMs;
	u16 port = 0;
	if (!invite_waits(port))
		return false;
	string64 text;
	xr_sprintf(text, "%s:%u", steam_join::kLoopback, u32(port));
	address = text;
	return true;
}

bool invite_ready()
{
	wchar_t lobby[32];
	const DWORD length = GetEnvironmentVariableW(L"RAYLINK_STEAM_LOBBY", lobby, 32);
	HMODULE api = GetModuleHandleW(L"steam_api64.dll");
	if (length == 0 || length >= 32 || !api)
		return false;
	typedef void*(__cdecl * friends_of_steam)();
	const friends_of_steam friends = reinterpret_cast<friends_of_steam>(GetProcAddress(api, "SteamAPI_SteamFriends_v018"));
	return friends && friends();
}

bool invite_window()
{
	wchar_t lobby[32];
	const DWORD length = GetEnvironmentVariableW(L"RAYLINK_STEAM_LOBBY", lobby, 32);
	HMODULE api = GetModuleHandleW(L"steam_api64.dll");
	if (length == 0 || length >= 32 || !api)
		return false;
	typedef void*(__cdecl * friends_of_steam)();
	typedef void(__cdecl * invite_dialog)(void*, unsigned long long);
	const friends_of_steam friends = reinterpret_cast<friends_of_steam>(GetProcAddress(api, "SteamAPI_SteamFriends_v018"));
	const invite_dialog dialog = reinterpret_cast<invite_dialog>(GetProcAddress(api, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog"));
	if (!friends || !dialog)
		return false;
	void* steam_friends = friends(); // null when Steam refused this process (x_ray.cpp)
	if (!steam_friends)
		return false;
	dialog(steam_friends, _wcstoui64(lobby, nullptr, 10));
	return true;
}

void register_lua(lua_State* L)
{
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	lua_pushcfunction(L, &l_steam_invite_ready);
	lua_setfield(L, -2, "steam_invite_ready");
	lua_pushcfunction(L, &l_steam_invite);
	lua_setfield(L, -2, "steam_invite");
	lua_pop(L, 1);
}
} // namespace steam
} // namespace hostplay
} // namespace coop
