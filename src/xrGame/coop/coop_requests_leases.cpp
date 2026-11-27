// Anomaly Coop - requests: leased ids (xrCoop/coop_id_lease.h).
// The host takes free ids out of its generator and leases kLeaseSize of them to each player in the world (Msg::IdLease),
// a spent one replaced 1:1. The leases belong to this world's generator: another world (join::world_generation) voids
// them, nothing is freed; a player who leaves or loads the world again gives his back.
// The client reserves the ids in its generator at once; an own item a script creates (alife():create into our actor,
// create(.., false) + register, clone_weapon) takes one and is created here at once, registered and online in our
// inventory, and asked for with the same id (ReqSpawn 0xC7). The host's answer: accepted - nothing to do (its
// EntitySpawn finds the item here); RateLimited - asked again; refused - our item goes (rollback) and the id goes back
// (Msg::IdLeaseAck). The client's state is under g_out_lock: Lua creates may run on the game thread, the answers and
// the frame come on the main thread.
#include "stdafx.h"
#include "coop_requests_impl.h"
#include "coop_events.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_entities.h"
#include "coop_alife.h"
#include "coop_items.h"
#include "coop_money.h"
#include "coop_hud_text.h"

#include <algorithm>

#include "Level.h"
#include "xrServer.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/xrMessages.h"
#include "ai_space.h"

