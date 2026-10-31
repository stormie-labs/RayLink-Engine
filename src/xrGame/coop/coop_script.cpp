#include "stdafx.h"
#include "pch_script.h"
#include "coop_script.h"

#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_story.h"
#include "coop_survival.h"
#include "coop_players.h" // players::avatar_safemode (coop_equip.cpp)
#include "coop_instrument.h"
#include "coop_dialog.h" // W11: dialog::host_talk_holder
#include "coop_requests.h" // R3 В2 review: requests::lua_explode_as
#include "coop_money.h" // F2 T1: money::register_lua
#include "coop_router.h" // F2 T10: router::register_lua (coop.route)
#include "coop_sync_api.h" // F2 T14: syncapi::register_lua (coop.sync)
#include "coop_game_settings.h" // Game settings T2: game::register_lua (coop.game)
#include "coop_levels.h" // F3a: levels::register_lua (coop.levels, the roles)
#include "xrCoop/coop_limits.h" // F2 T14: limits::Pacer (Ruling F2-14)

#include "ai_space.h"
#include "script_engine.h"
#include "Level.h"
#include "Actor.h"
#include "MainMenu.h"
#include "UIGameCustom.h"

#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace luabind;

namespace coop
{
namespace script
{
namespace
{
constexpr size_t kMaxName = 64; // event name: printable ASCII, no spaces
constexpr size_t kMaxPayload = 16 * 1024;
constexpr size_t kMaxOutbox = 512; // events queued by Lua, sent in frame()
constexpr size_t kMaxInbox = 512; // events waiting for Lua (P6)
constexpr size_t kMaxChat = 64;
constexpr u32 kDeliverPerFrame = 64;
constexpr u64 kStaleMs = 120000; // an event older than this is not delivered any more
constexpr s32 kTargetAll = -1;
constexpr size_t kMaxStashKeys = 32;
constexpr size_t kMaxStashValue = 256 * 1024;
constexpr size_t kMaxListed = 300; // diff entries per category handed to Lua (the log has all of them)
constexpr u64 kRejectWaitMs = 300000; // a refusal that found no usable UI for 5 minutes is dropped

struct Outgoing
{
	s32 target = kTargetAll;
	std::string name, payload;
};

struct Incoming
{
	PeerId from = kInvalidPeer;
	std::string name, payload;
	u64 t_ms = 0;
};

struct ChatLine
{
	std::string nick, text;
	u64 t_ms = 0;
};

// Engine facts for Lua (W5), queued by engine code, delivered from frame().
struct EngineEvent
{
	enum Kind : u8
	{
		TransitionVote, // coop_main.on_transition_vote(text = nick)
		ActorRevived, // SendScriptCallback("coop_actor_revived", peer, text = medkit section or nil)
		ContainerOpened, // SendScriptCallback("coop_on_container_opened", peer, id)
		Callback, // F3: SendScriptCallback(name, args...) (queue_callback)
	} kind = TransitionVote;
	std::string text;
	PeerId peer = kInvalidPeer;
	u16 id = 0xffff;
	u64 t_ms = 0;
	std::string name; // Callback
	std::vector<CallbackArg> args; // Callback, at most kMaxCallbackArgs
};
constexpr size_t kMaxEngineEvents = 64;
// send_script_callback reserves 16 Lua stack slots: traceback, SendScriptCallback, the name, then the arguments.
static_assert(kMaxCallbackArgs + 3 <= 16, "queue_callback arguments must fit the stack send_script_callback checks");

struct Stats
{
	u64 sent = 0; // events handed to the network (host: per message, not per receiver)
	u64 received = 0; // valid events received from the network
	u64 relayed = 0; // host: client events forwarded to other clients
	u64 delivered = 0; // coop_main.on_event calls that returned normally
	u64 send_rejected = 0; // coop.send_event refused (bad args, not ready, to myself, queue full)
	u64 send_held = 0; // F2 T14 (Ruling F2-14): frames a client held events back for the host's rate limit
	u64 drop_invalid = 0; // malformed / bad name / payload too big / bad target
	u64 drop_not_ready = 0; // host: sender is still loading the world (P6)
	u64 drop_overflow = 0; // inbox or chat queue full
	u64 drop_stale = 0; // waited too long for Lua
	u64 lua_errors = 0; // Lua error inside coop_main.on_event / on_chat / print_status
	u64 lua_missing = 0; // coop_main or the function is missing
	u64 chat_delivered = 0;
	u64 engine_events = 0, engine_delivered = 0, engine_dropped = 0; // W5 engine callbacks
	u64 engine_invalid = 0; // F3: queue_callback refused (bad name, too many or too long arguments)
};

// Host refusal waiting for a dialog (R-1.3, R-1.4). Kept outside Stats: reset() must not drop it.
struct Rejection
{
	bool pending = false;
	RejectReason reason = RejectReason::None;
	std::string text;
	std::vector<std::string> missing, extra, differ; // first kMaxListed of each
	u32 missing_total = 0, extra_total = 0, differ_total = 0;
	u64 t_ms = 0;
};

struct UiStats
{
	u64 rejections = 0; // refusals received
	u64 shown = 0; // coop_ui.on_rejected calls that returned normally
	u64 failed = 0; // coop_ui missing or Lua error
	u64 expired = 0; // no usable UI in time
	u64 stash_cleared = 0; // session ended with stashed values
};

// Co-op menu (W4): what the window shows besides the live session, and "host when the next world is loaded".
// Survives reset(); main thread only (console, menu callbacks, frame pump).
struct MenuMemo
{
	u16 port = 0; // host: listen port, client: target port (0 = unknown, e.g. the -coop_host autostart)
	std::string address; // client: target address
	std::string last_error; // why the last session ended or could not start ("" = nothing to show)
	std::string last_code; // RejectReason name for refusals, else "start" / "lost" / "left"
	bool last_by_user = false;
	u64 last_ms = 0;
	u64 rejected_ms = 0; // a refusal is more precise than the disconnect that follows it
	bool user_stop = false; // the next on_session_end was asked for by the player
	bool pending = false; // host as soon as a world newer than pending_gen is up
	u16 pending_port = 0;
	u32 pending_gen = 0;
	u64 pending_ms = 0;
	u64 ready_since = 0;
};
constexpr u64 kPendingMaxMs = 15 * 60 * 1000; // a forgotten "host after loading" must not open a port hours later
constexpr u64 kPendingSettleMs = 2000; // a new game changes level right after its first frame
constexpr u64 kRejectShadowMs = 3000;

MenuMemo g_menu;

Stats g_stats;
std::deque<Outgoing> g_outbox;
std::deque<Incoming> g_inbox;
std::deque<ChatLine> g_chat;
std::deque<EngineEvent> g_engine;
Rejection g_reject;
UiStats g_ui;
std::map<std::string, std::string> g_stash; // survives reset(): cleared by the pump when the session ends
size_t g_stash_bytes = 0;

// First lines in full, then every 100th, so a broken addon cannot flood the log.
bool log_allowed(u64 count) { return count <= 10 || count % 100 == 0; }

bool valid_name(const char* s, size_t n)
{
	if (!s || n == 0 || n > kMaxName)
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if (c <= 0x20 || c >= 0x7f)
			return false;
	}
	return true;
}

bool lua_ready()
{
	if (!g_pGameLevel || !g_actor)
		return false;
	return mgr().is_host() || mgr().is_live_client();
}

void write_event(Writer& w, PeerId origin, s32 target, const std::string& name, const std::string& payload)
{
	w.u32(origin);
	w.i32(target);
	w.str(name);
	w.str(payload);
}

// --- calling Lua -------------------------------------------------------------------------------

int lua_traceback(lua_State* L)
{
	const char* msg = lua_tostring(L, 1);
	if (!msg)
		msg = "(error object is not a string)";
	lua_pushstring(L, "debug");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_pushstring(L, msg);
		return 1;
	}
	lua_getfield(L, -1, "traceback");
	if (!lua_isfunction(L, -1))
	{
		lua_pop(L, 2);
		lua_pushstring(L, msg);
		return 1;
	}
	lua_pushstring(L, msg);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

// Pushes <module>.<fn>; false (stack unchanged) if the script or the function does not exist.
bool push_handler(lua_State* L, const char* module, const char* fn)
{
	lua_getglobal(L, module); // _G.__index auto-loads gamedata/scripts/<module>.script
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return false;
	}
	lua_getfield(L, -1, fn);
	lua_remove(L, -2);
	if (!lua_isfunction(L, -1))
	{
		lua_pop(L, 1);
		return false;
	}
	return true;
}

// Protected call of <module>.<fn>(args pushed by push_args). Never throws, never leaves the stack dirty.
template <typename PushArgs>
bool call_module(const char* module, const char* fn, PushArgs push_args)
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return false;
	const int top = lua_gettop(L);
	if (!lua_checkstack(L, 16))
		return false;
	lua_pushcfunction(L, &lua_traceback);
	const int errfunc = lua_gettop(L);
	if (!push_handler(L, module, fn))
	{
		++g_stats.lua_missing;
		if (log_allowed(g_stats.lua_missing))
			::Msg("! [coop] lua: %s.%s is missing (gamedata/scripts/%s.script not installed?)", module, fn, module);
		lua_settop(L, top);
		return false;
	}
	const int nargs = push_args(L);
	bool ok = true;
	if (lua_pcall(L, nargs, 0, errfunc) != 0)
	{
		ok = false;
		++g_stats.lua_errors;
		if (log_allowed(g_stats.lua_errors))
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] lua error in %s.%s (#%llu, session continues): %s", module, fn, g_stats.lua_errors,
			    err ? err : "?");
		}
	}
	lua_settop(L, top);
	return ok;
}

