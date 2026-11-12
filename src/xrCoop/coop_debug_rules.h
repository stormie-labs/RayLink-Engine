// Anomaly Coop — the server's debug (docs/DEBUG_DESIGN.md §B, protocol 29): the pure half.
//
// Debug is a right of the server, as sv_cheats: the world server (the host's process, or the dedicated host) keeps it on or off, the
// key -dbg of that process or its console (coop_debug on|off) switches it. Every player, the host player too, and every map server gets
// the state in Msg::DebugState: on, who switched it, a serial, and each player's personal flags (1 god, 2 unlimited ammo, 4 invisible
// to the AI). Here:
//   - StateMsg, the layout of DebugState, read with every check (a client that misreads a flag must not get god);
//   - ServerState, the world server's book: the switch, the flags, the serial;
//   - apply_local: what a player's machine sets on its own actor from the message, and what it takes back (only its own);
//   - refuses_request: the engine's gate of the coop.dbg.* requests (coop.sync): while debug is off only coop.dbg.view passes;
//   - local_cheat: the engine's own cheats of the -dbg key (g_god, run_string, ...) in a session;
//   - the console of the world server: coop_debug's word, the gates of coop_debug and coop_dbg, and coop_dbg's line;
//   - work package 4: who the AI of a level's authority leaves out (ai_ignores), whom a monster's stamina blow spares (blow_spares).
//
// Pure C++17, header-only, no engine dependencies: used by the engine (xrGame/coop/coop_debug.cpp) and by the unit-test exe
// (tests/test_debug_rules.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_protocol.h"
#include "coop_wire.h"

namespace coop
{
namespace debug
{
// Who switched the debug on. The values are on the wire.
enum class Source : uint8_t
{
	Off = 0, // off
	LaunchKey = 1, // the key -dbg of the world server's process (Core.isDebug: -dbg, -dbgdev, -dbgact, -dbgbullet, each an exact key)
	Console = 2, // coop_debug on in the world server's console (the host's F7 button runs it)
};

inline const char* source_name(Source s)
{
	switch (s)
	{
	case Source::Off: return "off";
	case Source::LaunchKey: return "key";
	case Source::Console: return "console";
	}
	return "?";
}

// A player's personal flags (§B.9). The values are on the wire.
constexpr uint8_t kFlagGod = 1; // his machine: AF_GODMODE on his actor; the authority of his level: no PlayerHit for him
constexpr uint8_t kFlagAmmo = 2; // his machine: AF_UNLIMITEDAMMO on his actor
constexpr uint8_t kFlagInvisible = 4; // the authority of his level: the AI leaves him out (ai_ignores; npc_ai::debug_unseen, Lua)
constexpr uint8_t kFlagsAll = kFlagGod | kFlagAmmo | kFlagInvisible;
// What a player's machine sets on its own actor; invisibility is the authority's business.
constexpr uint8_t kLocalFlags = kFlagGod | kFlagAmmo;

struct FlagRow
{
	PeerId peer = kInvalidPeer;
	uint8_t flags = 0;
};

// Msg::DebugState, world server -> every player (the host player over the LocalLink) and every map server, at every change, at the end
// of a player's join and of a map server's.
// Wire: u8 on (0 | 1), u8 source (Source), u32 serial, varu n (n <= kMaxPeerSlots), n x {u32 peer, u8 flags}.
// Off has the source Off and no rows (switching off clears every flag); on has a source; a row per player with flags, peers < kMaxPeerSlots,
// each once, flags within kFlagsAll.
struct StateMsg
{
	bool on = false;
	Source source = Source::Off;
	uint32_t serial = 0; // grows at every change: a receiver drops an older one (is_news)
	std::vector<FlagRow> rows;

