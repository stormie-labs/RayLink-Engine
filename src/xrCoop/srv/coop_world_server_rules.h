// The world server's side of its pipe, the pure part. The world server is the dedicated host (coop_dedicated_rules.h) started by
// the launcher with -coop_hidden: no window, standard input and output are pipes of the parent. This header holds what the game
// writes on that pipe (the parent's half is coop_world_parent_rules.h):
//   - Record, format, parse   the @rl lines of its standard output (grammar below);
//   - cp1251_to_utf8, output_line   the game's text as the pipe carries it;
//   - OutputQueue             the lines between the game's threads and a writer thread (coop_line_writer.h): bounded, never blocks the game.
// Pure C++17, header-only, no engine dependencies.
//
// ---------------------------------------------------------------- the standard output of the world server
//
// UTF-8, one line per write, each line ends with "\n" (a reader also takes "\r\n"). Two kinds of lines:
//   - "@rl ..." at column 0: for the parent, never shown as they are;
//   - every other line: a line of the game's log that coop_console_log lets through (log_shown, coop_dedicated_rules.h), as the window of the
//     dedicated host would show it. "!", "FATAL ERROR", "[error]" at the start mark an error.
//
// The grammar of an @rl line (one blank between the parts):
//   line     = "@rl" SP event [SP rest]
//   event    = 1*( "a"-"z" / "0"-"9" / "_" )
//   field    = key "=" value                 key: like event; value: bare or quoted
//   bare     = 1*( any byte but SP, TAB, '"' )
//   quoted   = '"' *( any byte but '"' and '\' / '\"' / '\\' ) '"'
//   code     = like event
//   text     = the rest of the line, as it is (may hold blanks and '"'), may be empty
// The events:
//   @rl phase <starting|world|session|ready|saving|down>
//         starting: the process runs (before the engine's core); world: the world's level is being made (-coop_new / -coop_load);
//         session: the world is up, the session is being opened; ready: the session is open (after "@rl ready"); saving: the quit began, the
//         world is being saved; down: saved, the process exits.
//   @rl ready port=<1..65535>                   the session listens on that port (always right before "@rl phase ready")
//   @rl wait <code> [text]                      why the world is not up yet (dedicated::wait_code; text = wait_text)
//   @rl players n=<n>                           the players of the session changed (the server itself and map servers are not counted)
//   @rl mapserver level=<name> state=<loading|ready|serving|gone>   a map server of a level changed its state
//   @rl error <code> [text]                     the start ended without a session: gave_up, bad_args, no_save
// A reader:
//   - takes the fields in any order and ignores keys it does not know;
//   - takes an event it does not know as a valid line (Event::Unknown, its name kept): a newer server never breaks an older parent;
//   - takes an unknown phase or state value as Phase::Unknown / MapState::Unknown;
//   - refuses (parse false) a line that is not "@rl ..." and a known event without its required field.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../coop_ids.h"
#include "../coop_text.h"

namespace coop
{
namespace world_server
{
constexpr char kPrefix[] = "@rl ";

enum class Event : uint8_t
{
	Unknown, // a valid @rl line of an event this build does not know: Record::name has it
	Phase,
	Ready,
	Wait,
	Players,
	MapServer,
	Error,
};

enum class Phase : uint8_t
{
	Unknown,
	Starting,
	World,
	Session,
	Ready,
	Saving,
	Down,
};

enum class MapState : uint8_t
{
	Unknown,
	Loading,
	Ready,
	Serving,
	Gone,
};

struct Record
{
	Event event = Event::Unknown;
	std::string name; // the event as the line names it
	Phase phase = Phase::Unknown; // Phase
	uint16_t port = 0; // Ready
	std::string code; // Wait, Error
	std::string text; // Wait, Error: may be empty
	uint32_t players = 0; // Players
	std::string level; // MapServer
	MapState state = MapState::Unknown; // MapServer

