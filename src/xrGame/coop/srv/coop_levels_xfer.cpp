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
#include "../coop_levels.h"
#include "../coop_alife.h"
#include "../coop_checkpoint_sources.h"
#include "../coop_dialog.h"
#include "../coop_entities.h"
#include "../coop_items.h"
#include "../coop_join.h"
#include "../coop_manager.h"
#include "coop_mapserver.h"
#include "coop_levels_alife.h" // R32 W2b: host_accept_print_status
#include "../coop_money.h"
#include "../coop_net.h"
#include "../coop_survival.h"
#include "../coop_world.h"
#include "xrCoop/srv/coop_levels_keeper.h"
#include "xrCoop/srv/coop_levels_audit.h"
#include "xrCoop/srv/coop_levels_journal.h" // R32 W3: the map server's journal, its batches and the world's Ack
#include "xrCoop/srv/coop_session_srv.h"
#include "xrCoop/coop_reconcile.h" // R32 W5: a refused change is repaired with StateDumpRequest Replace

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
constexpr u32 kLootWatchMs = 10000; // a map server: loot is looked at this long after it fell (GAMMA's ItemProcessor is done by then)
constexpr u32 kLootLookMs = 500; // ... this often
constexpr u32 kLootPerFrame = 16; // ... at most this many told in one frame (a body with a full backpack takes a few frames)
constexpr size_t kLootWatchedMax = 2048; // ... and no more watched at once: a battle of a hundred bodies is not worth more
constexpr size_t kPinnedMax = 64; // a map server: recalled squads pinned offline at once (a few companion squads in practice)

struct State
{
	// world server
	Keeper keeper;
	u64 fed_at = 0;
	std::map<PeerId, std::map<u16, u16>> fed; // bridge -> squad -> the game vertex it was told last
	u32 taken = 0, returned = 0, refused = 0, late = 0;
	u32 audits = 0, audit_rounds = 0; // LevelResync sent, reports heard
	srv::LevelsAudit manual_audit;
	u32 reserved_world = 0; // join::world_generation() the map servers' ids were kept out of
	u64 reserve_next = 0;
	std::string journal; // of the save just loaded (kept through a session start: a hosted save is loaded first)
	struct Body
	{
		BodyFacts facts;
		u64 at = 0;
		LevelId level = wire::kNoLevel; // Build 57 round 2 (review S1): the level of the map server that told it
	};
	std::map<u16, Body> bodies; // what the map servers say of the bodies of their players
	u32 body_reports = 0, body_refused = 0; // LevelEntity State heard, and thrown away
	u32 body_crossed = 0; // ... dropped as honest: it crossed his move (wire::BodyReport::Crossed)
	std::map<u16, wire::BodyCrossing> crossings; // the runs of crossed body reports, by character (a Take ends one)
	u32 spawns_refused = 0; // LevelEntity Spawn thrown away: those objects stand on their level alone
	std::map<LevelId, u64> logic_at; // when each level last said what its objects keep
	u32 logic_taken = 0, logic_refused = 0;
	u32 items_taken = 0, items_refused = 0; // LevelEntity Item: the wear of loot on a map server's level
	// R32 W3: the journals of the map servers (host_on_level_journal)
	journal::Receiver received; // the last batch applied of each bridge
	u32 batches_in = 0, batches_repeated = 0, batches_gap = 0, batches_malformed = 0, batches_late = 0;
	u32 entries_in = 0, entries_refused = 0;
	u32 carry_in = 0, done_in = 0, flushed_in = 0, id_asks_in = 0, id_blocks_out = 0; // the journal's own ops (WP10, WP11, WP9)
	u32 legacy_entity = 0; // LevelEntity facts of a peer that still sends them (one exe: none expected)
	IdGrants id_grants; // W9: the id blocks given to each bridge
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
		journal::ObjectHead head; // R32 A6: the object's root as the world still sees it when it applies the entry (see the hooks)
		std::string info; // Info: the info portion
		journal::ParentBefore before; // Parent: where it stood before this move (review WP4 #2)
	};
	xr_vector<Up> up; // what happened to the objects of his level, in order
	bool flushing = false; // client_flush_up runs: a send_up inside it sends the batch so far, it does not flush again
	journal::HeldMemory memory; // R32 A6: where the held creatures' children and the held squads' members stood (client_remember_held)
	std::set<u16> quiet; // local ids a refused create's repair releases now: the world never had them, their Destroy is not journaled
	u32 holder_world = 0; // join::world_generation() of g.holder: what the old world queued means nothing in the next
	// R32 W3: the level journal (one LevelJournal per frame on the lane of LevelXferUp) and the world's answers
	journal::Batcher batch;
	u32 journal_acked = 0; // the highest batch the world acknowledged
	u32 journal_batches = 0, journal_entries = 0, journal_refusals = 0, journal_bad = 0;
	u32 repairs_released = 0, repairs_fetched = 0, repairs_none = 0;
	// R32 W9: his ids, the slice and the world's blocks. An empty pool gives leader_free_id no id; the stock PerformIDgen then still
	// takes a stock id (xrServer.h), so W9's "no fallback" is open there (review WP4 #5)
	IdPool ids;
	u16 ids_slice = 0; // the slice the pool was made for
	u64 ids_check_next = 0;
	u32 id_blocks_in = 0, ids_empty = 0;
	bool ids_out = false; // his pool was empty once (logged once)
	std::map<u16, u64> refused_until;
	// Squads the world server recalled (recall): offline here until then, whoever stands by them. A friend of the leader who stays by
	// his companions would keep them online for good, and they would never go after him (testers 2026-11-13).
	std::map<u16, u64> pinned;
	xr_vector<u16> recalls; // recalled since the last frame: offline and home at the next one
	u32 recalled = 0, recall_sent = 0, recall_kept = 0; // recalls heard, gave back for one, votes a pin turned to offline
	bool handing_back = false; // it stops: nothing comes online here any more
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
	// Loot of its level whose state only it knows (testers 2026-11-13: a dead NPC's gun was 100 % on the world server and in the
	// players' hands). death_manager wears it in the death callback, ItemProcessor a few frames later: each is looked at for a
	// while and goes up, and to the players of this level, whenever its state differs from what was said of it.
	struct Watch
	{
		u64 until = 0;
		u64 next = 0;
		u32 said = 0; // items::state_digest of what went up, 0: nothing yet
		u16 dead = 0xffff; // the body it fell in (loot_root)
	};
	std::map<u16, Watch> watched; // local ids
	u32 items_told = 0;
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

// ---------------------------------------------------------------- R32 W3: the map server's level journal

// A map server: the journal so far goes up now, in batches under 1 MiB (A11), on the lane of LevelXferUp (client_send_all: with the
// hooks' ups queued so far). Under g.lock.
void client_send_journal()
{
	if (g.batch.empty())
		return;
	for (const wire::LevelJournalMsg& m : g.batch.take())
	{
		Writer w(m.wire_size() + 8);
		if (!m.write(w))
		{
			++g.journal_bad;
			::Msg("! [coop] levels: journal batch %u of %u entries did not write, it is lost", m.seq, u32(m.entries.size()));
			continue;
		}
		net::client_send(Msg::LevelJournal, w);
		++g.journal_batches;
		g.journal_entries += u32(m.entries.size());
	}
}

// R32 A6: the keeper generation of the unit at the object's root as this map server's holder holds it; 0 when that root is no unit
// it holds. The root: up its holders (8 at most), then a creature's squad (m_group_id, A5). Under g.lock.
u32 held_gen_of(const CSE_ALifeDynamicObject* e)
{
	if (!e || !g.holder)
		return 0;
	const CSE_ALifeDynamicObject* root = e;
	for (int depth = 0; root->ID_Parent != 0xffff && depth < 8; ++depth)
	{
		const CSE_ALifeDynamicObject* up = ai().alife().objects().object(root->ID_Parent, true);
		if (!up)
			break;
		root = up;
	}
	u16 unit = root->ID;
	const CSE_ALifeMonsterAbstract* m = smart_cast<const CSE_ALifeMonsterAbstract*>(root);
	if (m && m->m_group_id != 0xffff)
		unit = m->m_group_id;
	return g.holder->gen(join::to_host(unit));
}

