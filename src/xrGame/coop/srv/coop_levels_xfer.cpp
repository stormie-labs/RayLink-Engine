#include "stdafx.h"
#include "pch_script.h"
// The transfers of ownership in the engine, between the world server and a map server.
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
// The auditor: every 10 s it sends the hash of what it writes; a bucket that differs from the ledger is asked for in
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
#include "coop_dedicated.h" // dedicated::on() (xfer_load: the world server takes its save's done-records)
#include "coop_levels_alife.h" // host_accept_print_status
#include "../coop_money.h"
#include "../coop_net.h"
#include "../coop_script.h" // coop_op_done (script::queue_callback)
#include "../coop_survival.h"
#include "../coop_world.h"
#include "../coop_writer.h" // writing::root_facts_of, the root and its kind as the world reads them (held_gen_of)
#include "xrCoop/srv/coop_levels_keeper.h"
#include "xrCoop/srv/coop_levels_accept.h" // the decision on a body report (host_take_body)
#include "xrCoop/srv/coop_levels_audit.h"
#include "xrCoop/srv/coop_levels_journal.h" // the map server's journal, its batches and the world's Ack
#include "xrCoop/srv/coop_levels_root_gen.h" // the gen at a root, one rule with the world (same_as)
#include "xrCoop/srv/coop_session_srv.h"
#include "xrCoop/coop_reconcile.h" // a refused change is repaired with StateDumpRequest Replace

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
constexpr u32 kAuditEveryMs = 10000; // a map server's hash of what it holds, this often
constexpr u32 kScheduledPerRound = 8; // a map server: offline objects it holds that get their stock update in one ALife update
constexpr u32 kWithHolderLines = 8; // a map server: log lines of Destroys left out with their holder (journal::journals_destroy)
constexpr size_t kDepartedMax = 4096; // a map server: items left out with a departed character whose Destroy is awaited (g.departed)
constexpr u32 kJournalChunk = 0xC0C0; // of the ALife save; the stock reader looks for its own chunks and never sees it
constexpr u32 kDoneChunk = 0xC0C1; // the world's done-records (journal::done::Store), the same way
// The world hands a record's answer to Lua as one callback argument (coop_op_done): a record never holds more than that.
static_assert(journal::done::kMaxAnswer <= script::kMaxCallbackStr, "a done-record's answer fits one callback argument");
static_assert(journal::done::kMaxAnswer <= journal::DoneBody::kMaxAnswer, "a done-record fits a Done entry");
// A map server watches dropped loot for this long so it can report changes through the end of item processing.
constexpr u32 kLootWatchMs = 10000;
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
	// The done-records of the save just loaded (kDoneChunk), kept through a session start as `journal` (xfer_reset leaves it).
	// The world server takes them as its store on its next frame (host_adopt_done), a map server raises those of its level on the first
	// frame of its world (client_replay_done). The rules are journal::done::Loaded's.
	journal::done::Loaded done_loaded;
	// A map server says Ready once the records its replay queued reached its Lua (xfer_replay_heard)
	journal::done::ReplayGate replay_gate;
	struct Body
	{
		BodyFacts facts;
		u64 at = 0;
		LevelId level = wire::kNoLevel; // the level of the map server that told it
	};
	std::map<u16, Body> bodies; // what the map servers say of the bodies of their players
	u32 body_reports = 0, body_refused = 0; // LevelEntity State heard, and thrown away
	u32 body_crossed = 0; // ... dropped as honest: it crossed his move (wire::BodyReport::Crossed)
	std::map<u16, wire::BodyCrossing> crossings; // the runs of crossed body reports, by character (a Take ends one)
	u32 spawns_refused = 0; // LevelEntity Spawn thrown away: those objects stand on their level alone
	std::map<LevelId, u64> logic_at; // when each level last said what its objects keep
	u32 logic_taken = 0, logic_refused = 0;
	u32 items_taken = 0, items_refused = 0; // LevelEntity Item: the wear of loot on a map server's level
	// The journals of the map servers (host_on_level_journal)
	journal::Receiver received; // the last batch applied of each bridge
	u32 batches_in = 0, batches_repeated = 0, batches_gap = 0, batches_malformed = 0, batches_late = 0;
	u32 batches_starting = 0; // applied from a map server before its Ready
	u32 entries_in = 0, entries_refused = 0;
	u32 carry_in = 0, done_in = 0, flushed_in = 0, id_asks_in = 0, id_blocks_out = 0; // the journal's own ops
	// The done-records of numbered ops, per level (host_take_done), in the world's save (kDoneChunk)
	journal::done::Store done;
	std::map<LevelId, u64> done_at; // when each level last gave a new record: a save older than that lacks it (logic_came_since)
	u32 done_kept = 0, done_repeats = 0, done_refused = 0;
	u32 legacy_entity = 0; // LevelEntity facts of a peer that still sends them (one exe: none expected)
	IdGrants id_grants; // the id blocks given to each bridge
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
		journal::ObjectHead head; // the object's root as the world still sees it when it applies the entry (see the hooks)
		std::string info; // Info: the info portion; Done: the answer
		journal::ParentBefore before; // Parent: where it stood before this move
		u32 origin = 0, seq = 0; // Done: the op's number
	};
	xr_vector<Up> up; // what happened to the objects of his level, in order
	bool flushing = false; // client_flush_up runs: a send_up inside it sends the batch so far, it does not flush again
	journal::HeldMemory memory; // where the held creatures' children and the held squads' members stood (client_remember_held)
	std::set<u16> quiet; // local ids a refused create's repair releases now: the world never had them, their Destroy is not journaled
	// Local ids whose Parent out of a departed player's character was left out (journal::journals_move_out_of_character): their
	// Destroy is left out too (the world keeps them in him). At most kDepartedMax; past it a Destroy goes up and the world refuses it.
	std::set<u16> departed;
	journal::ReleasesUnderWay releasing; // local ids whose release is under way: never adopted at their Left
	u32 holder_world = 0; // join::world_generation() of g.holder: what the old world queued means nothing in the next
	// The level journal (one LevelJournal per frame on the lane of LevelXferUp) and the world's answers
	journal::Batcher batch;
	u32 journal_acked = 0; // the highest batch the world acknowledged
	u32 journal_batches = 0, journal_entries = 0, journal_refusals = 0, journal_bad = 0;
	u32 repairs_released = 0, repairs_fetched = 0, repairs_none = 0;
	u32 parents_unchanged = 0; // Parents left out, back where they stood (ParentNet::unchanged)
	u32 parents_unregistered = 0; // Parent hooks of xrServer entities outside this ALife, left out (journal::journals_parent)
	u32 parents_departed = 0, destroys_departed = 0; // moves out of a departed player's character and their Destroys, left out
	u32 destroys_with_holder = 0; // Destroys of children released with their holder, left out (journal::journals_destroy)
	u32 done_queued = 0, done_replayed = 0; // Done entries queued here, records of its level raised from its save
	// His ids, the slice and the world's blocks. An empty pool gives leader_free_id no id; what a create does then is
	// xrServer::CoopMayCreate's and PerformIDgen's (xrServer.h: refused at its start under coop_writer_gate = enforce)
	IdPool ids;
	u16 ids_slice = 0; // the slice the pool was made for
	u64 ids_check_next = 0;
	u32 id_blocks_in = 0, ids_empty = 0;
	bool ids_out = false; // his pool was empty once (logged once)
	std::map<u16, u64> refused_until;
	// Squads the world server recalled (recall): offline here until then, whoever stands by them. A friend of the leader who stays by
	// his companions would keep them online for good, and they would never go after him.
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
	// Loot of its level whose state only it knows (a dead NPC's gun was 100 % on the world server and in the
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