template <typename PushArgs>
bool call_lua(const char* fn, PushArgs push_args)
{
	return call_module("coop_main", fn, push_args);
}

// Protected SendScriptCallback(name, args...) (Anomaly _g.script). A name nobody AddScriptCallback-ed is not
// fatal there: axr_main.make_callback prints an error with a call stack and returns.
template <typename PushArgs>
bool send_script_callback(const char* name, PushArgs push_args)
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return false;
	const int top = lua_gettop(L);
	if (!lua_checkstack(L, 16))
		return false;
	lua_pushcfunction(L, &lua_traceback);
	const int errfunc = lua_gettop(L);
	lua_getglobal(L, "SendScriptCallback");
	if (!lua_isfunction(L, -1))
	{
		++g_stats.lua_missing;
		if (log_allowed(g_stats.lua_missing))
			::Msg("! [coop] lua: SendScriptCallback is missing, '%s' not fired", name);
		lua_settop(L, top);
		return false;
	}
	lua_pushstring(L, name);
	const int nargs = 1 + push_args(L);
	bool ok = true;
	if (lua_pcall(L, nargs, 0, errfunc) != 0)
	{
		ok = false;
		++g_stats.lua_errors;
		if (log_allowed(g_stats.lua_errors))
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] lua error in callback %s (#%llu, session continues): %s", name, g_stats.lua_errors,
			    err ? err : "?");
		}
	}
	lua_settop(L, top);
	return ok;
}

// --- network -----------------------------------------------------------------------------------

void deliver_later(PeerId from, std::string&& name, std::string&& payload)
{
	if (g_inbox.size() >= kMaxInbox)
	{
		g_inbox.pop_front();
		++g_stats.drop_overflow;
		if (log_allowed(g_stats.drop_overflow))
			::Msg("! [coop] script event queue full, oldest event dropped");
	}
	Incoming e;
	e.from = from;
	e.name = std::move(name);
	e.payload = std::move(payload);
	e.t_ms = mgr().now_ms();
	g_inbox.push_back(std::move(e));
}

void host_relay(PeerId origin, s32 target, const std::string& name, const std::string& payload)
{
	Writer w(name.size() + payload.size() + 16);
	write_event(w, origin, target, name, payload);
	if (target == kTargetAll)
		net::host_broadcast(Msg::ScriptEvent, w, Lane::Reliable, origin);
	else
		net::host_send(PeerId(target), Msg::ScriptEvent, w);
	++g_stats.relayed;
}

bool read_event(PeerId from, Reader& r, PeerId& wire_origin, s32& target, std::string& name, std::string& payload)
{
	wire_origin = r.u32();
	target = r.i32();
	name = r.str();
	payload = r.str();
	if (r.ok() && valid_name(name.data(), name.size()) && payload.size() <= kMaxPayload)
		return true;
	++g_stats.drop_invalid;
	if (log_allowed(g_stats.drop_invalid))
		::Msg("! [coop] malformed script event from #%u dropped", from);
	return false;
}

// The world server (the host's process): a player's event. F3a T7: the host player's comes from peer 0 over the internal
// channel (his coop.send_event to 0); the Lua of the host's process is the world server's and the host player's at once,
// so an event to 0 from either side is one delivery there, with from = 0.
void server_on_script_event(PeerId from, Reader& r)
{
	PeerId wire_origin = kInvalidPeer;
	s32 target = kTargetAll;
	std::string name, payload;
	if (!read_event(from, r, wire_origin, target, name, payload) || !mgr().is_host())
		return;
	// P6: the sender must be in the world; a loading client has no business sending game events.
	const bool ready = from == kHostPeer ? mgr().session().local() != nullptr :
	    mgr().session().peer(from) != nullptr && !sync::host_peer_joining(from);
	if (!ready)
	{
		++g_stats.drop_not_ready;
		if (log_allowed(g_stats.drop_not_ready))
			::Msg("! [coop] script event '%s' from #%u dropped: sender is not in the world yet", name.c_str(), from);
		return;
	}
	const PeerId origin = from; // the wire origin of a client is never trusted
	++g_stats.received;
	if (target == kTargetAll)
	{
		host_relay(origin, target, name, payload);
		if (from != kHostPeer) // the host player's own event to everybody else: not back to the Lua that sent it
			deliver_later(origin, std::move(name), std::move(payload));
	}
	else if (target == s32(kHostPeer))
		deliver_later(origin, std::move(name), std::move(payload));
	else if (target < 0 || PeerId(target) == from || !mgr().session().peer(PeerId(target)))
	{
		++g_stats.drop_invalid;
		if (log_allowed(g_stats.drop_invalid))
			::Msg("! [coop] script event '%s' from #%u to #%d dropped: no such player", name.c_str(), from, target);
	}
	else
		host_relay(origin, target, name, payload);
}

// A client: an event the world server relays. F3a T7: in the host's process the world server delivered every event its
// Lua is to get already (server_on_script_event): the copy a broadcast puts on the internal channel is not delivered
// twice, only a remote client takes it.
void client_on_script_event(PeerId from, Reader& r)
{
	PeerId wire_origin = kInvalidPeer;
	s32 target = kTargetAll;
	std::string name, payload;
	if (!read_event(from, r, wire_origin, target, name, payload) || !mgr().is_client())
		return;
	if (from != kHostPeer)
		return; // star topology: everything comes through the host
	++g_stats.received;
	deliver_later(wire_origin, std::move(name), std::move(payload));
}

// F2 T14 (Ruling F2-14): a client sends events only as fast as the host takes them (kClientRateRules ScriptEvent): the
// host drops what exceeds its limit and the sender never learns of it, so an event over it waits here for a later frame
// instead (send_event refuses only when this queue is full).
limits::Pacer g_pacer;

