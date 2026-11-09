#include "stdafx.h"
#include "pch_script.h"
// F3a split levels: the transfers of ownership in the engine (plan §1.7), between the world server and a map server.
//
// World server: the Keeper (xrCoop/coop_levels_keeper.h) over the bridges of the map servers. A map server's half comes as LevelXferUp,
// the world server's goes as LevelXferDown; after every packet the changes the keeper reports go into ALife (coop_levels_alife.cpp).
// Every four seconds a map server hears where the squads the world server simulates offline stand on or off its level (EntitySwitch
// offline, the usual message): its copy of them is otherwise as old as its world, and the switch manager judges by it.
//
// Map server: a Holder of the core. The switch manager asks xfer_stock_rules for each creature it looks at: a creature near a player
// is asked for and stays offline until it is granted; the grant's state goes into its copy, and from then the stock rules bring it
// online and take it offline. An object that goes offline here goes back to the world server with its state; it stays its own until
// the world server's Ack. While it holds them the state of what is online goes up every few seconds, so that the world's save and a
// map server that vanishes lose seconds at most.
//
// What happens on its level to objects that are not transferred goes up as it happens (LevelEntity): who holds an item, who died,
// what was destroyed, what it made under its own ids (xrCoop/coop_levels_keeper.h leader_ids_of). What the binders of its static objects
// keep goes up when it changed, the last time at the end of the hand-over (xrCoop/coop_levels_logic.h).
//
// The auditor (plan §1.7): every 10 s it sends the hash of what it writes; a bucket that differs from the ledger is asked for in
// full and goes through xfer::audit and heal, with the loud log line. Console coop_levels_audit asks at once.
//
// Not done yet (docs/SPLIT_LEVELS.md §7a): the journal in the save (requirement 2): marks are not saved, so a host's load takes
// everything back to its last streamed state, each object exactly once.
#include "coop_levels.h"
#include "coop_alife.h"
#include "coop_checkpoint_sources.h"
#include "coop_dialog.h"
#include "coop_entities.h"
#include "coop_join.h"
#include "coop_manager.h"
#include "xrCoop/coop_soft_move.h"
#include "coop_mapserver.h"
#include "coop_money.h"
#include "coop_net.h"
#include "coop_survival.h"
#include "coop_world.h"
#include "xrCoop/coop_levels_keeper.h"

#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "GameObject.h"
#include "Level.h"
#include "PhysicsShellHolder.h"
#include "script_engine.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <memory>
#include <set>