// A map server: its holder is of this world: the terms on which the writer rule counts its holds (xfer_authors, xfer_born). Also while
// it hands everything back: a unit on its way home is its until the world's Ack, as the world's keeper
// says (the writer changes only at the fence); the hand-back stops the switch vote and the scheduler (xfer_stock_rules,
// xfer_scheduled), not its writing. Under g.lock.
bool holder_writes()
{
	return g.holder && g.holder_world == join::world_generation();
}

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

// ---------------------------------------------------------------- the map server's level journal

// A map server: the journal so far goes up now, in batches under 1 MiB, on the lane of LevelXferUp (client_send_all: with the
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

// The keeper generation of the object's root as this map server's holder holds it, whatever that root is now (a unit,
// or the body of one held here that died: levels::root_gen, the world's rule as well); 0 when it does not hold the root. The root and
// its kind come from writing::root_facts_of, the function the world reads them with (srv/coop_levels_alife.cpp root_seen, same_as):
// up its holders (8 at most), then a creature's squad (m_group_id), and the kind is that root's own, so a member counts as its
// squad (a unit) on both sides, whatever the member is. Under g.lock.
u32 held_gen_of(const CSE_ALifeDynamicObject* e)
{
	if (!e || !g.holder)
		return 0;
	const writer::RootFacts root = writing::root_facts_of(e->ID);
	return root_gen(root.kind, g.holder->gen(join::to_host(root.root)));
}

// The head of a per-object entry: the object's id in the world's numbering, its section crc, the generation of its root.
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

// The head of a Parent entry: the item, and the generation of the root it had before this move, as the world
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

// A Died or a Destroy of a creature: the squad its head names and where it goes in g.up (journal::creature_place).
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

// Where things stood under this map server's held units at the end of the frame (journal::HeldMemory): the children of every
// held creature (a held squad's members, a held creature of no squad) and of every character its players play here (the departure
// flood, below), and the members of every held squad. A Parent hook that comes later finds the old holder here (its children lack the
// item by then), a Died or a Destroy finds the squad the death chain or remove_npc took the member out of. Children of children are
// not listed: an item in an item that moves names gen 0. Under g.lock.
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
	// And the children of the characters of the players of this level. A player who leaves it (quits,
	// drops, takes an exit) has his character switched offline here (join::on_peer_left -> take_offline, the same id as here): every item
	// rejected with a hook, taken back without one (add_offline_impl). Listed, each is back where it stood at the flush and is left out
	// (journal::ParentNet); unlisted, each went up as a Parent to the same character, which the world, writing the parked character by
	// then, refused as not the writer, and each repair (Fetch) made the item again here. His virtual peer leaves the trunk in the next
	// frame's world update and his character goes offline in its session update, both before that frame's flush and before this
	// snapshot is taken again; should that order change, the world still takes such a Parent as a repeat (accept::unchanged_parent).
	for (const auto& it : mapserver::trunk_peers().on())
		if (const CSE_ALifeDynamicObject* played = ai().alife().objects().object(it.second.proxy, true))
			children_of(played);
}

// A creature left a squad this map server writes (local ids), alive and here: it inherits the squad's hold at once
// (Holder::adopt), so no moment passes in which nobody writes it (its switch vote, the writer rule) and every head after the Left
// names its root with the gen the world gives it when it applies the same Left (Ledger::adopt). A squad that is not written here
// yet (its Spawn waits in this frame's journal) adopts it at the flush. A dead one (the death chain's Left) is nobody's unit: its
// body is a resident. One whose release is under way (the Released hook ran first) is never adopted: its
// Destroy, journaled, releases it on the world from the squad. The rule: journal::adopts_at_left. Under g.lock.
void client_adopt_left(u16 squad, u16 member, u64 now)
{
	if (!g.holder)
		return;
	const u16 unit = join::to_host(squad), id = join::to_host(member);
	CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(member, true) : nullptr;
	CSE_ALifeCreatureAbstract* c = e ? smart_cast<CSE_ALifeCreatureAbstract*>(e) : nullptr;
	journal::LeftAdoptFacts f;
	f.squad_written = g.holder->authors(unit);
	f.member_written = g.holder->authors(id);
	f.living_creature = c && c->g_Alive() && e->ID_Parent == 0xffff && !world::is_player_character(e) && transfers(e);
	f.releasing = g.releasing.under_way(member);
	if (journal::adopts_at_left(f))
		g.holder->adopt(id, unit, std::vector<u8>(), now);
}

