// The parent of the world server, the pure part: what the process that starts a world server does with it, as far as it is not
// the OS. The launcher's port of it (rl-core) follows this header; the unit tests pin it. The engine itself only writes the
// server's side (coop_world_server_rules.h).
//   - world_dir               the server's data folder ($app_data_root$), next to the game's;
//   - rewrite_saves           its fsgame's $game_saves$ pointed at the creator's savedgames: one list of worlds for the game and the launcher;
//   - command_line            its command line; the token is never in a line meant for a log (redact = true);
//   - filter_environment, port_candidates, LineSplitter, utf8_to_cp1251, world_fsgame;
//   - Watch, summary          what the lines of its pipe say about it.
// Pure C++17, header-only, no engine dependencies.
#pragma once

#include "coop_world_server_rules.h"

namespace coop
{
namespace world_server
{
// ---------------------------------------------------------------- the folders

namespace detail
{
// Every '/' a '\', the separators at the end gone.
inline std::string folder(std::string dir)
{
	for (char& c : dir)
		if (c == '/')
			c = '\\';
	while (!dir.empty() && dir.back() == '\\')
		dir.pop_back();
	return dir;
}
} // namespace detail

// The world server's data folder ($app_data_root$): "<the game's folder>_world\" next to it. D:\G\appdata\ -> D:\G\appdata_world\,
// D:/G/appdata -> D:\G\appdata_world\. Separators come out as '\', one at the end. Empty for an empty folder, a bare separator and a
// drive's root ("D:\": "D:_world\" would be a folder relative to the drive's current one).
inline std::string world_dir(const std::string& game_dir)
{
	const std::string dir = detail::folder(game_dir);
	if (dir.empty() || dir.back() == ':')
		return std::string();
	return dir + "_world\\";
}

// The world server's fsgame.ltx with its $game_saves$ in the creator's savedgames (`saves_dir`, absolute): the line's root becomes the folder
// above it and its add the folder's own name ("D:\G\appdata\| savedgames\"), so the engine's root + add is that folder and the fields after
// it (the mask of the saves) stay. Every other line is as it was. False, `out` untouched: no such line, the line has fewer than three fields
// (the engine asserts on it), or `saves_dir` is relative, empty, or holds '|' or '=' (they split the line for the engine's reader).
inline bool rewrite_saves(const std::string& text, const std::string& saves_dir, std::string& out)
{
	static const char kKeyName[] = "$game_saves$";
	const size_t name_len = sizeof(kKeyName) - 1;
	const std::string dir = detail::folder(saves_dir);
	const size_t slash = dir.rfind('\\');
	if (dir.empty() || slash == std::string::npos || slash + 1 >= dir.size() || dir.find_first_of("|=") != std::string::npos)
		return false;
	const std::string root = dir.substr(0, slash + 1);
	const std::string add = dir.substr(slash + 1) + "\\";
	for (size_t line = 0; line < text.size();)
	{
		size_t stop = text.find_first_of("\r\n", line);
		if (stop == std::string::npos)
			stop = text.size();
		size_t first = line;
		while (first < stop && (text[first] == ' ' || text[first] == '\t'))
			++first;
		if (text[line] != ';' && text.compare(first, name_len, kKeyName) == 0)
		{
			size_t eq = first + name_len;
			while (eq < stop && (text[eq] == ' ' || text[eq] == '\t'))
				++eq;
			if (eq < stop && text[eq] == '=')
			{
				const size_t bar1 = text.find('|', eq);
				const size_t bar2 = bar1 < stop ? text.find('|', bar1 + 1) : std::string::npos;
				if (bar2 == std::string::npos || bar2 >= stop)
					return false;
				size_t begin = bar2 + 1;
				while (begin < stop && (text[begin] == ' ' || text[begin] == '\t'))
					++begin;
				const size_t bar3 = text.find('|', begin);
				const size_t bar4 = bar3 < stop ? text.find('|', bar3 + 1) : std::string::npos;
				const size_t end = bar3 >= stop ? stop : (bar4 < stop ? bar4 : stop);
				out = text.substr(0, begin) + root + "| " + add + text.substr(end);
				return true;
			}
		}
		line = stop;
		while (line < text.size() && (text[line] == '\r' || text[line] == '\n'))
			++line;
	}
	return false;
}

// ---------------------------------------------------------------- the process

constexpr char kSteamEnvPrefix[] = "RAYLINK_STEAM_"; // the launcher's Steam mode (coop_manager.cpp steam_join_waits): never a server's
constexpr size_t kKeepLines = 200; // the log lines the parent keeps (the error screen shows the last 20)

// One argument in double quotes as CommandLineToArgvW reads it back: a quote inside gets a backslash, the backslashes before a quote (and
// before the closing one) are doubled. For the exe's path, which may hold blanks.
inline std::string quote_arg(const std::string& s)
{
	std::string out = "\"";
	size_t slashes = 0;
	for (char c : s)
	{
		if (c == '\\')
		{
			++slashes;
			continue;
		}
		out.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
		slashes = 0;
		out.push_back(c);
	}
	out.append(slashes * 2, '\\');
	return out + "\"";
}

// A value the engine reads up to the next blank (-fsltx: x_ray.cpp sscanf "%[^ ]"; -coop_stage): no blank, no quote, no control
// character, not empty, not starting with '-' (it would read as the next key), and without "-$": the engine runs what follows "-$"
// anywhere in its command line as a console command (Level_start.cpp).
inline bool bare_arg_ok(const std::string& s)
{
	if (s.empty() || s[0] == '-' || s.find("-$") != std::string::npos)
		return false;
	for (char c : s)
		if (static_cast<unsigned char>(c) < 0x20 || c == ' ' || c == '"' || c == 0x7F)
			return false;
	return true;
}

// -coop_nick takes one word (coop_manager.cpp cmd_param) into a string64. The engine searches its command line as raw text, so the word
// must not read as a key anywhere in it: ASCII letters, digits, '_' and '.' stay, so do the bytes from 0x80 (cp1251 letters); everything
// else ('-', '$', blanks, quotes, control characters) becomes '_'. At most 63 bytes.
inline std::string nick_word(const std::string& s)
{
	std::string out;
	for (char c : s)
	{
		if (out.size() >= 63)
			break;
		const unsigned char u = static_cast<unsigned char>(c);
		const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || u >= 0x80;
		out.push_back(keep ? c : '_');
	}
	return out;
}

// The bytes of a text as lower-case hex digits: a save's name for -coop_load_hex (coop_dedicated_rules.h reads it back).
inline std::string hex_of(const std::string& s)
{
	static const char kDigits[] = "0123456789abcdef";
	std::string out;
	out.reserve(s.size() * 2);
	for (const char c : s)
	{
		out.push_back(kDigits[static_cast<unsigned char>(c) >> 4]);
		out.push_back(kDigits[static_cast<unsigned char>(c) & 15]);
	}
	return out;
}

namespace detail
{
// The name of a save as the dedicated host takes it (coop_dedicated_rules.h save_name_ok), and "last".
inline bool save_ok(const std::string& name)
{
	if (name.empty() || name.size() > 200 || name == "." || name == "..")
		return false;
	for (char c : name)
		if (c == '(' || c == ')' || c == '/' || c == '\\' || c == ':' || c == '"' || static_cast<unsigned char>(c) < 0x20)
			return false;
	return true;
}

inline bool hex32(const std::string& s)
{
	if (s.size() != 32)
		return false;
	for (char c : s)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			return false;
	return true;
}
} // namespace detail

struct LaunchArgs
{
	std::string exe; // the game's own exe (GetModuleFileName): quoted
	std::string fsltx; // the server's fsgame_world.ltx: bare_arg_ok
	uint16_t port = kDefaultPort;
	bool world_load = false; // -coop_load <save> instead of -coop_new
	std::string save; // world_load: the stem in savedgames, or "last"
	std::string stage; // a new world: the staging file (-coop_stage), passed on as it is; empty: none
	std::string token_hex; // the operator's token, 32 hex digits
	std::string nick; // -coop_nick: the world's name, nick_word; empty: none
	bool dbg = false; // -dbg, when the game has it
};

// The whole command line of the world server:
//   "<exe>" -fsltx <fsltx> -coop_dedicated <port> -coop_hidden -coop_operator <token> -silent_error_mode [-dbg]
//   (-coop_new [-coop_stage <file>] | -coop_load last | -coop_load_hex <hex of the save's name>) [-coop_nick <word>]
// The engine never splits its command line into arguments: Core.Params is the raw text, lower-cased, and its readers search it with
// strstr; quotes mean nothing to them, and "-$" anywhere runs a console command. So the keys of the process stand first, and no value is
// on the line as text: a save's name goes as hex, the world's name as nick_word, the two paths are
// bare_arg_ok.
// redact: "<hidden>" stands for the token, for the log. False with `why`, `out` untouched, when a value cannot pass.
inline bool command_line(const LaunchArgs& a, bool redact, std::string& out, std::string& why)
{
	const char* bad = nullptr;
	if (a.exe.empty())
		bad = "no exe";
	else if (!bare_arg_ok(a.fsltx))
		bad = "the path of fsgame_world.ltx holds a blank or a quote: the engine cannot read it";
	else if (a.port == 0)
		bad = "no port";
	else if (!detail::hex32(a.token_hex))
		bad = "the token is not 32 hex digits";
	else if (a.world_load && !detail::save_ok(a.save))
		bad = "not a save's name";
	else if (!a.stage.empty() && a.world_load)
		bad = "a staging file belongs to a new world";
	else if (!a.stage.empty() && !bare_arg_ok(a.stage))
		bad = "the staging file's path holds a blank or a quote";
	if (bad)
	{
		why = bad;
		return false;
	}
	const std::string nick = nick_word(a.nick);
	std::string line;
	// one buffer, so that no piece of the line with the token is left in freed memory by a growing string
	line.reserve(a.exe.size() * 2 + a.fsltx.size() + a.stage.size() + a.save.size() * 2 + nick.size() + 256);
	line += quote_arg(a.exe);
	line += " -fsltx ";
	line += a.fsltx;
	line += " -coop_dedicated ";
	line += std::to_string(a.port);
	line += " -coop_hidden -coop_operator ";
	if (redact)
		line += "<hidden>";
	else
		line += a.token_hex;
	line += " -silent_error_mode";
	if (a.dbg)
		line += " -dbg";
	if (!a.world_load)
	{
		line += " -coop_new";
		if (!a.stage.empty())
		{
			line += " -coop_stage ";
			line += a.stage;
		}
	}
	else if (a.save == "last") // dedicated::kLastSave: the host's own word for its newest save, not a name
		line += " -coop_load last";
	else
	{
		line += " -coop_load_hex ";
		line += hex_of(a.save);
	}
	if (!nick.empty())
	{
		line += " -coop_nick ";
		line += nick;
	}
	out.swap(line);
	return true;
}

// The environment block of the server: `block` (entries "name=value\0", a "\0" at the end, as GetEnvironmentStrings gives it) without
// every entry whose name starts with `prefix`, compared without case (Windows' names have none). Entries that start with '=' (the
// drives' folders) stay. The result ends with two NULs, also when nothing is left.
template <class C> std::basic_string<C> filter_environment(const C* block, const char* prefix)
{
	std::basic_string<C> out;
	const size_t plen = std::strlen(prefix);
	for (const C* p = block; p && *p;)
	{
		size_t n = 0;
		while (p[n])
			++n;
		bool drop = n >= plen;
		for (size_t i = 0; drop && i < plen; ++i)
		{
			C c = p[i];
			if (c >= C('a') && c <= C('z'))
				c = C(c - C('a') + C('A'));
			drop = c == C(static_cast<unsigned char>(prefix[i]));
		}
		if (!drop)
		{
			out.append(p, n);
			out.push_back(C(0));
		}
		p += n + 1;
	}
	if (out.empty())
		out.push_back(C(0));
	out.push_back(C(0));
	return out;
}

// The ports to try, the host's own first (the launcher's Steam bridge sends to it, rl-link/src/lib.rs:100), then the next ones; never 0,
// never past 65535. 0: the default port.
inline std::vector<uint16_t> port_candidates(uint16_t preferred, size_t count)
{
	std::vector<uint16_t> out;
	uint32_t p = preferred ? preferred : kDefaultPort;
	for (; out.size() < count && p <= 65535; ++p)
		out.push_back(uint16_t(p));
	return out;
}

// The bytes of a pipe as lines: "\n" ends one, a "\r" before it goes. A line longer than kMaxLine comes out cut at kMaxLine, the rest is
// the next line.
class LineSplitter
{
public:
	static constexpr size_t kMaxLine = 8192;

