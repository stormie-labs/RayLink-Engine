// Anomaly Coop — Lua bridge.
//
// Exports the `coop` script namespace (registered from export_classes, script_engine_export.cpp)
// and carries Lua RPC between machines as Msg::ScriptEvent:
//   coop.send_event(name, payload, target)   target: -1 everyone else, 0 host, N peer N
//   -> coop_main.on_event(name, payload, from) on the receiving machine(s).
// Clients talk only to the host; the host relays client -> client events (star topology).
//
// Threading: Lua may call coop.* from the GameThread worker, so send_event only queues; frame()
// (main thread, after Session::update) sends the queue and delivers received events to Lua.
// Incoming events and chat lines wait in a queue until this machine is in the world (P6).
// A missing coop_main function or a Lua error is logged and counted, never fatal (R-11.4).
//
// Also (W3d):
//   coop.stash_set(key, value) / coop.stash_get(key)  strings kept for the whole session on this machine,
//     across level loads and Lua state rebuilds (coop_settings keeps the host's settings there).
//   on_rejected() -> coop_ui.on_rejected(info): the host's refusal (mod-list diff, duplicate key, ...)
//     shown in a window in the main menu or in game (R-1.3, R-1.4).
//
// Co-op menu (W4 menu, scripts\coop_menu.script):
//   coop.session_info() -> { phase, active, host, client, live, self_id, nick, port, address, players = {...},
//     max_players, default_port, last_error, last_error_code, last_error_age, last_by_user, pending_host,
//     pending_age }
//   coop.host(port) / coop.connect("ip[:port]") -> true | false, reason      coop.disconnect() -> bool
//   coop.host_when_ready(port | nil)  host as soon as the next world is loaded (save, last save, new game);
//     nil or 0 cancels. Kept by a frame pump that also runs in the main menu.
#pragma once

#include <cstdint>

#include "script_export_space.h"

namespace coop
{
class Session;
struct ManifestDiff;
enum class RejectReason : uint8_t;

namespace script
{
// Registers the Msg::ScriptEvent handler. Called once with the manager's session.
void install(Session& s);
// Session ended: drops queued events and chat lines, clears counters.
void reset();
// Main thread, once per frame after the network pump: sends queued events, delivers received ones.
void frame();
// coop_status section: counters, queues, then coop_main.print_status() (domains, emission mirror).
void print_status();
// A chat line from another player arrived: shown in the HUD by coop_main.on_chat(nick, text).
void on_chat(const char* nick, const char* text);
// Client: the host refused the handshake. Keeps the reason, the host's text and the mod-list diff; a frame
// pump shows them through coop_ui.on_rejected as soon as a dialog can be opened (main menu or in game).
// Main thread (Session callback). Survives reset(); a newer refusal replaces an unshown one.
void on_rejected(RejectReason reason, const char* text, const ManifestDiff& diff);

// --- co-op menu (W4) ---
// A session was started from the console or Lua: remembered for coop.session_info (address nullptr = host).
void note_start(const char* address, u16 port);
// Starting a session failed before it existed (port busy, no game loaded...): shown as the last error.
void note_start_failed(const char* why);
// The next on_session_end comes from the player (coop_disconnect / the menu): not shown as an error.
void note_user_stop();
// The session ended (Manager::stop, transport lost). Main thread. Keeps the reason for the menu.
void on_session_end(const char* reason);
} // namespace script

// "1.2.3.4:5678" / "1.2.3.4" / "[::1]:5678" -> address + port (kDefaultPort if none). coop_console.cpp.
bool parse_endpoint(LPCSTR args, xr_string& address, u16& port);
} // namespace coop

// Script export entry (export_classes).
struct coop_script_registrator
{
	DECLARE_SCRIPT_REGISTER_FUNCTION
};
