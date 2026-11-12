#include "stdafx.h"
#include "pch_script.h"
#include "coop_debug.h"
#include "coop_manager.h"
#include "xrCoop/coop_dedicated_rules.h" // log_shown: what the dedicated window shows of the log
#include "xrCoop/coop_operator.h" // op::allowed: what the operator's console may send (plan 2026-11-12 §2.3)
#include "coop_dedicated.h" // answer_line: the answers to an operator's command
#include "coop_net.h"
#include "coop_script.h"
#include "coop_ai.h" // coop.debug.unseen: npc_ai::debug_unseen
#include "Level.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/coop_host_console.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "Actor_Flags.h"

#include <cmath>
#include <string>

// See coop_debug.h; the rules are xrCoop/coop_debug_rules.h.

namespace coop
{
namespace debug
{
namespace
{
constexpr size_t kMaxSay = 2048; // a line of coop.debug.say

struct Stats
{
	u64 states_sent = 0; // broadcasts of a change (the world server)
	u64 states_to_one = 0; // the state at the end of a join
	u64 states_in = 0; // news taken (a player's machine, a map server)
	u64 stale = 0; // a serial seen already
	u64 bad = 0; // malformed
	u64 god_hits = 0; // PlayerHit not sent: god
	u64 requests_refused = 0; // coop.dbg.* while off
	u64 cheats_refused = 0; // the engine's -dbg cheats in a session
};

ServerState g_server; // the world server's state
StateMsg g_seen; // the last DebugState this process took (a player's machine, the host player's half, a map server)
bool g_seen_any = false;
u8 g_applied = 0; // the flags this machine set on its own actor from DebugState (apply_local)
bool g_dirty = false; // the world server: a change to send
Stats g_stats;

bool log_allowed(u64 count) { return count <= 10 || count % 100 == 0; }

// An answer to the operator: in the log, and in the dedicated host's window whatever its log level. The window shows a log line its level
// lets through (dedicated::log_shown); any other is written to it here, once (the Lua's coop.debug.say lines start without "[coop").
void say(const char* line)
{
	::Msg("%s", line);
	if (!dedicated::log_shown(line, coop_console::log_level()))
		coop_console::print(line);
	dedicated::answer_line(line); // a line of the operator's command (dedicated::run_for) goes back to him too
}

void say(const std::string& line) { say(line.c_str()); }

// This process is a client that holds the host's rights of a world server (plan 2026-11-12 §2.3).
bool operator_here() { return mgr().is_client() && mgr().session().self_host_rights(); }

// The operator's console: the line goes to the world server as an OperatorCommand; the server checks it again and answers.
void send_to_server(const std::string& line)
{
	if (!op::allowed(line))
	{
		say("! [coop] " + line.substr(0, line.find(' ')) + ": this line cannot go to the server (one blank between words, no ';')");
		return;
	}
	if (!mgr().session().send_operator_command(line))
	{
		say("! [coop] the world server is not reachable now");
		return;
	}
	say("[coop] sent to the world server: " + line);
}

bool world_server_now() { return mgr().active() && is_world_server(); }

// The world server's own state, or the last one this process took.
const StateMsg& seen_or_empty()
{
	static const StateMsg none;
	return g_seen_any ? g_seen : none;
}

std::string flag_words(u8 flags)
{
	std::string s;
	if (flags & kFlagGod)
		s += " god";
	if (flags & kFlagAmmo)
		s += " ammo";
	if (flags & kFlagInvisible)
		s += " invisible";
	return s.empty() ? std::string(" none") : s;
}

std::string nick_of(PeerId peer)
{
	for (const PeerInfo& p : mgr().session().players())
		if (p.id == peer)
			return p.nick;
	return "?";
}

bool is_player_peer(PeerId peer)
{
	for (const PeerInfo& p : mgr().session().players())
		if (p.id == peer)
			return true;
	return false;
}

u8 actor_flags()
{
	return u8((psActorFlags.test(AF_GODMODE) ? kFlagGod : 0) | (psActorFlags.test(AF_UNLIMITEDAMMO) ? kFlagAmmo : 0));
}

void set_actor_flags(u8 bits, bool value)
{
	if (bits & kFlagGod)
		psActorFlags.set(AF_GODMODE, value ? TRUE : FALSE);
	if (bits & kFlagAmmo)
		psActorFlags.set(AF_UNLIMITEDAMMO, value ? TRUE : FALSE);
}

// This machine's own actor follows its row of the state (§B.9). A map server's and the dedicated host's actor is a ghost: it keeps what
// it has (the map server keeps AF_GODMODE on its ghost for good, coop_mapserver.cpp ghost_frame).
void apply_own_actor(const StateMsg& m)
{
	const bool has_player = is_player() && !ghost_actor();
	const LocalApply a = apply_local(g_applied, wanted_local(m, mgr().self(), has_player), actor_flags());
	if (a.set)
		set_actor_flags(a.set, true);
	if (a.clear)
		set_actor_flags(a.clear, false);
	g_applied = a.applied;
	if (a.set || a.clear)
		::Msg("[coop] debug: my flags from the server:%s on,%s off", flag_words(a.set).c_str(), flag_words(a.clear).c_str());
}

// Takes back what this machine set (the session is over, or its world was replaced).
void take_back_own_flags()
{
	const LocalApply a = apply_local(g_applied, 0, actor_flags());
	if (a.clear)
	{
		set_actor_flags(a.clear, false);
		::Msg("[coop] debug: the server's flags taken back:%s", flag_words(a.clear).c_str());
	}
	g_applied = 0;
}

void fire_changed(bool on, Source source)
{
	script::queue_callback("coop_debug_changed", {script::cb_bool(on), script::cb_str(source_name(source))});
}

// The world server: its state changed. Sent from the next frame, to everybody; the Lua of this process hears it now.
void server_changed()
{
	g_dirty = true;
	fire_changed(g_server.on(), g_server.source());
}

void write_server_state(Writer& w) { write_state(w, g_server.message()); }

// Msg::DebugState: a player's machine (a remote client's session, the host player's LocalLink) and a map server's bridge.
void client_on_state(PeerId from, Reader& r)
{
	if (from != kHostPeer || !(mgr().is_client() || is_world_server()))
		return;
	StateMsg m;
	if (!read_state(r, m))
	{
		++g_stats.bad;
		if (log_allowed(g_stats.bad))
			::Msg("! [coop] debug: a malformed DebugState from the server (#%llu): ignored", g_stats.bad);
		return;
	}
	if (!is_news(g_seen_any, g_seen.serial, m.serial))
	{
		++g_stats.stale;
		return;
	}
	const bool was_on = g_seen_any && g_seen.on;
	const Source was_source = g_seen_any ? g_seen.source : Source::Off;
	g_seen = std::move(m);
	g_seen_any = true;
	++g_stats.states_in;
	apply_own_actor(g_seen);
	if (is_world_server())
		return; // the host's process: its world server's half told its Lua already
	if (g_seen.on != was_on || g_seen.source != was_source)
		::Msg("[coop] debug of the server: %s (%s)", g_seen.on ? "on" : "off", source_name(g_seen.source));
	fire_changed(g_seen.on, g_seen.source);
}

// ---------------------------------------------------------------- the console

void print_state()
{
	if (world_server_now())
	{
		say(make_string("[coop] debug: %s (%s), serial %u", g_server.on() ? "on" : "off", source_name(g_server.source()), g_server.serial())
		        .c_str());
		for (const auto& it : g_server.all_flags())
			say(make_string("[coop]   #%u '%s':%s", it.first, nick_of(it.first).c_str(), flag_words(it.second).c_str()).c_str());
		if (g_server.on() && g_server.all_flags().empty())
			say("[coop]   no player has personal flags");
		return;
	}
	if (!mgr().active())
	{
		say(make_string("[coop] debug: no co-op session (the key -dbg %s: a session hosted from here starts with the debug on)",
		    Core.isDebug() ? "is given" : "is not given")
		        .c_str());
		return;
	}
	const StateMsg& m = seen_or_empty();
	say(make_string("[coop] debug of the server: %s (%s), serial %u%s", m.on ? "on" : "off", source_name(m.source), m.serial,
	    g_seen_any ? "" : ", nothing heard yet")
	        .c_str());
	for (const FlagRow& row : m.rows)
		say(make_string("[coop]   #%u:%s", row.peer, flag_words(row.flags).c_str()).c_str());
}

void console_switch(LPCSTR args)
{
	const Switch sw = parse_switch(args ? args : "");
	if (sw == Switch::Bad)
	{
		say("! coop_debug [on | off]: the state without an argument");
		return;
	}
	if (sw == Switch::Status)
	{
		print_state();
		return;
	}
	switch (switch_allowed(mgr().active(), is_world_server(), operator_here()))
	{
	case Operator::Ok: break;
	case Operator::NoSession:
		say("! [coop] coop_debug: no co-op session (a host started with the key -dbg has the debug on from the start)");
		return;
	case Operator::ToServer: send_to_server(sw == Switch::On ? "coop_debug on" : "coop_debug off"); return;
	case Operator::NotServer:
	case Operator::DebugOff:
	case Operator::ServerConsoleOnly:
		say("! [coop] coop_debug: the server's debug is switched in the server's console (the host's, or the dedicated host's window)");
		return;
	}
	const bool on = sw == Switch::On;
	if (!(on ? g_server.switch_on(Source::Console) : g_server.switch_off()))
	{
		say(make_string("[coop] debug is %s already (%s)", on ? "on" : "off", source_name(g_server.source())).c_str());
		return;
	}
	server_changed();
	say(on ? "[coop] debug: on (console). Every player may use the debug window (F7); coop_dbg runs its commands here" :
	         "[coop] debug: off (console). The personal flags of every player are gone");
}

struct ConsoleCall
{
	const std::string* line;
	const ConsoleLine* parsed;
};

void set_string(lua_State* L, const char* key, const std::string& v)
{
	lua_pushlstring(L, v.data(), v.size());
	lua_setfield(L, -2, key);
}

// coop_debug_server.console(line, {cmd, player, peer, keys = {k = v}, words = {...}, rest})
int push_console_call(lua_State* L, const void* ctx)
{
	const ConsoleCall& c = *static_cast<const ConsoleCall*>(ctx);
	lua_pushlstring(L, c.line->data(), c.line->size());
	const ConsoleLine& p = *c.parsed;
	lua_createtable(L, 0, 6);
	set_string(L, "cmd", p.cmd);
	if (!p.player.empty())
		set_string(L, "player", p.player);
	if (p.player_number)
	{
		lua_pushnumber(L, lua_Number(p.peer));
		lua_setfield(L, -2, "peer");
	}
	lua_createtable(L, 0, int(p.keys.size()));
	for (const auto& kv : p.keys)
		set_string(L, kv.first.c_str(), kv.second);
	lua_setfield(L, -2, "keys");
	lua_createtable(L, int(p.words.size()), 0);
	for (size_t i = 0; i < p.words.size(); ++i)
	{
		lua_pushlstring(L, p.words[i].data(), p.words[i].size());
		lua_rawseti(L, -2, int(i + 1));
	}
	lua_setfield(L, -2, "words");
	set_string(L, "rest", p.rest);
	return 2;
}

void console_dbg(LPCSTR args)
{
	const std::string line = args ? args : "";
	ConsoleLine parsed;
	std::string why;
	if (!parse_console_line(line, parsed, why))
	{
		say("! coop_dbg: " + why);
		return;
	}
	switch (console_allowed(mgr().active(), is_world_server(), g_server.on(), parsed.cmd, operator_here()))
	{
	case Operator::Ok: break;
	case Operator::NoSession: say("! [coop] coop_dbg: no co-op session"); return;
	case Operator::NotServer: say("! [coop] coop_dbg: the server's console only (the host's, or the dedicated host's window)"); return;
	case Operator::DebugOff: say("! [coop] coop_dbg: debug_off (coop_debug on first)"); return;
	case Operator::ServerConsoleOnly: say("! [coop] coop_dbg " + parsed.cmd + ": the world server's own console only"); return;
	case Operator::ToServer: send_to_server("coop_dbg " + std::string(detail::trim(line))); return;
	}
	const ConsoleCall call{&line, &parsed};
	switch (script::call_now("coop_debug_server", "console", &push_console_call, &call))
	{
	case script::CallResult::Done: break; // its answer is said by the Lua (coop.debug.say)
	case script::CallResult::NotReady: say("! [coop] coop_dbg: the world's Lua is not up yet (no level)"); break;
	case script::CallResult::Failed: say("! [coop] coop_dbg: coop_debug_server.console failed, see the log"); break;
	}
}

class CCC_CoopDebug : public IConsole_Command
{
public:
	CCC_CoopDebug(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override { console_switch(args); }
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "the server's debug, as sv_cheats: coop_debug on | off (the server's console), without an argument the state");
	}
};

class CCC_CoopDbg : public IConsole_Command
{
public:
	CCC_CoopDbg(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = true;
		bLowerCaseArgs = false; // a nick, a value, the operator's Lua keep their case
	}
	void Execute(LPCSTR args) override { console_dbg(args); }
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "a debug command in the server's console (coop_debug on first): coop_dbg <command> [@player] [key=value ...]; coop_dbg help");
	}
};

