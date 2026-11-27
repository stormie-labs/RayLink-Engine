// Anomaly Coop — the map servers, the pure part.
// Every map is computed on the host's PC. A level with players and without the host is led by a background process of
// the same exe, a map server; it connects to the host's process over the loopback, the players of its level are virtual peers on it, and a
// client talks to the host's process alone. This header holds what the engine only has to carry out:
//   - bridges       the peer ids of the map servers on the host's session: no player slots;
//   - Args          the key -coop_mapserver <level> <gvid> <port> <token>, both ways;
//   - fsgame        the fsgame_ms<k>.ltx of a map server: the host's, with an appdata_ms<k> of its own next to the host's appdata;
//   - hello_verdict who may connect as a map server;
//   - route_incoming, bridge_takes, map_server_sends: which message goes where;
//   - VirtualPeers  who stands on a map server's level, from the world server's LevelMap: who came, who is in, who left.
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop/srv/coop_mapserver.cpp, coop/srv/coop_trunk.cpp), by the session and
// by the unit-test exe.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../coop_text.h"
#include "coop_levels_wire.h"
#include "../coop_protocol.h"
#include "coop_scope.h"
#include "coop_token.h"

namespace coop
{
namespace mapserver
{
// ---------------------------------------------------------------- bridges

// A map server is no player: its connection has a peer id of its own, far from the slots of the players (0 .. kMaxPeerSlots - 1).
// k = 1 .. kMaxBridges is the number of the map server, the one of its fsgame_ms<k>.ltx and of its slice of ids
// (coop_levels_keeper.h leader_ids_of).
constexpr uint32_t kMaxBridges = 7; // coop_max_map_servers is 0..7
constexpr PeerId kBridgeBase = 0x100;

constexpr PeerId bridge_peer(uint32_t k) { return kBridgeBase + k; }
constexpr bool is_bridge(PeerId peer) { return peer > kBridgeBase && peer <= kBridgeBase + kMaxBridges; }
static_assert(kBridgeBase >= kMaxPeerSlots, "a bridge's id is no player slot");
// The number of a map server by its bridge, 0 for anybody else.
constexpr uint32_t bridge_index(PeerId peer) { return is_bridge(peer) ? peer - kBridgeBase : 0; }

// Does the session's connection belong to the process that runs now? A connection outlives its process by the link's timeout, and by
// then a new process of the same number may be loading: the end of the old connection must not end the new process. joined() when the
// session reports the bridge, cut() when the process is given up on (the caller closes the connection), left() when the session
// reports its end: true only for the connection of the running process.
struct BridgeLink
{
	void joined() { m_linked = true; }
	void cut() { m_linked = false; }
	bool left()
	{
		const bool was = m_linked;
		m_linked = false;
		return was;
	}
	bool linked() const { return m_linked; }

private:
	bool m_linked = false;
};

// ---------------------------------------------------------------- the key

using srv::Token;
using srv::token_hex;

// -coop_mapserver <level> <gvid> <port> <token>: the level it leads (game graph level id), the vertex its ghost stands on, the
// port of the host's process, the token it proves itself with.
struct Args
{
	uint16_t level = levels::wire::kNoLevel;
	uint16_t graph = 0xFFFF;
	uint16_t port = 0;
	Token token{};
};

constexpr char kKey[] = "-coop_mapserver";

namespace detail
{
// A whole number up to `max` that ends at a blank or at the end of the text. Moves `p` past it and the blanks after it.
inline bool read_number(const char*& p, uint32_t max, uint32_t& out)
{
	const char* q = p;
	while (ascii::digit(*q))
		++q;
	uint64_t v = 0;
	if (!ascii::parse_decimal(std::string_view(p, size_t(q - p)), max, v, ascii::kAnyDigits, true) || (*q && *q != ' '))
		return false;
	out = uint32_t(v);
	while (*q == ' ')
		++q;
	p = q;
	return true;
}
} // namespace detail

// The arguments of the key anywhere in a command line. False, `out` as it was: no such key, or anything wrong with it. A longer
// key with the same beginning is not this one.
inline bool parse_args(const char* params, Args& out)
{
	if (!params)
		return false;
	const size_t key = sizeof(kKey) - 1;
	const char* p = params;
	for (;;)
	{
		p = std::strstr(p, kKey);
		if (!p)
			return false;
		if ((p == params || p[-1] == ' ') && p[key] == ' ')
			break;
		p += key;
	}
	p += key;
	while (*p == ' ')
		++p;
	uint32_t level = 0, graph = 0, port = 0;
	if (!detail::read_number(p, 0xFFFE, level) || !detail::read_number(p, 0xFFFE, graph) ||
	    !detail::read_number(p, 0xFFFF, port) || port == 0)
		return false;
	Args a;
	if (!srv::parse_bridge_token(p, a.token))
		return false;
	a.level = uint16_t(level);
	a.graph = uint16_t(graph);
	a.port = uint16_t(port);
	out = a;
	return true;
}

// The key and its arguments, as parse_args reads them.
inline std::string key_text(const Args& a)
{
	return std::string(kKey) + " " + std::to_string(a.level) + " " + std::to_string(a.graph) + " " + std::to_string(a.port) + " " +
	    token_hex(a.token);
}

// ---------------------------------------------------------------- fsgame

// The value of -fsltx in a command line, "fsgame.ltx" (the engine's default) without one.
inline std::string fsltx_of(const char* params)
{
	const char* p = ascii::find_key(params, "-fsltx");
	if (!p)
		return "fsgame.ltx";
	while (*p == ' ')
		++p;
	std::string out;
	while (*p && *p != ' ')
		out.push_back(*p++);
	return out.empty() ? std::string("fsgame.ltx") : out;
}

// fsgame_ms<k>.ltx next to the host's own.
inline std::string fsltx_name(const std::string& host_fsltx, uint32_t k)
{
	const size_t slash = host_fsltx.find_last_of("\\/");
	const std::string dir = slash == std::string::npos ? std::string() : host_fsltx.substr(0, slash + 1);
	return dir + "fsgame_ms" + std::to_string(k) + ".ltx";
}

namespace detail
{
inline size_t skip_blanks(const std::string& s, size_t at)
{
	while (at < s.size() && (s[at] == ' ' || s[at] == '\t'))
		++at;
	return at;
}
} // namespace detail

// Where the value of $app_data_root$ (its last field) is in the text of an fsgame.ltx: [begin, end), the blanks around it not in
// it. False when there is no such line.
inline bool find_app_data(const std::string& text, size_t& begin, size_t& end)
{
	static const char kKeyName[] = "$app_data_root$";
	const size_t name = sizeof(kKeyName) - 1;
	for (size_t line = 0; line < text.size();)
	{
		size_t stop = text.find_first_of("\r\n", line);
		if (stop == std::string::npos)
			stop = text.size();
		const size_t first = detail::skip_blanks(text, line);
		if (text.compare(first, name, kKeyName) == 0)
		{
			const size_t eq = detail::skip_blanks(text, first + name);
			if (eq < stop && text[eq] == '=')
			{
				const size_t bar = text.rfind('|', stop);
				if (bar == std::string::npos || bar < eq)
					return false;
				begin = detail::skip_blanks(text, bar + 1);
				end = stop;
				while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
					--end;
				return end > begin;
			}
		}
		line = text.find_first_not_of("\r\n", stop);
		if (line == std::string::npos)
			break;
	}
	return false;
}

// A folder that stands next to `dir` under the name `name`: appdata_a\ -> appdata_ms1\, D:\x\appdata -> D:\x\appdata_ms1\.
inline std::string sibling_dir(std::string dir, const std::string& name)
{
	while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/'))
		dir.pop_back();
	const size_t slash = dir.find_last_of("\\/");
	return (slash == std::string::npos ? std::string() : dir.substr(0, slash + 1)) + name + "\\";
}

// The folder of map server k.
inline std::string appdata_name(uint32_t k) { return "appdata_ms" + std::to_string(k); }

// The fsgame.ltx of map server k: the host's, every line as it is (the same gamedata, the same mods), its $app_data_root$ a folder
// of its own next to the host's. `out` gets the text and `dir` that folder as the line names it. False: no such line.
inline bool rewrite_fsgame(const std::string& text, uint32_t k, std::string& out, std::string& dir)
{
	size_t begin = 0, end = 0;
	if (!find_app_data(text, begin, end))
		return false;
	dir = sibling_dir(text.substr(begin, end - begin), appdata_name(k));
	out = text.substr(0, begin) + dir + text.substr(end);
	return true;
}

// The user.ltx of a map server: the host's, with what a process nobody looks at needs: a small window that keeps running without focus.
inline std::string map_server_user_ltx(const std::string& text)
{
	static const char* const kOwn[][2] = {{"rs_screenmode", "windowed"}, {"vid_mode", "640x480"}, {"g_always_active", "on"}};
	// What only a game started with -dbg registers (console_commands.cpp) and saves: a server without the key said "Unknown command"
	// for each of them at every start. No server needs them: they are left out.
	static const char* const kDebugOnly[] = {"g_god", "g_unlimitedammo", "ph_gravity", "log_missing_ini", "g_end_modif"};
	std::string out;
	for (size_t line = 0; line < text.size();)
	{
		size_t stop = text.find_first_of("\r\n", line);
		if (stop == std::string::npos)
			stop = text.size();
		bool own = false;
		for (const auto& kv : kOwn)
		{
			const size_t n = std::strlen(kv[0]);
			own = own || (text.compare(line, n, kv[0]) == 0 && (line + n >= text.size() || text[line + n] == ' '));
		}
		for (const char* key : kDebugOnly)
		{
			const size_t n = std::strlen(key);
			own = own || (text.compare(line, n, key) == 0 && (line + n >= text.size() || text[line + n] == ' '));
		}
		if (!own && stop > line)
			out.append(text, line, stop - line).append("\r\n");
		line = text.find_first_not_of("\r\n", stop);
		if (line == std::string::npos)
			break;
	}
	for (const auto& kv : kOwn)
		out.append(kv[0]).append(" ").append(kv[1]).append("\r\n");
	return out;
}

// What a map server is started with, after its exe.
inline std::string command_tail(const Args& a, uint32_t k, const std::string& fsltx)
{
	// -silent_error_mode: nobody sits in front of this process, a crash dialog of it would hold its link for good. No -nosound: the key
	// itself gives it a core with no device, and -nosound would leave its sounds without a file or a length (x_ray.cpp, psSoundSilent).
	return "-fsltx " + fsltx + " " + key_text(a) + " -silent_error_mode -coop_nick ms" + std::to_string(k);
}

// ---------------------------------------------------------------- who may connect as a map server

enum class HelloVerdict : uint8_t
{
	Accept,
	NotLoopback, // a map server stands on the host's PC: nobody else is one, whatever he knows
	BadToken, // no token of this session
};

inline HelloVerdict hello_verdict(bool loopback, bool token_issued)
{
	if (!loopback)
		return HelloVerdict::NotLoopback;
	return token_issued ? HelloVerdict::Accept : HelloVerdict::BadToken;
}

// "127.x.x.x:port": what Transport::address_of says of a connection that came over the loopback.
inline bool loopback_address(const std::string& ip_port) { return ip_port.compare(0, 4, "127.") == 0; }

// ---------------------------------------------------------------- which message goes where

// What the host's process does with a message a player sent: run its handler here, tunnel it to the map server of his level, or
// both (route(), coop_scope.h, is the decision; this adds the two lists of the things that go both ways). `on_host_level`: he
// stands on the level the host's process leads; `has_bridge`: the map server of his level is connected.
//   a world message       here; the ones that touch his body go to the authority of his level too;
//   a map message         there, and here only what the world keeps of him for the save and the HUD. While his map server is not
//                         connected there is nobody to act on it: it is dropped, as a stream is;
//   his repair            (StateDumpRequest, answered_by_his_map_server) there alone while his map server is connected, here
//                         alone before its bridge connects and after it drops: one answer. While his map server loads he is
//                         held on the loading screen and asks nothing, so the world's answer is not expected for him in play;
//                         the stamp of a weapon in his map server's answer reaches the world on its way up;
//   with split off, or on the host's level, everything as always.
struct Incoming
{
	bool here = true;
	bool tunnel = false;
};

// ONE process answers a player's repair. A StateDumpRequest of a player on a map server's level kept
// and answered by the world server (kept_from_every_level) and tunnelled to and answered by that map server (install_server,
// coop_entities.cpp: decides_world() || leads_level()). Each answer is an EntitySpawn of its own process (send_subtree ->
// write_spawn), and write_clone_initial stamps a magazined weapon there with that process's instance (clone_export keeps the first
// stamp it gave): the player got two spawns with two stamps. The one that matched his copy took him off the retry list
// (reconcile::Retry::done), the other failed its stage and put him back on it (Retry::add, a new 20 s), so the weapon was asked
// for again every 2 s with two answers each time; the item riders (jam, camo) keep a strict instance and were refused under the
// other stamp. While the bridge of his map server is up, that map server alone answers, because the facts he asks about are its:
//   - he compares his registry with ITS digest and roster (EntityHash reaches him from it only, level_passes; his EntityResync is
//     tunnelled only): only its copy closes the difference he asked about. Online on a map and the uses of what is online there
//     are no facts of the world (coop_entities.cpp mapless_row): the world's EntitySwitch and EntitySpawn of an object online at a
//     map server say "offline" (online_here: Mode::online_elsewhere), the map server's spawn is marked online at his level on its
//     way up (mark_spawn_of_your_level). The spawns follow the same choice: the world sends that level's players no copy
//     of a map server's spawn (host_spawn_from_leader);
//   - an object of another map and the world's own: the map server is a client of the world for them and repairs its copy against
//     it, so its answer carries the world's word one hop later, as the roster he compares with does.
// Without that bridge nobody else would answer, and the world's copy does, as before: only before the bridge connects and after it
// drops. While the map server loads, its players are held on the loading screen (the LoadWait barrier, docs/SPLIT_LEVELS.md, the
// life of a map server) and ask nothing; when it drops they load anew (host_rejoin_level). The world's answer is therefore not
// expected for a player in play (at most a repair asked in the moment between the drop and his new load). Only this request: his
// data the world keeps (vitals, equipment, items) still goes both ways, and what a map server asks its world
// (map_server_asks_the_world) never comes through here (coop_trunk.cpp host_incoming takes a bridge's messages first).
// The stamp: the world's Lua keeps the item data (coop_economy_core.script owns_world), and a weapon nobody had stamped
// before gets its stamp from the map server's answer. The world's copy takes it from that spawn on its way up (coop_trunk.cpp
// trunk_host_on_tunnel -> requests::adopt_spawn_stamp -> clone_take_stamp, the stamp alone), so a strict rider under it (jam, camo)
// is taken there too; a weapon the world had stamped otherwise keeps the world's stamp (the parts still pass by the alias,
// parts_alias). Not solved: a copy the map server's spawn never reached still holds the save's weapon (no stamp, its instance the
// name), and its strict riders and SE vars of that weapon are refused wherever the stamp is held, the world's copy now included, and
// it refuses theirs (tests/lua/test_srv_rider_stamp.lua, known limit).
inline bool answered_by_his_map_server(Msg type) { return type == Msg::StateDumpRequest; }

inline Incoming route_incoming(Msg type, bool split, bool on_host_level, bool has_bridge)
{
	Incoming out;
	if (!split || on_host_level)
		return out;
	const Scope scope = scope_of(type);
	if (route(scope, false) == Route::TunnelToServer)
	{
		out.tunnel = has_bridge;
		out.here = kept_from_every_level(type) && !(out.tunnel && answered_by_his_map_server(type));
	}
	else if (scope == Scope::World)
		out.tunnel = has_bridge && also_to_level_authority(type);
	return out;
}

// What the world server tells every map server beyond the registry of every level. A death:
// the stock death chain runs on every copy (to_bridges dropped it, so a map server never heard of a death on a level it does not
// lead); the roster's digest: a map server is a client of the world for every object it does not write, and repairs its copy
// against it. The players of another level do not get them (level_passes, coop_scope.h: the co-op's rows stay).
inline bool world_tells_map_servers(Msg type)
{
	switch (type)
	{
	case Msg::EntityDied:
	case Msg::EntityHash: return true;
	default: break;
	}
	return false;
}

// Does a broadcast of the world server reach a map server? It is a client of the world: what a player on another level gets, the
// stream of the host's level not among it, and world_tells_map_servers. Core messages are the session's own.
inline bool bridge_takes(Msg type)
{
	const Scope scope = scope_of(type);
	return scope == Scope::World || (scope == Scope::Map && (reaches_every_level(type) || world_tells_map_servers(type)));
}

// A map server asks its world for what it misses of the objects
// it does not write: an entity (StateDumpRequest: a squad it misses at a switch, a holder to refetch, client_ask_entity) and the
// rows of a bucket whose digest differs (EntityResync). The world server answers them itself (they are no tunnel's).
inline bool map_server_asks_the_world(Msg type)
{
	return type == Msg::StateDumpRequest || type == Msg::EntityResync;
}

// May a map server send this to the world server as a client of it (net::client_send)? Its actor is a ghost: what a player says of
// his body and his hands is nobody's here; what it asks of the world (money, story, the trunk, map_server_asks_the_world) goes.
inline bool map_server_sends(Msg type) { return scope_of(type) != Scope::Map || map_server_asks_the_world(type); }

// The lane a Tunnel carries, as its flags say it; and the other way.
inline uint8_t tunnel_lane_flags(Lane lane)
{
	return lane == Lane::Unreliable ? uint8_t(levels::wire::TunnelMsg::LaneUnreliable) :
	    lane == Lane::Bulk          ? uint8_t(levels::wire::TunnelMsg::LaneBulk) :
	                                  uint8_t(0);
}
inline Lane tunnel_lane(uint8_t flags)
{
	return (flags & levels::wire::TunnelMsg::LaneUnreliable) ? Lane::Unreliable :
	    (flags & levels::wire::TunnelMsg::LaneBulk)          ? Lane::Bulk :
	                                                           Lane::Reliable;
}

// ---------------------------------------------------------------- the players of a map server

// A player of the level, as the map server keeps him: he has a virtual peer from the moment LevelMap puts him on the level, his
// character comes online when he is in the world.
struct Visitor
{
	PeerId peer = kInvalidPeer;
	uint16_t proxy = levels::wire::kNoObject;
	bool in_world = false;
};

class VirtualPeers
{
public:
	struct Change
	{
		std::vector<Visitor> came; // a virtual peer for each, his world traffic waits
		std::vector<Visitor> in_world; // he finished loading
		std::vector<PeerId> left; // no player any more, or he went to another level
	};