namespace coop
{
namespace levels
{
namespace
{
constexpr xfer::Peer kSelf = 1; // what a Holder calls itself: the wire carries no peers, the session knows who sent what
constexpr u32 kStreamEveryMs = 3000; // the state of each object he holds goes up this often
constexpr u32 kStreamPerFrame = 8; // ... this many in one frame at most
constexpr u32 kRefusedMs = 10000; // a creature the world server refused is not asked for again before this
constexpr u32 kFeedEveryMs = 4000; // world server: the squads' places to a map server, this often
constexpr u32 kBodyEveryMs = 1000; // the players' places in the world server's world, this often
constexpr u32 kBodyFreshMs = 5000; // what it said of a body counts this long
constexpr u32 kFeedPerRound = 64; // ... at most this many squads to one map server in a round
constexpr u32 kReserveEveryMs = 5000; // world server: the map servers' ids are kept out of its generator again, this often
constexpr u32 kAuditEveryMs = 10000; // a map server's hash of what it holds, this often (plan §1.7)
constexpr u32 kScheduledPerRound = 8; // a map server: offline objects it holds that get their stock update in one ALife update
constexpr u32 kJournalChunk = 0xC0C0; // of the ALife save; the stock reader looks for its own chunks and never sees it

struct State
{
	// world server
	Keeper keeper;
	u64 fed_at = 0;
	std::map<PeerId, std::map<u16, u16>> fed; // bridge -> squad -> the game vertex it was told last
	u32 taken = 0, returned = 0, refused = 0, late = 0;
	u32 audits = 0, audit_rounds = 0; // LevelResync sent, reports heard
	u32 reserved_world = 0; // join::world_generation() the map servers' ids were kept out of
	u64 reserve_next = 0;
	std::string journal; // of the save just loaded (kept through a session start: a hosted save is loaded first)
	struct Body
	{
		BodyFacts facts;
		u64 at = 0;
	};
	std::map<u16, Body> bodies; // what the map servers say of the bodies of their players
	u32 body_reports = 0, body_refused = 0; // LevelEntity State heard, and thrown away
	u32 spawns_refused = 0; // LevelEntity Spawn thrown away: those objects stand on their level alone
	std::map<LevelId, u64> logic_at; // when each level last said what its objects keep
	u32 logic_taken = 0, logic_refused = 0;
	// map server
	std::unique_ptr<xfer::Holder> holder;
	xrCriticalSection lock; // the vote may run on the game thread's ALife update
	xr_vector<u16> asks; // creatures the switch manager wants online
	xr_vector<u16> gives; // objects it took offline here, or that stayed offline far from him
	xr_vector<u16> gone; // objects that left the world here
	struct Up
	{
		wire::LevelEntityHead::Op op;
		u16 a; // the object (this machine's numbering)
		u16 b; // its new holder, its killer
	};
	xr_vector<Up> up; // what happened to the objects of his level, in order
	u32 holder_world = 0; // join::world_generation() of g.holder: what the old world queued means nothing in the next
	u32 id_next = 0; // the next id of his slice to look at
	bool ids_out = false; // his slice was empty once (logged once)
	std::map<u16, u64> refused_until;
	bool handing_back = false; // it stops: nothing comes online here any more
	bool quiet = false; // the world server is away (the host's move)
	std::set<u16> settling; // granted, its stock update has not run yet: that update says whether it may come online at all
	u32 scheduled_at = 0; // where the last round of xfer_scheduled stopped
	u64 stream_next = 0;
	u32 stream_at = 0;
	u64 body_next = 0;
	u64 audit_next = 0;
	logic::Book book; // what the binders of its level would save
	logic::Round round;
	bool looking = false; // what a binder saves now is a look
	bool logic_over = false; // the last look is done: no binder runs any more
	u32 logic_told = 0;
	u64 logic_told_bytes = 0;
	u32 creates = 0, removes = 0, refusals = 0;
	// since the last line in the log: what the switch manager asked here (coop_mapserver_stats says where a level stays empty)
	struct Seen
	{
		u32 votes = 0, no_holder = 0, creatures = 0, in_reach = 0, asked = 0, sent = 0;
		float nearest = flt_max;
	} seen;
} g;

void send_down(const std::vector<xfer::Packet>& out)
{
	for (const xfer::Packet& p : out)
	{
		const u32 peer = g.keeper.session_of(p.to);
		wire::LevelXferDownMsg m;
		if (peer == Keeper::kNoSession || !down_of(p, m))
			continue; // a Drop to a map server that is gone is best effort
		Writer w(16 + m.cargo.size());
		if (m.write(w))
			net::host_send(PeerId(peer), Msg::LevelXferDown, w);
		else
			::Msg("! [coop] levels: the state of object %u is over a packet, it is not sent", u32(m.id));
	}
}

void host_apply_changes()
{
	for (Keeper::Change& c : g.keeper.take_changes())
		switch (c.kind)
		{
		case Keeper::Kind::Mark:
			host_mark(c.id);
			++g.taken;
			break;
		case Keeper::Kind::Unmark:
			host_unmark(c.id, c.state);
			++g.returned;
			break;
		case Keeper::Kind::State: host_apply_state(c.id, c.state); break;
		case Keeper::Kind::Gone: host_gone(c.id); break;
		}
}

void host_on_xfer_up(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return;
	wire::LevelXferUpMsg m;
	if (!m.read(r))
		return;
	if (!g.keeper.connected(from))
	{
		++g.late; // it is on its way out: what was on its way counts for nothing
		return;
	}
	const xfer::Packet p = packet_of(m);
	std::vector<u8> fresh;
	bool may = false;
	if (p.op == xfer::Op::TakeReq)
	{
		CSE_ALifeDynamicObject* e = (world_up() && ai().get_alife()) ? ai().alife().objects().object(m.id, true) : nullptr;
		may = e && transfers(e) && host_may_give(e, mapserver::level_of_bridge(from));
		if (may)
			fresh = state_with_logic(e);
		else
			++g.refused;
	}
	send_down(g.keeper.on_up(from, p, may ? &fresh : nullptr, mgr().now_ms()));
	host_apply_changes();
}

void send_up(const std::vector<xfer::Packet>& out)
{
	for (const xfer::Packet& p : out)
	{
		wire::LevelXferUpMsg m;
		if (!up_of(p, m))
			continue;
		Writer w(16 + m.cargo.size());
		if (m.write(w))
			net::client_send(Msg::LevelXferUp, w);
		else
			::Msg("! [coop] levels: the state of object %u is over a packet, it is not sent", u32(m.id));
	}
}

void client_on_xfer_down(PeerId from, Reader& r)
{
	if (from != kHostPeer || !is_map_server())
		return;
	wire::LevelXferDownMsg m;
	if (!m.read(r))
		return;
	xrCriticalSectionGuard guard(g.lock);
	if (g.holder)
		send_up(g.holder->on_packet(packet_of(m, kSelf), mgr().now_ms()));
}

// The objects the host gave or took back.
void client_events(u64 now)
{
	for (xfer::Holder::Event& ev : g.holder->take_events())
	{
		const u16 id = join::to_local(ev.id);
		switch (ev.kind)
		{
		case xfer::Holder::Event::Kind::Create:
			++g.creates;
			if (!client_apply_state(id, ev.state) || level_of_object(id) != my_level())
				send_up(g.holder->want_offline(ev.id, ev.state, now)); // it walked off this level on the world server: not for this one
			else
				g.settling.insert(ev.id);
			break;
		case xfer::Holder::Event::Kind::Remove:
		{
			++g.removes;
			g.settling.erase(ev.id);
			CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
			if (e && e->m_bOnline)
			{
				const entities::ClientApply apply;
				alife_rw().switch_offline(e);
			}
			luabind::functor<void> left;
			if (e && ai().script_engine().functor("coop_levels_brain.left", left))
				left(id); // the smarts of this level keep no job for it
			break;
		}
		case xfer::Holder::Event::Kind::Refused:
			++g.refusals;
			g.refused_until[ev.id] = now + kRefusedMs;
			break;
		}
	}
}

// What the switch manager found since the last frame.
void client_requests(u64 now)
{
	for (u16 id : g.asks)
	{
		const std::vector<xfer::Packet> out = g.holder->want_online(id, now);
		g.seen.sent += u32(out.size());
		send_up(out);
	}
	g.asks.clear();
	for (u16 id : g.gives)
	{
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(id), true);
		if (e && g.holder->authors(id) && !g.holder->frozen(id))
			send_up(g.holder->want_offline(id, state_with_logic(e), now));
	}
	g.gives.clear();
	g.gone.clear();
}

// What happened on its level, on the one reliable lane, so that a spawn comes before what follows it.
void client_flush_up()
{
	using Head = wire::LevelEntityHead;
	for (const State::Up& u : g.up)
	{
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(u.a, true);
		Writer w(64);
		Head head;
		head.op = u8(u.op);
		switch (u.op)
		{
		case Head::Parent:
		{
			// Where it stands now, not what the hook saw: a holder that goes offline rejects every child on the way and takes
			// it back without a word (add_offline_impl), as the host's own flush nets out (events::host_flush).
			const u16 holder = e ? e->ID_Parent : u.b;
			head.write(w);
			w.u16(join::to_host(u.a));
			w.u16(holder == 0xffff ? u16(0xffff) : join::to_host(holder));
			const Fvector pos = e ? e->o_Position : Fvector().set(0.f, 0.f, 0.f);
			float p[3] = {pos.x, pos.y, pos.z};
			w.vec3(p);
			break;
		}
		case Head::Died:
		{
			head.write(w);
			w.u16(join::to_host(u.a));
			w.u16(u.b == 0xffff ? u16(0xffff) : join::to_host(u.b));
			if (e)
			{
				const std::vector<u8> st = state_of(e);
				w.bytes(st.data(), st.size());
			}
			break;
		}
		case Head::Destroy:
			if (g.holder->authors(u.a))
			{
				send_up(g.holder->destroyed(u.a, mgr().now_ms()));
				continue;
			}
			head.write(w);
			w.u16(join::to_host(u.a));
			break;
		case Head::Spawn:
			if (!e)
				continue; // made and gone within a frame: nobody needs to hear of it
			head.write(w);
			if (!entities::write_entity(w, e))
				continue;
			break;
		case Head::Tent:
			if (!e)
				continue;
			head.write(w);
			w.u16(join::to_host(u.a));
			w.u16(join::to_host(u.b));
			break;
		case Head::Stash:
			head.write(w);
			w.u16(join::to_host(u.a));
			w.u8(u8(u.b));
			break;
		default: continue;
		}
		net::client_send(Msg::LevelEntity, w);
		if (u.op == Head::Spawn && transfers(e))
			send_up(g.holder->spawned(join::to_host(u.a), state_of(e), mgr().now_ms())); // the host must not simulate it too
	}
	g.up.clear();
}

// The state of what it holds, a few objects a frame.
void client_stream(u64 now)
{
	if (now < g.stream_next)
		return;
	const std::vector<u16> ids = g.holder->ids();
	u32 sent = 0;
	while (g.stream_at < ids.size() && sent < kStreamPerFrame)
	{
		const u16 id = ids[g.stream_at++];
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(id), true);
		if (e && e->m_bOnline && g.holder->authors(id) && !g.holder->frozen(id) && !g.holder->busy(id))
		{
			send_up(g.holder->changed(id, state_of(e), now));
			++sent;
		}
	}
	if (g.stream_at >= ids.size())
	{
		g.stream_at = 0;
		g.stream_next = now + kStreamEveryMs;
	}
}

// The players' places in the world server's world: what its save and their next join read; what it cannot see of a body, the rest and
// the camp ask it. Protocol 21: u16 proxy, the placement, LevelBodyFacts.
void client_body(u64 now)
{
	if (now < g.body_next)
		return;
	g.body_next = now + kBodyEveryMs;
	checkpoints::sources::map_server_scan(now);
	for (const auto& it : mapserver::trunk_peers().on())
	{
		const CSE_ALifeDynamicObject* e = ai().alife().objects().object(it.second.proxy, true);
		if (!e || !e->m_bOnline)
			continue;
		Writer w(48);
		wire::LevelEntityHead head;
		head.op = wire::LevelEntityHead::State;
		head.write(w);
		w.u16(it.second.proxy);
		float pos[3] = {e->o_Position.x, e->o_Position.y, e->o_Position.z};
		float ang[3] = {e->o_Angle.x, e->o_Angle.y, e->o_Angle.z};
		w.vec3(pos);
		w.vec3(ang);
		w.u32(e->m_tNodeID);
		w.u16(e->m_tGraphID);
		wire::LevelBodyFacts facts;
		const CGameObject* body = smart_cast<CGameObject*>(Level().Objects.net_Find(it.second.proxy));
		facts.fight = body && checkpoints::sources::in_combat(body) ? 1 : 0;
		facts.talking = dialog::host_lock_of(it.first) != 0xffff ? 1 : 0;
		facts.place = u8(checkpoints::sources::rest_place(it.second.proxy, facts.source));
		facts.write(w);
		net::client_send(Msg::LevelEntity, w, Lane::Unreliable);
	}
}

// A look: time_global() stands still, so that what a binder writes of it (xr_logic: activation_time - time_global()) says nothing, and an
// object with no logic writes nothing of it at all (coop_npc_logic.script).
struct Look
{
	const u32 was = Device.dwTimeGlobal;
	Look()
	{
		g.looking = true;
		Device.dwTimeGlobal = logic::kStillClock;
	}
	~Look()
	{
		Device.dwTimeGlobal = was;
		g.looking = false;
	}
};

// What goes in the object's entity as client_data: its save() and its binder's. Not the virtual net_Save: what a physics object writes after
// this chunk is where its bones are, and CSE_Abstract::load would not take that as client_data either.
bool saved_by(CGameObject* object, NET_Packet& p, const u8*& body, size_t& size)
{
	p.write_start();
	object->CGameObject::net_Save(p);
	return logic::chunk_body(p.B.data, p.B.count, body, size);
}

u64 look_at(CGameObject* object)
{
	const Look look;
	NET_Packet p;
	const u8* body = nullptr;
	size_t size = 0;
	if (!saved_by(object, p, body, size))
		return 0;
	return logic::digest(body, size, smart_cast<CPhysicsShellHolder*>(object) ? 1 : 0); // it begins with whether its shell sleeps
}

// One object of the book: what it would save now goes up if it is not what the world server has.
void client_look(u16 id)
{
	CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	if (!object || object->getDestroy())
		return; // offline here, or gone: what was said of it stands
	const u64 seen = look_at(object);
	if (!g.book.changed(id, seen))
		return;
	g.book.told(id, seen);
	NET_Packet p; // the real clock: a load adds its own time_global() to what this says
	const u8* body = nullptr;
	size_t size = 0;
	if (!saved_by(object, p, body, size))
		return;
	wire::LevelLogicBody m;
	m.id = join::to_host(id);
	m.bytes.assign(body, body + size);
	wire::LevelEntityHead head;
	head.op = wire::LevelEntityHead::Logic;
	Writer w(16 + size);
	head.write(w);
	if (!m.write(w))
	{
		::Msg("! [coop] levels: what object %u ('%s') keeps is %u bytes, it stays here", u32(id), object->cName().c_str(), u32(size));
		return;
	}
	net::client_send(Msg::LevelEntity, w);
	++g.logic_told;
	g.logic_told_bytes += size;
}

// What changed since the last round, a few objects a frame: what the world server's saves hold of this level is that old at most.
void client_logic(u64 now)
{
	if (g.logic_over || !g.round.due(now) || !mapserver::server_leads())
		return;
	for (const u16 id : g.round.take(g.book.ids(), now))
		client_look(id);
}

void send_resync(PeerId to, u32 mask)
{
	wire::LevelResyncMsg m;
	m.mask = mask;
	Writer w(8);
	m.write(w);
	net::host_send(to, Msg::LevelResync, w);
	++g.audits;
}

void host_on_level_hash(PeerId from, Reader& r)
{
	wire::LevelHashMsg m;
	if (!mgr().is_host() || !m.read(r) || !g.keeper.connected(from))
		return;
	xfer::LevelHash his;
	his.n = m.n;
	his.set = m.set;
	std::copy(std::begin(m.bucket), std::end(m.bucket), std::begin(his.bucket));
	const u32 mask = g.keeper.differs(from, his);
	if (mask)
		send_resync(from, mask);
}

void host_on_report(PeerId from, Reader& r)
{
	wire::LevelReportBody m;
	if (!m.read(r))
		return;
	std::vector<xfer::Entry> reported;
	reported.reserve(m.rows.size());
	for (const wire::LevelReportBody::Row& row : m.rows)
		reported.push_back(Keeper::bare(xfer::Entry{row.id, row.gen, xfer::kNoParent, true, 0}));
	std::vector<xfer::Finding> found;
	send_down(g.keeper.audit(from, reported, m.buckets, found));
	host_apply_changes();
	++g.audit_rounds;
	for (const xfer::Finding& f : found)
		::Msg("%s", xfer::describe(f).c_str());
}

// The auditor's question: the hash of what it writes, on the lane of its transfers (R9: everything sent before it was heard).
void client_audit(u64 now)
{
	if (now < g.audit_next)
		return;
	g.audit_next = now + kAuditEveryMs;
	const xfer::LevelHash h = xfer::hash_of(Keeper::bare(g.holder->report()));
	wire::LevelHashMsg m;
	m.level = my_level();
	m.n = h.n;
	m.set = h.set;
	std::copy(std::begin(h.bucket), std::end(h.bucket), std::begin(m.bucket));
	Writer w(160);
	m.write(w);
	net::client_send(Msg::LevelHash, w);
}

void client_on_resync(PeerId from, Reader& r)
{
	wire::LevelResyncMsg m;
	if (from != kHostPeer || !is_map_server() || !m.read(r))
		return;
	xrCriticalSectionGuard guard(g.lock);
	if (!g.holder)
		return;
	wire::LevelReportBody body;
	body.buckets = m.mask;
	for (const xfer::Entry& e : g.holder->report())
		if (m.mask & (1u << xfer::bucket_of(e.id)))
		{
			wire::LevelReportBody::Row row;
			row.id = e.id;
			row.gen = e.gen;
			body.rows.push_back(row);
		}
	wire::LevelEntityHead head;
	head.op = wire::LevelEntityHead::Report;
	Writer w(16 + body.rows.size() * 6);
	head.write(w);
	if (body.write(w))
		net::client_send(Msg::LevelEntity, w);
}

void host_on_level_entity(PeerId from, Reader& r)
{
	using Head = wire::LevelEntityHead;
	if (!mgr().is_host() || !g.keeper.connected(from) || !world_up())
		return;
	Head head;
	if (!head.read(r))
		return;
	const LevelId level = mapserver::level_of_bridge(from);
	const EchoOf echo(level); // its players have heard this from it
	switch (head.op)
	{
	case Head::Report: host_on_report(from, r); return;
	case Head::Parent:
	{
		const u16 item = r.u16();
		const u16 parent = r.u16();
		float p[3];
		r.vec3(p);
		Fvector pos;
		pos.set(p[0], p[1], p[2]);
		if (r.ok() && _valid(pos))
			host_apply_parent(item, parent, pos);
		return;
	}
	case Head::Died:
	{
		const u16 victim = r.u16();
		const u16 killer = r.u16();
		std::vector<u8> st(r.remaining());
		if (!st.empty())
			r.bytes(st.data(), st.size());
		if (r.ok())
			host_apply_death(victim, killer, st);
		return;
	}
	case Head::Destroy:
	{
		const u16 id = r.u16();
		if (r.ok() && !g.keeper.away(id))
			host_apply_release(id, level);
		return;
	}
	case Head::Spawn:
	{
		// Its id: within its slice, kept out of this generator: it is freed here for this object alone.
		Reader look(r.cursor(), r.remaining());
		const u16 id = look.u16();
		const u16 first = leader_ids_of(mapserver::bridge_index(from));
		xrServer& server = alife_rw().server();
		const bool taken = ai().alife().objects().object(id, true) != nullptr;
		if (!first || id < first || id >= first + kLeaderIdsEach || taken)
		{
			if (++g.spawns_refused <= 8 || g.spawns_refused % 100 == 0)
				::Msg("! [coop] levels: object %u of map server %u refused (%u so far), it stays on that level alone: %s", u32(id), mapserver::bridge_index(from),
				    g.spawns_refused, taken ? "this world has an object under that id" : "the id is not of its slice, the slice ran out");
			return;
		}
		if (!server.IsFreeID(id) && !server.ID_to_entity(id))
			server.FreeID(id, Device.TimerAsync());
		entities::host_spawn_from_leader(r);
		host_bind_squads(id);
		return;
	}
	case Head::Money:
	{
		const u32 peer = r.u32();
		const s64 delta = s64(r.u64());
		const bool trade = r.u8() != 0;
		if (r.ok() && level_of_player(PeerId(peer)) == level)
			money::host_adjust(PeerId(peer), delta, trade ? "trade" : "map server");
		return;
	}
	case Head::Tent:
	{
		const u16 tent = r.u16();
		const u16 character = r.u16();
		if (r.ok() && level_of_player(join::peer_of_proxy(character)) == level)
			checkpoints::sources::host_tent_up(tent, character);
		return;
	}
	case Head::Stash:
	{
		const u16 character = r.u16();
		const bool things_stay = r.u8() != 0;
		if (r.ok() && level_of_player(join::peer_of_proxy(character)) == level)
			survival::host_stash_over(character, things_stay);
		return;
	}
	case Head::Logic:
	{
		wire::LevelLogicBody m;
		if (!m.read(r))
			return;
		if (host_apply_logic(m.id, m.bytes, level))
		{
			++g.logic_taken;
			g.logic_at[level] = mgr().now_ms();
		}
		else
			++g.logic_refused;
		return;
	}
	case Head::State: break;
	default: return;
	}
	const u16 proxy = r.u16();
	float pos[3], ang[3];
	r.vec3(pos);
	r.vec3(ang);
	const u32 node = r.u32();
	const u16 graph = r.u16();
	wire::LevelBodyFacts facts;
	facts.read(r); // a short one fails r below
	CSE_ALifeDynamicObject* e = (world_up() && r.ok()) ? ai().alife().objects().object(proxy, true) : nullptr;
	// a report that crossed his move: his seat says where he is, not the map server that has not heard yet
	++g.body_reports;
	if (!r.ok() || !e || e->m_bOnline || !world::is_player_character(e) || level_of_vertex(graph) != level ||
	    level_of_player(join::peer_of_proxy(proxy)) != level || !_valid(pos[0]) || !_valid(pos[1]) || !_valid(pos[2]))
	{
		if (++g.body_refused <= 3)
			::Msg("! [coop] levels: a body report of object %u refused: read %d, here %d, online %d, level %u of %u", u32(proxy), r.ok() ? 1 : 0,
			    e ? 1 : 0, (e && e->m_bOnline) ? 1 : 0, u32(level_of_vertex(graph)), u32(level));
		return;
	}
	Fvector p, a;
	p.set(pos[0], pos[1], pos[2]);
	a.set(ang[0], ang[1], ang[2]);
	entities::place_offline(e, p, a, node, graph);
	State::Body& body = g.bodies[proxy];
	body.facts.fight = facts.fight != 0;
	body.facts.talking = facts.talking != 0;
	body.facts.place = facts.place;
	body.facts.source = facts.source;
	body.at = mgr().now_ms();
}

// Squads on the move: a map server hears where they stand when it changed, on its level or leaving it. Marked ones are its own already.
void host_feed(u64 now)
{
	if (now - g.fed_at < kFeedEveryMs)
		return;
	g.fed_at = now;
	for (const PeerId bridge : mgr().session().bridges())
	{
		if (!g.keeper.connected(bridge))
			continue;
		const LevelId level = mapserver::level_of_bridge(bridge);
		std::map<u16, u16>& fed = g.fed[bridge];
		u32 sent = 0;
		for (const auto& it : ai().alife().groups().objects())
		{
			CSE_ALifeOnlineOfflineGroup* squad = it.second;
			if (sent >= kFeedPerRound)
				break;
			if (!squad || squad->m_bOnline || g.keeper.away(squad->ID) || squad->squad_members().empty())
				continue;
			const u16 vertex = squad->m_tGraphID;
			const auto known = fed.find(squad->ID);
			const LevelId was = known == fed.end() ? level : level_of_vertex(known->second);
			if ((known != fed.end() && known->second == vertex) || (level_of_vertex(vertex) != level && was != level) ||
			    (known == fed.end() && level_of_vertex(vertex) != level))
				continue;
			fed[squad->ID] = vertex;
			entities::host_send_place(bridge, squad);
			++sent;
		}
	}
}
} // namespace

