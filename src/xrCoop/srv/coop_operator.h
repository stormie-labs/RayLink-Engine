// Anomaly Coop — the operator of a world server (docs/plans/2026-11-12-split-levels-native.md §2).
// The world server's own seat plays no character; the game of the world's creator on the same PC holds the host's rights
// instead. The process that starts the server makes a one-time 128-bit token and gives it to the server on its command
// line (-coop_operator <32 hex digits>) and to the creator's game, which presents it in Hello::token with role Player.
//   - operator_verdict  who of the players that say hello becomes the operator;
//   - allowed           which console lines the operator may send (Msg::OperatorCommand);
//   - command_verdict   what the server does with one OperatorCommand.
// Pure C++17, header-only, no engine dependencies: used by the session (coop_session.cpp), by xrGame and by the unit-test exe.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop
{
namespace op
{
using Token = std::array<uint8_t, 16>; // the size of HelloMsg::token (kHelloTokenSize)
using Key = std::array<uint8_t, 16>; // the same type as PlayerKey

constexpr const char* kKey = "-coop_operator";
// The longest console line an operator may send: the same as debug::kMaxConsoleLine (coop_debug_rules.h).
constexpr size_t kMaxLine = 1024;

// All zeros is no token: a player's Hello carries zeros, and a server started without the key has none.
inline bool token_none(const uint8_t* t, size_t size = 16)
{
	uint8_t any = 0;
	for (size_t i = 0; i < size; ++i)
		any |= t[i];
	return any == 0;
}
inline bool token_none(const Token& t) { return token_none(t.data(), t.size()); }

// Constant time: how long the compare takes says nothing of how many bytes matched.
inline bool token_equal(const uint8_t* a, const uint8_t* b, size_t size = 16)
{
	uint8_t diff = 0;
	for (size_t i = 0; i < size; ++i)
		diff |= uint8_t(a[i] ^ b[i]);
	return diff == 0;
}

namespace detail
{
inline int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}
} // namespace detail

// Exactly 32 hex digits (either case), ending at a blank or at the end of the text; not all zeros. false leaves `out` alone.
inline bool parse_token(const char* p, Token& out)
{
	if (!p)
		return false;
	Token t{};
	for (size_t i = 0; i < t.size(); ++i)
	{
		const int hi = detail::hex_digit(p[i * 2]);
		const int lo = hi < 0 ? -1 : detail::hex_digit(p[i * 2 + 1]);
		if (lo < 0)
			return false;
		t[i] = uint8_t(hi << 4 | lo);
	}
	const char end = p[t.size() * 2];
	if ((end && end != ' ' && end != '\t') || token_none(t))
		return false;
	out = t;
	return true;
}

// The token of `-coop_operator <hex>` in a command line: the key as a whole word (not a prefix of a longer one), one or more
// blanks, the token. false if the key is not there or its token is not valid, and if the key is there twice: the engine reads its
// command line as raw text, quotes mean nothing to it, so a value that reads as the key (a save's name) must not choose the operator
// (security review 2026-11-12).
inline bool find_token(const char* cmdline, Token& out)
{
	if (!cmdline)
		return false;
	const size_t klen = std::strlen(kKey);
	const char* value = nullptr;
	for (const char* p = std::strstr(cmdline, kKey); p; p = std::strstr(p + 1, kKey))
	{
		const bool starts = p == cmdline || p[-1] == ' ' || p[-1] == '\t';
		const char after = p[klen];
		if (!starts || (after != ' ' && after != '\t'))
			continue;
		if (value)
			return false;
		value = p + klen;
		while (*value == ' ' || *value == '\t')
			++value;
	}
	return value && parse_token(value, out);
}

// Is `host` this PC, written as a literal 127.a.b.c? The creator's game presents its token only there. The text comes from an
// invite, a console line or Lua and is resolved by name later: "127.0.0.1.evil.example" begins with "127." and is another
// machine (security review 2026-11-12). Four decimal octets 0..255 with no leading zero, the first one 127, nothing else.
inline bool loopback_literal(const char* host)
{
	if (!host)
		return false;
	const char* p = host;
	for (int octet = 0; octet < 4; ++octet)
	{
		if (*p < '0' || *p > '9')
			return false;
		if (*p == '0' && p[1] >= '0' && p[1] <= '9')
			return false; // a leading zero: some resolvers read it as octal
		int value = 0;
		int digits = 0;
		while (*p >= '0' && *p <= '9')
		{
			value = value * 10 + (*p - '0');
			if (++digits > 3)
				return false;
			++p;
		}
		if (value > 255 || (octet == 0 && value != 127))
			return false;
		if (octet < 3)
		{
			if (*p != '.')
				return false;
			++p;
		}
	}
	return *p == 0;
}

// A command line for the log: the value of every -coop_operator is replaced, and every word of exactly 32 hex digits (the
// one-time token of a map server, -coop_mapserver k level port <token>, is one). The engine prints its command line at the
// start and at every level (x_ray.cpp, Level.cpp), and logs go into reports.
inline std::string redact(const char* cmdline)
{
	std::string out = cmdline ? cmdline : "";
	for (size_t from = 0; from < out.size();)
	{
		while (from < out.size() && (out[from] == ' ' || out[from] == '\t'))
			++from;
		size_t end = from;
		while (end < out.size() && out[end] != ' ' && out[end] != '\t')
			++end;
		bool hex = end - from == 32;
		for (size_t i = from; hex && i < end; ++i)
			hex = detail::hex_digit(out[i]) >= 0;
		if (hex)
		{
			out.replace(from, 32, "<hidden>");
			end = from + 8;
		}
		from = end;
	}
	const size_t klen = std::strlen(kKey);
	size_t at = 0;
	while ((at = out.find(kKey, at)) != std::string::npos)
	{
		const bool starts = at == 0 || out[at - 1] == ' ' || out[at - 1] == '\t';
		size_t q = at + klen;
		if (!starts || q >= out.size() || (out[q] != ' ' && out[q] != '\t'))
		{
			at = q;
			continue;
		}
		while (q < out.size() && (out[q] == ' ' || out[q] == '\t'))
			++q;
		size_t end = q;
		while (end < out.size() && out[end] != ' ' && out[end] != '\t')
			++end;
		if (end > q)
			out.replace(q, end - q, "<hidden>");
		at = q;
	}
	return out;
}

// What a player's Hello makes of him (plan §2.2 step 4). Only Operator gives the host's rights; every other verdict lets him
// in as an ordinary player, and his answer (the Welcome) is the same for all of them: a remote client never learns whether a
// token exists or matched.
enum class Verdict : uint8_t
{
	NotLoopback, // not from this PC: the token is not even looked at
	NoMatch, // no token, or not the server's
	Operator, // the token matched, and the key is the one it is bound to (or it was not bound yet: it is now)
	OtherKey, // the token matched, but it is bound to another player's key already
};

// loopback: the Hello came over 127.x.x.x; token_matches: it carries the server's token (token_equal, not all zeros);
// bound_key: the key the token is bound to, nullptr while nobody used it; hello_key: the key of this Hello.
inline Verdict operator_verdict(bool loopback, bool token_matches, const Key* bound_key, const Key& hello_key)
{
	if (!loopback)
		return Verdict::NotLoopback;
	if (!token_matches)
		return Verdict::NoMatch;
	if (bound_key && !token_equal(bound_key->data(), hello_key.data(), hello_key.size()))
		return Verdict::OtherKey;
	return Verdict::Operator;
}

inline const char* verdict_name(Verdict v)
{
	switch (v)
	{
	case Verdict::NotLoopback: return "not_loopback";
	case Verdict::NoMatch: return "no_match";
	case Verdict::Operator: return "operator";
	case Verdict::OtherKey: return "other_key";
	}
	return "?";
}

// ---------------------------------------------------------------- the console lines of the operator

namespace detail
{
// A word of a command: what follows the command name, split at single blanks.
inline bool key_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '|' ||
	    c == '/' || c == '-';
}
inline size_t count_words(const std::string& rest)
{
	size_t n = 0;
	bool in = false;
	for (char c : rest)
	{
		if (c == ' ')
			in = false;
		else if (!in)
		{
			in = true;
			++n;
		}
	}
	return n;
}
} // namespace detail

