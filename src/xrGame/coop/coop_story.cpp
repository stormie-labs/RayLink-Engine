#include "stdafx.h"
#include "pch_script.h"
#include "coop_story_impl.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_progress.h"
#include "coop_world.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_rules.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "Level.h"
#include "Actor.h"
#include "InventoryOwner.h"
#include "GameTaskManager.h"
#include "GameTask.h"
#include "map_manager.h"
#include "map_location.h"
#include "alife_registry_wrappers.h"
#include "alife_registry_container.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"
#include "script_engine.h"
#include "xrCoop/coop_lua_fn.h"

int g_coop_share_rewards = 0; // console coop_share_rewards (host setting, read by coop_tasks.script)

namespace coop
{
namespace story
{
const char* cs(const shared_str& s) { return s.c_str() ? s.c_str() : ""; }
shared_str to_shared(const std::string& s) { return s.empty() ? shared_str() : shared_str(s.c_str()); }

// UI / sleep info portions belong to one machine (coop_rules.h): not sent, not applied, not hashed.
bool machine_info(const shared_str& id) { return rules::is_machine_info(cs(id)); }

u16 host_id_of(u16 local) { return local == kNoId ? kNoId : join::to_host(local); }
u16 local_id_of(u16 host) { return host == kNoId ? kNoId : join::to_local(host); }

bool in_game() { return world_up(); } // world gate: g_pGameLevel alone is set at the start of CLevel::CLevel
bool client_live() { return mgr().is_live_client() && in_game(); }
CGameTaskManager& tasks() { return Level().GameTaskManager(); }
CMapManager& spots() { return Level().MapManager(); }

// A player character: the local actor or a proxy/avatar entity. Same answer on every machine
// for the same entity (section check, not the connection registry).
bool is_player(u16 id)
{
	if (id == 0)
		return true;
	if (id == kNoId || !ai().get_alife())
		return false;
	return world::is_player_character(ai().alife().objects().object(id, true));
}

Stats g_stats;

// ------------------------------------------------------------------ applying guard

ApplyState g_apply;

bool applying_any() { return g_apply.kind != Kind::None; }
bool applying_this(Kind kind, const shared_str& key, u16 id)
{
	return g_apply.kind == kind && g_apply.key == key && (id == kNoId || g_apply.id == id);
}

int g_action_depth = 0; // RELATION_REGISTRY::Action nesting
CGameTask* g_silent_task = nullptr; // client: task whose host state is being applied
PeerId g_exec_peer = kInvalidPeer; // host: client whose request is being executed

// ------------------------------------------------------------------ outbox

// A client sends infos and spots only as fast as the host takes them
// (kClientRateRules InfoPortion, MapSpot): the host drops what exceeds its limit unseen, and a lost info or spot never
// comes back. What waits stays queued, in order; the queue is bounded (a broken mod must not fill the memory).
// The relations wait in a lane of their own (limits::Outbox, coop_limits.h): a fight in a big modpack can make dozens of
// set_community_relation calls per kill, and behind the relation pacer they held back the infos, spots and tasks and, at the
// cap, took their room. A queued relation of the same pair of communities is replaced by the newer value.
limits::Outbox<Out> g_outbox;
limits::Pacer g_pacer;

namespace
{
// The outbox refused `type` (its lane is full); the outbox counts it, the log gets the first and every 100th.
void note_outbox_full(Msg type)
{
	const bool relation = type == Msg::Relation;
	const u64 drops = g_outbox.dropped_of(type);
	if (limits::drop_logged(drops))
		::Msg("! [coop] story: the %s queue is full (%u waiting), a %s was dropped (%llu of that queue so far)",
		    relation ? "relation" : "info / spot / task", u32(relation ? g_outbox.relation_size() : g_outbox.story_size()),
		    msg_name(type), drops);
}
} // namespace

void queue_out(Out&& o, const limits::CommunityPair* pair)
{
	const Msg type = o.type;
	if (g_outbox.push(type, std::move(o), pair) == limits::Outbox<Out>::Push::Full)
		note_outbox_full(type);
}

void push_out(Msg type, Writer&& w, const limits::CommunityPair* pair)
{
	Out o;
	o.type = type;
	o.w = std::move(w);
	queue_out(std::move(o), pair);
}

void send_out(Msg type, const Writer& w)
{
	if (mode().decides_world())
		net::host_broadcast(type, w);
	else
		net::client_send(type, w);
}

namespace
{
// Messages that arrived before this machine was ready for them.
struct Deferred
{
	Msg type;
	PeerId from;
	std::vector<u8> data;
};
xr_vector<Deferred> g_deferred;

// ------------------------------------------------------------------ info portions

xr_map<std::pair<u8, shared_str>, u64> g_info_requests; // client: (op, id) -> sent at
// Client: coop.story_hold_infos is on; infos given meanwhile are kept in g_local_infos (this machine
// only, not hashed, kept by a resync) until the host gives the same info.
bool g_hold_infos = false;
xr_set<shared_str> g_local_infos;
u64 g_infos_held = 0, g_holds_leaked = 0;

KNOWN_INFO_VECTOR* actor_infos()
{
	if (!g_actor || !g_actor->m_known_info_registry)
		return nullptr;
	return &g_actor->m_known_info_registry->registry().objects();
}

DomainHash hash_infos()
{
	DomainHash d;
	if (const KNOWN_INFO_VECTOR* v = actor_infos())
	{
		for (const shared_str& id : *v)
		{
			if (machine_info(id))
				continue; // UI / sleep infos are not part of the group story
			if (g_local_infos.count(id))
				continue; // client: held by this machine only (the host does not have it)
			Fnv f;
			f.str(id);
			d.add(f.h);
		}
	}
	return d;
}

void client_apply_info(bool give, const shared_str& id)
{
	if (!g_actor || !id.size())
		return;
	if (machine_info(id))
		return; // never applied from the network (the host's UI is not ours)
	g_info_requests.erase(std::make_pair(u8(give ? InfoGive : InfoDisable), id));
	g_local_infos.erase(id); // the host's word: shared from now on
	Applying guard(Kind::Info, id);
	if (give)
		g_actor->OnReceiveInfo(id);
	else
		g_actor->OnDisableInfo(id);
	++g_stats.infos_in;
}

void send_info_list(PeerId to)
{
	const KNOWN_INFO_VECTOR* v = actor_infos();
	if (!v)
		return;
	xr_vector<const shared_str*> shared; // UI / sleep infos stay on the host
	shared.reserve(v->size());
	for (const shared_str& id : *v)
		if (!machine_info(id))
			shared.push_back(&id);
	Writer w(shared.size() * 32 + 8);
	w.u8(InfoFull);
	w.u32((u32)shared.size());
	for (const shared_str* id : shared)
		w.str(cs(*id));
	net::host_send(to, Msg::InfoPortion, w);
	++g_stats.infos_out;
}

void client_apply_info_list(Reader& r)
{
	const u32 n = r.u32();
	xr_set<shared_str> host;
	for (u32 i = 0; i < n && r.ok(); ++i)
	{
		const shared_str id = to_shared(r.str());
		if (!machine_info(id)) // the host's UI / sleep infos are not ours
			host.insert(id);
	}
	if (!r.ok() || !g_actor)
		return;
	for (auto it = g_local_infos.begin(); it != g_local_infos.end();)
		it = host.count(*it) ? g_local_infos.erase(it) : std::next(it); // the host has it now: shared
	xr_vector<shared_str> drop, add;
	if (const KNOWN_INFO_VECTOR* v = actor_infos())
	{
		xr_set<shared_str> mine;
		for (const shared_str& id : *v)
			mine.insert(id);
		for (const shared_str& id : mine)
			if (!host.count(id) && !g_local_infos.count(id) && !machine_info(id)) // ours alone, keep
				drop.push_back(id);
		for (const shared_str& id : host)
			if (id.size() && !mine.count(id))
				add.push_back(id);
	}
	for (const shared_str& id : drop)
		client_apply_info(false, id);
	for (const shared_str& id : add)
		client_apply_info(true, id);
	::Msg("[coop] story: info portions resynced from the host: %u total, %u added, %u removed", n, (u32)add.size(),
	    (u32)drop.size());
}
// One queued message goes out (a map spot is built from the live spot now, not when it was queued).
void send_queued(Out& o, bool host)
{
	if (!o.spot_add)
	{
		send_out(o.type, o.w);
		return;
	}
	if (!in_game())
		return;
	CMapLocation* ml = spots().GetMapLocation(o.spot_type, o.spot_id);
	if (!spot_replicable(ml, o.spot_type))
		return; // gone already, or turned out to be a task spot
	Writer w(96);
	w.u8(SpotAdd);
	if (host)
	{
		host_spot_body(w, ml, o.spot_type, o.spot_id);
		net::host_broadcast(Msg::MapSpot, w);
		++g_stats.spots_out;
	}
	else
	{
		const u16 hid = host_id_of(o.spot_id);
		LPCSTR h = ml->GetHint();
		write_spot(w, o.spot_type, hid, h ? xr_string(h) : xr_string(), ml->Serializable(), mgr().self());
		net::client_send(Msg::MapSpot, w);
		g_spot_pending[SpotKey{o.spot_type, hid}] = mgr().now_ms();
		++g_stats.requests_out;
	}
}

void flush_outbox()
{
	if (g_outbox.empty())
		return;
	const bool host = mode().decides_world();
	const u64 now = mgr().now_ms();
	// What the client pacer holds waits for a later frame, in order; per lane, so the relations
	// held by their pacer hold back no info, spot or task (limits::Outbox::drain).
	g_outbox.drain([host, now](Msg type) { return host || g_pacer.take(type, now); },
	    [host](Out& o) { send_queued(o, host); });
}
// ------------------------------------------------------------------ hashes and resync

u64 g_last_hash = 0;
u32 g_streak[kHashDomains] = {};

void hash_all(DomainHash out[kHashDomains])
{
	out[0] = hash_infos();
	out[1] = hash_tasks();
	out[2] = hash_relations();
	out[3] = hash_spots();
}
} // namespace

void host_send_full(PeerId to, u8 mask)
{
	if (mask & DomInfos)
		send_info_list(to);
	if (mask & DomTasks)
	{
		send_task_list(to);
		send_targets_to(to);
	}
	if (mask & DomRelations)
		send_relation_list(to);
	if (mask & DomSpots)
		send_spot_list(to);
	if (mask & DomProgress)
		send_progress_all(to);
}

void host_flush_all()
{
	flush_outbox();
	host_flush_relations();
	host_scan_tasks();
}

namespace
{
void request_resync(u8 mask, const char* why)
{
	if (mode().decides_world())
	{
		host_flush_all();
		u32 n = 0;
		for (const PeerInfo& p : mgr().session().peers())
		{
			if (p.state != PeerState::Joined)
				continue;
			host_send_full(p.id, mask);
			++n;
		}
		::Msg("[coop] story: pushed full story state (mask %u) to %u client(s) (%s)", mask, n, why);
		return;
	}
	if (!mgr().is_live_client())
	{
		::Msg("! [coop] story: not in a live session");
		return;
	}
	Writer w(2);
	w.u8(mask);
	net::client_send(Msg::StoryResync, w);
	++g_stats.resyncs_out;
	::Msg("[coop] story: asked the host for the full story state (mask %u, %s)", mask, why);
}

void host_hash_frame()
{
	PROF_EVENT("coop/coop_story/host_hash_frame");
	const u64 now = mgr().now_ms();
	if (now - g_last_hash < kHashIntervalMs)
		return;
	g_last_hash = now;
	if (!in_game())
		return;
	bool any = false;
	for (const PeerInfo& p : mgr().session().peers())
		any = any || p.state == PeerState::Joined;
	if (!any)
		return;
	host_flush_all(); // the hash must describe what was sent before it
	DomainHash h[kHashDomains];
	hash_all(h);
	const auto put = [](Writer& out, const DomainHash (&d)[kHashDomains])
	{
		out.u8(u8(kHashDomains));
		for (const DomainHash& x : d)
		{
			out.u32(x.count);
			out.u64(x.hash);
		}
	};
	Writer w(64);
	put(w, h);
	if (g_audience.empty())
	{
		net::host_broadcast(Msg::StoryHash, w);
		++g_stats.hashes;
		return;
	}
	// Tasks with an audience: one StoryHash per player, the same layout, only its tasks domain is his own (hash_tasks_for).
	// The host player and the bridges get the unfiltered one, where host_broadcast would send it.
	if (!mode().decides_world() && !mode().leads_level())
		return;
	Session& s = mgr().session();
	for (const PeerInfo& p : s.peers())
	{
		if (p.state != PeerState::Joined || !mode().reaches(p.id, Msg::StoryHash))
			continue;
		DomainHash mine[kHashDomains];
		for (u32 i = 0; i < kHashDomains; ++i)
			mine[i] = h[i];
		mine[1] = hash_tasks_for(p.id); // [1]: the tasks domain (hash_all's order)
		Writer pw(64);
		put(pw, mine);
		net::host_send(p.id, Msg::StoryHash, pw);
	}
	mode().deliver_links(Msg::StoryHash, w, Lane::Reliable);
	++g_stats.hashes;
}

void client_on_hash(Reader& r)
{
	const u8 n = r.u8();
	DomainHash host[kHashDomains];
	for (u32 i = 0; i < n && i < kHashDomains; ++i)
	{
		host[i].count = r.u32();
		host[i].hash = r.u64();
	}
	if (!r.ok() || n < kHashDomains)
		return;
	++g_stats.hashes;
	client_reconcile(false);
	DomainHash mine[kHashDomains];
	hash_all(mine);
	static const char* const kNames[kHashDomains] = {"info portions", "tasks", "relations", "map spots"};
	u8 mask = 0;
	for (u32 i = 0; i < kHashDomains; ++i)
	{
		if (mine[i] == host[i])
		{
			g_streak[i] = 0;
			continue;
		}
		++g_streak[i];
		++g_stats.mismatches;
		if (i == 2)
		{
			g_rel_host_hash = host[i];
			if (g_streak[i] == 4) // the next full list (asked for below) is diagnosed once
				g_rel_diag_wanted = true;
		}
		// Persistent differences (e.g. a spot on an object this machine never got) back off to 1/min.
		if (g_streak[i] > 3 && g_streak[i] % 6)
			continue;
		::Msg("[coop] story: %s differ from the host (%u/%016llx here, %u/%016llx host)%s", kNames[i], mine[i].count,
		    mine[i].hash, host[i].count, host[i].hash, g_streak[i] > 3 ? ", persistent" : "");
		mask |= u8(1u << i);
	}
	if (mask)
		request_resync(mask, "hash mismatch");
}

// ------------------------------------------------------------------ dispatch

bool g_client_inited = false;

void ensure_client_init()
{
	if (g_client_inited || !client_live())
		return;
	g_client_inited = true;
	client_swap_relations();
	g_known.clear();
	for (const SGameTaskKey& k : tasks().GetGameTasks())
		if (k.game_task)
			g_known.insert(TaskKey{k.task_id, k.game_task->m_ReceiveTime});
	g_own_sent = Progress();
	g_last_progress = 0;
	::Msg("[coop] story: live, local scripts track %u task(s)", (u32)g_known.size());
	request_resync(DomAll, "joined");
}
} // namespace

bool ready()
{
	if (mode().decides_world())
	{
		if (!in_game())
			return false;
		if (g_host_level != g_pGameLevel || g_host_epoch != join::host_world_epoch())
		{
			// Hosting started or the world was replaced (clients get that one from a fresh save):
			// remember the current task list without broadcasting it, before any request runs.
			// (The epoch also catches a reloaded level that got the old CLevel's address.)
			g_host_level = g_pGameLevel;
			g_host_epoch = join::host_world_epoch();
			g_host_sent.clear();
			g_host_prime = true;
			host_scan_tasks();
		}
		return true;
	}
	if (!client_live())
		return false;
	ensure_client_init();
	return true;
}

namespace
{
void handle_host(Msg type, PeerId from, Reader& r)
{
	switch (type)
	{
	case Msg::InfoPortion:
	{
		const u8 op = r.u8();
		const shared_str id = to_shared(r.str());
		if (!r.ok() || !id.size() || (op != InfoGive && op != InfoDisable) || !g_actor)
			return;
		if (machine_info(id))
			return; // a client's UI / sleep info is its own (an old client may still send it)
		++g_stats.requests_in;
		const bool give = op == InfoGive;
		ExecFor ef(from);
		if (g_actor->HasInfo(id) != give)
			g_actor->TransferInfo(id, give); // the hook broadcasts it to everybody
		else
		{
			Writer w(64); // nothing changed here: the client is behind, correct it
			w.u8(give ? InfoGive : InfoDisable);
			w.str(cs(id));
			net::host_send(from, Msg::InfoPortion, w);
		}
		break;
	}
	case Msg::TaskUpdate:
	{
		const u8 op = r.u8();
		if (op == TaskReqGive)
			host_on_task_give(from, r);
		else if (op == TaskReqState)
			host_on_task_state(from, r);
		break;
	}
	case Msg::MapSpot: host_on_spot_request(from, r.u8(), r); break;
	case Msg::Relation: host_on_relation_request(from, r.u8(), r); break;
	case Msg::Progress:
	{
		r.u16(); // the sender's own idea of its id: the host knows its proxy
		Progress p;
		p.rank = r.i32();
		p.reputation = r.i32();
		p.community = r.i32();
		p.valid = true;
		const u16 proxy = join::proxy_of_peer(from);
		if (!r.ok() || proxy == kNoId)
			return;
		progress::host_check(from, proxy, p.rank, p.reputation, p.community); // "refuse" stores, applies, sends clamped
		g_reported[from] = p;
		apply_progress(proxy, p);
		Writer w(16);
		write_progress(w, proxy, p);
		net::host_broadcast(Msg::Progress, w, Lane::Reliable, from);
		++g_stats.progress_in;
		break;
	}
	case Msg::StoryResync:
	{
		const u8 mask = r.u8();
		if (!r.ok())
			return;
		++g_stats.resyncs_in;
		host_flush_all();
		host_send_full(from, mask);
		break;
	}
	default: break;
	}
}

void handle_client(Msg type, Reader& r)
{
	switch (type)
	{
	case Msg::InfoPortion:
	{
		const u8 op = r.u8();
		if (op == InfoFull)
		{
			client_apply_info_list(r);
			return;
		}
		const shared_str id = to_shared(r.str());
		if (r.ok() && id.size() && (op == InfoGive || op == InfoDisable))
			client_apply_info(op == InfoGive, id);
		break;
	}
	case Msg::TaskList: client_on_task_list(r); break;
	case Msg::TaskUpdate:
	{
		const u8 op = r.u8();
		if (op == TaskUpsert)
			client_on_task_upsert(r);
		else if (op == TaskTargets)
			client_on_task_targets(r);
		break;
	}
	case Msg::MapSpot:
	{
		const u8 op = r.u8();
		if (op == SpotFull)
		{
			client_apply_spot_list(r);
			return;
		}
		if (op == SpotAdd)
		{
			const shared_str type = to_shared(r.str());
			const u16 id = r.u16();
			const xr_string hint = r.str().c_str();
			const bool ser = r.boolean();
			const PeerId author = r.u32();
			if (r.ok())
				apply_spot_add(type, id, hint, ser, author);
		}
		else if (op == SpotRemove)
		{
			const shared_str type = to_shared(r.str());
			const u16 hid = r.u16();
			const u16 id = local_id_of(hid);
			if (!r.ok() || !type.size() || id == kNoId)
				return;
			g_spot_pending.erase(SpotKey{type, hid});
			Applying guard(Kind::Spot, type, id);
			spots().RemoveMapLocation(type, id);
		}
		else if (op == SpotRemoveAll)
		{
			const u16 id = local_id_of(r.u16());
			if (r.ok() && id != kNoId)
				remove_replicable_spots_of(id);
		}
		++g_stats.spots_in;
		break;
	}
	case Msg::Relation:
	{
		const u8 op = r.u8();
		if (op == RelFull)
		{
			client_apply_relation_list(r);
			return;
		}
		if (op == RelPersonal)
		{
			const u16 a = r.u16(), b = r.u16();
			const int v = r.i32();
			if (r.ok() && a != kNoId && b != kNoId)
				client_set_personal(a, b, v);
		}
		else if (op == RelCommunity)
		{
			const int c = r.i32();
			const u16 to = r.u16();
			const int v = r.i32();
			if (r.ok() && to != kNoId)
				client_set_community(c, to, v);
		}
		else if (op == RelCommRelation)
		{
			const int a = r.i32(), b = r.i32(), v = r.i32();
			if (r.ok())
				client_set_comm_relation(a, b, v);
		}
		else if (op == RelReputationDelta)
		{
			// The host counted an attack / fight help of ours: our own reputation changes here,
			// and the next progress report tells everybody.
			const u16 who = r.u16();
			const int delta = r.i32();
			if (r.ok() && delta && g_actor && local_id_of(who) == 0)
			{
				g_actor->ChangeReputation(delta);
				++g_stats.rep_deltas_in;
			}
			return;
		}
		++g_stats.rel_in;
		break;
	}
	case Msg::Progress:
	{
		const u16 who = r.u16();
		Progress p;
		p.rank = r.i32();
		p.reputation = r.i32();
		p.community = r.i32();
		p.valid = true;
		if (r.ok())
			apply_progress(local_id_of(who), p);
		++g_stats.progress_in;
		break;
	}
	case Msg::StoryHash: client_on_hash(r); break;
	default: break;
	}
}

void handle(Msg type, PeerId from, Reader& r)
{
	if (mode().decides_world())
		handle_host(type, from, r);
	else
		handle_client(type, r);
}

void dispatch(Msg type, PeerId from, Reader& r)
{
	if (!mgr().active())
		return;
	// Clients talk only to the host; the host only takes requests from clients.
	if (mode().decides_world() ? from == kHostPeer : from != kHostPeer)
		return;
	if (!ready())
	{
		if (g_deferred.size() >= kMaxDeferred)
		{
			::Msg("! [coop] story: deferred queue full, dropping %s", msg_name(type));
			return;
		}
		Deferred d;
		d.type = type;
		d.from = from;
		d.data.assign(r.cursor(), r.cursor() + r.remaining());
		g_deferred.push_back(std::move(d));
		++g_stats.deferred;
		return;
	}
	handle(type, from, r);
}

void replay_deferred()
{
	if (g_deferred.empty() || !ready())
		return;
	xr_vector<Deferred> list;
	list.swap(g_deferred);
	for (Deferred& d : list)
	{
		Reader r(d.data.data(), d.data.size());
		handle(d.type, d.from, r);
	}
}

// ------------------------------------------------------------------ console

class CCC_CoopTasksResync : public IConsole_Command
{
public:
	CCC_CoopTasksResync(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		if (!mgr().active())
		{
			::Msg("! [coop] not in a session");
			return;
		}
		const bool all = args && strstr(args, "all");
		request_resync(all ? u8(DomAll) : u8(DomTasks), "console");
	}
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "co-op: reload the task list from the host (client) or push it to all clients (host); 'all' = whole story");
	}
};

const char* peer_name(PeerId id)
{
	if (id == kHostPeer)
		return "host";
	const PeerInfo* p = mgr().session().peer(id);
	return p ? p->nick.c_str() : "?";
}
} // namespace

