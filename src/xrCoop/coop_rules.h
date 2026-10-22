// Anomaly Coop — story rules shared by the engine layer and the unit tests.
// Pure C++17, no engine dependencies.
#pragma once

#include <string_view>

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
} // namespace rules
} // namespace coop
