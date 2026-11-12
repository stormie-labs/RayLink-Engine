// Anomaly Coop — the dedicated host, the pure part (docs/plans/2026-10-24-f3a-split-levels.md §12, docs/DEDICATED_HOST.md). The world server
// without a window and without a player: the key -coop_dedicated. Its actor is a ghost on the service level fake_start, so for the session
// it is a host who stands on a level of his own and plays nothing; every level with players has a map server, and everybody who plays is
// an ordinary client. This header holds what the engine only has to carry out:
//   - Args         the keys -coop_dedicated [port], -coop_new [-coop_stage <file>], -coop_load <save>;
//   - start_line   the console command that starts that world;
//   - newest_save  what -coop_load last means;
//   - restore_list what coop_restore offers;
//   - parse_line   what a line typed into the console window is;
//   - log_shown    which lines of the game's log the window shows;
//   - console_plan -coop_hidden: no window, the launcher's pipe only, its end is quit; a redirected output gets the log and the @rl lines;
//   - wait_code    the reason to wait as one word, for "@rl wait" (coop_world_server_rules.h);
//   - world_wait, HostAutostart  why the world is not up yet, and the autostart of the session that tries until it is.
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
// The same save, its name as hex digits of its bytes: what a parent process writes (world_server::command_line, the launcher). The engine
// reads its command line as raw text, quotes mean nothing to it, so a name written as text could read as a key (security review
// 2026-11-12).
constexpr char kLoadHexKey[] = "-coop_load_hex";
constexpr char kHiddenKey[] = "-coop_hidden"; // plan 2026-11-12 stage 1: run by the launcher, no window of its own
constexpr char kStageKey[] = "-coop_stage"; // plan 2026-11-12 stage 5: the new world's staging file (coop_world_stage.h), with -coop_new
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
	// World::New: the path of the staging file (-coop_stage), one word as world_server::command_line passes it; empty: none, the world
	// starts as it always did
	std::string stage;
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

// -coop_hidden in a command line.
inline bool has_hidden_key(const char* params) { return params && detail::find_key(params, kHiddenKey) != nullptr; }

// What the console of the dedicated host is. Without -coop_hidden it is what it always was: a window of its own, and a standard input that is
// a pipe or a file read beside it, whose end means nothing. With it there is no window; the input is the launcher's pipe and its end means the
// launcher (or the host's game, which owns the launcher's server) is gone: the world is saved and the process quits.
struct ConsolePlan
{
	bool window = true; // AllocConsole, the window's own line editing
	bool read_input = false; // a thread reads the standard input
	bool eof_quits = false; // the end of that input is quit
	// plan 2026-11-12 stage 2: the log lines the window would show and the @rl lines (coop_world_server_rules.h) go to the standard output.
	// Only with -coop_hidden and an output that is a pipe or a file: the window stays as it was, and a hidden host nobody reads writes nothing.
	bool write_output = false;
};

inline ConsolePlan console_plan(bool hidden, bool input_is_stream, bool output_is_stream = false)
{
	ConsolePlan p;
	p.window = !hidden;
	p.read_input = input_is_stream;
	p.eof_quits = hidden && input_is_stream;
	p.write_output = hidden && output_is_stream;
	return p;
}

inline bool save_name_ok(const std::string& name);

