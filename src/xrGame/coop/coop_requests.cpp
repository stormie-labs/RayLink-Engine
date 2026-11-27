// Anomaly Coop - client requests and host arbitration (coop_requests.h). This file: the client's outbox and the answers
// to it, the host's gate of every request (the rate buckets, a joining player, the answer), and the module's install,
// reset, frame and coop_status. Each kind of request has its file (coop_requests_<kind>.cpp, coop_local_zones.cpp); what
// they share is coop_requests_impl.h.
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
#include "coop_trade.h"
#include "coop_items.h"
#include "coop_money.h"
#include "xrCoop/coop_item_rider_rules.h"

#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "WeaponMagazined.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrMessages.h"
#include "../xrEngine/XR_IOConsole.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "ai_space.h"

namespace coop
{
namespace requests
{
namespace
{
constexpr u32 kResultTimeoutMs = 10000;
constexpr u32 kChildSpawnHoldMs = 3000;
constexpr u32 kLogBurst = 20;
constexpr u64 kBadLogFirst = 10;
u64 g_bad[BadCount] = {};

// ---------------------------------------------------------------- client side

struct Inflight
{
	Msg type;
	u16 item;
	u64 sent;
	u32 hold = 0; // released by the answer (or the timeout)
	xr_string silencer;
	std::vector<u8> retry;
	u32 world = 0;
};

xr_map<xr_string, u64> g_child_spawns; // "section#holder" -> time asked (fake missiles, rockets)
xr_map<u32, Inflight> g_inflight;
u32 g_seq = 0;

Action removal_action(const RemovalBook::Request& request)
{
	Action a;
	a.kind = request.type == Msg::ReqUse ? Act::Use : Act::Destroy;
	a.item_world = join::world_generation();
	a.item = request.item;
	a.removal_entity = request.identity;
	return a;
}

void send_action(const Action& a)
{
	if (a.item_world && *a.item_world != join::world_generation())
	{
		money::client_release(a.hold);
		return;
	}
	switch (a.kind)
	{
	case Act::Spawn:
	{
		if (a.flags & kSpawnRiderFailed)
		{
			xr_vector<u32> holds;
			{
				xrCriticalSectionGuard guard(g_out_lock);
				if (!g_specs.forget(a.lease))
					return;
				g_spec_actions.erase(a.lease);
				mark_lease_rollback_locked(a.lease, holds);
			}
			for (u32 hold : holds)
				money::client_release(hold);
			rollback_leased(a.lease, "replacement rider activation failed");
			return;
		}
		const u32 seq = track(Msg::ReqSpawn, a.item);
		Writer w(a.bytes.size() + a.section.size() + 32);
		const float p[3] = {a.pos.x, a.pos.y, a.pos.z};
		if (a.lease != kNoId)
		{
			// we have the item under this id already; the host creates it with the same one or refuses
			spawn::write_leased(w, seq, join::to_host(a.item), join::to_host(a.lease), join::to_host(a.source), p,
			    a.section.c_str(), a.bytes.data(), a.bytes.size());
			xrCriticalSectionGuard guard(g_out_lock);
			g_specs.sent(a.lease, seq, mgr().now_ms());
		}
		else // section + item state, the host builds the entity itself
			spawn::write_state(w, seq, join::to_host(a.item), p, a.section.c_str(), a.bytes.data(), a.bytes.size());
		net::client_send(Msg::ReqSpawn, w);
		++g_cl.sent[0];
		break;
	}
	case Act::Destroy:
	case Act::Use:
	{
		const Msg type = a.kind == Act::Use ? Msg::ReqUse : Msg::ReqDestroy;
		u32 seq;
		{
			xrCriticalSectionGuard guard(g_out_lock);
			if (!g_removals.ready(a.item, a.removal_entity, mgr().now_ms()) ||
			    world::server_entity(a.item) != a.removal_entity)
				return;
			seq = track(type, a.item);
			g_inflight.at(seq).world = join::world_generation();
			g_removals.sent(a.item, seq);
		}
		Writer w(8);
		w.u32(seq);
		w.u16(join::to_host(a.item));
		net::client_send(type, w);
		++g_cl.sent[a.kind == Act::Use ? 2 : 1];
		break;
	}
	case Act::Move:
	{
		if (a.from == a.to)
			return;
		// one side must be ours; moves between other holders are the host's (or an NPC's) business
		if (events::root_of(a.from) != kActor && events::root_of(a.to) != kActor)
		{
			++g_cl.foreign_moves;
			return;
		}
		const Msg type = a.from == kNoId ? Msg::ReqTake : (a.to == kNoId ? Msg::ReqDrop : Msg::ReqTransfer);
		// a weapon of ours leaving our character takes its riders along (the host writes them first)
		const bool from_ours = a.from != kNoId && events::root_of(a.from) == kActor;
		const bool to_ours = a.to != kNoId && events::root_of(a.to) == kActor;
		const std::string carried = riders::carries(smart_cast<CWeapon*>(valid_go(a.item)) != nullptr, from_ours, to_ours,
		                                (a.flags & kMoveLocal) != 0) ?
		    client_carry(a.item) :
		    std::string();
		Writer w(24 + carried.size() + 4);
		w.u32(track(type, a.item));
		w.u16(join::to_host(a.item));
		w.u16(join::to_host(a.from));
		w.u16(join::to_host(a.to));
		const float p[3] = {a.pos.x, a.pos.y, a.pos.z};
		w.vec3(p);
		w.u8(a.flags);
		if (!carried.empty())
			w.str(carried); // optional: a host without it reads up to the flags and ignores the rest
		net::client_send(type, w);
		++g_cl.sent[3];
		break;
	}
	case Act::Hit:
	{
		SHit hit = a.hit;
		if (!client_hit_perks(a.item, hit)) // our perks and skills, before the host sees the hit
			break;
		Writer w(64);
		w.u32(++g_seq); // the host answers hits only when it refuses them
		w.u16(join::to_host(a.item));
		const Fvector me = Actor() ? Actor()->Position() : Fvector().set(0.f, 0.f, 0.f);
		const float p[3] = {me.x, me.y, me.z};
		w.vec3(p);
		write_hit(w, hit, join::to_host(hit.whoID), join::to_host(hit.weaponID));
		net::client_send(Msg::ReqHit, w);
		++g_cl.sent[4];
		break;
	}
	case Act::ItemEvent:
	{
		// tracked, so that the host's answer releases the payment held out of the money shown
		const u32 seq = track(Msg::ReqItemEvent, a.item);
		g_inflight[seq].hold = a.hold;
		Writer w(a.bytes.size() + 16 + ledger::kUpgradePaymentSize);
		w.u32(seq);
		w.u16(join::to_host(a.item));
		w.u16(a.ge_type);
		w.u16(u16(a.bytes.size()));
		w.bytes(a.bytes.data(), a.bytes.size());
		ledger::write(w, ledger::UpgradePayment{join::to_host(a.mechanic), a.paid});
		if (a.ge_type == GE_ADDON_DETACH)
		{
			g_inflight[seq].silencer = a.section;
			g_inflight[seq].retry = w.data();
			g_inflight[seq].world = join::world_generation();
		}
		net::client_send(Msg::ReqItemEvent, w);
		++g_cl.sent[6];
		break;
	}
	case Act::Died:
	{
		Writer w(8);
		w.u16(join::to_host(kActor));
		w.u16(join::to_host(a.item));
		net::client_send(Msg::PlayerDied, w);
		++g_cl.sent[7];
		break;
	}
	}
}

// The create of ours an action waits for - an item it names the host has not made yet (in
// its budget queue, or asked again after RateLimited): sent before it, the host would refuse the action (Missing) and
// make the item later, a duplicate. A leased spawn never waits for itself; a spawn into a pending item does. kNoId: none.
// Under g_out_lock.
u16 held_by_locked(const Action& a)
{
	const auto targets = lease_targets(a);
	return g_specs.first_pending({targets[0], targets[1], targets[2]});
}

void client_flush()
{
	PROF_EVENT("coop/coop_requests/client_flush");
	xr_vector<Action> out;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		out.swap(g_outbox);
		const u64 now = mgr().now_ms();
		const u32 world = join::world_generation();
		const bool can_send_removals = is_live_mirror() && g_pool_world == world;
		if (can_send_removals)
			g_removals.discard_gone(world::server_entity);
		for (auto it = g_child_spawns.begin(); it != g_child_spawns.end();)
			it = now - it->second > kChildSpawnHoldMs ? g_child_spawns.erase(it) : std::next(it);
		xr_vector<Action> paced_out;
		paced_out.reserve(out.size());
		for (Action& a : out)
		{
			if (a.item_world && *a.item_world != world)
			{
				money::client_release(a.hold);
				continue;
			}
			const u16 by = g_specs.size() ? held_by_locked(a) : kNoId;
			if (by != kNoId)
			{
				g_held.hold(by, std::move(a));
				++g_lc.held;
				continue;
			}
			if (a.kind == Act::Spawn)
				g_spawn_backlog.push_back(std::move(a));
			else if (is_removal(a.kind))
			{
				if (can_send_removals && g_removals.ready(a.item, a.removal_entity, now) && g_destroy_pacer.take(now))
					paced_out.push_back(std::move(a));
				else if (g_removals.pending(a.item, a.removal_entity))
					g_outbox.push_back(std::move(a));
			}
			else
				paced_out.push_back(std::move(a));
		}
		while (!g_spawn_backlog.empty() && g_spawn_pacer.take(now))
		{
			paced_out.push_back(std::move(g_spawn_backlog.front()));
			g_spawn_backlog.pop_front();
		}
		g_lc.paced += g_spawn_backlog.empty() ? 0 : 1;
		out.swap(paced_out);
	}
	for (const Action& a : out)
		send_action(a);
	const u64 now = mgr().now_ms();
	for (auto it = g_inflight.begin(); it != g_inflight.end();)
	{
		if (!it->second.silencer.empty() && it->second.world != join::world_generation())
		{
			g_detaching_silencers.erase(it->second.item);
			it = g_inflight.erase(it);
			continue; // Old object ids must never be replayed into a loaded world.
		}
		if (now - it->second.sent > kResultTimeoutMs)
		{
			++g_cl.timeouts;
			if (!it->second.silencer.empty())
			{
				Writer retry;
				retry.bytes(it->second.retry.data(), it->second.retry.size());
				net::client_send(Msg::ReqItemEvent, retry);
				it->second.sent = now;
				++it;
				continue; // Same sequence: a late answer must not create a second addon.
			}
			if (is_removal(it->second.type) && it->second.world == join::world_generation())
			{
				xrCriticalSectionGuard guard(g_out_lock);
				g_removals.answer(it->second.item, it->first, false, false, now);
			}
			money::client_release(it->second.hold); // the next balance from the host decides
			it = g_inflight.erase(it);
		}
		else
			++it;
	}
}

void client_on_result(PeerId, Reader& r)
{
	const u32 seq = r.u32();
	const bool accepted = r.boolean();
	const u8 reason = r.u8();
	const u16 subject = r.u16();
	if (!r.ok())
		return;
	// the host keeps a create of ours in its budget queue - not an answer yet (it comes
	// when the create is made or refused); no watchdog for it
	if (reason == u8(Reason::Queued))
	{
		bool ours = false;
		{
			xrCriticalSectionGuard guard(g_out_lock);
			if (g_pool_world == join::world_generation())
				ours = g_specs.queued(seq);
		}
		g_lc.queued += ours ? 1 : 0;
		g_inflight.erase(seq);
		return;
	}
	// the answer to a create of ours under a leased id
	u16 leased = kNoId;
	lease::Outcome outcome = lease::Outcome::Unknown;
	xr_vector<u32> holds; // money held for upgrades of a rolled-back item
	{
		xrCriticalSectionGuard guard(g_out_lock);
		if (g_pool_world == join::world_generation())
			outcome = g_specs.answer(seq, accepted, reason == u8(Reason::RateLimited), mgr().now_ms(), leased);
		if (outcome == lease::Outcome::Confirmed || outcome == lease::Outcome::RolledBack)
			g_spec_actions.erase(leased);
		if (outcome == lease::Outcome::Confirmed)
		{
			g_clone_slots.confirmed(leased);
			// what waited for the item goes out first with the next flush, in its order (an action
			// that names another pending item waits again there)
			std::vector<Action> back = g_held.release(leased);
			g_outbox.insert(g_outbox.begin(), std::make_move_iterator(back.begin()), std::make_move_iterator(back.end()));
		}
		else if (outcome == lease::Outcome::RolledBack)
			mark_lease_rollback_locked(leased, holds);
	}
	for (u32 hold : holds)
		money::client_release(hold);
	switch (outcome)
	{
	case lease::Outcome::Confirmed:
		++g_lc.confirmed;
		after_clone_register(leased, true);
		items::client_resend_state(leased); // what changed on it while the host had not made it yet
		break;
	case lease::Outcome::Retry: ++g_lc.retried; break;
	case lease::Outcome::RolledBack: rollback_leased(leased, reason_name(Reason(_min<u8>(reason, u8(Reason::Count))))); break;
	case lease::Outcome::Unknown: break;
	}
	Msg type = Msg::ReqResult;
	u16 local_item = kNoId;
	xr_string detached_silencer;
	bool same_world = false;
	bool removal_requeued = false;
	auto it = g_inflight.find(seq);
	if (it != g_inflight.end())
	{
		type = it->second.type;
		local_item = it->second.item;
		detached_silencer = it->second.silencer;
		same_world = it->second.world == join::world_generation();
		if (is_removal(type) && same_world)
		{
			xrCriticalSectionGuard guard(g_out_lock);
			const auto retry = g_removals.answer(local_item, seq, accepted, reason == u8(Reason::RateLimited), mgr().now_ms());
			if (retry)
			{
				g_outbox.push_back(removal_action(*retry));
				removal_requeued = true;
			}
		}
		// an upgrade's answer. The host sends the balance right after it (charged or not), so the money shown
		// stays as it is until then.
		money::client_release(it->second.hold);
		g_inflight.erase(it);
	}
	if (!detached_silencer.empty())
	{
		g_detaching_silencers.erase(local_item);
		if (accepted && same_world)
		{
			CWeaponMagazined* weapon = smart_cast<CWeaponMagazined*>(world::object_here(local_item));
			if (weapon)
			{
				entities::ClientApply apply;
				weapon->Detach(detached_silencer.c_str(), false);
				silencer_state(weapon, true);
				// Send the cleared flag before the player can attach another silencer.
				items::client_send_state_now(local_item);
			}
		}
	}
	if (accepted)
	{
		++g_cl.accepted;
		return;
	}
	++g_cl.rejected;
	if (removal_requeued)
		return; // the next flush sends it when the retry delay and the shared pacer allow
	// our item has the upgrade the host refused; it is built again from the host's
	// entity. Not for Offline / NoProxy: our world is being replaced (or we have no character), and the next world
	// comes with the host's state of our items.
	if (type == Msg::ReqItemEvent && detached_silencer.empty() && reason != u8(Reason::Offline) && reason != u8(Reason::NoProxy))
		client_rebuild_item(local_item);
	if (type == Msg::ReqTake || type == Msg::ReqDrop || type == Msg::ReqTransfer)
		trade::invalidate_views(); // an open box/body window shows the real content again
	if (g_log.allow())
		::Msg("[coop] host refused %s #%u: %s (object %u)", type == Msg::ReqResult ? "request" : msg_name(type), seq,
		    reason_name(Reason(_min<u8>(reason, u8(Reason::Count)))), subject);
}

// ---------------------------------------------------------------- host side

constexpr float kRate[CatCount] = {20.f, 30.f, 30.f, 80.f, 20.f}; // per second
constexpr float kBurst[CatCount] = {40.f, 60.f, 60.f, 160.f, 40.f};

struct Bucket
{
	float tokens = -1.f;
	u64 last = 0;
};

struct PeerLimits
{
	Bucket b[CatCount];
};

xr_map<PeerId, PeerLimits> g_limits;
} // namespace

lua_errors::LogBudget g_log{kLogBurst};
bool note_bad(Bad kind) { return ++g_bad[kind] <= kBadLogFirst; }

CGameObject* valid_go(u16 id)
{
	CGameObject* go = world::object_here(id);
	return go && !go->object_removed() ? go : nullptr;
}

bool is_player(u16 id) { return id == kActor || world::is_player_proxy(id); }

// The router may run on the game thread; sending happens in frame() on the main thread.
xrCriticalSection g_out_lock;

xr_vector<Action> g_outbox;
ClientCounters g_cl;

void push(Action&& a)
{
	xrCriticalSectionGuard guard(g_out_lock);
	g_outbox.push_back(std::move(a));
}

u32 track(Msg type, u16 item)
{
	const u32 seq = ++g_seq;
	g_inflight[seq] = {type, item, mgr().now_ms()};
	return seq;
}

bool is_removal(Act kind) { return kind == Act::Destroy || kind == Act::Use; }
bool is_removal(Msg type) { return type == Msg::ReqDestroy || type == Msg::ReqUse; }
HostCounters g_h;

bool allow(PeerId peer, Cat c)
{
	Bucket& b = g_limits[peer].b[c];
	const u64 now = mgr().now_ms();
	if (b.tokens < 0.f)
	{
		b.tokens = kBurst[c];
		b.last = now;
	}
	b.tokens = _min(kBurst[c], b.tokens + float(now - b.last) * kRate[c] / 1000.f);
	b.last = now;
	if (b.tokens < 1.f)
		return false;
	b.tokens -= 1.f;
	return true;
}

// A leased create waits in the budget queue: not an answer (that comes when it is made or
// refused), so the asker's watchdog does not ask again - a second request with its id would queue twice.
void notify_queued(const Req& q, u16 id)
{
	Writer w(12);
	w.u32(q.seq);
	w.boolean(false);
	w.u8(u8(Reason::Queued));
	w.u16(id);
	net::host_send(q.from, Msg::ReqResult, w);
}

void reply(const Req& q, Reason why, u16 subject)
{
	Writer w(12);
	w.u32(q.seq);
	w.boolean(why == Reason::Ok);
	w.u8(u8(why));
	w.u16(subject);
	net::host_send(q.from, Msg::ReqResult, w);
	++g_h.results[u32(why)];
	if (why != Reason::Ok && g_log.allow())
		::Msg("[coop] refused %s #%u from player #%u: %s (object %u)", msg_name(q.type), q.seq, q.from, reason_name(why),
		    subject);
}

// Common checks of every request. False = drop (already answered when an answer makes sense).
// Precaching: the world was just loaded. The session pump runs before join::frame() notices the
// new world and marks the peers as joining, so requests made in the old world would land here.
bool host_drops_requests_of(PeerId from) { return sync::host_peer_joining(from) || Device.dwPrecacheFrame; }

bool host_begin(PeerId from, Reader& r, Msg type, Req& q)
{
	if (!mode().decides_here())
		return false;
	if (host_drops_requests_of(from))
	{
		++g_h.dropped_joining; // its ids belong to a world that was replaced (load, level change)
		// an answer, or the client waits out kResultTimeoutMs before it shows the truth again. A joining player
		// dropped his requests with his world: this answer would only follow him into the new one.
		if (type != Msg::ReqItemEvent && !sync::host_peer_joining(from)) // host_on_item_event answers its own
		{
			q.from = from;
			q.type = type;
			q.seq = r.u32();
			if (r.ok())
				reply(q, Reason::Busy, kNoId);
		}
		return false;
	}
	q.from = from;
	q.type = type;
	q.seq = r.u32();
	q.proxy = join::proxy_of_peer(from);
	if (!r.ok())
	{
		++g_h.malformed;
		return false;
	}
	if (!g_pGameLevel || !ai().get_alife() || !Level().Server)
	{
		reply(q, Reason::Offline, kNoId);
		return false;
	}
	if (q.proxy == kNoId)
	{
		reply(q, Reason::NoProxy, kNoId);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- API

const char* reason_name(Reason r)
{
	switch (r)
	{
	case Reason::Ok: return "ok";
	case Reason::Malformed: return "malformed";
	case Reason::NoProxy: return "no character";
	case Reason::Missing: return "missing";
	case Reason::Moved: return "somebody was first";
	case Reason::NotOwner: return "not yours";
	case Reason::NotAllowed: return "not allowed";
	case Reason::BadSection: return "unknown section";
	case Reason::RateLimited: return "too many requests";
	case Reason::TooFar: return "too far";
	case Reason::Offline: return "offline";
	case Reason::FriendlyFire: return "friendly fire is off";
	case Reason::Engine: return "engine refused";
	case Reason::Queued: return "waits for the cost budget";
	case Reason::Busy: return "the host is loading a world";
	default: return "?";
	}
}

void init() { load_spawn_settings(); }

void install_server(Session& s)
{
	s.on(Msg::ReqSpawn, &host_on_spawn);
	s.on(Msg::ReqDestroy, [](PeerId from, Reader& r) { host_on_destroy_req(from, r, Msg::ReqDestroy); });
	s.on(Msg::ReqUse, [](PeerId from, Reader& r) { host_on_destroy_req(from, r, Msg::ReqUse); });
	s.on(Msg::ReqTake, [](PeerId from, Reader& r) { host_on_move(from, r, Msg::ReqTake); });
	s.on(Msg::ReqDrop, [](PeerId from, Reader& r) { host_on_move(from, r, Msg::ReqDrop); });
	s.on(Msg::ReqTransfer, [](PeerId from, Reader& r) { host_on_move(from, r, Msg::ReqTransfer); });
	s.on(Msg::ReqHit, &host_on_hit);
	s.on(Msg::ReqItemEvent, &host_on_item_event); // ReqMoney: coop_money
	s.on(Msg::ReqLaunch, &host_on_launch);
	s.on(Msg::IdLeaseAck, &host_on_lease_ack);
}

void install_client(Endpoint& e)
{
	e.on(Msg::ReqResult, [](PeerId from, Reader& r) { if (!mode().decides_world()) client_on_result(from, r); });
	e.on(Msg::IdLease, &client_on_lease);
	e.on(Msg::PlayerHit, [](PeerId from, Reader& r) { if (!mode().decides_world()) client_on_player_hit(from, r); });
}

void reset()
{
	{
		xrCriticalSectionGuard guard(g_out_lock);
		g_outbox.clear();
		g_child_spawns.clear();
		g_launches.clear();
	}
	g_inflight.clear();
	g_detaching_silencers.clear();
	g_silencer_receipts.clear();
	g_silencer_world = 0;
	g_rebuilds.clear();
	g_limits.clear();
	g_spawn_budgets.clear(); // a session starts or ends (kept across a peer leaving and rejoining)
	g_spawn_queue.clear(drop_queued); // nothing of the old session is created
	g_last_launch.clear();
	g_admission.clear(); // held moves of the old session
	g_log = lua_errors::LogBudget{kLogBurst};
	// Lua may still hold these pointers; reset runs when a session ends, long after any script used them. Only
	// those of this world: an old world's Lua state is closed.
	if (g_temps_world == join::world_generation())
		for (CSE_Abstract* e : g_lua_temps)
			F_entity_Destroy(e);
	else
		g_lc.temps_left += g_lua_temps.size();
	g_lua_temps.clear();
	g_lua_forwarded.clear();
	// the session's leases go; in the world they came from, back into its generator
	if (g_leases.total() && g_leases.epoch() == join::world_generation())
		host_free_ids(g_leases.drop_all());
	g_leases.clear();
	{
		xrCriticalSectionGuard guard(g_out_lock);
		g_pool.clear();
		g_specs.clear();
		g_clone_slots.clear();
		g_clone_prepared.clear();
		g_clone_committed.clear();
		g_clone_confirmed.clear();
		g_clone_canonical.clear();
		g_spec_actions.clear();
		g_temp_leases.clear(); // their objects went with g_lua_temps
		g_give_back.clear();
		g_give_back_wait.clear();
		g_held.clear([](Action&) {}); // money holds go with the session (money::reset)
		g_removals.clear();
		g_destroy_pacer.reset();
		g_spawn_backlog.clear();
		g_spawn_pacer.reset();
	}
	quarantine_expire(true);
}

void on_peer_left(PeerId id)
{
	g_silencer_receipts.erase(id);
	// his waiting spawn requests go at once, unanswered
	const auto his = [id](const spawn::QueuedRequest& r) { return PeerId(r.peer) == id; };
	for (const std::string& key : g_spawn_queue.keys())
		g_spawn_queue.drop_if(key, his, drop_queued);
	if (mode().decides_here())
	{
		// what he still held goes back to this world's generator
		host_sync_leases();
		host_free_ids(g_leases.drop(id));
	}
}

void frame()
{
	PROF_EVENT("coop/coop_requests/frame");
	if (mode().decides_here())
	{
		host_pump_spawns();
		host_pump_leases();
	}
	if (!is_mirror())
		return;
	client_flush_launches(); // before the outbox (a launch usually precedes a drop of the launcher)
	client_pump_leases(); // retries go out with this frame's outbox
	client_flush();
	if (mgr().is_live_client() && world_up()) // it destroys and asks for objects of the world
		client_pump_rebuilds();
}

void register_console()
{
	CMD4(CCC_Integer, "coop_client_hit_perks", &g_coop_client_hit_perks, 0, 1);
}

void print_status()
{
	if (mode().decides_here())
	{
		u64 ok = g_h.results[u32(Reason::Ok)], refused = 0;
		for (u32 i = 1; i < u32(Reason::Count); ++i)
			refused += g_h.results[i];
		::Msg("[coop]   requests in: %llu accepted, %llu refused (moved %llu, not yours %llu, too far %llu, rate %llu), "
		      "%llu hits applied, %llu dropped while rejoining, %llu malformed, %llu local spawns",
		    ok, refused, g_h.results[u32(Reason::Moved)], g_h.results[u32(Reason::NotOwner)],
		    g_h.results[u32(Reason::TooFar)], g_h.results[u32(Reason::RateLimited)], g_h.hits_applied,
		    g_h.dropped_joining, g_h.malformed, g_h.spawns_local);
		::Msg("[coop]   player damage: %llu hits on characters, %llu sent to players (%llu from explosions no creature "
		      "set off), friendly fire %s (%llu hits between players dropped at the character, %llu ReqHit refused), "
		      "%llu charges set off as their player",
		    g_h.proxy_hits, g_h.player_hits_sent, g_h.explosion_hits_sent, g_coop_friendly_fire ? "on" : "off",
		    g_h.friendly_fire_dropped, g_h.results[u32(Reason::FriendlyFire)], g_h.explosions_as_player);
		::Msg("[coop]   refused as invalid (R3): %llu bones, %llu spawn sections, %llu spawn states, %llu numbers, "
		      "%llu out-of-reach moves claimed as local, %llu upgrades",
		    g_bad[BadBone], g_bad[BadSection], g_bad[BadSpawnState], g_bad[BadNumber], g_bad[BadMoveRange],
		    g_bad[BadUpgrade]);
		::Msg("[coop]   moves into containers (move rules, F4 G6-1): %llu asked, %llu let in, %llu refused by a rule, %llu "
		      "refused as too many in flight, %llu without a rule, %llu failed (moved as stock), %llu undone by the engine; "
		      "%u held now",
		    g_ac.asked, g_ac.let_in, g_ac.refused, g_ac.too_many, g_ac.no_rule, g_ac.failed, g_ac.undone,
		    u32(g_admission.size()));
		::Msg("[coop]   moves into players (move rules, R32 P4): %llu asked, %llu refused by a rule, %llu failed (moved as stock)",
		    g_ac.player_asked, g_ac.player_refused, g_ac.player_failed);
		::Msg("[coop]   script moves here (transfer_item): %llu refused, %llu done already, %llu of items being destroyed, %llu "
		      "to the holder the item has (the stock pair: into the ruck)",
		    g_script_moves.refused, g_script_moves.already, g_script_moves.dying, g_script_moves.in_place);
		::Msg("[coop]   weapon riders carried by moves (F4 G3-1c): %llu taken, %llu written, %llu too long, %llu not the "
		      "holder's, %llu script failures",
		    g_carry.taken, g_carry.written, g_carry.refused_long, g_carry.not_holder, g_carry.lua_failed);
		using spawn::SpawnVerdict;
		::Msg("[coop]   spawns refused: class %llu, line %llu, parent %llu, denied %llu, quest %llu, budget %llu "
		      "(budget %u RU in a row + %u RU/s per character, %u characters this session; %u allow-listed, %u "
		      "deny-listed sections)",
		    g_spawn_refused[u32(SpawnVerdict::UnknownClass)], g_spawn_refused[u32(SpawnVerdict::MissingLine)],
		    g_spawn_refused[u32(SpawnVerdict::WrongParent)], g_spawn_refused[u32(SpawnVerdict::Denied)],
		    g_spawn_refused[u32(SpawnVerdict::QuestItem)], g_spawn_refused[u32(SpawnVerdict::OverBudget)],
		    g_spawn_policy.cost_burst, g_spawn_policy.cost_per_sec, (u32)g_spawn_budgets.size(),
		    (u32)g_spawn_allow.size(), (u32)g_spawn_deny.size());
		::Msg("[coop]   spawns queued %u (waiting for the cost budget now; %llu queued in all, %llu created from the "
		      "queue, %llu dropped: player gone or world replaced; at most %u per character, F2-21)",
		    (u32)g_spawn_queue.total(), g_spawn_queued, g_spawn_dequeued, g_spawn_queue_dropped,
		    u32(spawn::kMaxQueued));
		::Msg("[coop]   quest allowed in dialog %llu (quest items a player's talk gave his own character, F2-16)",
		    g_spawn_quest_gifts);
		const lease::HostLeases::Stats& ls = g_leases.stats();
		::Msg("[coop]   leased ids (F2 T12b): %u held by %u player(s) (%u each); %llu granted in %llu messages, %llu spent "
		      "(%llu creates, %llu of them clones), %llu given back in %llu acks (%llu not theirs, %llu bad acks), %llu "
		      "freed by players who left, %llu voided by a new world; %llu requests with an id not theirs; %llu times "
		      "short of ids; %u free id blocks",
		    (u32)g_leases.total(), (u32)g_leases.holders().size(), u32(lease::kLeaseSize), ls.granted, g_hl.messages,
		    ls.spent, g_hl.spawns, g_hl.clones, g_hl.ack_ids, g_hl.acks, g_hl.ack_not_his, g_hl.bad_acks, ls.dropped,
		    ls.voided, g_hl.not_his, g_hl.short_of_ids,
		    g_pGameLevel && Level().Server ? Level().Server->AvailableIDBlocks() : 0u);
		using ledger::UpgradeVerdict;
		const auto upgrade_refused = [](UpgradeVerdict v) { return g_upgrades.verdicts[u32(v)]; };
		::Msg("[coop]   upgrades at a mechanic (F2 T5): %llu bought (%llu RU charged), %llu already there, refused: no "
		      "mechanic %llu (not his talk %llu, not a live NPC with an inventory %llu, not a mechanic %llu), too far "
		      "%llu, cost unknown %llu, underpaid %llu, not enough money %llu, engine %llu; upgrades without a mechanic "
		      "%llu (%llu refused, scripted_upgrades %s); %llu named a mechanic without a cost; %llu answered while "
		      "their player joined",
		    g_upgrades.bought, g_upgrades.charged, g_upgrades.already, upgrade_refused(UpgradeVerdict::NoMechanic),
		    g_upgrades.not_his, g_upgrades.not_alive, g_upgrades.not_mechanic, upgrade_refused(UpgradeVerdict::TooFar),
		    upgrade_refused(UpgradeVerdict::UnknownCost), upgrade_refused(UpgradeVerdict::Underpaid),
		    upgrade_refused(UpgradeVerdict::Insufficient), g_upgrades.engine, g_upgrades.without_mechanic,
		    upgrade_refused(UpgradeVerdict::Scripted), money::scripted_upgrades() ? "true" : "false",
		    g_upgrades.no_cost, g_upgrades.answered_dropped);
		return;
	}
	// sent[5] is not used (ReqMoney is coop_money's)
	::Msg("[coop]   requests out: spawn %llu, destroy %llu, use %llu, move %llu, hit %llu, item event %llu; "
	      "%llu accepted, %llu refused, %llu unanswered, %u in flight; %llu while not live, %llu foreign moves, %llu "
	      "duplicate child spawns; lua %llu forwarded / %llu refused; %llu hits received",
	    g_cl.sent[0], g_cl.sent[1], g_cl.sent[2], g_cl.sent[3], g_cl.sent[4], g_cl.sent[6],
	    g_cl.accepted, g_cl.rejected, g_cl.timeouts, (u32)g_inflight.size(), g_cl.not_live, g_cl.foreign_moves,
	    g_cl.child_dups, g_cl.lua_forwarded, g_cl.lua_refused, g_cl.hits_in);
	::Msg("[coop]   hit perks (%s): %llu hits through the before-hit Lua, %llu dropped by it, %llu lua errors",
	    g_coop_client_hit_perks ? "on" : "off", g_cl.hits_perked, g_cl.hits_cancelled, g_cl.perk_errors);
	::Msg("[coop]   weapon riders sent with moves (F4 G3-1c): %llu, %llu too long, %llu script failures", g_carry.sent,
	    g_carry.too_long, g_carry.failed);
	::Msg("[coop]   refused upgrades (F2 T5): %llu items built again from the host's copy (%u waiting, %llu given up)",
	    g_rebuilt, (u32)g_rebuilds.size(), g_rebuild_lost);
	size_t pool = 0, pending = 0, temps = 0, waiting = 0;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		pool = g_pool.size();
		pending = g_specs.size();
		temps = g_temp_leases.size();
		waiting = g_give_back.size() + g_give_back_wait.size();
	}
	size_t quarantined = 0;
	{
		xrCriticalSectionGuard guard(g_quarantine_lock);
		quarantined = g_quarantine.size();
	}
	size_t held = 0;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		held = g_held.size();
	}
	::Msg("[coop]   leased ids (F2 T12b): %u ready, %llu own items created at once (%llu clones, %llu from create(.., "
	      "false)), %u waiting for the host, %u temporary objects hold one; %llu confirmed, %llu retried (rate limit), "
	      "%llu taken back (refused; %u in quarantine), %llu not created (id taken here); %llu releases done by a clone; "
	      "%llu ids given back in %llu acks (%u to go); leases: %llu malformed, %llu of another world",
	    (u32)pool, g_lc.created, g_lc.clones, g_lc.temps, (u32)pending, (u32)temps, g_lc.confirmed, g_lc.retried,
	    g_lc.rolled_back, (u32)quarantined, g_lc.not_created, g_lc.released_with_clone, g_lc.given_back, g_lc.acks,
	    (u32)waiting, g_lc.bad_leases, g_lc.stale_leases);
	::Msg("[coop]   leased ids, round 1: %llu requests waited for their item's create (%u waiting now, %llu dropped with "
	      "a rollback), %llu clones refused (their weapon waits for the host), %llu temporary objects and %llu "
	      "quarantined entities of an old world left unfreed",
	    g_lc.held, (u32)held, g_lc.held_dropped, g_lc.clones_refused, g_lc.temps_left, g_lc.quarantine_left);
	size_t backlog = 0;
	zones::Book::Stats zone_stats;
	size_t zones_alive = 0;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		backlog = g_spawn_backlog.size();
		zone_stats = g_zones.stats();
		zones_alive = g_zones.size();
	}
	::Msg("[coop]   personal map zones: %u alive here, %llu made, %llu released, %llu refused (no leased id to spare), "
	      "%llu ids not lent out again, %llu let go of while their world was going",
	    (u32)zones_alive, zone_stats.issued, zone_stats.retired, zone_stats.refused, zone_stats.lost, g_lc.zones_left);
	::Msg("[coop]   leased ids, round 2: %u spawn requests wait for the pace (%llu frames paced), %llu creates in the "
	      "host's budget queue, %llu asked again for want of an answer, %llu taken back unanswered, %llu clones refused "
	      "as not a variant",
	    (u32)backlog, g_lc.paced, g_lc.queued, g_lc.resent, g_lc.unanswered, g_lc.not_variant);
}

