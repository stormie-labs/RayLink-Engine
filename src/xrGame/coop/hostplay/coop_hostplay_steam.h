// Кооп: только там, где хост играет сам (docs/MODES.md)
// Anomaly Coop — Steam, the co-op's only (docs/MODES.md section 8; INTEG-C, rv_A-S6-E-net fix 1). The co-op layer
// (coop_hostplay_mode.cpp) asks these; nothing of the Core includes this file.
#pragma once

#include <string>

struct lua_State;

namespace coop
{
namespace hostplay
{
namespace steam
{
// Every frame (Layer::join_request), in a session or not: once every kSteamJoinPollMs, an invite the launcher accepted while the game
// runs (the tunnel's loopback address it left in the file RAYLINK_STEAM_JOIN names; the file is taken). True and `address`
// ("127.0.0.1:<port>"): join it.
bool poll(u64 now, std::string& address);
// The launcher's Steam mode started this game with a lobby: Steam's "invite a friend" window can open over the game.
bool invite_ready();
// Opens that window; false when it cannot.
bool invite_window();
// coop.steam_invite_ready() and coop.steam_invite() (Layer::register_lua, every Lua state).
void register_lua(lua_State* L);
} // namespace steam
} // namespace hostplay
} // namespace coop