// ================================================================== public

void register_console()
{
	CMD1(CCC_CoopTasksResync, "coop_tasks_resync");
	CMD4(CCC_Integer, "coop_share_rewards", &g_coop_share_rewards, 0, 3);
}

void install_server(Session& s)
{
	const Msg types[] = {Msg::InfoPortion, Msg::TaskUpdate, Msg::MapSpot, Msg::Relation, Msg::Progress, Msg::StoryResync};
	for (Msg t : types)
		s.on(t, [t](PeerId from, Reader& r) { dispatch(t, from, r); });
}

void install_client(Endpoint& e)
{
	const Msg types[] = {Msg::InfoPortion, Msg::TaskList, Msg::TaskUpdate, Msg::MapSpot, Msg::Relation, Msg::Progress,
		Msg::StoryHash};
	for (Msg t : types)
		e.on(t, [t](PeerId from, Reader& r) { dispatch(t, from, r); });
}

void reset()
{
	g_outbox.clear();
	g_pacer.reset();
	g_deferred.clear();
	g_info_requests.clear();
	g_hold_infos = false;
	g_local_infos.clear(); // a new world brings its own infos (personal tasks stay: process lifetime)
	g_host_sent.clear();
	g_host_prime = true;
	g_host_level = nullptr;
	g_host_epoch = 0;
	g_host_last_scan = 0;
	g_target_sent.clear();
	g_target_last_scan = 0;
	g_target_rx.clear();
	g_target_unknown_logged.clear();
	g_target_unknown = 0;
	g_finisher.clear();
	g_audience.clear(); // the host's Lua pushes the audiences of a loaded world again (plan A: on its first host tick)
	g_client_recs.clear();
	g_known.clear();
	g_pending.clear(); // the objects stay alive: scripts may hold them
	g_state_requests.clear();
	g_last_reconcile = 0;
	reset_relations();
	g_spot_authors.clear();
	g_spot_pending.clear();
	g_own_sent = Progress();
	g_last_progress = 0;
	g_reported.clear();
	g_last_hash = 0;
	for (u32& s : g_streak)
		s = 0;
	g_client_inited = false;
	g_apply = ApplyState();
	g_action_depth = 0;
	g_silent_task = nullptr;
	g_exec_peer = kInvalidPeer;
}