// The head of a per-object entry (A13): the object's id in the world's numbering, its section crc, the generation of its root.
// Under g.lock.
journal::ObjectHead head_of(u16 local_id)
{
	journal::ObjectHead h;
	h.id = join::to_host(local_id);
	const CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(local_id, true) : nullptr;
	if (e)
	{
		h.crc = journal::section_crc(e->s_name.c_str());
		h.gen = held_gen_of(e);
	}
	return h;
}

// R32 A6 (review WP4 #2): the head of a Parent entry: the item, and the generation of the root it had before this move, as the world
// still sees it when it applies the entry (coop_levels_alife.cpp object_facts reads root_seen(item) before host_apply_parent). The
// hook comes after the move, so `before` (HeldMemory) says where it stood: on the ground, its own root; with a holder, that holder's
// root now (a change of that root since came up first: a Died, a Left); not known, no held creature held it at the end of the last
// frame and no hook moved it since: its old root is no unit this map server holds (gen 0). Under g.lock.
journal::ObjectHead head_before_move(u16 item, const journal::ParentBefore& before)
{
	journal::ObjectHead h;
	h.id = join::to_host(item);
	const CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(item, true) : nullptr;
	if (e)
		h.crc = journal::section_crc(e->s_name.c_str());
	if (!before.known || !g.holder || !e)
		return h;
	if (before.parent == 0xffff)
		h.gen = g.holder->gen(join::to_host(item)); // it lay on its own: its own root
	else
		h.gen = held_gen_of(ai().alife().objects().object(before.parent, true));
	return h;
}

// R32 A6 (review WP4 #1): a Died or a Destroy of a creature: the squad its head names and where it goes in g.up (journal::creature_place).
// The stock death chain (se_stalker:on_death -> squad:on_npc_death -> remove_npc -> unregister_member) and a script's remove_npc take a
// member out of its squad here before the hook runs; the world still has it in that squad until it applies the entry. Under g.lock.
journal::CreaturePlace place_of_creature(u16 local_id, journal::ObjectHead& h)
{
	const CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(local_id, true) : nullptr;
	const CSE_ALifeMonsterAbstract* m = e ? smart_cast<const CSE_ALifeMonsterAbstract*>(e) : nullptr;
	journal::CreaturePlace p;
	p.at = g.up.size();
	if (!m || m->ID_Parent != 0xffff)
		return p;
	p = journal::creature_place(g.up, local_id, m->m_group_id, g.memory.squad_before(local_id));
	if (p.squad != wire::kNoObject && g.holder)
		h.gen = g.holder->gen(join::to_host(p.squad));
	return p;
}

// R32 A6: where things stood under this map server's held units at the end of the frame (journal::HeldMemory): the children of every
// held creature (a held squad's members, a held creature of no squad) and the members of every held squad. A Parent hook that comes
// later finds the old holder here (its children lack the item by then), a Died or a Destroy finds the squad the death chain or
// remove_npc took the member out of. Children of children are not listed: an item in an item that moves names gen 0. Under g.lock.
void client_remember_held()
{
	g.memory.begin();
	if (!g.holder || !ai().get_alife())
		return;
	const auto children_of = [](const CSE_ALifeDynamicObject* holder) {
		for (const u16 child : holder->children)
			g.memory.child(child, holder->ID);
	};
	for (const u16 id : g.holder->ids())
	{
		if (!g.holder->authors(id))
			continue;
		const CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(id), true);
		if (!e)
			continue;
		if (const CSE_ALifeOnlineOfflineGroup* squad = smart_cast<const CSE_ALifeOnlineOfflineGroup*>(e))
		{
			for (const auto& m : squad->squad_members())
			{
				g.memory.member(m.first, e->ID);
				if (const CSE_ALifeDynamicObject* member = ai().alife().objects().object(m.first, true))
					children_of(member);
			}
		}
		else if (smart_cast<const CSE_ALifeCreatureAbstract*>(e))
			children_of(e);
	}
}

void journal_refused_locally(u8 op, u16 id)
{
	if (++g.journal_bad <= 8 || g.journal_bad % 100 == 0)
		::Msg("! [coop] levels: a journal %s of object %u does not read, it stays here (%u so far)", journal::op_name(op), u32(id),
		    g.journal_bad);
}

// A per-object entry: the head, then today's LevelEntity layout of the op. Under g.lock.
void journal_object(u8 op, const journal::ObjectHead& h, const Writer& layout)
{
	journal::Entry e;
	if (!journal::object_entry(op, h, layout, e) || !g.batch.add(std::move(e)))
		journal_refused_locally(op, h.id);
}

// An entry without a head (Joined, Left, Info, Carry, Done, Flushed). Under g.lock.
template <class B>
void journal_body(u8 op, const B& body, u16 id)
{
	journal::Entry e;
	if (!journal::body_entry(op, body, e) || !g.batch.add(std::move(e)))
		journal_refused_locally(op, id);
}

void client_flush_up();

// A map server: everything queued so far goes up now: the hooks' ups into the journal (client_flush_up), then the journal. send_up
// calls it before every transfer packet, so every fact queued before that packet is applied on the world before it (W3: a Spawn
// before its SpawnPrepare, a frame's facts before its hand-back; review WP4 #3: the rejects of a squad's items that the stock switch
// queued before its ReleasePrepare, with the generation the world has until that packet). A send_up inside client_flush_up sends the
// batch so far: the entries before that transfer packet. Under g.lock.
void client_send_all()
{
	if (!g.flushing && g.holder)
		client_flush_up();
	client_send_journal();
}

