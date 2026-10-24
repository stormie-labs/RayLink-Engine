#include "stdafx.h"
#include "coop_events.h"
#include "coop_requests.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_entities.h"
#include "coop_alife.h"
#include "coop_remote.h"
#include "coop_dialog.h"
#include "coop_story.h"
#include "coop_callgate.h" // F2 T12
#include "xrCoop/coop_rules.h"

#include "Level.h"
#include "Car.h"
#include "Actor.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "Hit.h"
#include "eatable_item.h"
#include "Missile.h"
#include "CustomRocket.h"
#include "xrServer.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrMessages.h"

namespace coop
{
namespace events
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u16 kActor = 0; // the local actor on every machine (the join swap keeps it at 0)
constexpr u32 kPendingTimeoutMs = 15000;
constexpr u32 kDumpAfterMs = 1000;
constexpr u32 kLogBurst = 20;

// Host hooks run inside xrServer::OnMessage, which the game thread may call; frame() runs on
// the main thread. They never overlap in this engine, the lock only keeps that an assumption.
xrCriticalSection g_lock;

// ---------------------------------------------------------------- host state
xr_map<u16, u16> g_parent_changes; // item -> parent before its first change since the last frame
xr_vector<std::pair<u16, u16>> g_deaths; // victim, killer
xr_vector<u16> g_local_destroyed;
xr_set<u16> g_local_relayed; // non-ALife entities announced with EntityLocal
xr_map<u16, u16> g_last_holder; // free item -> player character that dropped/threw it
xr_map<u16, Fvector> g_drop_positions; // item -> exact drop position asked for (drop_item_and_teleport)

// ---------------------------------------------------------------- client state
struct ParentTarget
{
	u16 parent = kNoId; // local numbering
	Fvector pos{};
	bool online = true;
	bool exact_pos = false; // the drop position was chosen (script teleport), not "at the hand"
	u64 since = 0;
	bool dump_asked = false;
};
xr_map<u16, ParentTarget> g_parent_targets; // item (local id) -> where the host says it is

struct DeathPending
{
	u16 killer = kNoId;
	u64 since = 0;
};
xr_map<u16, DeathPending> g_death_targets;

struct Counters
{
	// client router
	u64 spawns_forwarded = 0, spawns_refused = 0, destroys_forwarded = 0, destroys_blocked = 0;
	u64 moves_forwarded = 0, moves_blocked = 0, hits_forwarded = 0, hits_blocked = 0, actor_hits_blocked = 0;
	u64 deaths_blocked = 0, other_blocked = 0, level_changes = 0, saves_blocked = 0, vehicles_blocked = 0;
	u64 launches_forwarded = 0, launches_dropped = 0; // W3a: own rockets (ReqLaunch / old drop fallback)
	u64 echo_dropped = 0; // R4 4: spawns and world-object requests not made from reactions to the host's story
	// client apply
	u64 parent_applied = 0, parent_failed = 0, parent_timeouts = 0, deaths_applied = 0, local_spawns = 0,
	    local_destroys = 0, local_errors = 0, launches_applied = 0, launches_missed = 0;
	u64 dropped_before_live = 0; // R2 I2: world messages that came before we were live
	// host relay
	u64 parent_sent = 0, deaths_sent = 0, player_deaths = 0, local_spawn_sent = 0, local_destroy_sent = 0,
	    launches_relayed = 0;
} g_c;

u32 g_log_count = 0;
bool log_allowed() { return ++g_log_count <= kLogBurst || (g_log_count % 100) == 0; }
bool g_vehicle_note = false; // client: tell the player once per refused attempt (HUD from frame())

bool router_on()
{
	const Phase p = mgr().phase();
	return p == Phase::LoadingWorld || p == Phase::Live;
}

// Game object that is in the level and not going away.
CGameObject* find_go(u16 id)
{
	if (id == kNoId || !g_pGameLevel)
		return nullptr;
	CGameObject* go = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return go && !go->getDestroy() ? go : nullptr;
}

// Game object the server accepts in an ownership event (see is_object_valid_on_svclient).
CGameObject* valid_go(u16 id)
{
	CGameObject* go = find_go(id);
	return go && !go->object_removed() ? go : nullptr;
}

bool is_ours(u16 id) { return id != kNoId && root_of(id) == kActor; }

bool is_mirrored_creature(u16 id)
{
	return id != kActor && entities::is_mirrored(id) &&
	    smart_cast<CSE_ALifeCreatureAbstract*>(find_entity(id)) != nullptr;
}

void inject_item_event(u16 type, u16 dest, u16 item)
{
	NET_Packet P;
	CGameObject::u_EventGen(P, type, dest);
	P.w_u16(item);
	CGameObject::u_EventSend(P);
}

bool swallow(u64& counter)
{
	++counter;
	return true;
}

// R2 I2: a world message that comes before we are live is dropped (rules::live_only): it is from the window between
// Welcome and JoinRequest or from a world we have left. Applied, it would change our own world or the new one.
bool before_live(Msg type)
{
	if (!rules::live_only(type) || mgr().is_live_client())
		return false;
	++g_c.dropped_before_live;
	return true;
}

void put_vec(Writer& w, const Fvector& v)
{
	const float f[3] = {v.x, v.y, v.z};
	w.vec3(f);
}

Fvector get_vec(Reader& r)
{
	float f[3] = {0.f, 0.f, 0.f};
	r.vec3(f);
	return Fvector().set(f[0], f[1], f[2]);
}

// ---------------------------------------------------------------- client router

bool filter_message(NET_Packet& P);

bool filter_spawn(NET_Packet& P)
{
	// R4 4: no spawn request from a reaction to a host broadcast (rules::client_echo). A host command's own spawn
	// (EntityLocal, with the host's id) goes to the local server as before; a spawn made while we apply the host's
	// story (an addon's info callback) is neither sent nor made here: world objects are the host's, and the host
	// runs that story with its own reactions.
	// Ruling F2-12: also a spawn into our own inventory (a new object is nobody's item yet): shared rewards are routed
	// by the task layer (W3c), a second one here would be a double reward.
	if (rules::client_echo(story::applying(), entities::client_applying(), false) == rules::Echo::Drop)
		return entities::client_applying() ? false : swallow(g_c.echo_dropped);
	u16 type;
	P.r_begin(type);
	string256 section;
	P.r_stringZ_s(section);
	CSE_Abstract* e = F_entity_Create(section);
	if (!e)
		return swallow(g_c.spawns_refused);
	P.r_pos = 0;
	e->Spawn_Read(P);
	if (requests::client_forward_spawn(e))
		++g_c.spawns_forwarded;
	else
	{
		++g_c.spawns_refused;
		if (log_allowed())
			::Msg("[coop] client spawn of [%s] (parent %u) refused: world spawns are the host's job", section,
			    e->ID_Parent);
	}
	F_entity_Destroy(e);
	return true;
}

// R4 4, Ruling F2-12 (rules::client_echo): true = a request about `id` made now would be a reaction to a host
// broadcast, so it is not made (the caller drops the event and counts it). Asked right before each request, after the
// router's own blocks. While we apply the host's story only world objects are dropped: our own items (root: our actor;
// one we give away is still ours until the host moves it) go to the host as before, it checks them as any request.
// A host command's events never get here (filter_event passes them on first).
bool echo_of(u16 id)
{
	return rules::client_echo(story::applying(), entities::client_applying(), is_ours(id)) == rules::Echo::Drop;
}

bool on_take(u16 dest, u16 item, bool trade)
{
	CSE_Abstract* e = find_entity(item);
	if (!e)
		return false; // not part of the host world: a purely local object
	if (echo_of(item))
		return swallow(g_c.echo_dropped);
	requests::client_move(item, e->ID_Parent, dest, nullptr, trade ? 2 : 0);
	++g_c.moves_forwarded;
	return true;
}

bool on_reject(u16 dest, u16 item, bool before_destroy, const Fvector* pos, bool trade)
{
	if (!find_entity(item))
		return false;
	if (before_destroy)
		return swallow(g_c.moves_blocked); // the GE_DESTROY that follows goes to the host
	if (echo_of(item))
		return swallow(g_c.echo_dropped);
	// A thrown missile leaves the grenade/bolt that held it: local and immediate (the throw
	// must not wait for the network); the host repeats it on its copy.
	CGameObject* go = find_go(item);
	if (go && smart_cast<CMissile*>(go) && smart_cast<CMissile*>(find_go(dest)) && is_ours(dest))
	{
		requests::client_move(item, dest, kNoId, pos, 1);
		++g_c.moves_forwarded;
		return false;
	}
	requests::client_move(item, dest, kNoId, pos, trade ? 2 : 0);
	++g_c.moves_forwarded;
	return true;
}

bool on_destroy(u16 dest)
{
	CSE_Abstract* e = find_entity(dest);
	if (!e)
		return false; // not part of the host world
	if (smart_cast<CSE_ALifeCreatureAbstract*>(e))
		return swallow(g_c.destroys_blocked); // creatures live and die on the host
	if ((is_ours(dest) || e->ID_Parent == kNoId) && echo_of(dest))
		return swallow(g_c.echo_dropped); // one of the two requests below, about a world object
	if (is_ours(dest))
	{
		CGameObject* go = find_go(dest);
		requests::client_destroy(dest, go && smart_cast<CEatableItem*>(go));
		++g_c.destroys_forwarded;
		return true; // the host releases it, its EntityDestroy removes it here (R-6.4)
	}
	if (e->ID_Parent == kNoId)
	{
		// a free item, e.g. our thrown grenade or bolt: the host checks who held it last
		requests::client_destroy(dest, false);
		++g_c.destroys_forwarded;
		return true;
	}
	return swallow(g_c.destroys_blocked);
}

bool on_hit(NET_Packet& P, u16 dest, u16 type)
{
	SHit hit;
	hit.PACKET_TYPE = type;
	hit.DestID = dest;
	hit.Read_Packet_Cont(P);
	if (dest == kActor)
	{
		// Falls, anomalies, radiation, our own explosions: local. A hit from a host-driven
		// creature or another player's avatar arrives from the host as PlayerHit instead.
		CGameObject* who = find_go(hit.whoID);
		if (who && who->ID() != kActor && sync::is_remote_driven(who))
			return swallow(g_c.hits_blocked);
		return false;
	}
	if (!find_entity(dest))
		return false; // purely local object
	// Only hits we caused are claims; anything else hitting a host object is the host's business.
	const bool ours = hit.whoID == kActor || is_ours(hit.whoID);
	if (!ours)
		return swallow(g_c.hits_blocked);
	if (echo_of(dest))
		return swallow(g_c.echo_dropped);
	requests::client_hit(dest, hit);
	++g_c.hits_forwarded;
	return true;
}

bool filter_event(NET_Packet& P)
{
	u16 msg;
	P.r_begin(msg);
	u32 time;
	u16 type, dest;
	P.r_u32(time);
	P.r_u16(type);
	P.r_u16(dest);

	if (type == GE_DIE && dest == kActor)
	{
		u16 killer = kNoId;
		P.r_u16(killer);
		requests::client_player_died(killer);
		return false; // our own death is ours; the host is told
	}
	// R4 4: no request from a reaction to a host broadcast (rules::client_echo). A host command (entities::ClientApply)
	// goes to the local server unchanged, as before: its events are the host's change itself. While we apply the
	// host's story (an addon's info callback), an event that would become a request about a world object is dropped
	// (echo_of): the object stays as the host has it, and the host runs that story with its own reactions. Our own
	// items still go to the host (Ruling F2-12). What the router blocks or keeps local stays so; GE_MONEY goes on.
	if (entities::client_applying())
		return false;

	switch (type)
	{
	case GE_OWNERSHIP_TAKE:
	case GE_OWNERSHIP_TAKE_MP_FORCED:
	case GE_TRADE_BUY:
	{
		u16 item;
		P.r_u16(item);
		return on_take(dest, item, type == GE_TRADE_BUY);
	}
	case GE_OWNERSHIP_REJECT:
	case GE_TRADE_SELL:
	{
		u16 item;
		P.r_u16(item);
		const bool before_destroy = !P.r_eof() && P.r_u8() != 0;
		Fvector pos;
		const bool has_pos = !P.r_eof();
		if (has_pos)
			P.r_vec3(pos);
		return on_reject(dest, item, before_destroy, has_pos ? &pos : nullptr, type == GE_TRADE_SELL);
	}
	case GE_LAUNCH_ROCKET:
	{
		u16 item;
		P.r_u16(item);
		if (is_ours(dest) && find_entity(item))
		{
			if (echo_of(item))
				return swallow(g_c.echo_dropped);
			// W3a: the host launches its copy with our launch (it flies, detonates and is removed for everybody);
			// ours flies as a visual copy that never detonates here (WP7) and goes with the host's destroy.
			if (requests::client_launch(dest, item))
				++g_c.launches_forwarded;
			else
			{
				requests::client_move(item, dest, kNoId, nullptr, 1); // no rocket object here: the host drops its copy
				++g_c.launches_dropped;
			}
		}
		return false; // the launch is local and immediate
	}
	case GE_DESTROY:
	case GE_DESTROY_REJECT:
		return on_destroy(dest);
	case GE_HIT:
	case GE_HIT_STATISTIC:
		return on_hit(P, dest, type);
	case GE_DIE:
	case GE_ASSIGN_KILLER:
		if (dest == kActor || !find_entity(dest))
			return false;
		return swallow(g_c.deaths_blocked); // deaths of host creatures come as EntityDied
	case GE_MONEY:
		if (dest == kActor)
		{
			u32 money;
			P.r_u32(money);
			// A change of our money goes to the host as a ReqMoney with a cause, a reaction to the host's story too:
			// the host keeps the balance and bounds the change (F2 T1, protocol 7). A MoneyBalance applied here is a
			// host command (entities::ClientApply) and never gets this far.
			requests::client_money(money);
		}
		return false;
	case GE_INSTALL_UPGRADE:
		if (is_ours(dest) && P.r_pos < P.B.count)
		{
			if (echo_of(dest))
				return swallow(g_c.echo_dropped);
			requests::client_item_event(dest, type, P.B.data + P.r_pos, P.B.count - P.r_pos);
		}
		return false; // installed here, repeated on the host copy
	case GE_CHANGE_POS:
	{
		Fvector pos;
		P.r_vec3(pos);
		if (echo_of(dest))
			return false; // amends no drop request: it moves the object here, as without a drop in flight
		return requests::client_move_position(dest, pos); // drop_item_and_teleport: folded into the drop
	}
	case GE_TELEPORT_OBJECT:
	case GE_ADD_RESTRICTION:
	case GE_REMOVE_RESTRICTION:
	case GE_REMOVE_ALL_RESTRICTIONS:
		return is_mirrored_creature(dest) ? swallow(g_c.other_blocked) : false; // AI of host creatures
	case GEG_PLAYER_ATTACH_HOLDER:
	{
		// Client cars are kinematic copies driven by host snapshots (WP7): nobody drives them here.
		u16 holder = kNoId;
		P.r_u16(holder);
		if (dest != kActor || !smart_cast<CCar*>(find_go(holder)))
			return false; // mounted guns and other holders stay local
		g_vehicle_note = true;
		return swallow(g_c.vehicles_blocked);
	}
	case GE_TRANSFER_AMMO: // multiplayer only: destroys an entity on the local server
	case GE_RESPAWN: // multiplayer phantoms
		return swallow(g_c.other_blocked);
	default:
		return false; // local presentation/state, see the decision table in the WP2 report
	}
}

bool filter_pack(NET_Packet& P)
{
	u16 msg;
	P.r_begin(msg);
	struct Sub
	{
		u32 offset, size;
	};
	xr_vector<Sub> keep;
	bool dropped = false;
	while (!P.r_eof())
	{
		const u32 size = P.r_u8();
		const u32 offset = P.r_pos;
		if (offset + size > P.B.count)
			break;
		NET_Packet sub;
		sub.B.count = size;
		CopyMemory(sub.B.data, P.B.data + offset, size);
		sub.r_pos = 0;
		P.r_pos = offset + size;
		if (filter_message(sub))
			dropped = true;
		else
			keep.push_back({offset, size});
	}
	if (!dropped)
		return false;
	if (keep.empty())
		return true;
	NET_Packet out;
	out.w_begin(M_EVENT_PACK);
	for (const Sub& s : keep)
	{
		out.w_u8(u8(s.size));
		out.w(P.B.data + s.offset, s.size);
	}
	P.B.count = out.B.count;
	CopyMemory(P.B.data, out.B.data, out.B.count);
	return false;
}

bool filter_message(NET_Packet& P)
{
	u16 type;
	P.r_begin(type);
	switch (type)
	{
	case M_SPAWN:
		P.r_pos = 0;
		return filter_spawn(P);
	case M_EVENT:
		P.r_pos = 0;
		return filter_event(P);
	case M_EVENT_PACK:
		P.r_pos = 0;
		return filter_pack(P);
	case M_CHANGE_LEVEL:
		if (entities::client_applying())
			return false;
		join::client_request_level_change(P); // the host moves the whole group
		return swallow(g_c.level_changes);
	case M_SAVE_GAME:
	case M_LOAD_GAME:
	case M_RELOAD_GAME:
		if (entities::client_applying())
			return false;
		::Msg("! [coop] saving/loading is the host's job in a co-op session");
		return swallow(g_c.saves_blocked);
	default:
		return false; // updates, client ready, etc.: local
	}
}

// ---------------------------------------------------------------- client apply

enum class Step : u8
{
	Done,
	Wait,
	Fail
};

const char* entity_name(u16 id)
{
	CSE_Abstract* e = find_entity(id);
	return e ? e->name_replace() : "?";
}

// One safe step towards "item is held by t.parent" on this machine (local ids). Online objects
// move through the normal ownership events (server entity + game object stay consistent),
// offline ones through ALife attach/detach. Never edits ID_Parent alone.
Step step_parent(u16 item, const ParentTarget& t)
{
	CSE_Abstract* e = find_entity(item);
	if (!e)
		return Step::Wait;
	if (e->ID_Parent == t.parent)
		return Step::Done;

	entities::ClientApply scope;
	CGameObject* go = find_go(item);
	if (go)
	{
		if (!valid_go(item))
			return Step::Wait; // being removed here; EntityDestroy follows
		CGameObject* target = t.parent == kNoId ? nullptr : valid_go(t.parent);
		const bool transfer = e->ID_Parent != kNoId && target != nullptr;
		if (e->ID_Parent != kNoId)
		{
			if (!valid_go(e->ID_Parent))
				return Step::Wait; // holder not in the level (yet): retry until the timeout
			inject_item_event(transfer ? GE_TRADE_SELL : GE_OWNERSHIP_REJECT, e->ID_Parent, item);
			if (e->ID_Parent != kNoId)
				return Step::Fail;
		}
		if (t.parent == kNoId)
		{
			if (t.exact_pos)
			{
				NET_Packet P; // after the reject in the same queue: the item has its physics shell by then
				CGameObject::u_EventGen(P, GE_CHANGE_POS, item);
				P.w_vec3(t.pos);
				CGameObject::u_EventSend(P);
			}
			return Step::Done;
		}
		if (target)
		{
			inject_item_event(transfer ? GE_TRADE_BUY : GE_OWNERSHIP_TAKE, t.parent, item);
			return e->ID_Parent == t.parent ? Step::Done : Step::Fail;
		}
		// the new holder is offline here: take the item offline first, attach next step
		CSE_ALifeDynamicObject* d = smart_cast<CSE_ALifeDynamicObject*>(e);
		if (!d || !ai().alife().objects().object(item, true))
			return Step::Fail;
		if (d->m_bOnline)
			alife_rw().switch_offline(d);
		return Step::Wait; // its game object goes away, then the offline branch attaches it
	}

	CSE_ALifeDynamicObject* d = ai().alife().objects().object(item, true);
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(d);
	if (!d || !ii)
		return Step::Fail; // a non-ALife object without a game object
	if (d->m_bOnline)
		return Step::Wait; // its game object is being created or destroyed
	if (d->ID_Parent != kNoId)
	{
		CSE_ALifeDynamicObject* holder = ai().alife().objects().object(d->ID_Parent, true);
		if (!holder || std::find(holder->children.begin(), holder->children.end(), item) == holder->children.end())
			return Step::Fail;
		alife_rw().graph().detach(*holder, ii, holder->m_tGraphID, true, true);
	}
	if (t.parent == kNoId)
	{
		d->o_Position = t.pos;
		if (t.online)
			alife_rw().switch_online(d);
		return Step::Done;
	}
	CSE_ALifeDynamicObject* holder = ai().alife().objects().object(t.parent, true);
	if (!holder)
		return Step::Wait;
	if (holder->m_bOnline)
	{
		// the holder is online here: bring the item online free, take it with an event next step
		d->o_Position = holder->o_Position;
		alife_rw().switch_online(d);
		return Step::Wait;
	}
	alife_rw().graph().attach(*holder, ii, d->m_tGraphID, true, true);
	return Step::Done;
}

void client_step_parents()
{
	if (!g_pGameLevel || !ai().get_alife() || g_parent_targets.empty())
		return;
	const u64 now = mgr().now_ms();
	for (auto it = g_parent_targets.begin(); it != g_parent_targets.end();)
	{
		const u16 item = it->first;
		ParentTarget& t = it->second;
		const Step s = step_parent(item, t);
		if (s == Step::Wait)
		{
			if (!t.dump_asked && !find_entity(item) && now - t.since > kDumpAfterMs)
			{
				Writer w(4);
				w.u16(join::to_host(item));
				net::client_send(Msg::StateDumpRequest, w); // P3: find it first (host resends it)
				t.dump_asked = true;
			}
			if (now - t.since <= kPendingTimeoutMs)
			{
				++it;
				continue;
			}
			++g_c.parent_timeouts;
			::Msg("! [coop] item %u [%s]: could not move it to %u in time", join::to_host(item), entity_name(item),
			    t.parent == kNoId ? kNoId : join::to_host(t.parent));
		}
		else if (s == Step::Fail)
		{
			++g_c.parent_failed;
			::Msg("! [coop] item %u [%s]: cannot apply the host's holder %u here", join::to_host(item),
			    entity_name(item), t.parent == kNoId ? kNoId : join::to_host(t.parent));
		}
		else
			++g_c.parent_applied;
		it = g_parent_targets.erase(it);
	}
}

bool step_death(u16 id, const DeathPending& d)
{
	CSE_Abstract* e = find_entity(id);
	if (!e)
		return false;
	entities::ClientApply scope;
	if (CGameObject* go = find_go(id))
		remote::on_host_death(go, d.killer); // WP1: GE_DIE through the local server, then Die()
	else if (CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(e))
	{
		// offline here: it comes online dead
		if (c->g_Alive())
			c->set_health(0.f);
		c->set_killer_id(d.killer);
	}
	++g_c.deaths_applied;
	return true;
}

void client_step_deaths()
{
	if (!g_pGameLevel || !ai().get_alife() || g_death_targets.empty())
		return;
	const u64 now = mgr().now_ms();
	for (auto it = g_death_targets.begin(); it != g_death_targets.end();)
	{
		if (step_death(it->first, it->second) || now - it->second.since > kPendingTimeoutMs)
			it = g_death_targets.erase(it);
		else
			++it;
	}
}

void client_on_entity_parent(PeerId, Reader& r)
{
	if (before_live(Msg::EntityParent))
		return;
	const u16 item = join::to_local(r.u16());
	const u16 old_h = r.u16();
	const u16 new_h = r.u16();
	ParentTarget t;
	t.online = r.boolean();
	float p[3];
	r.vec3(p);
	t.exact_pos = (r.u8() & 1) != 0;
	if (!r.ok() || item == kNoId)
		return;
	(void)old_h;
	t.parent = new_h == kNoId ? kNoId : join::to_local(new_h);
	t.pos.set(p[0], p[1], p[2]);
	t.since = mgr().now_ms();
	entities::client_on_holder_changed(item, t.parent, t.online); // a dropped item stays online here
	g_parent_targets[item] = t; // latest wins (state, not events: P5)
	client_step_parents();
}

void client_on_entity_died(PeerId, Reader& r)
{
	if (before_live(Msg::EntityDied))
		return;
	const u16 victim = join::to_local(r.u16());
	const u16 killer_h = r.u16();
	if (!r.ok() || victim == kNoId || victim == kActor)
		return;
	DeathPending d;
	d.killer = killer_h == kNoId ? kNoId : join::to_local(killer_h);
	d.since = mgr().now_ms();
	g_death_targets[victim] = d;
	client_step_deaths();
}

// EntityLocal op 2 (W3a): the host launched a player's rocket. Our copy, still in that player's launcher
// here, flies the same way through the engine's own launch event. It never detonates here: the host's
// explosion (WP7) and its destroy (op 1) follow.
void client_launch_copy(u16 rocket, Reader& r)
{
	const u16 launcher = join::to_local(r.u16());
	const Fvector pos = get_vec(r);
	Fvector dir = get_vec(r);
	const Fvector vel = get_vec(r);
	const Fvector ang = get_vec(r);
	if (!r.ok() || !_valid(pos) || !_valid(dir) || !_valid(vel) || !_valid(ang) || dir.magnitude() < EPS_L)
		return;
	CCustomRocket* rocket_go = smart_cast<CCustomRocket*>(valid_go(rocket));
	CGameObject* launcher_go = valid_go(launcher);
	CSE_Abstract* e = Level().Server->ID_to_entity(rocket);
	if (!rocket_go || !launcher_go || !e || e->ID_Parent != launcher || rocket_go->H_Parent() != launcher_go)
	{
		++g_c.launches_missed; // not in that launcher here: EntityParent frees it, the explosion still plays
		return;
	}
	dir.normalize();
	Fmatrix xf;
	xf.identity();
	xf.k.set(dir);
	Fvector::generate_orthonormal_basis(xf.k, xf.j, xf.i);
	xf.c.set(pos);
	rocket_go->SetLaunchParams(xf, vel, ang);
	entities::ClientApply scope;
	NET_Packet P; // as the weapons do after LaunchRocket: the launcher detaches it with bLaunch = true
	CGameObject::u_EventGen(P, GE_LAUNCH_ROCKET, launcher);
	P.w_u16(rocket);
	CGameObject::u_EventSend(P);
	++g_c.launches_applied;
}

void client_on_entity_local(PeerId, Reader& r)
{
	if (before_live(Msg::EntityLocal))
		return;
	const u8 op = r.u8();
	const u16 id = join::to_local(r.u16());
	if (!r.ok() || !g_pGameLevel || !Level().Server)
		return;
	if (op == 2)
		return client_launch_copy(id, r);
	if (op == 1)
	{
		CGameObject* go = find_go(id);
		if (!go)
			return; // never had it, or already gone
		entities::ClientApply scope;
		NET_Packet P;
		CGameObject::u_EventGen(P, GE_DESTROY, id);
		CGameObject::u_EventSend(P);
		++g_c.local_destroys;
		return;
	}
	if (op != 0)
		return; // unknown op
	const u16 parent_h = r.u16();
	const u16 size = r.u16();
	NET_Packet spawn;
	if (size > sizeof(spawn.B.data) || !r.bytes(spawn.B.data, size))
		return;
	spawn.B.count = size;
	const u16 parent = parent_h == kNoId ? kNoId : join::to_local(parent_h);
	if (Level().Server->ID_to_entity(id))
		return; // already here (P6: a repeat is harmless)
	if (!Level().Server->IsFreeID(id))
	{
		// F2 T12b (R2 minor "ID already used"): our generator has it taken without a server entity (an offline ALife
		// object, a leased id of ours): Process_spawn would assert on it
		++g_c.local_errors;
		::Msg("! [coop] local entity %u [?] skipped: its id is taken here", join::to_host(id));
		return;
	}
	if (parent != kNoId && !Level().Server->ID_to_entity(parent))
		return; // its holder is not online here; it is a transient child, nothing to keep
	u16 type;
	spawn.r_begin(type);
	string256 section;
	spawn.r_stringZ_s(section);
	if (type != M_SPAWN)
		return;
	CSE_Abstract* e = F_entity_Create(section);
	if (!e)
		return;
	spawn.r_pos = 0;
	e->Spawn_Read(spawn);
	e->ID = id;
	e->ID_Parent = parent;
	e->ID_Phantom = kNoId;
	e->RespawnTime = 0;
	NET_Packet P;
	e->Spawn_Write(P, TRUE);
	F_entity_Destroy(e);
	{
		entities::ClientApply scope;
		Level().Send(P, net_flags(TRUE));
	}
	CSE_Abstract* got = Level().Server->ID_to_entity(id);
	if (!got || xr_strcmp(got->s_name, section))
	{
		++g_c.local_errors;
		::Msg("! [coop] local entity %u [%s] could not get its id here", join::to_host(id), section);
		return;
	}
	++g_c.local_spawns;
}

// PlayerDied: client -> host (own death), host -> clients (relay).
void on_player_died(PeerId from, Reader& r)
{
	u16 character = r.u16();
	const u16 killer = r.u16();
	if (!r.ok())
		return;
	if (mgr().is_host())
	{
		character = join::proxy_of_peer(from);
		if (character == kNoId || sync::host_peer_joining(from))
			return;
		const PeerInfo* p = mgr().session().peer(from);
		::Msg("[coop] player '%s' died (killer %u)", p ? p->nick.c_str() : "?", killer);
		Writer w(8);
		w.u16(character);
		w.u16(killer);
		net::host_broadcast(Msg::PlayerDied, w, Lane::Reliable, from);
		++g_c.player_deaths;
		return;
	}
	// The avatar's death pose is the players module's job; here it is only made visible (P9).
	::Msg("[coop] player character %u died (killer %u)", character, killer);
	++g_c.player_deaths;
}

// ---------------------------------------------------------------- host relay

// Level-change review: what the hooks recorded while the world was not up (a load records the parent changes of the whole
// new population) belongs to a world that is gone or still being built; every client rejoins from the new world's save,
// and an id of the old world names another object in the new one.
void host_discard()
{
	xrCriticalSectionGuard guard(g_lock);
	g_parent_changes.clear();
	g_drop_positions.clear();
	g_deaths.clear();
	g_local_destroyed.clear();
}

void host_flush()
{
	xr_map<u16, u16> changes;
	xr_map<u16, Fvector> drops;
	xr_vector<std::pair<u16, u16>> deaths;
	xr_vector<u16> destroyed;
	{
		xrCriticalSectionGuard guard(g_lock);
		changes.swap(g_parent_changes);
		drops.swap(g_drop_positions);
		deaths.swap(g_deaths);
		destroyed.swap(g_local_destroyed);
	}
	for (const auto& c : changes)
	{
		CSE_Abstract* e = find_entity(c.first);
		if (!e)
			continue; // destroyed meanwhile: EntityDestroy/EntityLocal tells the clients
		const u16 now = e->ID_Parent;
		if (now == c.second)
			continue; // back where it was
		Fvector pos = e->o_Position;
		auto exact = now == kNoId ? drops.find(c.first) : drops.end();
		if (exact != drops.end())
			pos = exact->second;
		else if (now == kNoId)
			if (CSE_Abstract* holder = find_entity(c.second))
				pos = holder->o_Position;
		const CSE_ALifeObject* ao = smart_cast<const CSE_ALifeObject*>(e);
		Writer w(24);
		w.u16(c.first);
		w.u16(c.second);
		w.u16(now);
		w.boolean(ao ? ao->m_bOnline : true);
		const float p[3] = {pos.x, pos.y, pos.z};
		w.vec3(p);
		w.u8(exact != drops.end() ? 1 : 0);
		net::host_broadcast(Msg::EntityParent, w);
		++g_c.parent_sent;
	}
	for (const auto& d : deaths)
	{
		Writer w(8);
		w.u16(d.first);
		w.u16(d.second);
		if (d.first == kActor)
		{
			net::host_broadcast(Msg::PlayerDied, w); // the host player
			++g_c.player_deaths;
		}
		else if (!world::is_player_proxy(d.first))
		{
			net::host_broadcast(Msg::EntityDied, w);
			++g_c.deaths_sent;
		}
	}
	for (u16 id : destroyed)
	{
		Writer w(4);
		w.u8(1);
		w.u16(id);
		net::host_broadcast(Msg::EntityLocal, w);
		++g_c.local_destroy_sent;
	}
}
} // namespace

