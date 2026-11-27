#include "stdafx.h"
#include "coop_entities.h"
#include "coop_entities_impl.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_requests.h"
#include "xrCoop/coop_limits.h"
#include "coop_events.h"
#include "coop_items.h"
#include "coop_writer.h"

#include "Level.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrEngine/XR_IOConsole.h"
#include "../xrEngine/xr_ioc_cmd.h"

namespace coop
{
namespace entities
{
// ---------------------------------------------------------------- the roster (xrCoop/coop_reconcile.h)

reconcile::Retry g_id_retry; // client, D3: an EntitySpawn that found its id taken here
namespace
{
int g_coop_reconcile = 1;
struct RosterAsk
{
	PeerId peer = kInvalidPeer;
	u8 bucket = 0;
};
xr_vector<RosterAsk> g_rc_asked; // host: rosters to send, after the frame's events
u8 g_rc_bucket = 0; // host: the next digest
u64 g_rc_last_step = 0, g_rc_last_roster = 0;
reconcile::Watch g_rc_watch; // client
reconcile::Queue g_rc_repairs; // client: asked for at the rate of StateDumpRequest
limits::Pacer g_rc_pacer;
u32 g_rc_log = 0;
struct RosterStats
{
	u64 digests = 0, rosters = 0, rows = 0, repairs_in = 0, repairs_gone = 0; // host
	u64 compared = 0, differed = 0, rosters_asked = 0, rosters_in = 0, repairs_asked = 0, retries = 0; // client
	u64 kinds[7] = {}; // client: repairs asked by kind (bit of reconcile::Repair)
	u64 traits_applied = 0, squad_members_out = 0, squad_members_in = 0, squad_waits = 0;
} g_rc;

// Client: asks the host about one entity (host numbering).
void client_ask_entity(u16 id_host, u8 what)
{
	Writer w(4);
	w.u16(id_host);
	w.u8(what);
	net::client_send(Msg::StateDumpRequest, w);
}

// As this machine sees it. An online creature's server entity lags behind its game object (a client's puppet is told of
// its death, not its entity); the rest of the world keeps it in the entity.
bool row_alive(CSE_ALifeDynamicObject* e)
{
	CSE_ALifeMonsterAbstract* m = e->cast_monster_abstract();
	if (!m)
		return true;
	if (online_here(e) && g_pGameLevel)
		if (CEntityAlive* a = smart_cast<CEntityAlive*>(Level().Objects.net_Find(e->ID)))
			return !!a->g_Alive();
	return m->g_Alive();
}

// True if a living creature or a player holds `e` in the end: such things change with every shot and hit, and their
// state goes out when they are opened, taken or their holder dies. `root_online`: the top holder is online here.
bool held_by_the_living(CSE_ALifeDynamicObject* e, bool& root_online)
{
	CSE_ALifeDynamicObject* root = e;
	for (int depth = 0; root->ID_Parent != kNoId && depth < 8; ++depth)
	{
		CSE_ALifeDynamicObject* holder = ai().alife().objects().object(root->ID_Parent, true);
		if (!holder)
			break;
		root = holder;
	}
	root_online = online_here(root);
	if (root == e)
		return false;
	if (root->ID == 0 || world::is_player_character(root))
		return true;
	if (root->cast_monster_abstract())
		return row_alive(root);
	return root->cast_trader_abstract() != nullptr; // a trader of its own class never dies
}

// The row of an entity, ids in host numbering. False: it is not on the roster: an object the game does not save (a
// bolt, a body ALife is about to remove) is in no join save either, and a client that came later never had it.
bool row_of(CSE_ALifeDynamicObject* e, reconcile::Row& r)
{
	if (!e->can_save() || e->redundant())
		return false;
	r = reconcile::Row();
	r.id = join::to_host(e->ID);
	if (r.id == 0 || e->ID == 0 || world::is_player_character(e))
	{
		r.online = true; // a player's character: its section, its place and its life are not the same at both ends
		return true;
	}
	r.parent = e->ID_Parent == kNoId ? kNoId : join::to_host(e->ID_Parent);
	CSE_ALifeOnlineOfflineGroup* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e);
	// A squad from the wire stays offline here and its members switch one by one (apply_group_switch): its vote tells.
	if (r.parent == kNoId)
		r.online = group && is_mirror() ? g_online.count(e->ID) != 0 : online_here(e);
	r.alive = row_alive(e);
	r.kind = reconcile::kind_of(e->s_name.c_str());
	if (group)
	{
		std::vector<u16> members;
		for (const auto& m : group->squad_members())
			members.push_back(join::to_host(m.first));
		r.state = reconcile::members_digest(members);
	}
	else if (CSE_ALifeTraderAbstract* t = e->cast_trader_abstract())
		r.state = reconcile::traits_digest(s32(t->m_community_index), s32(t->m_rank), s32(t->m_reputation));
	else if (e->cast_inventory_item())
	{
		bool root_online = false;
		if (!held_by_the_living(e, root_online))
			r.state = items::state_digest(e, root_online);
	}
	return true;
}

// R32 W5.1 (verify-D finding 3): the row of `e` as a copy that leads a map of its own sees it when it compares its world (a map
// server): no online flag (reconcile::mapless) and an item's state without the uses, which row_of reads only from a game object here
// (state_digest with_uses = its top holder online here). Online on a map and the uses of what is online there are no facts its world
// has: with them every bucket that held anything online on that map differed for ever.
reconcile::Row mapless_row(CSE_ALifeDynamicObject* e, const reconcile::Row& here)
{
	reconcile::Row r = reconcile::mapless(here);
	bool root_online = false;
	if (r.state && e->cast_inventory_item() && !held_by_the_living(e, root_online) && root_online)
		r.state = items::state_digest(e, false);
	return r;
}

// The rows of one bucket; `mapless` (when given) gets the same rows as a copy that leads a map of its own compares them (mapless_row).
void collect_rows(u8 bucket, std::vector<reconcile::Row>& rows, std::vector<reconcile::Row>* mapless = nullptr)
{
	rows.clear();
	if (mapless)
		mapless->clear();
	for (u32 id = bucket; id < kNoId; id += xfer::kBuckets)
	{
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(u16(id)), true);
		reconcile::Row r;
		if (!e || !row_of(e, r))
			continue;
		rows.push_back(r);
		if (mapless)
			mapless->push_back(mapless_row(e, r));
	}
}

// R32 W5.1 (verify-D finding 3): a copy with a map of its own (Mode::leads_level: a map server, a client of its world) decides what is
// online there; it compares its world's digest and roster mapless. A mirror shares its host's map: every fact counts, as before.
bool compares_mapless() { return mode().leads_level(); }

bool peer_is_in_world(const PeerInfo& p) { return p.state == PeerState::Joined && !sync::host_peer_joining(p.id); }

void host_send_roster(const RosterAsk& ask)
{
	std::vector<reconcile::Row> rows;
	collect_rows(ask.bucket, rows);
	Writer w(rows.size() * reconcile::kRowBytes + 8);
	reconcile::write_roster(w, ask.bucket, rows);
	net::host_send(ask.peer, Msg::EntityRoster, w);
	++g_rc.rosters;
	g_rc.rows += rows.size();
}

void host_reconcile()
{
	if (!world_up() || !g_coop_reconcile)
		return;
	const u64 now = mgr().now_ms();
	if (!g_rc_asked.empty() && now - g_rc_last_roster >= reconcile::kRosterGapMs)
	{
		g_rc_last_roster = now;
		host_send_roster(g_rc_asked.front());
		g_rc_asked.erase(g_rc_asked.begin());
	}
	if (now - g_rc_last_step < reconcile::kStepMs)
		return;
	g_rc_last_step = now;
	const auto& peers = mgr().session().peers();
	if (std::none_of(peers.begin(), peers.end(), peer_is_in_world))
		return; // nobody to compare with: no walk at all
	const u8 bucket = g_rc_bucket;
	g_rc_bucket = u8((g_rc_bucket + 1) % xfer::kBuckets);
	std::vector<reconcile::Row> rows, mapless;
	collect_rows(bucket, rows, &mapless);
	Writer w(8);
	reconcile::write(w, reconcile::hash_bucket(bucket, rows));
	for (const PeerInfo& p : peers)
		if (peer_is_in_world(p))
			if (mode().reaches(p.id, Msg::EntityHash)) // F3a multilevel: a digest of the host's level's stream
				net::host_send(p.id, Msg::EntityHash, w);
	// R32 W5.1: and to the mode's links, as StoryHash goes: a map server is a client of its world (bridge_takes(EntityHash)) and leads
	// a map of its own, so its digest is the mapless one it compares (verify-D finding 3). The co-op host player's link does not take
	// it (no link message: he is in the host's world itself), a map server has no link.
	Writer links(8);
	reconcile::write(links, reconcile::hash_bucket(bucket, mapless));
	mode().deliver_links(Msg::EntityHash, links, Lane::Reliable);
	++g_rc.digests;
}

} // namespace

