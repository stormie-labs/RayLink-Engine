// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — what a server's process costs: memory, Lua, ALife, the frame (srv/coop_mapserver_stats.cpp).
#pragma once

namespace coop
{
namespace srv
{
// Every frame of a server's process (a map server, the world server), from the server's boot: one line in the log every 60 s.
void mapserver_stats_frame();
// The console command coop_mapserver_stats, in every process (srv::register_console).
void register_mapserver_stats_console();
} // namespace srv
} // namespace coop