// ---------------------------------------------------------------- API

CSE_Abstract* find_entity(u16 id)
{
	if (id == kNoId)
		return nullptr;
	if (ai().get_alife())
		if (CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true))
			return e;
	if (g_pGameLevel && Level().Server)
		return Level().Server->ID_to_entity(id);
	return nullptr;
}

u16 root_of(u16 id)
{
	u16 cur = id;
	for (int depth = 0; depth < 8 && cur != kNoId; ++depth)
	{
		CSE_Abstract* e = find_entity(cur);
		if (!e)
			return depth == 0 ? kNoId : cur;
		if (e->ID_Parent == kNoId)
			return cur;
		cur = e->ID_Parent;
	}
	return cur;
}

void install_server(Session& s) { s.on(Msg::PlayerDied, &on_player_died); }

void install_client(Endpoint& e)
{
	// Client handlers drop these world messages until we are live (R2 I2, before_live).
	e.on(Msg::EntityParent, [](PeerId from, Reader& r) { if (mgr().is_client()) client_on_entity_parent(from, r); });
	e.on(Msg::EntityDied, [](PeerId from, Reader& r) { if (mgr().is_client()) client_on_entity_died(from, r); });
	e.on(Msg::EntityLocal, [](PeerId from, Reader& r) { if (mgr().is_client()) client_on_entity_local(from, r); });
	e.on(Msg::PlayerDied, &on_player_died);
}