	static Record of_phase(Phase p)
	{
		Record r;
		r.event = Event::Phase;
		r.phase = p;
		return r;
	}
	static Record of_ready(uint16_t port)
	{
		Record r;
		r.event = Event::Ready;
		r.port = port;
		return r;
	}
	static Record of_wait(std::string code, std::string text)
	{
		Record r;
		r.event = Event::Wait;
		r.code = std::move(code);
		r.text = std::move(text);
		return r;
	}
	static Record of_players(uint32_t n)
	{
		Record r;
		r.event = Event::Players;
		r.players = n;
		return r;
	}
	static Record of_mapserver(std::string level, MapState s)
	{
		Record r;
		r.event = Event::MapServer;
		r.level = std::move(level);
		r.state = s;
		return r;
	}
	static Record of_error(std::string code, std::string text)
	{
		Record r;
		r.event = Event::Error;
		r.code = std::move(code);
		r.text = std::move(text);
		return r;
	}
};

inline const char* phase_name(Phase p)
{
	switch (p)
	{
	case Phase::Starting: return "starting";
	case Phase::World: return "world";
	case Phase::Session: return "session";
	case Phase::Ready: return "ready";
	case Phase::Saving: return "saving";
	case Phase::Down: return "down";
	case Phase::Unknown: break;
	}
	return "unknown";
}

inline const char* state_name(MapState s)
{
	switch (s)
	{
	case MapState::Loading: return "loading";
	case MapState::Ready: return "ready";
	case MapState::Serving: return "serving";
	case MapState::Gone: return "gone";
	case MapState::Unknown: break;
	}
	return "unknown";
}

namespace detail
{
// A code as a word: lower case, anything else '_'; never empty.
inline std::string word(const std::string& s)
{
	std::string out;
	for (char c : s)
		out.push_back(c >= 'A' && c <= 'Z' ? ascii::lower(c) : (ascii::lower_ident_char(c) ? c : '_'));
	return out.empty() ? std::string("unknown") : out;
}

// A text on one line: CR, LF, TAB become blanks.
inline std::string flat(const std::string& s)
{
	std::string out = s;
	for (char& c : out)
		if (c == '\r' || c == '\n' || c == '\t')
			c = ' ';
	return out;
}

inline std::string value(const std::string& v)
{
	bool plain = !v.empty();
	for (char c : v)
		plain = plain && c != ' ' && c != '\t' && c != '"' && c != '\\' && c != '\r' && c != '\n';
	if (plain)
		return v;
	std::string out = "\"";
	for (char c : v)
	{
		if (c == '"' || c == '\\')
			out.push_back('\\');
		out.push_back(c == '\r' || c == '\n' || c == '\t' ? ' ' : c);
	}
	return out + "\"";
}

// The fields of `s` from `at`: calls f(key, value) for each; false on a broken one.
template <class F> bool fields(const std::string& s, size_t at, F f)
{
	while (at < s.size())
	{
		if (s[at] == ' ' || s[at] == '\t')
		{
			++at;
			continue;
		}
		const size_t eq = s.find('=', at);
		if (eq == std::string::npos || eq == at)
			return false;
		const std::string key = s.substr(at, eq - at);
		for (char c : key)
			if (!ascii::lower_ident_char(c))
				return false;
		std::string v;
		at = eq + 1;
		if (at < s.size() && s[at] == '"')
		{
			bool closed = false;
			for (++at; at < s.size(); ++at)
			{
				if (s[at] == '\\' && at + 1 < s.size())
					v.push_back(s[++at]);
				else if (s[at] == '"')
				{
					closed = true;
					++at;
					break;
				}
				else
					v.push_back(s[at]);
			}
			if (!closed)
				return false;
		}
		else
			while (at < s.size() && s[at] != ' ' && s[at] != '\t')
				v.push_back(s[at++]);
		f(key, v);
	}
	return true;
}
} // namespace detail

// The line, without its "\n".
inline std::string format(const Record& r)
{
	std::string out = "@rl ";
	switch (r.event)
	{
	case Event::Phase: return out + "phase " + phase_name(r.phase);
	case Event::Ready: return out + "ready port=" + std::to_string(r.port);
	case Event::Players: return out + "players n=" + std::to_string(r.players);
	case Event::MapServer: return out + "mapserver level=" + detail::value(r.level) + " state=" + state_name(r.state);
	case Event::Wait:
	case Event::Error:
	{
		out += r.event == Event::Wait ? "wait " : "error ";
		out += detail::word(r.code);
		const std::string text = detail::flat(r.text);
		return text.empty() ? out : out + " " + text;
	}
	case Event::Unknown: break;
	}
	return out + detail::word(r.name);
}

// "@rl " at column 0.
inline bool is_rl(const std::string& line) { return line.compare(0, 4, kPrefix) == 0; }

// False: not an @rl line, or a known event without what it needs. A CR at the end (a "\r\n" pipe) is not part of the line.
inline bool parse(std::string line, Record& out)
{
	if (!line.empty() && line.back() == '\r')
		line.pop_back();
	if (!is_rl(line))
		return false;
	size_t at = 4;
	size_t end = at;
	while (end < line.size() && ascii::lower_ident_char(line[end]))
		++end;
	if (end == at || (end < line.size() && line[end] != ' '))
		return false;
	Record r;
	r.name = line.substr(at, end - at);
	at = end < line.size() ? end + 1 : end;
	const std::string rest = line.substr(at);
	if (r.name == "phase")
	{
		r.event = Event::Phase;
		const std::string p = rest.substr(0, rest.find(' '));
		if (p.empty())
			return false;
		for (Phase k : {Phase::Starting, Phase::World, Phase::Session, Phase::Ready, Phase::Saving, Phase::Down})
			if (p == phase_name(k))
				r.phase = k;
	}
	else if (r.name == "wait" || r.name == "error")
	{
		r.event = r.name == "wait" ? Event::Wait : Event::Error;
		const size_t blank = rest.find(' ');
		r.code = rest.substr(0, blank);
		if (r.code.empty())
			return false;
		r.text = blank == std::string::npos ? std::string() : rest.substr(blank + 1);
	}
	else if (r.name == "ready" || r.name == "players" || r.name == "mapserver")
	{
		bool has_a = false, has_b = false, ok = true;
		const bool fine = detail::fields(rest, 0, [&](const std::string& key, const std::string& v) {
			uint32_t n = 0;
			if (r.name == "ready" && key == "port")
			{
				has_a = true;
				ok = ok && ascii::parse_u32(v, 65535, n) && n > 0;
				r.port = uint16_t(n);
			}
			else if (r.name == "players" && key == "n")
			{
				has_a = true;
				ok = ok && ascii::parse_u32(v, 0xFFFFFFFFu, n);
				r.players = n;
			}
			else if (r.name == "mapserver" && key == "level")
			{
				has_a = true;
				r.level = v;
			}
			else if (r.name == "mapserver" && key == "state")
			{
				has_b = true;
				for (MapState k : {MapState::Loading, MapState::Ready, MapState::Serving, MapState::Gone})
					if (v == state_name(k))
						r.state = k;
			}
		});
		if (!fine || !ok || !has_a || (r.name == "mapserver" && !has_b))
			return false;
		r.event = r.name == "ready" ? Event::Ready : (r.name == "players" ? Event::Players : Event::MapServer);
	}
	else
		r.event = Event::Unknown;
	out = std::move(r);
	return true;
}

// ---------------------------------------------------------------- the game's text on the pipe

// The game's text is cp1251, the pipe is UTF-8. 0x98, the one byte cp1251 leaves out, is U+FFFD.
inline std::string cp1251_to_utf8(const std::string& s)
{
	static const uint16_t kHigh[64] = {0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A,
	    0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C,
	    0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE,
	    0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s)
	{
		uint32_t u = c;
		if (c >= 0xC0)
			u = 0x0410 + (c - 0xC0);
		else if (c >= 0x80)
			u = kHigh[c - 0x80];
		if (u < 0x80)
			out.push_back(char(u));
		else if (u < 0x800)
		{
			out.push_back(char(0xC0 | (u >> 6)));
			out.push_back(char(0x80 | (u & 0x3F)));
		}
		else
		{
			out.push_back(char(0xE0 | (u >> 12)));
			out.push_back(char(0x80 | ((u >> 6) & 0x3F)));
			out.push_back(char(0x80 | (u & 0x3F)));
		}
	}
	return out;
}

// What one write puts on the pipe: the line in UTF-8, its CR and LF blanks, and one "\n".
inline std::string output_line(const std::string& game_text)
{
	std::string out = cp1251_to_utf8(game_text);
	for (char& c : out)
		if (c == '\r' || c == '\n')
			c = ' ';
	return out + "\n";
}

// ---------------------------------------------------------------- between the game and the pipe

// The game's threads push, one writer thread pops and writes (a write to a full pipe blocks the writer, never the game). When the parent
// does not read, the queue fills: the oldest line of the same kind goes (the newest state is what a parent wants), and the next pop says
// how many went. @rl lines have their own bound, so a flood of log lines never pushes a phase out. The note names the reader that did
// not take the lines (`sink`) and why (`why`): the same queue serves the pipe and the console window (coop_line_writer.h).
class OutputQueue
{
public:
	static constexpr size_t kLogLines = 4096;
	static constexpr size_t kRlLines = 512;