bool client_forward_spawn(CSE_Abstract* e)
{
	if (!e || !is_mirror())
		return false;
	const u16 parent = e->ID_Parent;
	if (parent == kNoId || events::root_of(parent) != kActor)
		return false; // world-caused: loot, population, effects are the host's job
	if (!is_live_mirror())
	{
		++g_cl.not_live;
		return true;
	}
	// A transient child of an item (fake missile in a grenade, rocket in a launcher) is asked for
	// once per holder until it arrives; the engine retries every throw start.
	CSE_Abstract* holder = world::server_entity(parent);
	if (holder && !smart_cast<CSE_ALifeTraderAbstract*>(holder) && !smart_cast<CSE_ALifeInventoryBox*>(holder))
	{
		string32 tail;
		xr_sprintf(tail, "#%u", parent);
		const xr_string key = xr_string(e->s_name.c_str()) + tail;
		xrCriticalSectionGuard guard(g_out_lock);
		if (g_child_spawns.count(key))
		{
			++g_cl.child_dups;
			return true;
		}
		g_child_spawns[key] = mgr().now_ms();
	}
	// the host builds the entity from the section; the item state goes as a coop_items blob, which the host
	// checks like any ItemState (no raw spawn bytes).
	Action a;
	a.kind = Act::Spawn;
	a.item = parent;
	a.pos = e->o_Position;
	a.section = e->s_name.c_str() ? e->s_name.c_str() : "";
	std::vector<u8> state;
	if (items::capture_spawn_state(e, state))
		a.bytes.assign(state.begin(), state.end());
	push(std::move(a));
	return true;
}

