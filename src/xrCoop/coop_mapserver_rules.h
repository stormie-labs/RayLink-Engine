// Anomaly Coop — the map servers, the pure part (docs/plans/2026-10-24-f3a-split-levels.md §1.1, §1.3, §1.8). The owner's decision of
// 2026-10-24: every map is computed on the host's PC. A level with players and without the host is led by a background process of
// the same exe, Mk; it connects to the host's process over the loopback, the players of its level are virtual peers on it, and a
// client talks to the host's process alone. This header holds what the engine only has to carry out:
//   - bridges       the peer ids of the map servers on the host's session: no player slots;
//   - Args          the key -coop_mapserver <level> <gvid> <port> <token>, both ways;
//   - fsgame        the fsgame_ms<k>.ltx of a map server: the host's, with an appdata_ms<k> of its own next to the host's appdata;
//   - hello_verdict who may connect as a map server;
//   - route_incoming, bridge_takes, map_server_sends: which message goes where;
//   - VirtualPeers  who stands on a map server's level, from the world server's LevelMap: who came, who is in, who left.
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop/coop_mapserver.cpp, coop/coop_trunk.cpp), by the session and
// by the unit-test exe.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_levels_wire.h"
#include "coop_protocol.h"
#include "coop_scope.h"

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

using Token = std::array<uint8_t, kHelloTokenSize>;

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

inline std::string token_hex(const Token& t)
{
	static const char* const digits = "0123456789abcdef";
	std::string out;
	out.reserve(t.size() * 2);
	for (const uint8_t b : t)
	{
		out.push_back(digits[b >> 4]);
		out.push_back(digits[b & 15]);
	}
	return out;
}

namespace detail
{
inline int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

// A whole number up to `max` that ends at a blank or at the end of the text. Moves `p` past it and the blanks after it.
inline bool read_number(const char*& p, uint32_t max, uint32_t& out)
{
	uint32_t v = 0;
	const char* q = p;
	while (*q >= '0' && *q <= '9')
	{
		v = v * 10 + uint32_t(*q - '0');
		if (v > max)
			return false;
		++q;
	}
	if (q == p || (*q && *q != ' '))
		return false;
	out = v;
	while (*q == ' ')
		++q;
	p = q;
	return true;
}
} // namespace detail

// Exactly 32 lowercase hex digits (the engine's command line is lowercase), ending at a blank or at the end of the text.
inline bool parse_token(const char* p, Token& out)
{
	Token t{};
	for (size_t i = 0; i < t.size(); ++i)
	{
		const int hi = detail::hex_value(p[i * 2]);
		const int lo = hi < 0 ? -1 : detail::hex_value(p[i * 2 + 1]);
		if (lo < 0)
			return false;
		t[i] = uint8_t(hi << 4 | lo);
	}
	const char end = p[t.size() * 2];
	if (end && end != ' ')
		return false;
	out = t;
	return true;
}

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
	if (!parse_token(p, a.token))
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
	static const char kFs[] = "-fsltx ";
	const char* p = params ? std::strstr(params, kFs) : nullptr;
	if (!p)
		return "fsgame.ltx";
	p += sizeof(kFs) - 1;
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
//   with split off, or on the host's level, everything as always.
struct Incoming
{
	bool here = true;
	bool tunnel = false;
};

inline Incoming route_incoming(Msg type, bool split, bool on_host_level, bool has_bridge)
{
	Incoming out;
	if (!split || on_host_level)
		return out;
	const Scope scope = scope_of(type);
	if (route(scope, false, false) == Route::TunnelToServer)
	{
		out.here = kept_from_every_level(type);
		out.tunnel = has_bridge;
	}
	else if (scope == Scope::World)
		out.tunnel = has_bridge && also_to_level_authority(type);
	return out;
}

// Does a broadcast of the world server reach a map server? It is a client of the world: what a player on another level gets, the
// stream of the host's level not among it. Core messages are the session's own.
inline bool bridge_takes(Msg type)
{
	const Scope scope = scope_of(type);
	return scope == Scope::World || (scope == Scope::Map && reaches_every_level(type));
}

// May a map server send this to the world server as a client of it (net::client_send)? Its actor is a ghost: what a player says of
// his body and his hands is nobody's here; what it asks of the world (money, story, the trunk) goes.
inline bool map_server_sends(Msg type) { return scope_of(type) != Scope::Map; }

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