std::vector<u16> squads_at(PeerId bridge)
{
	std::vector<u16> out;
	if (!mgr().is_host() || !g.keeper.connected(bridge) || !ai().get_alife())
		return out;
	const LevelId level = mapserver::level_of_bridge(bridge);
	for (const auto& it : ai().alife().groups().objects())
		if (g.keeper.away(it.second->ID) && level_of_vertex(it.second->m_tGraphID) == level)
			out.push_back(it.second->ID);
	return out;
}

bool body_facts(u16 proxy, BodyFacts& out)
{
	const auto it = g.bodies.find(proxy);
	if (it == g.bodies.end() || mgr().now_ms() - it->second.at > kBodyFreshMs)
		return false;
	out = it->second.facts;
	return true;
}

void xfer_install_server(Session& s)
{
	s.on(Msg::LevelXferUp, &host_on_xfer_up);
	s.on(Msg::LevelEntity, &host_on_level_entity);
	s.on(Msg::LevelHash, &host_on_level_hash);
}

void xfer_install_client(Endpoint& e)
{
	e.on(Msg::LevelXferDown, &client_on_xfer_down);
	e.on(Msg::LevelResync, &client_on_resync);
}

void xfer_audit_now()
{
	if (mgr().is_host())
		for (const PeerId bridge : mgr().session().bridges())
			if (g.keeper.connected(bridge))
				send_resync(bridge, xfer::kAllBuckets);
	g.audit_next = 0;
	xfer_print_status();
}

