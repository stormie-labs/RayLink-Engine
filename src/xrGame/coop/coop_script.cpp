#include "stdafx.h"
#include "pch_script.h"
#include "coop_script.h"

#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"

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

struct Stats
{
	u64 sent = 0; // events handed to the network (host: per message, not per receiver)
	u64 received = 0; // valid events received from the network
	u64 relayed = 0; // host: client events forwarded to other clients
	u64 delivered = 0; // coop_main.on_event calls that returned normally
	u64 send_rejected = 0; // coop.send_event refused (bad args, not ready, to myself, queue full)
	u64 drop_invalid = 0; // malformed / bad name / payload too big / bad target
	u64 drop_not_ready = 0; // host: sender is still loading the world (P6)
	u64 drop_overflow = 0; // inbox or chat queue full
	u64 drop_stale = 0; // waited too long for Lua
	u64 lua_errors = 0; // Lua error inside coop_main.on_event / on_chat / print_status
	u64 lua_missing = 0; // coop_main or the function is missing
	u64 chat_delivered = 0;
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

Stats g_stats;
std::deque<Outgoing> g_outbox;
std::deque<Incoming> g_inbox;
std::deque<ChatLine> g_chat;
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

void on_script_event(PeerId from, Reader& r)
{
	const PeerId wire_origin = r.u32();
	const s32 target = r.i32();
	std::string name = r.str();
	std::string payload = r.str();
	if (!r.ok() || !valid_name(name.data(), name.size()) || payload.size() > kMaxPayload)
	{
		++g_stats.drop_invalid;
		if (log_allowed(g_stats.drop_invalid))
			::Msg("! [coop] malformed script event from #%u dropped", from);
		return;
	}

	if (mgr().is_host())
	{
		// P6: the sender must be in the world; a loading client has no business sending game events.
		const PeerInfo* sender = mgr().session().peer(from);
		if (!sender || sync::host_peer_joining(from))
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
		return;
	}

	if (mgr().is_client())
	{
		if (from != kHostPeer)
			return; // star topology: everything comes through the host
		++g_stats.received;
		deliver_later(wire_origin, std::move(name), std::move(payload));
	}
}

void flush_outbox()
{
	if (g_outbox.empty())
		return;
	const bool host = mgr().is_host();
	const bool client = mgr().is_live_client();
	while (!g_outbox.empty())
	{
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
			if (o.target == kTargetAll)
				net::host_broadcast(Msg::ScriptEvent, w);
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

// coop.players() -> { {id=, nick=, avatar_id= (local numbering, nil if unknown), rtt=, is_self=}, ... }
int l_players(lua_State* L)
{
	lua_newtable(L);
	if (!mgr().active())
		return 1;
	const PeerId self = mgr().self();
	int index = 0;
	auto push_entry = [L, &index, self](PeerId id, const char* nick, u16 avatar, u32 rtt)
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
		lua_pushboolean(L, id == self);
		lua_setfield(L, -2, "is_self");
		lua_rawseti(L, -2, ++index);
	};
	if (self != kInvalidPeer)
		push_entry(self, mgr().nick(), 0, 0); // our own actor is always id 0 here
	for (const PeerInfo& p : mgr().session().peers())
	{
		if (p.id == self)
			continue;
		push_entry(p.id, p.nick.c_str(), world::avatar_of(p.id), p.rtt_ms);
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
	const PeerId self = mgr().self();
	if (target != kTargetAll && PeerId(target) == self)
		return send_fail(L, "target is this machine");
	if (target != kTargetAll && !mgr().session().peer(PeerId(target)))
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
	const bool need = g_reject.pending || !g_stash.empty();
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

void _BCL UiPump::OnFrame()
{
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

void register_raw(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"players", &l_players},
		{"avatar_of", &l_avatar_of},
		{"peer_of_avatar", &l_peer_of_avatar},
		{"to_host_id", &l_to_host_id},
		{"to_local_id", &l_to_local_id},
		{"send_event", &l_send_event},
		{"send_chat", &l_send_chat},
		{"stash_set", &l_stash_set},
		{"stash_get", &l_stash_get},
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
}
} // namespace

void install(Session& s) { s.on(Msg::ScriptEvent, &on_script_event); }

void reset()
{
	g_outbox.clear();
	g_inbox.clear();
	g_chat.clear();
	g_stats = Stats();
}

void frame()
{
	if (!mgr().active())
	{
		if (!g_outbox.empty() || !g_inbox.empty() || !g_chat.empty())
			reset();
		return;
	}
	flush_outbox();
	if (!lua_ready())
		return;
	deliver_chat();
	deliver_inbox();
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
}

void print_status()
{
	const Stats& s = g_stats;
	::Msg("[coop-lua] events: sent %llu, received %llu, relayed %llu, delivered %llu", s.sent, s.received, s.relayed,
	    s.delivered);
	::Msg("[coop-lua] refused sends %llu | dropped: invalid %llu, sender not ready %llu, overflow %llu, stale %llu",
	    s.send_rejected, s.drop_invalid, s.drop_not_ready, s.drop_overflow, s.drop_stale);
	::Msg("[coop-lua] lua errors %llu, missing handlers %llu, chat lines shown %llu | queued: out %u, in %u, chat %u",
	    s.lua_errors, s.lua_missing, s.chat_delivered, (u32)g_outbox.size(), (u32)g_inbox.size(), (u32)g_chat.size());
	::Msg("[coop-lua] stash %u keys / %u bytes (dropped at session end %llu) | refusals %llu: shown %llu, failed %llu, "
	      "expired %llu, waiting %s",
	    (u32)g_stash.size(), (u32)g_stash_bytes, g_ui.stash_cleared, g_ui.rejections, g_ui.shown, g_ui.failed,
	    g_ui.expired, g_reject.pending ? "yes" : "no");
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