namespace coop
{
namespace requests
{
namespace
{
u64 g_last_ack = 0;

// Rolled-back items: released from our ALife and server like any destroyed item
// (on_unregister cleans the mods' data under the id), but their entity is freed only kQuarantineMs later, as a script
// may still hold the se_obj it got from the create - and only in the world it was released in
// (lease::Quarantine).
constexpr u64 kQuarantineMs = 120000;

u32 g_lease_epoch_was = 0; // the epoch before a move of the host's this machine stayed through (lease::epoch_taken)

// What the host's ReqSpawn takes into a player's character (an item class with every line it reads unchecked, not a
// launcher's rocket, no default upgrades its own tree refuses - FATAL on a new object here as there): anything else
// takes the request path and its refusal.
bool lease_section_ok(const char* section)
{
	if (!section || !spawnable_section(section))
		return false;
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(section, "class"));
	return rule && spawn::character_takes(rule->game) && !missing_item_line(section, *rule).line &&
	    items::section_defaults_ok(shared_str(section));
}

void give_back(u16 id)
{
	xrCriticalSectionGuard guard(g_out_lock);
	g_give_back.push_back(id);
}

// Our holder is online, so the new item comes online in it at once: the steps of entities' bring_child_online for an
// item the host made (add_online_impl for a child).
void bring_online_in_holder(CSE_ALifeDynamicObject* e)
{
	xrServer& server = alife_rw().server();
	CSE_ALifeDynamicObject* holder = ai().alife().objects().object(e->ID_Parent, true);
	if (!holder || !holder->m_bOnline || !server.ID_to_entity(e->ID_Parent))
		return; // stays offline in its holder, comes online with it
	ClientID client;
	client.set(server.GetServerClient() ? server.GetServerClient()->ID.value() : 0);
	NET_Packet packet;
	e->s_flags.or(M_SPAWN_UPDATE);
	CSE_Abstract* abstract = e;
	server.entity_Destroy(abstract); // frees the id; ALife keeps the entity (m_bALifeControl)
	e->o_Position = holder->o_Position;
	e->m_tNodeID = holder->m_tNodeID;
	server.Process_spawn(packet, client, FALSE, e);
	e->s_flags.and(u16(-1) ^ M_SPAWN_UPDATE);
	e->m_bOnline = true;
}

bool temp_clone_source_locked(u16 id)
{
	for (const auto& it : g_temp_leases)
		if (it.second.source == id)
			return true;
	return false;
}

// These items are already being removed or replaced locally and cannot take another mutation.
bool replacement_fenced_locked(u16 id)
{
	if (std::find(g_give_back_wait.begin(), g_give_back_wait.end(), id) != g_give_back_wait.end())
		return true;
	if (g_clone_prepared.count(id) || temp_clone_source_locked(id))
		return true;
	for (const auto& clone : g_clone_prepared)
		if (join::to_local(clone.second.source) == id)
			return true;
	return false;
}

bool host_world_ready() { return world_up() && Level().Server && !Device.dwPrecacheFrame; }
} // namespace

// ---------------------------------------------------------------- the client
lease::ClientPool g_pool; // ids reserved in our generator, ready for a create

lease::SpecBook g_specs; // our creates the host has not answered yet
xr_map<u16, Action> g_spec_actions; // the request of each of them, sent again after RateLimited
xr_map<CSE_Abstract*, LeasedTemp> g_temp_leases; // alife():create / clone_weapon (.., false) objects that hold an id
xr_vector<u16> g_give_back; // ids for the next IdLeaseAck
xr_vector<u16> g_give_back_wait; // ids of refused creates whose object is still here (its destroy takes a frame)
u32 g_pool_world = 0; // join::world_generation() the ids are reserved in

// Requests about our items the host has not created yet wait for its answer (lease::Held)
lease::Held<Action> g_held;

RemovalBook g_removals;
lease::DestroyPacer g_destroy_pacer; // ReqDestroy and ReqUse share the decider's bucket

// Our ReqSpawn go no faster than the host takes them; the rest waits here, in order
lease::SpawnPacer g_spawn_pacer;

xr_deque<Action> g_spawn_backlog;
LeaseCounters g_lc;
xrCriticalSection g_quarantine_lock; // entity_Destroy runs where the release runs
xr_set<CSE_Abstract*> g_quarantine_marks;
lease::Quarantine<CSE_Abstract*> g_quarantine;

// The item a Spawn creates does not depend on itself, only its holder.
std::array<u16, 3> lease_targets(const Action& a)
{
	switch (a.kind)
	{
	case Act::Spawn:
	case Act::Destroy:
	case Act::Use:
	case Act::ItemEvent: return {a.item, kNoId, kNoId};
	case Act::Move: return {a.item, a.from, a.to};
	case Act::Hit:
	case Act::Died: return {kNoId, kNoId, kNoId};
	}
	return {kNoId, kNoId, kNoId};
}

// Mark and discard under the same lock as the SpecBook rollback, before Lua or destruction can reenter the outbox.
void mark_lease_rollback_locked(u16 id, xr_vector<u32>& holds)
{
	g_give_back_wait.push_back(id);
	g_clone_slots.refused(id, mgr().now_ms());
	const auto mentions = [id](const Action& a)
	{
		const auto targets = lease_targets(a);
		return a.lease == id || std::find(targets.begin(), targets.end(), id) != targets.end();
	};
	const auto drop = [&holds](const Action& a)
	{
		if (a.hold)
			holds.push_back(a.hold);
		if (is_removal(a.kind))
			g_removals.forget(a.item, a.removal_entity);
	};
	g_lc.held_dropped += g_held.drop_matching(id, mentions, drop);
	const auto discard = [&](const Action& a)
	{
		if (!mentions(a))
			return false;
		drop(a);
		return true;
	};
	g_outbox.erase(std::remove_if(g_outbox.begin(), g_outbox.end(), discard), g_outbox.end());
	g_spawn_backlog.erase(std::remove_if(g_spawn_backlog.begin(), g_spawn_backlog.end(), discard), g_spawn_backlog.end());
}

// world_up, not g_pGameLevel (set when a level's construction starts) and a simulator that may be half built
bool lease_world_ready() { return is_live_mirror() && world_up() && Level().Server; }

// The ids belong to the generator of the world they were reserved in: another world here (the host's load, a rejoin of
// ours: join::world_generation) leaves nothing of them, nor of the creates in flight. Under g_out_lock.
void lease_sync_world_locked()
{
	const u32 world = join::world_generation();
	if (world == g_pool_world)
		return;
	g_pool_world = world;
	g_pool.clear();
	g_zones.clear(); // their objects went with the old world's ALife
	g_specs.clear();
	g_clone_slots.clear();
	g_clone_prepared.clear();
	g_clone_confirmed.clear();
	g_clone_canonical.clear();
	g_spec_actions.clear();
	g_give_back.clear();
	g_give_back_wait.clear();
	for (auto& it : g_temp_leases)
		it.first->ID = kNoId; // still a temporary object (g_lua_temps): its register takes the request path
	g_temp_leases.clear();
	g_held.clear([](const Action& a) { money::client_release(a.hold); });
	g_removals.clear();
	g_destroy_pacer.reset();
	g_spawn_backlog.clear();
	g_spawn_pacer.reset();
	xrCriticalSectionGuard guard(g_quarantine_lock);
	g_quarantine_marks.clear(); // their entities went with the old world, or wait in the quarantine
}

// A leased id for a new item of `section` in `parent` (local numbering), kNoId for the request path: not live, not
// into our actor, a section the host refuses there, no id left.
u16 take_lease(const char* section, u16 parent)
{
	if (parent != kActor || !lease_world_ready() || !lease_section_ok(section))
		return kNoId;
	xrCriticalSectionGuard guard(g_out_lock);
	lease_sync_world_locked();
	return g_pool.take();
}

// The name CALifeSimulatorBase::spawn_item gives a new object: its section and its id, at least 4 digits (the host
// names a leased item the same way).
void name_with_id(CSE_Abstract* e)
{
	string256 name;
	xr_sprintf(name, "%s%04u", e->s_name.c_str(), u32(e->ID));
	e->set_name_replace(name);
}

// `e` (a temporary object of make_temp, its parent our actor) becomes our item now under leased `id`: registered in our
// ALife, online in our inventory, asked for with the same id. `source`: the weapon a clone replaces (local numbering).
// False: not created (`e` untouched; the caller gives the id back).
bool create_leased(CSE_Abstract* e, u16 id, u16 source)
{
	CSE_ALifeDynamicObject* d = smart_cast<CSE_ALifeDynamicObject*>(e);
	CSE_ALifeDynamicObject* holder = lease_world_ready() ? ai().alife().objects().object(e->ID_Parent, true) : nullptr;
	xrServer* server = Level().Server;
	// the id is ours: reserved in our generator (never free) and nothing here holds it
	if (!d || !holder || !server || server->IsFreeID(id) || server->ID_to_entity(id) ||
	    ai().alife().objects().object(id, true))
	{
		++g_lc.not_created;
		if (g_log.allow())
			::Msg("! [coop] our new [%s] could not take its leased id %u here: asked for without it",
			    e->s_name.c_str(), id);
		return false;
	}
	Action a;
	a.kind = Act::Spawn;
	a.item_world = join::world_generation();
	a.item = e->ID_Parent;
	a.pos = holder->o_Position;
	a.section = e->s_name.c_str() ? e->s_name.c_str() : "";
	a.lease = id;
	a.source = source;
	std::vector<u8> state;
	if (source != kNoId)
	{
		const auto prepared = g_clone_prepared.find(id);
		if (prepared == g_clone_prepared.end() || !before_clone_register(e) ||
		    !replacement::encode(prepared->second, state))
			return false;
	}
	else
		items::capture_spawn_state(e, state);
	if (!state.empty())
		a.bytes.assign(state.begin(), state.end());
	if (source != kNoId)
	{
		free_source_slot(id, source); // the clone takes its old weapon's slot
		items::client_made_here(id); // the host's push of it keeps our state
	}
	{
		entities::ClientApply apply; // our own world takes it as the host's would: nothing of it is a request
		e->ID = id;
		name_with_id(e);
		e->ID_Phantom = kNoId;
		d->o_Position = holder->o_Position;
		d->m_tNodeID = holder->m_tNodeID;
		d->m_tGraphID = holder->m_tGraphID;
		d->m_bOnline = false;
		alife_rw().register_object(d, true);
		bring_online_in_holder(d);
	}
	if (source != kNoId && !clone_activate(id))
		a.flags |= kSpawnRiderFailed; // rolled back after the Lua clone wrapper has finished its source clear
	xrCriticalSectionGuard guard(g_out_lock);
	g_specs.add(id, source);
	g_spec_actions[id] = a;
	g_outbox.push_back(std::move(a));
	++g_lc.created;
	g_lc.clones += source != kNoId ? 1 : 0;
	return true;
}

// A temporary object of ours holds a leased id (create / clone_weapon (.., false)): taken out of g_temp_leases.
bool take_temp_lease(CSE_Abstract* e, LeasedTemp& out)
{
	xrCriticalSectionGuard guard(g_out_lock);
	lease_sync_world_locked();
	const auto it = g_temp_leases.find(e);
	if (it == g_temp_leases.end())
		return false;
	out = it->second;
	g_temp_leases.erase(it);
	return true;
}

// A pending clone replaces its source as part of the create. Its release is handled by the clone.
bool clone_source(u16 id)
{
	if (id == kNoId)
		return false;
	xrCriticalSectionGuard guard(g_out_lock);
	return g_specs.is_source(id) || temp_clone_source_locked(id);
}

bool can_queue_mutation_locked(u16 id, bool mirror_live)
{
	if (mirror_live)
		lease_sync_world_locked();
	return g_specs.can_queue_mutation(id, mirror_live, mirror_live && replacement_fenced_locked(id));
}

// Lua also needs to block a second action while a create or a removal is pending.
bool client_lease_waiting(u16 id)
{
	if (id == kNoId)
		return false;
	xrCriticalSectionGuard guard(g_out_lock);
	return g_pool_world == join::world_generation() && (replacement_fenced_locked(id) ||
	    g_specs.waiting(id, g_removals.pending(id, world::server_entity(id))));
}

// A leased id a create could not use. Reserved here and held by nothing: back into the pool (given to
// the host, it could hand it out again while our generator has it taken: FATAL in client_on_spawn). Free here: the host's
// again. Held here by an object (should not happen): neither.
void return_unused_lease(u16 id)
{
	xrServer* server = g_pGameLevel ? Level().Server : nullptr;
	if (!server)
		return;
	if (server->IsFreeID(id))
	{
		give_back(id);
		return;
	}
	if (server->ID_to_entity(id) || (ai().get_alife() && ai().alife().objects().object(id, true)))
		return;
	xrCriticalSectionGuard guard(g_out_lock);
	g_pool.put_back(id);
}

// The host refused our create under `id` (or kept rate-limiting it): our item goes - released from our ALife like any
// destroyed item, its entity kept a while (the quarantine) - and the id goes back to the host once it is gone here.
void rollback_leased(u16 id, const char* why)
{
	after_clone_register(id, false);
	++g_lc.rolled_back;
	CSE_ALifeDynamicObject* e = lease_world_ready() ? ai().alife().objects().object(id, true) : nullptr;
	if (!e)
		return;
	if (g_log.allow())
		::Msg("! [coop] the host refused our new [%s] %u (%s): taken back here", e->s_name.c_str(), id, why);
	{
		xrCriticalSectionGuard guard(g_quarantine_lock);
		g_quarantine_marks.insert(e);
	}
	entities::ClientApply apply; // a destroy of our own world: no request of it goes to the host
	if (e->m_bOnline)
	{
		NET_Packet packet;
		packet.w_begin(M_EVENT);
		packet.w_u32(Level().timeServer());
		packet.w_u16(GE_DESTROY);
		packet.w_u16(id);
		Level().Send(packet, net_flags(TRUE, TRUE));
	}
	else
		alife_rw().release(e, true);
}

// Rolled-back entities are freed kQuarantineMs after their release (all of them when the session goes).
void quarantine_expire(bool all)
{
	xr_vector<CSE_Abstract*> done;
	{
		xrCriticalSectionGuard guard(g_quarantine_lock);
		// one of another world is left: its script object lives in a Lua state that world closed
		g_quarantine.expire(mgr().now_ms(), kQuarantineMs, join::world_generation(), all,
		    [&done](CSE_Abstract* e) { done.push_back(e); }, [](CSE_Abstract*) { ++g_lc.quarantine_left; });
		if (all)
			g_quarantine_marks.clear();
	}
	for (CSE_Abstract* e : done)
		F_entity_Destroy(e);
}

// Client frame, before the outbox goes: the retries due, the ids to give back (paced: kAckGapMs), the quarantine.
void client_pump_leases()
{
	PROF_EVENT("coop/coop_requests/client_pump_leases");
	quarantine_expire(false);
	if (!lease_world_ready())
		return;
	const u64 now = mgr().now_ms();
	clone_slots_frame(now);
	lease::IdsMsg ack;
	xr_vector<u16> unanswered; // creates the host never answered
	xr_vector<u32> holds;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		lease_sync_world_locked();
		const lease::SpecBook::Due due = g_specs.due(now);
		for (u16 id : due.resend)
		{
			const auto it = g_spec_actions.find(id);
			if (it == g_spec_actions.end())
			{
				g_specs.forget(id);
				continue;
			}
			g_specs.sent(id, 0, now); // in our outbox (paced): not due again before it goes out and is answered
			g_outbox.push_back(it->second);
			++g_lc.resent;
		}
		for (u16 id : due.rollback)
		{
			// kMaxResends requests without an answer: no waiting for ever - taken back, and the
			// requests that waited for it go too
			g_spec_actions.erase(id);
			mark_lease_rollback_locked(id, holds);
			unanswered.push_back(id);
		}
		for (auto it = g_give_back_wait.begin(); it != g_give_back_wait.end();)
		{
			if (ai().alife().objects().object(*it, true) || Level().Server->ID_to_entity(*it) ||
			    Level().Objects.net_Find(*it))
				++it; // its destroy has not run yet
			else
			{
				g_clone_slots.removed(*it, now);
				g_give_back.push_back(*it);
				it = g_give_back_wait.erase(it);
			}
		}
		if (!g_give_back.empty() && now - g_last_ack >= lease::kAckGapMs)
		{
			const size_t n = _min<size_t>(g_give_back.size(), lease::kMaxIdsPerMsg);
			for (size_t i = 0; i < n; ++i)
				ack.ids.push_back(join::to_host(g_give_back[i]));
			g_give_back.erase(g_give_back.begin(), g_give_back.begin() + n);
			g_last_ack = now;
		}
	}
	for (u32 hold : holds)
		money::client_release(hold);
	for (u16 id : unanswered)
		rollback_leased(id, "no answer from the host"); // its id goes back to the host (IdLeaseAck) once it is gone
	if (!unanswered.empty())
	{
		g_lc.unanswered += unanswered.size();
		hud_text::say("st_coop_lease_lost", "Co-op: the host did not answer, a new item was taken back");
	}
	if (ack.ids.empty())
		return;
	ack.epoch = mgr().session().world_epoch();
	Writer w(8 + 2 * ack.ids.size());
	ack.write(w);
	net::client_send(Msg::IdLeaseAck, w);
	++g_lc.acks;
	g_lc.given_back += ack.ids.size();
}