void reset()
{
	xrCriticalSectionGuard guard(g_lock);
	g_parent_changes.clear();
	g_deaths.clear();
	g_local_destroyed.clear();
	g_local_relayed.clear();
	g_last_holder.clear();
	g_drop_positions.clear();
	g_parent_targets.clear();
	g_death_targets.clear();
	g_log_count = 0;
	g_vehicle_note = false;
}

void frame()
{
	if (mgr().is_host())
	{
		if (world_up())
			host_flush();
		else
			host_discard();
	}
	else if (mgr().is_live_client() && world_up())
	{
		client_step_parents();
		client_step_deaths();
		if (g_vehicle_note)
		{
			g_vehicle_note = false;
			dialog::hud_message("st_coop_vehicle_host", "Vehicles are driven by the host in co-op");
		}
	}
}

void print_status()
{
	if (mgr().is_host())
	{
		::Msg("[coop]   events out: %llu parent changes, %llu deaths, %llu player deaths, local %llu spawns / %llu "
		      "destroys (%u relayed alive), %llu player rocket launches",
		    g_c.parent_sent, g_c.deaths_sent, g_c.player_deaths, g_c.local_spawn_sent, g_c.local_destroy_sent,
		    (u32)g_local_relayed.size(), g_c.launches_relayed);
		return;
	}
	::Msg("[coop]   rockets: %llu own launches sent to the host (%llu dropped without a rocket object), %llu "
	      "launches of other players copied / %llu missed",
	    g_c.launches_forwarded, g_c.launches_dropped, g_c.launches_applied, g_c.launches_missed);
	::Msg("[coop]   router: spawns %llu fwd / %llu refused, destroys %llu fwd / %llu blocked, moves %llu fwd / %llu "
	      "blocked, hits %llu fwd / %llu blocked (+%llu direct on actor), deaths blocked %llu, other blocked %llu, "
	      "level exits %llu, save/load blocked %llu, vehicles refused %llu",
	    g_c.spawns_forwarded, g_c.spawns_refused, g_c.destroys_forwarded, g_c.destroys_blocked, g_c.moves_forwarded,
	    g_c.moves_blocked, g_c.hits_forwarded, g_c.hits_blocked, g_c.actor_hits_blocked, g_c.deaths_blocked,
	    g_c.other_blocked, g_c.level_changes, g_c.saves_blocked, g_c.vehicles_blocked);
	::Msg("[coop]   events in: parents %llu applied / %llu failed / %llu timed out (%u pending), deaths %llu (%u "
	      "pending), local %llu spawns / %llu destroys / %llu errors",
	    g_c.parent_applied, g_c.parent_failed, g_c.parent_timeouts, (u32)g_parent_targets.size(), g_c.deaths_applied,
	    (u32)g_death_targets.size(), g_c.local_spawns, g_c.local_destroys, g_c.local_errors);
	::Msg("[coop]   echo dropped: %llu spawns / world-object requests made in reaction to the host's story (R4 4); "
	      "%llu world messages (parents, deaths, local entities) dropped before live (R2 I2)",
	    g_c.echo_dropped, g_c.dropped_before_live);
}