void host_on_resync(PeerId from, Reader& r)
{
	const u8 bucket = r.u8();
	if (!(mode().decides_world() || mode().leads_level()) || !r.ok() || bucket >= xfer::kBuckets)
		return;
	for (const RosterAsk& ask : g_rc_asked)
		if (ask.peer == from && ask.bucket == bucket)
			return;
	g_rc_asked.push_back({from, bucket});
}

void client_on_hash(PeerId, Reader& r)
{
	reconcile::BucketHash host;
	if (!reconcile::read(r, host) || !g_coop_reconcile || !world_up() || Device.dwPrecacheFrame)
		return;
	std::vector<reconcile::Row> rows, mapless;
	const bool own_map = compares_mapless();
	collect_rows(host.bucket, rows, own_map ? &mapless : nullptr);
	const bool same = reconcile::hash_bucket(host.bucket, own_map ? mapless : rows) == host;
	++g_rc.compared;
	g_rc.differed += same ? 0 : 1;
	if (g_rc_watch.on_hash(host.bucket, same, mgr().now_ms()))
	{
		Writer w(2);
		w.u8(host.bucket);
		net::client_send(Msg::EntityResync, w);
		++g_rc.rosters_asked;
	}
}

void client_on_roster(PeerId, Reader& r)
{
	u8 bucket = 0;
	std::vector<reconcile::Row> host, local, mapless;
	if (!reconcile::read_roster(r, bucket, host) || !g_coop_reconcile || !world_up())
		return;
	++g_rc.rosters_in;
	// R32 W5.1 (verify-D finding 3): a copy with a map of its own compares its world's rows as its digest did: mapless at both ends
	const bool own_map = compares_mapless();
	collect_rows(bucket, local, own_map ? &mapless : nullptr);
	if (own_map)
		for (reconcile::Row& h : host)
			h = reconcile::mapless(h);
	const u64 now = mgr().now_ms();
	for (const reconcile::Action& a : reconcile::diff(host, own_map ? mapless : local))
	{
		const u16 id = join::to_local(a.id);
		if (writing::writes(id))
			continue; // R32 W5.1: the writer is the truth of its object (a map server's own); a mirror writes nothing, repairs all
		if ((a.what & (reconcile::Gone | reconcile::Replace)) && requests::client_lease_waiting(id))
			continue; // our own create waits for the host
		if ((a.what & reconcile::Parent) && events::client_parent_pending(id))
			continue; // on its way
		if (!g_rc_watch.allow(a.id, now))
			continue;
		const CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
		// On the host's roster and here, but off ours: a squad emptied here is "redundant". Its spawn would change nothing.
		const u8 what = (a.what & reconcile::Spawn) && e ? u8(reconcile::State | reconcile::Switch) : a.what;
		g_rc_repairs.push(a.id, what);
		if (log_allowed(g_rc_log))
			::Msg("[coop] roster: %u [%s] differs from the host: repair 0x%02x", a.id, e ? e->s_name.c_str() : "not here",
			    u32(what));
	}
}