// The death or the release of `local_id` goes before the Left of its squad that the stock chain queued first
// (journal::creature_place): the world applies it while the creature is still in that squad, so the creature never became a unit
// of its own there. Its adoption at that Left (client_adopt_left) is undone here. Under g.lock.
void client_unadopt(u16 local_id)
{
	if (g.holder)
		g.holder->forget(join::to_host(local_id));
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
// calls it before every transfer packet, so every fact queued before that packet is applied on the world before it (a Spawn
// before its SpawnPrepare, a frame's facts before its hand-back: the rejects of a squad's items that the stock switch
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
		client_send_all(); // the facts before the transfer
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
		// The journal's ItemFacts, source Loot (the layout of ItemState and its source byte)
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

// What happened on its level goes into the journal, in order, so that a spawn comes before what follows it. The head of a
// move, a death and a release is the one its hook took: the root the world still sees when it applies the entry;
// the head of the rest is the object as it stands now. Under g.lock.
void client_flush_up()
{
	using Head = wire::LevelEntityHead;
	if (!g.holder)
		return;
	xr_vector<State::Up> due;
	due.swap(g.up); // send_up below may run hooks that queue more: the next client_send_all takes them
	const bool was_flushing = g.flushing;
	g.flushing = true;
	// A release that is over took its object out of the registry: its mark goes (one under way stays); with no
	// ALife no object stands at all
	if (ai().get_alife())
		g.releasing.prune([](u16 local_id) { return ai().alife().objects().object(local_id, true) != nullptr; });
	else
		g.releasing.clear();
	journal::ParentNet net; // one Parent of an object per flush, from where it stood to where it stands now
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
			const bool sends = net.send(u.a, u.before, holder);
			// out of a player's character no player plays here any more: the world keeps it in him (with its Destroy, below)
			const CSE_ALifeDynamicObject* from = sends && u.before.known && u.before.parent != 0xffff ?
			    ai().alife().objects().object(u.before.parent, true) : nullptr;
			const bool from_character = from && world::is_player_character(from);
			if (sends && !journal::journals_move_out_of_character(from_character, from_character && writing::writes(from->ID)))
			{
				++g.parents_departed;
				if (g.departed.size() < kDepartedMax)
					g.departed.insert(u.a);
			}
			else if (sends)
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
			if (g.departed.erase(u.a))
			{
				++g.destroys_departed; // its holder's world keeps it in him
				continue;
			}
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
			// A creature it held on its own goes with a squad it writes from now on (the world drops its record at this
			// Joined); one that left a squad it writes is its own, also when that squad's spawn was flushed only now (above)
			if (u.op == Head::Joined && g.holder->authors(body.squad))
				g.holder->forget(body.member);
			else if (u.op == Head::Left)
				client_adopt_left(u.a, u.b, mgr().now_ms());
			break;
		}
		case Head::Info:
		{
			if (u.a == 0 || (e && world::is_player_character(e)))
				continue; // a player's infos are his story's, the ghost is nobody
			journal::InfoBody body;
			body.id = join::to_host(u.a);
			body.on = u.b ? 1 : 0;
			body.info = u.info;
			journal_body(Head::Info, body, body.id);
			break;
		}
		case Head::Item:
		{
			// The writer's own change of an item it writes (level_item_facts_up); the facts are read from the item here now.
			// Its source is what items::note_facts recorded before it raised this up (Decider: a script's set,
			// an upgrade sold; Owner: the owner's own use the writer took), read once whether the item is still here or not; nothing
			// recorded (0) is a writer's word all the same: Decider, never the wear-only Loot path
			const u8 noted = items::level_facts_source(u.a);
			std::string section;
			std::vector<u8> blob;
			if (!e || !items::level_item_state(u.a, section, blob))
				continue;
			journal::ItemFactsBody body;
			body.item = join::to_host(u.a);
			body.section = section;
			body.flags = 0;
			body.blob = std::move(blob);
			body.source = noted != journal::ItemFactsBody::Loot ? noted : u8(journal::ItemFactsBody::Decider);
			if (body.write(w))
				journal_object(Head::Item, now_head, w);
			else
				journal_refused_locally(Head::Item, body.item);
			break;
		}
		case Head::Done:
		{
			// A numbered op's answer (journal_done), after the facts of its effect queued before it: the same batch
			journal::DoneBody body;
			body.origin = u.origin;
			body.op = u.seq;
			body.answer = u.info;
			journal_body(Head::Done, body, wire::kNoObject);
			break;
		}
		default: continue;
		}
	}
	g.parents_unchanged += net.unchanged(); // what the snapshot let it leave out
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
		Writer w(48); // the journal's State (the head names his character), on the reliable lane with the frame's facts
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
		facts.partner = lock != 0xffff ? join::to_host(lock) : wire::kNoObject; // whom he talks to
		facts.place = u8(checkpoints::sources::rest_place(it.second.proxy, facts.source));
		// The base whose ground he stands on, by this level's geometry (the world server decides)
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
		xrCriticalSectionGuard guard(g.lock); // the journal's Logic (this runs outside the frame's lock: it is Lua)
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

// The members of a squad in this process's registry (the world's numbering on the world server, this machine's on a map
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
	// A held squad hashes with the digest of its members on both sides
	const u32 mask = g.keeper.differs(from, his, [](uint16_t id) -> uint64_t {
		std::vector<u16> members;
		return members_of(id, members) ? srv::members_digest(members) : 0;
	});
	if (mask)
		send_resync(from, mask);
}

// The world takes the member list of a squad from the map server that holds it (the writer): a listed member it lacks in the
// squad joins it, a member it has that the writer does not list leaves it. Each change is a Joined or a Left entry through the world's
// one acceptance (host_apply_fact): a creature joins only when that map server also writes where it comes from (no
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
	if (host_apply_heal(from, e, why, g.received.last(u32(from))) == ApplyVerdict::Refused) // a Left takes it home
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