bool client_filter(NET_Packet& P)
{
	if (!mgr().is_client() || !router_on())
		return false;
	const u32 saved = P.r_pos;
	const bool swallowed = filter_message(P);
	P.r_pos = saved;
	return swallowed;
}

bool client_block_actor_hit(const CActor* actor, const SHit* hit)
{
	if (coop::hold_loading_screen()) return true; // F3a T8 R1: nobody is hurt behind a held loading screen (a session never pauses)
	if (mgr().is_host())
		return requests::host_actor_hit_dropped(actor, hit); // R3 minor: friendly fire covers other players' explosions
	if (!hit || !mgr().is_client() || !router_on() || entities::client_applying())
		return false;
	const CGameObject* who = hit->who ? smart_cast<const CGameObject*>(hit->who) : nullptr;
	if (!who || who == actor || !sync::is_remote_driven(who))
		return false;
	++g_c.actor_hits_blocked; // the host sends this damage as PlayerHit
	return true;
}

void host_on_parent_changed(u16 item, u16 old_parent, u16 new_parent)
{
	if (!mgr().is_host())
		return;
	u16 holder = kNoId;
	if (old_parent != kNoId)
	{
		const u16 root = root_of(old_parent);
		if (root == kActor || world::is_player_proxy(root))
			holder = root;
	}
	xrCriticalSectionGuard guard(g_lock);
	g_parent_changes.emplace(item, old_parent); // keeps the first old parent of this frame
	if (new_parent != kNoId)
		g_last_holder.erase(item);
	else if (holder != kNoId)
		g_last_holder[item] = holder;
}