void flush_outbox()
{
	if (g_outbox.empty())
		return;
	const bool host = mgr().is_host();
	const bool client = mgr().is_live_client();
	while (!g_outbox.empty())
	{
		if (client && !g_pacer.take(Msg::ScriptEvent, mgr().now_ms()))
		{
			++g_stats.send_held; // F2 T14: the rest goes in a later frame, in order
			break;
		}
		Outgoing o = std::move(g_outbox.front());
		g_outbox.pop_front();
		if (!host && !client)
		{
			++g_stats.send_rejected; // session ended or left the world before the frame
			continue;
		}
		Writer w(o.name.size() + o.payload.size() + 16);
		write_event(w, mgr().self(), o.target, o.name, o.payload);
		if (host)
		{
			// F3a T7: to every other player, the host player being the sender; to 0, over the internal channel to the
			// world server, which delivers it to the one Lua of the host's process (server_on_script_event)
			if (o.target == kTargetAll)
				net::host_broadcast(Msg::ScriptEvent, w, Lane::Reliable, kHostPeer);
			else if (o.target == s32(kHostPeer))
				net::client_send(Msg::ScriptEvent, w);
			else if (mgr().session().peer(PeerId(o.target)))
				net::host_send(PeerId(o.target), Msg::ScriptEvent, w);
			else
			{
				++g_stats.send_rejected; // the player left meanwhile
				continue;
			}
		}
		else
			net::client_send(Msg::ScriptEvent, w);
		++g_stats.sent;
	}
}

void deliver_inbox()
{
	u32 budget = kDeliverPerFrame;
	const u64 now = mgr().now_ms();
	while (!g_inbox.empty() && budget-- > 0)
	{
		Incoming e = std::move(g_inbox.front());
		g_inbox.pop_front();
		if (now - e.t_ms > kStaleMs)
		{
			++g_stats.drop_stale;
			if (log_allowed(g_stats.drop_stale))
				::Msg("! [coop] script event '%s' from #%u is too old, dropped", e.name.c_str(), e.from);
			continue;
		}
		const bool ok = call_lua("on_event", [&e](lua_State* L)
		{
			lua_pushlstring(L, e.name.data(), e.name.size());
			lua_pushlstring(L, e.payload.data(), e.payload.size());
			lua_pushnumber(L, lua_Number(e.from));
			return 3;
		});
		if (ok)
			++g_stats.delivered;
	}
}

void deliver_chat()
{
	const u64 now = mgr().now_ms();
	while (!g_chat.empty())
	{
		ChatLine c = std::move(g_chat.front());
		g_chat.pop_front();
		if (now - c.t_ms > kStaleMs)
		{
			++g_stats.drop_stale;
			continue;
		}
		if (call_lua("on_chat", [&c](lua_State* L)
		{
			lua_pushlstring(L, c.nick.data(), c.nick.size());
			lua_pushlstring(L, c.text.data(), c.text.size());
			return 2;
		}))
			++g_stats.chat_delivered;
	}
}

void push_engine_event(EngineEvent&& e)
{
	if (g_engine.size() >= kMaxEngineEvents)
	{
		// F3 T2 review: a dropped event is not sent again (Lua re-reads the state, e.g. coop.checkpoint.get); say which.
		const EngineEvent& old = g_engine.front();
		static const char* const kEventNames[] = {"on_transition_vote", "coop_actor_revived", "coop_on_container_opened"};
		static_assert(sizeof(kEventNames) / sizeof(kEventNames[0]) == size_t(EngineEvent::Callback), "F3 T6 R1 M-4: one name per kind before Callback");
		const char* what = old.kind == EngineEvent::Callback ? old.name.c_str() :
			old.kind < EngineEvent::Callback ? kEventNames[old.kind] : "?";
		++g_stats.engine_dropped;
		if (log_allowed(g_stats.engine_dropped))
			::Msg("! [coop] engine event queue full (%u): '%s' dropped", u32(kMaxEngineEvents), what);
		g_engine.pop_front();
	}
	e.t_ms = mgr().now_ms();
	g_engine.push_back(std::move(e));
	++g_stats.engine_events;
}

void deliver_engine()
{
	const u64 now = mgr().now_ms();
	while (!g_engine.empty())
	{
		EngineEvent e = std::move(g_engine.front());
		g_engine.pop_front();
		if (now - e.t_ms > kStaleMs)
		{
			++g_stats.engine_dropped;
			continue;
		}
		bool ok = false;
		switch (e.kind)
		{
		case EngineEvent::TransitionVote:
			ok = call_lua("on_transition_vote", [&e](lua_State* L)
			{
				lua_pushlstring(L, e.text.data(), e.text.size());
				return 1;
			});
			break;
		case EngineEvent::ActorRevived:
			ok = send_script_callback("coop_actor_revived", [&e](lua_State* L)
			{
				if (e.peer == kInvalidPeer)
					lua_pushnil(L);
				else
					lua_pushnumber(L, lua_Number(e.peer));
				if (e.text.empty())
					lua_pushnil(L);
				else
					lua_pushlstring(L, e.text.data(), e.text.size());
				return 2;
			});
			break;
		case EngineEvent::ContainerOpened:
			ok = send_script_callback("coop_on_container_opened", [&e](lua_State* L)
			{
				lua_pushnumber(L, lua_Number(e.peer));
				lua_pushnumber(L, lua_Number(e.id));
				return 2;
			});
			break;
		case EngineEvent::Callback:
			ok = send_script_callback(e.name.c_str(), [&e](lua_State* L)
			{
				for (const CallbackArg& a : e.args)
				{
					switch (a.kind)
					{
					case CallbackArg::Bool: lua_pushboolean(L, a.i != 0 ? 1 : 0); break;
					case CallbackArg::Int: lua_pushinteger(L, lua_Integer(a.i)); break;
					case CallbackArg::Str: lua_pushlstring(L, a.s.data(), a.s.size()); break;
					case CallbackArg::Nil:
					default: lua_pushnil(L); break;
					}
				}
				return int(e.args.size());
			});
			break;
		}
		if (ok)
			++g_stats.engine_delivered;
	}
}

// --- the `coop` namespace ---------------------------------------------------------------------

bool l_is_active() { return mgr().active(); }
bool l_is_host() { return mgr().is_host(); }
bool l_is_client() { return mgr().is_client(); }
// Host, or client that finished loading the host's world.
bool l_is_live() { return mgr().is_host() || mgr().is_live_client(); }

int l_self_id()
{
	if (!mgr().active())
		return -1;
	const PeerId self = mgr().self();
	return self == kInvalidPeer ? -1 : int(self);
}

bool arg_id(lua_State* L, int idx, lua_Number& out)
{
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	out = lua_tonumber(L, idx);
	return out >= 0 && out <= lua_Number(0xffffffffu);
}

// coop.players() -> { {id=, nick=, avatar_id= (local numbering, nil if unknown), rtt=, loss= (0..1, the transport's
// smoothed packet loss; 0 for ourselves), is_self=}, ... } (loss: scripts\coop_hud_netgraph)
int l_players(lua_State* L)
{
	lua_newtable(L);
	if (!mgr().active())
		return 1;
	const PeerId self = mgr().self();
	int index = 0;
	auto push_entry = [L, &index, self](PeerId id, const char* nick, u16 avatar, u32 rtt, float loss)
	{
		lua_newtable(L);
		lua_pushnumber(L, lua_Number(id));
		lua_setfield(L, -2, "id");
		lua_pushstring(L, nick ? nick : "");
		lua_setfield(L, -2, "nick");
		if (avatar != 0xffff)
		{
			lua_pushnumber(L, lua_Number(avatar));
			lua_setfield(L, -2, "avatar_id");
		}
		lua_pushnumber(L, lua_Number(rtt));
		lua_setfield(L, -2, "rtt");
		lua_pushnumber(L, lua_Number(loss));
		lua_setfield(L, -2, "loss");
		lua_pushboolean(L, id == self);
		lua_setfield(L, -2, "is_self");
		lua_rawseti(L, -2, ++index);
	};
	if (self != kInvalidPeer)
		push_entry(self, mgr().nick(), 0, 0, 0.f); // our own actor is always id 0 here
	for (const PeerInfo& p : mgr().session().peers())
	{
		if (p.id == self)
			continue;
		push_entry(p.id, p.nick.c_str(), world::avatar_of(p.id), p.rtt_ms, p.packet_loss);
	}
	return 1;
}