// ---------------------------------------------------------------- Lua

bool arg_peer(lua_State* L, int idx, PeerId& out)
{
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	const double v = double(lua_tonumber(L, idx));
	if (!std::isfinite(v) || v < 0.0 || v >= double(kMaxPeerSlots) || v != std::floor(v))
		return false;
	out = PeerId(v);
	return true;
}

int fail(lua_State* L, const char* why)
{
	lua_pushboolean(L, 0);
	lua_pushstring(L, why);
	return 2;
}

// coop.debug.state() -> {on, source, serial, operator, mine, flags = {[peer] = bits}}
int l_state(lua_State* L)
{
	const bool ws = world_server_now();
	const StateMsg m = ws ? g_server.message() : seen_or_empty();
	lua_createtable(L, 0, 6);
	lua_pushboolean(L, m.on ? 1 : 0);
	lua_setfield(L, -2, "on");
	lua_pushstring(L, source_name(m.source));
	lua_setfield(L, -2, "source");
	lua_pushnumber(L, lua_Number(m.serial));
	lua_setfield(L, -2, "serial");
	lua_pushboolean(L, ws ? 1 : 0);
	lua_setfield(L, -2, "operator");
	const bool has_player = mgr().active() && is_player() && !ghost_actor();
	lua_pushnumber(L, lua_Number(has_player ? m.flags_of(mgr().self()) : 0));
	lua_setfield(L, -2, "mine");
	lua_createtable(L, 0, int(m.rows.size()));
	for (const FlagRow& row : m.rows)
	{
		lua_pushnumber(L, lua_Number(row.flags));
		lua_rawseti(L, -2, int(row.peer));
	}
	lua_setfield(L, -2, "flags");
	return 1;
}