void xfer_reset()
{
	g.keeper.restart();
	g.fed.clear();
	g.fed_at = 0;
	g.bodies.clear();
	g.logic_at.clear();
	g.logic_taken = 0;
	g.logic_refused = 0;
	g.book.clear();
	g.round = logic::Round();
	g.logic_over = false;
	g.logic_told = 0;
	g.logic_told_bytes = 0;
	xrCriticalSectionGuard guard(g.lock);
	g.holder.reset();
	g.asks.clear();
	g.gives.clear();
	g.gone.clear();
	g.refused_until.clear();
	g.handing_back = false;
	g.quiet = false;
	g.settling.clear();
	g.scheduled_at = 0;
	g.up.clear();
	g.id_next = 0;
	g.ids_out = false;
	g.reserved_world = 0;
	g.stream_next = 0;
	g.stream_at = 0;
	g.body_next = 0;
	g.audit_next = 0;
}

void xfer_host_carry_over()
{
	g.journal.clear();
	for (const u16 id : g.keeper.marks())
		host_mark_in_new_world(id);
	::Msg("[coop] soft move: %u object(s) stay with the map servers", u32(g.keeper.marked()));
}

void xfer_quiet(bool quiet)
{
	g.quiet = quiet;
	g.stream_next = 0;
	g.body_next = 0;
	g.audit_next = 0;
}