// coop.avatar_of(peer) -> object id (local numbering) of that player's character here, or nil.
int l_avatar_of(lua_State* L)
{
	lua_Number v = 0;
	if (!mgr().active() || !arg_id(L, 1, v))
	{
		lua_pushnil(L);
		return 1;
	}
	const PeerId peer = PeerId(v);
	const u16 id = peer == mgr().self() ? u16(0) : world::avatar_of(peer);
	if (id == 0xffff)
		lua_pushnil(L);
	else
		lua_pushnumber(L, lua_Number(id));
	return 1;
}

// coop.peer_of_avatar(obj_id) -> peer id driving that character, or nil (0 = our own actor = self).
int l_peer_of_avatar(lua_State* L)
{
	lua_Number v = 0;
	if (!mgr().active() || !arg_id(L, 1, v) || v > 0xffff)
	{
		lua_pushnil(L);
		return 1;
	}
	const u16 id = u16(v);
	const PeerId peer = id == 0 ? mgr().self() : world::avatar_peer(id);
	if (peer == kInvalidPeer)
		lua_pushnil(L);
	else
		lua_pushnumber(L, lua_Number(peer));
	return 1;
}

// F0 final review (task 8): coop.owners_known() -> true once avatar_id / peer_of_avatar cover every player in the
// world (host: always; client: after the host's first AvatarList since it joined), false before and outside a session.
int l_owners_known(lua_State* L)
{
	lua_pushboolean(L, mgr().active() && world::owners_known());
	return 1;
}

// Ids on the wire are in host numbering; only the pair (0, my proxy) differs on a client.
int l_to_host_id(lua_State* L)
{
	lua_Number v = 0;
	if (!arg_id(L, 1, v) || v > 0xffff)
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, lua_Number(mgr().is_client() ? join::to_host(u16(v)) : u16(v)));
	return 1;
}

int l_to_local_id(lua_State* L)
{
	lua_Number v = 0;
	if (!arg_id(L, 1, v) || v > 0xffff)
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, lua_Number(mgr().is_client() ? join::to_local(u16(v)) : u16(v)));
	return 1;
}

int send_fail(lua_State* L, const char* why)
{
	++g_stats.send_rejected;
	if (log_allowed(g_stats.send_rejected))
		::Msg("! [coop] coop.send_event refused: %s", why);
	lua_pushboolean(L, 0);
	lua_pushstring(L, why);
	return 2;
}

// coop.send_event(name, payload, target) -> true | false, reason
//   name    : 1..64 printable ASCII characters, no spaces
//   payload : string up to 16 KiB (binary safe), nil = ""
//   target  : -1 or nil = every other player, 0 = host, N = player N
int l_send_event(lua_State* L)
{
	if (lua_type(L, 1) != LUA_TSTRING)
		return send_fail(L, "name must be a string");
	size_t name_len = 0;
	const char* name = lua_tolstring(L, 1, &name_len);
	if (!valid_name(name, name_len))
		return send_fail(L, "bad name (1..64 printable characters, no spaces)");

	size_t payload_len = 0;
	const char* payload = "";
	if (!lua_isnoneornil(L, 2))
	{
		if (!lua_isstring(L, 2))
			return send_fail(L, "payload must be a string");
		payload = lua_tolstring(L, 2, &payload_len);
	}
	if (payload_len > kMaxPayload)
		return send_fail(L, "payload is larger than 16 KiB");

	s32 target = kTargetAll;
	if (!lua_isnoneornil(L, 3))
	{
		if (lua_type(L, 3) != LUA_TNUMBER)
			return send_fail(L, "target must be a number");
		const lua_Number t = lua_tonumber(L, 3);
		if (t < -1 || t > lua_Number(kMaxPlayers))
			return send_fail(L, "bad target");
		target = s32(t);
	}

	const bool host = mgr().is_host();
	if (!host && !mgr().is_live_client())
		return send_fail(L, "not in a co-op world"); // P6: sender must be ready
	// F3a T7: in the host's process 0 is the world server and the host player at once (one Lua): an event to 0 goes over
	// the internal channel and comes back to this Lua with from = 0, as a client's event to the host does there.
	const bool to_own_world = host && target == s32(kHostPeer);
	const PeerId self = mgr().self();
	if (target != kTargetAll && PeerId(target) == self && !to_own_world)
		return send_fail(L, "target is this machine");
	if (target != kTargetAll && !to_own_world && !mgr().session().peer(PeerId(target)))
		return send_fail(L, "no such player");
	if (g_outbox.size() >= kMaxOutbox)
		return send_fail(L, "send queue is full");

	Outgoing o;
	o.target = target;
	o.name.assign(name, name_len);
	o.payload.assign(payload, payload_len);
	g_outbox.push_back(std::move(o));
	lua_pushboolean(L, 1);
	return 1;
}

// coop.send_chat(text) -> bool. Same as the coop_chat console command.
int l_send_chat(lua_State* L)
{
	size_t len = 0;
	const char* text = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &len) : nullptr;
	if (!text || !len || len > 512 || !mgr().session().is_active())
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	mgr().send_chat(text);
	lua_pushboolean(L, 1);
	return 1;
}

// --- session stash and the refusal window (W3d) ---------------------------------------------------

constexpr size_t kMaxStashTotal = 1024 * 1024;

// Frame pump for work that must go on while the manager is Off (it does not run Manager::frame then):
// showing a refusal in the main menu and dropping the stash once the session has ended.
class UiPump : public pureFrame
{
public:
	void _BCL OnFrame() override;
};

UiPump g_pump;
bool g_pump_on = false;

void pump_sync()
{
	const bool need = g_reject.pending || !g_stash.empty() || g_menu.pending;
	if (need && !g_pump_on)
	{
		Device.seqFrame.Add(&g_pump, REG_PRIORITY_LOW);
		g_pump_on = true;
	}
	else if (!need && g_pump_on)
	{
		Device.seqFrame.Remove(&g_pump); // deferred by CRegistrator when called from inside OnFrame
		g_pump_on = false;
	}
}

void stash_clear()
{
	g_stash.clear();
	g_stash_bytes = 0;
}

// coop.stash_set(key, value) -> bool. Keeps a string for this machine until the co-op session ends (level
// loads and Lua state rebuilds included). value nil removes the key. Key: 1..64 printable characters.
int l_stash_set(lua_State* L)
{
	size_t key_len = 0;
	const char* key = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &key_len) : nullptr;
	if (!key || !valid_name(key, key_len) || !mgr().active())
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const std::string k(key, key_len);
	auto it = g_stash.find(k);
	if (lua_isnoneornil(L, 2))
	{
		if (it != g_stash.end())
		{
			g_stash_bytes -= it->second.size();
			g_stash.erase(it);
		}
		pump_sync();
		lua_pushboolean(L, 1);
		return 1;
	}
	if (lua_type(L, 2) != LUA_TSTRING)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	size_t len = 0;
	const char* value = lua_tolstring(L, 2, &len);
	const size_t old = it != g_stash.end() ? it->second.size() : 0;
	if (len > kMaxStashValue || (it == g_stash.end() && g_stash.size() >= kMaxStashKeys) ||
	    g_stash_bytes - old + len > kMaxStashTotal)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	g_stash[k].assign(value, len);
	g_stash_bytes = g_stash_bytes - old + len;
	pump_sync();
	lua_pushboolean(L, 1);
	return 1;
}

// coop.stash_get(key) -> string or nil (always nil outside a session).
int l_stash_get(lua_State* L)
{
	size_t key_len = 0;
	const char* key = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &key_len) : nullptr;
	if (!key || !mgr().active())
	{
		lua_pushnil(L);
		return 1;
	}
	const auto it = g_stash.find(std::string(key, key_len));
	if (it == g_stash.end())
		lua_pushnil(L);
	else
		lua_pushlstring(L, it->second.data(), it->second.size());
	return 1;
}

// A dialog can be opened: CurrentDialogHolder() is the main menu when it is active, else the game HUD.
bool ui_ready()
{
	if (!g_pGamePersistent || Device.dwPrecacheFrame)
		return false;
	CMainMenu* menu = MainMenu();
	if (menu && menu->IsActive())
		return true;
	return g_pGameLevel && g_actor && CurrentGameUI();
}