void send_up(const std::vector<xfer::Packet>& out)
{
	if (!out.empty())
		client_send_all(); // the facts before the transfer (W3)
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
			if (e && ai().script_engine().functor("coop_srv_levels_brain.left", left))
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

// The squads the world server recalled: offline now and home, as xfer_hand_back does for all of them, but with the switch said to the
// players of this level (no ClientApply: host_records), so a friend standing by sees them go, as the stock's companions vanish when they
// are teleported to their leader. One that is not its own or is on its way home already only keeps its pin.
void client_recalls(u64 now)
{
	xr_vector<u16> due;
	due.swap(g.recalls); // a switch runs Lua (the binders' net_destroy), which may recall again: that waits for the next frame
	for (const u16 id : due)
	{
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(join::to_local(id), true);
		if (!e || !g.holder->authors(id) || g.holder->frozen(id))
			continue;
		if (e->m_bOnline)
			alife_rw().switch_offline(e); // xfer_on_switched_offline queues it in `gives` too: the frozen check there skips it
		send_up(g.holder->want_offline(id, state_with_logic(e), now));
		++g.recall_sent;
		::Msg("[coop] levels: squad %u recalled by the world server, it goes home", u32(id));
	}
}

// A world item whose state nobody else says: on the ground, in a body, in a box. Its top holder, or nullptr: a player's item is his
// (ItemState), a living holder's reaches a player's hands with the authority's push (items::host_push). Both ends ask it. `dead`: a
// body whose death is being said now (its entity may not have the health of its object yet).
CSE_ALifeDynamicObject* loot_root(CSE_ALifeDynamicObject* item, u16 dead = 0xffff)
{
	if (!item || !item->cast_inventory_item())
		return nullptr;
	CSE_ALifeDynamicObject* root = item;
	for (int depth = 0; root->ID_Parent != 0xffff && depth < 8; ++depth)
	{
		CSE_ALifeDynamicObject* up = ai().alife().objects().object(root->ID_Parent, true);
		if (!up)
			return nullptr;
		root = up;
	}
	if (root->ID_Parent != 0xffff || root->ID == 0 || world::is_player_character(root))
		return nullptr;
	CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(root);
	return (c && root->ID != dead && c->g_Alive()) ? nullptr : root;
}

// A map server: loot of its level to look at for a while (State::Watch). Under g.lock.
void watch_loot(u16 local_id, u64 now, u16 dead = 0xffff)
{
	if (!loot_root(ai().alife().objects().object(local_id, true), dead))
		return;
	auto it = g.watched.find(local_id);
	if (it == g.watched.end())
	{
		if (g.watched.size() >= kLootWatchedMax)
			return;
		it = g.watched.emplace(local_id, State::Watch()).first;
		it->second.next = now;
	}
	it->second.until = now + kLootWatchMs;
	if (dead != 0xffff)
		it->second.dead = dead;
}

// A map server: what changed of the loot it looks at goes up (LevelEntity Item, the layout of ItemState) and to its players. Under g.lock.
void client_loot(u64 now)
{
	u32 told = 0;
	for (auto it = g.watched.begin(); it != g.watched.end() && told < kLootPerFrame;)
	{
		State::Watch& watch = it->second;
		if (now < watch.next)
		{
			++it;
			continue;
		}
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(it->first, true);
		if (!e || now >= watch.until || !loot_root(e, watch.dead))
		{
			it = g.watched.erase(it);
			continue;
		}
		watch.next = now + kLootLookMs;
		const u32 digest = items::state_digest(e, false);
		std::string section;
		std::vector<u8> blob;
		if (digest == watch.said || !items::level_item_state(it->first, section, blob))
		{
			++it;
			continue;
		}
		// R32 W3: the journal's ItemFacts, source Loot (the layout of ItemState and its source byte)
		journal::ItemFactsBody body;
		body.item = join::to_host(it->first);
		body.section = section;
		body.flags = 0; // a level's word is no player's authority push
		body.blob = blob;
		body.source = journal::ItemFactsBody::Loot;
		Writer w(blob.size() + section.size() + 24);
		if (body.write(w))
			journal_object(wire::LevelEntityHead::Item, head_of(it->first), w);
		else
			journal_refused_locally(wire::LevelEntityHead::Item, body.item);
		items::level_item_tell(it->first, section, blob);
		watch.said = digest;
		++g.items_told;
		++told;
		++it;
	}
}

// What happened on its level goes into the journal (R32 W3), in order, so that a spawn comes before what follows it. The head of a
// move, a death and a release is the one its hook took: the root the world still sees when it applies the entry (A6; review WP4 #1,
// #2); the head of the rest is the object as it stands now. Under g.lock.
void client_flush_up()
{
	using Head = wire::LevelEntityHead;
	if (!g.holder)
		return;
	xr_vector<State::Up> due;
	due.swap(g.up); // send_up below may run hooks that queue more: the next client_send_all takes them
	const bool was_flushing = g.flushing;
	g.flushing = true;
	journal::ParentNet net; // review WP4 #2: one Parent of an object per flush, from where it stood to where it stands now
	for (const State::Up& u : due)
	{
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(u.a, true);
		const journal::ObjectHead now_head = e ? head_of(u.a) : u.head;
		Writer w(64);
		switch (u.op)
		{
		case Head::Parent:
		{
			// Where it stands now, not what the hook saw: a holder that goes offline rejects every child on the way and takes
			// it back without a word (add_offline_impl), as the host's own flush nets out (events::host_flush).
			const u16 holder = e ? e->ID_Parent : u.b;
			if (net.send(u.a, u.before, holder))
			{
				w.u16(join::to_host(u.a));
				w.u16(holder == 0xffff ? u16(0xffff) : join::to_host(holder));
				const Fvector pos = e ? e->o_Position : Fvector().set(0.f, 0.f, 0.f);
				float p[3] = {pos.x, pos.y, pos.z};
				w.vec3(p);
				journal_object(Head::Parent, u.head, w); // the root before its first move of this flush
			}
			watch_loot(u.a, mgr().now_ms()); // a gun a wounded NPC threw, what a player put down: its wear after the move
			break;
		}
		case Head::Died:
		{
			if (net.unborn_here(u.a))
				continue;
			w.u16(join::to_host(u.a));
			w.u16(u.b == 0xffff ? u16(0xffff) : join::to_host(u.b));
			if (e)
			{
				const std::vector<u8> st = state_of(e);
				w.bytes(st.data(), st.size());
				const u64 now = mgr().now_ms();
				for (const u16 child : e->children)
					watch_loot(child, now, u.a); // death_manager wears what the body keeps
			}
			journal_object(Head::Died, u.head, w); // the squad the world still has it in (place_of_creature)
			break;
		}
		case Head::Destroy:
			if (g.holder->authors(u.a))
			{
				send_up(g.holder->destroyed(u.a, mgr().now_ms())); // a unit it holds goes on the transfer lane (host_gone)
				continue;
			}
			if (net.unborn_here(u.a))
				continue; // the world never heard of it
			w.u16(join::to_host(u.a));
			journal_object(Head::Destroy, u.head, w);
			break;
		case Head::Spawn:
			if (!e)
			{
				net.unborn(u.a);
				continue; // made and gone within a frame: nobody needs to hear of it
			}
			if (!entities::write_entity(w, e))
			{
				net.unborn(u.a);
				continue;
			}
			net.spawned(u.a); // written where it stands now: its moves of this flush are in it
			journal_object(Head::Spawn, now_head, w);
			if (transfers(e))
				send_up(g.holder->spawned(join::to_host(u.a), state_of(e), mgr().now_ms())); // the host must not simulate it too
			break;
		case Head::Tent:
			if (!e)
				continue;
			w.u16(join::to_host(u.a));
			w.u16(join::to_host(u.b));
			journal_object(Head::Tent, now_head, w);
			break;
		case Head::Stash:
			w.u16(join::to_host(u.a));
			w.u8(u8(u.b));
			journal_object(Head::Stash, now_head, w);
			break;
		case Head::Joined:
		case Head::Left:
		{
			journal::MemberBody body;
			body.squad = join::to_host(u.a);
			body.member = join::to_host(u.b);
			journal_body(u8(u.op), body, body.member);
			break;
		}
		case Head::Info:
		{
			if (u.a == 0 || (e && world::is_player_character(e)))
				continue; // a player's infos are his story's, the ghost is nobody (W1.5)
			journal::InfoBody body;
			body.id = join::to_host(u.a);
			body.on = u.b ? 1 : 0;
			body.info = u.info;
			journal_body(Head::Info, body, body.id);
			break;
		}
		case Head::Item:
		{
			// W8.3: the writer's own change of an item it writes (level_item_facts_up); the facts are read from the item here now
			std::string section;
			std::vector<u8> blob;
			if (!e || !items::level_item_state(u.a, section, blob))
				continue;
			journal::ItemFactsBody body;
			body.item = join::to_host(u.a);
			body.section = section;
			body.flags = 0;
			body.blob = std::move(blob);
			body.source = journal::ItemFactsBody::Decider;
			if (body.write(w))
				journal_object(Head::Item, now_head, w);
			else
				journal_refused_locally(Head::Item, body.item);
			break;
		}
		default: continue;
		}
	}
	g.flushing = was_flushing;
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
		Writer w(48); // R32 W3: the journal's State (the head names his character), on the reliable lane with the frame's facts
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
		const u16 lock = dialog::host_lock_of(it.first);
		facts.talking = lock != 0xffff ? 1 : 0;
		facts.partner = lock != 0xffff ? join::to_host(lock) : wire::kNoObject; // Build 57 (protocol 33): whom he talks to
		facts.place = u8(checkpoints::sources::rest_place(it.second.proxy, facts.source));
		// Build 57 (owner 2026-11-18): the base whose ground he stands on, by this level's geometry (the world server decides)
		checkpoints::sources::map_server_footing(it.second.proxy, body ? body->Position() : e->o_Position, facts.footing);
		facts.write(w);
		journal_object(wire::LevelEntityHead::State, head_of(u16(it.second.proxy)), w);
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
	Writer w(16 + size);
	if (!m.write(w))
	{
		::Msg("! [coop] levels: what object %u ('%s') keeps is %u bytes, it stays here", u32(id), object->cName().c_str(), u32(size));
		return;
	}
	{
		xrCriticalSectionGuard guard(g.lock); // R32 W3: the journal's Logic (this runs outside the frame's lock: it is Lua)
		journal_object(wire::LevelEntityHead::Logic, head_of(id), w);
	}
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

void send_resync(PeerId to, u32 mask, u32 round = 0)
{
	wire::LevelResyncMsg m;
	m.mask = mask;
	Writer w(8);
	m.write(w);
	srv::write_audit_round(w, round);
	net::host_send(to, Msg::LevelResync, w);
	++g.audits;
}

// R32 W5.2: the members of a squad in this process's registry (the world's numbering on the world server, this machine's on a map
// server), false for no squad.
bool members_of(u16 local_id, std::vector<u16>& out)
{
	out.clear();
	CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(local_id, true) : nullptr;
	const CSE_ALifeOnlineOfflineGroup* squad = e ? smart_cast<const CSE_ALifeOnlineOfflineGroup*>(e) : nullptr;
	if (!squad)
		return false;
	for (const auto& m : squad->squad_members())
		out.push_back(m.first);
	return true;
}

u32 audit_members_healed = 0, audit_members_refused = 0; // world server: members joined or left by the audit, and heals refused

void host_on_level_hash(PeerId from, Reader& r)
{
	wire::LevelHashMsg m;
	if (!mgr().is_host() || !m.read(r) || !g.keeper.connected(from))
		return;
	xfer::LevelHash his;
	his.n = m.n;
	his.set = m.set;
	std::copy(std::begin(m.bucket), std::end(m.bucket), std::begin(his.bucket));
	// R32 W5.2: a held squad hashes with the digest of its members on both sides
	const u32 mask = g.keeper.differs(from, his, [](uint16_t id) -> uint64_t {
		std::vector<u16> members;
		return members_of(id, members) ? srv::members_digest(members) : 0;
	});
	if (mask)
		send_resync(from, mask);
}

// R32 W5.2: the world takes the member list of a squad from the map server that holds it (the writer): a listed member it lacks in the
// squad joins it, a member it has that the writer does not list leaves it. Each change is a Joined or a Left entry through the world's
// one acceptance (host_apply_fact; review WP4 #9): a creature joins only when that map server also writes where it comes from (no
// squad of another writer loses it: two map servers that both list it would take it from each other every round), it must be a
// living creature the world has, and a Left needs the writer of the squad.
bool host_heal_member(PeerId from, u8 op, u16 squad, u16 member)
{
	journal::MemberBody body;
	body.squad = squad;
	body.member = member;
	journal::Entry e;
	if (!journal::body_entry(op, body, e))
		return false;
	uint8_t why = 0;
	if (host_apply_fact(from, e, why, g.received.last(u32(from))) == ApplyVerdict::Refused)
	{
		++audit_members_refused; // host_apply_fact named it in the log
		return false;
	}
	++audit_members_healed;
	::Msg("! [coop] levels audit: member %u %s squad %u as map server %u holds it", u32(member),
	    op == wire::LevelEntityHead::Joined ? "joins" : "leaves", u32(squad), mapserver::bridge_index(from));
	return true;
}

void host_heal_members(PeerId from, const std::vector<srv::MemberRow>& rows)
{
	for (const srv::MemberRow& row : rows)
	{
		if (g.keeper.holder_of(row.squad) != u32(from))
			continue; // only the writer's word counts (host_apply_fact asks it again)
		std::vector<u16> world;
		if (!members_of(row.squad, world))
			continue;
		const srv::MemberHeal heal = srv::heal_members(world, row.members);
		for (const u16 id : heal.leave)
			host_heal_member(from, wire::LevelEntityHead::Left, row.squad, id);
		for (const u16 id : heal.join)
			host_heal_member(from, wire::LevelEntityHead::Joined, row.squad, id);
	}
}

void host_on_report(PeerId from, Reader& r)
{
	wire::LevelReportBody m;
	std::vector<srv::MemberRow> members;
	u32 round = 0;
	if (!m.read(r) || !srv::read_members(r, members) || !srv::read_audit_round(r, round))
		return;
	if (round && !g.manual_audit.reply(round, from, m.buckets, ::Msg))
		return;
	if (!g.keeper.connected(from))
		return;
	std::vector<xfer::Entry> reported;
	reported.reserve(m.rows.size());
	for (const wire::LevelReportBody::Row& row : m.rows)
		reported.push_back(Keeper::bare(xfer::Entry{row.id, row.gen, xfer::kNoParent, true, 0}));
	std::vector<xfer::Finding> found;
	send_down(g.keeper.audit(from, reported, m.buckets, found));
	host_apply_changes();
	host_heal_members(from, members); // after the ledger's heal: the squads it still holds
	++g.audit_rounds;
	for (const xfer::Finding& f : found)
		::Msg("%s", xfer::describe(f).c_str());
}

// The digest of the members of a held squad (R32 W5.2), 0 for anything else. Under g.lock.
uint64_t held_members_digest(u16 id)
{
	std::vector<u16> members;
	if (!members_of(join::to_local(id), members))
		return 0;
	for (u16& m : members)
		m = join::to_host(m);
	return srv::members_digest(members);
}

// R32 W9: below the low-water mark the map server asks the world for another block of ids (IdAsk in its journal, answered in the
// Ack), again after kIdAskAgainMs while none came. Looked at every 2 s; the count stops at the mark. Under g.lock.
void client_ids(u64 now)
{
	if (now < g.ids_check_next)
		return;
	g.ids_check_next = now + 2000;
	const u16 first = leader_ids_of(mapserver::bridge_index(mgr().self()));
	const xrServer* server = (first && g_pGameLevel) ? Level().Server : nullptr;
	if (!server)
		return;
	if (g.ids_slice != first)
	{
		g.ids.reset(first, kLeaderIdsEach);
		g.ids_slice = first;
	}
	const size_t free = g.ids.free_count([server](u16 i) { return server->IsFreeID(i) != 0; }, kIdLowWater);
	if (g.ids.ask_now(free, now) && g.batch.add(journal::id_ask_entry()))
		::Msg("[coop] levels: %u free id(s) left in this map server's pool, another block is asked for", u32(free));
}

// The auditor's question: the hash of what it writes, on the lane of its transfers (R9: everything sent before it was heard).
void client_audit(u64 now)
{
	if (now < g.audit_next)
		return;
	g.audit_next = now + kAuditEveryMs;
	std::vector<xfer::Entry> mine = Keeper::bare(g.holder->report());
	for (xfer::Entry& e : mine)
		e.digest = held_members_digest(e.id); // R32 W5.2: the content, not only who writes what
	const xfer::LevelHash h = xfer::hash_of(mine);
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
	u32 round = 0;
	if (from != kHostPeer || !is_map_server() || !m.read(r) || !srv::read_audit_round(r, round))
		return;
	xrCriticalSectionGuard guard(g.lock);
	if (!g.holder)
		return;
	client_send_all(); // the facts queued so far first: the world applies them before it compares this report (R9)
	wire::LevelReportBody body;
	body.buckets = m.mask;
	std::vector<srv::MemberRow> members; // R32 W5.2: the member list of each held squad of these buckets
	for (const xfer::Entry& e : g.holder->report())
		if (m.mask & (1u << xfer::bucket_of(e.id)))
		{
			wire::LevelReportBody::Row row;
			row.id = e.id;
			row.gen = e.gen;
			body.rows.push_back(row);
			srv::MemberRow mr;
			if (members_of(join::to_local(e.id), mr.members) && mr.members.size() <= srv::kMaxMembers)
			{
				mr.squad = e.id;
				for (u16& id : mr.members)
					id = join::to_host(id);
				members.push_back(std::move(mr));
			}
		}
	wire::LevelEntityHead head;
	head.op = wire::LevelEntityHead::Report;
	Writer w(16 + body.rows.size() * 6);
	head.write(w);
	if (!body.write(w) || !srv::write_members(w, members))
		return;
	srv::write_audit_round(w, round);
	net::client_send(Msg::LevelEntity, w);
}

// LevelEntity keeps Report alone (the answer to LevelResync). R32 W3: every fact of a map server comes in its LevelJournal. One exe
// serves every role (protocol 34: a Dev 57 exe cannot join), so a LevelEntity fact can come only from a fault; applying it here would
// pass by the world's acceptance (W2b: writer, identity, fence), so it is counted and named in the log instead, never applied.
void host_on_level_entity(PeerId from, Reader& r)
{
	using Head = wire::LevelEntityHead;
	if (!mgr().is_host() || !world_up())
		return;
	Head head;
	if (!head.read(r))
		return;
	if (head.op == Head::Report)
	{
		host_on_report(from, r);
		return;
	}
	if (++g.legacy_entity <= 8 || g.legacy_entity % 100 == 0)
		::Msg("! [coop] levels: LevelEntity %s from map server %u is not applied (%u so far): its facts come in the journal (R32 W3)",
		    journal::op_name(head.op), mapserver::bridge_index(from), g.legacy_entity);
}

// R32 W3, W9: what the world gives a map server for an IdAsk: a block of its own free ids below the slices, kept out of its generator
// from now on (host_keep_id_blocks). False: none left, or that bridge holds kIdBlocksEach.
bool host_give_id_block(PeerId bridge, IdBlock& out)
{
	xrServer& server = alife_rw().server();
	if (!g.id_grants.grant(u32(bridge), [&](u16 id) { return server.IsFreeID(id) != 0; }, out))
		return false;
	for (u32 id = out.first; id < u32(out.first) + out.count; ++id)
		server.PerformIDgen(u16(id)); // free: grant() looked
	++g.id_blocks_out;
	::Msg("[coop] levels: ids %u..%u given to map server %u", u32(out.first), u32(out.first) + out.count - 1, mapserver::bridge_index(bridge));
	return true;
}

// The ids of the blocks given are kept out of this generator again, as host_reserve_leader_ids does with the slices: what a map server
// made under them and destroyed comes back to this generator with the entity.
void host_keep_id_blocks()
{
	xrServer& server = alife_rw().server();
	for (const IdBlock& b : g.id_grants.all())
		for (u32 id = b.first; id < u32(b.first) + b.count; ++id)
			if (server.IsFreeID(u16(id)))
				server.PerformIDgen(u16(id));
}

// The bridge is gone: the ids of its blocks that no object holds go back to this generator, the blocks may be given again.
void host_forget_id_blocks(PeerId bridge)
{
	if (!world_up() || !ai().get_alife())
	{
		g.id_grants.forget(u32(bridge));
		return;
	}
	xrServer& server = alife_rw().server();
	for (const IdBlock& b : g.id_grants.forget(u32(bridge)))
		for (u32 id = b.first; id < u32(b.first) + b.count; ++id)
			if (!server.IsFreeID(u16(id)) && !server.ID_to_entity(u16(id)) && !ai().alife().objects().object(u16(id), true))
				server.FreeID(u16(id), Device.TimerAsync());
}

// What one journal entry the world took or refused changes besides it (review WP4 #6): a Logic entry taken is what that level's
// objects keep after the held save, so serve_bridge serves a fresh save to the next map server of that level (logic_came_since);
// the status counts of the logic, the loot wear and the refused objects of map servers.
void host_count_entry(PeerId from, u8 op, bool taken)
{
	using Head = wire::LevelEntityHead;
	switch (op)
	{
	case Head::Logic:
		if (taken)
		{
			g.logic_at[mapserver::level_of_bridge(from)] = mgr().now_ms();
			++g.logic_taken;
		}
		else
			++g.logic_refused;
		break;
	case Head::Item:
		if (taken)
			++g.items_taken;
		else
			++g.items_refused;
		break;
	case Head::Spawn:
		if (!taken)
			++g.spawns_refused;
		break;
	default: break;
	}
}

// R32 W3: a map server's journal batch. Read whole first (LevelJournalMsg::read refuses a malformed batch whole: nothing of it is
// applied), then every entry in order through the world's one apply (host_apply_fact: acceptance, then the apply), the journal's own
// ops here; every batch is answered with LevelJournalAck {upto, the refused entries, an id block for an IdAsk}. A repeated batch is
// answered again and applied never.
void host_on_level_journal(PeerId from, Reader& r)
{
	using Head = wire::LevelEntityHead;
	if (!mgr().is_host() || !world_up() || !ai().get_alife())
		return;
	wire::LevelJournalMsg m;
	if (!m.read(r))
	{
		if (++g.batches_malformed <= 8 || g.batches_malformed % 100 == 0)
			::Msg("! [coop] levels: a journal batch of map server %u does not read, refused whole (%u so far)", mapserver::bridge_index(from),
			    g.batches_malformed);
		return; // its seq is not known: the map server's unacknowledged count says so
	}
	if (!g.keeper.connected(from))
	{
		++g.batches_late; // it is on its way out: what was on its way counts for nothing (as LevelXferUp)
		return;
	}
	++g.batches_in;
	std::vector<wire::LevelJournalAckMsg::Refusal> refusals;
	bool give_block = false;
	const journal::Receiver::Seen seen = g.received.look(u32(from), m.seq);
	if (seen == journal::Receiver::Seen::Repeat)
		++g.batches_repeated;
	else
	{
		if (seen == journal::Receiver::Seen::Gap && (++g.batches_gap <= 8 || g.batches_gap % 100 == 0))
			::Msg("! [coop] levels: journal batch %u of map server %u after %u: batches between were lost (%u so far)", m.seq,
			    mapserver::bridge_index(from), g.received.last(u32(from)), g.batches_gap);
		for (const journal::Entry& e : m.entries)
		{
			++g.entries_in;
			switch (e.op)
			{
			case Head::Carry: // W6: the module carry (WP10) has no consumer in this build yet
				if (g.carry_in++ == 0)
					::Msg("* [coop] levels: journal Carry heard from map server %u, nobody takes it yet (R32 WP10)", mapserver::bridge_index(from));
				continue;
			case Head::Done: // W7: the done-records (WP11/WP15) have no consumer in this build yet
				if (g.done_in++ == 0)
					::Msg("* [coop] levels: journal Done heard from map server %u, nobody takes it yet (R32 WP11)", mapserver::bridge_index(from));
				continue;
			case Head::Flushed: // W4: FlushRound is not wired in this build yet (WP9)
				if (g.flushed_in++ == 0)
					::Msg("* [coop] levels: journal Flushed heard from map server %u, no flush round waits yet (R32 WP9)",
					    mapserver::bridge_index(from));
				continue;
			case Head::IdAsk:
				++g.id_asks_in;
				give_block = true;
				continue;
			default: break;
			}
			uint8_t why = 0;
			const bool taken = host_apply_fact(from, e, why, m.seq) != ApplyVerdict::Refused;
			host_count_entry(from, e.op, taken);
			if (!taken)
			{
				++g.entries_refused;
				wire::LevelJournalAckMsg::Refusal f;
				f.seq = m.seq;
				f.op = e.op;
				f.id = journal::refused_id(e);
				f.why = why >= 1 && why <= journal::kLastWhy ? why : u8(journal::Refused);
				refusals.push_back(f);
			}
		}
		g.received.applied(u32(from), m.seq);
	}
	IdBlock block;
	const bool has_block = give_block && host_give_id_block(from, block);
	if (give_block && !has_block)
		::Msg("! [coop] levels: map server %u asked for ids, none left to give (it holds %u block(s))", mapserver::bridge_index(from),
		    u32(g.id_grants.blocks_of(u32(from)).size()));
	for (const wire::LevelJournalAckMsg& ack : journal::acks_of(g.received.last(u32(from)), refusals, has_block, block.first, block.count))
	{
		Writer w(16 + ack.refusals.size() * 8);
		if (ack.write(w))
			net::host_send(from, Msg::LevelJournalAck, w);
	}
}

// Squads on the move: a map server hears where they stand when it changed, on its level or leaving it. Marked ones are its own already.
void host_feed(u64 now)
{
	if (now - g.fed_at < kFeedEveryMs)
		return;
	g.fed_at = now;
	for (const PeerId bridge : srv::SessionSrv(mgr().session()).bridges())
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

bool host_take_body(PeerId from, LevelId level, Reader& r)
{
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
	const LevelId body_level = level_of_vertex(graph);
	const LevelId seat = level_of_player(join::peer_of_proxy(proxy));
	wire::BodyReport verdict = wire::body_report_verdict(r.ok(), e != nullptr, e && e->m_bOnline, e && world::is_player_character(e),
	    body_level, level, seat, _valid(pos[0]) && _valid(pos[1]) && _valid(pos[2]));
	u64 crossed_ms = 0;
	if (verdict == wire::BodyReport::Crossed)
	{
		crossed_ms = wire::body_crossing_age(g.crossings[proxy], level, seat, mgr().now_ms());
		verdict = wire::body_crossing_verdict(crossed_ms); // a map server that never hears he left is at fault
	}
	if (verdict == wire::BodyReport::Crossed)
	{
		// honest (stand b57g creator10_server): he left that level, its map server hears it from the next LevelMap
		if (++g.body_crossed <= 3)
			::Msg("[coop] levels: a body report of object %u crossed his move: of level %u, he is on %u now, dropped", u32(proxy),
			    u32(body_level), u32(seat));
		return false;
	}
	if (verdict != wire::BodyReport::Take)
	{
		if (++g.body_refused <= 3)
			::Msg("! [coop] levels: a body report of object %u refused: read %d, here %d, online %d, level %u of %u, seat %u, off his seat "
			      "%llu ms",
			    u32(proxy), r.ok() ? 1 : 0, e ? 1 : 0, (e && e->m_bOnline) ? 1 : 0, u32(body_level), u32(level), u32(seat), crossed_ms);
		return false;
	}
	g.crossings.erase(proxy);
	Fvector p, a;
	p.set(pos[0], pos[1], pos[2]);
	a.set(ang[0], ang[1], ang[2]);
	entities::place_offline(e, p, a, node, graph);
	State::Body& body = g.bodies[proxy];
	body.facts.fight = facts.fight != 0;
	body.facts.talking = facts.talking != 0;
	// Build 57 (protocol 33): the NPC of his talk there, while he talks, and only an object this world has (a creature of that level
	// is an object of the world under the same id)
	const u16 partner = wire::kept_partner(facts);
	body.facts.partner = partner != wire::kNoObject && ai().alife().objects().object(partner, true) ? partner : wire::kNoObject;
	body.facts.place = facts.place;
	body.facts.source = facts.source;
	// Build 57 (owner 2026-11-18): the footing is kept only when it names a base of this world on the level of its sender
	// (sources::host_footing: refused ones are counted, the first few logged); else he stands on no base. Round 3 (review S5):
	// the body position of this same report must stand on that base's ground too.
	body.facts.footing =
	    checkpoints::sources::host_footing(facts.footing, level, pos) ? facts.footing : checkpoint::base::Footing();
	body.at = mgr().now_ms();
	body.level = level; // Build 57 round 2 (review S1): body_facts answers only while he is on this level
	return true;
}

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
	// Build 57 round 2 (review S1): fresh, and told by the map server of the level he is on now (a report of the level he has
	// just left says nothing of where he is: checkpoint::base::facts_current)
	if (it == g.bodies.end() ||
	    !checkpoint::base::facts_current(it->second.level, level_of_player(join::peer_of_proxy(proxy)), it->second.at,
	        mgr().now_ms(), kBodyFreshMs))
		return false;
	out = it->second.facts;
	return true;
}

namespace
{
// An object and what it holds, down to 8 levels (local ids).
void subtree_of(const CSE_ALifeDynamicObject* e, std::vector<u16>& out, int depth = 0)
{
	if (!e || depth > 8)
		return;
	out.push_back(e->ID);
	for (const u16 child : e->children)
		subtree_of(ai().alife().objects().object(child, true), out, depth + 1);
}

// R32 W2b, W5, W9: a map server hears the world's answer to its journal. Every batch up to `upto` is applied but the rows; each row is
// repaired towards the world (journal::repair_of): a create the world refused is released here with its id kept reserved, a change
// or a removal it refused is asked for again (StateDumpRequest Replace: the world's EntitySpawn or EntityDestroy replaces the copy).
// An id block joins the pool.
void client_on_journal_ack(PeerId from, Reader& r)
{
	if (from != kHostPeer || !is_map_server())
		return;
	wire::LevelJournalAckMsg m;
	if (!m.read(r))
		return;
	std::vector<u16> release, fetch;
	{
		xrCriticalSectionGuard guard(g.lock);
		if (m.upto > g.batch.last_seq())
			return; // an answer to a journal of an earlier world (the seq started again)
		if (m.upto > g.journal_acked)
			g.journal_acked = m.upto;
		if (m.has_block)
		{
			if (g.ids.add_block(m.first, m.count))
			{
				++g.id_blocks_in;
				g.ids_out = false;
				::Msg("[coop] levels: ids %u..%u are this map server's from now on", u32(m.first), u32(m.first) + m.count - 1);
			}
			else
				::Msg("! [coop] levels: the world's id block %u..%u does not fit this map server's pool, not used", u32(m.first),
				    u32(m.first) + m.count - 1);
		}
		for (const wire::LevelJournalAckMsg::Refusal& f : m.refusals)
		{
			++g.journal_refusals;
			switch (journal::repair_of(f.op, f.why))
			{
			case journal::Repair::ReleaseKeepId:
				if (f.id != wire::kNoObject)
				{
					g.ids.reserve(f.id); // W9: refused ids stay reserved
					release.push_back(f.id);
				}
				break;
			case journal::Repair::Fetch:
				if (f.id != wire::kNoObject)
					fetch.push_back(f.id);
				break;
			case journal::Repair::None: ++g.repairs_none; break;
			}
			if (g.journal_refusals <= 8 || g.journal_refusals % 100 == 0)
				::Msg("! [coop] levels: the world refused %s of object %u in batch %u (%u so far): %s", journal::op_name(f.op), u32(f.id), f.seq,
				    g.journal_refusals, journal::why_name(f.why));
		}
	}
	// outside the lock: a release runs the hooks (xfer_on_released), which take it
	for (const u16 id : release)
	{
		CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(join::to_local(id), true) : nullptr;
		if (!e || world::is_player_character(e))
			continue;
		const bool held = xfer_authors(id);
		if (held)
			alife_rw().release(e, true); // a unit it holds: its Destroy goes on the transfer lane and clears the world's ledger
		else
		{
			// The world has no such object: no Destroy of it or of what it holds goes up (xfer_on_released skips them), but this
			// level's players are told (no ClientApply: host_records relays the release to them, review WP4 #10)
			std::vector<u16> subtree;
			subtree_of(e, subtree);
			{
				xrCriticalSectionGuard guard(g.lock);
				g.quiet.insert(subtree.begin(), subtree.end());
			}
			alife_rw().release(e, true);
			xrCriticalSectionGuard guard(g.lock);
			for (const u16 local : subtree)
				g.quiet.erase(local);
		}
		++g.repairs_released;
	}
	for (const u16 id : fetch)
	{
		Writer w(4);
		w.u16(id);
		w.u8(reconcile::Replace);
		net::client_send(Msg::StateDumpRequest, w);
		++g.repairs_fetched;
	}
}
} // namespace

uint32_t journal_unacked()
{
	xrCriticalSectionGuard guard(g.lock);
	return g.batch.last_seq() > g.journal_acked ? g.batch.last_seq() - g.journal_acked : 0;
}

bool journal_carry(const std::string& blob)
{
	if (!is_map_server() || blob.empty())
		return false;
	// W6: a part of at most kMaxPart bytes each; the whole goes in the journal of this frame, in order
	const size_t part = journal::CarryBody::kMaxPart;
	const size_t total = (blob.size() + part - 1) / part;
	if (total > 0xffff)
		return false;
	xrCriticalSectionGuard guard(g.lock);
	for (size_t i = 0; i < total; ++i)
	{
		journal::CarryBody b;
		b.part = u16(i);
		b.total = u16(total);
		const size_t at = i * part;
		const size_t n = blob.size() - at < part ? blob.size() - at : part;
		b.blob.assign(blob.begin() + ptrdiff_t(at), blob.begin() + ptrdiff_t(at + n));
		journal_body(wire::LevelEntityHead::Carry, b, wire::kNoObject);
	}
	return true;
}

void xfer_install_server(Session& s)
{
	s.on(Msg::LevelXferUp, &host_on_xfer_up);
	s.on(Msg::LevelEntity, &host_on_level_entity);
	s.on(Msg::LevelJournal, &host_on_level_journal); // R32 W3
	s.on(Msg::LevelHash, &host_on_level_hash);
}

void xfer_install_client(Endpoint& e)
{
	e.on(Msg::LevelXferDown, &client_on_xfer_down);
	e.on(Msg::LevelResync, &client_on_resync);
	e.on(Msg::LevelJournalAck, &client_on_journal_ack); // R32 W2b, W9
}

void xfer_audit_now()
{
	if (mgr().is_host() && split())
	{
		std::vector<PeerId> ready;
		for (const PeerId bridge : srv::SessionSrv(mgr().session()).bridges())
			if (g.keeper.connected(bridge))
				ready.push_back(bridge);
		const u32 round = g.manual_audit.begin(ready, ::Msg);
		for (const PeerId bridge : ready)
			send_resync(bridge, xfer::kAllBuckets, round);
	}
	g.audit_next = 0;
	xfer_print_status();
}

void xfer_reset()
{
	g.manual_audit.clear();
	g.keeper.restart();
	g.fed.clear();
	g.fed_at = 0;
	g.bodies.clear();
	g.crossings.clear();
	g.logic_at.clear();
	g.logic_taken = 0;
	g.logic_refused = 0;
	g.items_taken = 0;
	g.items_refused = 0;
	g.items_told = 0;
	g.received.clear(); // R32 W3: every bridge numbers its journal from 1 again
	g.id_grants.clear(); // R32 W9: a new world gives its blocks again
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
	g.pinned.clear();
	g.recalls.clear();
	g.recalled = 0;
	g.recall_sent = 0;
	g.recall_kept = 0;
	g.handing_back = false;
	g.settling.clear();
	g.watched.clear();
	g.scheduled_at = 0;
	g.up.clear();
	g.flushing = false;
	g.memory.begin(); // R32 A6
	g.quiet.clear();
	g.batch.reset(); // R32 W3
	g.journal_acked = 0;
	g.ids_slice = 0; // R32 W9: the pool is made again for the next world's slice
	g.ids_check_next = 0;
	g.ids_out = false;
	g.reserved_world = 0;
	g.stream_next = 0;
	g.stream_at = 0;
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
	g.manual_audit.disconnect(peer);
	g.received.forget(u32(peer)); // R32 W3: its next connection numbers its journal from 1
	host_forget_id_blocks(peer); // R32 W9
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
		if (g.reserved_world != join::world_generation())
			g.id_grants.clear(); // R32 W9: the blocks of an earlier world are nobody's
		host_reserve_leader_ids(g.reserved_world != join::world_generation());
		host_keep_id_blocks(); // R32 W9: and the blocks given to the map servers
		g.reserved_world = join::world_generation();
		g.reserve_next = now + kReserveEveryMs;
	}
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
			g.batch.reset(); // R32 W3: what the old world queued means nothing in the next
			g.journal_acked = 0;
			g.ids_slice = 0; // R32 W9: the pool of the next world (leader_free_id makes it)
			g.refused_until.clear();
			g.settling.clear();
			g.watched.clear();
			g.pinned.clear();
			g.recalls.clear();
			g.quiet.clear();
			client_remember_held(); // R32 A6: nothing held yet in this world
		}
		client_flush_up(); // R32 W3: what the hooks queued since the last frame, before this frame's transfers (review WP4 #3)
		client_recalls(now);
		client_requests(now);
		send_up(g.holder->tick(now));
		client_events(now);
		client_loot(now);
		client_stream(now);
		client_body(now);
		client_audit(now);
		client_ids(now);
	}
	client_logic(now); // outside the lock: this is Lua, and what it does comes back through the hooks below
	xrCriticalSectionGuard guard(g.lock);
	client_send_all(); // R32 W3: one LevelJournal for the frame's facts (send_up sent the part before a transfer already)
	client_remember_held(); // R32 A6: where things stand under the held units now, for the hooks until the next frame
}

