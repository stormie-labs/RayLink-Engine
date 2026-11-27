// coop::story, progress: each player's rank, reputation and community on the character that stands for him elsewhere.
#include "stdafx.h"
#include "coop_story_impl.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_progress.h"
#include "xrCoop/coop_mode.h"

#include "Level.h"
#include "Actor.h"
#include "InventoryOwner.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"

namespace coop
{
namespace story
{
// ------------------------------------------------------------------ progress (rank, reputation, community)

Progress g_own_sent; // last own progress sent

namespace
{
u64 g_progress_refreshed = 0;
} // namespace

u64 g_last_progress = 0;
xr_map<PeerId, Progress> g_reported; // host: last values reported by each client

// The proxy already carries the change: keep it when the reported values are put back, until the
// player's next report (which includes it) arrives.
void host_add_reported_reputation(PeerId peer, int delta)
{
	auto it = g_reported.find(peer);
	if (it != g_reported.end())
	{
		// He may have told any number ("log" stores it as told): the sum is made wide and kept in the int range
		const s64 sum = s64(it->second.reputation) + s64(delta);
		it->second.reputation = int(sum > s64(INT_MAX) ? INT_MAX : (sum < s64(INT_MIN) ? INT_MIN : sum));
	}
}

namespace
{
Progress own_progress()
{
	Progress p;
	if (!g_actor)
		return p;
	p.rank = g_actor->Rank();
	p.reputation = g_actor->Reputation();
	p.community = g_actor->Community();
	p.valid = true;
	return p;
}
} // namespace

void write_progress(Writer& w, u16 who, const Progress& p)
{
	w.u16(who);
	w.i32(p.rank);
	w.i32(p.reputation);
	w.i32(p.community);
}

// Puts a player's progress on the character that stands for that player here.
void apply_progress(u16 local_id, const Progress& p)
{
	if (local_id == 0 || local_id == kNoId || !in_game())
		return; // our own actor is ours
	CInventoryOwner* io = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(local_id));
	if (io)
	{
		if (io->Rank() != p.rank)
			io->SetRank(p.rank);
		if (io->Reputation() != p.reputation)
			io->SetReputation(p.reputation);
		if (community_ok(p.community) && io->Community() != p.community)
			io->SetCommunity(p.community);
		return;
	}
	CSE_ALifeTraderAbstract* tr = smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(local_id, true));
	if (!tr)
		return;
	tr->m_rank = p.rank;
	tr->m_reputation = p.reputation;
	if (community_ok(p.community))
		tr->m_community_index = p.community;
}

void progress_frame()
{
	PROF_EVENT("coop/coop_story/progress_frame");
	const u64 now = mgr().now_ms();
	if (now - g_last_progress < kProgressIntervalMs || !in_game())
		return;
	g_last_progress = now;
	const Progress mine = own_progress();
	if (mine.valid && (mine != g_own_sent || now - g_progress_refreshed >= kProgressRefreshMs))
	{
		g_progress_refreshed = now;
		Writer w(16);
		write_progress(w, host_id_of(0), mine);
		send_out(Msg::Progress, w);
		g_own_sent = mine;
		++g_stats.progress_out;
	}
	if (!mode().decides_world())
		return;
	// The host's own engine may change a proxy's rank/reputation (combat bookkeeping): the player's
	// machine owns them, so the reported values are put back. Slots of players who left are reused.
	for (auto it = g_reported.begin(); it != g_reported.end();)
	{
		const u16 proxy = join::proxy_of_peer(it->first);
		if (proxy == kNoId)
		{
			it = g_reported.erase(it);
			continue;
		}
		progress::host_check(it->first, proxy, it->second.rank, it->second.reputation, it->second.community);
		apply_progress(proxy, it->second);
		++it;
	}
}

void send_progress_all(PeerId to)
{
	const Progress mine = own_progress();
	if (mine.valid)
	{
		Writer w(16);
		write_progress(w, 0, mine);
		net::host_send(to, Msg::Progress, w);
	}
	for (const auto& it : g_reported)
	{
		const u16 proxy = join::proxy_of_peer(it.first);
		if (it.first == to || proxy == kNoId)
			continue;
		Writer w(16);
		write_progress(w, proxy, it.second);
		net::host_send(to, Msg::Progress, w);
	}
	++g_stats.progress_out;
}
} // namespace story

namespace progress
{
// coop_progress.h: the keeper puts a fight's reputation into the base of the player's stored report (his next Progress carries it,
// so the check sees no rise beyond his budget), then tells his machine. The co-op host gets here from host_flush_rep_deltas, the
// world server through coop.progress.apply_verified_fight.
bool host_apply_fight(PeerId peer, u16 character, int delta)
{
	if (peer == kInvalidPeer || peer == kHostPeer || !delta || character == kNoId)
		return false;
	story::host_add_reported_reputation(peer, delta);
	Writer w(16);
	w.u8(story::RelReputationDelta);
	w.u16(character);
	w.i32(delta);
	net::host_send(peer, Msg::Relation, w);
	return true;
}
} // namespace progress
} // namespace coop