void set_field(lua_State* L, const char* name, const std::string& v)
{
	lua_pushlstring(L, v.data(), v.size());
	lua_setfield(L, -2, name);
}

void set_field(lua_State* L, const char* name, lua_Number v)
{
	lua_pushnumber(L, v);
	lua_setfield(L, -2, name);
}

void set_list(lua_State* L, const char* name, const std::vector<std::string>& v)
{
	lua_newtable(L);
	int i = 0;
	for (const std::string& s : v)
	{
		lua_pushlstring(L, s.data(), s.size());
		lua_rawseti(L, -2, ++i);
	}
	lua_setfield(L, -2, name);
}

// coop_ui.on_rejected{ reason, code, text, missing, extra, differ, missing_total, extra_total, differ_total,
//   key_path, log_path }
void deliver_rejection()
{
	Rejection r = std::move(g_reject);
	g_reject = Rejection();
	string_path key_path;
	FS.update_path(key_path, "$app_data_root$", "coop_player.key");
	const std::string key(key_path);
	const std::string log_path(xrLogger::GetLogPath());
	const bool ok = call_module("coop_ui", "on_rejected", [&r, &key, &log_path](lua_State* L)
	{
		lua_newtable(L);
		set_field(L, "reason", std::string(reject_reason_name(r.reason)));
		set_field(L, "code", lua_Number(u8(r.reason)));
		set_field(L, "text", r.text);
		set_list(L, "missing", r.missing);
		set_list(L, "extra", r.extra);
		set_list(L, "differ", r.differ);
		set_field(L, "missing_total", lua_Number(r.missing_total));
		set_field(L, "extra_total", lua_Number(r.extra_total));
		set_field(L, "differ_total", lua_Number(r.differ_total));
		set_field(L, "key_path", key);
		set_field(L, "log_path", log_path);
		return 1;
	});
	if (ok)
	{
		++g_ui.shown;
		::Msg("[coop] host refusal (%s) shown on screen", reject_reason_name(r.reason));
	}
	else
		++g_ui.failed; // logged by call_module; the log already has the full diff
}

// --- co-op menu (W4) -----------------------------------------------------------------------------

void menu_error(const char* code, const std::string& text, bool by_user)
{
	g_menu.last_error = text;
	g_menu.last_code = code ? code : "";
	g_menu.last_by_user = by_user;
	g_menu.last_ms = mgr().now_ms();
}

void menu_clear_error()
{
	g_menu.last_error.clear();
	g_menu.last_code.clear();
	g_menu.last_by_user = false;
	g_menu.last_ms = 0;
	g_menu.rejected_ms = 0;
}

void menu_cancel_pending(const char* why)
{
	if (!g_menu.pending)
		return;
	g_menu.pending = false;
	g_menu.ready_since = 0;
	if (why)
		::Msg("[coop] hosting after loading cancelled: %s", why);
	pump_sync();
}

// In a level with ALife and a living actor; quiet version of join::host_can_start (that one logs).
bool level_ready_for_host()
{
	if (!g_pGameLevel || !g_actor || Device.dwPrecacheFrame || !ai().get_alife())
		return false;
	return g_actor->g_Alive();
}

bool menu_start_host(u16 port, std::string& why)
{
	if (mgr().active())
	{
		why = "already in a co-op session";
		return false;
	}
	if (!level_ready_for_host())
	{
		why = "no game is loaded: load a save or start a new game first";
		return false;
	}
	menu_clear_error();
	if (!mgr().start_host(port))
	{
		string256 text;
		xr_sprintf(text, "could not host on port %u (is it used by another program?), see the log", u32(port));
		why = text;
		menu_error("start", why, false);
		return false;
	}
	note_start(nullptr, port);
	return true;
}

// Frame pump part: host once a world newer than the request is up and has settled.
void menu_pending_frame()
{
	if (!g_menu.pending)
		return;
	const u64 now = mgr().now_ms();
	if (mgr().active())
	{
		menu_cancel_pending("a session is already running");
		return;
	}
	// pending-host clock (owner, live 2026-10-23): choosing faction and gear in the new game screen took over 15 min and
	// the session silently never opened. The main menu up is the start flow, not a forgotten request: no clock there.
	CMainMenu* pending_menu = MainMenu();
	if (pending_menu && pending_menu->IsActive())
		g_menu.pending_ms = now; // pending-host clock: counts from the moment the main menu is gone
	if (now - g_menu.pending_ms > kPendingMaxMs)
	{
		menu_cancel_pending("no game was loaded within 15 minutes");
		return;
	}
	if (join::world_generation() == g_menu.pending_gen || !level_ready_for_host())
	{
		g_menu.ready_since = 0;
		return;
	}
	if (!g_menu.ready_since)
	{
		g_menu.ready_since = now;
		return;
	}
	if (now - g_menu.ready_since < kPendingSettleMs)
		return;
	const u16 port = g_menu.pending_port;
	g_menu.pending = false;
	g_menu.ready_since = 0;
	std::string why;
	if (menu_start_host(port, why))
		::Msg("[coop] the game is loaded: hosting on port %u as requested from the menu", u32(port));
	else
		::Msg("! [coop] hosting after loading failed: %s", why.c_str());
}

void _BCL UiPump::OnFrame()
{
	menu_pending_frame();
	if (!g_stash.empty() && !mgr().active())
	{
		stash_clear();
		++g_ui.stash_cleared;
	}
	if (g_reject.pending)
	{
		if (mgr().now_ms() - g_reject.t_ms > kRejectWaitMs)
		{
			++g_ui.expired;
			::Msg("! [coop] host refusal (%s) was not shown: no menu or game screen for %u s",
			    reject_reason_name(g_reject.reason), u32(kRejectWaitMs / 1000));
			g_reject = Rejection();
		}
		else if (ui_ready())
			deliver_rejection();
	}
	pump_sync();
}

void copy_capped(const std::vector<std::string>& from, std::vector<std::string>& to, u32& total)
{
	total = u32(from.size());
	to.assign(from.begin(), from.begin() + ptrdiff_t(from.size() < kMaxListed ? from.size() : kMaxListed));
}

// --- co-op menu: Lua side (W4) -------------------------------------------------------------------

void set_bool(lua_State* L, const char* name, bool v)
{
	lua_pushboolean(L, v ? 1 : 0);
	lua_setfield(L, -2, name);
}

// Port argument: nil/none -> kDefaultPort, else an integer 1..65535; false if invalid.
bool arg_port(lua_State* L, int idx, u16& port)
{
	if (lua_isnoneornil(L, idx))
	{
		port = kDefaultPort;
		return true;
	}
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	const lua_Number v = lua_tonumber(L, idx);
	if (v < 1 || v > 65535 || v != lua_Number(s32(v)))
		return false;
	port = u16(v);
	return true;
}

int fail2(lua_State* L, const char* why)
{
	lua_pushboolean(L, 0);
	lua_pushstring(L, why);
	return 2;
}