// The digest of the members of a held squad, 0 for anything else. Under g.lock.
uint64_t held_members_digest(u16 id)
{
	std::vector<u16> members;
	if (!members_of(join::to_local(id), members))
		return 0;
	for (u16& m : members)
		m = join::to_host(m);
	return srv::members_digest(members);
}

// Below the low-water mark the map server asks the world for another block of ids (IdAsk in its journal, answered in the
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

// The auditor's question: the hash of what it writes, on the lane of its transfers (everything sent before it was heard).
// The frame's facts go first (client_send_all: the hooks' ups, then the journal), so the world has applied every Joined and Left this
// hash's member digests count before it compares them. Under g.lock.
void client_audit(u64 now)
{
	if (now < g.audit_next)
		return;
	g.audit_next = now + kAuditEveryMs;
	client_send_all();
	std::vector<xfer::Entry> mine = Keeper::bare(g.holder->report());
	for (xfer::Entry& e : mine)
		e.digest = held_members_digest(e.id); // the content, not only who writes what
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
	client_send_all(); // the facts queued so far first: the world applies them before it compares this report
	wire::LevelReportBody body;
	body.buckets = m.mask;
	std::vector<srv::MemberRow> members; // the member list of each held squad of these buckets
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

// LevelEntity keeps Report alone (the answer to LevelResync). Every fact of a map server comes in its LevelJournal. One exe
// serves every role (protocol 34: a Dev 57 exe cannot join), so a LevelEntity fact can come only from a fault; applying it here would
// pass by the world's acceptance (writer, identity, fence), so it is counted and named in the log instead, never applied.
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

// What the world gives a map server for an IdAsk: a block of its own free ids below the slices, kept out of its generator
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

// What one journal entry the world took or refused changes besides it: a Logic entry taken is what that level's
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

// A Done entry of the map server behind `from`, batch `seq`: the store keeps the record for that map server's
// level, the first answer of an op standing (a repeat changes nothing and is no refusal), and a new record goes to the world's Lua as
// coop_op_done(origin, seq, answer) (coop_srv_mode.script -> coop_ops_done.heard: the world's ledger, its save). The level is marked:
// a held save older than this record is not served to the level's next map server (logic_came_since), which then gets the record in
// the fresh save it is served (xfer_save). A record the store does not take (a number or an answer journal::done::valid refuses, a
// bridge no level names, a level past the bound) is answered in the Ack (Refused; the map server's repair does nothing about it).
void host_take_done(PeerId from, const journal::Entry& e, u32 seq, std::vector<wire::LevelJournalAckMsg::Refusal>& refusals)
{
	namespace jd = journal::done;
	journal::DoneBody body;
	Reader r(e.body.data(), e.body.size());
	const LevelId level = mapserver::level_of_bridge(from);
	const jd::Put put = journal::reads_whole(r, body) ? g.done.put(level, body.origin, body.op, body.answer) : jd::Put::Refused;
	switch (put)
	{
	case jd::Put::Kept:
	{
		++g.done_kept;
		g.done_at[level] = mgr().now_ms();
		script::CallbackArg answer;
		answer.kind = script::CallbackArg::Str;
		answer.s = body.answer; // whole: an answer may hold any byte (cb_str stops at a zero)
		script::queue_callback("coop_op_done", {script::cb_int(s64(body.origin)), script::cb_int(s64(body.op)), answer});
		break;
	}
	case jd::Put::Repeat: ++g.done_repeats; break;
	case jd::Put::Refused:
	{
		if (++g.done_refused <= 8 || g.done_refused % 100 == 0)
			::Msg("! [coop] levels: map server %u's record of op %u:%u (%u bytes) is not kept (%u so far)", mapserver::bridge_index(from),
			    body.origin, body.op, u32(body.answer.size()), g.done_refused);
		wire::LevelJournalAckMsg::Refusal f;
		f.seq = seq;
		f.op = wire::LevelEntityHead::Done;
		f.id = wire::kNoObject;
		f.why = journal::Refused;
		refusals.push_back(f);
		++g.entries_refused;
		break;
	}
	}
}

// A map server's journal batch. Read whole first (LevelJournalMsg::read refuses a malformed batch whole: nothing of it is
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
	// A map server still Starting (in session, before its Ready) may make objects while it loads its world: its batches are
	// applied and answered like any other, the writer rule refuses what it may not write yet and its repair settles it. A bridge no
	// map names any more is on its way out: what was on its way counts for nothing (as LevelXferUp).
	if (!journal::applies_batch_of(g.keeper.connected(from), mapserver::bridge_of(mapserver::level_of_bridge(from)) == from))
	{
		++g.batches_late;
		return;
	}
	if (!g.keeper.connected(from))
		++g.batches_starting;
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
			case Head::Carry: // the module carry has no consumer: counted, said once
				if (g.carry_in++ == 0)
					::Msg("* [coop] levels: journal Carry heard from map server %u, nobody takes it yet (R32 WP10)", mapserver::bridge_index(from));
				continue;
			case Head::Done: // the level's done-record of a numbered op, the first answer standing
				++g.done_in;
				host_take_done(from, e, m.seq, refusals);
				continue;
			case Head::Flushed: // no flush round waits for it: counted, said once
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

accept::BodyDecision host_take_body(PeerId from, LevelId level, Reader& r, const accept::Facts& f)
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
	accept::BodyFacts b;
	b.read = r.ok();
	b.known = e != nullptr;
	b.online_here = e && e->m_bOnline;
	b.player_character = e && world::is_player_character(e);
	b.body_level = level_of_vertex(graph);
	b.sender_level = level;
	b.seat_level = level_of_player(join::peer_of_proxy(proxy));
	b.position_valid = _valid(pos[0]) && _valid(pos[1]) && _valid(pos[2]);
	// the run of this character's crossed reports: only a crossing starts one (a Take ends it, a session end clears them all)
	const bool had_run = g.crossings.count(proxy) != 0;
	const accept::BodyDecision d = accept::body_report(f, b, g.crossings[proxy], mgr().now_ms());
	if (!had_run && d.outcome != accept::BodyOutcome::Crossed)
		g.crossings.erase(proxy);
	if (d.outcome == accept::BodyOutcome::Refused && d.by_acceptance)
		return d; // the caller's refusal line
	++g.body_reports;
	if (d.outcome == accept::BodyOutcome::Crossed)
	{
		// honest: he left that level, its map server hears it from the next LevelMap
		if (++g.body_crossed <= 3)
			::Msg("[coop] levels: a body report of object %u crossed his move: of level %u, he is on %u now, dropped", u32(proxy),
			    u32(b.body_level), u32(b.seat_level));
		return d;
	}
	if (d.outcome != accept::BodyOutcome::Taken)
	{
		if (++g.body_refused <= 3)
			::Msg("! [coop] levels: a body report of object %u refused: read %d, here %d, online %d, level %u of %u, seat %u, sender "
			      "writes him %d, off his seat %llu ms",
			    u32(proxy), b.read ? 1 : 0, b.known ? 1 : 0, b.online_here ? 1 : 0, u32(b.body_level), u32(level), u32(b.seat_level),
			    accept::sender_writes(f) ? 1 : 0, d.crossed_ms);
		return d;
	}
	g.crossings.erase(proxy);
	Fvector p, a;
	p.set(pos[0], pos[1], pos[2]);
	a.set(ang[0], ang[1], ang[2]);
	entities::place_offline(e, p, a, node, graph);
	State::Body& body = g.bodies[proxy];
	body.facts.fight = facts.fight != 0;
	body.facts.talking = facts.talking != 0;
	// The NPC of his talk there, while he talks, and only an object this world has (a creature of that level
	// is an object of the world under the same id)
	const u16 partner = wire::kept_partner(facts);
	body.facts.partner = partner != wire::kNoObject && ai().alife().objects().object(partner, true) ? partner : wire::kNoObject;
	body.facts.place = facts.place;
	body.facts.source = facts.source;
	// The footing is kept only when it names a base of this world on the level of its sender
	// (sources::host_footing: refused ones are counted, the first few logged); else he stands on no base.
	// The body position of this same report must stand on that base's ground too.
	body.facts.footing =
	    checkpoints::sources::host_footing(facts.footing, level, pos) ? facts.footing : checkpoint::base::Footing();
	body.at = mgr().now_ms();
	body.level = level; // body_facts answers only while he is on this level
	return d;
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
	// Fresh, and told by the map server of the level he is on now (a report of the level he has
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

// A map server hears the world's answer to its journal. Every batch up to `upto` is applied but the rows; each row is
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
					g.ids.reserve(f.id); // refused ids stay reserved
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
			{
				// The object as this map server has it (its section, its parent in the world's numbering)
				const CSE_ALifeDynamicObject* mine =
				    f.id != wire::kNoObject && ai().get_alife() ? ai().alife().objects().object(join::to_local(f.id), true) : nullptr;
				std::string note = journal::object_note(mine != nullptr, mine ? mine->s_name.c_str() : nullptr, 0,
				    mine && mine->ID_Parent != 0xffff ? join::to_host(mine->ID_Parent) : wire::kNoObject, false, wire::kNoObject);
				if (f.why == journal::Identity && mine && f.op != wire::LevelEntityHead::Parent)
				{
					// The gen this map server names the object's root with NOW, next to the world's line (root gen here, sent:
					// the gen of the head is there, this side keeps no copy of it). Not for a Parent: its head named the root the
					// thing left, and by now it stands in its new root (a player's character: always 0), so the number says nothing.
					char gen[48];
					std::snprintf(gen, sizeof(gen), ", root gen now %u", held_gen_of(mine));
					note += gen;
				}
				::Msg("! [coop] levels: the world refused %s of object %u in batch %u (%u so far): %s; %s", journal::op_name(f.op), u32(f.id),
				    f.seq, g.journal_refusals, journal::why_name(f.why), note.c_str());
			}
		}
	}
	// outside the lock: a release runs the hooks (xfer_on_released), which take it
	for (const u16 id : release)
	{
		CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(join::to_local(id), true) : nullptr;
		if (!e || world::is_player_character(e))
			continue;
		// what its holder registered (its own lock, whatever world the writer rule asks of): a unit it holds goes out on the
		// transfer lane, or its SpawnPrepare would register a unit the world never had
		bool held = false;
		{
			xrCriticalSectionGuard guard(g.lock);
			held = g.holder && g.holder->authors(id);
		}
		if (held)
			alife_rw().release(e, true); // a unit it holds: its Destroy goes on the transfer lane and clears the world's ledger
		else
		{
			// The world has no such object: no Destroy of it or of what it holds goes up (xfer_on_released skips them), but this
			// level's players are told (no ClientApply: host_records relays the release to them)
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

// The done-records this map server's replay raised reached its Lua (or it raised none); until then
// mapserver::server_frame does not say Ready, so no player it lets in retries an op ahead of the ledger's record. Main thread.
bool xfer_replay_heard()
{
	xrCriticalSectionGuard guard(g.lock);
	return g.replay_gate.settled();
}

// The Done entry goes into g.up behind the ups of the op's effect its hooks queued so far, so client_flush_up
// writes it after them into the same batch (never straight into g.batch, ahead of them). No holder of this world yet (its first frame
// clears g.up) or the hand-over over (logic_hand_over sent the last batch): it would never go up, so the ledger keeps the answer here.
bool journal_done(uint32_t origin, uint32_t seq, const std::string& answer)
{
	if (!is_map_server() || !journal::done::valid(origin, seq, answer.size()))
		return false;
	xrCriticalSectionGuard guard(g.lock);
	if (!holder_writes() || g.logic_over)
		return false;
	State::Up u{wire::LevelEntityHead::Done, wire::kNoObject, wire::kNoObject, journal::ObjectHead(), answer};
	u.origin = origin;
	u.seq = seq;
	g.up.push_back(std::move(u));
	++g.done_queued;
	return true;
}

void xfer_install_server(Session& s)
{
	s.on(Msg::LevelXferUp, &host_on_xfer_up);
	s.on(Msg::LevelEntity, &host_on_level_entity);
	s.on(Msg::LevelJournal, &host_on_level_journal);
	s.on(Msg::LevelHash, &host_on_level_hash);
}

void xfer_install_client(Endpoint& e)
{
	e.on(Msg::LevelXferDown, &client_on_xfer_down);
	e.on(Msg::LevelResync, &client_on_resync);
	e.on(Msg::LevelJournalAck, &client_on_journal_ack);
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
	g.received.clear(); // every bridge numbers its journal from 1 again
	g.id_grants.clear(); // a new world gives its blocks again
	g.done.clear(); // the store of the world's save comes back on the next frame (host_adopt_done: done_loaded is kept)
	g.done_at.clear();
	g.done_kept = 0;
	g.done_repeats = 0;
	g.done_refused = 0;
	g.book.clear();
	g.round = logic::Round();
	g.logic_over = false;
	g.logic_told = 0;
	g.logic_told_bytes = 0;
	xrCriticalSectionGuard guard(g.lock);
	g.holder.reset();
	g.replay_gate.reset(); // no replay waits for a Ready of a session that is over
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
	g.memory.begin();
	g.quiet.clear();
	g.departed.clear();
	g.releasing.clear();
	g.batch.reset();
	g.journal_acked = 0;
	g.ids_slice = 0; // the pool is made again for the next world's slice
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
	g.received.forget(u32(peer)); // its next connection numbers its journal from 1
	host_forget_id_blocks(peer);
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
	// The done-records of every level. The save a starting map server is served (serve_bridge) is its snapshot:
	// it raises those of its level (client_replay_done); the world server takes them back at its next load (host_adopt_done).
	if (!g.done.empty())
	{
		Writer w(4096);
		g.done.write(w);
		out.open_chunk(kDoneChunk);
		out.w(w.ptr(), u32(w.size()));
		out.close_chunk();
	}
}

void xfer_load(IReader& in)
{
	g.journal.clear();
	// The done-records of this save (none: an empty store), for the frame that takes them (host_adopt_done, client_replay_done).
	// Only the world server (-coop_dedicated, from its start) and a map server take them (journal::done::loads_records); a co-op process
	// and a player's machine never look for the chunk and have nothing pending. A chunk the file cuts short is handed over as unreadable.
	const bool reads = journal::done::loads_records(dedicated::on(), is_map_server());
	const size_t done = reads ? in.find_chunk(kDoneChunk) : 0;
	const bool whole = done && in.elapsed() >= 0 && done <= size_t(in.elapsed());
	g.done_loaded.load(reads, whole ? static_cast<const uint8_t*>(in.pointer()) : nullptr, done);
	const size_t size = in.find_chunk(kJournalChunk);
	if (!size || size > xfer::kJournalMaxBytes)
		return;
	g.journal.resize(size);
	in.r(&g.journal[0], u32(size));
}

namespace
{
// The world server: the store of the save it loaded replaces its own (a load is a world of its own). Its Lua ledger loads the same save's
// m_data.coop_ops_done itself: nothing is raised. Main thread, before the frame's journals.
void host_adopt_done()
{
	const size_t bytes = g.done_loaded.bytes();
	const journal::done::Loaded::Took took = g.done_loaded.adopt(g.done); // nothing pending: Nothing, the store as it is
	if (took == journal::done::Loaded::Took::Nothing)
		return;
	g.done_at.clear(); // the marks of the store it replaced
	if (took == journal::done::Loaded::Took::Unreadable)
		::Msg("! [coop] levels: the done-records of this save (%u bytes) do not read: none kept", u32(bytes));
}

// A map server, on the first frame of its world (xfer_client_frame, the holder made for it; levels::frame runs it before
// mapserver::server_frame, which says Ready, so before any player of its level is served): every record of its level in the save it was
// served (its snapshot) goes to its Lua as coop_op_done(origin, seq, answer), in the order the world kept them (coop_srv_mode.script ->
// coop_ops_done.heard: a retry of such an op gets the recorded answer and runs nothing; a record its ledger loaded from the same save's
// m_data already is no news). The events wait in the engine's queue until its Lua is up. Under g.lock.
void client_replay_done()
{
	if (!is_map_server())
		return;
	const size_t bytes = g.done_loaded.bytes();
	const LevelId level = mapserver::key().level;
	std::vector<journal::done::Record> records;
	switch (g.done_loaded.replay(level, records))
	{
	case journal::done::Loaded::Took::Nothing: return; // its world's save was taken already (one replay per world load)
	case journal::done::Loaded::Took::Unreadable:
		::Msg("! [coop] levels: the done-records of the world's save (%u bytes) do not read: none raised", u32(bytes));
		return;
	case journal::done::Loaded::Took::Taken: break;
	}
	for (const journal::done::Record& rec : records)
	{
		script::CallbackArg answer;
		answer.kind = script::CallbackArg::Str;
		answer.s = rec.answer; // whole: an answer may hold any byte (cb_str stops at a zero)
		script::queue_callback("coop_op_done", {script::cb_int(s64(rec.origin)), script::cb_int(s64(rec.seq)), answer});
		++g.done_replayed;
	}
	if (!records.empty())
	{
		g.replay_gate.queued(Device.dwFrame); // Ready waits for a later frame whose script delivery handed them to Lua
		::Msg("[coop] levels: %u done-record(s) of level %u from the world's save handed to the ledger", u32(records.size()), u32(level));
	}
}
} // namespace

void xfer_host_frame()
{
	PROF_EVENT("coop/coop_levels_xfer/xfer_host_frame");
	if (!world_up() || !ai().get_alife())
		return;
	host_adopt_done(); // the done-records of the save it loaded
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
			g.id_grants.clear(); // the blocks of an earlier world are nobody's
		host_reserve_leader_ids(g.reserved_world != join::world_generation());
		host_keep_id_blocks(); // and the blocks given to the map servers
		g.reserved_world = join::world_generation();
		g.reserve_next = now + kReserveEveryMs;
	}
	send_down(g.keeper.tick(now));
	host_apply_changes();
	host_feed(now);
}

void xfer_client_frame()
{
	PROF_EVENT("coop/coop_levels_xfer/xfer_client_frame");
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
			g.batch.reset(); // what the old world queued means nothing in the next
			g.journal_acked = 0;
			g.ids_slice = 0; // the pool of the next world (leader_free_id makes it)
			g.refused_until.clear();
			g.settling.clear();
			g.watched.clear();
			g.pinned.clear();
			g.recalls.clear();
			g.quiet.clear();
			g.departed.clear();
			g.releasing.clear();
			client_remember_held(); // nothing held yet in this world
			g.replay_gate.reset(); // a new world's replay arms it again
			client_replay_done(); // its level's done-records from the world's save, before it says Ready and serves anybody
		}
		// Was this frame's script delivery able to run (coop_script.cpp lua_ready(): the level and the actor,
		// which world_up() above reads, and a live session)? Then it handed the replay of an earlier frame to Lua whole.
		g.replay_gate.frame(Device.dwFrame, net::live());
		client_flush_up(); // what the hooks queued since the last frame, before this frame's transfers
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
	client_send_all(); // one LevelJournal for the frame's facts (send_up sent the part before a transfer already)
	client_remember_held(); // where things stand under the held units now, for the hooks until the next frame
}