void host_on_death(u16 victim, u16 killer)
{
	if (!mgr().is_host())
		return;
	xrCriticalSectionGuard guard(g_lock);
	g_deaths.emplace_back(victim, killer);
}

void host_on_destroy(const CSE_Abstract* entity)
{
	if (!entity || !mgr().is_host())
		return;
	xrCriticalSectionGuard guard(g_lock);
	g_last_holder.erase(entity->ID);
	if (g_local_relayed.erase(entity->ID))
		g_local_destroyed.push_back(entity->ID);
}

void host_relay_local_spawn(CSE_Abstract* entity)
{
	if (!entity || !mgr().is_host())
		return;
	NET_Packet spawn;
	entity->Spawn_Write(spawn, FALSE);
	Writer w(spawn.B.count + 16);
	w.u8(0);
	w.u16(entity->ID);
	w.u16(entity->ID_Parent);
	w.u16(u16(spawn.B.count));
	w.bytes(spawn.B.data, spawn.B.count);
	net::host_broadcast(Msg::EntityLocal, w);
	{
		xrCriticalSectionGuard guard(g_lock);
		g_local_relayed.insert(entity->ID);
	}
	++g_c.local_spawn_sent;
}

void host_set_drop_position(u16 item, const Fvector& pos)
{
	xrCriticalSectionGuard guard(g_lock);
	g_drop_positions[item] = pos;
}