// coop.session_info() -> table (see coop_script.h). Never fails, works in the main menu.
int l_session_info(lua_State* L)
{
	const Manager& m = mgr();
	const u64 now = m.now_ms();
	lua_newtable(L);
	set_field(L, "phase", std::string(phase_name(m.phase())));
	set_bool(L, "active", m.active());
	set_bool(L, "host", m.is_host());
	set_bool(L, "client", m.is_client());
	set_bool(L, "live", m.is_host() || m.is_live_client());
	set_field(L, "self_id", m.active() && m.self() != kInvalidPeer ? lua_Number(m.self()) : lua_Number(-1));
	set_field(L, "nick", std::string(m.nick()));
	set_field(L, "max_players", lua_Number(kMaxPlayers));
	set_field(L, "default_port", lua_Number(kDefaultPort));
	if (m.active() && g_menu.port)
		set_field(L, "port", lua_Number(g_menu.port));
	if (m.is_client() && !g_menu.address.empty())
		set_field(L, "address", g_menu.address);
	if (!g_menu.last_error.empty())
	{
		set_field(L, "last_error", g_menu.last_error);
		set_field(L, "last_error_code", g_menu.last_code);
		set_field(L, "last_error_age", lua_Number(now - g_menu.last_ms));
		set_bool(L, "last_by_user", g_menu.last_by_user);
	}
	if (g_menu.pending)
	{
		set_field(L, "pending_host", lua_Number(g_menu.pending_port));
		set_field(L, "pending_age", lua_Number(now - g_menu.pending_ms));
	}

	// players: this machine first, then the session's peers (client: host + the PeerList)
	lua_newtable(L);
	int index = 0;
	auto push_player = [L, &index](PeerId id, const char* nick, u32 rtt, float loss, bool joined, bool self,
	                       const char* address)
	{
		lua_newtable(L);
		set_field(L, "id", lua_Number(id));
		set_field(L, "nick", std::string(nick ? nick : ""));
		set_field(L, "rtt", lua_Number(rtt));
		set_field(L, "loss", lua_Number(loss));
		set_bool(L, "joined", joined);
		set_bool(L, "is_self", self);
		set_bool(L, "is_host", id == kHostPeer);
		if (address && *address)
			set_field(L, "address", std::string(address));
		lua_rawseti(L, -2, ++index);
	};
	if (m.active())
	{
		const PeerId self = m.self();
		if (self != kInvalidPeer)
			push_player(self, m.nick(), 0, 0.f, true, true, nullptr);
		for (const PeerInfo& p : m.session().peers())
		{
			if (p.id == self)
				continue;
			push_player(p.id, p.nick.c_str(), p.rtt_ms, p.packet_loss, p.state == PeerState::Joined, false,
			    m.is_host() ? p.address.c_str() : nullptr);
		}
	}
	lua_setfield(L, -2, "players");
	return 1;
}

// coop.host(port) -> true | false, reason. Hosts the game that is loaded now.
int l_host(lua_State* L)
{
	u16 port = 0;
	if (!arg_port(L, 1, port))
		return fail2(L, "bad port (1..65535)");
	menu_cancel_pending(nullptr);
	std::string why;
	if (!menu_start_host(port, why))
		return fail2(L, why.c_str());
	lua_pushboolean(L, 1);
	return 1;
}

// coop.host_when_ready(port) -> true | false, reason. nil or 0 cancels a pending request.
int l_host_when_ready(lua_State* L)
{
	if (lua_isnoneornil(L, 1) || (lua_type(L, 1) == LUA_TNUMBER && lua_tonumber(L, 1) == 0))
	{
		menu_cancel_pending("cancelled from the menu");
		lua_pushboolean(L, 1);
		return 1;
	}
	u16 port = 0;
	if (!arg_port(L, 1, port))
		return fail2(L, "bad port (1..65535)");
	if (mgr().active())
		return fail2(L, "already in a co-op session");
	menu_clear_error();
	g_menu.pending = true;
	g_menu.pending_port = port;
	g_menu.pending_gen = join::world_generation();
	g_menu.pending_ms = mgr().now_ms();
	g_menu.ready_since = 0;
	pump_sync();
	::Msg("[coop] will host on port %u as soon as a game is loaded", u32(port));
	lua_pushboolean(L, 1);
	return 1;
}

// coop.connect("ip[:port]") -> true | false, reason. The result arrives later (session_info().phase).
int l_connect(lua_State* L)
{
	if (lua_type(L, 1) != LUA_TSTRING)
		return fail2(L, "address must be a string");
	size_t len = 0;
	const char* s = lua_tolstring(L, 1, &len);
	if (len == 0 || len > 255)
		return fail2(L, "bad address");
	xr_string address;
	u16 port = 0;
	if (!parse_endpoint(s, address, port))
		return fail2(L, "bad address (ip or ip:port)");
	if (mgr().active())
		return fail2(L, "already in a co-op session");
	menu_cancel_pending(nullptr);
	menu_clear_error();
	if (!mgr().start_client(address.c_str(), port))
	{
		std::string why = "could not start connecting to ";
		why += address.c_str();
		why += ", see the log";
		menu_error("start", why, false);
		return fail2(L, why.c_str());
	}
	note_start(address.c_str(), port);
	lua_pushboolean(L, 1);
	return 1;
}

// coop.disconnect() -> true if a session was running. Also drops a pending "host after loading".
int l_disconnect(lua_State* L)
{
	menu_cancel_pending("cancelled from the menu");
	const bool was = mgr().active();
	if (was)
	{
		note_user_stop();
		mgr().stop("left the session");
	}
	lua_pushboolean(L, was ? 1 : 0);
	return 1;
}

// --- W5 glue for the W4 scripts ---------------------------------------------------------------

// coop.spot_is_local(type) -> bool
int l_spot_is_local(lua_State* L)
{
	if (lua_type(L, 1) != LUA_TSTRING)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, story::spot_is_local(lua_tostring(L, 1)) ? 1 : 0);
	return 1;
}

// coop.story_hold_infos(on) -> bool (false: not a client in the world, nothing is held)
int l_story_hold_infos(lua_State* L)
{
	const int t = lua_type(L, 1);
	if (t != LUA_TBOOLEAN && t != LUA_TNIL)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, story::hold_infos(lua_toboolean(L, 1) != 0) ? 1 : 0);
	return 1;
}

// coop.story_local_task(id[, on = true]) -> bool
int l_story_local_task(lua_State* L)
{
	const int t = lua_type(L, 2);
	if (lua_type(L, 1) != LUA_TSTRING || (t != LUA_TBOOLEAN && t != LUA_TNIL && t != LUA_TNONE))
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const bool on = t == LUA_TBOOLEAN ? lua_toboolean(L, 2) != 0 : true;
	lua_pushboolean(L, story::set_local_task(lua_tostring(L, 1), on) ? 1 : 0);
	return 1;
}

bool arg_local_id(lua_State* L, int idx, u16& out)
{
	lua_Number v = 0;
	if (!mgr().active() || !arg_id(L, idx, v) || v >= lua_Number(0xffff))
		return false;
	out = u16(v);
	return true;
}

// coop.life_of(local_id) -> "alive" | "wounded" | "dead" | "spectating" | "respawning" (0 = me), nil without a session
int l_life_of(lua_State* L)
{
	u16 id = 0;
	if (!arg_local_id(L, 1, id))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, survival::life_name(survival::life_of(id)));
	return 1;
}

// coop.rollback_failed(character, seq): the host found no save to go back to, that death goes the checkpoint way
int l_rollback_failed(lua_State* L)
{
	u16 id = 0;
	u16 seq = 0;
	if (arg_local_id(L, 1, id) && arg_local_id(L, 2, seq))
		survival::rollback_failed(id, seq);
	return 0;
}

// coop.steam_invite_ready() -> true when the launcher's Steam mode started this game with a lobby
int l_steam_invite_ready(lua_State* L)
{
	lua_pushboolean(L, steam_invite_ready() ? 1 : 0);
	return 1;
}

// coop.steam_invite() -> bool: Steam's "invite a friend" window over the game
int l_steam_invite(lua_State* L)
{
	lua_pushboolean(L, steam_invite_window() ? 1 : 0);
	return 1;
}

// coop.rollback_pending() -> true while the group waits for the host's load
int l_rollback_pending(lua_State* L)
{
	lua_pushboolean(L, survival::rollback_pending() ? 1 : 0);
	return 1;
}

// coop.saves_open() -> true in a session of the rollback death mode: saves work as in the single player game
int l_saves_open(lua_State* L)
{
	lua_pushboolean(L, survival::saves_open() ? 1 : 0);
	return 1;
}

// coop.bleed_left(local_id) -> milliseconds of bleed-out left (0 unless wounded), nil without a session
int l_bleed_left(lua_State* L)
{
	u16 id = 0;
	if (!arg_local_id(L, 1, id))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, lua_Number(survival::bleed_left_ms(id)));
	return 1;
}

void set_num(lua_State* L, const char* name, lua_Number v)
{
	lua_pushnumber(L, v);
	lua_setfield(L, -2, name);
}

