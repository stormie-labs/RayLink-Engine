#include "stdafx.h"
#include "coop_entities.h"
#include "coop_instrument.h"
#include "attachable_item.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_remote.h"
#include "coop_requests.h"
#include "coop_sync_api.h" // F2 T14
#include "xrCoop/coop_mirror.h" // B2
#include "xrCoop/coop_attachments.h"

#include "Level.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "coop_alife.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "Torch.h"
#include "Weapon.h"
#include "sight_manager.h"
#include "object_handler_planner.h"
#include "object_handler_space.h"
#include "inventory_item.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrMessages.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

namespace coop
{
namespace entities
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u32 kSnapshotIntervalMs = 66; // ~15 Hz
constexpr u32 kInterpDelayMs = 120;
constexpr u32 kMaxPartitions = 4;
constexpr size_t kSnapshotBudget = 1100; // bytes per EntitySnapshot packet

// ---------------------------------------------------------------- host side

enum class HostEventKind : u8
{
	Registered,
	Released,
	Switched,
};
struct HostEvent
{
	HostEventKind kind;
	u16 id;
	bool online;
};
// Filled from ALife/server hooks (game thread or main thread, never concurrently with frame()).
xr_vector<HostEvent> g_host_events;
u64 g_last_snapshot = 0;
u64 g_host_spawns = 0, g_host_releases = 0, g_host_switches = 0, g_host_events_sent = 0, g_snapshots_sent = 0;
u64 g_attachment_incomplete = 0, g_attachment_applies = 0, g_attachment_retries = 0;

void write_packet(Writer& w, const NET_Packet& p)
{
	w.u16(u16(p.B.count));
	w.bytes(p.B.data, p.B.count);
}

bool read_packet(Reader& r, NET_Packet& p)
{
	const u16 n = r.u16();
	if (!r.ok() || n > sizeof(p.B.data))
		return false;
	p.B.count = n;
	p.r_pos = 0;
	return r.bytes(p.B.data, n);
}

void write_placement(Writer& w, const CSE_ALifeDynamicObject* e)
{
	float pos[3] = {e->o_Position.x, e->o_Position.y, e->o_Position.z};
	w.vec3(pos);
	float ang[3] = {e->o_Angle.x, e->o_Angle.y, e->o_Angle.z};
	w.vec3(ang);
	w.u32(e->m_tNodeID);
	w.u16(e->m_tGraphID);
}

void host_send_spawn(CSE_ALifeDynamicObject* e)
{
	NET_Packet spawn, update;
	e->Spawn_Write(spawn, FALSE);
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);

	Writer w(spawn.B.count + update.B.count + 32);
	w.u16(e->ID);
	w.u16(e->ID_Parent);
	w.boolean(e->m_bOnline);
	write_placement(w, e);
	write_packet(w, spawn);
	write_packet(w, update);
	net::host_broadcast(Msg::EntitySpawn, w);
	++g_host_spawns;
}

void write_switch(Writer& w, CSE_ALifeDynamicObject* e, bool online)
{
	w.u16(e->ID);
	w.boolean(online);
	write_placement(w, e);
	NET_Packet update;
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);
	write_packet(w, update);
}

void host_send_switch(CSE_ALifeDynamicObject* e, bool online)
{
	Writer w(256);
	write_switch(w, e, online);
	net::host_broadcast(Msg::EntitySwitch, w);
	++g_host_switches;
}

void host_process_events()
{
	if (g_host_events.empty())
		return;
	// Review R2 C2: KERNEL:disconnect/start run earlier in the same FrameMove (CApplication::OnFrame has a
	// higher priority than coop::on_frame): the simulator is gone, the events belong to the old world, and the
	// clients rejoin through SessionReset anyway.
	if (!ai().get_alife())
	{
		g_host_events.clear();
		return;
	}
	xr_vector<HostEvent> events;
	events.swap(g_host_events);
	for (const HostEvent& ev : events)
	{
		switch (ev.kind)
		{
		case HostEventKind::Registered:
			if (CSE_ALifeDynamicObject* e = ai().alife().objects().object(ev.id, true))
				host_send_spawn(e);
			break;
		case HostEventKind::Released:
		{
			Writer w(4);
			w.u16(ev.id);
			net::host_broadcast(Msg::EntityDestroy, w);
			++g_host_releases;
			break;
		}
		case HostEventKind::Switched:
			if (CSE_ALifeDynamicObject* e = ai().alife().objects().object(ev.id, true))
				host_send_switch(e, ev.online);
			break;
		}
	}
}

// Generic animation state: per bone partition the newest blend's motion and phase.
struct AnimState
{
	u32 motion[kMaxPartitions] = {instrument::kNoMotion, instrument::kNoMotion, instrument::kNoMotion, instrument::kNoMotion};
	u16 time_ms[kMaxPartitions] = {};
	u8 count = 0;
};

void capture_anims(CGameObject* obj, AnimState& a)
{
	a.count = 0;
	IKinematicsAnimated* ka = obj->Visual() ? smart_cast<IKinematicsAnimated*>(obj->Visual()) : nullptr;
	if (!ka)
		return;
	for (u32 part = 0; part < kMaxPartitions; ++part)
	{
		const u32 n = ka->LL_PartBlendsCount(part);
		const u32 pick = instrument::pick_mirror_blend(n, [&](u32 i) {
			instrument::BlendView v;
			if (const CBlend* b = ka->LL_PartBlend(part, i))
			{
				v.live = b->blend_state() != CBlend::eFREE_SLOT;
				v.fading = b->blend_state() == CBlend::eFalloff;
				v.weight = b->blendAmount;
				v.channel = b->channel;
			}
			return v;
		});
		const CBlend* best = pick < n ? ka->LL_PartBlend(part, pick) : nullptr;
		a.motion[part] = best ? best->motionID.val : instrument::kNoMotion;
		a.time_ms[part] = best ? u16(_min(best->timeCurrent, 65.f) * 1000.f) : 0;
		a.count = u8(part + 1);
	}
}

bool is_creature(CGameObject* obj)
{
	return remote::is_puppet_type(obj); // stalkers, monsters, traders (WP1)
}

