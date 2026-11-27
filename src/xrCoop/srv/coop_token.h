// Anomaly Coop — the 16-byte tokens of the server layer: the operator's (-coop_operator) and a map server's (-coop_mapserver).
// Both travel in HelloMsg::token and on a command line as 32 hex digits. The two command lines are read by different rules,
// and each keeps its own: the operator's token may come from a person (either case, a tab after it, never all zeros), a map
// server's is written by its host with token_hex (lower case, a space after it).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace coop
{
namespace srv
{
constexpr size_t kTokenSize = 16; // HelloMsg::token (kHelloTokenSize)
using Token = std::array<uint8_t, kTokenSize>;

// All zeros is no token: a player's Hello carries zeros, and a server started without the key has none.
inline bool token_none(const uint8_t* t, size_t size = kTokenSize)
{
	uint8_t any = 0;
	for (size_t i = 0; i < size; ++i)
		any |= t[i];
	return any == 0;
}
inline bool token_none(const Token& t) { return token_none(t.data(), t.size()); }

// Constant time: how long the compare takes says nothing of how many bytes matched.
inline bool token_equal(const uint8_t* a, const uint8_t* b, size_t size = kTokenSize)
{
	uint8_t diff = 0;
	for (size_t i = 0; i < size; ++i)
		diff |= uint8_t(a[i] ^ b[i]);
	return diff == 0;
}

// Lower-case hex digits, two a byte.
inline std::string token_hex(const uint8_t* t, size_t n)
{
	static const char kDigits[] = "0123456789abcdef";
	std::string out;
	out.reserve(n * 2); // one buffer: no copy of the token is left in freed memory by a growing string
	for (size_t i = 0; i < n; ++i)
	{
		out.push_back(kDigits[t[i] >> 4]);
		out.push_back(kDigits[t[i] & 15]);
	}
	return out;
}
inline std::string token_hex(const Token& t) { return token_hex(t.data(), t.size()); }

// The value of a hex digit, -1 for anything else. `upper`: 'A'..'F' count too.
inline int hex_digit(char c, bool upper)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (upper && c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

namespace detail
{
// Exactly 32 hex digits from `p` (upper case only when `upper`); `end` says what may follow them.
template <class End> bool read_token(const char* p, bool upper, End end, Token& out)
{
	if (!p)
		return false;
	Token t{};
	for (size_t i = 0; i < t.size(); ++i)
	{
		const int hi = hex_digit(p[i * 2], upper);
		const int lo = hi < 0 ? -1 : hex_digit(p[i * 2 + 1], upper);
		if (lo < 0)
			return false;
		t[i] = uint8_t(hi << 4 | lo);
	}
	if (!end(p[t.size() * 2]))
		return false;
	out = t;
	return true;
}
} // namespace detail

// The operator's token: 32 hex digits in either case, ending at a blank (space, tab) or at the end of the text; not all zeros.
// false leaves `out` alone.
inline bool parse_operator_token(const char* p, Token& out)
{
	Token t{};
	if (!detail::read_token(p, true, [](char c) { return !c || c == ' ' || c == '\t'; }, t) || token_none(t))
		return false;
	out = t;
	return true;
}

// A map server's token: 32 lower-case hex digits (its host writes them with token_hex), ending at a space or at the end of the
// text. false leaves `out` alone.
inline bool parse_bridge_token(const char* p, Token& out)
{
	return detail::read_token(p, false, [](char c) { return !c || c == ' '; }, out);
}
} // namespace srv
} // namespace coop