namespace
{
void client_reconcile()
{
	if (!g_coop_reconcile || !world_up())
		return;
	const u64 now = mgr().now_ms();
	std::vector<u16> due;
	g_id_retry.due(now, due);
	for (u16 id : due)
	{
		client_ask_entity(id, reconcile::kAskMissing);
		++g_rc.retries;
	}
	while (g_rc_repairs.size() && g_rc_pacer.take(Msg::StateDumpRequest, now))
	{
		reconcile::Action a;
		g_rc_repairs.pop(a);
		client_ask_entity(a.id, a.what);
		++g_rc.repairs_asked;
		for (u32 bit = 0; bit < 7; ++bit)
			g_rc.kinds[bit] += (a.what >> bit) & 1;
	}
}
} // namespace

// F2: an NPC's community, rank and reputation changed by the host's Lua (stock scripts raise them with every kill).
void client_on_traits(PeerId, Reader& r)
{
	reconcile::TraitsMsg m;
	if (!reconcile::read(r, m) || !world_up())
		return;
	const u16 id = join::to_local(m.object);
	CSE_ALifeDynamicObject* e = id ? ai().alife().objects().object(id, true) : nullptr;
	CSE_ALifeTraderAbstract* t = e ? e->cast_trader_abstract() : nullptr;
	if (!t || world::is_player_character(e))
		return;
	ClientApply scope;
	CGameObject* go = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	if (CInventoryOwner* owner = go && !go->getDestroy() ? go->cast_inventory_owner() : nullptr)
	{
		owner->SetCommunity(m.community);
		owner->SetRank(m.rank);
		owner->SetReputation(m.reputation);
	}
	t->m_community_index = m.community;
	t->m_rank = m.rank;
	t->m_reputation = m.reputation;
	++g_rc.traits_applied;
}