// Ambient scripts run only on the host. Replicate their real attachments, not a list of known prop sections.
attachments::State capture_attachments(CGameObject* obj)
{
	attachments::State state;
	CInventoryOwner* owner = obj->cast_inventory_owner();
	if (!owner) return state;
	for (CAttachableItem* a : owner->attached_objects())
		if (a && a->enabled() && !smart_cast<CTorch*>(&a->item()))
		{
			attachments::Item item;
			item.id = a->item().object().ID();
			item.bone = a->bone_name().c_str();
			a->offset().getHPB(item.angles[0], item.angles[1], item.angles[2]);
			item.position[0] = a->offset().c.x;
			item.position[1] = a->offset().c.y;
			item.position[2] = a->offset().c.z;
			if (!item.valid() || state.items.size() == attachments::kMaxItems)
			{
				// A partial list must not be mistaken for a command to detach the remaining items.
				state.items.clear(); state.complete = false; ++g_attachment_incomplete;
				return state;
			}
			state.items.push_back(item);
		}
	return state;
}

// W5 C2.6: the head torch of an NPC is switched by the host's Lua (sr_light spawns/releases it, story effects
// enable/disable it as an attachment) and AI actions; a puppet runs none of that. Its state rides in the snapshot.
CTorch* attached_torch(CGameObject* obj)
{
	CInventoryOwner* owner = smart_cast<CInventoryOwner*>(obj);
	if (!owner)
		return nullptr;
	for (CAttachableItem* a : owner->attached_objects())
		if (CTorch* t = a ? smart_cast<CTorch*>(&a->item()) : nullptr)
			return t;
	return nullptr;
}

// Live 2026-10-24 (owner): "on the client you do not see an NPC raise its weapon at whoever aimed at it". A puppet runs
// no object handler of its own: its torso plays the host's aim, but its hands stayed empty. The item in hands rides in
// the snapshot (`held`, the host id, when CreatureHolds is set).
u8 creature_flags(CGameObject* obj, u16& held)
{
	u8 flags = 0;
	held = kNoId;
	CAI_Stalker* stalker = obj->cast_stalker();
	if (!stalker)
		return flags;
	if (CTorch* t = attached_torch(obj))
		if (t->torch_active())
			flags |= CreatureTorchLit;
	if (PIItem item = stalker->inventory().ActiveItem())
	{
		held = item->object().ID();
		flags |= CreatureHolds;
		if (CWeapon* w = smart_cast<CWeapon*>(item))
			if (w->strapped_mode())
				flags |= CreatureStrapped;
	}
	if (stalker->sight().enabled())
		for (const Fmatrix* m : {&stalker->sight().current_spine_rotation(), &stalker->sight().current_shoulder_rotation(),
			     &stalker->sight().current_head_rotation()})
			if (!m->i.similar(Fidentity.i, EPS_L) || !m->j.similar(Fidentity.j, EPS_L) || !m->k.similar(Fidentity.k, EPS_L))
			{
				flags |= CreatureAims;
				break;
			}
	return flags;
}

// Live 2026-10-24 (owner): "the NPC does not point its gun and walks oddly sideways in front of the client". A stalker
// aims by turning spine, shoulder and head bones (CSightManager::compute_aiming -> the animation's bone callbacks); a
// puppet runs no sight update, so its torso stayed square with its legs. The host's final rotations ride along.
void write_aim(Writer& w, CAI_Stalker& stalker)
{
	for (const Fmatrix* m : {&stalker.sight().current_spine_rotation(), &stalker.sight().current_shoulder_rotation(),
		     &stalker.sight().current_head_rotation()})
	{
		Fquaternion q;
		q.set(*m);
		w.u16(u16(pack_unit(q.x)));
		w.u16(u16(pack_unit(q.y)));
		w.u16(u16(pack_unit(q.z)));
		w.u16(u16(pack_unit(q.w)));
	}
}

// Client: any torch the puppet has, attached first (an item the host keeps detached is still lit by Switch).
CTorch* puppet_torch(CGameObject* obj)
{
	if (CTorch* t = attached_torch(obj))
		return t;
	CInventoryOwner* owner = smart_cast<CInventoryOwner*>(obj);
	if (!owner)
		return nullptr;
	for (PIItem item : owner->inventory().m_all)
		if (CTorch* t = item ? smart_cast<CTorch*>(item) : nullptr)
			return t;
	return nullptr;
}

// True when the puppet shows what the host reported; false when an item has not arrived yet (EntitySpawn): try again
// later. The item in hands goes through the puppet's object handler, as an avatar's does (coop_equip apply_equip).
bool apply_creature_flags(CGameObject* obj, u8 flags, u16 held)
{
	CAI_Stalker* stalker = obj->cast_stalker();
	if (!stalker)
		return true;
	bool done = true;
	const bool lit = (flags & CreatureTorchLit) != 0;
	if (CTorch* t = puppet_torch(obj))
	{
		if (t->torch_active() != lit)
			t->Switch(lit);
	}
	else if (lit)
		done = false;

	CGameObject* item = nullptr;
	if (flags & CreatureHolds)
	{
		CObject* o = held != kNoId ? Level().Objects.net_Find(held) : nullptr;
		if (o && o->H_Parent() == obj)
			item = smart_cast<CGameObject*>(o);
		if (!item)
			return false; // not in its inventory here yet
	}
	ClientApply scope; // local inventory moves of the puppet pass the client router
	const bool strapped = (flags & CreatureStrapped) != 0;
	stalker->CObjectHandler::set_goal(strapped ? MonsterSpace::eObjectActionStrapped : MonsterSpace::eObjectActionIdle, item);
	// Live 2026-10-25 (build 21): "an NPC at a campfire drinks beer with his gun in the same hand". The weapon goes on the
	// back only when the torso animation of strapping ends (CObjectActionStrapping::on_animation_end sets
	// eWorldPropertyStrapped, which CObjectHandler::weapon_bones reads); a puppet plays the host's motions, so that end
	// never comes and the weapon stayed in the right hand. The host's word sets the planner's world state itself.
	CPropertyStorage& world = stalker->CObjectHandler::planner().m_storage;
	world.set_property(ObjectHandlerSpace::eWorldPropertyStrapped, strapped);
	world.set_property(ObjectHandlerSpace::eWorldPropertyStrapped2Idle, false);
	return done;
}