	// The flags of a player; 0 when he has no row or the debug is off.
	uint8_t flags_of(PeerId peer) const
	{
		if (!on)
			return 0;
		for (const FlagRow& r : rows)
			if (r.peer == peer)
				return r.flags;
		return 0;
	}
};

inline void write_state(Writer& w, const StateMsg& m)
{
	w.u8(m.on ? 1 : 0);
	w.u8(uint8_t(m.source));
	w.u32(m.serial);
	w.varu(m.rows.size());
	for (const FlagRow& r : m.rows)
	{
		w.u32(r.peer);
		w.u8(r.flags);
	}
}

// False on an overrun or anything StateMsg says a message is not; `out` is untouched then.
inline bool read_state(Reader& r, StateMsg& out)
{
	StateMsg m;
	const uint8_t on = r.u8();
	const uint8_t source = r.u8();
	m.serial = r.u32();
	const uint64_t n = r.varu();
	if (!r.ok() || on > 1 || source > uint8_t(Source::Console) || n > kMaxPeerSlots)
		return false;
	m.on = on == 1;
	m.source = Source(source);
	if (m.on != (m.source != Source::Off) || (!m.on && n != 0))
		return false;
	m.rows.reserve(size_t(n));
	for (uint64_t i = 0; i < n; ++i)
	{
		FlagRow row;
		row.peer = r.u32();
		row.flags = r.u8();
		if (!r.ok() || row.peer >= kMaxPeerSlots || (row.flags & ~kFlagsAll) != 0)
			return false;
		for (const FlagRow& seen : m.rows)
			if (seen.peer == row.peer)
				return false;
		m.rows.push_back(row);
	}
	out = std::move(m);
	return true;
}

// The receiver: the first message is news whatever its serial (a server that was never switched sends 0), then only a newer serial.
// The same state comes twice at a join (queued while he loads, and sent at its end): the second one is no news.
inline bool is_news(bool seen_any, uint32_t seen_serial, uint32_t serial) { return !seen_any || serial > seen_serial; }

// The world server's state: lives in its process for the session (a world loaded or a host's move keeps it; the next session starts off,
// or on with the key).
class ServerState
{
public:
	enum class FlagsResult : uint8_t
	{
		Changed,
		Same, // he has these flags already
		DebugOff, // nobody gets flags while the debug is off
		BadFlags, // a bit outside kFlagsAll
		BadPeer, // no player slot
	};

	bool on() const { return m_on; }
	Source source() const { return m_source; }
	uint32_t serial() const { return m_serial; }
	uint8_t flags(PeerId peer) const
	{
		const auto it = m_flags.find(peer);
		return it == m_flags.end() ? uint8_t(0) : it->second;
	}
	// Every player with flags (none has 0), by peer.
	const std::map<PeerId, uint8_t>& all_flags() const { return m_flags; }

