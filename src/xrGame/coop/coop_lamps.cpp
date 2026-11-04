// Anomaly Coop — hanging lamps across machines. See coop_lamps.h.
#include "stdafx.h"
#include "coop_lamps.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_sync.h"
#include "xrCoop/coop_relay_rules.h"

#include "Level.h"
#include "HangingLamp.h"

namespace coop
{
namespace lamps
{
namespace
{
using relay::LampEntry;

xrCriticalSection g_lock; // on_changed may run on the game thread
relay::LampBook g_book; // host, under g_lock
xr_map<u16, u8> g_dirty; // host, under g_lock: what the clients have not heard
u64 g_next_send = 0;
xr_set<PeerId> g_told; // host: the players in the world that have the book

struct Word
{
	u8 state = 0;
	u64 received = 0;
};
xr_map<u16, Word> g_words; // client: the host's word by local id; a lamp not here yet is waited for
u64 g_next_check = 0;

struct Stats
{
	u64 sent = 0, received = 0, applied = 0, malformed = 0, waited_out = 0;
} g_st;

u8 state_of(CHangingLamp* lamp)
{
	return u8((lamp->IsOn() ? relay::LampOn : 0) | (lamp->coop_dead() ? relay::LampDead : 0));
}

void flush(xr_vector<LampEntry>& batch, PeerId to)
{
	if (batch.empty())
		return;
	Writer w(batch.size() * 3 + 2);
	relay::write_lamps(w, batch.data(), batch.size());
	if (to == kInvalidPeer)
		net::host_broadcast(Msg::LampState, w);
	else
		net::host_send(to, Msg::LampState, w);
	g_st.sent += batch.size();
	batch.clear();
}

void add(xr_vector<LampEntry>& batch, u16 id, u8 state, PeerId to)
{
	LampEntry e;
	e.id = id;
	e.state = state;
	batch.push_back(e);
	if (batch.size() >= relay::kMaxLampsPerMsg)
		flush(batch, to);
}

void send_changes(u64 now)
{
	if (now < g_next_send)
		return;
	xr_map<u16, u8> dirty;
	{
		xrCriticalSectionGuard guard(&g_lock);
		if (g_dirty.empty())
			return;
		dirty.swap(g_dirty);
	}
	g_next_send = now + relay::kLampSendMs;
	xr_vector<LampEntry> batch;
	for (const auto& it : dirty)
		add(batch, it.first, it.second, kInvalidPeer);
	flush(batch, kInvalidPeer);
}

// A player who entered the world gets the book (host_broadcast queues the changes of his loading for him). Frames, not a
// hook of the join: the rule is only "in the world, not told yet".
void tell_new_players()
{
	xr_set<PeerId> in_world;
	for (const PeerInfo& p : mgr().session().peers())
	{
		if (p.id == kHostPeer || p.state != PeerState::Joined || sync::host_peer_joining(p.id))
			continue;
		in_world.insert(p.id);
		if (g_told.count(p.id))
			continue;
		xr_vector<LampEntry> batch;
		{
			xrCriticalSectionGuard guard(&g_lock);
			for (const auto& it : g_book.lamps())
				add(batch, it.first, it.second, p.id);
		}
		flush(batch, p.id);
	}
	g_told.swap(in_world);
}

// True when the lamp is here and answers to the word (a flicker keeps its own turn until it ends; a death does not wait).
bool apply(u16 id, const Word& word)
{
	CObject* o = g_pGameLevel ? Level().Objects.net_Find(id) : nullptr;
	CHangingLamp* lamp = o && !o->getDestroy() ? smart_cast<CHangingLamp*>(o) : nullptr;
	if (!lamp)
		return false;
	const bool dead = (word.state & relay::LampDead) != 0;
	const bool on = (word.state & relay::LampOn) != 0;
	if (dead ? !lamp->coop_dead() || lamp->IsOn() : !lamp->IsFlickering() && lamp->IsOn() != on)
	{
		lamp->coop_set(on, dead);
		++g_st.applied;
	}
	return true;
}

void client_on_lamps(PeerId, Reader& r)
{
	std::vector<LampEntry> list;
	if (!relay::read_lamps(r, list))
	{
		++g_st.malformed;
		return;
	}
	if (!is_mirror())
		return;
	const u64 now = mgr().now_ms();
	for (const LampEntry& e : list)
	{
		const u16 id = join::to_local(e.id);
		Word& word = g_words[id];
		word.state = e.state;
		word.received = now;
		apply(id, word);
		++g_st.received;
	}
}

void client_check(u64 now)
{
	if (now < g_next_check)
		return;
	g_next_check = now + relay::kLampCheckMs;
	for (auto it = g_words.begin(); it != g_words.end();)
	{
		if (!apply(it->first, it->second) && now - it->second.received > relay::kLampPendingMs)
		{
			it = g_words.erase(it);
			++g_st.waited_out;
		}
		else
			++it;
	}
}
} // namespace

void install_server(Session&) {} // nothing a client sends here

void install_client(Endpoint& e) { e.on(Msg::LampState, &client_on_lamps); }

void world_down()
{
	{
		xrCriticalSectionGuard guard(&g_lock);
		g_book.clear();
		g_dirty.clear();
	}
	g_told.clear();
	g_words.clear();
	g_next_send = g_next_check = 0;
}

void reset()
{
	world_down();
	g_st = Stats();
}

void frame()
{
	const u64 now = mgr().now_ms();
	if (is_map_authority())
	{
		tell_new_players();
		send_changes(now);
	}
	else if (is_mirror())
		client_check(now);
}

void print_status()
{
	u32 book = 0;
	{
		xrCriticalSectionGuard guard(&g_lock);
		book = u32(g_book.size());
	}
	if (is_map_authority())
		::Msg("[coop]   lamps: %u changed in this world, %llu entries sent", book, g_st.sent);
	else
		::Msg("[coop]   lamps: %llu words in, %llu applied, %u held, %llu malformed, %llu lamps never came", g_st.received,
		    g_st.applied, (u32)g_words.size(), g_st.malformed, g_st.waited_out);
}

void on_changed(CHangingLamp* lamp, bool soft)
{
	if (!lamp || !is_map_authority())
		return;
	const u8 state = state_of(lamp);
	xrCriticalSectionGuard guard(&g_lock);
	if (g_book.note(lamp->ID(), state, soft))
		g_dirty[lamp->ID()] = state;
}
} // namespace lamps
} // namespace coop