bool apply_attachments(CGameObject* obj, const attachments::State& state)
{
	CInventoryOwner* owner = obj->cast_inventory_owner();
	if (!state.complete) return false;
	if (!owner) return state.items.empty();
	IKinematics* k = obj->Visual() ? obj->Visual()->dcast_PKinematics() : nullptr;
	if (!k) return false;
	// Copy IDs before enable(false) mutates the owner's attachment list. Torches retain their separate light path.
	std::vector<uint16_t> enabled;
	for (CAttachableItem* a : owner->attached_objects())
		if (a && !smart_cast<CTorch*>(&a->item())) enabled.push_back(a->item().object().ID());
	ClientApply scope;
	return attachments::reconcile(state, enabled,
		[&](uint16_t id) {
			CObject* o = Level().Objects.net_Find(id);
			CInventoryItem* item = o && o->H_Parent() == obj ? smart_cast<CInventoryItem*>(o) : nullptr;
			if (CAttachableItem* a = item ? smart_cast<CAttachableItem*>(item) : nullptr)
			{
				a->enable(false);
				if (owner->attached(item)) owner->detach(item);
			}
		},
		[&](const attachments::Item& wanted) {
			CObject* o = Level().Objects.net_Find(wanted.id);
			CInventoryItem* item = o && !o->getDestroy() && o->H_Parent() == obj ? smart_cast<CInventoryItem*>(o) : nullptr;
			CAttachableItem* a = item ? smart_cast<CAttachableItem*>(item) : nullptr;
			if (!a || smart_cast<CTorch*>(item) || item->m_pInventory != &owner->inventory()) return false;
			const u16 bone = k->LL_BoneID(wanted.bone.c_str());
			if (bone == BI_NONE) return false;
			Fmatrix offset;
			offset.setHPB(wanted.angles[0], wanted.angles[1], wanted.angles[2]);
			offset.c.set(wanted.position[0], wanted.position[1], wanted.position[2]);
			const Fmatrix& old = a->offset();
			const bool same_pose = a->bone_name() == wanted.bone.c_str() && old.i.similar(offset.i) &&
				old.j.similar(offset.j) && old.k.similar(offset.k) && old.c.similar(offset.c);
			if (!same_pose || !owner->attached(item))
			{
				a->enable(false);
				if (owner->attached(item)) owner->detach(item);
				a->coop_set_attach_position(wanted.bone.c_str(), offset);
				a->enable(true);
			}
			a->set_bone_id(bone); // a changed NPC visual may have different numeric bone IDs
			return owner->attached(item);
		});
}

void host_send_snapshots()
{
	const u64 now = mgr().now_ms();
	if (now - g_last_snapshot < kSnapshotIntervalMs)
		return;
	g_last_snapshot = now;

	Writer w(kSnapshotBudget + 128);
	u16 in_packet = 0;
	auto flush = [&]()
	{
		if (!in_packet)
			return;
		Writer out(w.size() + 8);
		out.u32(u32(now));
		out.u16(in_packet);
		out.bytes(w.ptr(), w.size());
		net::host_broadcast(Msg::EntitySnapshot, out, Lane::Unreliable);
		++g_snapshots_sent;
		w.clear();
		in_packet = 0;
	};

	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CGameObject* obj = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
		if (!obj || obj->getDestroy() || !is_creature(obj) || world::is_player_proxy(obj->ID()))
			continue;
		CEntityAlive* alive = smart_cast<CEntityAlive*>(obj);
		float h, p, b;
		obj->XFORM().getHPB(h, p, b);
		const Fvector& pos = obj->Position();

		Writer entry(128);
		entry.u16(obj->ID());
		float v[3] = {pos.x, pos.y, pos.z};
		entry.vec3(v);
		entry.angle16(angle_normalize(h));
		entry.u8(u8(clampr(alive ? alive->GetfHealth() : 1.f, 0.f, 1.f) * 255.f));
		AnimState a;
		capture_anims(obj, a);
		u16 held;
		const u8 flags = creature_flags(obj, held);
		entry.u8(pack_anim_byte(a.count, flags)); // W5 C2.6: creature flags in the high nibble
		for (u8 k = 0; k < a.count; ++k)
		{
			entry.u32(a.motion[k]);
			entry.u16(a.time_ms[k]);
		}
		if (flags & CreatureHolds)
			entry.u16(held);
		if (flags & CreatureAims)
			write_aim(entry, *obj->cast_stalker());
		capture_attachments(obj).write(entry);
		if (in_packet && w.size() + entry.size() > kSnapshotBudget) flush();
		w.bytes(entry.ptr(), entry.size());
		if (++in_packet >= 255 || w.size() >= kSnapshotBudget)
			flush();
	}
	flush();
}

// ---------------------------------------------------------------- client side

xr_set<u16> g_online; // local ids the host has online
int g_applying = 0; // > 0 while we execute a host command on our ALife

struct MirrorSample
{
	u64 local_time = 0;
	Fvector pos{};
	float yaw = 0.f;
	float health = 1.f;
	AnimState anims;
	u8 flags = 0; // CreatureFlags (W5 C2.6)
	u16 held = kNoId; // local id of the item in hands (CreatureHolds)
	attachments::State attachments;
	Fquaternion aim[3]; // spine, shoulder, head bone rotations (CreatureAims; identity without it)
};
struct MirrorTrack
{
	xr_deque<MirrorSample> samples;
	s64 clock_offset = 0;
	bool have_offset = false;
	u32 last_remote = 0;
	u32 applied[kMaxPartitions] = {};
	u8 flags_shown = 0xff; // W5 C2.6: creature flags the puppet shows; 0xff = not applied yet
	u16 held_shown = kNoId; // the item it holds
	u64 flags_retry = 0; // a failed apply (torch not here yet) is tried again from this time
	u64 flags_check = 0; // and the shown state is re-checked from time to time (local engine paths may switch it)
	attachments::State attachments_shown;
	bool attachments_pending = true;
	u64 attachments_retry = 0, attachments_check = 0;
};
xr_map<u16, MirrorTrack> g_tracks;
xr_set<u16> g_refetched; // entities asked again because their holder was missing (once each)
u64 g_client_spawns = 0, g_client_releases = 0, g_client_switches = 0, g_missing = 0, g_blocked_spawns = 0,
    g_forwarded_spawns = 0;
u64 g_unbound_dropped = 0; // member ids forgotten by squads from the wire (released, dead, switched alone, disbanded)
// B2 (live 2026-10-23, FATAL "child registered but not found" at a client's exit): the mirror's holder/child books.
u64 g_mirror_with_holder = 0; // children the host had offline, brought online with their holder here (mirror::placement)
u64 g_mirror_broken_holder = 0; // children left offline: their holder is online here without a server entity
u64 g_mirror_stale = 0; // stale child ids of a gone entity taken out of a holder's list before an EntitySpawn
u64 g_mirror_lost = 0; // child ids with no server entity met by a holder leaving the server (mirror_lost_child)
u64 g_mirror_listed_destroys = 0; // entities that left the server while their holder still listed them
// B2 review M2: two budgets. The routine "comes online in its holder" line fires for a whole inventory at a world's
// load and must not use up the lines of the repairs (the "!" lines a live run is read for).
u32 g_mirror_log = 0, g_mirror_log_info = 0;
bool log_allowed(u32& n) { return ++n <= 20 || (n % 100) == 0; }
bool mirror_log_allowed() { return log_allowed(g_mirror_log); }

