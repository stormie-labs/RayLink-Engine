// Anomaly Coop — the internal channel of the host's process (F3a T4, docs/plans/2026-10-24-f3a-split-levels.md §1.2).
//
// A listen server: the world server lives inside the host's game process, and the host player is a client of it over
// this link, with the same messages, the same handlers and the same checks as a remote client. Above the line is the
// co-op protocol (what a client sends the host as a message, the host player sends over the link); below it the objects of
// the process's one level and its stock Lua are shared, there is no second copy of the world in the process.
//
//  - The world server's Session sees the host player as peer 0 (kHostPeer) flagged local (Session::attach_local): its
//    send(0, ...) and broadcast(...) put a message into the link; what the host player pushes runs the server's handlers
//    with from = 0, after the same sender check and the same world gate as a remote client's message. Toward the host
//    player the world server is peer 0 as well (from = 0), as it is for a remote client.
//  - The host player's end: this object is his Endpoint (on: the client-side handlers of every module, install_client);
//    he sends with push_to_server (net::client_send in the host's process).
//  - Delivery is at the next pump, never inside a push: Session::update() pumps the link once per call (the game: once per
//    frame, Manager::frame). It takes what each direction had queued before the pump began and runs it, the host player's
//    messages first; what a handler pushes meanwhile waits for the next update, so a request and its answer move by one
//    hop per update and a handler can never re-enter another. One queue per direction: the order of the pushes is kept per
//    lane, and across the lanes as well.
//  - No ENet, no blobs, no rate limits: the host player does not download the world.
//  - The gate (the game: link_enabled) says which messages travel the link at all; the rest is refused at the push
//    (counted). The host keeps its direct paths ("if host — itself") until its module is moved onto the link (F3a T7), so
//    nothing may run twice: a message that is not moved never reaches either side.
//
// Pure C++17, header-only, no engine dependencies, single-threaded (the thread that calls Session::update).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "coop_protocol.h"
#include "coop_wire.h"

namespace coop
{
// The client side of the co-op protocol: where a process takes the world server's messages and runs its client-side
// handlers (each module's install_client). A remote client's endpoint is its Session (connected to the host); the host's
// process has the host player's end of its LocalLink.
class Endpoint
{
public:
	using Handler = std::function<void(PeerId from, Reader& payload)>;
	virtual ~Endpoint() = default;
	// Game message handlers (type >= Msg::GameBase): one per type, the last wins, an empty one removes it.
	virtual void on(Msg type, Handler h) = 0;
};

// The messages moved onto the link (F3a T7 adds them module by module, together with the removal of the host's direct
// path in that module). Empty: nothing travels the link yet, the host process behaves exactly as before the link.
inline constexpr std::array<Msg, 0> kLinkMessages{};

// The game's gate of the link (LocalLink::set_gate): the world server sends the host player only these messages over the
// link and takes only these from him.
inline bool link_enabled(Msg type)
{
	return std::find(kLinkMessages.begin(), kLinkMessages.end(), type) != kLinkMessages.end();
}

class LocalLink final : public Endpoint
{
public:
	// Which game messages the link takes; none set = every one (the library's tests). The game sets link_enabled.
	using Gate = bool (*)(Msg type);

	struct Stats
	{
		uint64_t to_server = 0; // pushed by the host player and taken
		uint64_t to_client = 0; // pushed by the world server and taken
		uint64_t refused = 0; // pushed either way but not taken: the gate refuses it, or it is no game message
	};

	LocalLink() = default;
	LocalLink(const LocalLink&) = delete;
	LocalLink& operator=(const LocalLink&) = delete;

	void set_gate(Gate gate) { m_gate = gate; }
	// True if a push of `type` is taken (either way).
	bool admits(Msg type) const
	{
		return static_cast<uint16_t>(type) >= static_cast<uint16_t>(Msg::GameBase) && (!m_gate || m_gate(type));
	}