void xfer_host_connect(PeerId peer)
{
	if (g.keeper.connected(peer))
		return;
	g.keeper.connect(peer);
	g.fed.erase(peer);
}

void xfer_host_disconnect(PeerId peer)
{
	if (!g.keeper.connected(peer))
		return;
	const size_t held = g.keeper.held_by(peer);
	g.keeper.disconnect(peer, mgr().now_ms());
	host_apply_changes();
	g.fed.erase(peer);
	::Msg("[coop] levels: map server %u leads no level now, %u object(s) came home", mapserver::bridge_index(peer), u32(held));
}

void xfer_save(IWriter& out)
{
	if (!mgr().is_host() || !split())
		return;
	const std::string text = g.keeper.journal();
	out.open_chunk(kJournalChunk);
	out.w(text.data(), u32(text.size()));
	out.close_chunk();
}

void xfer_load(IReader& in)
{
	g.journal.clear();
	const size_t size = in.find_chunk(kJournalChunk);
	if (!size || size > xfer::kJournalMaxBytes)
		return;
	g.journal.resize(size);
	in.r(&g.journal[0], u32(size));
}

void xfer_host_frame()
{
	if (!world_up() || !ai().get_alife())
		return;
	if (!g.journal.empty())
	{
		const bool ok = g.keeper.resume(g.journal);
		if (!ok)
			::Msg("! [coop] levels: the transfer journal of this save is damaged, the objects stay at their saved state");
		g.journal.clear();
		host_apply_changes();
		g.fed.clear();
	}
	const u64 now = mgr().now_ms();
	if (g.reserved_world != join::world_generation() || now >= g.reserve_next)
	{
		// again every few seconds: what a map server made under its ids and destroyed goes back to this generator with the entity
		host_reserve_leader_ids(g.reserved_world != join::world_generation());
		g.reserved_world = join::world_generation();
		g.reserve_next = now + kReserveEveryMs;
	}
	if (softmove::book_waits(join::host_soft_window(), mgr().session().soft_replaying()))
		return; // the book waits with the map servers: nothing of it can be said now, and the time that passes is nobody's fault
	send_down(g.keeper.tick(now));
	host_apply_changes();
	host_feed(now);
}

