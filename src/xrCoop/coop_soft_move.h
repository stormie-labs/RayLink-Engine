// Anomaly Coop — split levels: the host changes level and nobody else notices (docs/SPLIT_LEVELS.md §7a "Переход хоста").
// A player on a level of a map server keeps playing while the host's process writes its save and reloads: his world is the map
// server's, and the host's process is only the road to it. Three things stand between that and the old "everybody reloads":
//   stays, keeps_server   who stays in his world and which map server keeps its process (the rest go the old way);
//   Window                the host's side of the move: the go, the save (T0), the new world, the end of the parking (T1);
//   Relay                 the road while the main thread does not run: what the transport's keep-alive thread does with a packet by
//                         itself. It is a table made once at the go and only read after that, and pure, so that thread needs nothing
//                         of the session.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "coop_levels_wire.h"
#include "coop_mapserver_rules.h"
#include "coop_protocol.h"
#include "coop_scope.h"
#include "coop_wire.h"

namespace coop
{
namespace softmove
{
namespace wire = levels::wire;

// Frames of the old world this long after its save mean the reload is not coming (the level change wrote its save and the stock code
// kept the level all the same): the parking comes off, the world goes on.
constexpr uint64_t kReloadWatchMs = 10000;

// ---------------------------------------------------------------- who stays

// A player at the host's go, as the world server sees him.
struct Facts
{
	bool joined = false; // in the session
	bool known = false; // a join of his reached this world
	bool proxy = false; // his character is known
	bool loading = false; // on his loading screen: he has no world to stay in
	bool joining = false; // the world's traffic waits for him
	bool choosing = false; // in the choice of a character
	bool server_ready = false; // the map server of his level leads it and is connected; there is none on the host's old level
	bool on_target = false; // he stands on the level the host goes to: its map server closes, the host's process leads it from now on
};

inline bool stays(const Facts& f)
{
	return f.joined && f.known && f.proxy && !f.loading && !f.joining && !f.choosing && f.server_ready && !f.on_target;
}

// `soft_on`: coop_soft_host_move; `ready`: it leads its level now (Ready, Serving). A server that is still loading has nothing to keep.
constexpr bool keeps_server(bool soft_on, bool ready, uint16_t level, uint16_t target)
{
	return soft_on && ready && level != target;
}

// ---------------------------------------------------------------- the window

enum class Stage : uint8_t
{
	Off,
	Going, // parked, the relay runs; the level change writes its save
	Saved, // T0: the world's clock stands from here on, the reload comes within frames
	World, // the new world exists: what the host's process sends the parked now waits, it is of the world they will live in
};

// The host's side of one move. All times are the session's clock.
class Window
{
public:
	Stage stage() const { return m_stage; }
	bool active() const { return m_stage != Stage::Off; }
	uint16_t target() const { return m_target; }

	void go(uint16_t target, uint64_t now)
	{
		*this = Window();
		m_stage = Stage::Going;
		m_target = target;
		m_go_at = now;
	}
	void saved(uint64_t now, uint64_t clock = 0)
	{
		if (m_stage != Stage::Going)
			return;
		m_stage = Stage::Saved;
		m_saved_at = now;
		m_saved_clock = clock;
	}
	void world_created(uint64_t now)
	{
		if (m_stage != Stage::Saved)
			return;
		m_stage = Stage::World;
		m_created_at = now;
	}
	void world_up(uint64_t now)
	{
		if (m_stage == Stage::World && !m_up_at)
			m_up_at = now;
	}

	// The reload did not follow its save. A level being built runs frames too, with no world up: those say nothing.
	bool overdue(uint64_t now, bool old_world_up) const
	{
		return m_stage == Stage::Saved && old_world_up && now >= m_saved_at + kReloadWatchMs;
	}

	uint64_t go_at() const { return m_go_at; }
	uint64_t saved_at() const { return m_saved_at; }
	uint64_t up_at() const { return m_up_at; }
	uint64_t saved_clock() const { return m_saved_clock; }
	uint64_t save_ms() const { return m_saved_at >= m_go_at ? m_saved_at - m_go_at : 0; }
	// How long the world was away: from the save to now.
	uint64_t away_ms(uint64_t now) const { return now >= m_saved_at ? now - m_saved_at : 0; }