void frame()
{
	PROF_EVENT("coop/coop_story/frame");
	if (g_hold_infos)
	{
		// coop.story_hold_infos(true) without the matching false (a Lua error between them): never
		// carry a hold into the next frame. Lua runs synchronously, never across this call.
		g_hold_infos = false;
		if (++g_holds_leaked <= 10)
			::Msg("! [coop] story: info hold left on by a script, released");
	}
	if (!mgr().active())
		return;
	if (mode().decides_world())
	{
		if (!ready())
			return;
		replay_deferred();
		flush_outbox();
		host_flush_relations();
		host_flush_rep_deltas();
		if (mgr().now_ms() - g_host_last_scan >= kTaskScanIntervalMs)
			host_scan_tasks();
		if (mgr().now_ms() - g_target_last_scan >= kTargetScanIntervalMs)
			host_scan_targets();
		progress_frame();
		host_hash_frame();
		return;
	}
	if (!client_live())
	{
		g_outbox.clear(); // loading: local changes are healed by the resync at live
		return;
	}
	ensure_client_init();
	replay_deferred();
	// A process that does not decide the world and leads its map is a map server (mode(): the co-op client and a server's
	// player lead none); its ghost's words and the reputation of the fights on its map are its own steps (tests/test_story_bridges.cpp)
	const bool leads_its_map = mode().leads_level();
	if (leads_its_map)
		ghost_frame();
	flush_outbox();
	if (leads_its_map)
		host_flush_rep_deltas(); // a fight on its level: its fact goes up, the world server applies it and tells him (H-FIGHTREP)
	if (mgr().now_ms() - g_last_reconcile >= kReconcileIntervalMs)
		client_reconcile(false);
	progress_frame();
	const u64 now = mgr().now_ms();
	for (auto it = g_info_requests.begin(); it != g_info_requests.end();)
		it = now - it->second > kRequestRetryMs ? g_info_requests.erase(it) : std::next(it);
	for (auto it = g_state_requests.begin(); it != g_state_requests.end();)
		it = now - it->second > kRequestRetryMs ? g_state_requests.erase(it) : std::next(it);
	for (auto it = g_spot_pending.begin(); it != g_spot_pending.end();)
		it = now - it->second > kSpotPendingMs ? g_spot_pending.erase(it) : std::next(it);
}

