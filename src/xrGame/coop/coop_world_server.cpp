// Anomaly Coop - the game's own world server: the process, its pipes, its state. See coop_world_server.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_world_server.h"
#include "coop_manager.h"
#include "xrCoop/coop_mapserver_rules.h" // fsltx_of, map_server_user_ltx: the same fsgame and user.ltx a map server gets
#include "xrCoop/coop_world_server_rules.h"

#include <winsock2.h> // after windows.h: xrCore_platform.h defines WIN32_LEAN_AND_MEAN, so winsock.h was never included
#include <bcrypt.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "xrCoop/coop_world_stage.h" // kFileName: the staging file of a new world

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace coop
{
namespace world_server
{
namespace
{
constexpr u64 kReadyWaitMs = 16 * 60 * 1000; // the server gives up by itself at 15 minutes (DEDICATED_HOST.md); one more for its "error"
constexpr u64 kStopWaitMs = 60 * 1000; // plan section 3.5: "until @rl phase down or the process ends (up to 60 s)", then terminated
constexpr DWORD kPollMs = 50; // the reader looks at the pipe and the process this often when the pipe is empty
constexpr size_t kChunksPerPass = 64; // 256 KiB of the pipe, then the reader looks at the process and the deadlines
constexpr size_t kPortTries = 16; // the host's port, then up to 15 next ones
constexpr size_t kStateLines = 20; // lines in state(): the error screen shows the last 20 (plan section 3.3)
constexpr size_t kMaxLinesAsked = kKeepLines;
constexpr UINT kKilledCode = 0xC0DE0060; // the exit code of a server terminated after the stop's wait
constexpr char kWorldFsltx[] = "fsgame_world.ltx";

// One start of a server. Allocated at the first start and never freed: nothing of it runs at the engine's exit (the reader thread may
// still be inside ReadFile then; the OS ends it and closes our ends of the pipes, which is the server's EOF: it saves and quits).
struct Child
{
	// the main thread's, set before the reader starts
	HANDLE process = nullptr;
	HANDLE reader = nullptr;
	HANDLE out_read = nullptr; // the reader's
	DWORD pid = 0;
	u16 asked_port = 0;
	u64 started_at = 0;

	// shared: under g_lock
	HANDLE in_write = nullptr; // closed once: stop(), the ready timeout, or the reader at the end
	op::Token token{};
	u16 port = 0; // the port it was started with: operator_token_for compares with it
	Watch watch;
	bool stop_asked = false;
	u64 stop_at = 0;
	bool killed = false;
	bool ready_timeout = false;
	bool done = false; // the reader has ended: the process is gone and its output read
};

std::mutex* g_lock = nullptr;
Child* g = nullptr;

void close_handle(HANDLE& h)
{
	if (h)
		CloseHandle(h);
	h = nullptr;
}

// Under g_lock. The server's EOF: it saves its world and quits (coop_host_console.cpp).
void close_input_locked(Child& c)
{
	close_handle(c.in_write);
	if (!c.stop_asked)
	{
		c.stop_asked = true;
		c.stop_at = GetTickCount64();
	}
}

// ---------------------------------------------------------------- the reader thread: nothing of Lua or of the game here

void feed(Child& c, LineSplitter& split, const char* data, size_t n)
{
	std::vector<std::string> lines;
	split.push(data, n, lines);
	if (lines.empty())
		return;
	std::lock_guard<std::mutex> lock(*g_lock);
	for (const std::string& l : lines)
		c.watch.line(l);
}

// What the pipe holds now, at most max_chunks reads of 4 KiB (a server that writes without a pause must not keep the reader from the
// stop's deadline). False: it is broken (the server closed its output, or is gone).
bool drain(Child& c, LineSplitter& split, bool& got_any, size_t max_chunks)
{
	char buf[4096];
	got_any = false;
	for (size_t chunk = 0; chunk < max_chunks; ++chunk)
	{
		DWORD avail = 0;
		if (!PeekNamedPipe(c.out_read, nullptr, 0, nullptr, &avail, nullptr))
			return false;
		if (!avail)
			return true;
		DWORD got = 0;
		if (!ReadFile(c.out_read, buf, avail < sizeof(buf) ? avail : DWORD(sizeof(buf)), &got, nullptr) || !got)
			return false;
		got_any = true;
		feed(c, split, buf, got);
	}
	return true;
}

DWORD WINAPI reader_main(void* arg)
{
	Child& c = *static_cast<Child*>(arg);
	LineSplitter split;
	bool pipe_ok = true;
	for (;;)
	{
		bool got_any = false;
		if (pipe_ok)
			pipe_ok = drain(c, split, got_any, kChunksPerPass);
		if (WaitForSingleObject(c.process, got_any ? 0 : kPollMs) == WAIT_OBJECT_0)
			break;
		const u64 now = GetTickCount64();
		std::lock_guard<std::mutex> lock(*g_lock);
		// never ready: no "@rl ready port=" (a server that was ready once may stay up for hours)
		if (!c.stop_asked && c.watch.port == 0 && !c.watch.down && now - c.started_at > kReadyWaitMs)
		{
			c.ready_timeout = true; // it never said ready: ask it to quit, then the stop's wait as for any stop
			close_input_locked(c);
		}
		if (c.stop_asked && !c.killed && now - c.stop_at > kStopWaitMs)
		{
			c.killed = true;
			TerminateProcess(c.process, kKilledCode);
		}
	}
	bool got_any = false;
	if (pipe_ok)
		drain(c, split, got_any, size_t(-1)); // what it wrote before it went: a gone writer adds nothing, the pipe ends
	std::string rest;
	DWORD code = 0;
	GetExitCodeProcess(c.process, &code);
	close_handle(c.out_read);
	std::lock_guard<std::mutex> lock(*g_lock);
	if (split.flush(rest))
		c.watch.line(rest);
	c.watch.exit(u32(code));
	close_handle(c.in_write);
	SecureZeroMemory(c.token.data(), c.token.size()); // its server is gone: the token opens nothing any more
	c.done = true;
	return 0;
}

// ---------------------------------------------------------------- the start

std::string read_text(const std::string& path)
{
	std::string out;
	if (FILE* f = std::fopen(path.c_str(), "rb"))
	{
		char chunk[4096];
		size_t n;
		while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
			out.append(chunk, n);
		std::fclose(f);
	}
	return out;
}

bool write_text(const std::string& path, const std::string& text)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	return std::fclose(f) == 0 && ok;
}

bool file_exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Where the engine found this process's fsgame (CLocatorAPI searchForFsltx): as coop_mapserver.cpp find_fsltx does.
bool find_fsltx(const std::string& name, std::string& found)
{
	const std::string app = Core.ApplicationPath;
	const std::string where[] = {name, "../" + name, app + name, app + "../" + name};
	for (const std::string& path : where)
		if (file_exists(path))
		{
			found = path;
			return true;
		}
	return false;
}

// fsgame_world.ltx next to the game's fsgame, the server's data folder with its user.ltx (and axr_options.ltx the first time). `fsltx`:
// the path for -fsltx, without a blank (the engine reads the key up to one).
bool prepare_files(std::string& fsltx, std::string& why)
{
	std::string found;
	const std::string name = mapserver::fsltx_of(Core.Params);
	if (!find_fsltx(name, found))
	{
		why = "cannot find this game's fsgame '" + name + "'";
		return false;
	}
	string_path saves, root;
	FS.update_path(saves, "$game_saves$", "");
	FS.update_path(root, "$app_data_root$", "");
	std::string text;
	if (!world_fsgame(read_text(found), saves, text))
	{
		why = "'" + found + "' has no $app_data_root$ or $game_saves$ line the server can take, or the saves' folder '" + std::string(saves) +
		      "' cannot stand in it";
		return false;
	}
	const size_t slash = found.find_last_of("\\/");
	fsltx = (slash == std::string::npos ? std::string() : found.substr(0, slash + 1)) + kWorldFsltx;
	if (!write_text(fsltx, text))
	{
		why = "cannot write '" + fsltx + "'";
		return false;
	}
	if (!bare_arg_ok(fsltx))
	{
		char short_path[MAX_PATH];
		const DWORD n = GetShortPathNameA(fsltx.c_str(), short_path, MAX_PATH);
		if (n && n < MAX_PATH && bare_arg_ok(short_path))
			fsltx = short_path; // 8.3 names: the volume keeps them, the path has no blank any more
	}
	const std::string mine = world_dir(root);
	if (mine.empty())
	{
		why = "no data folder of the game";
		return false;
	}
	CreateDirectoryA(mine.c_str(), nullptr);
	if (!write_text(mine + "user.ltx", mapserver::map_server_user_ltx(read_text(std::string(root) + "user.ltx"))))
	{
		why = "cannot write the server's user.ltx in '" + mine + "'";
		return false;
	}
	// plan section 2.4: the machine's mod settings, written by whoever made the folder; the server keeps its own afterwards
	const std::string options = read_text(std::string(root) + "axr_options.ltx");
	if (!options.empty() && !file_exists(mine + "axr_options.ltx"))
		write_text(mine + "axr_options.ltx", options);
	return true;
}

// The world server's data folder (its $app_data_root$), made if it is not there. False: this game has no data folder to stand next to.
bool world_folder(std::string& out)
{
	string_path root;
	FS.update_path(root, "$app_data_root$", "");
	const std::string mine = world_dir(root);
	if (mine.empty())
		return false;
	CreateDirectoryA(mine.c_str(), nullptr);
	out = mine;
	return true;
}

// A UDP port nobody holds now (the session is ENet, UDP). An exclusive bind, closed at once: the server binds it a moment later.
bool port_free(u16 port)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return true; // cannot tell: the server says so itself if it cannot bind
	bool free_port = false;
	const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s != INVALID_SOCKET)
	{
		BOOL one = TRUE;
		setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
		sockaddr_in a;
		ZeroMemory(&a, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons(port);
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		free_port = bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0;
		closesocket(s);
	}
	WSACleanup();
	return free_port;
}

bool make_token(op::Token& out)
{
	for (int tries = 0; tries < 4; ++tries)
	{
		if (BCryptGenRandom(nullptr, out.data(), ULONG(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
			return false;
		if (!op::token_none(out))
			return true;
	}
	return false;
}

// The handles a start opened, for a start that fails half-way.
struct Opened
{
	HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
	~Opened()
	{
		close_handle(in_read);
		close_handle(in_write);
		close_handle(out_read);
		close_handle(out_write);
		if (attrs)
		{
			DeleteProcThreadAttributeList(attrs);
			HeapFree(GetProcessHeap(), 0, attrs);
		}
	}
};

bool start(const LaunchArgs& base, std::string& why)
{
	if (!g_lock)
		g_lock = new std::mutex();
	{
		std::lock_guard<std::mutex> lock(*g_lock);
		if (g && !g->done)
		{
			why = "a world server of this game runs already";
			return false;
		}
	}
	if (g)
	{
		WaitForSingleObject(g->reader, INFINITE); // done: it has returned or is returning
		close_handle(g->reader);
		close_handle(g->process);
		delete g;
		g = nullptr;
	}
	if (mgr().active())
	{
		why = "in a co-op session: leave it first";
		return false;
	}

	LaunchArgs a = base;
	const std::vector<u16> ports = port_candidates(a.port, kPortTries);
	u16 port = 0;
	for (u16 p : ports)
		if (port_free(p))
		{
			port = p;
			break;
		}
	if (!port)
	{
		why = "no free UDP port from " + std::to_string(ports.empty() ? 0 : ports.front());
		return false;
	}
	if (!prepare_files(a.fsltx, why))
		return false;
	char exe[MAX_PATH];
	const DWORD exe_len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
	if (!exe_len || exe_len >= MAX_PATH)
	{
		why = "cannot name this exe";
		return false;
	}
	a.exe = exe;
	a.port = port;
	a.dbg = Core.ParamsData.test(ECoreParams::dbg);
	op::Token token{};
	if (!make_token(token))
	{
		why = "the system's random generator failed";
		return false;
	}
	a.token_hex = token_hex(token.data(), token.size());
	std::string line, shown;
	const bool line_ok = command_line(a, false, line, why) && command_line(a, true, shown, why);
	SecureZeroMemory(&a.token_hex[0], a.token_hex.size());
	if (!line_ok)
	{
		SecureZeroMemory(&line[0], line.size());
		return false;
	}

	std::wstring env;
	if (wchar_t* block = GetEnvironmentStringsW())
	{
		env = filter_environment(block, kSteamEnvPrefix); // never a Steam client: coop_manager.cpp steam_join_waits
		FreeEnvironmentStringsW(block);
	}

	Opened h;
	SECURITY_ATTRIBUTES sa;
	ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	if (!CreatePipe(&h.in_read, &h.in_write, &sa, 0) || !CreatePipe(&h.out_read, &h.out_write, &sa, 0) ||
	    !SetHandleInformation(h.in_write, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(h.out_read, HANDLE_FLAG_INHERIT, 0))
	{
		SecureZeroMemory(&line[0], line.size());
		why = "cannot make its pipes: error " + std::to_string(GetLastError());
		return false;
	}
	// Only the two pipe ends go to the child, not every inheritable handle of the game.
	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
	h.attrs = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attr_size));
	HANDLE inherit[2] = {h.in_read, h.out_write};
	if (!h.attrs || !InitializeProcThreadAttributeList(h.attrs, 1, 0, &attr_size))
	{
		if (h.attrs)
			HeapFree(GetProcessHeap(), 0, h.attrs);
		h.attrs = nullptr;
		SecureZeroMemory(&line[0], line.size());
		why = "cannot make its attribute list";
		return false;
	}
	if (!UpdateProcThreadAttribute(h.attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr))
	{
		SecureZeroMemory(&line[0], line.size());
		why = "cannot limit its handles";
		return false;
	}
	STARTUPINFOEXA si;
	ZeroMemory(&si, sizeof(si));
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = h.in_read;
	si.StartupInfo.hStdOutput = h.out_write;
	si.StartupInfo.hStdError = h.out_write;
	si.lpAttributeList = h.attrs;
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));
	// No kill-on-close job (plan section 3.3): a crash of the game must not kill a world before it is saved; its EOF saves it. It breaks
	// away from a job the game may run in (a launcher's), where that job allows it.
	const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
	BOOL made = CreateProcessA(exe, &line[0], nullptr, nullptr, TRUE, flags | CREATE_BREAKAWAY_FROM_JOB, env.empty() ? nullptr : &env[0],
	    nullptr, &si.StartupInfo, &pi);
	if (!made && GetLastError() == ERROR_ACCESS_DENIED)
		made = CreateProcessA(exe, &line[0], nullptr, nullptr, TRUE, flags, env.empty() ? nullptr : &env[0], nullptr, &si.StartupInfo, &pi);
	const DWORD err = made ? 0 : GetLastError();
	SecureZeroMemory(&line[0], line.size());
	if (!made)
	{
		why = "cannot start it: error " + std::to_string(err);
		::Msg("! [coop] world server: %s", why.c_str());
		return false;
	}
	CloseHandle(pi.hThread);
	close_handle(h.in_read); // the child's ends: our copies would keep its pipes open after it is gone
	close_handle(h.out_write);

	Child* c = new Child();
	c->process = pi.hProcess;
	c->pid = pi.dwProcessId;
	c->out_read = h.out_read;
	h.out_read = nullptr;
	c->in_write = h.in_write;
	h.in_write = nullptr;
	c->token = token;
	SecureZeroMemory(token.data(), token.size());
	c->port = port;
	c->asked_port = base.port ? base.port : kDefaultPort;
	c->started_at = GetTickCount64();
	{
		std::lock_guard<std::mutex> lock(*g_lock);
		g = c;
	}
	c->reader = CreateThread(nullptr, 0, &reader_main, c, 0, nullptr);
	if (!c->reader)
	{
		// nobody would read it: closing its input makes it save and quit; the stop's terminate is nobody's either, so now
		std::lock_guard<std::mutex> lock(*g_lock);
		close_handle(c->in_write);
		TerminateProcess(c->process, kKilledCode);
		close_handle(c->out_read);
		c->watch.exit(kKilledCode);
		c->killed = true;
		c->done = true;
		SecureZeroMemory(c->token.data(), c->token.size());
		why = "cannot start its reader thread";
		return false;
	}
	::Msg("[coop] world server: started, process %lu, port %u%s: %s", pi.dwProcessId, u32(port), port != c->asked_port ? " (its port was taken)" : "",
	    shown.c_str());
	return true;
}

bool stop()
{
	if (!g_lock || !g)
		return false;
	std::lock_guard<std::mutex> lock(*g_lock);
	if (g->done)
		return false;
	if (g->in_write)
		::Msg("[coop] world server: stop: its input is closed, it saves the world and quits");
	close_input_locked(*g);
	return true;
}

// ---------------------------------------------------------------- Lua

const char* phase_text(const Watch& w) { return w.phase == Phase::Unknown ? "none" : phase_name(w.phase); }

void set_string(lua_State* L, const char* key, const std::string& v)
{
	lua_pushlstring(L, v.data(), v.size());
	lua_setfield(L, -2, key);
}

void set_number(lua_State* L, const char* key, double v)
{
	lua_pushnumber(L, v);
	lua_setfield(L, -2, key);
}

void set_bool(lua_State* L, const char* key, bool v)
{
	lua_pushboolean(L, v ? 1 : 0);
	lua_setfield(L, -2, key);
}

void push_lines(lua_State* L, const std::vector<std::string>& lines)
{
	lua_createtable(L, int(lines.size()), 0);
	int i = 0;
	for (const std::string& l : lines)
	{
		const std::string cp = utf8_to_cp1251(l);
		lua_pushlstring(L, cp.data(), cp.size());
		lua_rawseti(L, -2, ++i);
	}
}

int fail(lua_State* L, const std::string& why)
{
	lua_pushboolean(L, 0);
	lua_pushlstring(L, why.data(), why.size());
	return 2;
}

// A string field of the argument table: true and `out` if it is a string; `present` says whether it was there at all.
bool string_field(lua_State* L, const char* key, std::string& out, bool& present, size_t max_len)
{
	lua_getfield(L, 1, key);
	present = !lua_isnil(L, -1);
	bool ok = !present;
	if (lua_type(L, -1) == LUA_TSTRING)
	{
		size_t n = 0;
		const char* s = lua_tolstring(L, -1, &n);
		ok = n <= max_len;
		if (ok)
			out.assign(s, n);
	}
	lua_pop(L, 1);
	return ok;
}

// coop.world_server.start{ world, save, port, stage, nick } -> true | false, why
int l_start(lua_State* L)
{
	if (!lua_istable(L, 1))
		return fail(L, "start takes a table: { world = \"new\" | \"load\", ... }");
	LaunchArgs a;
	std::string world;
	bool present = false;
	if (!string_field(L, "world", world, present, 8) || (world != "new" && world != "load"))
		return fail(L, "world must be \"new\" or \"load\"");
	a.world_load = world == "load";
	if (!string_field(L, "save", a.save, present, 200) || (a.world_load && !present))
		return fail(L, "save must be a save's name (or \"last\") for world = \"load\"");
	lua_getfield(L, 1, "stage");
	const bool own_stage = lua_isboolean(L, -1) && lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	if (own_stage)
	{
		// the file Lua wrote at stage_file(): the server reads it from its own data folder by its name (dedicated::stage_is_name), so a
		// folder with blanks in its path is no obstacle
		std::string folder;
		if (!world_folder(folder) || !file_exists(folder + world_stage::kFileName))
			return fail(L, "stage = true needs the file of coop.world_server.stage_file()");
		a.stage = world_stage::kFileName;
	}
	else if (!string_field(L, "stage", a.stage, present, MAX_PATH))
		return fail(L, "stage must be true (the file of stage_file()) or a file's path");
	if (!string_field(L, "nick", a.nick, present, 256))
		return fail(L, "nick must be a string");
	lua_getfield(L, 1, "port");
	if (!lua_isnil(L, -1))
	{
		const lua_Number p = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
		if (p < 1 || p > 65535)
		{
			lua_pop(L, 1);
			return fail(L, "port must be 1..65535");
		}
		a.port = u16(p);
	}
	lua_pop(L, 1);
	std::string why;
	if (!start(a, why))
	{
		::Msg("! [coop] world server: not started: %s", why.c_str());
		return fail(L, why);
	}
	lua_pushboolean(L, 1);
	return 1;
}

// coop.world_server.stage_file() -> the path of the new world's staging file in the world server's data folder (the folder is made),
// or nil. Lua writes the file there (coop_world_stage.h is its format) and starts the server with stage = true.
int l_stage_file(lua_State* L)
{
	std::string folder;
	if (!world_folder(folder))
	{
		lua_pushnil(L);
		return 1;
	}
	const std::string path = folder + world_stage::kFileName;
	lua_pushlstring(L, path.data(), path.size());
	return 1;
}

// coop.world_server.state() -> table | nil
int l_state(lua_State* L)
{
	if (!g_lock || !g)
	{
		lua_pushnil(L);
		return 1;
	}
	std::lock_guard<std::mutex> lock(*g_lock);
	const Child& c = *g;
	const Watch& w = c.watch;
	lua_createtable(L, 0, 24);
	set_string(L, "state", summary_name(summary(w, c.stop_asked)));
	set_string(L, "phase", phase_text(w));
	set_number(L, "port", w.port);
	set_number(L, "asked_port", c.asked_port);
	set_bool(L, "port_moved", c.port != c.asked_port);
	set_string(L, "wait", w.wait_code);
	set_string(L, "wait_text", utf8_to_cp1251(w.wait_text));
	set_number(L, "players", w.players);
	lua_createtable(L, 0, int(w.mapservers.size()));
	for (const auto& it : w.mapservers)
		set_string(L, it.first.c_str(), state_name(it.second));
	lua_setfield(L, -2, "mapservers");
	std::string error = w.error_code, error_text = utf8_to_cp1251(w.error_text);
	if (c.killed)
	{
		error = "killed";
		error_text = "it did not quit in 60 s and was terminated";
	}
	else if (c.ready_timeout && error.empty())
	{
		error = "ready_timeout";
		error_text = "it was not ready in 16 minutes";
	}
	set_string(L, "error", error);
	set_string(L, "error_text", error_text);
	set_bool(L, "exited", w.exited);
	set_number(L, "exit_code", w.exit_code);
	set_bool(L, "crashed", w.crashed);
	set_bool(L, "killed", c.killed);
	set_bool(L, "stopping", c.stop_asked && !w.exited);
	set_number(L, "pid", c.pid);
	set_number(L, "elapsed_ms", double(GetTickCount64() - c.started_at));
	push_lines(L, w.last(kStateLines));
	lua_setfield(L, -2, "lines");
	return 1;
}

// coop.world_server.lines(n) -> { line, ... }
int l_lines(lua_State* L)
{
	size_t n = kStateLines;
	if (lua_isnumber(L, 1))
	{
		const lua_Number v = lua_tonumber(L, 1);
		n = v < 1 ? 1 : (v > lua_Number(kMaxLinesAsked) ? kMaxLinesAsked : size_t(v));
	}
	if (!g_lock || !g)
	{
		lua_createtable(L, 0, 0);
		return 1;
	}
	std::vector<std::string> lines;
	{
		std::lock_guard<std::mutex> lock(*g_lock);
		lines = g->watch.last(n);
	}
	push_lines(L, lines);
	return 1;
}

int l_running(lua_State* L)
{
	lua_pushboolean(L, running() ? 1 : 0);
	return 1;
}

int l_stop(lua_State* L)
{
	lua_pushboolean(L, stop() ? 1 : 0);
	return 1;
}

// coop.machine_memory() -> { phys_total_mb, phys_free_mb, commit_total_mb, commit_free_mb, load } | nil
int l_machine_memory(lua_State* L)
{
	MEMORYSTATUSEX m;
	ZeroMemory(&m, sizeof(m));
	m.dwLength = sizeof(m);
	if (!GlobalMemoryStatusEx(&m))
	{
		lua_pushnil(L);
		return 1;
	}
	const double mb = 1024.0 * 1024.0;
	lua_createtable(L, 0, 5);
	set_number(L, "phys_total_mb", double(m.ullTotalPhys) / mb);
	set_number(L, "phys_free_mb", double(m.ullAvailPhys) / mb);
	set_number(L, "commit_total_mb", double(m.ullTotalPageFile) / mb); // the commit limit: RAM and the page files
	set_number(L, "commit_free_mb", double(m.ullAvailPageFile) / mb);
	set_number(L, "load", m.dwMemoryLoad);
	return 1;
}
} // namespace

bool operator_token_for(uint16_t port, op::Token& out)
{
	if (!g_lock || !g || !port)
		return false;
	std::lock_guard<std::mutex> lock(*g_lock);
	// Only once the server has said it listens there ("@rl ready port=P"): it binds its port minutes after it starts, and until then the
	// port may be anybody's, who would get the token from the game's Hello (security review 2026-11-12).
	if (g->done || g->port != port || g->watch.port != port || op::token_none(g->token))
		return false;
	out = g->token;
	return true;
}

bool running()
{
	if (!g_lock || !g)
		return false;
	std::lock_guard<std::mutex> lock(*g_lock);
	return !g->done;
}

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"start", &l_start},
		{"stage_file", &l_stage_file},
		{"state", &l_state},
		{"status", &l_state},
		{"lines", &l_lines},
		{"running", &l_running},
		{"stop", &l_stop},
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	lua_createtable(L, 0, 6);
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_setfield(L, -2, "world_server"); // coop.world_server
	lua_pushcfunction(L, &l_machine_memory);
	lua_setfield(L, -2, "machine_memory"); // coop.machine_memory
	lua_pop(L, 1); // coop
}
} // namespace world_server
} // namespace coop