// F5: a squad's members changed after the join save. A squad from the wire only lists ids; one the engine bound holds
// its members for real, and takes a new one the way the host did. A member that cannot be taken yet (not here, or online
// on one side only: the roster repairs that first) waits for the next round.
void client_on_squad(PeerId, Reader& r)
{
	reconcile::SquadMsg m;
	if (!reconcile::read(r, m) || !world_up())
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(m.squad), true);
	CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e);
	if (!g)
		return;
	ClientApply scope;
	xr_set<u16> wanted;
	for (u16 id : m.members)
		wanted.insert(join::to_local(id));
	// Before the list changes. An empty squad bound nobody: only ids, as a squad from the wire.
	const bool bound = !g->squad_members().empty() && group_bound(*g);
	xr_vector<u16> leaving;
	for (const auto& member : g->squad_members())
		if (!wanted.count(member.first))
			leaving.push_back(member.first);
	for (u16 id : leaving)
	{
		g->unregister_member(id);
		++g_rc.squad_members_out;
	}
	const u64 now = mgr().now_ms();
	for (u16 id : wanted)
	{
		if (g->squad_members().find(id) != g->squad_members().end())
			continue;
		CSE_ALifeMonsterAbstract* c = smart_cast<CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(id, true));
		if (!c)
		{
			// R32 W5: a listed member that is not here (or another object holds its id) is asked for, as a repair of the roster: the
			// answer makes it, or replaces what holds its id (client_on_spawn), and the next EntitySquad takes it in
			const u16 member_host = join::to_host(id);
			if (g_rc_watch.allow(member_host, now))
				g_rc_repairs.push(member_host, reconcile::kAskMissing);
			++g_rc.squad_waits;
			continue;
		}
		if (!c->g_Alive() || c->ID_Parent != kNoId || (bound && c->m_bOnline != g->m_bOnline))
		{
			++g_rc.squad_waits;
			continue;
		}
		leave_group(c);
		if (bound)
			g->register_member(id);
		else
			const_cast<CSE_ALifeOnlineOfflineGroup::MEMBERS&>(g->squad_members())[id] = nullptr; // as its STATE_Read
		++g_rc.squad_members_in;
	}
}

// Host: a client asks about an entity. Without `what`: one of its handlers misses it; with it: a repair of the roster
// (xrCoop/coop_reconcile.h). The answers are the host's usual world messages to that client alone, and each of them
// changes nothing where it is already true.
void host_on_entity_request(PeerId from, Reader& r)
{
	const u16 id = r.u16();
	if (!r.ok() || !ai().get_alife()) // review R2 C2: no world between levels / in the main menu
		return;
	u8 what = r.remaining() ? r.u8() : reconcile::kAskMissing;
	++g_rc.repairs_in;
	const auto send_destroy = [&]()
	{
		Writer w(4);
		w.u16(id);
		net::host_send(from, Msg::EntityDestroy, w);
		++g_rc.repairs_gone;
	};
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
	{
		// A transient EntityLocal can still be in the online entity table outside ALife.
		if ((what & reconcile::Replace) || ((what & reconcile::Gone) && !world::server_entity(id)))
			send_destroy();
		return;
	}
	if (id == 0 || world::is_player_character(e))
		what &= reconcile::Spawn; // a player's character is nobody's to replace, and has no switches here
	if (what & reconcile::Replace)
		send_destroy();
	// R32 W5: a squad with its members, a holder with what it holds (a Coop client's squad repair, a map server's missing squad)
	if ((what & (reconcile::Spawn | reconcile::Replace)) && !send_subtree(from, e))
		return;
	if ((what & reconcile::Switch) && e->ID_Parent == kNoId)
	{
		// A member goes with its squad: an EntitySwitch of its own tells a client that it left the squad (client_on_switch).
		CSE_ALifeMonsterAbstract* m = e->cast_monster_abstract();
		CSE_ALifeDynamicObject* squad = m && m->m_group_id != kNoId ? ai().alife().objects().object(m->m_group_id, true) : nullptr;
		CSE_ALifeDynamicObject* who = squad ? squad : e;
		Writer w(256);
		write_switch(w, who, online_here(who));
		net::host_send(from, Msg::EntitySwitch, w);
	}
	if (what & reconcile::Parent)
		events::host_send_parent(from, e);
	if (what & reconcile::Died)
		events::host_send_death(from, e);
	if (!(what & reconcile::State))
		return;
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
	{
		reconcile::SquadMsg m;
		m.squad = id;
		for (const auto& member : g->squad_members())
			m.members.push_back(member.first);
		Writer w(m.members.size() * 2 + 8);
		reconcile::write(w, m);
		net::host_send(from, Msg::EntitySquad, w);
	}
	else if (CSE_ALifeTraderAbstract* t = e->cast_trader_abstract())
	{
		reconcile::TraitsMsg m;
		m.object = id;
		m.community = s32(t->m_community_index);
		m.rank = s32(t->m_rank);
		m.reputation = s32(t->m_reputation);
		Writer w(16);
		reconcile::write(w, m);
		net::host_send(from, Msg::EntityTraits, w);
	}
	else
		items::host_send_state(from, id);
}

