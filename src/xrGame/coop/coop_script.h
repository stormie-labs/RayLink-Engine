// Anomaly Coop: the Lua bridge. Registers the `coop` script namespace (export_classes, script_engine_export.cpp): its own functions
// (register_raw in coop_script.cpp) and the tables of the other modules (each module's register_lua, called from there).
//
// Threading: Lua may call coop.* from the GameThread worker, so nothing that sends or calls Lua runs there: coop.send_event and
// the engine's events only queue, and frame() (main thread, after the network pump) sends the queue and delivers to Lua.
//
// Lua RPC between machines (Msg::ScriptEvent): coop.send_event(name, payload, target), target -1 every other player, 0 the world
// server, N player N -> coop_main.on_event(name, payload, from) on the receiving machine(s). Clients talk to the world server only,
// it relays client -> client events (star topology). In the co-op host's process 0 is the world server and the host player at once
// (one Lua state): an event to 0 there goes over the internal channel and comes back to that Lua once, with from = 0.
// coop.reserve_world_event(name) keeps a relay from rewriting that event's sender to peer 0. Events and chat lines wait in a queue
// until this machine is in the world.
//
// Engine -> Lua callbacks: queue_callback(name, {cb_int(..), cb_str(..), ...}) queues SendScriptCallback(name, args...), delivered
// by frame() in the order queued. A missing Lua function or a Lua error is logged and counted, never fatal.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "../../xrServerEntities/script_export_space.h"

struct lua_State;

namespace coop
{
class Session;
class Endpoint;
struct ManifestDiff;
enum class RejectReason : uint8_t;

namespace script
{
// Lua registration can run on the game thread, while relay admission runs in the network pump.
class WorldEventReservations
{
public:
	static constexpr size_t kMaxName = 64;

	static bool valid_name(const char* name, size_t size)
	{
		if (!name || size == 0 || size > kMaxName)
			return false;
		for (size_t i = 0; i < size; ++i)
		{
			const auto c = static_cast<unsigned char>(name[i]);
			if (c <= 0x20 || c >= 0x7f)
				return false;
		}
		return true;
	}

	bool reserve(const char* name, size_t size)
	{
		if (!valid_name(name, size))
			return false;
		const std::string_view key(name, size);
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_names.find(key) != m_names.end())
			return true;
		if (m_names.size() >= kMaxReservations)
			return false;
		m_names.emplace(key);
		return true;
	}

	bool blocks_relay(bool relays_as_host, std::string_view name) const
	{
		if (!relays_as_host)
			return false;
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_names.find(name) != m_names.end();
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_names.clear();
	}

private:
	static constexpr size_t kMaxReservations = 64;
	mutable std::mutex m_mutex;
	std::set<std::string, std::less<>> m_names;
};

// The Msg::ScriptEvent handler, in both tables (installed by role when a session starts).
void install_server(Session& s); // the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // the client's (a remote client's session, the host player's LocalLink)
// Session ended: drops queued events and chat lines, clears counters.
void reset();
// The entity under local id `id` was released on this process (xrCoop/coop_item_rider_rules.h)
// (coop_entities.cpp host_on_entity_released; any game thread). It takes the next tick of the counter that numbers the
// inbox's arrivals: coop.released_after(id, tick) answers from it. Outside a session: nothing.
void note_released(u16 id);
// A refused upgrade rebuilds a local copy of the same instance. Skip its next release, or cancel the mark.
void keep_instance(u16 id);
void unkeep_instance(u16 id);
// Main thread, once per frame after the network pump: sends queued events, delivers received ones.
void frame();
// coop_status section: counters, queues, then coop_main.print_status() (domains, emission mirror).
void print_status();
// The console command coop_parity [tag]: scripts\coop_parity.script (host: every machine dumps, client: refuses).
void parity_command(const char* tag);
// coop_dbg (coop_debug.cpp): <module>.<fn>(the arguments `push` puts on the stack, their count returned) now, protected,
// if this machine's Lua is up in a co-op world. NotReady: nothing called; Failed: the function is missing or Lua raised an error (both
// logged). Main thread.
enum class CallResult : u8
{
	Done,
	NotReady,
	Failed,
};
CallResult call_now(const char* module, const char* fn, int (*push)(lua_State* L, const void* ctx), const void* ctx);
// A chat line arrived: shown in the HUD by coop_main.on_chat(nick, text, from). `from`: the sender's peer id (0 = the host or the world
// server; Lua shows a line of a world server's own seat from "World" by this, never by a nick a player could take); 0xffffffff
// (kInvalidPeer): not known, Lua gets nil.
void on_chat(const char* nick, const char* text, uint32_t from = 0xffffffffu);
// Client: the host refused the handshake. Keeps the reason, the host's text and the mod-list diff; a frame
// pump shows them through coop_ui.on_rejected as soon as a dialog can be opened (main menu or in game).
// Main thread (Session callback). Survives reset(); a newer refusal replaces an unshown one.
void on_rejected(RejectReason reason, const char* text, const ManifestDiff& diff);

// --- co-op menu ---
// A session was started from the console or Lua: remembered for coop.session_info (address nullptr = host).
void note_start(const char* address, u16 port);
// Starting a session failed before it existed (port busy, no game loaded...): shown as the last error.
void note_start_failed(const char* why);
// The next on_session_end comes from the player (coop_disconnect / the menu): not shown as an error.
void note_user_stop();
// The session ended (Manager::stop, transport lost). Main thread. Keeps the reason for the menu. `code`: how it ended
// (Session::end_code), nullptr when this machine stopped it. A client it sends out of a world reads why in the main menu.
void on_session_end(const char* reason, const char* code = nullptr);

// --- engine -> Lua callbacks. Called on the main thread; queued, delivered by frame() in game. ---
// This machine's player was revived -> SendScriptCallback("coop_actor_revived", reviver_peer, medkit_section)
// (peer id: 0 = host; nil if not known; section nil if unknown).
void on_actor_revived(uint32_t reviver_peer, const char* medkit_section);
// Host: a client opened a box or a corpse -> SendScriptCallback("coop_on_container_opened", peer, container_id).
void on_container_opened(uint32_t peer, u16 container_id);

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