// coop.debug.flags(peer) -> bits
int l_flags(lua_State* L)
{
	PeerId peer = kInvalidPeer;
	lua_pushnumber(L, arg_peer(L, 1, peer) ? lua_Number(flags(peer)) : lua_Number(0));
	return 1;
}

// A boolean field of the flags table; absent keeps the bit.
bool table_bit(lua_State* L, const char* key, u8 bit, u8& bits, bool& bad)
{
	lua_getfield(L, 2, key);
	const int t = lua_type(L, -1);
	if (t == LUA_TBOOLEAN)
	{
		if (lua_toboolean(L, -1))
			bits = u8(bits | bit);
		else
			bits = u8(bits & ~bit);
	}
	else if (t != LUA_TNIL)
		bad = true;
	lua_pop(L, 1);
	return !bad;
}

// coop.debug.set_flags(peer, bits | {god, ammo, invis}) -> true | false, why
int l_set_flags(lua_State* L)
{
	if (!world_server_now())
		return fail(L, "the world server only");
	PeerId peer = kInvalidPeer;
	if (!arg_peer(L, 1, peer))
		return fail(L, "bad peer");
	if (!is_player_peer(peer))
		return fail(L, "no such player");
	u8 bits = g_server.flags(peer);
	if (lua_type(L, 2) == LUA_TNUMBER)
	{
		const double v = double(lua_tonumber(L, 2));
		if (!std::isfinite(v) || v < 0.0 || v > double(kFlagsAll) || v != std::floor(v))
			return fail(L, "bad flags");
		bits = u8(v);
	}
	else if (lua_type(L, 2) == LUA_TTABLE)
	{
		bool bad = false;
		table_bit(L, "god", kFlagGod, bits, bad);
		table_bit(L, "ammo", kFlagAmmo, bits, bad);
		table_bit(L, "invis", kFlagInvisible, bits, bad);
		if (bad)
			return fail(L, "bad flags");
	}
	else
		return fail(L, "bad flags");
	switch (g_server.set_flags(peer, bits))
	{
	case ServerState::FlagsResult::Changed:
		server_changed();
		::Msg("[coop] debug: #%u '%s' flags:%s", peer, nick_of(peer).c_str(), flag_words(bits).c_str());
		break;
	case ServerState::FlagsResult::Same: break;
	case ServerState::FlagsResult::DebugOff: return fail(L, kRefusedOff);
	case ServerState::FlagsResult::BadFlags: return fail(L, "bad flags");
	case ServerState::FlagsResult::BadPeer: return fail(L, "bad peer");
	}
	lua_pushboolean(L, 1);
	return 1;
}