// Msg::IdLease: ids added to our lease. Each one is reserved in our generator, or given back when an object of ours holds
// it (a bolt, a fake missile: the same class as R2's "ID already used").
void client_on_lease(PeerId from, Reader& r)
{
	if (from != kHostPeer || mode().decides_world())
		return;
	lease::IdsMsg m;
	if (!m.read(r))
	{
		++g_lc.bad_leases;
		return;
	}
	if (!lease_world_ready() || !lease::epoch_taken(m.epoch, mgr().session().world_epoch(), g_lease_epoch_was))
	{
		++g_lc.stale_leases; // another world's: the host voided them with it
		return;
	}
	xrServer& server = alife_rw().server();
	for (u16& id : m.ids)
		id = join::to_local(id); // identity: a leased id is neither 0 nor our proxy (both are taken in the host's world)
	std::vector<u16> rejected;
	xrCriticalSectionGuard guard(g_out_lock);
	lease_sync_world_locked();
	g_pool.add(mgr().session().world_epoch(), m.ids,
	    [&server](u16 id)
	    {
		    if (!server.IsFreeID(id))
			    return false; // an object of ours holds it
		    server.PerformIDgen(id); // reserved: no object of ours takes it now
		    return true;
	    },
	    rejected);
	g_give_back.insert(g_give_back.end(), rejected.begin(), rejected.end());
}