void xfer_client_frame()
{
	if (!world_up() || !ai().get_alife())
		return;
	const u64 now = mgr().now_ms();
	{
		xrCriticalSectionGuard guard(g.lock);
		if (!g.holder || g.holder_world != join::world_generation())
		{
			g.holder = std::make_unique<xfer::Holder>(kSelf);
			g.holder_world = join::world_generation();
			g.asks.clear();
			g.gives.clear();
			g.up.clear();
			g.refused_until.clear();
			g.settling.clear();
		}
		client_requests(now);
		client_flush_up();
		send_up(g.holder->tick(now));
		client_events(now);
		if (g.quiet)
			return; // it would all wait in the world server's inbox: a state that is old by then, in tens of thousands
		client_stream(now);
		client_body(now);
		client_audit(now);
	}
	client_logic(now); // outside the lock: this is Lua, and what it does comes back through the hooks below
}

bool logic_may_update(CGameObject* object)
{
	if (!is_map_server())
		return true;
	if (g.logic_over)
		return false;
	const u16 id = object->ID();
	if (g.book.met(id) || !ai().get_alife())
		return true;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	bool kept = false;
	if (e)
	{
		const logic::Kind kind = logic_kind(e);
		const bool binder = static_cast<CScriptBinder*>(object)->object() != nullptr;
		kept = logic::watched(kind, binder, logic::stays_with_level(kind) && binder && object->net_SaveRelevant());
	}
	g.book.meet(id, kept, kept ? look_at(object) : 0);
	return true;
}