// S4 (A-S4H): the stock hooks ask the session's mode (xrCoop/coop_mode.h); only a map server's mode (srv/coop_srv_mode.cpp) calls
// logic_may_update, xfer_scheduled, xfer_on_switched_offline and leader_free_id, only the world server's online_at_server. They ask no
// mode themselves any more: a co-op process never reaches them.
bool logic_may_update(CGameObject* object)
{
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
	{
		xrCriticalSectionGuard guard(g.lock);
		client_send_all(); // R32 W3: the last looks and the facts queued before them go up now, before the hand-over goes on
	}
	g.logic_over = true;
	::Msg("[coop] levels: what the objects of this level keep: %u said of %u watched, %llu bytes", g.logic_told, u32(g.book.watched()), g.logic_told_bytes);
}

bool logic_looking() { return g.looking; }

bool logic_came_since(LevelId level, u64 at)
{
	const auto it = g.logic_at.find(level);
	return logic::save_is_stale(at, it != g.logic_at.end(), it != g.logic_at.end() ? it->second : 0);
}

// The keeper marks objects only in a world server's session (a map server's take, its journal), and the session's end clears them
// (xfer_reset): every other process, a co-op host too, has none, so its direct callers (alife_simulator_script.cpp, xrServer_sls_clear.cpp,
// coop_entities.cpp, coop_requests.cpp, coop.levels.online_at_server) get false there as before.
bool online_at_server(u16 id)
{
	if (!g.keeper.marked())
		return false;
	if (g.keeper.away(id))
		return true;
	const CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
	const CSE_ALifeMonsterAbstract* m = e ? smart_cast<const CSE_ALifeMonsterAbstract*>(e) : nullptr;
	return m && m->m_group_id != 0xffff && g.keeper.away(m->m_group_id);
}

