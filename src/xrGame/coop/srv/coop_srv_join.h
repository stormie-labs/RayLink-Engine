// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the server's part of the join (S6 pattern P-G, unit A-S6J): srv/coop_srv_join.cpp. Called by the server's modes
// (srv/coop_srv_mode.cpp) only: the Core reaches none of it (it asks the session's mode, xrCoop/coop_mode.h H-JOIN).
#pragma once

#include "xrCoop/coop_protocol.h"

namespace coop
{
class Endpoint;
class Session;
namespace dedicated
{
struct WorldFacts; // xrCoop/coop_dedicated_rules.h
} // namespace dedicated

namespace srv
{
// --- the world server
// What its autostart waits for, member by member, quietly (was join::host_world_facts).
dedicated::WorldFacts world_facts();
// Its handlers on its session, after the Core's (join::install_server): a bridge's JoinRequest and WorldLoaded are answered here, a
// player's go on to the Core's; a player's LevelMove (he leaves his level alone) is the server's only. A new session forgets the bridges
// that waited for a world.
void join_install_world(Session& s);
// Every frame: the map servers that asked for their world while there was none to give get it when there is.
void join_world_frame();

// --- a map server
// Its handlers on its connection to the world server, after the Core's client tables (join::install_client): the world server's
// SessionReset ends this process (its world is gone; a new process comes for the new one).
void join_install_map(Endpoint& world);

// --- a player of a server world: the feed lines (S5, xrCoop/srv/coop_srv_feed_rules.h)
// A session starts (his Welcome said it is a server's): nobody is known yet.
void feed_start();
// Every frame of his session: "<nick> went to <level>" / "<nick> came from <level>" in his PDA feed, from the changes of the world server's
// table of seats he gets (LevelMap).
void feed_frame();
} // namespace srv
} // namespace coop
