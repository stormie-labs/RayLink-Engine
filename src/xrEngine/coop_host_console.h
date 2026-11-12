// Anomaly Coop — the console window of the dedicated host (docs/DEDICATED_HOST.md). A real Windows console: the exe is a GUI program, so the
// window comes from AllocConsole. A thread reads it, line by line, with the editing and the history of the console itself; the game's main
// thread takes the lines (xrGame/coop/coop_dedicated.cpp). The game's log comes through the logger's callback, filtered by
// xrCoop/coop_dedicated_rules.h log_shown. Closing the window or Ctrl+C asks the game to save and quit.
//
// A standard input that is a pipe or a file at the start (a launcher, a test) is read beside the window.
// -coop_hidden (plan 2026-11-12 stage 1): no window at all, only that input; its end (the launcher or the host's game died) is quit.
// Text in the game is cp1251, the window speaks UTF-16: the conversion is here.
#pragma once

#include "engine.h"

namespace coop_console
{
// WinMain, before the engine's core: the log of the whole start shows.
ENGINE_API void open();
ENGINE_API bool is_open();
// The logger's callback. Any thread.
ENGINE_API void log_line(const char* line);
// 0 errors, 1 the session (the default), 2 everything.
ENGINE_API void set_log_level(int level);
ENGINE_API int log_level();
// Writes a line whatever the log level: answers to the operator.
ENGINE_API void print(const char* line);
ENGINE_API void set_title(const char* title);
// Main thread: the next line typed, false if there is none.
ENGINE_API bool next_line(char* out, size_t size);
// The window is being closed, or Ctrl+C: true once. It stays true for the rest of the process in closing().
ENGINE_API bool quit_asked();
// The game is done with the quit: the handler of the closing window may return.
ENGINE_API void quit_done();
} // namespace coop_console
