// Anomaly Coop: an invite accepted in Steam while the game runs. The launcher's Steam helper enters the lobby and opens the
// tunnel on loopback, then leaves its address in a file the game was told about (RAYLINK_STEAM_JOIN). Nothing but a loopback
// address is taken from that file.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace coop
{
namespace steam_join
{
constexpr const char* kLoopback = "127.0.0.1";

// "127.0.0.1:<port>", line ends allowed after it. False for anything else.
inline bool parse_tunnel(const char* text, uint16_t& port)
{
	const size_t prefix = strlen(kLoopback);
	if (!text || strncmp(text, kLoopback, prefix) != 0 || text[prefix] != ':')
		return false;
	const char* digits = text + prefix + 1;
	size_t count = 0;
	unsigned long value = 0;
	while (digits[count] >= '0' && digits[count] <= '9' && count < 6)
		value = value * 10 + static_cast<unsigned long>(digits[count++] - '0');
	if (count == 0 || value == 0 || value > 65535)
		return false;
	for (const char* rest = digits + count; *rest; ++rest)
		if (*rest != '\r' && *rest != '\n')
			return false;
	port = static_cast<uint16_t>(value);
	return true;
}
} // namespace steam_join
} // namespace coop