void set_str(lua_State* L, const char* name, const char* v)
{
	lua_pushstring(L, v ? v : "");
	lua_setfield(L, -2, name);
}

// HUD v2 part 2 (docs/HUD_V2.md, scripts\coop_hud_revive.script): coop.revive_progress() -> nil | table; nil without a session
// or when I neither hold USE on a wounded teammate nor lie wounded myself. A table has only the parts that apply:
//   reviving   = { peer, avatar_id, nick, progress 0..1, ms_left, item = "medkit"|"stimpack", section, sent }  I revive
//                somebody (sent: the hold is over, the Request is out, up to 3 s until coop_revive_done)
//   revived_by = { peer, avatar_id, nick, progress 0..1, ms_left }   a teammate holds USE on me (I am wounded)
//   give_up    = { progress 0..1, ms_left }                          I hold USE to bleed out at once (I am wounded)
//   bleed      = { left_ms, total_ms }                               I am wounded
// peer / avatar_id are left out when unknown (avatar_id in this machine's numbering). Calling it tells the engine a Lua HUD
// draws the down / revive state, so the engine's own text for it is not shown.
int l_revive_progress(lua_State* L)
{
	survival::ReviveProgress p;
	if (!mgr().active() || !survival::revive_progress(p))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	if (p.reviving)
	{
		lua_newtable(L);
		if (p.target_peer != kInvalidPeer)
			set_num(L, "peer", lua_Number(p.target_peer));
		if (p.target_local != 0xffff)
			set_num(L, "avatar_id", lua_Number(p.target_local));
		set_str(L, "nick", p.target_nick.c_str());
		set_num(L, "progress", lua_Number(p.reviving_fraction));
		set_num(L, "ms_left", lua_Number(p.reviving_left_ms));
		set_str(L, "item", p.item_kind);
		set_str(L, "section", p.item_section.c_str());
		lua_pushboolean(L, p.reviving_sent ? 1 : 0);
		lua_setfield(L, -2, "sent");
		lua_setfield(L, -2, "reviving");
	}
	if (p.revived_by)
	{
		lua_newtable(L);
		if (p.by_peer != kInvalidPeer)
			set_num(L, "peer", lua_Number(p.by_peer));
		if (p.by_local != 0xffff)
			set_num(L, "avatar_id", lua_Number(p.by_local));
		set_str(L, "nick", p.by_nick.c_str());
		set_num(L, "progress", lua_Number(p.by_fraction));
		set_num(L, "ms_left", lua_Number(p.by_left_ms));
		lua_setfield(L, -2, "revived_by");
	}
	if (p.giving_up)
	{
		lua_newtable(L);
		set_num(L, "progress", lua_Number(p.giveup_fraction));
		set_num(L, "ms_left", lua_Number(p.giveup_left_ms));
		lua_setfield(L, -2, "give_up");
	}
	if (p.wounded)
	{
		lua_newtable(L);
		set_num(L, "left_ms", lua_Number(p.bleed_left_ms));
		set_num(L, "total_ms", lua_Number(p.bleed_total_ms));
		lua_setfield(L, -2, "bleed");
	}
	return 1;
}
} // namespace
} // namespace script

namespace script
{
namespace
{
// coop.player_safemode(peer) -> bool (weapon lowered), nil if unknown
int l_player_safemode(lua_State* L)
{
	lua_Number v = 0;
	if (!mgr().active() || !arg_id(L, 1, v))
	{
		lua_pushnil(L);
		return 1;
	}
	const PeerId peer = PeerId(v);
	if (peer == mgr().self())
	{
		CActor* a = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
		if (!a)
			lua_pushnil(L);
		else
			lua_pushboolean(L, a->is_safemode() ? 1 : 0);
		return 1;
	}
	bool lowered = false;
	const u16 avatar = world::avatar_of(peer);
	if (avatar == 0xffff || !players::avatar_safemode(avatar, lowered))
		lua_pushnil(L);
	else
		lua_pushboolean(L, lowered ? 1 : 0);
	return 1;
}

// W11: coop.talk_holder(npc_id) -> peer id of the player talking to that NPC (host only; 0 = the host
// himself), nil if nobody, on a client or without a session.
int l_talk_holder(lua_State* L)
{
	lua_Number v = 0;
	if (!mgr().is_host() || !arg_id(L, 1, v) || v >= lua_Number(0xffff))
	{
		lua_pushnil(L);
		return 1;
	}
	const PeerId holder = dialog::host_talk_holder(u16(v));
	if (holder == kInvalidPeer)
		lua_pushnil(L);
	else
		lua_pushnumber(L, lua_Number(holder));
	return 1;
}

// Ruling F2-34 (F2 T12 round 2): coop.talk_recent(peer, npc_id, ms) -> true if player `peer` holds the talk lock of
// NPC npc_id (host numbering) now or held it no more than ms ago (ms within 0..60000); false otherwise, on a client and
// on bad arguments. coop_npc_companions takes a client's recruit only right after a talk.
int l_talk_recent(lua_State* L)
{
	lua_Number peer = 0, npc = 0, ms = 0;
	const bool ok = mgr().is_host() && arg_id(L, 1, peer) && arg_id(L, 2, npc) && npc < lua_Number(0xffff) &&
	    lua_type(L, 3) == LUA_TNUMBER;
	if (ok)
		ms = lua_tonumber(L, 3);
	lua_pushboolean(L, ok && ms >= 0 && ms <= 60000 &&
	    dialog::host_talked_recently(PeerId(peer), u16(npc), u32(ms)) ? 1 : 0);
	return 1;
}

void register_raw(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"players", &l_players},
		{"avatar_of", &l_avatar_of},
		{"peer_of_avatar", &l_peer_of_avatar},
		{"owners_known", &l_owners_known},
		{"to_host_id", &l_to_host_id},
		{"to_local_id", &l_to_local_id},
		{"send_event", &l_send_event},
		{"send_chat", &l_send_chat},
		{"stash_set", &l_stash_set},
		{"stash_get", &l_stash_get},
		{"session_info", &l_session_info},
		{"host", &l_host},
		{"host_when_ready", &l_host_when_ready},
		{"connect", &l_connect},
		{"disconnect", &l_disconnect},
		{"spot_is_local", &l_spot_is_local},
		{"story_hold_infos", &l_story_hold_infos},
		{"story_local_task", &l_story_local_task},
		{"life_of", &l_life_of},
		{"steam_invite_ready", &l_steam_invite_ready},
		{"steam_invite", &l_steam_invite},
		{"rollback_failed", &l_rollback_failed},
		{"rollback_pending", &l_rollback_pending},
		{"saves_open", &l_saves_open},
		{"bleed_left", &l_bleed_left},
		{"revive_progress", &l_revive_progress}, // HUD v2 part 2: the revive / give-up holds for scripts\coop_hud_revive
		{"player_safemode", &l_player_safemode},
		{"instrument", &players::lua_instrument},
		{"talk_holder", &l_talk_holder}, // W11
		{"talk_recent", &l_talk_recent}, // F2 T12 round 2 (Ruling F2-34)
		{"explode_as", &coop::requests::lua_explode_as}, // R3 В2 review: a planted charge goes off as its planter
		{"lease_ids", &coop::requests::lua_lease_ids}, // F2 T12b: ids this client can create its own items with now
		{"lease_waiting", &coop::requests::lua_lease_waiting}, // F2 T12b round 1: an item that waits for the host
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		::Msg("! [coop] script namespace 'coop' was not created, Lua API unavailable");
		return;
	}
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_pop(L, 1);
	money::register_lua(L); // F2 T1: coop.money_give / money_grant / money_set / money_of / talk_lock_of
	router::register_lua(L); // F2 T10: coop.route.of / runs / note / none_functions (scripts\coop_route.script)
	syncapi::register_lua(L); // F2 T14: coop.sync.* (coop_sync_api.h)
	game::register_lua(L); // Game settings T2: coop.game.* (coop_game_settings.h)
	join::register_lua(L); // transition wait: coop.transition() (coop_join.cpp, scripts\coop_hud_transition.script)
	levels::register_lua(L); // F3a: coop.levels.*, coop.is_world_server / is_map_authority / is_player / is_mirror (coop_levels.cpp)
}
} // namespace