struct ApplyGuard
{
	ApplyGuard() { ++g_applying; }
	~ApplyGuard() { --g_applying; }
};

void read_placement(Reader& r, Fvector& pos, Fvector& ang, u32& node, u16& graph)
{
	float p[3], a[3];
	r.vec3(p);
	r.vec3(a);
	pos.set(p[0], p[1], p[2]);
	ang.set(a[0], a[1], a[2]);
	node = r.u32();
	graph = r.u16();
}

void place(CSE_ALifeDynamicObject* e, const Fvector& pos, const Fvector& ang, u32 node, u16 graph_id)
{
	// Only objects that live in the graph registry move between its vertices (graph().change
	// requires used_ai_locations: a squad that is not bound to its members here has none and must
	// not be added to the level registry, see apply_group_switch).
	if (!e->m_bOnline && e->ID_Parent == kNoId && e->used_ai_locations() && e->m_tGraphID != graph_id &&
	    ai().game_graph().valid_vertex_id(graph_id) && ai().game_graph().valid_vertex_id(e->m_tGraphID))
		alife_rw().graph().change(e, e->m_tGraphID, graph_id);
	e->o_Position = pos;
	e->o_Angle = ang;
	e->m_tNodeID = node;
	e->m_tGraphID = graph_id;
}

void apply_update(CSE_ALifeDynamicObject* e, NET_Packet& update)
{
	update.r_pos = 0;
	u16 type;
	update.r_begin(type);
	if (type == M_UPDATE)
		e->UPDATE_Read(update);
}

// A squad lists its members by id. A squad from the save has them bound by the engine
// (on_after_game_load); a squad that came over the wire has null member pointers, which
// switch_online/offline and on_failed_switch_online dereference. Its members are ordinary
// objects here (m_group_id free), so the stock on_unregister never takes them out of it:
// unregister_member only forgets such an id (alife_online_offline_group.cpp).
bool group_bound(const CSE_ALifeOnlineOfflineGroup& g)
{
	for (const auto& m : g.squad_members())
		if (!m.second)
			return false;
	return true;
}

// Client: forget `id` in every squad that lists it unbound. On the host the creature has left its
// squad (release, death, a switch of its own); here nothing else would drop the id, and a stale id
// makes the squad switch whatever creature reuses it later.
void drop_unbound(u16 id)
{
	const CALifeGroupRegistry::OBJECTS& groups = ai().alife().groups().objects();
	for (const auto& it : groups)
	{
		CSE_ALifeOnlineOfflineGroup* g = it.second;
		const CSE_ALifeOnlineOfflineGroup::MEMBERS& members = g->squad_members();
		const auto found = members.find(id);
		if (found != members.end() && !found->second)
		{
			g->unregister_member(id);
			++g_unbound_dropped;
		}
	}
}

// The host switches squad members only together with their squad. A member that the host
// switches (or removes) on its own has left its squad there (death, script): do the same here,
// otherwise the squad keeps a raw pointer to it (bound) or a stale id (from the wire).
void leave_group(CSE_ALifeDynamicObject* e)
{
	CSE_ALifeMonsterAbstract* m = smart_cast<CSE_ALifeMonsterAbstract*>(e);
	if (!m)
		return;
	if (m->m_group_id == kNoId)
	{
		drop_unbound(m->ID);
		return;
	}
	const CALifeGroupRegistry::OBJECTS& groups = ai().alife().groups().objects();
	auto it = groups.find(m->m_group_id);
	if (it == groups.end() || !it->second->member(m->ID, true))
	{
		m->m_group_id = kNoId;
		return;
	}
	it->second->unregister_member(m->ID);
}

// Client: a squad is going away. As on the host (remove_squad) it is empty when released: bound
// members become ordinary objects, unbound ids are forgotten (a destructor never meets them).
void disband(CSE_ALifeOnlineOfflineGroup& g)
{
	xr_vector<u16> ids;
	for (const auto& m : g.squad_members())
	{
		ids.push_back(m.first);
		if (!m.second)
			++g_unbound_dropped;
	}
	for (u16 id : ids)
		g.unregister_member(id);
}

// Client, a squad from the wire: the host took its dead out of it at death (on_death ->
// notify_on_member_death), nothing tells the squad here. Forget them, or its next switch drags
// the bodies offline while the host keeps them where they fell.
void drop_dead_unbound(CSE_ALifeOnlineOfflineGroup& g)
{
	xr_vector<u16> dead;
	for (const auto& m : g.squad_members())
	{
		CSE_ALifeMonsterAbstract* c =
		    m.second ? nullptr : smart_cast<CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(m.first, true));
		if (c && !c->g_Alive())
			dead.push_back(m.first);
	}
	for (u16 id : dead)
		g.unregister_member(id);
	g_unbound_dropped += dead.size();
}

void apply_switch(CSE_ALifeDynamicObject* e, bool online, const Fvector& pos, const Fvector& ang, u32 node,
                  u16 graph_id, NET_Packet* update)
{
	if (online)
	{
		if (e->m_bOnline)
			return;
		place(e, pos, ang, node, graph_id);
		if (update)
			apply_update(e, *update);
		alife_rw().switch_online(e);
		return;
	}
	if (e->m_bOnline)
		alife_rw().switch_offline(e);
	place(e, pos, ang, node, graph_id);
	if (update)
		apply_update(e, *update);
}