namespace detail
{
// -coop_load_hex <hex>: pairs of hex digits (either case: the engine lower-cases its command line) up to the next blank, 1..200 bytes,
// none of them a control character.
inline bool read_hex_name(const char* p, std::string& out)
{
	const auto digit = [](char c) -> int
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	p = skip_blanks(p);
	std::string name;
	for (; *p && !blank(*p); p += 2)
	{
		const int hi = digit(p[0]);
		const int lo = p[1] ? digit(p[1]) : -1;
		if (hi < 0 || lo < 0 || name.size() >= 200)
			return false;
		const int c = hi * 16 + lo;
		if (c < 0x20)
			return false;
		name.push_back(char(c));
	}
	if (name.empty())
		return false;
	out = name;
	return true;
}
} // namespace detail

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
	const char* load_hex = detail::find_key(params, kLoadHexKey);
	if (fresh && (load || load_hex))
	{
		why = "-coop_new and -coop_load exclude each other";
		return false;
	}
	if (load && load_hex)
	{
		why = "-coop_load and -coop_load_hex exclude each other";
		return false;
	}
	if (fresh)
		a.world = World::New;
	if (load_hex)
	{
		a.world = World::Load;
		if (!detail::read_hex_name(load_hex, a.save) || !save_name_ok(a.save))
		{
			why = "-coop_load_hex needs a save's name as hex digits of its bytes";
			return false;
		}
	}
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
	// Stage 5: -coop_stage <file>, one word up to the next blank (world_server::command_line refuses a path with a blank or a quote).
	if (const char* stage = detail::find_key(params, kStageKey))
	{
		if (a.world != World::New)
		{
			why = "-coop_stage belongs to -coop_new";
			return false;
		}
		const char* v = detail::skip_blanks(stage);
		for (; *v && !detail::blank(*v); ++v)
			a.stage.push_back(*v);
		if (a.stage.empty() || a.stage[0] == '-' || a.stage.find('"') != std::string::npos)
		{
			why = "-coop_stage needs the path of the staging file, one word without blanks or quotes";
			return false;
		}
	}
	out = a;
	return true;
}

// Core.Params is the process's command line lower-cased in place (xrCore.cpp: xr_strdup(GetCommandLine) + xr_strlwr, then the lines of an
// optional commandline.txt appended), so the path parse_args read from it is lower-case. Windows opens it all the same; the server still
// takes the path's own letters from the raw command line (`raw`, GetCommandLineA) at the same place, when that place holds the same text
// regardless of case, so that its log and <file>.used keep the name the parent wrote. Otherwise (a key from commandline.txt) the lower-case one.
inline std::string stage_original_case(const char* params, const char* raw, const std::string& stage)
{
	const char* after = params ? detail::find_key(params, kStageKey) : nullptr;
	if (!after || !raw || stage.empty())
		return stage;
	const size_t at = size_t(detail::skip_blanks(after) - params);
	const size_t raw_len = std::strlen(raw);
	if (raw_len < at + stage.size() || std::strncmp(params + at, stage.c_str(), stage.size()) != 0)
		return stage;
	std::string out(raw + at, stage.size());
	for (size_t i = 0; i < out.size(); ++i)
	{
		const char c = out[i];
		const char low = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
		if (low != stage[i])
			return stage;
	}
	return out;
}