	void clear() { *this = Window(); }

private:
	Stage m_stage = Stage::Off;
	uint16_t m_target = wire::kNoLevel;
	uint64_t m_go_at = 0;
	uint64_t m_saved_at = 0;
	uint64_t m_saved_clock = 0;
	uint64_t m_created_at = 0;
	uint64_t m_up_at = 0;
};

// Game milliseconds a window of `real_ms` is worth.
inline uint64_t game_ms_ahead(uint64_t real_ms, float factor)
{
	return factor > 0.f ? uint64_t(double(real_ms) * double(factor)) : 0u;
}

// What the clock of the new world still has to go, or the first time sync sends everybody on the map servers back to the save's minute.
// That clock has run since its load (CALifeTimeManager::load), so the step is to where the old clock would stand, never by the whole window.
// `saved_clock` 0: no clock was read at the save.
inline uint64_t clock_behind(uint64_t saved_clock, uint64_t real_ms, float factor, uint64_t clock_now)
{
	if (!saved_clock)
		return 0;
	const uint64_t target = saved_clock + game_ms_ahead(real_ms, factor);
	return target > clock_now ? target - clock_now : 0;
}

// The transfer book stands still in the window and until what the map servers said in it has been read: a grant answered there has its
// commit among that, and its time limit must not run out before it is.
inline bool book_waits(bool window, bool replaying) { return window || replaying; }

// ---------------------------------------------------------------- the relay

// What became of a packet.
enum class Take : uint8_t
{
	Keep, // not the relay's: it waits for the main thread as ever
	Sent, // it went on, that is all there was to it
	SentAndKept, // it went on, and the world keeps it as well: the main thread takes it later and must not tunnel it again
	Dropped, // for nobody: a player who is not on that level, a map server that is not there
};

// The table of the road: the connections as the transport knows them, a handle and its ENet connectID, so that a slot somebody else took
// since is nobody's here. Made on the main thread before the relay starts, after that it is only read: any thread.
class Relay
{
public:
	struct Link
	{
		const void* handle = nullptr;
		uint32_t link = 0;
		uint32_t id = 0; // a session slot, a bridge's peer id
		uint16_t level = wire::kNoLevel;
	};
	struct Out
	{
		const void* to = nullptr;
		uint32_t link = 0;
		Lane lane = Lane::Reliable;
		uint16_t type = 0;
		std::vector<uint8_t> body; // without the type
	};

	void add_client(const void* handle, uint32_t link, uint32_t peer, uint16_t level) { m_clients.push_back(Link{handle, link, peer, level}); }
	void add_bridge(const void* handle, uint32_t link, uint32_t bridge, uint16_t level) { m_bridges.push_back(Link{handle, link, bridge, level}); }
	const std::vector<Link>& clients() const { return m_clients; }
	const std::vector<Link>& bridges() const { return m_bridges; }

	// A packet (its 2-byte type first) from the connection `from`, `link`, on `channel`; `out` gets what is to be sent, in order.
	Take on_packet(const void* from, uint32_t link, uint8_t channel, const uint8_t* packet, size_t size, std::vector<Out>& out) const
	{
		if (size < 2)
			return Take::Keep;
		const uint16_t type = uint16_t(packet[0] | (uint16_t(packet[1]) << 8));
		const Lane lane = channel < uint8_t(Lane::Count) ? Lane(channel) : Lane::Reliable;
		if (const Link* c = find(m_clients, from, link))
			return from_client(*c, lane, type, packet, size, out);
		if (const Link* b = find(m_bridges, from, link))
			return from_bridge(*b, type, packet, size, out);
		return Take::Keep;
	}

private:
	static const Link* find(const std::vector<Link>& rows, const void* handle, uint32_t link)
	{
		for (const Link& l : rows)
			if (l.handle == handle && l.link == link)
				return &l;
		return nullptr;
	}

	const Link* bridge_of(uint16_t level) const
	{
		for (const Link& l : m_bridges)
			if (l.level == level)
				return &l;
		return nullptr;
	}

	// As mapserver::host_incoming: the same route on the same words. What a client may not send at all is the session's to kick him for.
	Take from_client(const Link& c, Lane lane, uint16_t type, const uint8_t* packet, size_t size, std::vector<Out>& out) const
	{
		if (type < uint16_t(Msg::GameBase))
			return Take::Keep;
		const Msg msg = static_cast<Msg>(type);
		const MsgSender sender = msg_sender(msg);
		if (sender == MsgSender::HostOnly || sender == MsgSender::TrunkOnly || msg == Msg::Tunnel)
			return Take::Keep;
		const Link* bridge = bridge_of(c.level);
		const mapserver::Incoming go = mapserver::route_incoming(msg, true, false, bridge != nullptr);
		if (!go.tunnel)
			return go.here ? Take::Keep : Take::Dropped;
		wire::TunnelMsg m;
		m.peer = c.id;
		m.flags = mapserver::tunnel_lane_flags(lane);
		m.packet.assign(packet, packet + size);
		Writer w(size + 8);
		if (!m.write(w))
			return go.here ? Take::Keep : Take::Dropped;
		out.push_back(Out{bridge->handle, bridge->link, lane, uint16_t(Msg::Tunnel), w.data()});
		return go.here ? Take::SentAndKept : Take::Sent;
	}

	// As mapserver::trunk_host_on_tunnel.
	Take from_bridge(const Link& b, uint16_t type, const uint8_t* packet, size_t size, std::vector<Out>& out) const
	{
		if (type != uint16_t(Msg::Tunnel))
			return Take::Keep;
		Reader r(packet + 2, size - 2);
		wire::TunnelMsg m;
		if (!m.read(r))
			return Take::Dropped;
		const Lane lane = mapserver::tunnel_lane(m.flags);
		const uint16_t inner = uint16_t(m.type());
		const std::vector<uint8_t> body(m.packet.begin() + 2, m.packet.end());
		bool sent = false;
		for (const Link& c : m_clients)
		{
			if (c.level != b.level || (!(m.flags & wire::TunnelMsg::AllOnMap) && c.id != m.peer))
				continue;
			out.push_back(Out{c.handle, c.link, lane, inner, body});
			sent = true;
		}
		return sent ? Take::Sent : Take::Dropped;
	}

	std::vector<Link> m_clients;
	std::vector<Link> m_bridges;
};
} // namespace softmove
} // namespace coop