void apply_group_switch(CSE_ALifeOnlineOfflineGroup& g, bool online, const Fvector& pos, const Fvector& ang,
                        u32 node, u16 graph_id, NET_Packet& update)
{
	const bool bound = group_bound(g);
	if (!bound)
		drop_dead_unbound(g); // before the votes: a body's own vote is the host's (EntitySwitch of its own)
	xr_vector<u16> members;
	for (const auto& m : g.squad_members())
		members.push_back(m.first);
	// Members follow their squad: keep their votes in step for the local switch manager.
	for (u16 id : members)
	{
		if (online)
			g_online.insert(id);
		else
		{
			g_online.erase(id);
			g_tracks.erase(id);
		}
	}

	if (bound)
	{
		if (online && !g.m_bOnline)
		{
			place(&g, pos, ang, node, graph_id);
			apply_update(&g, update);
			// Offline members stand where their squad is (CSE_ALifeOnlineOfflineGroup::update on the host).
			for (const auto& m : g.squad_members())
			{
				if (m.second->m_bOnline)
					continue;
				m.second->o_Position = pos;
				m.second->m_tNodeID = node;
				m.second->m_tGraphID = graph_id;
			}
			alife_rw().switch_online(&g);
		}
		else if (!online)
		{
			if (g.m_bOnline)
				alife_rw().switch_offline(&g);
			place(&g, pos, ang, node, graph_id);
			apply_update(&g, update);
		}
		return;
	}

	// The squad came over the wire: its members are ordinary top-level objects here, switch them
	// one by one and leave the squad itself alone.
	place(&g, pos, ang, node, graph_id);
	apply_update(&g, update);
	for (u16 id : members)
	{
		CSE_ALifeMonsterAbstract* m = smart_cast<CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(id, true));
		if (m && m->ID_Parent == kNoId && m->m_group_id == kNoId)
			apply_switch(m, online, pos, ang, node, graph_id, nullptr);
	}
}

// The holder is online here, so the item must be too: on the host it was created online (like
// alife():create with an online parent). Same steps as add_online_impl takes for each child.
void bring_child_online(CSE_ALifeDynamicObject* e)
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

void client_on_spawn(PeerId, Reader& r)
{
	const u16 id_host = r.u16();
	const u16 id = join::to_local(id_host);
	const u16 parent_host = r.u16();
	const u16 parent = parent_host == kNoId ? kNoId : join::to_local(parent_host);
	const bool online = r.boolean();
	Fvector pos, ang;
	u32 node;
	u16 graph_id;
	read_placement(r, pos, ang, node, graph_id);
	NET_Packet spawn, update;
	if (!read_packet(r, spawn) || !read_packet(r, update) || !ai().get_alife())
		return;
	if (ai().alife().objects().object(id, true))
		return; // already have it (P6: repeat is harmless)
	if (alife_rw().server().ID_to_entity(id))
	{
		// A local non-ALife object (EntityLocal, bolt...) holds the id: PerformIDgen would assert.
		++g_missing;
		::Msg("! [coop] id %u is taken here by a local object, mirrored entity skipped", id_host);
		return;
	}
	if (!alife_rw().server().IsFreeID(id))
	{
		// F2 T12b round 1 (R2 minor "ID already used"): our generator has it taken with no entity holding it (a leased id
		// of ours, an entity between its steps): PerformIDgen below would assert
		++g_missing;
		::Msg("! [coop] id %u is taken in our id generator, mirrored entity skipped", id_host);
		return;
	}
	if (parent != kNoId && !ai().alife().objects().object(parent, true))
	{
		// Its holder is not here (P6): fetch the holder, then this entity again (once).
		++g_missing;
		if (g_refetched.insert(id).second)
		{
			Writer w(8);
			w.u16(parent_host);
			net::client_send(Msg::StateDumpRequest, w);
			Writer w2(4);
			w2.u16(id_host);
			net::client_send(Msg::StateDumpRequest, w2);
		}
		return;
	}

	u16 type;
	spawn.r_begin(type);
	string64 section;
	spawn.r_stringZ(section);
	CSE_Abstract* abstract = F_entity_Create(section);
	CSE_ALifeDynamicObject* e = smart_cast<CSE_ALifeDynamicObject*>(abstract);
	if (!e)
	{
		::Msg("! [coop] cannot create mirrored entity %u [%s]", id, section);
		F_entity_Destroy(abstract);
		return;
	}
	spawn.r_pos = 0;
	e->Spawn_Read(spawn);
	apply_update(e, update);
	ApplyGuard guard;
	e->ID = alife_rw().server().PerformIDgen(id); // free: checked above
	e->ID_Parent = parent;
	// Not registered yet: set the placement only, register_object puts it into the registries.
	e->o_Position = pos;
	e->o_Angle = ang;
	e->m_tNodeID = node;
	e->m_tGraphID = graph_id;
	e->m_bOnline = false;
	CSE_ALifeDynamicObject* holder = parent == kNoId ? nullptr : ai().alife().objects().object(parent, true);
	if (holder)
	{
		// B2: the id is neither in ALife nor on the server here, so a holder still listing it keeps a stale id of a gone
		// entity; register_object would list it twice and the holder's teardown would meet the stale one.
		xr_vector<u16>& list = holder->children;
		const auto stale = std::remove(list.begin(), list.end(), id);
		if (stale != list.end())
		{
			list.erase(stale, list.end());
			++g_mirror_stale;
			if (mirror_log_allowed())
				::Msg("! [coop] mirror: holder %u [%s] still listed %u, a gone entity, before its EntitySpawn: taken out",
				    parent_host, holder->name_replace(), id_host);
		}
	}
	alife_rw().register_object(e, true);
	// B2: a child follows its holder HERE; the host's flag of a child is its holder's state there (xrCoop/coop_mirror.h).
	switch (mirror::placement(parent != kNoId, online, holder && holder->m_bOnline,
	    holder && alife_rw().server().ID_to_entity(parent)))
	{
	case mirror::Placement::SwitchVote:
		g_online.insert(id);
		break;
	case mirror::Placement::WithHolder:
		if (!online)
		{
			++g_mirror_with_holder;
			if (log_allowed(g_mirror_log_info))
				::Msg("[coop] mirror: %u [%s] comes online in its holder %u [%s] here (offline on the host, where its "
				      "holder is offline)",
				    id_host, section, parent_host, holder->name_replace());
		}
		bring_child_online(e);
		break;
	case mirror::Placement::HolderBroken:
		++g_mirror_broken_holder;
		if (mirror_log_allowed())
			::Msg("! [coop] mirror: %u [%s] stays offline: its holder %u [%s] is online here without a server entity",
			    id_host, section, parent_host, holder->name_replace());
		break;
	case mirror::Placement::Offline:
		break;
	}
	++g_client_spawns;
}

void client_on_destroy(PeerId, Reader& r)
{
	const u16 id = join::to_local(r.u16());
	if (id == 0 || !ai().get_alife())
		return;
	g_online.erase(id);
	g_tracks.erase(id);
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
		return;
	ApplyGuard guard;
	leave_group(e); // on the host scripts unregister a member before releasing it
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
	{
		disband(*g);
		if (!g->m_bOnline && !ai().game_graph().valid_vertex_id(g->m_tGraphID))
			g->m_tGraphID = ai().alife().graph().actor()->m_tGraphID; // never placed: unregister reads its vertex
		alife_rw().release(e, true); // a squad has no game object, even when online
	}
	else if (e->m_bOnline)
	{
		// Online: the standard event chain destroys the game object and then the entity (E3).
		NET_Packet packet;
		packet.w_begin(M_EVENT);
		packet.w_u32(Level().timeServer());
		packet.w_u16(GE_DESTROY);
		packet.w_u16(id);
		Level().Send(packet, net_flags(TRUE, TRUE));
	}
	else
		alife_rw().release(e, true);
	++g_client_releases;
}

