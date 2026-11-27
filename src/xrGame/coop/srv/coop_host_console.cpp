#include "stdafx.h"
#include "coop_host_console.h"

#include "../../../xrCoop/srv/coop_dedicated_rules.h"
#include "../../../xrCoop/srv/coop_line_writer.h"
#include "../../../xrCoop/srv/coop_world_server_rules.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <process.h>
#include <mutex>
#include <string>

namespace coop_console
{
namespace
{
constexpr UINT kGameCodePage = 1251;
constexpr size_t kMaxQueuedLines = 64;
constexpr DWORD kCloseWaitMs = 4500; // Windows ends a closing console's process after 5 s: the world's last save has this long

std::atomic<bool> g_open{false};
std::atomic<bool> g_window{false}; // false with -coop_hidden: the console of the process is nobody's, its title is not ours to set
std::atomic<bool> g_eof_quits{false}; // -coop_hidden: the end of the launcher's pipe is quit
std::atomic<int> g_level{coop::dedicated::kLogDefault};
std::atomic<bool> g_quit_asked{false};
std::atomic<bool> g_quit_done{false};
std::mutex g_lines_lock;
std::deque<std::string> g_lines;
// No game thread writes to the window or the pipe: either can hold a write without end (a paused window, a parent that does not read),
// and the log's callback runs on the game's threads under the logger's locks. Each output has its own coop::world_server::LineWriter: the
// game pushes, its one thread writes (coop_line_writer.h). The writers are made once and never destroyed: the logger calls log_line up to
// the DLLs' detach, after this file's static objects are gone.
using coop::world_server::LineWriter;
using coop::world_server::OutputQueue;

// Set before its writer starts, never changed after: read by that writer's thread only.
HANDLE g_out = INVALID_HANDLE_VALUE;
HANDLE g_stdout = INVALID_HANDLE_VALUE;

// The window takes log lines only, never @rl lines.
LineWriter& window()
{
	static LineWriter* const w =
	    new LineWriter(OutputQueue(OutputQueue::kLogLines, OutputQueue::kRlLines, "console", "the window's output was paused"));
	return *w;
}

// -coop_hidden with a redirected standard output: the log and the @rl lines in UTF-8.
LineWriter& pipe()
{
	static LineWriter* const w = new LineWriter();
	return *w;
}

std::string to_game(const wchar_t* text, int size)
{
	const int n = WideCharToMultiByte(kGameCodePage, 0, text, size, nullptr, 0, nullptr, nullptr);
	std::string out(size_t(n > 0 ? n : 0), '\0');
	if (n > 0)
		WideCharToMultiByte(kGameCodePage, 0, text, size, &out[0], n, nullptr, nullptr);
	return out;
}

// The window's writer thread only: WriteConsoleW waits while the window's output is paused. A failed write loses that line, the window
// stays.
bool write_window(const std::string& line)
{
	const int size = int(line.size());
	const int n = MultiByteToWideChar(kGameCodePage, 0, line.data(), size, nullptr, 0);
	std::wstring text(size_t(n > 0 ? n : 0), L'\0');
	if (n > 0)
		MultiByteToWideChar(kGameCodePage, 0, line.data(), size, &text[0], n);
	text += L"\r\n";
	DWORD written = 0;
	WriteConsoleW(g_out, text.c_str(), DWORD(text.size()), &written, nullptr);
	return true;
}

// The pipe's writer thread only. A failed write: the parent closed its end, nobody will read the rest.
bool write_pipe(const std::string& line)
{
	const std::string bytes = coop::world_server::output_line(line);
	DWORD written = 0;
	return WriteFile(g_stdout, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
}

// The writers' threads. A refused thread leaves that writer off and the server runs on without that output, as the engine's own
// thread_spawn runs on after the same _beginthread fails.
bool start_thread(void (*entry)(void*), void* arg) { return _beginthread(entry, 0, arg) != uintptr_t(-1); }

// A line of the log or an answer: the window and the pipe, each in the order of the calls.
void push_log(const char* line)
{
	if (!line)
		return;
	if (g_window)
		window().push(line, false);
	pipe().push(line, false);
}

void push_line(std::string line)
{
	std::lock_guard<std::mutex> hold(g_lines_lock);
	if (g_lines.size() < kMaxQueuedLines)
		g_lines.push_back(std::move(line));
}

DWORD WINAPI read_window(LPVOID)
{
	const HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (in == INVALID_HANDLE_VALUE)
		return 1;
	wchar_t text[1024];
	for (;;)
	{
		DWORD got = 0;
		if (!ReadConsoleW(in, text, 1023, &got, nullptr))
			return 1;
		push_line(to_game(text, int(got)));
	}
}

// A pipe or a file: the bytes are the game's own text, a line ends at a line feed.
DWORD WINAPI read_stream(LPVOID handle)
{
	std::string line;
	char chunk[512];
	for (;;)
	{
		DWORD got = 0;
		if (!ReadFile(HANDLE(handle), chunk, sizeof(chunk), &got, nullptr) || !got)
		{
			if (!line.empty())
				push_line(line);
			if (g_eof_quits)
				g_quit_asked = true; // the launcher, or the host's game that owned it, is gone: save the world and go
			return 0;
		}
		for (DWORD i = 0; i < got; ++i)
		{
			if (chunk[i] != '\n')
			{
				line.push_back(chunk[i]);
				continue;
			}
			push_line(line);
			line.clear();
		}
	}
}

BOOL WINAPI on_control(DWORD event)
{
	switch (event)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
		g_quit_asked = true;
		return TRUE;
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		g_quit_asked = true;
		for (DWORD waited = 0; waited < kCloseWaitMs && !g_quit_done; waited += 50)
			Sleep(50);
		return TRUE;
	default: return FALSE;
	}
}
} // namespace

void open()
{
	if (g_open)
		return;
	// what the process was started with, before the console takes the standard handles
	const HANDLE given = GetStdHandle(STD_INPUT_HANDLE);
	const DWORD kind = given && given != INVALID_HANDLE_VALUE ? GetFileType(given) : FILE_TYPE_UNKNOWN;
	const bool stream = kind == FILE_TYPE_PIPE || kind == FILE_TYPE_DISK;
	const HANDLE given_out = GetStdHandle(STD_OUTPUT_HANDLE);
	const DWORD out_kind = given_out && given_out != INVALID_HANDLE_VALUE ? GetFileType(given_out) : FILE_TYPE_UNKNOWN;
	const bool out_stream = out_kind == FILE_TYPE_PIPE || out_kind == FILE_TYPE_DISK;
	// -coop_hidden: no window of its own (a process started with CREATE_NO_WINDOW has a console already, AllocConsole would fail and
	// take the reader of the pipe with it); the launcher owns the pipe, and its end means quit. A redirected output gets the log and @rl.
	const coop::dedicated::ConsolePlan plan =
	    coop::dedicated::console_plan(coop::dedicated::has_hidden_key(GetCommandLineA()), stream, out_stream);
	if (plan.write_output)
	{
		g_stdout = given_out;
		pipe().start(write_pipe, start_thread);
		rl_line(coop::world_server::format(coop::world_server::Record::of_phase(coop::world_server::Phase::Starting)).c_str());
	}
	if (plan.window)
	{
		if (!AllocConsole())
			return;
		g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		SetConsoleTitleW(L"RayLink: dedicated host");
		if (g_out != INVALID_HANDLE_VALUE)
			window().start(write_window, start_thread);
	}
	SetConsoleCtrlHandler(on_control, TRUE);
	g_window = plan.window;
	g_eof_quits = plan.eof_quits;
	g_open = true;
	const HANDLE window = plan.window ? CreateThread(nullptr, 0, read_window, nullptr, 0, nullptr) : nullptr;
	if (window)
		CloseHandle(window);
	const HANDLE reader = plan.read_input ? CreateThread(nullptr, 0, read_stream, given, 0, nullptr) : nullptr;
	if (reader)
		CloseHandle(reader);
}

void log_line(const char* line)
{
	if (!g_open || !coop::dedicated::log_shown(line, g_level))
		return;
	push_log(line);
}

void rl_line(const char* line)
{
	if (line)
		pipe().push(line, true);
}

bool output_on() { return pipe().on(); }

bool flush_output(unsigned ms) { return pipe().flush(ms); }

bool flush_window(unsigned ms) { return window().flush(ms); }

void stop_output()
{
	window().close();
	pipe().close();
}

void set_log_level(int level) { g_level = level; }
int log_level() { return g_level; }

void print(const char* line)
{
	if (g_open)
		push_log(line);
}

void set_title(const char* title)
{
	if (!g_open || !g_window)
		return;
	const int n = MultiByteToWideChar(kGameCodePage, 0, title, -1, nullptr, 0);
	std::wstring text(size_t(n > 0 ? n : 1), L'\0');
	if (n > 0)
		MultiByteToWideChar(kGameCodePage, 0, title, -1, &text[0], n);
	SetConsoleTitleW(text.c_str());
}

bool next_line(char* out, size_t size)
{
	std::lock_guard<std::mutex> hold(g_lines_lock);
	if (g_lines.empty() || !size)
		return false;
	strncpy_s(out, size, g_lines.front().c_str(), _TRUNCATE);
	g_lines.pop_front();
	return true;
}

bool quit_asked() { return g_quit_asked; }
void quit_done() { g_quit_done = true; }
} // namespace coop_console