// ---------------------------------------------------------------- the host
lease::HostLeases g_leases;

HostLeaseCounters g_hl;
void host_sync_leases() { g_leases.set_epoch(join::world_generation()); }

// Ids the leases held go back to this world's generator. An id it has free already is left alone (a second free would
// put it into the free list twice).
void host_free_ids(const std::vector<u16>& ids)
{
	if (!g_pGameLevel || !Level().Server)
		return;
	xrServer& server = *Level().Server;
	for (u16 id : ids)
		if (lease::leasable(id) && !server.IsFreeID(id))
			server.FreeID(id, Device.TimerAsync());
}

// Host frame: every player in the world holds a whole lease (one IdLease per player and frame with what he lacks); a
// player who loads the world again (a rejoin) gives his back: his machine starts a new pool with its new world.
void host_pump_leases()
{
	PROF_EVENT("coop/coop_requests/host_pump_leases");
	host_sync_leases();
	if (!host_world_ready())
		return;
	xrServer& server = *Level().Server;
	for (const PeerInfo& p : mgr().session().peers())
	{
		if (p.state != PeerState::Joined)
			continue;
		// a player on a map server's level has his ids from it: what he spawns goes there, and only its own leases count there
		if (sync::host_peer_joining(p.id) || mode().where(p.id) == Where::Away)
		{
			if (g_leases.count(p.id))
			{
				drop_leased_queued(p.id);
				host_free_ids(g_leases.drop(p.id));
			}
			continue;
		}
		const u16 proxy = join::proxy_of_peer(p.id);
		if (proxy == kNoId || !join::host_player_in_world(proxy))
			continue;
		const size_t want = g_leases.want(p.id);
		if (!want)
			continue;
		std::vector<u16> bad;
		lease::IdsMsg m;
		m.epoch = mgr().session().world_epoch();
		m.ids = g_leases.grant(p.id, want,
		    [&server]() -> u16
		    {
			    // a nearly full world keeps its last blocks (PerformIDgen(0xffff) asserts with none)
			    return server.AvailableIDBlocks() >= lease::kMinFreeBlocks ? server.PerformIDgen(kNoId) : kNoId;
		    },
		    bad);
		for (u16 id : bad)
			if (!g_leases.leased(id))
				server.FreeID(id, Device.TimerAsync()); // picked, not leased
		if (m.ids.empty())
		{
			g_hl.short_of_ids += want == lease::kLeaseSize ? 1 : 0;
			continue;
		}
		Writer w(8 + 2 * m.ids.size());
		m.write(w);
		net::host_send(p.id, Msg::IdLease, w);
		++g_hl.messages;
	}
}

