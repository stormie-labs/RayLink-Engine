// Anomaly Coop — world state owned by the host: one game clock and one sky for the group
// (SPEC R-10.1, R-10.2, P5).
//
// Time:    the host sends TimeSync {ALife game time, ALife time factor, host clock, paused} every second
//          and right after any jump (sleep, scripted skip, load, factor change). A client follows it only
//          when it is off by more than a small tolerance, and never moves its own clock: local time jumps
//          become a request to the host, local factor changes are refused.
// Weather: the host sends WeatherSync {cycle, weather fx + time left, current/next env section + their
//          times, blend} on change and every 5 s. A client applies it and refuses its own script weather.
// Slow motion (engine console `time_factor`) is forced to 1 on every machine during a session.
//
// This header is included from alife_time_manager.h (many translation units): keep it free of includes.
#pragma once

namespace coop
{
class Session;

namespace worldstate
{
void install(Session& s);
void frame(); // main thread, every frame while a session is active
void reset(); // session started / ended
void print_status();

// --- engine hooks (each called from a one-line "Anomaly Coop" hook) ---

// CALifeTimeManager::set_time_factor (level.set_time_factor, al_time_factor, time_factor_single).
// Client in a session: false, the change is refused (the host owns the clock).
// Host: true, and a TimeSync goes out at once. No session: true.
bool allow_time_factor_change(float factor);

// level.change_game_time (Anomaly sleep, fast travel, scripted skips).
// Client in a session: true, the caller must not move time; a request goes to the host, which shows
// "[coop] <nick> wants to sleep" (R-10.2 v1: the host decides, nothing else happens).
// Host: false (the jump happens) and a TimeSync goes out at once. No session: false.
bool intercept_time_jump(u32 days, u32 hours, u32 mins);

// level.set_weather / set_weather_smooth / set_weather_weight / set_weather_fx /
// start_weather_fx_from_time / stop_weather_fx.
// Client in a session: true, the call is refused (Anomaly weather scripts are random per machine).
// Host: false, and a WeatherSync goes out at once. No session: false.
bool refuse_script_weather(const char* what);

// Console `time_factor` / `freeze_time` (engine device time scale, slow motion).
// True during a session for any value other than 1: refused with a log line.
bool refuse_engine_time_factor(float factor);
} // namespace worldstate
} // namespace coop
