// Anomaly Coop — story and client rules shared by the engine layer and the unit tests.
// Pure C++17, no engine dependencies.
#pragma once

#include <cstdint>
#include <string_view>

#include "coop_protocol.h"

namespace coop
{
namespace rules
{
// "Machine" info portions describe what ONE machine's UI or body is doing right now: a window is open,
// the character is talking or asleep. They are not group story and stay on their machine (R4 1, R6 minor):
// they are never sent, never applied when received and never counted in the info hash. Otherwise one
// player opening the PDA closes the inventory / trade / loot windows of everybody, and a client's
// `actor_is_sleeping` puts the host's story to sleep.
//   exact:  ui_pda, actor_is_sleeping, sleep_active
//   prefix: ui_talk*
//   suffix: *_wnd_open
// Case-sensitive, like the engine's shared strings. No id (nullptr or empty) is not a machine info.
inline bool is_machine_info(const char* id)
{
	if (!id || !*id)
		return false;
	const std::string_view s(id);

	constexpr std::string_view kExact[] = {"ui_pda", "actor_is_sleeping", "sleep_active"};
	for (const std::string_view e : kExact)
		if (s == e)
			return true;

	constexpr std::string_view kPrefix = "ui_talk";
	if (s.size() >= kPrefix.size() && s.substr(0, kPrefix.size()) == kPrefix)
		return true;

	constexpr std::string_view kSuffix = "_wnd_open";
	if (s.size() >= kSuffix.size() && s.substr(s.size() - kSuffix.size()) == kSuffix)
		return true;

	return false;
}

// R2 I2: a world message (host -> clients) that a client applies only in the Live phase. Before Live the client is in
// its own world or loading the host's: such a message comes from the window between Welcome and JoinRequest, or from
// a world it has left. It is dropped: what the host did before host_begin_join is in the save the client loads, what
// it does after is queued for that client until WorldLoaded (P6). The join lifecycle (Welcome, WorldBlob,
// SessionReset, ...), AvatarList (it may come before the proxies exist, F0 Task 8) and time are not world messages.
inline bool live_only(Msg type)
{
	switch (type)
	{
	case Msg::EntitySpawn:
	case Msg::EntityDestroy:
	case Msg::EntitySwitch:
	case Msg::EntitySnapshot:
	case Msg::EntityEvent:
	case Msg::EntityDied:
	case Msg::EntityParent:
	case Msg::EntityState:
	case Msg::EntityLocal:
	case Msg::ItemState:
	case Msg::ZoneState:
		return true;
	default:
		return false;
	}
}

// R4 4: what a client does while it applies a host broadcast is a reaction to it, not a request. The host made the
// change itself (a host command, entities::ClientApply) or runs the same story with its own reactions (an addon's
// info callback while story::applying()). Such a reaction is dropped instead of being forwarded to the host: forwarded,
// it would happen twice.
enum class Echo : uint8_t
{
	Forward, // a request of this player: the router may send it to the host
	Drop, // a reaction to a host broadcast: nothing goes to the host
};

inline Echo client_echo(bool story_applying, bool entities_applying)
{
	return story_applying || entities_applying ? Echo::Drop : Echo::Forward;
}
} // namespace rules
} // namespace coop
