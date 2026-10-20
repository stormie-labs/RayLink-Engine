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
#pragma once

#include "script_export_space.h"

namespace coop
{
class Session;

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
} // namespace script
} // namespace coop

// Script export entry (export_classes).
struct coop_script_registrator
{
	DECLARE_SCRIPT_REGISTER_FUNCTION
};