int l_is_operator(lua_State* L)
{
	lua_pushboolean(L, world_server_now() ? 1 : 0);
	return 1;
}

// coop.debug.say(line)
int l_say(lua_State* L)
{
	size_t n = 0;
	const char* s = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : nullptr;
	if (s)
		say(std::string(s, n < kMaxSay ? n : kMaxSay));
	return 0;
}

// coop.debug.unseen(id) -> bool (work package 4; coop_npc_combat asks it in xr_combat_ignore.is_enemy for every enemy candidate)
int l_unseen(lua_State* L)
{
	bool yes = false;
	if (lua_type(L, 1) == LUA_TNUMBER && g_pGameLevel && anyone_has(kFlagInvisible))
	{
		const double v = double(lua_tonumber(L, 1));
		if (std::isfinite(v) && v >= 0.0 && v < 65535.0 && v == std::floor(v))
			yes = npc_ai::debug_unseen(Level().Objects.net_Find(u16(v)));
	}
	lua_pushboolean(L, yes ? 1 : 0);
	return 1;
}
} // namespace

// ---------------------------------------------------------------- the operator of a world server (plan 2026-11-12 §2.3)

// Declared in coop_dedicated.cpp (run_for), the one caller. `rest`: what follows "coop_dbg " in a line that passed op::command_verdict.
// The line runs as a request of the operator's peer, never as the server's console: coop_debug_server.operator_console(line, parsed,
// peer) decides with his identity (a script without that function refuses: nothing runs). The server console's own commands (Lua) are
// refused here, before any Lua.
void operator_dbg(PeerId from, const std::string& rest)
{
	ConsoleLine parsed;
	std::string why;
	if (!parse_console_line(rest, parsed, why))
	{
		say("! coop_dbg: " + why);
		return;
	}
	switch (remote_console_allowed(mgr().active(), g_server.on(), parsed.cmd))
	{
	case Operator::Ok: break;
	case Operator::NoSession: say("! [coop] coop_dbg: no co-op session"); return;
	case Operator::DebugOff: say("! [coop] coop_dbg: debug_off (coop_debug on first)"); return;
	case Operator::ServerConsoleOnly:
	case Operator::NotServer:
	case Operator::ToServer:
		::Msg("! [coop] operator #%u: coop_dbg %s refused (the server's console only)", from, parsed.cmd.c_str());
		say("! [coop] coop_dbg " + parsed.cmd + ": the world server's own console only");
		return;
	}
	struct Call
	{
		const std::string* line;
		const ConsoleLine* parsed;
		PeerId peer;
	};
	const Call call{&rest, &parsed, from};
	const auto push = [](lua_State* L, const void* ctx) -> int
	{
		const Call& c = *static_cast<const Call*>(ctx);
		const ConsoleCall inner{c.line, c.parsed};
		push_console_call(L, &inner);
		lua_pushnumber(L, lua_Number(c.peer));
		return 3;
	};
	switch (script::call_now("coop_debug_server", "operator_console", push, &call))
	{
	case script::CallResult::Done: break;
	case script::CallResult::NotReady: say("! [coop] coop_dbg: the world's Lua is not up yet (no level)"); break;
	case script::CallResult::Failed:
		say("! [coop] coop_dbg: refused, coop_debug_server.operator_console failed or is missing, see the log");
		break;
	}
}