	// True if the state changed. Already on: nothing changes (the source of the switch stays).
	bool switch_on(Source source)
	{
		if (m_on || source == Source::Off)
			return false;
		m_on = true;
		m_source = source;
		++m_serial;
		return true;
	}
	// Off clears every flag. True if the state changed.
	bool switch_off()
	{
		if (!m_on)
			return false;
		m_on = false;
		m_source = Source::Off;
		m_flags.clear();
		++m_serial;
		return true;
	}
	FlagsResult set_flags(PeerId peer, uint8_t flags)
	{
		if (peer >= kMaxPeerSlots)
			return FlagsResult::BadPeer;
		if ((flags & ~kFlagsAll) != 0)
			return FlagsResult::BadFlags;
		if (!m_on)
			return FlagsResult::DebugOff;
		if (this->flags(peer) == flags)
			return FlagsResult::Same;
		if (flags)
			m_flags[peer] = flags;
		else
			m_flags.erase(peer);
		++m_serial;
		return FlagsResult::Changed;
	}
	// A player left: his flags go. True if he had any.
	bool drop_peer(PeerId peer)
	{
		if (!m_flags.erase(peer))
			return false;
		++m_serial;
		return true;
	}
	// A new session (or none): off, serial 0.
	void reset()
	{
		m_on = false;
		m_source = Source::Off;
		m_serial = 0;
		m_flags.clear();
	}
	StateMsg message() const
	{
		StateMsg m;
		m.on = m_on;
		m.source = m_source;
		m.serial = m_serial;
		for (const auto& it : m_flags)
			m.rows.push_back({it.first, it.second});
		return m;
	}

private:
	bool m_on = false;
	Source m_source = Source::Off;
	uint32_t m_serial = 0;
	std::map<PeerId, uint8_t> m_flags; // no row has 0
};

// ---------------------------------------------------------------- the player's machine: its own actor

struct LocalApply
{
	uint8_t set = 0; // to switch on on the actor
	uint8_t clear = 0; // to switch off
	uint8_t applied = 0; // what this machine set from DebugState and owns after it
};

// `applied`: what this machine set on its actor from DebugState before; `wanted`: its row now (wanted_local); `current`: what its actor
// has now. Only kLocalFlags count. What is wanted and missing is set (and owned); what it owns and is no longer wanted is cleared; a flag
// the player had himself (g_god before the session) is never taken: it stays his.
inline LocalApply apply_local(uint8_t applied, uint8_t wanted, uint8_t current)
{
	applied &= kLocalFlags;
	wanted &= kLocalFlags;
	current &= kLocalFlags;
	LocalApply a;
	a.set = uint8_t(wanted & ~current);
	a.clear = uint8_t(applied & ~wanted & current);
	a.applied = uint8_t((applied & wanted & current) | a.set);
	return a;
}

// The flags this machine's own actor should have: its row, if the debug is on and the process has a player of its own (a map server's
// and the dedicated host's actor is a ghost: it keeps what it has).
inline uint8_t wanted_local(const StateMsg& m, PeerId self, bool has_player)
{
	return has_player ? uint8_t(m.flags_of(self) & kLocalFlags) : uint8_t(0);
}

// ---------------------------------------------------------------- the gate of the requests (coop.sync.request, §B.5)

constexpr std::string_view kRequestPrefix = "coop.dbg.";
constexpr std::string_view kViewRequest = "coop.dbg.view";
// The engine's answer to a refused one (coop_debug_client.script maps it to its code debug_off).
constexpr const char* kRefusedOff = "debug is off";

inline bool is_debug_request(std::string_view name) { return name.compare(0, kRequestPrefix.size(), kRequestPrefix) == 0; }

// The world server, before it looks for the handler: while the debug is off only the view passes. Other names are not this gate's.
inline bool refuses_request(std::string_view name, bool on) { return !on && is_debug_request(name) && name != kViewRequest; }

// ---------------------------------------------------------------- the engine's local cheats (§B.8)

enum class Cheat : uint8_t
{
	Run, // as the stock: out of a session, or not one of these
	RefuseClient, // a process without the world server in a session (a client, a map server): the server's debug decides
	ServerFlags, // the world server in a session: g_god / g_unlimitedammo go through the personal flags (F7, coop_dbg god)
};

// What the key -dbg registers in the engine's console (console_commands.cpp, MASTER_GOLD); log_missing_ini is no cheat.
inline bool is_local_cheat(std::string_view cmd)
{
	return cmd == "g_god" || cmd == "g_unlimitedammo" || cmd == "run_string" || cmd == "run_script" || cmd == "jump_to_level" ||
	    cmd == "ph_gravity" || cmd == "g_end_modif";
}

inline Cheat local_cheat(std::string_view cmd, bool in_session, bool world_server)
{
	if (!in_session || !is_local_cheat(cmd))
		return Cheat::Run;
	if (!world_server)
		return Cheat::RefuseClient;
	return cmd == "g_god" || cmd == "g_unlimitedammo" ? Cheat::ServerFlags : Cheat::Run; // the operator keeps his Lua and the rest
}

// ---------------------------------------------------------------- the console of the world server (§B.7)

namespace detail
{
inline bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string_view trim(std::string_view s)
{
	while (!s.empty() && blank(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && blank(s.back()))
		s.remove_suffix(1);
	return s;
}

inline bool ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

inline std::string lower(std::string_view s)
{
	std::string out(s);
	for (char& c : out)
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	return out;
}
} // namespace detail

// coop_debug [on | off]: what its argument says.
enum class Switch : uint8_t
{
	Status, // no argument: show the state
	On,
	Off,
	Bad,
};

inline Switch parse_switch(std::string_view arg)
{
	arg = detail::trim(arg);
	if (arg.empty())
		return Switch::Status;
	if (arg == "on" || arg == "1")
		return Switch::On;
	if (arg == "off" || arg == "0")
		return Switch::Off;
	return Switch::Bad;
}

enum class Operator : uint8_t
{
	Ok,
	NoSession, // no co-op session
	NotServer, // a process without the world server: a client, a map server
	DebugOff, // coop_dbg while the debug is off (owner 2026-11-07: as sv_cheats)
	ToServer, // plan 2026-11-12 §2.3: the operator of a world server (a client): the line goes to the server as an OperatorCommand
	ServerConsoleOnly, // a command only the world server's own console runs (the Lua of `coop_dbg lua`): never over the network
};

// The commands of coop_dbg that only the world server's own console runs (coop_debug_cmds.script class "operator": Lua on the server's
// PC). No line that came over the network runs one, the operator's neither. `cmd` as parse_console_line keeps it (lower case).
inline bool server_console_only(std::string_view cmd) { return cmd == "lua"; }

// coop_debug on|off: the world server's console in a session (the state alone is shown anywhere). `op`: this client holds the host's
// rights of a world server (Session::self_host_rights): his own console sends the line to the server, which decides.
inline Operator switch_allowed(bool active, bool world_server, bool op = false)
{
	if (!active)
		return Operator::NoSession;
	if (world_server)
		return Operator::Ok;
	return op ? Operator::ToServer : Operator::NotServer;
}

// coop_dbg <cmd> ...: the world server's console in a session with the debug on. `help` lists the commands while it is off too: it
// changes nothing. The operator's console sends any line but the server console's own (the server knows whether its debug is on).
inline Operator console_allowed(bool active, bool world_server, bool on, std::string_view cmd, bool op = false)
{
	const Operator o = switch_allowed(active, world_server, op);
	if (o == Operator::ToServer)
		return server_console_only(cmd) ? Operator::ServerConsoleOnly : Operator::ToServer;
	if (o != Operator::Ok)
		return o;
	return on || cmd == "help" ? Operator::Ok : Operator::DebugOff;
}

// The world server, coop_dbg of its operator (Msg::OperatorCommand): the rule of its own console, less what only that console runs.
inline Operator remote_console_allowed(bool active, bool on, std::string_view cmd)
{
	if (server_console_only(cmd))
		return Operator::ServerConsoleOnly;
	return console_allowed(active, true, on, cmd);
}

constexpr size_t kMaxConsoleLine = 1024; // the whole line after coop_dbg
constexpr size_t kMaxCommand = 32;
constexpr size_t kMaxToken = 64; // a word, a key, a value, a player's name (§B.5: strings up to 64)
constexpr size_t kMaxKeys = 16;
constexpr size_t kMaxWords = 8;

// coop_dbg <cmd> [@player] [key=value ...] [word ...] (§B.7). The command and the keys in any case, kept in lower case (the registry's
// ids); values, words and the player's name as typed. "..." keeps blanks together; a part in quotes is plain text: a quoted '@' is no
// player and a quoted '=' no key. Lua decides what a word means (coop_debug_server.script: "surge start", "help spawn_item").
struct ConsoleLine
{
	std::string cmd;
	std::string player; // "@Bob" -> "Bob"; empty: none on the line
	bool player_number = false; // "@2": digits alone, three at most: a peer number (Lua: P.player_by_name)
	PeerId peer = kInvalidPeer; // its value
	std::vector<std::pair<std::string, std::string>> keys; // in the order typed
	std::vector<std::string> words;
	std::string rest; // everything after the command, trimmed, as typed

