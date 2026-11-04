// Anomaly Coop — the roles of a process (docs/plans/2026-10-24-f3a-split-levels.md §1.1) as flags. They replace the pair host /
// client: every check "am I the host" is one of the roles below, whichever the code needs.
//   WorldServer   the host's process: the world, offline, time, story, money, the save, the registry of levels;
//   MapAuthority  the process that computes a level: the objects, the AI, the physics, the requests of the players on it. The
//                 host's process for the host's level, a map server for every other level with players. Never a client: the
//                 owner's decision of 2026-10-24, repeated 2026-11-01 ("the maps are on one side only, the host's");
//   Player        the host's process (the host player, a client of its own world server over the LocalLink) and a client;
//   Mirror        a client, on every level: its objects are copies of the authority's;
//   MapServer     a background process of the host's PC without a player (the key -coop_mapserver, the plan's Mk);
//   WorldClient   a process that asks the world server for everything of the world: a client, a map server.
// Until a module is moved over, the old names keep their meaning: is_host() is WorldServer, is_client() is WorldClient; a
// module's "host" half of a map asks is_map_authority(), which the host's process and a map server answer.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstdint>

namespace coop
{
enum class ProcessRole : uint8_t
{
	WorldServer = 1 << 0,
	MapAuthority = 1 << 1,
	Player = 1 << 2,
	Mirror = 1 << 3,
	MapServer = 1 << 4,
	WorldClient = 1 << 5,
};

constexpr bool has(uint8_t roles, ProcessRole r) { return (roles & uint8_t(r)) != 0; }

constexpr uint8_t host_roles()
{
	return uint8_t(uint8_t(ProcessRole::WorldServer) | uint8_t(ProcessRole::MapAuthority) | uint8_t(ProcessRole::Player));
}

// The dedicated host (the key -coop_dedicated): the host's process without its player. Its actor is a ghost on a level of its own, and
// nobody leads a level for it: every level with players has a map server. test_dedicated.cpp
constexpr uint8_t dedicated_roles()
{
	return uint8_t(uint8_t(ProcessRole::WorldServer) | uint8_t(ProcessRole::MapAuthority));
}

// A client: he plays, he asks the world server and he mirrors the authority of his level. test_levels_lead.cpp
// roles_a_client_never_is_a_map_authority.
constexpr uint8_t client_roles()
{
	return uint8_t(uint8_t(ProcessRole::Player) | uint8_t(ProcessRole::WorldClient) | uint8_t(ProcessRole::Mirror));
}

// A map server: the authority of its level and, for the world, a client of the world server. Its actor is a ghost: no player.
constexpr uint8_t map_server_roles()
{
	return uint8_t(uint8_t(ProcessRole::MapAuthority) | uint8_t(ProcessRole::MapServer) | uint8_t(ProcessRole::WorldClient));
}
} // namespace coop