	// The world server's table: what changed for level `level`. A row without a character says nothing yet; the host's own row is
	// no player of a map server (on its way down: the host came to its level).
	Change update(uint16_t level, const std::vector<levels::wire::LevelMapMsg::Player>& rows)
	{
		Change out;
		std::map<PeerId, Visitor> next;
		for (const levels::wire::LevelMapMsg::Player& r : rows)
		{
			if (r.level != level || r.proxy == levels::wire::kNoObject || r.peer == kHostPeer)
				continue;
			Visitor v;
			v.peer = r.peer;
			v.proxy = r.proxy;
			v.in_world = r.in_world != 0;
			next[v.peer] = v;
			const auto was = m_on.find(v.peer);
			if (was == m_on.end() || was->second.proxy != v.proxy)
			{
				if (was != m_on.end())
					out.left.push_back(v.peer); // another character under the same player: he starts over
				out.came.push_back(v);
				if (v.in_world)
					out.in_world.push_back(v);
			}
			else if (v.in_world && !was->second.in_world)
				out.in_world.push_back(v);
		}
		for (const auto& it : m_on)
			if (next.find(it.first) == next.end())
				out.left.push_back(it.first);
		m_on.swap(next);
		return out;
	}

	const std::map<PeerId, Visitor>& on() const { return m_on; }
	bool empty() const { return m_on.empty(); }
	void clear() { m_on.clear(); }

private:
	std::map<PeerId, Visitor> m_on;
};
} // namespace mapserver
} // namespace coop
