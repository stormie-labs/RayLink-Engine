#include "stdafx.h"
#include "coop_host_console.h"

#include "../xrCoop/coop_dedicated_rules.h"

#include <atomic>
#include <cstring>
#include <deque>
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
std::mutex g_out_lock;
HANDLE g_out = INVALID_HANDLE_VALUE;

std::string to_game(const wchar_t* text, int size)
{
	const int n = WideCharToMultiByte(kGameCodePage, 0, text, size, nullptr, 0, nullptr, nullptr);
	std::string out(size_t(n > 0 ? n : 0), '\0');
	if (n > 0)
		WideCharToMultiByte(kGameCodePage, 0, text, size, &out[0], n, nullptr, nullptr);
	return out;
}

void write_line(const char* line)
{
	const int size = int(strlen(line));
	const int n = MultiByteToWideChar(kGameCodePage, 0, line, size, nullptr, 0);
	std::wstring text(size_t(n > 0 ? n : 0), L'\0');
	if (n > 0)
		MultiByteToWideChar(kGameCodePage, 0, line, size, &text[0], n);
	text += L"\r\n";
	DWORD written = 0;
	std::lock_guard<std::mutex> hold(g_out_lock);
	if (g_out != INVALID_HANDLE_VALUE)
		WriteConsoleW(g_out, text.c_str(), DWORD(text.size()), &written, nullptr);
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
	// -coop_hidden: no window of its own (a process started with CREATE_NO_WINDOW has a console already, AllocConsole would fail and
	// take the reader of the pipe with it); the launcher owns the pipe, and its end means quit
	const coop::dedicated::ConsolePlan plan = coop::dedicated::console_plan(coop::dedicated::has_hidden_key(GetCommandLineA()), stream);
	if (plan.window)
	{
		if (!AllocConsole())
			return;
		g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		SetConsoleTitleW(L"RayLink: dedicated host");
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

bool is_open() { return g_open; }

void log_line(const char* line)
{
	if (g_open && g_window && coop::dedicated::log_shown(line, g_level))
		write_line(line);
}

void set_log_level(int level) { g_level = level; }
int log_level() { return g_level; }

void print(const char* line)
{
	if (g_open && g_window)
		write_line(line);
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
