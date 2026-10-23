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
//
// Glue for the W4 scripts (W5 C1):
//   coop.spot_is_local(type) -> bool            map spot type never replicated (engine-local, prefix "coop_")
//   coop.story_hold_infos(on) -> bool           client: infos given while on stay on this machine (coop_story.h)
//   coop.story_local_task(id[, on=true]) -> bool  personal engine task, never routed to / mirrored from the host
//   coop.life_of(local_id) -> "alive"|"wounded"|"dead"|"spectating"|"respawning"   (0 = me; nil if no session)
//   coop.bleed_left(local_id) -> ms             bleed-out left of a wounded player, 0 otherwise
//   coop.player_safemode(peer) -> bool | nil    weapon lowered (Anomaly safe mode); nil if not known yet
//   coop.revive_progress() -> nil | { reviving = {peer, avatar_id, nick, progress, ms_left, item, section},
//     revived_by = {peer, avatar_id, nick, progress, ms_left}, give_up = {progress, ms_left}, bleed = {left_ms, total_ms} }
//     (HUD v2 part 2: the USE holds of a revive; parts that do not apply are absent; nil: nothing to show / no session;
//     calling it tells the engine a Lua HUD draws the down / revive state, its own text steps aside)
// Engine callbacks fired from frame(): coop_main.on_transition_vote(nick) (host), SendScriptCallback
// "coop_actor_revived" (reviver_peer, medkit_section), "coop_revive_done" (target_peer, medkit_section: the reviver's
// machine, his revive went through; HUD v2 part 2) and "coop_on_container_opened" (peer, container_id).
// F3 modules fire their own through queue_callback(name, {cb_int(..), cb_str(..), ...}).
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

#include "script_export_space.h"

namespace coop
{
class Session;
class Endpoint;
struct ManifestDiff;
enum class RejectReason : uint8_t;

namespace script
{
// The Msg::ScriptEvent handler, in both tables (F3a T4: installed by role when a session starts).
void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
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

// --- engine events for Lua (W5). Called on the main thread; queued, delivered by frame() in game. ---
// Host: a client stands at a level exit -> coop_main.on_transition_vote(nick).
void on_transition_vote(const char* nick);
// This machine's player was revived -> SendScriptCallback("coop_actor_revived", reviver_peer, medkit_section)
// (peer id: 0 = host; section nil if unknown).
void on_actor_revived(uint32_t reviver_peer, const char* medkit_section);
// Host: a client opened a box or a corpse -> SendScriptCallback("coop_on_container_opened", peer, container_id).
void on_container_opened(uint32_t peer, u16 container_id);

// --- engine -> Lua callbacks of the F3 modules (checkpoints, death, rest, autosave) ---
// One argument of SendScriptCallback: nil, a boolean, an integer or a string.
struct CallbackArg
{
	enum Kind : u8
	{
		Nil,
		Bool,
		Int,
		Str
	} kind = Nil;
	s64 i = 0; // Bool: 0 / 1, Int: the value (a Lua number: exact up to 2^53)
	std::string s; // Str
};
CallbackArg cb_nil();
CallbackArg cb_bool(bool v);
CallbackArg cb_int(s64 v);
CallbackArg cb_str(const char* v); // nullptr -> nil
// Queues SendScriptCallback(name, args...): delivered by frame() on the main thread once this machine is in the world,
// in the order queued, with the other engine events (same queue limit; dropped if older than 2 minutes). Main thread.
// A name that is not 1..64 printable characters without spaces, more than kMaxCallbackArgs arguments or a string
// argument over kMaxCallbackStr bytes is a bug of the caller: the callback is dropped, logged and counted.
constexpr size_t kMaxCallbackArgs = 8;
constexpr size_t kMaxCallbackStr = 4096;
void queue_callback(const char* name, std::initializer_list<CallbackArg> args);
} // namespace script

// "1.2.3.4:5678" / "1.2.3.4" / "[::1]:5678" -> address + port (kDefaultPort if none). coop_console.cpp.
bool parse_endpoint(LPCSTR args, xr_string& address, u16& port);
} // namespace coop

// Script export entry (export_classes).
struct coop_script_registrator
{
	DECLARE_SCRIPT_REGISTER_FUNCTION
};