void host_relay_launch(u16 rocket, u16 launcher, const Fvector& pos, const Fvector& dir, const Fvector& vel,
    const Fvector& angular_vel, PeerId requester)
{
	if (!mgr().is_host())
		return;
	Writer w(64);
	w.u8(2);
	w.u16(rocket);
	w.u16(launcher);
	put_vec(w, pos);
	put_vec(w, dir);
	put_vec(w, vel);
	put_vec(w, angular_vel);
	// Same reliable lane as the EntityParent of this launch, which goes out later in frame(): the copies
	// launch before the parent change reaches them (else they would drop inert from the launcher).
	net::host_broadcast(Msg::EntityLocal, w, Lane::Reliable, requester);
	++g_c.launches_relayed;
}

bool client_refuse_vehicle(CGameObject* holder)
{
	if (!holder || !mgr().is_client() || !router_on() || entities::client_applying() || !smart_cast<CCar*>(holder))
		return false;
	// F2 T12: the call gate decides and logs (world, no request path: the host drives cars); round 1: refused while the
	// host's story is applied too, as before T12 (an engine apply scope returned above)
	callgate::Args args;
	args.runs_while_applying = false;
	if (callgate::client_gate("game_object:attach_vehicle", holder, args) == calls::Gate::Run)
		return false; // an ltx override lets this client drive
	g_vehicle_note = true; // the HUD note goes out from frame()
	++g_c.vehicles_blocked;
	return true;
}

u16 host_last_holder(u16 item)
{
	xrCriticalSectionGuard guard(g_lock);
	auto it = g_last_holder.find(item);
	return it == g_last_holder.end() ? kNoId : it->second;
}
} // namespace events
} // namespace coop