	void push(const char* data, size_t n, std::vector<std::string>& out)
	{
		for (size_t i = 0; i < n; ++i)
		{
			if (data[i] == '\n')
			{
				end_line(out);
				continue;
			}
			m_line.push_back(data[i]);
			if (m_line.size() >= kMaxLine)
				end_line(out);
		}
	}

	// The end of the pipe: what is left without its "\n".
	bool flush(std::string& out)
	{
		if (m_line.empty())
			return false;
		out.swap(m_line);
		m_line.clear();
		if (!out.empty() && out.back() == '\r')
			out.pop_back();
		return true;
	}

private:
	void end_line(std::vector<std::string>& out)
	{
		if (!m_line.empty() && m_line.back() == '\r')
			m_line.pop_back();
		out.push_back(m_line);
		m_line.clear();
	}

	std::string m_line;
};

// The pipe's UTF-8 back to the game's cp1251 (its UI and Lua): what cp1251 has no byte for, and broken UTF-8, is '?'.
inline std::string utf8_to_cp1251(const std::string& s)
{
	static const auto table = [] {
		std::map<uint32_t, char> m;
		for (int b = 0x80; b < 0x100; ++b)
		{
			const std::string u = cp1251_to_utf8(std::string(1, char(b)));
			uint32_t cp = 0;
			if (u.size() == 2)
				cp = (uint32_t(uint8_t(u[0])) & 0x1F) << 6 | (uint8_t(u[1]) & 0x3F);
			else if (u.size() == 3)
				cp = (uint32_t(uint8_t(u[0])) & 0x0F) << 12 | (uint32_t(uint8_t(u[1])) & 0x3F) << 6 | (uint8_t(u[2]) & 0x3F);
			if (cp && cp != 0xFFFD)
				m[cp] = char(b);
		}
		return m;
	}();
	std::string out;
	for (size_t i = 0; i < s.size();)
	{
		const uint8_t c = uint8_t(s[i]);
		size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
		bool ok = len != 0 && i + len <= s.size();
		for (size_t k = 1; ok && k < len; ++k)
		{
			const uint8_t cc = uint8_t(s[i + k]);
			ok = (cc >> 6) == 2;
			cp = cp << 6 | (cc & 0x3F);
		}
		if (!ok)
		{
			out.push_back('?');
			++i;
			continue;
		}
		i += len;
		if (cp < 0x80)
			out.push_back(char(cp));
		else
		{
			const auto it = table.find(cp);
			out.push_back(it == table.end() ? '?' : it->second);
		}
	}
	return out;
}

namespace detail
{
// "$key$ = a | b | c": [begin, end) of the last field of the first line of `key` that is not commented out, the blanks around it not in it.
inline bool last_field(const std::string& text, const char* key, size_t& begin, size_t& end)
{
	const size_t klen = std::strlen(key);
	for (size_t line = 0; line < text.size();)
	{
		size_t stop = text.find_first_of("\r\n", line);
		if (stop == std::string::npos)
			stop = text.size();
		size_t first = line;
		while (first < stop && (text[first] == ' ' || text[first] == '\t'))
			++first;
		if (first < stop && text[first] != ';' && text.compare(first, klen, key) == 0)
		{
			size_t eq = first + klen;
			while (eq < stop && (text[eq] == ' ' || text[eq] == '\t'))
				++eq;
			if (eq < stop && text[eq] == '=')
			{
				const size_t bar = text.rfind('|', stop - 1);
				if (bar == std::string::npos || bar < eq)
					return false;
				begin = bar + 1;
				while (begin < stop && (text[begin] == ' ' || text[begin] == '\t'))
					++begin;
				end = stop;
				while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
					--end;
				return end > begin;
			}
		}
		line = stop;
		while (line < text.size() && (text[line] == '\r' || text[line] == '\n'))
			++line;
	}
	return false;
}
} // namespace detail

// The world server's fsgame_world.ltx from the game's fsgame.ltx: its $app_data_root$ is world_dir of the game's (as the line writes it,
// relative or not), its $game_saves$ the creator's savedgames (rewrite_saves, `saves_dir` absolute). Every other line as it was. False,
// `out` untouched: one of the two lines is missing or rewrite_saves refuses.
inline bool world_fsgame(const std::string& text, const std::string& saves_dir, std::string& out)
{
	size_t begin = 0, end = 0;
	if (!detail::last_field(text, "$app_data_root$", begin, end))
		return false;
	const std::string dir = world_dir(text.substr(begin, end - begin));
	if (dir.empty())
		return false;
	std::string rewritten;
	if (!rewrite_saves(text.substr(0, begin) + dir + text.substr(end), saves_dir, rewritten))
		return false;
	out = rewritten;
	return true;
}

// What the parent knows of its server, from the lines of its standard output and the end of the process. Fed by one thread; the caller
// locks around it.
struct Watch
{
	explicit Watch(size_t max_lines = kKeepLines) : max_lines(max_lines ? max_lines : 1) {}