bool xfer_authors(u16 local_id)
{
	return g.holder && g.holder->authors(local_id);
}

PeerId holder_of(u16 id)
{
	if (!mgr().is_host() || !g.keeper.marked())
		return kInvalidPeer;
	u32 peer = g.keeper.holder_of(id);
	if (peer == Keeper::kNoSession)
	{
		// a member goes with its squad, as in online_at_server
		const CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
		const CSE_ALifeMonsterAbstract* m = e ? smart_cast<const CSE_ALifeMonsterAbstract*>(e) : nullptr;
		if (m && m->m_group_id != 0xffff)
			peer = g.keeper.holder_of(m->m_group_id);
	}
	return peer == Keeper::kNoSession ? kInvalidPeer : PeerId(peer);
}

u32 xfer_held_gen(u16 root, PeerId bridge, u32& spawn_gen)
{
	spawn_gen = 0;
	if (!mgr().is_host() || !g.keeper.marked())
		return 0;
	uint32_t born = 0;
	const u32 gen = g.keeper.gen_held_by(root, bridge, born);
	spawn_gen = born;
	return gen;
}

// R32 W9 (review WP4 #4): may `bridge`'s map server make an object under `id`? Of its slice, or of a block this world gave it for an
// IdAsk (g.id_grants). The world's acceptance of a Spawn asks it (coop_levels_alife.cpp host_apply_fact: f.id_in_slice).
bool host_id_is_senders(PeerId bridge, u16 id)
{
	return id_is_senders(id, leader_ids_of(mapserver::bridge_index(bridge)), g.id_grants, u32(bridge));
}