void logic_hand_over()
{
	if (!is_map_server() || g.logic_over)
		return;
	if (world_up())
		for (const u16 id : g.book.ids())
			client_look(id);
	g.logic_over = true;
	::Msg("[coop] levels: what the objects of this level keep: %u said of %u watched, %llu bytes", g.logic_told, u32(g.book.watched()), g.logic_told_bytes);
}

bool logic_looking() { return g.looking; }

bool logic_came_since(LevelId level, u64 at)
{
	const auto it = g.logic_at.find(level);
	return logic::save_is_stale(at, it != g.logic_at.end(), it != g.logic_at.end() ? it->second : 0);
}

bool online_at_server(u16 id)
{
	if (!mgr().is_host() || !g.keeper.marked())
		return false;
	if (g.keeper.away(id))
		return true;
	const CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
	const CSE_ALifeMonsterAbstract* m = e ? smart_cast<const CSE_ALifeMonsterAbstract*>(e) : nullptr;
	return m && m->m_group_id != 0xffff && g.keeper.away(m->m_group_id);
}

bool xfer_stock_rules(u16 local_id)
{
	CSE_ALifeDynamicObject* e = (ai().get_alife()) ? ai().alife().objects().object(local_id, true) : nullptr;
	if (!e)
		return true;
	if (world::is_player_character(e))
		return join::host_player_in_world(e->ID); // a player of this level, in the world: the host's code of a level drives his character
	const u16 id = join::to_host(unit_of(e)); // a member of a squad that came over the wire goes with that squad
	xrCriticalSectionGuard guard(g.lock);
	++g.seen.votes;
	if (!g.holder || g.holder_world != join::world_generation() || g.handing_back)
	{
		++g.seen.no_holder;
		return !transfers(e);
	}
	const bool held = g.holder->authors(id);
	const bool frozen = held && (g.holder->frozen(id) || g.settling.count(id) != 0);
	const float distance = observer_distance(e->o_Position);
	const bool in_reach = distance <= ai().alife().online_distance() || !e->can_switch_offline();
	const LeadAsk a = lead_vote(transfers(e), held, frozen, in_reach);
	if (transfers(e))
	{
		++g.seen.creatures;
		g.seen.in_reach += in_reach ? 1 : 0;
		g.seen.nearest = _min(g.seen.nearest, distance);
	}
	if (a.ask)
	{
		++g.seen.asked;
		const auto it = g.refused_until.find(id);
		if (it == g.refused_until.end() || mgr().now_ms() >= it->second)
			g.asks.push_back(id);
	}
	if (held && !frozen && !e->m_bOnline && distance > ai().alife().offline_distance() && e->can_switch_offline())
		g.gives.push_back(id); // granted, but the player walked away before it came online: it goes home
	return a.vote == LeadVote::Stock;
}

void xfer_scheduled()
{
	if (!is_map_server() || !world_up() || !ai().get_alife())
		return;
	xr_vector<u16> due;
	{
		xrCriticalSectionGuard guard(g.lock);
		if (!g.holder || g.holder_world != join::world_generation() || g.handing_back)
			return;
		const std::vector<u16> ids = g.holder->ids();
		for (size_t i = 0; i < ids.size() && due.size() < kScheduledPerRound; ++i)
		{
			const u16 id = ids[(g.scheduled_at + i) % ids.size()];
			CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(id), true);
			const CSE_ALifeMonsterAbstract* m = e ? smart_cast<CSE_ALifeMonsterAbstract*>(e) : nullptr;
			if (m && m->m_group_id != 0xffff)
				continue; // what it spawned as a squad is held member by member: a member goes with its squad, as in the stock scheduler
			if (e && !e->m_bOnline && g.holder->authors(id) && !g.holder->frozen(id))
				due.push_back(id);
		}
		g.scheduled_at = ids.empty() ? 0 : u32((g.scheduled_at + due.size()) % ids.size());
	}
	for (const u16 id : due)
	{
		// outside the lock: this is Lua, and whatever it does to the object comes back through the hooks above
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(id), true);
		CSE_ALifeSchedulable* s = e ? smart_cast<CSE_ALifeSchedulable*>(e) : nullptr;
		if (s && !e->m_bOnline && s->need_update(e))
			s->update();
		e = ai().alife().objects().object(join::to_local(id), true);
		xrCriticalSectionGuard guard(g.lock);
		g.settling.erase(id);
		if (e && !e->m_bOnline && level_of_vertex(e->m_tGraphID) != my_level())
			g.gives.push_back(id); // it walked off this level while it stayed offline: the switch manager will never look at it again
	}
}