	size_t max_lines;
	Phase phase = Phase::Unknown; // the last phase it said; Unknown before the first. Never back from Down
	bool down = false; // it said "@rl phase down": its world is saved
	uint16_t port = 0; // "@rl ready port="
	std::string wait_code, wait_text; // the last "@rl wait", cleared at "phase ready"
	uint32_t players = 0;
	std::map<std::string, MapState> mapservers; // by level; "gone" ones are not here
	std::string error_code, error_text; // the last "@rl error"
	std::deque<std::string> lines; // the log lines (not @rl), the newest max_lines
	bool exited = false;
	bool crashed = false; // the process ended without "@rl phase down"
	uint32_t exit_code = 0;

	// One line of the pipe, without its "\n".
	void line(const std::string& l)
	{
		Record r;
		if (!parse(l, r))
		{
			lines.push_back(l);
			while (lines.size() > max_lines)
				lines.pop_front();
			return;
		}
		switch (r.event)
		{
		case Event::Phase:
			if (down || r.phase == Phase::Unknown)
				break;
			phase = r.phase;
			down = r.phase == Phase::Down;
			if (r.phase == Phase::Ready)
			{
				wait_code.clear();
				wait_text.clear();
			}
			break;
		case Event::Ready: port = r.port; break;
		case Event::Wait:
			wait_code = r.code;
			wait_text = r.text;
			break;
		case Event::Players: players = r.players; break;
		case Event::MapServer:
			if (r.state == MapState::Gone)
				mapservers.erase(r.level);
			else
				mapservers[r.level] = r.state;
			break;
		case Event::Error:
			error_code = r.code;
			error_text = r.text;
			break;
		case Event::Unknown: break;
		}
	}

	// The process ended with `code`.
	void exit(uint32_t code)
	{
		exited = true;
		exit_code = code;
		crashed = !down;
	}

	// The newest n log lines, oldest first.
	std::vector<std::string> last(size_t n) const
	{
		const size_t from = lines.size() > n ? lines.size() - n : 0;
		return std::vector<std::string>(lines.begin() + std::ptrdiff_t(from), lines.end());
	}
};

enum class Summary : uint8_t
{
	Starting, // running, not ready yet
	Ready, // the session is open: the game may connect
	Failed, // running, but it said an error before it was ready (gave_up, bad_args, no_save): stop it
	Stopping, // asked to stop (its input is closed), or saving
	Down, // ended after "down": the world is saved
	Crashed, // ended without "down"
};

inline Summary summary(const Watch& w, bool stop_asked)
{
	if (w.exited)
		return w.crashed ? Summary::Crashed : Summary::Down;
	if (stop_asked || w.phase == Phase::Saving || w.phase == Phase::Down)
		return Summary::Stopping;
	if (w.phase == Phase::Ready)
		return Summary::Ready;
	return w.error_code.empty() ? Summary::Starting : Summary::Failed;
}
} // namespace world_server
} // namespace coop