// True if the operator may run `line` on the server (plan §2.3): one of
//   coop_kick <args>, coop_time [args], coop_restore [args], coop_autosave_now, coop_players, coop_status,
//   coop_debug on|off, coop_dbg [args], /go, say <text>, coop_game_set <key> <value>.
// Never quit, coop_world, coop_host or any other line of the engine's console. The line itself: 1..kMaxLine bytes, no control
// characters (below 0x20, DEL) and no ';', no blank at its start or end, single blanks between the words.
inline bool allowed(const std::string& line)
{
	if (line.empty() || line.size() > kMaxLine || line.front() == ' ' || line.back() == ' ')
		return false;
	for (size_t i = 0; i < line.size(); ++i)
	{
		const unsigned char c = static_cast<unsigned char>(line[i]);
		if (c < 0x20 || c == 0x7F || c == ';')
			return false;
		if (c == ' ' && i + 1 < line.size() && line[i + 1] == ' ')
			return false;
	}
	const size_t blank = line.find(' ');
	const std::string cmd = line.substr(0, blank);
	const std::string rest = blank == std::string::npos ? std::string() : line.substr(blank + 1);
	const size_t words = detail::count_words(rest);

	if (cmd == "coop_autosave_now" || cmd == "coop_players" || cmd == "coop_status" || cmd == "/go")
		return words == 0;
	if (cmd == "coop_time" || cmd == "coop_restore")
		return true;
	if (cmd == "coop_dbg")
	{
		// The debug command is an id of coop_debug_cmds.script: letters, digits, '_'. "lua" (its one command of class "operator",
		// for the server's own console) never comes over the network, in any letter case: coop_debug_server lowercases the id.
		// The server checks the class again when it runs the line (security review 2026-11-12).
		if (words == 0)
			return true;
		const std::string id = rest.substr(0, rest.find(' '));
		std::string lower;
		for (char c : id)
		{
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
				return false;
			lower += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
		}
		return lower != "lua";
	}
	if (cmd == "coop_kick" || cmd == "say")
		return words >= 1;
	if (cmd == "coop_debug")
		return rest == "on" || rest == "off";
	if (cmd == "coop_game_set")
	{
		if (words < 2)
			return false;
		const size_t kend = rest.find(' ');
		if (kend == 0 || kend > 128)
			return false;
		for (size_t i = 0; i < kend; ++i)
			if (!detail::key_char(rest[i]))
				return false;
		return true;
	}
	return false;
}