void print_status()
{
	DomainHash h[kHashDomains];
	if (in_game())
		hash_all(h);
	::Msg("[coop] story: infos %u/%016llx, tasks %u/%016llx, relations %u/%016llx, map spots %u/%016llx", h[0].count,
	    h[0].hash, h[1].count, h[1].hash, h[2].count, h[2].hash, h[3].count, h[3].hash);
	::Msg("[coop] story: out infos %llu tasks %llu rel %llu spots %llu progress %llu requests %llu | in infos %llu tasks %llu "
	      "rel %llu spots %llu progress %llu requests %llu",
	    g_stats.infos_out, g_stats.tasks_out, g_stats.rel_out, g_stats.spots_out, g_stats.progress_out, g_stats.requests_out,
	    g_stats.infos_in, g_stats.tasks_in, g_stats.rel_in, g_stats.spots_in, g_stats.progress_in, g_stats.requests_in);
	::Msg("[coop] story: tasks created %llu detached %llu, known %u, pending %u | dropped: reactions %llu, combat goodwill %llu, "
	      "task spot edits %llu, foreign spot removals %llu | hashes %llu, mismatches %llu, resyncs out %llu in %llu, "
	      "deferred %llu",
	    g_stats.tasks_created, g_stats.tasks_detached, (u32)g_known.size(), (u32)g_pending.size(), g_stats.derived_dropped,
	    g_stats.action_dropped, g_stats.task_edits_dropped, g_stats.spot_removes_refused, g_stats.hashes,
	    g_stats.mismatches, g_stats.resyncs_out, g_stats.resyncs_in, g_stats.deferred);
	::Msg("[coop] story: player proxies in combat bookkeeping %llu, reputation deltas out %llu in %llu, share rewards %d",
	    g_stats.proxy_actions, g_stats.rep_deltas_out, g_stats.rep_deltas_in, g_coop_share_rewards);
	::Msg("[coop] story: fight facts up %llu, taken %llu, refused %llu", g_stats.rep_facts_up, g_stats.rep_facts_in,
	    g_stats.rep_facts_refused);
	::Msg("[coop] story: held infos %llu (kept here %u, holds left on %llu) | personal tasks %u, local task edits %llu",
	    g_infos_held, (u32)g_local_infos.size(), g_holds_leaked, (u32)g_local_tasks.size(), g_local_task_edits);
	::Msg("[coop] story: outbox %u (relations %u), held for the host's rate %llu frames, dropped (queue full) %llu infos / "
	      "spots / tasks and %llu relations, relations replaced by a newer value of their pair %llu",
	    (u32)g_outbox.size(), (u32)g_outbox.relation_size(), g_outbox.held(), g_outbox.dropped_story(),
	    g_outbox.dropped_relation(), g_outbox.merged());
	for (const auto& it : g_finisher)
		::Msg("[coop] story:   task '%s' finished at the request of %s", cs(it.first), peer_name(it.second));
	print_task_targets(); // task spots of host and client, compared line by line
	if (in_game()) // the Lua side of shared tasks, when coop_tasks.script has it
	{
		std::string error;
		luafn::call_named(ai().script_engine().lua(), "coop_tasks", "print_status", nullptr, [](lua_State*) { return 0; }, 0,
		    [](lua_State*) {}, error);
	}
}

