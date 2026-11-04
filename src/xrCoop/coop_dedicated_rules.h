// Anomaly Coop — the dedicated host, the pure part (docs/plans/2026-10-24-f3a-split-levels.md §12, docs/DEDICATED_HOST.md). The world server
// without a window and without a player: the key -coop_dedicated. Its actor is a ghost on the service level fake_start, so for the session
// it is a host who stands on a level of his own and plays nothing; every level with players has a map server, and everybody who plays is
// an ordinary client. This header holds what the engine only has to carry out:
//   - Args         the keys -coop_dedicated [port], -coop_new, -coop_load <save>;
//   - start_line   the console command that starts that world;
//   - newest_save  what -coop_load last means;
//   - restore_list what coop_restore offers;
//   - parse_line   what a line typed into the console window is;
//   - log_shown    which lines of the game's log the window shows.
// Pure C++17, header-only, no engine dependencies: used by xrEngine (x_ray.cpp, the key), xrGame (coop/coop_dedicated.cpp) and by the
// unit-test exe.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop
{
namespace dedicated
{
constexpr char kKey[] = "-coop_dedicated";
constexpr char kNewKey[] = "-coop_new";
constexpr char kLoadKey[] = "-coop_load";
constexpr char kLastSave[] = "last"; // -coop_load last: the newest save of this folder
constexpr char kServiceLevel[] = "fake_start"; // where the ghost stands: nothing of the game happens there
constexpr uint16_t kDefaultPort = 27960; // coop::kDefaultPort, this header is included by the engine without the protocol

enum class World : uint8_t
{
	None, // no key: the operator chooses in the window (coop_world)
	New,
	Load,
};

struct Args
{
	uint16_t port = kDefaultPort;
	World world = World::None;
	std::string save; // World::Load: a name in savedgames without its extension, or kLastSave
};

namespace detail
{
inline bool blank(char c) { return c == ' ' || c == '\t'; }

// `key` as a word of `params`: at the start or after a blank, and ending with the word. The pointer past it, or nullptr.
inline const char* find_key(const char* params, const char* key)
{
	const size_t n = std::strlen(key);
	for (const char* p = params; p && (p = std::strstr(p, key)) != nullptr; p += n)
		if ((p == params || blank(p[-1])) && (p[n] == '\0' || blank(p[n])))
			return p + n;
	return nullptr;
}

inline const char* skip_blanks(const char* p)
{
	while (blank(*p))
		++p;
	return p;
}

// A save's name may hold blanks ("stalker - autosave"): quoted, or up to the next key, a blank, a dash and a letter.
inline std::string read_name(const char* p)
{
	p = skip_blanks(p);
	std::string out;
	if (p[0] == '-' && p[1] >= 'a' && p[1] <= 'z')
		return out; // the next key: no name
	if (*p == '"')
	{
		for (++p; *p && *p != '"'; ++p)
			out.push_back(*p);
		return out;
	}
	for (; *p; ++p)
	{
		if (blank(*p) && p[1] == '-' && p[2] >= 'a' && p[2] <= 'z')
			break;
		out.push_back(*p);
	}
	while (!out.empty() && blank(out.back()))
		out.pop_back();
	return out;
}
} // namespace detail

// The key in a command line. It decides the headless start in xrEngine before anything else runs.
inline bool has_key(const char* params) { return params && detail::find_key(params, kKey) != nullptr; }

// False with `why` when the keys contradict each other or a value is not usable: the window says so and waits for the operator.
inline bool parse_args(const char* params, Args& out, std::string& why)
{
	Args a;
	const char* after = params ? detail::find_key(params, kKey) : nullptr;
	if (!after)
	{
		why = "no key -coop_dedicated";
		return false;
	}
	const char* p = detail::skip_blanks(after);
	if (*p >= '0' && *p <= '9')
	{
		uint32_t v = 0;
		for (; *p >= '0' && *p <= '9'; ++p)
		{
			v = v * 10 + uint32_t(*p - '0');
			if (v > 65535)
				break;
		}
		if (v == 0 || v > 65535 || (*p && !detail::blank(*p)))
		{
			why = "the port must be 1..65535";
			return false;
		}
		a.port = uint16_t(v);
	}
	const char* fresh = detail::find_key(params, kNewKey);
	const char* load = detail::find_key(params, kLoadKey);
	if (fresh && load)
	{
		why = "-coop_new and -coop_load exclude each other";
		return false;
	}
	if (fresh)
		a.world = World::New;
	if (load)
	{
		a.world = World::Load;
		a.save = detail::read_name(load);
		if (a.save.empty())
		{
			why = "-coop_load needs a save's name, or last";
			return false;
		}
	}
	out = a;
	return true;
}

// The console command that starts a world. A name of a save may not break out of the brackets of the command, nor out of savedgames.
inline bool save_name_ok(const std::string& name)
{
	if (name.empty() || name.size() > 200)
		return false;
	for (const char c : name)
		if (c == '(' || c == ')' || c == '/' || c == '\\' || c == ':' || c == '"' || c == '\r' || c == '\n')
			return false;
	return name != "." && name != "..";
}

inline std::string start_line(World world, const std::string& save)
{
	const std::string server = world == World::New ? std::string("all/single/alife/new") : save + "/single/alife/load";
	return "start server(" + server + ") client(localhost)";
}

// The file with the newest time among those of a folder of saves, nothing if there is none. time = whatever grows with time.
struct SaveFile
{
	std::string name; // without the extension
	uint64_t time = 0;
};

inline std::string newest_save(const std::vector<SaveFile>& files)
{
	const SaveFile* best = nullptr;
	for (const SaveFile& f : files)
		if (!best || f.time > best->time)
			best = &f;
	return best ? best->name : std::string();
}

// The saves coop_restore offers: the autosaves of this folder, the newest first. The temporary pair of a save being written is none of them.
inline std::vector<SaveFile> restore_list(std::vector<SaveFile> files)
{
	const std::string prefix = "coop_auto_";
	files.erase(std::remove_if(files.begin(), files.end(),
	                [&](const SaveFile& f) {
		                return f.name.compare(0, prefix.size(), prefix) != 0 || f.name == "coop_auto_tmp" || f.name == "coop_auto_bak";
	                }),
	    files.end());
	std::stable_sort(files.begin(), files.end(), [](const SaveFile& a, const SaveFile& b) { return a.time > b.time; });
	return files;
}

// coop_restore <what>: a number of restore_list, from 1, or a name. Nothing if there is no such save.
inline std::string pick_restore(const std::vector<SaveFile>& list, const std::string& what)
{
	if (!what.empty() && what.find_first_not_of("0123456789") == std::string::npos && what.size() < 4)
	{
		const size_t n = size_t(std::stoi(what));
		return n >= 1 && n <= list.size() ? list[n - 1].name : std::string();
	}
	for (const SaveFile& f : list)
		if (f.name == what)
			return f.name;
	return std::string();
}

// What a line of the console window is.
enum class Line : uint8_t
{
	Empty,
	Quit, // quit, exit, stop: the world is saved first
	Command, // a console command of the game, `text`
	Chat, // anything after "say ": a line from the host to all players, `text`
	Unknown, // a word starting with '/' that means nothing
};

struct Parsed
{
	Line kind = Line::Empty;
	std::string text;
};

inline Parsed parse_line(std::string s)
{
	while (!s.empty() && (detail::blank(s.back()) || s.back() == '\r' || s.back() == '\n'))
		s.pop_back();
	size_t from = 0;
	while (from < s.size() && detail::blank(s[from]))
		++from;
	s.erase(0, from);
	Parsed out;
	if (s.empty())
		return out;
	if (s == "quit" || s == "exit" || s == "stop")
	{
		out.kind = Line::Quit;
		return out;
	}
	if (s == "/go")
	{
		// the chat's /go: the group waits at a level exit and goes now
		out.kind = Line::Command;
		out.text = "coop_accept_transition";
		return out;
	}
	if (s.compare(0, 4, "say ") == 0)
	{
		out.kind = Line::Chat;
		out.text = s.substr(4);
		return out;
	}
	if (s[0] == '/')
	{
		out.kind = Line::Unknown;
		out.text = s;
		return out;
	}
	out.kind = Line::Command;
	out.text = s;
	return out;
}

// Which lines of the log the window shows. The log of a GAMMA start is tens of thousands of lines, the operator wants the session: 0 errors
// only, 1 the session and the errors, 2 everything.
constexpr int kLogDefault = 1;

inline bool log_shown(const char* line, int level)
{
	if (level >= 2)
		return true;
	if (!line || !*line)
		return false;
	const char mark = line[0];
	const bool error = mark == '!' || std::strstr(line, "FATAL ERROR") != nullptr || std::strncmp(line, "[error]", 7) == 0;
	if (error)
		return true;
	if (level <= 0)
		return false;
	const char* text = (mark == '*' || mark == '~' || mark == '-') && line[1] == ' ' ? line + 2 : line;
	return std::strncmp(text, "[coop", 5) == 0 || std::strncmp(text, "[x-ray]: Starting Main Loop", 27) == 0;
}
} // namespace dedicated
} // namespace coop