void client_on_switch(PeerId, Reader& r)
{
	const u16 id = join::to_local(r.u16());
	const bool online = r.boolean();
	Fvector pos, ang;
	u32 node;
	u16 graph_id;
	read_placement(r, pos, ang, node, graph_id);
	NET_Packet update;
	if (!read_packet(r, update) || id == 0 || !ai().get_alife())
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
	{
		++g_missing; // the host will not resend: ask for it (P3: find first, then create)
		Writer w(4);
		w.u16(join::to_host(id));
		net::client_send(Msg::StateDumpRequest, w);
		return;
	}
	ApplyGuard guard;
	if (online)
		g_online.insert(id);
	else
	{
		g_online.erase(id);
		g_tracks.erase(id);
	}
	++g_client_switches;
	if (e->ID_Parent != kNoId)
		return; // held by something here: it goes online/offline with its holder only
	leave_group(e);
	if (CSE_ALifeOnlineOfflineGroup* g = smart_cast<CSE_ALifeOnlineOfflineGroup*>(e))
		apply_group_switch(*g, online, pos, ang, node, graph_id, update);
	else
		apply_switch(e, online, pos, ang, node, graph_id, &update);
}

void client_on_snapshot(PeerId, Reader& r)
{
	const u32 remote_time = r.u32();
	const u16 count = r.u16();
	const u64 now = mgr().now_ms();
	for (u16 i = 0; i < count && r.ok(); ++i)
	{
		const u16 id = join::to_local(r.u16());
		MirrorSample s;
		float p[3];
		r.vec3(p);
		s.pos.set(p[0], p[1], p[2]);
		s.yaw = r.angle16();
		s.health = float(r.u8()) / 255.f;
		const u8 packed = r.u8();
		s.anims.count = anim_count_of(packed);
		if (s.anims.count > kMaxPartitions) return;
		s.flags = creature_flags_of(packed);
		for (u8 k = 0; k < s.anims.count; ++k)
		{
			s.anims.motion[k] = r.u32();
			s.anims.time_ms[k] = r.u16();
		}
		if (s.flags & CreatureHolds)
			s.held = join::to_local(r.u16());
		for (Fquaternion& q : s.aim)
		{
			q.identity();
			if (s.flags & CreatureAims)
			{
				q.x = unpack_unit(s16(r.u16()));
				q.y = unpack_unit(s16(r.u16()));
				q.z = unpack_unit(s16(r.u16()));
				q.w = unpack_unit(s16(r.u16()));
			}
		}
		if (!s.attachments.read(r) || id == 0)
			break;
		for (auto& item : s.attachments.items) item.id = join::to_local(item.id);
		MirrorTrack& t = g_tracks[id];
		if (t.have_offset && s32(remote_time - t.last_remote) <= 0)
			continue; // latest wins
		const s64 offset = s64(now) - s64(remote_time);
		if (!t.have_offset || offset < t.clock_offset)
			t.clock_offset = offset;
		else
			t.clock_offset += 1;
		t.have_offset = true;
		t.last_remote = remote_time;
		s.local_time = u64(s64(remote_time) + t.clock_offset);
		t.samples.push_back(s);
		while (t.samples.size() > 24)
			t.samples.pop_front();
	}
}

// Host: a client asks for an entity it does not have.
void host_on_entity_request(PeerId from, Reader& r)
{
	const u16 id = r.u16();
	if (!r.ok() || !ai().get_alife()) // review R2 C2: no world between levels / in the main menu
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
	if (!e)
		return;
	NET_Packet spawn, update;
	e->Spawn_Write(spawn, FALSE);
	update.w_begin(M_UPDATE);
	e->UPDATE_Write(update);
	Writer w(spawn.B.count + update.B.count + 32);
	w.u16(e->ID);
	w.u16(e->ID_Parent);
	w.boolean(e->m_bOnline);
	write_placement(w, e);
	write_packet(w, spawn);
	write_packet(w, update);
	net::host_send(from, Msg::EntitySpawn, w);
}

bool interpolate(MirrorTrack& t, u64 render_time, MirrorSample& out)
{
	if (t.samples.empty())
		return false;
	if (t.samples.size() == 1 || render_time <= t.samples.front().local_time)
	{
		out = t.samples.front();
		return true;
	}
	for (size_t i = 1; i < t.samples.size(); ++i)
	{
		const MirrorSample& b = t.samples[i];
		if (b.local_time < render_time)
			continue;
		const MirrorSample& a = t.samples[i - 1];
		const float span = float(b.local_time - a.local_time);
		const float k = span > 0.f ? float(render_time - a.local_time) / span : 1.f;
		out = k < 0.5f ? a : b;
		out.pos.lerp(a.pos, b.pos, k);
		out.yaw = angle_lerp(a.yaw, b.yaw, k);
		for (int i = 0; i < 3; ++i)
			out.aim[i].slerp(a.aim[i], b.aim[i], clampr(k, 0.f, 1.f));
		return true;
	}
	out = t.samples.back();
	return true;
}
} // namespace

// ---------------------------------------------------------------- API

void install_server(Session& s)
{
	s.on(Msg::StateDumpRequest, [](PeerId from, Reader& r) { if (mgr().is_host()) host_on_entity_request(from, r); });
}

void install_client(Endpoint& e)
{
	// Live only: before that the client is still in its own world (or the menu). The host queues
	// world changes for a loading client and flushes them after WorldLoaded (P6); what arrives
	// earlier predates the snapshot and is already inside it.
	e.on(Msg::EntitySpawn, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_spawn(from, r); });
	e.on(Msg::EntityDestroy, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_destroy(from, r); });
	e.on(Msg::EntitySwitch, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_switch(from, r); });
	e.on(Msg::EntitySnapshot, [](PeerId from, Reader& r) { if (mgr().is_live_client()) client_on_snapshot(from, r); });
}

void reset()
{
	g_host_events.clear();
	g_online.clear();
	g_tracks.clear();
	g_refetched.clear();
	g_last_snapshot = 0;
}