// The stock hooks ask the session's mode (xrCoop/coop_mode.h); only a map server's mode (srv/coop_srv_mode.cpp) calls
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
		client_send_all(); // the last looks and the facts queued before them go up now, before the hand-over goes on
	}
	g.logic_over = true;
	::Msg("[coop] levels: what the objects of this level keep: %u said of %u watched, %llu bytes", g.logic_told, u32(g.book.watched()), g.logic_told_bytes);
}

bool logic_looking() { return g.looking; }

bool logic_came_since(LevelId level, u64 at)
{
	const auto it = g.logic_at.find(level);
	// A done-record of the level after that save too: the level's next map server gets it only in a fresh save (xfer_save)
	const auto done = g.done_at.find(level);
	return logic::save_is_stale(at, it != g.logic_at.end(), it != g.logic_at.end() ? it->second : 0) ||
	    logic::save_is_stale(at, done != g.done_at.end(), done != g.done_at.end() ? done->second : 0);
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

// The writer rule asks it from any thread (ALife runs on its own, mt_alife): under the transfers' lock, and only of a holder
// of this world (holder_writes: also through its hand-back).
bool xfer_authors(u16 local_id)
{
	xrCriticalSectionGuard guard(g.lock);
	return holder_writes() && g.holder->authors(join::to_host(local_id));
}

bool xfer_born(u16 local_id)
{
	if (!ai().get_alife())
		return false;
	xrCriticalSectionGuard guard(g.lock);
	if (!holder_writes() || g.holder->authors(join::to_host(local_id)) || !journal::spawn_queued(g.up, local_id))
		return false;
	const CSE_ALifeDynamicObject* e = ai().alife().objects().object(local_id, true);
	return e && transfers(e);
}

bool xfer_made_here(u16 local_id)
{
	if (!is_map_server())
		return false;
	const u16 first = leader_ids_of(mapserver::bridge_index(mgr().self()));
	xrCriticalSectionGuard guard(g.lock);
	return id_is_senders(local_id, first, IdGrants(), 0) || (g.ids_slice == first && g.ids.owns(local_id));
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

bool host_adopt_member(PeerId bridge, u16 squad, u16 member, const std::vector<u8>& state)
{
	if (!mgr().is_host() || !g.keeper.adopt(member, squad, u32(bridge), state))
		return false;
	host_apply_changes(); // its Mark: it is marked online already as the squad's member (mark_object changes nothing)
	return true;
}

void host_member_joined(PeerId bridge, u16 member)
{
	if (mgr().is_host())
		g.keeper.drop_own(member, u32(bridge), mgr().now_ms());
}

// May `bridge`'s map server make an object under `id`? Of its slice, or of a block this world gave it for an
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
	g.releasing.released(local_id); // the Left its release raises next adopts nothing
	// A child released with its holder: the stock release raised the holder's hook first (its Destroy is queued), and the world's
	// release of that holder takes this child too (journal::journals_destroy)
	const CSE_ALifeDynamicObject* released = ai().get_alife() ? ai().alife().objects().object(local_id, true) : nullptr;
	const bool with_holder = released && released->ID_Parent != 0xffff && g.releasing.under_way(released->ID_Parent);
	if (!journal::journals_destroy(g.quiet.count(local_id) != 0, with_holder))
	{
		if (with_holder && !g.quiet.count(local_id) && ++g.destroys_with_holder <= kWithHolderLines)
		{
			const CSE_ALifeDynamicObject* holder = ai().alife().objects().object(released->ID_Parent, true);
			::Msg("[coop] levels: the Destroy of %u [%s] is left out: released with its holder %u [%s], whose release the world runs "
			      "(%u so far%s)",
			    u32(local_id), released->name(), u32(released->ID_Parent), holder ? holder->name() : "?", g.destroys_with_holder,
			    g.destroys_with_holder == kWithHolderLines ? "; the rest only count in srv_levels_status" : "");
		}
		return; // a create the world refused, released by its repair: the world never had it; or one its holder's release takes
	}
	// The head now, while the object is still there (the frame's flush finds it gone). A member a script's remove_npc took out
	// of its squad first names that squad and goes before the Left of it, which the world's release makes needless
	State::Up u{wire::LevelEntityHead::Destroy, local_id, 0xffff, head_of(local_id), {}};
	const journal::CreaturePlace p = place_of_creature(local_id, u.head);
	g.up.insert(g.up.begin() + ptrdiff_t(p.at), u);
	if (p.before_left)
	{
		g.up.erase(g.up.begin() + ptrdiff_t(p.at + 1));
		client_unadopt(local_id); // the world releases it from its squad: the flush journals its Destroy (no transfer lane)
	}
}

void up_parent(u16 item, u16 new_parent)
{
	if (!is_map_server() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g.lock);
	// An object of this map server's ALife only (journal::journals_parent): an xrServer entity outside it (an NPC's bolt) is no object of the
	// world's, and a refused create's repair releases what the world never had
	const bool registered = ai().get_alife() && ai().alife().objects().object(item, true) != nullptr;
	if (!journal::journals_parent(registered, g.quiet.count(item) != 0))
	{
		if (!registered)
			++g.parents_unregistered;
		return;
	}
	// The hook comes after the move; the head names the root before it, the root the world still sees
	const journal::ParentBefore before = g.memory.moved(item, new_parent);
	g.up.push_back(State::Up{wire::LevelEntityHead::Parent, item, new_parent, head_before_move(item, before), {}, before});
}

void up_died(u16 victim, u16 killer)
{
	if (!is_map_server() || entities::client_applying() || victim == 0)
		return;
	xrCriticalSectionGuard guard(g.lock);
	// The death chain took a member out of its squad before this hook; the world has not run it yet. The head
	// names that squad, and the Died goes before the Left the member hook queued (the world's own death chain takes it out after)
	State::Up u{wire::LevelEntityHead::Died, victim, killer, head_of(victim), {}};
	const journal::CreaturePlace p = place_of_creature(victim, u.head);
	g.up.insert(g.up.begin() + ptrdiff_t(p.at), u);
	if (p.before_left)
		client_unadopt(victim); // it dies in its squad on the world, its body is nobody's unit
}

void up_spawned(u16 id)
{
	if (!is_map_server())
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.releasing.spawned(id); // a new object under the id, whoever made it: nothing of it is on its way out
	if (entities::client_applying())
		return;
	g.up.push_back(State::Up{wire::LevelEntityHead::Spawn, id, 0xffff, head_of(id), {}});
}

// A member joined or left a squad of its level (stock register_member / unregister_member, through
// LevelSide::level_entity_up Joined / Left).
void up_member(u16 squad, u16 member, bool joined)
{
	if (!is_map_server() || entities::client_applying() || squad == 0xffff || member == 0xffff || squad == member)
		return;
	xrCriticalSectionGuard guard(g.lock);
	if (g.quiet.count(member))
		return; // a create the world refused, released by its repair: the world never had it, nor its memberships (as up_parent)
	g.up.push_back(State::Up{joined ? wire::LevelEntityHead::Joined : wire::LevelEntityHead::Left, squad, member, journal::ObjectHead(), {}});
	if (!joined)
		client_adopt_left(squad, member, mgr().now_ms()); // it inherits the squad's hold now, before any later hook names it
}

// An info portion of a non-actor object it writes was given or taken (LevelSide::level_info_up).
void up_info(u16 id, const char* info, bool on)
{
	if (!is_map_server() || entities::client_applying() || !info || !*info)
		return;
	xrCriticalSectionGuard guard(g.lock);
	g.up.push_back(State::Up{wire::LevelEntityHead::Info, id, u16(on ? 1 : 0), journal::ObjectHead(), info});
}

// The state of an item it writes changed (LevelSide::level_item_facts_up); the facts are read at the frame's flush.
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
	Writer w(16); // the journal's Money (no head: a player's book is no object)
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

// The next id of this map server's pool (its slice and the world's blocks), 0xffff when none is free: never an id of the stock
// generator by this function. The pool is made for the slice of this map server's number the first time and again when that number
// changes or the world does (xfer_client_frame); a low pool asks the world for a block (client_ids), an empty one at its next look.
// No fallback: the create is not refused here but at its start. A script create, a spawn
// packet and Process_spawn ask xrServer::CoopMayCreate first, which refuses under coop_writer_gate = enforce (nothing is created) and
// lets it go on under log. What still reaches PerformIDgen with an empty pool (a create under log, a stock chain that
// already runs such as an object's supplies, a create of coop's own C++) gets the stock generator's id through the logged
// CoopTookStockId, never silently: the world refuses its Spawn (IdRange) and the repair releases it one round trip later.
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
		::Msg("! [coop] levels: every id of this map server's pool is in use (%u ids, %u reserved): a create is refused under "
		      "coop_writer_gate enforce, else it takes a stock id, which the world refuses (W9)",
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
		host_accept_print_status(); // the journal's entries taken and refused
		::Msg("[coop] levels: journal batches %u (repeated %u, after a gap %u, malformed %u, late %u, before Ready %u), entries %u, refused %u; "
		      "carry %u, flushed %u (no consumer yet); id asks %u, blocks given %u; LevelEntity facts not applied %u",
		    g.batches_in, g.batches_repeated, g.batches_gap, g.batches_malformed, g.batches_late, g.batches_starting, g.entries_in,
		    g.entries_refused, g.carry_in, g.flushed_in, g.id_asks_in, g.id_blocks_out, g.legacy_entity);
		::Msg("[coop] levels: done-records heard %u: kept %u, repeats %u, refused %u; %u level(s) keep records, %u went for the bounds",
		    g.done_in, g.done_kept, g.done_repeats, g.done_refused, u32(g.done.levels()), g.done.evicted());
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
	::Msg("[coop] levels: Parent entries left out, back where they stood (a holder or a player's character went offline here): %u",
	    g.parents_unchanged);
	::Msg("[coop] levels: Parent hooks of entities outside this map server's ALife left out (an NPC's bolt: the world never has it): %u",
	    g.parents_unregistered);
	::Msg("[coop] levels: moves out of a player's character no player plays here any more left out: %u, their Destroys %u (the world "
	      "keeps them in him)",
	    g.parents_departed, g.destroys_departed);
	::Msg("[coop] levels: Destroys of objects released with their holder left out (the world's release of the holder takes them): %u",
	    g.destroys_with_holder);
	::Msg("[coop] levels: done-records journaled %u, raised from the world's save %u", g.done_queued, g.done_replayed);
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