void on_peer_left(PeerId id)
{
	g_rc_asked.erase(std::remove_if(g_rc_asked.begin(), g_rc_asked.end(), [id](const RosterAsk& a) { return a.peer == id; }),
	    g_rc_asked.end());
}

void on_client_live()
{
	g_tracks.clear();
	g_rc_watch.reset();
	g_rc_repairs.clear();
	g_id_retry.clear();
}

void reconcile_frame()
{
	PROF_EVENT("coop/coop_roster/reconcile_frame");
	if (mode().decides_world() || mode().leads_level())
		host_reconcile();
	// R32 W5.1: every copy of a world repairs towards its writer: a client, and a map server (the host of its level, a live client of
	// its world) for what its world writes (client_on_roster skips what this process writes). The co-op host and the world server
	// are hosts only (Phase::Hosting): no client part runs there
	if (mgr().is_live_client())
		client_reconcile();
	mode().reconcile_world(); // the mode's own part (none has one now: the Core's client part covers a map server)
}

void register_console()
{
	CMD4(CCC_Integer, "coop_reconcile", &g_coop_reconcile, 0, 1); // the roster's digests and repairs
}

void reset_roster()
{
	g_rc_asked.clear();
	g_rc_bucket = 0;
	g_rc_last_step = g_rc_last_roster = 0;
	g_rc_watch.reset();
	g_rc_repairs.clear();
	g_id_retry.clear();
	g_rc_pacer.reset();
	g_rc_log = 0;
}

void print_roster_host()
{
	::Msg("[coop]   roster: %s, %llu digests, %llu rosters (%llu rows, %u waiting), %llu entity requests, %llu destroys "
	      "of ids that are gone", g_coop_reconcile ? "on" : "off", g_rc.digests, g_rc.rosters, g_rc.rows,
	    (u32)g_rc_asked.size(), g_rc.repairs_in, g_rc.repairs_gone);
	if (mgr().is_live_client()) // R32 W5.1: a map server is a client of its world too
		::Msg("[coop]   roster of the world: %llu digests compared, %llu differed, %llu rosters asked / %llu came, %llu repairs "
		      "asked (%u waiting), %llu spawns replaced another object under their id, %llu stale spawns of an object written "
		      "here (ours stood), %llu squads made whole",
		    g_rc.compared, g_rc.differed, g_rc.rosters_asked, g_rc.rosters_in, g_rc.repairs_asked, (u32)g_rc_repairs.size(),
		    g_replaced, g_stale_spawns, g_squads_whole);
}

void print_roster_client()
{
	::Msg("[coop]   roster: %s, %llu digests compared, %llu differed (%u buckets differ now, %llu healed), %llu rosters "
	      "asked / %llu came, %llu repairs asked (spawn %llu, switch %llu, holder %llu, death %llu, state %llu, gone %llu, "
	      "replace %llu), %u waiting (%llu refused), %u under way, %llu retries of a taken id (%u now)",
	    g_coop_reconcile ? "on" : "off", g_rc.compared, g_rc.differed, g_rc_watch.watched(), g_rc_watch.healed(),
	    g_rc.rosters_asked, g_rc.rosters_in, g_rc.repairs_asked, g_rc.kinds[0], g_rc.kinds[1], g_rc.kinds[2],
	    g_rc.kinds[3], g_rc.kinds[4], g_rc.kinds[5], g_rc.kinds[6], (u32)g_rc_repairs.size(), g_rc_repairs.refused(),
	    (u32)g_rc_watch.repairing(), g_rc.retries, (u32)g_id_retry.size());
	::Msg("[coop]   roster applied: %llu traits, squad members %llu out / %llu in (%llu waited)", g_rc.traits_applied,
	    g_rc.squad_members_out, g_rc.squad_members_in, g_rc.squad_waits);
}
} // namespace entities
} // namespace coop