void on_peer_left(PeerId) {}

void host_begin_join(PeerId) {}
void host_end_join(PeerId) {}

// F3a T8 (live 2026-10-24: "the client sees nobody, dogs killed him unseen"): a mover of a simultaneous load got the
// prepared save with the host's actor as its only online object (the host's new world was not loaded yet), and the
// objects the host's world brought online while it loaded never reached him as switches. Once he is in, he gets the
// host's whole online set, as a joiner gets it in his world save (client_set_initial_online). A repeat changes nothing.
void host_send_online_to(PeerId to)
{
	if (!ai().get_alife())
		return;
	u32 sent = 0;
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!e || !e->m_bOnline || e->ID_Parent != kNoId || e->ID == 0)
			continue; // the host's actor is the client's proxy of him: no switch ever (host_on_switch skips 0)
		Writer w(256);
		write_switch(w, e, true);
		net::host_send(to, Msg::EntitySwitch, w);
		++sent;
	}
	g_host_switches += sent;
	::Msg("[coop] online set of the new world -> #%u: %u object(s)", to, sent);
}

void host_collect_online(xr_vector<u16>& out)
{
	out.clear();
	if (!ai().get_alife())
		return;
	// The host actor (0) is included: on the client it is our proxy, the host's avatar, and it
	// gets no EntitySwitch ever (host_on_switch skips 0), so without this it stays offline there.
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		if (e && e->m_bOnline && e->ID_Parent == kNoId)
			out.push_back(e->ID);
	}
}

void host_on_entity_registered(CSE_ALifeDynamicObject* e)
{
	if (mgr().is_host() && e)
		g_host_events.push_back({HostEventKind::Registered, e->ID, false});
}

void host_on_entity_released(u16 id)
{
	syncapi::on_entity_destroyed(id); // F2 T14: its coop.sync scope goes (host, single player; any game thread)
	if (mgr().is_host())
		g_host_events.push_back({HostEventKind::Released, id, false});
}

void host_on_switch(CSE_ALifeDynamicObject* e, bool online)
{
	if (mgr().is_host() && e && e->ID != 0)
		g_host_events.push_back({HostEventKind::Switched, e->ID, online});
}

void host_on_event(NET_Packet&) {}

void client_set_initial_online(const xr_vector<u16>& host_ids)
{
	g_online.clear();
	for (u16 id : host_ids)
		g_online.insert(join::to_local(id));
	g_online.erase(0);
	::Msg("[coop] host has %u objects online", (u32)g_online.size());
}

bool client_wants_online(u16 id) { return g_online.count(id) != 0; }

void client_on_holder_changed(u16 id, u16 new_parent, bool host_online)
{
	if (id == 0 || id == kNoId)
		return;
	if (new_parent == kNoId && host_online)
		g_online.insert(id);
	else
	{
		g_online.erase(id); // in a holder it follows the holder; a free offline item stays offline
		g_tracks.erase(id);
	}
}

void on_client_live() { g_tracks.clear(); }

bool mirror_lost_child(CSE_Abstract* holder, u16 child)
{
	if (!holder)
		return false;
	CSE_ALifeDynamicObject* c = ai().get_alife() ? ai().alife().objects().object(child, true) : nullptr;
	CSE_ALifeDynamicObject* h = ai().get_alife() ? ai().alife().objects().object(holder->ID, true) : nullptr;
	CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(c);
	const bool detachable =
	    item && h == holder && ai().get_game_graph() && ai().game_graph().valid_vertex_id(h->m_tGraphID);
	const mirror::LostChild what = mirror::lost_child(join::world_is_mirror(), c != nullptr, c && c->m_bOnline,
	    c && c->ID_Parent == holder->ID, detachable);
	if (what == mirror::LostChild::Fatal)
		return false; // single player, the host: the stock assert
	++g_mirror_lost;
	if (mirror_log_allowed())
	{
		string256 known;
		if (c)
			xr_sprintf(known, "[%s] in ALife, %s, held by %u", c->s_name.c_str(), c->m_bOnline ? "online" : "offline",
			    c->ID_Parent);
		else
			xr_strcpy(known, "neither in ALife");
		::Msg("! [coop] mirror: holder %u [%s][%s] lists child %u with no server entity here (%s): %s (local ids, 0 = "
		      "our actor)",
		    holder->ID, holder->name_replace(), holder->s_name.c_str(), child, known,
		    what == mirror::LostChild::Detach ? "taken out of it in ALife, it lies free and offline here"
		                                      : "dropped from its list");
	}
	if (what == mirror::LostChild::Detach)
		alife_rw().graph().detach(*h, item, h->m_tGraphID, true, true); // takes the id out of h->children
	else
	{
		xr_vector<u16>& list = holder->children;
		const auto it = std::find(list.rbegin(), list.rend(), child); // the last one: Perform_destroy reads back()
		if (it != list.rend())
			list.erase(std::next(it).base());
	}
	return true;
}

void mirror_on_destroy(xrServer& server, CSE_Abstract* entity)
{
	if (!entity || entity->ID_Parent == kNoId || !join::world_is_mirror() || server.ID_to_entity(entity->ID) != entity)
		return; // not on the server: add_online / bring_child_online free the id of an offline entity before its spawn
	CSE_Abstract* holder = ai().get_alife() ? ai().alife().objects().object(entity->ID_Parent, true) : nullptr;
	if (!holder)
		holder = server.ID_to_entity(entity->ID_Parent);
	if (!holder || std::find(holder->children.begin(), holder->children.end(), entity->ID) == holder->children.end())
		return;
	++g_mirror_listed_destroys;
	if (mirror_log_allowed())
		::Msg("! [coop] mirror: %u [%s] leaves the server while its holder %u [%s] still lists it (frame %u, local ids)",
		    entity->ID, entity->s_name.c_str(), entity->ID_Parent, holder->name_replace(), Device.dwFrame);
}

bool is_mirrored(u16 id) { return mgr().is_client() && id != 0; }

bool client_applying() { return g_applying > 0; }

ClientApply::ClientApply() { ++g_applying; }
ClientApply::~ClientApply() { --g_applying; }

