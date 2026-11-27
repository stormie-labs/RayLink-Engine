// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — what the server layer registers before any session: its console commands and its Lua table. ServerLayer
// (coop_srv_mode.cpp) calls them through the switchboard (coop_mode.cpp register_layers_console / register_layers_lua).
#pragma once

struct lua_State;

namespace coop
{
namespace srv
{
void register_console(); // coop_srv_console.cpp: srv_* and the old coop_* names
void register_lua(lua_State* L); // coop_srv_script.cpp: the table srv.*
} // namespace srv
} // namespace coop