// F3a T7: ScriptEvent goes both ways (Either): each side has its own function, so that the host player's copy on the
// internal channel never runs the world server's part.
void install_server(Session& s) { s.on(Msg::ScriptEvent, &server_on_script_event); }

void install_client(Endpoint& e) { e.on(Msg::ScriptEvent, &client_on_script_event); }

void reset()
{
	g_outbox.clear();
	g_inbox.clear();
	g_chat.clear();
	g_engine.clear();
	g_stats = Stats();
	g_pacer.reset(); // F2 T14
}

void frame()
{
	if (!mgr().active())
	{
		if (!g_outbox.empty() || !g_inbox.empty() || !g_chat.empty() || !g_engine.empty())
			reset();
		return;
	}
	flush_outbox();
	if (!lua_ready())
		return;
	deliver_chat();
	deliver_inbox();
	deliver_engine();
}

void on_transition_vote(const char* nick)
{
	EngineEvent e;
	e.kind = EngineEvent::TransitionVote;
	e.text = nick && *nick ? nick : "?";
	push_engine_event(std::move(e));
}

void on_actor_revived(uint32_t reviver_peer, const char* medkit_section)
{
	EngineEvent e;
	e.kind = EngineEvent::ActorRevived;
	e.peer = reviver_peer;
	e.text = medkit_section ? medkit_section : "";
	push_engine_event(std::move(e));
}

void on_container_opened(uint32_t peer, u16 container_id)
{
	EngineEvent e;
	e.kind = EngineEvent::ContainerOpened;
	e.peer = peer;
	e.id = container_id;
	push_engine_event(std::move(e));
}

CallbackArg cb_nil() { return CallbackArg(); }

CallbackArg cb_bool(bool v)
{
	CallbackArg a;
	a.kind = CallbackArg::Bool;
	a.i = v ? 1 : 0;
	return a;
}

CallbackArg cb_int(s64 v)
{
	CallbackArg a;
	a.kind = CallbackArg::Int;
	a.i = v;
	return a;
}

CallbackArg cb_str(const char* v)
{
	CallbackArg a;
	if (!v)
		return a;
	a.kind = CallbackArg::Str;
	a.s = v;
	return a;
}

void queue_callback(const char* name, std::initializer_list<CallbackArg> args)
{
	size_t n = 0; // bounded: a name past kMaxName is refused anyway
	if (name)
		while (n <= kMaxName && name[n])
			++n;
	bool ok = valid_name(name, n) && args.size() <= kMaxCallbackArgs;
	for (const CallbackArg& a : args)
		if (a.kind == CallbackArg::Str && a.s.size() > kMaxCallbackStr)
			ok = false;
	if (!ok)
	{
		++g_stats.engine_invalid;
		if (log_allowed(g_stats.engine_invalid))
			::Msg("! [coop] engine callback '%.64s' dropped: bad name, %u arguments (max %u) or a string over %u bytes",
			    name ? name : "(null)", u32(args.size()), u32(kMaxCallbackArgs), u32(kMaxCallbackStr));
		return;
	}
	EngineEvent e;
	e.kind = EngineEvent::Callback;
	e.name.assign(name, n);
	e.args.assign(args.begin(), args.end());
	push_engine_event(std::move(e));
}

void on_chat(const char* nick, const char* text)
{
	if (!text || !*text)
		return;
	if (g_chat.size() >= kMaxChat)
	{
		g_chat.pop_front();
		++g_stats.drop_overflow;
	}
	ChatLine c;
	c.nick = nick ? nick : "?";
	c.text = text;
	c.t_ms = mgr().now_ms();
	g_chat.push_back(std::move(c));
}

void on_rejected(RejectReason reason, const char* text, const ManifestDiff& diff)
{
	++g_ui.rejections;
	g_reject = Rejection();
	g_reject.pending = true;
	g_reject.reason = reason;
	g_reject.text = text ? text : "";
	copy_capped(diff.missing_here, g_reject.missing, g_reject.missing_total);
	copy_capped(diff.extra_here, g_reject.extra, g_reject.extra_total);
	copy_capped(diff.differ, g_reject.differ, g_reject.differ_total);
	g_reject.t_ms = mgr().now_ms();
	pump_sync();
	// co-op menu: the refusal is the last error until the next attempt
	std::string why = text && *text ? text : reject_reason_name(reason);
	menu_error(reject_reason_name(reason), why, false);
	g_menu.rejected_ms = g_menu.last_ms;
}

void note_start(const char* address, u16 port)
{
	g_menu.port = port;
	g_menu.address = address ? address : "";
	g_menu.user_stop = false;
	if (!address)
		menu_cancel_pending(nullptr);
}

void note_start_failed(const char* why) { menu_error("start", why ? why : "could not start the session", false); }

void note_user_stop() { g_menu.user_stop = true; }

void on_session_end(const char* reason)
{
	const bool by_user = g_menu.user_stop;
	g_menu.user_stop = false;
	g_menu.port = 0;
	g_menu.address.clear();
	// A refusal (on_rejected) is followed by the transport closing: keep the precise one.
	if (!by_user && g_menu.rejected_ms && mgr().now_ms() - g_menu.rejected_ms < kRejectShadowMs)
		return;
	menu_error(by_user ? "left" : "lost", reason && *reason ? reason : "the session ended", by_user);
}

void print_status()
{
	const Stats& s = g_stats;
	::Msg("[coop-lua] events: sent %llu, received %llu, relayed %llu, delivered %llu", s.sent, s.received, s.relayed,
	    s.delivered);
	::Msg("[coop-lua] refused sends %llu, held for the host's rate %llu frames | dropped: invalid %llu, sender not ready "
	      "%llu, overflow %llu, stale %llu",
	    s.send_rejected, s.send_held, s.drop_invalid, s.drop_not_ready, s.drop_overflow, s.drop_stale); // F2 T14
	::Msg("[coop-lua] lua errors %llu, missing handlers %llu, chat lines shown %llu | queued: out %u, in %u, chat %u",
	    s.lua_errors, s.lua_missing, s.chat_delivered, (u32)g_outbox.size(), (u32)g_inbox.size(), (u32)g_chat.size());
	::Msg("[coop-lua] stash %u keys / %u bytes (dropped at session end %llu) | refusals %llu: shown %llu, failed %llu, "
	      "expired %llu, waiting %s",
	    (u32)g_stash.size(), (u32)g_stash_bytes, g_ui.stash_cleared, g_ui.rejections, g_ui.shown, g_ui.failed,
	    g_ui.expired, g_reject.pending ? "yes" : "no");
	::Msg("[coop-lua] menu: port %u, address '%s', host after loading %s, last error [%s] '%s'", u32(g_menu.port),
	    g_menu.address.c_str(), g_menu.pending ? "pending" : "no", g_menu.last_code.c_str(), g_menu.last_error.c_str());
	::Msg("[coop-lua] engine callbacks: queued %llu, delivered %llu, dropped %llu, refused %llu, waiting %u",
	    s.engine_events, s.engine_delivered, s.engine_dropped, s.engine_invalid, (u32)g_engine.size());
	if (lua_ready())
	{
		call_lua("print_status", [](lua_State*) { return 0; });
		call_module("coop_settings", "print_status", [](lua_State*) { return 0; }); // host-owned settings (W3d)
	}
}
} // namespace script
} // namespace coop

#pragma optimize("s", on)
void coop_script_registrator::script_register(lua_State* L)
{
	module(L, "coop")
	[
		def("is_active", &coop::script::l_is_active),
		def("is_host", &coop::script::l_is_host),
		def("is_client", &coop::script::l_is_client),
		def("is_live", &coop::script::l_is_live),
		def("self_id", &coop::script::l_self_id)
	];
	// Functions with arguments are plain C functions: a wrong argument from an addon returns nil/false
	// instead of a luabind "no matching overload" error, which is fatal in this engine (R-11.4).
	coop::script::register_raw(L);
}