bool recall(u16 local_id, u32 ms)
{
	if (!is_map_server() || !ai().get_alife())
		return false;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(local_id, true);
	if (!e || !transfers(e))
		return false;
	const u16 id = join::to_host(unit_of(e)); // the key of the vote (xfer_stock_rules): a member goes with its squad
	xrCriticalSectionGuard guard(g.lock);
	if (ms == 0)
	{
		g.pinned.erase(id);
		return true;
	}
	if (g.pinned.size() >= kPinnedMax && g.pinned.count(id) == 0)
		return false;
	g.pinned[id] = mgr().now_ms() + _min(ms, kRecallPinMaxMs);
	g.recalls.push_back(id);
	++g.recalled;
	return true;
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
	const auto pin = g.pinned.find(id);
	if (pin != g.pinned.end())
	{
		if (mgr().now_ms() < pin->second)
		{
			// recalled: the players near it do not count, it is not asked for, and what it holds of it goes home
			++g.recall_kept;
			if (held && !frozen && !e->m_bOnline)
				g.gives.push_back(id);
			return false;
		}
		g.pinned.erase(pin); // the stock rules again: a pin never keeps anything offline for good
	}
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
	if (!world_up() || !ai().get_alife())
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
	xrCriticalSectionGuard guard(g.lock);
	g.gives.push_back(join::to_host(local_id));
}