	const std::string* key(std::string_view k) const
	{
		for (const auto& kv : keys)
			if (kv.first == k)
				return &kv.second;
		return nullptr;
	}
};

// Commands whose rest is taken as typed, with no words or keys read from it: `lua <code>` (the operator's Lua).
inline bool raw_command(std::string_view cmd) { return cmd == "lua"; }

// False with `why` for a line that is no command; `out` is filled as far as it got.
inline bool parse_console_line(std::string_view line, ConsoleLine& out, std::string& why)
{
	if (line.size() > kMaxConsoleLine)
	{
		why = "the line is longer than " + std::to_string(kMaxConsoleLine) + " bytes";
		return false;
	}
	// the command: the first word
	size_t i = 0;
	while (i < line.size() && detail::blank(line[i]))
		++i;
	const size_t cmd_from = i;
	while (i < line.size() && !detail::blank(line[i]))
		++i;
	const std::string_view cmd = line.substr(cmd_from, i - cmd_from);
	if (cmd.empty())
	{
		why = "no command: coop_dbg help";
		return false;
	}
	for (const char c : cmd)
		if (!detail::ident_char(c))
		{
			why = "a command is letters, digits and '_': '" + std::string(cmd) + "'";
			return false;
		}
	if (cmd.size() > kMaxCommand)
	{
		why = "the command is longer than " + std::to_string(kMaxCommand);
		return false;
	}
	out.cmd = detail::lower(cmd);
	out.rest = std::string(detail::trim(line.substr(i)));
	if (raw_command(out.cmd))
		return true;

	const size_t npos = std::string::npos;
	const size_t n = line.size();
	while (i < n)
	{
		while (i < n && detail::blank(line[i]))
			++i;
		if (i >= n)
			break;
		// a token: up to a blank outside quotes; quoted parts lose their quotes; quote_at: where the first quoted part begins in it
		std::string text;
		size_t quote_at = npos;
		while (i < n && !detail::blank(line[i]))
		{
			if (line[i] != '"')
			{
				text += line[i++];
				continue;
			}
			if (quote_at == npos)
				quote_at = text.size();
			++i;
			while (i < n && line[i] != '"')
				text += line[i++];
			if (i >= n)
			{
				why = "a quote is never closed";
				return false;
			}
			++i; // the closing quote
		}
		const size_t eq = text.find('=');
		if (!text.empty() && text[0] == '@' && quote_at != 0)
		{
			if (!out.player.empty())
			{
				why = "two players: '@" + out.player + "' and '" + text + "'";
				return false;
			}
			std::string name = text.substr(1);
			if (name.empty())
			{
				why = "'@' without a player";
				return false;
			}
			if (name.size() > kMaxToken)
			{
				why = "a player's name is longer than " + std::to_string(kMaxToken);
				return false;
			}
			if (quote_at == npos && name.size() <= 3 && name.find_first_not_of("0123456789") == npos)
			{
				out.player_number = true;
				out.peer = PeerId(std::stoul(name));
			}
			out.player = std::move(name);
		}
		else if (eq != npos && (quote_at == npos || eq < quote_at))
		{
			const std::string key = detail::lower(text.substr(0, eq));
			std::string value = text.substr(eq + 1);
			if (key.empty())
			{
				why = "'=" + value + "' has no key";
				return false;
			}
			for (const char c : key)
				if (!detail::ident_char(c))
				{
					why = "a key is letters, digits and '_': '" + key + "'";
					return false;
				}
			if (key.size() > kMaxToken || value.size() > kMaxToken)
			{
				why = "'" + key + "': a key or a value is longer than " + std::to_string(kMaxToken);
				return false;
			}
			if (value.empty())
			{
				why = "'" + key + "=' has no value";
				return false;
			}
			if (out.key(key))
			{
				why = "the key '" + key + "' twice";
				return false;
			}
			if (out.keys.size() >= kMaxKeys)
			{
				why = "more than " + std::to_string(kMaxKeys) + " keys";
				return false;
			}
			out.keys.emplace_back(key, std::move(value));
		}
		else
		{
			if (text.empty())
			{
				why = "an empty word (\"\")";
				return false;
			}
			if (text.size() > kMaxToken)
			{
				why = "a word is longer than " + std::to_string(kMaxToken);
				return false;
			}
			if (out.words.size() >= kMaxWords)
			{
				why = "more than " + std::to_string(kMaxWords) + " words";
				return false;
			}
			out.words.push_back(std::move(text));
		}
	}
	return true;
}

// ---------------------------------------------------------------- the AI and the monsters' blows (§B.9, A.2 p. 7; work package 4)

// Who a body the AI looks at, or a monster's blow lands on, is on this machine.
enum class Body : uint8_t
{
	Other, // nobody's character: an NPC, a monster, a parked character whose player left
	OwnPlayer, // this machine's actor, played here (a player's machine, the host's process)
	Ghost, // this machine's actor, played by nobody (a map server, the dedicated host)
	Remote, // another player's character here (a proxy on the authority of his level); `flags` are his player's
};

// The AI of the authority of a level (the world server, a map server) leaves out a player with the flag invisible, as the stock
// ai_ignore_actor leaves out the actor: no vision, hearing or hit memory of him, and he is no enemy. `flags`: his player's flags in the
// state this process acts on (0 while the debug is off). A player's machine has no AI of its own (puppets): never there.
inline bool ai_ignores(bool session, bool authority, Body body, uint8_t flags)
{
	return session && authority && (body == Body::OwnPlayer || body == Body::Remote) && (flags & kFlagInvisible) != 0;
}

// A monster's blow that drains stamina (a burer's anti-aim, a controller's melee) spares the player it lands on. In a session: his flag
// god from the server, not this process's AF_GODMODE (a g_god of before the session); the actor's scripted god (AF_GODMODE_RT, a scene)
// still spares him; a ghost is nobody: nothing lands. Out of a session: the stock GodMode() (`stock_god`).
inline bool blow_spares(bool session, Body body, uint8_t flags, bool stock_god, bool scripted_god)
{
	if (!session)
		return stock_god;
	switch (body)
	{
	case Body::Ghost: return true;
	case Body::OwnPlayer: return scripted_god || (flags & kFlagGod) != 0;
	case Body::Remote: return (flags & kFlagGod) != 0;
	case Body::Other: return false;
	}
	return false;
}
} // namespace debug
} // namespace coop