	// ------------------------------------------------------------------ the host player's end

	// His client-side handlers (install_client of every module in the host's process). Run by the world server's
	// Session when it pumps the link, after its checks (Session::attach_local).
	void on(Msg type, Handler h) override
	{
		if (h)
			m_handlers[static_cast<uint16_t>(type)] = std::move(h);
		else
			m_handlers.erase(static_cast<uint16_t>(type));
	}
	void clear_handlers() { m_handlers.clear(); }
	// A copy of his handler of `type` (it may re-register itself while it runs), empty if none.
	Handler handler(Msg type) const
	{
		const auto it = m_handlers.find(static_cast<uint16_t>(type));
		return it == m_handlers.end() ? Handler() : it->second;
	}

	// The host player -> the world server. false: not taken (the gate, or no game message), counted in stats().refused.
	bool push_to_server(Msg type, Lane lane, const uint8_t* data, size_t size)
	{
		return push(m_to_server, m_stats.to_server, type, lane, data, size);
	}
	bool push_to_server(Msg type, Lane lane, const Writer& payload)
	{
		return push_to_server(type, lane, payload.ptr(), payload.size());
	}

	// ------------------------------------------------------------------ the world server's end (its Session)

	// The world server -> the host player (Session::send(0, ...), Session::broadcast). false: not taken.
	bool push_to_client(Msg type, Lane lane, const uint8_t* data, size_t size)
	{
		return push(m_to_client, m_stats.to_client, type, lane, data, size);
	}
	bool push_to_client(Msg type, Lane lane, const Writer& payload)
	{
		return push_to_client(type, lane, payload.ptr(), payload.size());
	}

	// Takes the messages queued toward the server before the call, oldest first, and calls fn(Msg type, Lane lane,
	// std::vector<uint8_t>& payload) for each (the payload may be moved out). What fn pushes meanwhile stays queued for
	// the next drain; a clear_queues() from inside fn ends the drain. Returns how many were taken.
	template <class Fn>
	size_t drain_server(Fn&& fn)
	{
		return drain(m_to_server, fn);
	}
	// The same toward the host player.
	template <class Fn>
	size_t drain_client(Fn&& fn)
	{
		return drain(m_to_client, fn);
	}

	size_t queued_to_server() const { return m_to_server.size(); }
	size_t queued_to_client() const { return m_to_client.size(); }
	// The session ended: what is in flight belongs to a session that is over. Handlers and the gate stay.
	void clear_queues()
	{
		m_to_server.clear();
		m_to_client.clear();
		++m_generation;
	}
	const Stats& stats() const { return m_stats; }

private:
	struct Packet
	{
		Msg type = Msg::GameBase;
		Lane lane = Lane::Reliable;
		std::vector<uint8_t> payload;
	};

	bool push(std::deque<Packet>& q, uint64_t& taken, Msg type, Lane lane, const uint8_t* data, size_t size)
	{
		if (!admits(type))
		{
			++m_stats.refused;
			return false;
		}
		Packet p;
		p.type = type;
		p.lane = lane;
		if (size)
			p.payload.assign(data, data + size);
		q.push_back(std::move(p));
		++taken;
		return true;
	}

	template <class Fn>
	size_t drain(std::deque<Packet>& q, Fn& fn)
	{
		const size_t due = q.size(); // what was queued before the drain began
		const uint64_t generation = m_generation;
		size_t taken = 0;
		while (taken < due && generation == m_generation && !q.empty())
		{
			Packet p = std::move(q.front());
			q.pop_front();
			++taken;
			fn(p.type, p.lane, p.payload);
		}
		return taken;
	}

	Gate m_gate = nullptr;
	std::map<uint16_t, Handler> m_handlers;
	std::deque<Packet> m_to_server;
	std::deque<Packet> m_to_client;
	uint64_t m_generation = 0; // clear_queues() count: a drain stops at a clear
	Stats m_stats;
};
} // namespace coop