// ---------------------------------------------------------------- the module

void install_server(Session&) {} // the world server takes no DebugState (it is HostOnly: a client that sends one is kicked)

void install_client(Endpoint& e) { e.on(Msg::DebugState, &client_on_state); }

void on_session_started()
{
	reset();
	if (!is_world_server())
		return;
	if (Core.isDebug() && g_server.switch_on(Source::LaunchKey))
	{
		server_changed();
		::Msg("[coop] debug: on (the key -dbg of this host). coop_debug off switches it off");
	}
}

void reset()
{
	take_back_own_flags();
	g_server.reset();
	g_seen = StateMsg();
	g_seen_any = false;
	g_dirty = false;
}

void on_peer_left(PeerId peer)
{
	if (is_world_server() && g_server.drop_peer(peer))
		server_changed();
}

void host_on_peer_in_world(PeerId peer)
{
	if (!world_server_now())
		return;
	Writer w(32);
	write_server_state(w);
	net::host_send(peer, Msg::DebugState, w);
	++g_stats.states_to_one;
}

void frame()
{
	if (!g_dirty)
		return;
	g_dirty = false;
	if (!world_server_now())
		return;
	Writer w(64);
	write_server_state(w);
	net::host_broadcast(Msg::DebugState, w);
	++g_stats.states_sent;
}

