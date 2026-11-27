// Anomaly Coop — ASCII text helpers for names, ini values, command lines and console lines.
// Several sets look alike and are not the same: three blank sets, three word alphabets, a case-insensitive compare and an exact
// one. Each place keeps the set it had; the names say which one it is, so two of them are never merged by accident.
// No locale: the engine's names are ASCII, and a byte above 127 is never a letter here.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace coop
{
namespace ascii
{
// ---------------------------------------------------------------- characters

constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// The blanks between the words of one line: an ini value, a command line.
constexpr bool space_or_tab(char c) { return c == ' ' || c == '\t'; }
// ... and the CR a line read from a file with CRLF ends with.
constexpr bool space_tab_cr(char c) { return c == ' ' || c == '\t' || c == '\r'; }
// Any blank of a text that may span lines: a console line, a list value.
constexpr bool space_or_eol(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
// [a-z0-9_]: a code written as one word (world_server::detail::word).
constexpr bool lower_ident_char(char c) { return (c >= 'a' && c <= 'z') || digit(c) || c == '_'; }
// [A-Za-z0-9_]: an identifier (a console command, a section name).
constexpr bool ident_char(char c) { return lower_ident_char(c) || (c >= 'A' && c <= 'Z'); }
// [A-Za-z0-9_.-]: a file stem or a save's name (protocol char_name_valid).
constexpr bool name_char(char c) { return ident_char(c) || c == '.' || c == '-'; }

// ---------------------------------------------------------------- strings

// `s` without the blanks `blank` names at both ends.
template <class Blank> std::string_view trim(std::string_view s, Blank blank)
{
	while (!s.empty() && blank(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && blank(s.back()))
		s.remove_suffix(1);
	return s;
}

inline std::string lowered(std::string_view s)
{
	std::string out(s);
	for (char& c : out)
		c = lower(c);
	return out;
}

// Equal, ASCII case aside (how the engine compares section, key and partition names: stricmp).
inline bool iequals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (lower(a[i]) != lower(b[i]))
			return false;
	return true;
}

inline bool istarts_with(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

// Exactly equal. Not strcmp: xrCore marks it deprecated (C4995) in every engine file that includes these headers.
inline bool equal(const char* a, const char* b) { return std::string_view(a) == std::string_view(b); }

// ---------------------------------------------------------------- numbers and keys

// Any digit count: what bounds the number is `max` alone.
constexpr size_t kAnyDigits = ~size_t(0);

// `s` is a plain decimal number within [0, max]: digits only (no sign, no blank), at most `max_digits` of them, a leading zero
// only when `leading_zero` allows it ("0" itself always). `out` is left alone on false.
inline bool parse_decimal(std::string_view s, uint64_t max, uint64_t& out, size_t max_digits, bool leading_zero)
{
	if (s.empty() || s.size() > max_digits || (!leading_zero && s.size() > 1 && s[0] == '0'))
		return false;
	uint64_t v = 0;
	for (const char c : s)
	{
		if (!digit(c))
			return false;
		const uint64_t d = uint64_t(c - '0');
		if (v > (~uint64_t(0) - d) / 10)
			return false;
		v = v * 10 + d;
	}
	if (v > max)
		return false;
	out = v;
	return true;
}

// A u32 field of a text file (a journal, a parent's pipe line): at most 10 digits, leading zeros allowed.
inline bool parse_u32(std::string_view s, uint32_t max, uint32_t& out)
{
	uint64_t v = 0;
	if (!parse_decimal(s, max, v, 10, true))
		return false;
	out = uint32_t(v);
	return true;
}

// `key` as a whole word of a command line: at its start or after a space or a tab, and followed by one or by the end. The text
// right after the key, or nullptr. A longer key with the same beginning is not this one, and every occurrence is tried.
inline const char* find_key(const char* params, const char* key)
{
	const size_t n = std::strlen(key);
	for (const char* p = params; p && (p = std::strstr(p, key)) != nullptr; p += n)
		if ((p == params || space_or_tab(p[-1])) && (p[n] == '\0' || space_or_tab(p[n])))
			return p + n;
	return nullptr;
}
} // namespace ascii
} // namespace coop