void xfer_on_switched_offline(u16 local_id)
{
	if (!is_map_server())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.gives.push_back(join::to_host(local_id));
}

void xfer_on_released(u16 local_id)
{
	if (!is_map_server() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Destroy, local_id, 0xffff});
}

void up_parent(u16 item, u16 new_parent)
{
	if (!is_map_server() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Parent, item, new_parent});
}

void up_died(u16 victim, u16 killer)
{
	if (!is_map_server() || entities::client_applying() || victim == 0)
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Died, victim, killer});
}

void up_spawned(u16 id)
{
	if (!is_map_server() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Spawn, id, 0xffff});
}

void up_money(PeerId peer, s64 delta, bool trade)
{
	if (!is_map_server())
		return;
	Writer w(16);
	wire::LevelEntityHead head;
	head.op = wire::LevelEntityHead::Money;
	head.write(w);
	w.u32(peer);
	w.u64(u64(delta));
	w.u8(trade ? 1 : 0);
	net::client_send(Msg::LevelEntity, w);
}

void up_tent(u16 tent, u16 character)
{
	if (!is_map_server())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Tent, tent, character});
}

void up_stash(u16 character, bool things_stay)
{
	if (!is_map_server())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Stash, character, u16(things_stay ? 1 : 0)});
}

bool leader_takes_ids() { return is_map_server(); }

u16 leader_free_id()
{
	const u16 first = leader_ids_of(mapserver::bridge_index(mgr().self()));
	const xrServer* server = (first && g_pGameLevel) ? Level().Server : nullptr;
	if (!server)
		return 0xffff;
	for (u32 i = 0; i < kLeaderIdsEach; ++i)
	{
		const u32 at = (g.id_next + i) % kLeaderIdsEach;
		if (server->IsFreeID(u16(first + at)))
		{
			g.id_next = at + 1;
			return u16(first + at);
		}
	}
	if (!g.ids_out)
	{
		g.ids_out = true;
		::Msg("! [coop] levels: every id of this map server's slice is in use, the next object takes one of the world server's");
	}
	return 0xffff;
}

u32 xfer_held()
{
	xrCriticalSectionGuard guard(g.lock);
	return g.holder ? u32(g.holder->ids().size()) : 0u;
}

void xfer_hand_back(u64 now)
{
	if (!is_map_server() || !ai().get_alife())
		return;
	xrCriticalSectionGuard guard(g.lock);
	if (!g.holder)
		return;
	g.handing_back = true;
	for (u16 id : g.holder->ids())
	{
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(id), true);
		if (!e)
			continue;
		if (e->m_bOnline)
		{
			const entities::ClientApply apply;
			alife_rw().switch_offline(e);
		}
		if (g.holder->authors(id) && !g.holder->frozen(id))
			send_up(g.holder->want_offline(id, state_with_logic(e), now));
	}
}

void xfer_print_status()
{
	if (mgr().is_host())
	{
		const xfer::Ledger::Counters c = g.keeper.ledger().counters();
		::Msg("[coop] levels: %u object(s) online at map servers, taken %u, returned %u, refused %u, late %u | xfer in flight %llu, "
		      "retries %llu, completed %llu, rolled back %llu, resyncs %u asked %u heard, violations %llu",
		    u32(g.keeper.marked()), g.taken, g.returned, g.refused, g.late, c.in_flight, c.retries, c.completed, c.rolled_back,
		    g.audits, g.audit_rounds, c.violations);
		::Msg("[coop] levels: body reports %u, refused %u, %u bod(ies) known", g.body_reports, g.body_refused, u32(g.bodies.size()));
		::Msg("[coop] levels: objects of map servers refused %u", g.spawns_refused);
		::Msg("[coop] levels: what static objects keep: %u taken, %u refused", g.logic_taken, g.logic_refused);
		return;
	}
	::Msg("[coop] levels: what the objects of this level keep: %u said of %u watched, %llu bytes", g.logic_told, u32(g.book.watched()), g.logic_told_bytes);
	xrCriticalSectionGuard guard(g.lock);
	if (g.holder)
		::Msg("[coop] levels: holding %u object(s), %s, granted %u, taken back %u, refused %u, retries %llu",
		    u32(g.holder->ids().size()), g.holder->idle() ? "idle" : "busy", g.creates, g.removes, g.refusals, g.holder->retries());
	const u16 proxy = join::proxy_of_peer(1);
	const CSE_ALifeDynamicObject* player = (ai().get_alife() && proxy != 0xffff) ? ai().alife().objects().object(proxy, true) : nullptr;
	::Msg("[coop] levels: switch votes %u, no holder %u, creatures %u, near %u, nearest %.0f m of %.0f, asked %u, sent %u, holder world %u of %u, "
	      "player #1 object %u%s at (%.0f, %.0f, %.0f)",
	    g.seen.votes, g.seen.no_holder, g.seen.creatures, g.seen.in_reach, g.seen.nearest == flt_max ? -1.f : g.seen.nearest,
	    ai().get_alife() ? ai().alife().online_distance() : 0.f, g.seen.asked, g.seen.sent, g.holder_world, join::world_generation(), u32(proxy),
	    player ? "" : " (none)", player ? player->o_Position.x : 0.f, player ? player->o_Position.y : 0.f, player ? player->o_Position.z : 0.f);
	g.seen = State::Seen();
}
} // namespace levels
} // namespace coop