void print_status()
{
	const bool ws = world_server_now();
	const StateMsg m = ws ? g_server.message() : seen_or_empty();
	::Msg("[coop] debug: %s (%s), serial %u, %u player(s) with flags%s | states: sent %llu, at joins %llu, taken %llu, stale %llu, bad "
	      "%llu | god hits dropped %llu, coop.dbg requests refused %llu, local cheats refused %llu",
	    m.on ? "on" : "off", source_name(m.source), m.serial, u32(m.rows.size()), ws ? " (this is the server)" : "", g_stats.states_sent,
	    g_stats.states_to_one, g_stats.states_in, g_stats.stale, g_stats.bad, g_stats.god_hits, g_stats.requests_refused,
	    g_stats.cheats_refused);
}

bool on() { return world_server_now() ? g_server.on() : g_seen_any && g_seen.on; }

uint8_t flags(PeerId peer) { return world_server_now() ? g_server.flags(peer) : seen_or_empty().flags_of(peer); }

bool anyone_has(uint8_t flag)
{
	if (world_server_now())
	{
		for (const auto& it : g_server.all_flags())
			if (it.second & flag)
				return true;
		return false;
	}
	if (!g_seen_any || !g_seen.on)
		return false;
	for (const FlagRow& row : g_seen.rows)
		if (row.flags & flag)
			return true;
	return false;
}

bool host_drops_hit(PeerId peer)
{
	if (!(flags(peer) & kFlagGod))
		return false;
	++g_stats.god_hits;
	return true;
}

bool refuses_request(const std::string& name)
{
	if (!debug::refuses_request(name, g_server.on()))
		return false;
	++g_stats.requests_refused;
	if (log_allowed(g_stats.requests_refused))
		::Msg("[coop] debug: request '%s' refused, the debug is off (#%llu)", name.c_str(), g_stats.requests_refused);
	return true;
}

bool refuse_local_cheat(const char* command)
{
	const char* name = command ? command : "";
	switch (local_cheat(name, mgr().active(), is_world_server()))
	{
	case Cheat::Run: return false;
	case Cheat::RefuseClient:
		++g_stats.cheats_refused;
		::Msg("! [coop] %s: in a co-op session the server turns the debug on (the host's F7, or coop_debug on in its console); "
		      "ask it for this in the debug window (F7)",
		    name);
		return true;
	case Cheat::ServerFlags:
		++g_stats.cheats_refused;
		::Msg("! [coop] %s: in a co-op session the personal flags go through the server's debug: F7, or coop_dbg %s @player on=1",
		    name, xr_strcmp(name, "g_god") ? "ammo" : "god");
		return true;
	}
	return false;
}

void register_console()
{
	CMD1(CCC_CoopDebug, "coop_debug");
	CMD1(CCC_CoopDbg, "coop_dbg");
}

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"state", &l_state},
		{"flags", &l_flags},
		{"set_flags", &l_set_flags},
		{"is_operator", &l_is_operator},
		{"say", &l_say},
		{"unseen", &l_unseen},
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	lua_createtable(L, 0, 8);
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_setfield(L, -2, "debug"); // coop.debug
	lua_pop(L, 1); // coop
}
} // namespace debug
} // namespace coop