void client_destroy(u16 local_item, bool use)
{
	const bool live = is_live_mirror();
	xrCriticalSectionGuard guard(g_out_lock);
	if (!can_queue_mutation_locked(local_item, live))
	{
		if (!live)
			++g_cl.not_live;
		return;
	}
	const auto* entity = world::server_entity(local_item);
	const Msg type = use ? Msg::ReqUse : Msg::ReqDestroy;
	if (entity && g_removals.begin(local_item, type, entity))
		g_outbox.push_back(removal_action({local_item, type, entity}));
}

void client_move(u16 local_item, u16 from, u16 to, const Fvector* pos, u8 flags)
{
	const bool live = is_live_mirror();
	xrCriticalSectionGuard guard(g_out_lock);
	if (!can_queue_mutation_locked(local_item, live))
	{
		if (!live)
			++g_cl.not_live;
		return;
	}
	if (to != kNoId && !(flags & kMoveLocal))
	{
		// reject + take of one item right after each other is one transfer (box <-> actor, trade).
		// `from` of the take is the local holder, which lags behind our own requests still on the
		// way (box -> actor -> box before the first answer): the reject names the real source.
		for (auto it = g_outbox.rbegin(); it != g_outbox.rend(); ++it)
		{
			if (it->kind != Act::Move || it->item != local_item || it->item_world != g_pool_world)
				continue;
			if (it->open)
			{
				it->to = to;
				it->open = false;
				it->flags |= flags;
				return;
			}
			break;
		}
	}
	Action a;
	a.kind = Act::Move;
	a.item_world = g_pool_world;
	a.item = local_item;
	a.from = from;
	a.to = to;
	a.flags = flags;
	if (pos)
	{
		a.pos = *pos;
		a.flags |= kMovePos;
	}
	a.open = to == kNoId && !(flags & kMoveLocal);
	g_outbox.push_back(std::move(a));
}