// What the server does with one Msg::OperatorCommand that passed the rate limit.
// ---------------------------------------------------------------- what the server does with a command (security review 2026-11-12)

// coop_kick <id|nick>: which player it means. `rows`: the session's players only (the caller leaves out the map servers' bridges and a
// seat that plays no character). An id that is a player's wins over a nick that reads as the same number; an id is written as decimal
// digits without a leading zero. A nick is compared without the case of ASCII letters; a nick two players have is no target.
struct KickRow
{
	uint32_t id;
	std::string nick;
};
constexpr int kKickNobody = -1;
constexpr int kKickAmbiguous = -2;

inline int kick_pick(const std::vector<KickRow>& rows, const std::string& who)
{
	if (who.empty())
		return kKickNobody;
	bool number = who.size() <= 10 && !(who.size() > 1 && who[0] == '0');
	uint64_t id = 0;
	for (const char c : who)
	{
		if (c < '0' || c > '9')
		{
			number = false;
			break;
		}
		id = id * 10 + uint64_t(c - '0');
	}
	if (number)
		for (size_t i = 0; i < rows.size(); ++i)
			if (rows[i].id == id)
				return int(i);
	const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
	int found = kKickNobody;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		const std::string& nick = rows[i].nick;
		bool same = nick.size() == who.size();
		for (size_t k = 0; same && k < nick.size(); ++k)
			same = lower(nick[k]) == lower(who[k]);
		if (!same)
			continue;
		if (found != kKickNobody)
			return kKickAmbiguous;
		found = int(i);
	}
	return found;
}

// A value of a script event's payload as coop_main.encode writes it: '%', ';' and '=' as %XX.
inline std::string escape_value(const std::string& s)
{
	static const char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(s.size() + 8);
	for (const char c : s)
	{
		if (c == '%' || c == ';' || c == '=')
		{
			out += '%';
			out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
			out += kHex[static_cast<unsigned char>(c) & 0xF];
		}
		else
			out += c;
	}
	return out;
}

// The text of the answer to an operator's command as it is sent: the lines escaped and joined by "\n", at most `cap` bytes counted as
// sent (an escaped character is three). The first line that does not fit is left out whole, and what follows it: an event longer than
// the script events' limit would be dropped by the client without a word.
inline std::string answer_text(const std::vector<std::string>& lines, size_t cap)
{
	std::string text;
	for (const std::string& l : lines)
	{
		const std::string e = escape_value(l);
		if (text.size() + e.size() + (text.empty() ? 0 : 1) > cap)
			break;
		if (!text.empty())
			text += '\n';
		text += e;
	}
	return text;
}

enum class Command : uint8_t
{
	Run, // hand it to the console of the server (stage 3: dedicated::run_for)
	NotOperator, // the sender holds no host's rights: refused, counted
	TooLong, // over kMaxLine: refused, counted
	NotAllowed, // not on the list: refused, counted
};

inline Command command_verdict(bool from_operator, const std::string& line)
{
	if (!from_operator)
		return Command::NotOperator;
	if (line.size() > kMaxLine)
		return Command::TooLong;
	return allowed(line) ? Command::Run : Command::NotAllowed;
}

inline const char* command_name(Command c)
{
	switch (c)
	{
	case Command::Run: return "run";
	case Command::NotOperator: return "not_operator";
	case Command::TooLong: return "too_long";
	case Command::NotAllowed: return "not_allowed";
	}
	return "?";
}
} // namespace op
} // namespace coop