bool intercept_local_spawn(CSE_Abstract* entity)
{
	if (!mgr().is_client() || g_applying || !entity)
		return false;
	// Only the host creates entities. Things our player caused (parent is our actor or one of
	// its items) go to the host as a request; world-caused spawns (loot on death, smart
	// terrain population) are the host's business and are dropped here.
	u16 owner = entity->ID_Parent;
	for (int depth = 0; owner != kNoId && owner != 0 && depth < 4; ++depth)
	{
		CSE_ALifeDynamicObject* p = ai().get_alife() ? ai().alife().objects().object(owner, true) : nullptr;
		owner = p ? p->ID_Parent : kNoId;
	}
	if (owner == 0)
	{
		// Full state (ammo count, condition, flags) so nothing is created or lost by the round trip.
		requests::client_forward_spawn(entity);
		++g_forwarded_spawns;
	}
	else
		++g_blocked_spawns;
	return true;
}

bool motion_valid(IKinematicsAnimated* ka, u32 motion)
{
	if (!ka || !instrument::motion_present(motion))
		return false;
	MotionID m;
	m.val = motion;
	if (m.slot >= ka->LL_MotionsSlotCount())
		return false;
	// Read-only: shared_motions gives its definition table through a non-const accessor only.
	const MotionDefVec* defs = const_cast<shared_motions&>(ka->LL_MotionsSlot(m.slot)).motion_defs();
	return defs && m.idx < defs->size();
}

bool drive_mirror(CGameObject* obj)
{
	auto it = g_tracks.find(obj->ID());
	if (it == g_tracks.end())
		return false;
	MirrorSample s;
	if (!interpolate(it->second, mgr().now_ms() - kInterpDelayMs, s))
		return false;
	Fmatrix xform;
	xform.setHPB(s.yaw, 0.f, 0.f);
	xform.c.set(s.pos);
	obj->XFORM().set(xform);
	remote::set_health(obj, s.health); // WP1: snapshot health (never kills, see remote::on_host_death)
	if (CAI_Stalker* stalker = obj->cast_stalker())
	{
		Fmatrix bones[3];
		for (int i = 0; i < 3; ++i)
			bones[i].rotation(s.aim[i]);
		stalker->sight().coop_set_rotations(bones[0], bones[1], bones[2]);
	}

	// W5 C2.6: creature flags from the newest sample (state, not motion: no interpolation).
	MirrorTrack& track = it->second;
	const u64 now = mgr().now_ms();
	if (((track.attachments_pending || !(s.attachments == track.attachments_shown)) && now >= track.attachments_retry)
		|| now >= track.attachments_check)
	{
		track.attachments_check = now + 2000;
		if (apply_attachments(obj, s.attachments))
		{
			track.attachments_pending = false;
			track.attachments_shown = s.attachments; track.attachments_retry = 0; ++g_attachment_applies;
		}
		else
		{
			track.attachments_pending = true;
			track.attachments_retry = now + 100; ++g_attachment_retries;
		}
	}
	const u8 want = track.samples.back().flags;
	const u16 want_held = track.samples.back().held;
	if (((want != track.flags_shown || want_held != track.held_shown) && now >= track.flags_retry)
		|| now >= track.flags_check)
	{
		track.flags_check = now + 2000;
		if (apply_creature_flags(obj, want, want_held))
		{
			track.flags_shown = want;
			track.held_shown = want_held;
			track.flags_retry = 0;
		}
		else
			track.flags_retry = now + 1000;
	}

	IKinematicsAnimated* ka = obj->Visual() ? smart_cast<IKinematicsAnimated*>(obj->Visual()) : nullptr;
	if (ka)
	{
		for (u8 part = 0; part < s.anims.count; ++part)
		{
			const u32 value = s.anims.motion[part];
			if (!instrument::motion_present(value))
				continue;
			if (!motion_valid(ka, value))
				continue;
			MotionID m;
			m.val = value;
			CBlend* playing = nullptr;
			for (u32 i=0,n=ka->LL_PartBlendsCount(part); i<n; ++i)
			{
				CBlend* b=ka->LL_PartBlend(part,i);
				if (b && instrument::mirrored_channel(b->channel) && b->motionID==m && b->blend_state()==CBlend::eAccrue) { playing=b; break; }
			}
			if (!playing) playing=ka->LL_PlayCycle(u16(part),m,TRUE,nullptr,nullptr);
			if (playing)
			{
				const float phase=clampr(float(s.anims.time_ms[part])/1000.f,0.f,_max(0.f,playing->timeTotal-.001f));
				if (instrument::phase_needs_sync(phase,playing->timeCurrent)) playing->timeCurrent=phase;
				it->second.applied[part]=value;
			}
		}
	}
	return true;
}

void frame()
{
	if (mgr().is_host())
	{
		// Level-change review: what the engine recorded while the world was not up belongs to a world that is gone, or is
		// being built (every client rejoins from the new world's save): an id of the old one names another object in the
		// new one. R2 C2 did this when the simulator was null; a level in the making (g_pGameLevel set, simulator half
		// built, no actor) was not covered.
		if (!world_up())
		{
			g_host_events.clear();
			return;
		}
		host_process_events();
		host_send_snapshots();
	}
}

void print_status()
{
	::Msg("[coop]   NPC attachments: %llu incomplete host samples, %llu client applies, %llu retries",
		g_attachment_incomplete, g_attachment_applies, g_attachment_retries);
	if (mgr().is_host())
		::Msg("[coop]   world out: %llu spawns, %llu releases, %llu switches, %llu snapshot packets", g_host_spawns,
		    g_host_releases, g_host_switches, g_snapshots_sent);
	else
	{
		::Msg("[coop]   world in: %u online, %u tracked, %llu spawns, %llu releases, %llu switches, %llu missing, "
		    "local spawns %llu forwarded / %llu blocked",
		    (u32)g_online.size(), (u32)g_tracks.size(), g_client_spawns, g_client_releases, g_client_switches,
		    g_missing, g_forwarded_spawns, g_blocked_spawns);
		u32 wire_squads = 0, wire_ids = 0;
		if (ai().get_alife())
			for (const auto& it : ai().alife().groups().objects())
			{
				u32 n = 0;
				for (const auto& m : it.second->squad_members())
					if (!m.second)
						++n;
				wire_squads += n ? 1 : 0;
				wire_ids += n;
			}
		::Msg("[coop]   squads from the wire: %u now (%u unbound member ids), %llu ids forgotten", wire_squads, wire_ids,
		    g_unbound_dropped);
		::Msg("[coop]   mirror holders (B2): %llu children online with their holder (offline on the host), %llu left "
		      "offline in a holder without a server entity, %llu stale ids taken out, %llu lost children met, %llu left "
		      "the server still listed",
		    g_mirror_with_holder, g_mirror_broken_holder, g_mirror_stale, g_mirror_lost, g_mirror_listed_destroys);
	}
}
} // namespace entities
} // namespace coop