bool applying() { return applying_any(); }

// ------------------------------------------------------------------ hooks: info portions

bool on_info(const CInventoryOwner* owner, const shared_str& info_id, bool give)
{
	if (!owner || !info_id.size() || !mgr().active() || owner->object_id() != 0)
		return false;
	if (machine_info(info_id))
		return false; // UI / sleep info of this machine: applied here, never sent, never held or dropped
	if (owner->HasInfo(info_id) == give)
		return false; // nothing changes: let the engine run its no-op path
	const u8 op = give ? InfoGive : InfoDisable;
	if (mode().decides_world())
	{
		Writer w(64);
		w.u8(op);
		w.str(cs(info_id));
		push_out(Msg::InfoPortion, std::move(w));
		++g_stats.infos_out;
		return false;
	}
	if (!mgr().is_live_client())
		return false; // loading the world: healed by the resync at live
	if (applying_this(Kind::Info, info_id))
		return false;
	if (applying_any())
	{
		++g_stats.derived_dropped;
		return true;
	}
	if (g_hold_infos || g_local_infos.count(info_id))
	{
		// Held by a condlist (or one of ours): applied here only, never sent to the host.
		if (give)
			g_local_infos.insert(info_id);
		else
			g_local_infos.erase(info_id);
		++g_infos_held;
		return false;
	}
	const auto key = std::make_pair(op, info_id);
	const u64 now = mgr().now_ms();
	auto it = g_info_requests.find(key);
	if (it != g_info_requests.end() && now - it->second < kRequestRetryMs)
		return true; // already asked (TransferInfo also echoes through the local server)
	g_info_requests[key] = now;
	Writer w(64);
	w.u8(op);
	w.str(cs(info_id));
	push_out(Msg::InfoPortion, std::move(w));
	++g_stats.requests_out;
	return true;
}
bool hold_infos(bool on)
{
	if (!on)
	{
		g_hold_infos = false;
		return true;
	}
	if (!mgr().is_live_client())
		return false; // the host's infos are the story; a loading client has nothing to hold
	g_hold_infos = true;
	return true;
}

} // namespace story
} // namespace coop