// Msg::IdLeaseAck: ids the player gives back (busy on his machine, or his create was refused) go back to the generator.
void host_on_lease_ack(PeerId from, Reader& r)
{
	if (!mode().decides_here())
		return;
	lease::IdsMsg m;
	if (!m.read(r))
	{
		++g_hl.bad_acks;
		return;
	}
	host_sync_leases();
	if (m.epoch != mgr().session().world_epoch() || !host_world_ready())
		return; // another world's: voided with it
	++g_hl.acks;
	std::vector<u16> back;
	for (u16 id : m.ids)
	{
		if (g_leases.give_back(from, id))
			back.push_back(id);
		else
			++g_hl.ack_not_his; // not his, or spent: an entity has it
	}
	g_hl.ack_ids += back.size();
	host_free_ids(back);
}

// A request with a leased id: the id is the asker's (checked before anything touches the generator), it goes into his
// character, and a clone's source is a magazined weapon of his (`source_cost`: its cost, what the clone does not pay).
Reason host_lease_checks(const Req& q, u16 parent, u16 id, u16 source, const char* section, u32& source_cost)
{
	source_cost = 0;
	host_sync_leases();
	if (!g_leases.owns(q.from, id))
	{
		++g_hl.not_his;
		return Reason::NotOwner;
	}
	if (parent != q.proxy)
		return Reason::NotAllowed; // a leased create goes into his character only
	if (source == kNoId)
		return Reason::Ok;
	CSE_Abstract* src = world::server_entity(source);
	if (!src)
		return Reason::Missing;
	if (events::root_of(source) != q.proxy)
		return Reason::NotOwner;
	if (!smart_cast<CSE_ALifeItemWeaponMagazined*>(src))
		return Reason::NotAllowed;
	if (!clone_variant(src->s_name.c_str(), section))
		return Reason::NotAllowed; // a variant of the same weapon only
	source_cost = pSettings->line_exist(src->s_name, "cost") ? pSettings->r_u32(src->s_name, "cost") : 0;
	return Reason::Ok;
}

// The leased create is made (`created` is `id`): the id is the world's now. A clone's source goes with it.
void host_leased_made(const Req& q, u16 id, u16 created, u16 source)
{
	g_leases.spend(q.from, id);
	++g_hl.spawns;
	if (created != id && g_log.allow())
		::Msg("! [coop] ReqSpawn #%u of player #%u was created as %u, not as its leased id %u", q.seq, q.from, created, id);
	if (source == kNoId)
		return;
	++g_hl.clones;
}

void client_keeps_world(u32 epoch)
{
	g_lease_epoch_was = mgr().session().world_epoch();
	g_pool.keep_epoch(epoch);
}

bool keep_released(CSE_Abstract* entity)
{
	if (!entity)
		return false;
	xrCriticalSectionGuard guard(g_quarantine_lock);
	if (!g_quarantine_marks.erase(entity))
		return false;
	g_quarantine.keep(entity, mgr().now_ms(), join::world_generation());
	return true;
}

} // namespace requests
} // namespace coop
