// Anomaly Coop — the game log's close (xrCore log.cpp), pure rules. Tests: tests/test_log_close.cpp.
// Pure C++17, header-only, no engine dependencies.
//
// TZ R38 (RL-N8HP-7FDG, 2026-11-22): a normal quit, and the launcher said "abnormal, exit code 1". The log ended as a clean one
// does ("[xrLogger] InternalCloseLog called"), and there was no dump. The log closes early in the end of the process
// (CLocatorAPI::_destroy); everything after it - the rest of the core's teardown, the static destructors of the CRT's exit, the
// DLLs' detach - wrote into a logger that dropped every line. A failure there (an assert, a pure call, abort, an invalid
// parameter) still went through xrDebug::backend: the launcher's watcher answers its DebugBreak "handled", and the backend ends
// the process with TerminateProcess(1). Shown on the stand: a pure call in ExitProcess after the log closed, exit code 1, the log
// as complete as the owner's (temp/r38). From now on what comes after the close is written straight into the file (late), so
// such an end says why.
#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace coop
{
namespace log_close
{
// Where a line goes. Open: the queue, the log thread prints it. Closed: straight into the file, on the caller's thread.
enum class Route
{
	Queue,
	Late
};
inline Route route(bool alive) { return alive ? Route::Queue : Route::Late; }

// A late line has no time stamp: the stamp (logTimestamps) is std::put_time, and the CRT's exit tears down the C++ locale under
// it (the stand, 2026-11-22: a FATAL's Msg after the close faulted inside std::time_put).
inline bool stamp(bool timestamps, bool alive) { return timestamps && alive; }

// Takes the record at the front, if there is one. The caller holds the queue's lock: the test for an empty queue and the pop are
// one step. Two drainers (the log thread and the thread that closes the log, or several threads in immediate mode) each take a
// record once; the old two steps (empty() under the lock, front() and pop() under a second hold) let the later one pop an empty
// std::deque (undefined: MSVC's size wraps to SIZE_MAX, the next front() reads a block that is not there).
template <class Queue, class Record>
bool take_front(Queue& queue, Record& out)
{
	if (queue.empty())
		return false;
	out = std::move(queue.front());
	queue.pop();
	return true;
}

// The first line that comes after the close, before it.
constexpr const char* kLateHeader = "[xrLogger] the log was closed; the process wrote this after it:";

// The bytes a late line puts at the end of the file: its lines with the file's CRLF (a CRLF stays one), the header before the first
// late line of the process.
inline std::string late_text(const char* text, bool first)
{
	std::string out;
	if (first)
	{
		out += kLateHeader;
		out += "\r\n";
	}
	const char* p = text ? text : "";
	for (; *p; ++p)
	{
		if (*p == '\r' && p[1] == '\n')
			continue; // its '\n' comes next
		if (*p == '\n')
			out += "\r\n";
		else
			out += *p;
	}
	out += "\r\n";
	return out;
}
} // namespace log_close
} // namespace coop