void xfer_on_released(u16 local_id)
{
	if (!is_map_server() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g.lock);
	if (g.quiet.count(local_id))
		return; // a create the world refused, released by its repair: the world never had it
	// R32 A6: the head now, while the object is still there (the frame's flush finds it gone). A member a script's remove_npc took out
	// of its squad first names that squad and goes before the Left of it, which the world's release makes needless (review WP4 #1)
	State::Up u{wire::LevelEntityHead::Destroy, local_id, 0xffff, head_of(local_id), {}};
	const journal::CreaturePlace p = place_of_creature(local_id, u.head);
	g.up.insert(g.up.begin() + ptrdiff_t(p.at), u);
	if (p.before_left)
		g.up.erase(g.up.begin() + ptrdiff_t(p.at + 1));
}

void up_parent(u16 item, u16 new_parent)
{
	if (!is_map_server() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g.lock);
	if (g.quiet.count(item))
		return;
	// R32 A6 (review WP4 #2): the hook comes after the move; the head names the root before it, the root the world still sees
	const journal::ParentBefore before = g.memory.moved(item, new_parent);
	g.up.push_back(State::Up{wire::LevelEntityHead::Parent, item, new_parent, head_before_move(item, before), {}, before});
}

void up_died(u16 victim, u16 killer)
{
	if (!is_map_server() || entities::client_applying() || victim == 0)
		return;
	xrCriticalSectionGuard guard(g.lock);
	// R32 A6 (review WP4 #1): the death chain took a member out of its squad before this hook; the world has not run it yet. The head
	// names that squad, and the Died goes before the Left the member hook queued (the world's own death chain takes it out after)
	State::Up u{wire::LevelEntityHead::Died, victim, killer, head_of(victim), {}};
	const journal::CreaturePlace p = place_of_creature(victim, u.head);
	g.up.insert(g.up.begin() + ptrdiff_t(p.at), u);
}