bool client_move_position(u16 local_item, const Fvector& pos)
{
	xrCriticalSectionGuard guard(g_out_lock);
	for (auto it = g_outbox.rbegin(); it != g_outbox.rend(); ++it)
	{
		if (it->kind == Act::Move && it->item == local_item && it->to == kNoId && it->item_world == join::world_generation())
		{
			it->pos = pos;
			it->flags |= kMovePos;
			return true;
		}
	}
	return false;
}

void client_hit(u16 local_target, const SHit& hit)
{
	if (!is_live_mirror())
	{
		++g_cl.not_live;
		return;
	}
	Action a;
	a.kind = Act::Hit;
	a.item = local_target;
	a.hit = hit;
	a.hit.who = nullptr; // ids only: the action may outlive the object
	push(std::move(a));
}

void client_flush_outbox()
{
	frame(); // exactly what the end of this frame would send, in the same order
	money::client_flush(); // and the money changes made before
}

void client_item_event(u16 local_item, u16 ge_type, const void* data, u32 size)
{
	if (size > 512)
		return;
	xrCriticalSectionGuard guard(g_out_lock);
	if (!can_queue_mutation_locked(local_item, is_live_mirror()))
		return;
	Action a;
	a.kind = Act::ItemEvent;
	a.item_world = g_pool_world;
	a.item = local_item;
	a.ge_type = ge_type;
	a.bytes.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
	// an upgrade bought at a mechanic carries him and its price (items::UpgradePurchase); else none, nothing
	if (ge_type == GE_INSTALL_UPGRADE)
		items::claim_upgrade_payment(local_item, a.mechanic, a.paid, a.hold);
	g_outbox.push_back(std::move(a));
}

void client_player_died(u16 local_killer)
{
	if (!is_live_mirror())
		return;
	Action a;
	a.kind = Act::Died;
	a.item = local_killer;
	push(std::move(a));
}
} // namespace requests
} // namespace coop