// -coop_stage <value>: a bare file name (no folder, no drive) is a file of the server's own data folder ($app_data_root$), where its
// parent wrote it (the game's coop.world_server.stage_file, the launcher). A path of a player's profile may hold blanks, which the
// command line cannot carry: the engine reads the value up to a blank. A value with a separator or a drive is a path as it is.
inline bool stage_is_name(const std::string& stage)
{
	return !stage.empty() && stage != "." && stage != ".." && stage.find_first_of("\\/:") == std::string::npos;
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
	// A UTF-8 BOM before the first line of a pipe (PowerShell writes one): "Unknown command: <BOM>coop_status" (live, 2026-11-01).
	if (s.compare(0, 3, "\xEF\xBB\xBF") == 0)
		s.erase(0, 3);
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

// ---------------------------------------------------------------- the session comes up by itself

// What host_can_start (xrGame coop_join.cpp) asks of the world, member by member, plus the precache of the old autostart.
struct WorldFacts
{
	bool level = false; // g_pGameLevel
	bool ready = false; // its bReady
	bool precache = false; // Device.dwPrecacheFrame: 0 in the headless start
	bool alife = false;
	bool alife_actor = false; // ai().alife().graph().actor()
	bool client_actor = false; // g_actor: the actor online on the client of this process
	bool alive = false; // the ALife actor's health > 0
};

// Why the world is not up; None: it is, hosting may start.
enum class Wait : uint8_t
{
	None,
	NoLevel,
	Loading,
	Precache,
	NoAlife,
	NoAlifeActor,
	NoClientActor,
	ActorDead,
	Refused, // all is up, but the session refused to start (a busy port): the next try comes later
};

inline Wait world_wait(const WorldFacts& f)
{
	if (!f.level)
		return Wait::NoLevel;
	if (!f.ready)
		return Wait::Loading;
	if (f.precache)
		return Wait::Precache;
	if (!f.alife)
		return Wait::NoAlife;
	if (!f.alife_actor)
		return Wait::NoAlifeActor;
	if (!f.client_actor)
		return Wait::NoClientActor;
	if (!f.alive)
		return Wait::ActorDead;
	return Wait::None;
}

inline const char* wait_text(Wait w)
{
	switch (w)
	{
	case Wait::None: return "the world is up";
	case Wait::NoLevel: return "no level yet: coop_world, or the level is being created";
	case Wait::Loading: return "the level is loading";
	case Wait::Precache: return "the level precaches";
	case Wait::NoAlife: return "no ALife";
	case Wait::NoAlifeActor: return "ALife has no actor";
	case Wait::NoClientActor: return "the actor is not online on the client of this process (g_actor is null)";
	case Wait::ActorDead: return "the actor is dead";
	case Wait::Refused: return "hosting was refused, the line above says why; the next try comes later";
	}
	return "unknown";
}

// The same reason as one word, for the line "@rl wait <code> <text>" (coop_world_server_rules.h): the parent keys on it, the text is for people.
inline const char* wait_code(Wait w)
{
	switch (w)
	{
	case Wait::None: return "none";
	case Wait::NoLevel: return "no_level";
	case Wait::Loading: return "loading";
	case Wait::Precache: return "precache";
	case Wait::NoAlife: return "no_alife";
	case Wait::NoAlifeActor: return "no_alife_actor";
	case Wait::NoClientActor: return "no_client_actor";
	case Wait::ActorDead: return "actor_dead";
	case Wait::Refused: return "refused";
	}
	return "unknown";
}

constexpr uint64_t kHostRetryMs = 5000; // after the first refusal
constexpr uint64_t kHostRetryMaxMs = 30000; // the waits double up to this
constexpr uint64_t kHostGiveUpMs = 15 * 60 * 1000; // from the first frame with a level: a GAMMA new world takes minutes, never fifteen

// The autostart of the dedicated host's session, called every frame while there is none. It tries the moment the world is up, after a refusal
// again later, says every new reason to wait once, and gives up loudly when the world has had kHostGiveUpMs since its level came. Without a
// level there is no clock: a host without -coop_new / -coop_load waits for the operator's coop_world as long as it takes.
class HostAutostart
{
public:
	enum class Do : uint8_t
	{
		Nothing,
		Try, // start_host now; then up() or refused()
		GiveUp,
	};
	struct Step
	{
		Do act = Do::Nothing;
		bool say = false; // a new reason to wait, or the giving up: one line
		Wait reason = Wait::None;
		uint64_t waited_ms = 0; // since the first frame with a level
	};

	Step tick(uint64_t now, Wait wait)
	{
		Step s;
		if (m_up || m_gave_up)
			return s;
		if (wait != Wait::NoLevel && !m_clock)
		{
			m_clock = true;
			m_since = now;
		}
		s.reason = wait == Wait::None && m_refusals && now < m_next ? Wait::Refused : wait;
		s.waited_ms = m_clock && now > m_since ? now - m_since : 0;
		if (m_clock && s.waited_ms >= kHostGiveUpMs)
		{
			m_gave_up = true;
			s.act = Do::GiveUp;
			s.say = true;
			return s;
		}
		if (s.reason == Wait::None)
		{
			s.act = Do::Try;
			return s;
		}
		if (s.reason != m_said)
		{
			m_said = s.reason;
			s.say = true;
		}
		return s;
	}

	void refused(uint64_t now)
	{
		++m_refusals;
		uint64_t wait = kHostRetryMs;
		for (uint32_t i = 1; i < m_refusals && wait < kHostRetryMaxMs; ++i)
			wait *= 2;
		m_next = now + (wait < kHostRetryMaxMs ? wait : kHostRetryMaxMs);
	}

	void up() { m_up = true; }
	bool done() const { return m_up; }
	bool gave_up() const { return m_gave_up; }

private:
	bool m_up = false;
	bool m_gave_up = false;
	bool m_clock = false;
	uint64_t m_since = 0;
	uint64_t m_next = 0;
	uint32_t m_refusals = 0;
	Wait m_said = Wait::None;
};

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