	explicit OutputQueue(size_t log_lines = kLogLines, size_t rl_lines = kRlLines, std::string sink = "stdout",
	    std::string why = "the parent did not read")
	    : m_max_log(log_lines), m_max_rl(rl_lines), m_sink(std::move(sink)), m_why(std::move(why))
	{
	}

	void push(std::string line, bool rl)
	{
		size_t& count = rl ? m_rl : m_log;
		if (count >= (rl ? m_max_rl : m_max_log))
		{
			for (auto it = m_lines.begin(); it != m_lines.end(); ++it)
				if (it->second == rl)
				{
					m_lines.erase(it);
					--count;
					++m_dropped;
					break;
				}
		}
		m_lines.emplace_back(std::move(line), rl);
		++count;
	}

	bool pop(std::string& out)
	{
		if (m_dropped)
		{
			out = "[coop] " + m_sink + ": " + std::to_string(m_dropped) + " line(s) of the log dropped, " + m_why;
			m_dropped = 0;
			return true;
		}
		if (m_lines.empty())
			return false;
		out = std::move(m_lines.front().first);
		--(m_lines.front().second ? m_rl : m_log);
		m_lines.pop_front();
		return true;
	}

	bool empty() const { return m_lines.empty() && !m_dropped; }
	uint32_t dropped() const { return m_dropped; }

private:
	std::deque<std::pair<std::string, bool>> m_lines;
	size_t m_max_log;
	size_t m_max_rl;
	std::string m_sink;
	std::string m_why;
	size_t m_log = 0;
	size_t m_rl = 0;
	uint32_t m_dropped = 0;
};
} // namespace world_server
} // namespace coop
