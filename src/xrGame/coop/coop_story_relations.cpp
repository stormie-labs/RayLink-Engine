// coop::story, relations: personal and community goodwill and the community table, the host's broadcasts, a client's
// requests, the full list and its diagnosis, and the reputation from the host's combat bookkeeping.
#include "stdafx.h"
#include "coop_story_impl.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_progress.h"
#include "coop_world.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/srv/coop_ghost_rules.h"

#include "Actor.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "relation_registry.h"
#include "character_community.h"
#include "alife_registry_wrappers.h"
#include "alife_registry_container.h"
#include "alife_simulator.h"
#include "ai_space.h"

namespace coop
{
namespace story
{
// ------------------------------------------------------------------ relations

namespace
{
xr_set<RelKey> g_rel_dirty; // host: keys whose current value must be broadcast
ghost::Log g_ghost_said; // a map server: what its logic said of "the actor", for the players of its level
xr_map<u16, int> g_rep_delta; // host: proxy -> reputation change from attacks / fight help (sent in frame)
RELATION_DATA& rel_data(u16 id) { return RELATION_REGISTRY::relation_registry().registry().objects(id); }

const CRelationRegistry::OBJECT_REGISTRY* rel_all()
{
	if (!ai().get_alife())
		return nullptr;
	return &ai().alife().registry().get<CRelationRegistry>().objects();
}
} // namespace

bool community_ok(int c) { return c >= 0 && c <= int(CHARACTER_COMMUNITY::GetMaxIndex()); }

namespace
{
int rel_value(const RelKey& k)
{
	switch (k.kind)
	{
	case RelPersonal: return RELATION_REGISTRY().GetGoodwill(u16(k.a), u16(k.b));
	case RelCommunity: return RELATION_REGISTRY().GetCommunityGoodwill(k.a, u16(k.b));
	default: return RELATION_REGISTRY().GetCommunityRelation(k.a, k.b);
	}
}
} // namespace

void host_flush_relations()
{
	if (g_rel_dirty.empty() || !ai().get_alife())
		return;
	xr_set<RelKey> dirty;
	dirty.swap(g_rel_dirty);
	for (const RelKey& k : dirty)
	{
		Writer w(16);
		w.u8(k.kind);
		if (k.kind == RelPersonal)
		{
			w.u16(u16(k.a));
			w.u16(u16(k.b));
		}
		else if (k.kind == RelCommunity)
		{
			w.i32(k.a);
			w.u16(u16(k.b));
		}
		else
		{
			w.i32(k.a);
			w.i32(k.b);
		}
		w.i32(rel_value(k));
		net::host_broadcast(Msg::Relation, w);
		++g_stats.rel_out;
	}
}

// Host: the engine changed a player's reputation on its proxy (Action: attack, fight help). The player's
// machine owns its reputation (the proxy's value is put back from its reports), so the change goes to it.
// Where the fight is, its record is not always: a process that leads a level apart from its
// world (mode().level(): a map server) tells the world the fact (LevelSide::level_fight_reputation) and the world applies it and tells
// him (coop_srv_progress.script checks it, then progress::host_apply_fight); a process that keeps the record applies it here.
void host_flush_rep_deltas()
{
	if (g_rep_delta.empty())
		return;
	xr_map<u16, int> list;
	list.swap(g_rep_delta);
	for (const auto& it : list)
	{
		if (!it.second)
			continue;
		if (LevelSide* l = mode().level())
		{
			l->level_fight_reputation(it.first, it.second);
			++g_stats.rep_facts_up;
			continue;
		}
		const PeerId peer = join::peer_of_proxy(it.first);
		if (progress::host_apply_fight(peer, it.first, it.second))
			++g_stats.rep_deltas_out;
	}
}

// Raw writes on a client: the value is the host's, already clamped; no hooks run.
void client_set_personal(u16 host_from, u16 host_to, int v)
{
	rel_data(local_id_of(host_from)).personal[local_id_of(host_to)].SetGoodwill(v);
}
void client_set_community(int community, u16 host_to, int v)
{
	if (community_ok(community))
		rel_data(local_id_of(host_to)).communities[community].SetGoodwill(v);
}
void client_set_comm_relation(int a, int b, int v)
{
	if (community_ok(a) && community_ok(b))
		CHARACTER_COMMUNITY::set_relation(a, b, v);
}

DomainHash hash_relations()
{
	DomainHash d;
	const CRelationRegistry::OBJECT_REGISTRY* all = rel_all();
	if (!all)
		return d;
	for (const auto& it : *all)
	{
		const u16 from = it.first;
		const bool from_player = is_player(from);
		for (const auto& p : it.second.personal)
		{
			if (p.second.Goodwill() == NEUTRAL_GOODWILL || !(from_player || is_player(p.first)))
				continue;
			Fnv f;
			f.pod(u8(RelPersonal));
			f.pod(host_id_of(from));
			f.pod(host_id_of(p.first));
			f.pod(p.second.Goodwill());
			d.add(f.h);
		}
		if (!from_player)
			continue;
		for (const auto& c : it.second.communities)
		{
			if (c.second.Goodwill() == NEUTRAL_GOODWILL)
				continue;
			Fnv f;
			f.pod(u8(RelCommunity));
			f.pod(int(c.first));
			f.pod(host_id_of(from));
			f.pod(c.second.Goodwill());
			d.add(f.h);
		}
	}
	const int n = int(CHARACTER_COMMUNITY::GetMaxIndex()) + 1;
	for (int a = 0; a < n; ++a)
		for (int b = 0; b < n; ++b)
		{
			Fnv f;
			f.pod(u8(RelCommRelation));
			f.pod(a);
			f.pod(b);
			f.pod(RELATION_REGISTRY().GetCommunityRelation(a, b));
			d.add(f.h);
		}
	return d;
}

void send_relation_list(PeerId to)
{
	const CRelationRegistry::OBJECT_REGISTRY* all = rel_all();
	if (!all)
		return;
	struct P
	{
		u16 from, to;
		int v;
	};
	struct C
	{
		int comm;
		u16 to;
		int v;
	};
	xr_vector<P> personal;
	xr_vector<C> community;
	for (const auto& it : *all)
	{
		const bool from_player = is_player(it.first);
		for (const auto& p : it.second.personal)
			if (p.second.Goodwill() != NEUTRAL_GOODWILL && (from_player || is_player(p.first)))
				personal.push_back(P{it.first, p.first, p.second.Goodwill()});
		if (from_player)
			for (const auto& c : it.second.communities)
				if (c.second.Goodwill() != NEUTRAL_GOODWILL)
					community.push_back(C{c.first, it.first, c.second.Goodwill()});
	}
	const int n = int(CHARACTER_COMMUNITY::GetMaxIndex()) + 1;
	Writer w(personal.size() * 8 + community.size() * 10 + n * n * 4 + 16);
	w.u8(RelFull);
	w.u32((u32)personal.size());
	for (const P& p : personal)
	{
		w.u16(p.from);
		w.u16(p.to);
		w.i32(p.v);
	}
	w.u32((u32)community.size());
	for (const C& c : community)
	{
		w.i32(c.comm);
		w.u16(c.to);
		w.i32(c.v);
	}
	w.u32(u32(n));
	for (int a = 0; a < n; ++a)
		for (int b = 0; b < n; ++b)
			w.i32(RELATION_REGISTRY().GetCommunityRelation(a, b));
	net::host_send(to, Msg::Relation, w);
	++g_stats.rel_out;
}

namespace
{
// Diagnosis of a relations hash that never agrees (the same two hashes from the join to the end, through every full resync,
// with about a thousand entries on both sides: a few personal ones and the community matrix). Right after a full list is
// applied, when the hash has been wrong for several checks, say where three things stand: the list the host just sent (hashed here with the
// formula of hash_relations), the hash the host said, and what this machine holds; and the first entries of this
// machine that differ from the list. List == host hash but != here: the apply does not stick (a hook, a clamp, the id
// swap). List != host hash: the host's list and its own hash disagree.
struct RelTriple
{
	int a, b, v; // personal: from, to (host numbering); community: community, to; the matrix is a separate table
};
} // namespace

DomainHash g_rel_host_hash; // client: the relations entry of the last StoryHash
bool g_rel_diag_wanted = false;

namespace
{
DomainHash hash_of_relation_list(const xr_vector<RelTriple>& personal, const xr_vector<RelTriple>& community, u32 n,
                                 const xr_vector<int>& table)
{
	DomainHash d;
	for (const RelTriple& t : personal)
	{
		Fnv f;
		f.pod(u8(RelPersonal));
		f.pod(u16(t.a));
		f.pod(u16(t.b));
		f.pod(CHARACTER_GOODWILL(t.v));
		d.add(f.h);
	}
	for (const RelTriple& t : community)
	{
		Fnv f;
		f.pod(u8(RelCommunity));
		f.pod(int(t.a));
		f.pod(u16(t.b));
		f.pod(CHARACTER_GOODWILL(t.v));
		d.add(f.h);
	}
	if (table.size() == size_t(n) * n)
		for (u32 a = 0; a < n; ++a)
			for (u32 b = 0; b < n; ++b)
			{
				Fnv f;
				f.pod(u8(RelCommRelation));
				f.pod(int(a));
				f.pod(int(b));
				f.pod(CHARACTER_GOODWILL(table[a * n + b]));
				d.add(f.h);
			}
	return d;
}

void diagnose_relations(const xr_vector<RelTriple>& personal, const xr_vector<RelTriple>& community, u32 n,
                        const xr_vector<int>& table)
{
	const DomainHash list = hash_of_relation_list(personal, community, n, table);
	const DomainHash here = hash_relations();
	::Msg("[coop] story: relations diagnosis: the host's list %u/%016llx, the host's hash %u/%016llx, this machine %u/%016llx",
	      list.count, list.hash, g_rel_host_hash.count, g_rel_host_hash.hash, here.count, here.hash);
	if (here == list)
	{
		::Msg("[coop] story: relations diagnosis: this machine holds exactly the list; the difference is between the "
		      "host's list and the host's own hash");
		return;
	}
	u32 shown = 0, differ = 0;
	const auto say = [&](const char* what, int a, int b, int host, int mine)
	{
		++differ;
		if (shown++ < 8)
			::Msg("[coop] story: relations diagnosis: %s %d -> %d: list %d, here %d", what, a, b, host, mine);
	};
	// the matrix
	if (table.size() == size_t(n) * n && n == u32(CHARACTER_COMMUNITY::GetMaxIndex()) + 1)
		for (u32 a = 0; a < n; ++a)
			for (u32 b = 0; b < n; ++b)
			{
				const int mine = RELATION_REGISTRY().GetCommunityRelation(int(a), int(b));
				if (mine != table[a * n + b])
					say("matrix", int(a), int(b), table[a * n + b], mine);
			}
	// personal entries, in host numbering, as hash_relations counts them
	xr_map<std::pair<int, int>, int> held;
	if (const CRelationRegistry::OBJECT_REGISTRY* all = rel_all())
		for (const auto& it : *all)
		{
			const bool from_player = is_player(it.first);
			for (const auto& p : it.second.personal)
				if (p.second.Goodwill() != NEUTRAL_GOODWILL && (from_player || is_player(p.first)))
					held[{int(host_id_of(it.first)), int(host_id_of(p.first))}] = p.second.Goodwill();
		}
	xr_map<std::pair<int, int>, int> listed;
	for (const RelTriple& t : personal)
		listed[{t.a, t.b}] = t.v;
	for (const auto& l : listed)
	{
		const auto h = held.find(l.first);
		if (h == held.end())
			say("personal missing here", l.first.first, l.first.second, l.second, 0);
		else if (h->second != l.second)
			say("personal", l.first.first, l.first.second, l.second, h->second);
	}
	for (const auto& h : held)
		if (listed.find(h.first) == listed.end())
			say("personal not in the list", h.first.first, h.first.second, 0, h.second);
	::Msg("[coop] story: relations diagnosis: %u entr%s of this machine differ from the list", differ, differ == 1 ? "y" : "ies");
}
} // namespace

void client_apply_relation_list(Reader& r)
{
	struct P
	{
		u16 from, to;
		int v;
	};
	struct C
	{
		int comm;
		u16 to;
		int v;
	};
	xr_vector<P> personal;
	xr_vector<C> community;
	const u32 np = r.u32();
	for (u32 i = 0; i < np && r.ok(); ++i)
	{
		P p;
		p.from = r.u16();
		p.to = r.u16();
		p.v = r.i32();
		personal.push_back(p);
	}
	const u32 nc = r.u32();
	for (u32 i = 0; i < nc && r.ok(); ++i)
	{
		C c;
		c.comm = r.i32();
		c.to = r.u16();
		c.v = r.i32();
		community.push_back(c);
	}
	const u32 n = r.u32();
	xr_vector<int> table;
	if (n <= 256)
	{
		table.reserve(n * n);
		for (u32 i = 0; i < n * n && r.ok(); ++i)
			table.push_back(r.i32());
	}
	if (!r.ok() || !ai().get_alife())
		return;

	// Replace every player-related entry; NPC-to-NPC relations stay the host world's business.
	xr_vector<u16> ids;
	if (const CRelationRegistry::OBJECT_REGISTRY* all = rel_all())
		for (const auto& it : *all)
			ids.push_back(it.first);
	for (u16 id : ids)
	{
		RELATION_DATA& d = rel_data(id);
		if (is_player(id))
		{
			d.personal.clear();
			d.communities.clear();
			continue;
		}
		for (auto it = d.personal.begin(); it != d.personal.end();)
		{
			if (is_player(it->first))
				it = d.personal.erase(it);
			else
				++it;
		}
	}
	for (const P& p : personal)
		client_set_personal(p.from, p.to, p.v);
	for (const C& c : community)
		client_set_community(c.comm, c.to, c.v);
	if (n == u32(CHARACTER_COMMUNITY::GetMaxIndex()) + 1 && table.size() == size_t(n) * n)
		for (u32 a = 0; a < n; ++a)
			for (u32 b = 0; b < n; ++b)
				client_set_comm_relation(int(a), int(b), table[a * n + b]);
	if (g_rel_diag_wanted) // the hash was wrong several times in a row (client_on_hash): one diagnosis per streak
	{
		g_rel_diag_wanted = false;
		xr_vector<RelTriple> pp, cc;
		for (const P& p : personal)
			pp.push_back(RelTriple{int(p.from), int(p.to), p.v});
		for (const C& c : community)
			cc.push_back(RelTriple{c.comm, int(c.to), c.v});
		diagnose_relations(pp, cc, n, table);
	}
	++g_stats.rel_in;
	::Msg("[coop] story: relations resynced from the host: %u personal, %u community", np, nc);
}

// Right after join the client's relation registry is the host's, in host numbering: the actor (0)
// and our proxy are swapped like the characters were (coop_join), so personal attitudes and
// community goodwill follow the right player.
void client_swap_relations()
{
	const u16 p = join::my_proxy();
	if (p == kNoId || !ai().get_alife())
		return;
	{
		RELATION_DATA mine = rel_data(p);
		RELATION_DATA host = rel_data(0);
		rel_data(0) = mine;
		rel_data(p) = host;
	}
	xr_vector<u16> ids;
	if (const CRelationRegistry::OBJECT_REGISTRY* all = rel_all())
		for (const auto& it : *all)
			ids.push_back(it.first);
	u32 swapped = 0;
	for (u16 id : ids)
	{
		PERSONAL_RELATION_MAP& m = rel_data(id).personal;
		auto i0 = m.find(0);
		auto ip = m.find(p);
		const bool h0 = i0 != m.end(), hp = ip != m.end();
		if (!h0 && !hp)
			continue;
		const SRelation r0 = h0 ? i0->second : SRelation();
		const SRelation rp = hp ? ip->second : SRelation();
		m.erase(0);
		m.erase(p);
		if (hp)
			m[0] = rp;
		if (h0)
			m[p] = r0;
		++swapped;
	}
	::Msg("[coop] story: relations swapped to this player (actor <-> proxy %u, %u personal lists)", p, swapped);
}

namespace
{
bool client_relation_request(u8 op, int a, int b, int v)
{
	// A mirror's fight is the host's to count. A map server is the authority of the fights on its level: what they do to goodwill it asks the
	// world server for like any other relation, and hears it back.
	if (g_action_depth > 0 && !mode().leads_level())
	{
		++g_stats.action_dropped;
		return true;
	}
	if (applying_any())
	{
		++g_stats.derived_dropped;
		return true;
	}
	Writer w(16);
	w.u8(op);
	if (op == RelReqCommunitySet || op == RelReqCommunityChange || op == RelReqCommRelation)
		w.i32(a);
	else
		w.u16(host_id_of(u16(a)));
	if (op == RelReqCommRelation)
		w.i32(b);
	else
		w.u16(host_id_of(u16(b)));
	w.i32(v);
	// "The relation of community a to b is v" is a fact of a pair: a queued one is replaced by this value (the
	// host needs the last). The other ops (a goodwill to a player, by a value or by a delta) are not merged.
	const limits::CommunityPair pair{a, b};
	push_out(Msg::Relation, std::move(w), op == RelReqCommRelation ? &pair : nullptr);
	++g_stats.requests_out;
	return true;
}
} // namespace

void host_on_relation_request(PeerId from, u8 op, Reader& r)
{
	++g_stats.requests_in;
	ExecFor ef(from);
	switch (op)
	{
	case RelReqPersonalSet:
	case RelReqPersonalChange:
	case RelReqPersonalForce:
	{
		const u16 a = r.u16(), b = r.u16();
		const int v = r.i32();
		if (!r.ok() || a == kNoId || b == kNoId || !(is_player(a) || is_player(b)))
			return;
		if (op == RelReqPersonalSet)
			RELATION_REGISTRY().SetGoodwill(a, b, v);
		else if (op == RelReqPersonalChange)
			RELATION_REGISTRY().ChangeGoodwill(a, b, v);
		else
			RELATION_REGISTRY().ForceSetGoodwill(a, b, v);
		break;
	}
	case RelReqCommunitySet:
	case RelReqCommunityChange:
	{
		const int c = r.i32();
		const u16 to = r.u16();
		const int v = r.i32();
		if (!r.ok() || !community_ok(c) || !is_player(to))
			return;
		if (op == RelReqCommunitySet)
			RELATION_REGISTRY().SetCommunityGoodwill(c, to, v);
		else
			RELATION_REGISTRY().ChangeCommunityGoodwill(c, to, v);
		break;
	}
	case RelReqCommRelation:
	{
		const int a = r.i32(), b = r.i32(), v = r.i32();
		if (!r.ok() || !community_ok(a) || !community_ok(b))
			return;
		RELATION_REGISTRY().SetCommunityRelation(a, b, v);
		break;
	}
	case RelUpFightFact:
	{
		// A map server's fight fact. The keeper's script checks the transport's sender (a map server's bridge), his
		// route and the order through coop_trust before it applies it (coop_srv_progress.script); a fact that does not read, or a process
		// whose script has no keeper entry (the co-op: no map server ever sends one), drops it here.
		fight::FactMsg f;
		if (f.read(r) && progress::host_fight_fact(from, f.epoch, f.fact, f.character, f.delta))
			++g_stats.rep_facts_in;
		else
			++g_stats.rep_facts_refused;
		break;
	}
	default: break;
	}
}

// A map server: what its logic said of "the actor" goes to the players of its level, whenever they come. The outbox has its cap, and a
// level's first look can say a lot at once.
void ghost_frame()
{
	PROF_EVENT("coop/coop_story/ghost_frame");
	constexpr u32 kPerFrame = 16;
	constexpr size_t kOutboxRoom = limits::kMaxRelationOutbox / 2;
	if (!g_ghost_said.size())
		return;
	for (const PeerInfo& p : mgr().session().peers())
	{
		const u16 proxy = join::proxy_of_peer(p.id);
		if (proxy == kNoId || g_outbox.relation_size() >= kOutboxRoom)
			continue;
		g_ghost_said.tell(proxy, kPerFrame,
		    [](const ghost::Said& s) { client_relation_request(s.op, s.first, s.second, s.value); });
	}
}

void reset_relations()
{
	g_rel_dirty.clear();
	g_ghost_said.clear();
	g_rep_delta.clear();
}

// ------------------------------------------------------------------ hooks: relations

bool acts_as_actor(const CEntityAlive* who)
{
	// A section check: the same answer on every machine, in a session or not.
	return !smart_cast<const CActor*>(who) && world::is_player_character(who);
}

RelationActionScope::RelationActionScope(CEntityAlive* from, int action)
{
	// Only the outermost call (ATTACK may call itself for FIGHT_HELP) measures the whole change. A kill is
	// counted by the killer's own machine (its CEntityAlive::Die runs Action for its actor).
	if (g_action_depth++ != 0 || action == RELATION_REGISTRY::KILL ||
	    (!mode().decides_world() && !mode().leads_level()) || !acts_as_actor(from))
		return;
	m_owner = smart_cast<CInventoryOwner*>(from);
	if (!m_owner)
		return;
	m_id = from->ID();
	m_reputation = m_owner->Reputation();
	++g_stats.proxy_actions;
}

RelationActionScope::~RelationActionScope()
{
	--g_action_depth;
	if (!m_owner)
		return;
	const int delta = m_owner->Reputation() - m_reputation;
	if (delta)
		g_rep_delta[m_id] += delta; // sent from frame() (this may run on the game thread)
}

namespace
{
// A map server's actor is a ghost, and id 0 on the world server is the host's character: such a word is the players' of this level, it
// never goes up as it is. Said while the level loads it waits for them as well.
bool ghost_said(const ghost::Said& s)
{
	if (!mode().ghost_said(s))
		return false;
	if (applying_any())
		++g_stats.derived_dropped;
	else
		g_ghost_said.add(s);
	return true;
}
} // namespace

bool on_personal_goodwill(u16 from, u16 to, int value, GoodwillOp op)
{
	if (!mgr().active() || !ai().get_alife() || !(is_player(from) || is_player(to)))
		return false;
	if (mode().decides_world())
	{
		g_rel_dirty.insert(RelKey{RelPersonal, from, to});
		return false;
	}
	const u8 req = op == GoodwillOp::Set ? RelReqPersonalSet : op == GoodwillOp::Change ? RelReqPersonalChange : RelReqPersonalForce;
	if (ghost_said(ghost::Said{req, false, from, to, value}))
		return true;
	if (!mgr().is_live_client())
		return false;
	return client_relation_request(req, from, to, value);
}

bool on_community_goodwill(int community, u16 to, int value, GoodwillOp op)
{
	if (!mgr().active() || !ai().get_alife() || !is_player(to))
		return false;
	if (mode().decides_world())
	{
		g_rel_dirty.insert(RelKey{RelCommunity, community, to});
		return false;
	}
	const u8 req = op == GoodwillOp::Change ? RelReqCommunityChange : RelReqCommunitySet;
	if (ghost_said(ghost::Said{req, true, community, to, value}))
		return true;
	if (!mgr().is_live_client())
		return false;
	return client_relation_request(req, community, to, value);
}

bool on_community_relation(int community_from, int community_to, int value)
{
	if (!mgr().active())
		return false;
	if (mode().decides_world())
	{
		g_rel_dirty.insert(RelKey{RelCommRelation, community_from, community_to});
		return false;
	}
	if (!mgr().is_live_client())
		return false;
	return client_relation_request(RelReqCommRelation, community_from, community_to, value);
}
} // namespace story
} // namespace coop