void up_spawned(u16 id)
{
	if (!is_map_server() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Spawn, id, 0xffff, head_of(id), {}});
}

// R32 W3 (world-view D3): a member joined or left a squad of its level (stock register_member / unregister_member, through
// LevelSide::level_entity_up Joined / Left).
void up_member(u16 squad, u16 member, bool joined)
{
	if (!is_map_server() || entities::client_applying() || squad == 0xffff || member == 0xffff || squad == member)
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{joined ? wire::LevelEntityHead::Joined : wire::LevelEntityHead::Left, squad, member, journal::ObjectHead(), {}});
}

// R32 W3 (companions G5): an info portion of a non-actor object it writes was given or taken (LevelSide::level_info_up).
void up_info(u16 id, const char* info, bool on)
{
	if (!is_map_server() || entities::client_applying() || !info || !*info)
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Info, id, u16(on ? 1 : 0), journal::ObjectHead(), info});
}

// R32 W8.3: the state of an item it writes changed (LevelSide::level_item_facts_up); the facts are read at the frame's flush.
void up_item_facts(u16 id)
{
	if (!is_map_server() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g.lock);
	for (const State::Up& u : g.up)
		if (u.op == wire::LevelEntityHead::Item && u.a == id)
			return; // once a frame: the flush reads what it is then
	g.up.push_back(State::Up{wire::LevelEntityHead::Item, id, 0xffff, journal::ObjectHead(), {}});
}

void up_money(PeerId peer, s64 delta, bool trade)
{
	if (!is_map_server())
		return;
	Writer w(16); // R32 W3: the journal's Money (no head: a player's book is no object)
	w.u32(peer);
	w.u64(u64(delta));
	w.u8(trade ? 1 : 0);
	journal::Entry e;
	e.op = wire::LevelEntityHead::Money;
	e.body = w.data();
	xrCriticalSectionGuard guard(g.lock);
	if (!g.batch.add(std::move(e)))
		journal_refused_locally(wire::LevelEntityHead::Money, wire::kNoObject);
}

void up_tent(u16 tent, u16 character)
{
	if (!is_map_server())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Tent, tent, character, head_of(tent), {}});
}

void up_stash(u16 character, bool things_stay)
{
	if (!is_map_server())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Stash, character, u16(things_stay ? 1 : 0), head_of(character), {}});
}

// R32 W9: the next id of this map server's pool (its slice and the world's blocks), 0xffff when none is free: never an id of the stock
// generator by this function. The pool is made for the slice of this map server's number the first time and again when that number
// changes or the world does (xfer_client_frame); a low pool asks the world for a block (client_ids), an empty one at its next look.
// Open (review WP4 #5, W9 "no fallback"): the create is not refused here. PerformIDgen (xrServer.h) hands a 0xffff of own_free_id to
// the stock generator, which takes its lowest free id: the create goes on under an id outside the pool, the world refuses its Spawn
// (IdRange) and the repair releases it one round trip later. Refusing the create needs xrServer.h and the create paths (the lead's).
u16 leader_free_id()
{
	const u16 first = leader_ids_of(mapserver::bridge_index(mgr().self()));
	const xrServer* server = (first && g_pGameLevel) ? Level().Server : nullptr;
	if (!server)
		return 0xffff;
	xrCriticalSectionGuard guard(g.lock);
	if (g.ids_slice != first)
	{
		g.ids.reset(first, kLeaderIdsEach);
		g.ids_slice = first;
	}
	const u16 id = g.ids.next([server](u16 i) { return server->IsFreeID(i) != 0; });
	if (id != wire::kNoObject)
		return id;
	++g.ids_empty;
	g.ids_check_next = 0; // client_ids looks at once: below the low-water mark it asks for a block (kIdAskAgainMs apart)
	if (!g.ids_out)
	{
		g.ids_out = true;
		::Msg("! [coop] levels: every id of this map server's pool is in use (%u ids, %u reserved): a create takes a stock id, which the "
		      "world refuses (W9 no-fallback is open)",
		    u32(g.ids.size()), u32(g.ids.reserved_count()));
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
		::Msg("[coop] levels: body reports %u, refused %u, crossed a move %u, %u bod(ies) known", g.body_reports, g.body_refused,
		    g.body_crossed, u32(g.bodies.size()));
		::Msg("[coop] levels: objects of map servers refused %u", g.spawns_refused);
		::Msg("[coop] levels: what static objects keep: %u taken, %u refused", g.logic_taken, g.logic_refused);
		::Msg("[coop] levels: the wear of loot on map servers' levels: %u taken, %u refused", g.items_taken, g.items_refused);
		host_accept_print_status(); // R32 W2b: the journal's entries taken and refused
		::Msg("[coop] levels: journal batches %u (repeated %u, after a gap %u, malformed %u, late %u), entries %u, refused %u; carry %u, done %u, "
		      "flushed %u (no consumer yet); id asks %u, blocks given %u; LevelEntity facts not applied %u",
		    g.batches_in, g.batches_repeated, g.batches_gap, g.batches_malformed, g.batches_late, g.entries_in, g.entries_refused, g.carry_in,
		    g.done_in, g.flushed_in, g.id_asks_in, g.id_blocks_out, g.legacy_entity);
		::Msg("[coop] levels audit: members healed %u, heals the world's acceptance refused %u", audit_members_healed,
		    audit_members_refused);
		return;
	}
	::Msg("[coop] levels: what the objects of this level keep: %u said of %u watched, %llu bytes", g.logic_told, u32(g.book.watched()), g.logic_told_bytes);
	xrCriticalSectionGuard guard(g.lock);
	::Msg("[coop] levels: the wear of loot told %u, %u item(s) watched", g.items_told, u32(g.watched.size()));
	::Msg("[coop] levels: journal batches %u, entries %u, acknowledged up to %u of %u, refused %u (released %u, fetched %u, kept %u), "
	      "not written %u; ids: pool %u, reserved %u, blocks from the world %u, empty %u time(s)",
	    g.journal_batches, g.journal_entries, g.journal_acked, g.batch.last_seq(), g.journal_refusals, g.repairs_released,
	    g.repairs_fetched, g.repairs_none, g.journal_bad + g.batch.dropped(), u32(g.ids.size()), u32(g.ids.reserved_count()),
	    g.id_blocks_in, g.ids_empty);
	if (g.holder)
		::Msg("[coop] levels: holding %u object(s), %s, granted %u, taken back %u, refused %u, retries %llu",
		    u32(g.holder->ids().size()), g.holder->idle() ? "idle" : "busy", g.creates, g.removes, g.refusals, g.holder->retries());
	::Msg("[coop] levels: recalls heard %u, gave back %u, votes kept offline %u, %u squad(s) pinned", g.recalled, g.recall_sent,
	    g.recall_kept, u32(g.pinned.size()));
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
