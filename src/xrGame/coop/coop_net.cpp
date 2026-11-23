#include "stdafx.h"
#include "coop_net.h"
#include "coop_manager.h"
#include "coop_sync.h"
#include "xrCoop/coop_mode.h"

namespace coop
{
namespace net
{
bool live() { return (mgr().active() && mode().decides_world()) || mgr().is_live_client(); }
bool host() { return mgr().active() && (mode().decides_world() || mode().leads_level()); }
bool client() { return client(mgr().active(), mode()); }

namespace
{
struct Queued
{
	Msg type;
	Lane lane;
	std::vector<u8> payload;
};
xr_map<PeerId, xr_vector<Queued>> g_queues;
u64 g_queued_total = 0;
constexpr size_t kMaxQueued = 50000; // a peer that never finishes loading must not eat the host's memory
xr_set<PeerId> g_overflow;

void queue(PeerId to, Msg type, const Writer& payload, Lane lane)
{
	if (lane == Lane::Unreliable)
		return; // snapshots are worthless later; the next one will do
	Queued q;
	q.type = type;
	q.lane = lane;
	q.payload.assign(payload.data().begin(), payload.data().end());
	xr_vector<Queued>& queue_of_peer = g_queues[to];
	if (queue_of_peer.size() >= kMaxQueued)
	{
		g_overflow.insert(to);
		return;
	}
	queue_of_peer.push_back(std::move(q));
	++g_queued_total;
}
} // namespace

// S4 (docs/MODES.md section 4, P-B): who a world change reaches is the mode's (xrCoop/coop_mode.h): the co-op host's session and its
// LocalLink, the world server's players and bridges, a map server's players. The callers do not change.
void host_broadcast(Msg type, const Writer& payload, Lane lane, PeerId except) { mode().deliver(type, payload, lane, except); }

void host_stream(Msg type, const Writer& payload, PeerId except) { mode().stream(type, payload, except); }

void host_send(PeerId to, Msg type, const Writer& payload, Lane lane)
{
	if (!mode().decides_world() && !mode().leads_level())
		return; // the world's or a map's decider only (the old is_map_authority)
	to_peer(to, type, payload, lane);
}

void to_peer(PeerId to, Msg type, const Writer& payload, Lane lane)
{
	if (sync::host_peer_joining(to))
		queue(to, type, payload, lane);
	else
		mgr().session().send(to, lane, type, payload);
}

void client_send(Msg type, const Writer& payload, Lane lane) { mode().up(type, payload, lane); }

void host_flush(PeerId to)
{
	auto it = g_queues.find(to);
	if (it == g_queues.end())
		return;
	Session& s = mgr().session();
	for (const Queued& q : it->second)
	{
		Writer w(q.payload.size());
		if (!q.payload.empty())
			w.bytes(q.payload.data(), q.payload.size());
		s.send(to, q.lane, q.type, w);
	}
	::Msg("[coop] flushed %u queued world change(s) to #%u", (u32)it->second.size(), to);
	g_queues.erase(it);
}

void host_drop(PeerId to)
{
	g_queues.erase(to);
	g_overflow.erase(to);
}

bool host_queue_overflow(PeerId to) { return g_overflow.count(to) != 0; }

void print_status()
{
	::Msg("[coop]   join queues: %llu changes queued in all, at most %u per peer", g_queued_total, u32(kMaxQueued));
	for (const auto& it : g_queues)
		::Msg("[coop]     #%u loads: %u queued%s", it.first, u32(it.second.size()), g_overflow.count(it.first) ? ", overflowed" : "");
}

void reset()
{
	g_queues.clear();
	g_overflow.clear();
	g_queued_total = 0;
}
} // namespace net
} // namespace coop
