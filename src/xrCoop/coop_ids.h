// Anomaly Coop — the "nothing here" marks and the shared constants every module names the same way.
// A module that means something narrower keeps its own name as an alias (kNoBag = kNoId); it never spells the value again.
// Pure C++17, header-only, no dependencies: the engine includes it where the protocol is not wanted (dedicated rules).
#pragma once

#include <cstdint>

namespace coop
{
constexpr uint16_t kNoId = 0xffff; // no object: the stock invalid id (ALife never gives it out)
constexpr uint16_t kNoLevel = 0xffff; // no level: a game graph level id nobody has (loading, choosing a character)
constexpr uint16_t kDefaultPort = 27960; // the host's UDP port when nothing names one
} // namespace coop
