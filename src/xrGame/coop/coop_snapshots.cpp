#include "stdafx.h"
#include "coop_entities.h"
#include "coop_entities_impl.h"
#include "attachable_item.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_remote.h"
#include "xrCoop/coop_enemy_rules.h"
#include "xrCoop/coop_mirror_pose_rules.h"

#include "Level.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/monsters/bloodsucker/bloodsucker.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "Torch.h"
#include "Weapon.h"
#include "HudItem.h"
#include "sight_manager.h"
#include "object_handler_planner.h"
#include "object_handler_space.h"
#include "inventory_item.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

namespace coop
{
namespace entities
{
u64 g_snapshots_sent = 0;
u64 g_hands_slow_unseen = 0;
xr_map<u16, MirrorTrack> g_tracks;
u64 g_pose_fallbacks = 0; // partitions of copies given their own idle (mirror_pose::PartPlay::Fallback)
// P101 review: when this process last took a snapshot sample of any creature (client_on_snapshot), 0 = none this session
// (mirror_pose::stream_reaches); the one "[coop-anim] no snapshot stream" line of the gap since then, and its budget.
u64 g_snapshot_in_at = 0;
bool g_pose_gap_said = false;

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

namespace
{
constexpr u32 kSnapshotIntervalMs = 66; // ~15 Hz
constexpr u32 kInterpDelayMs = 120;
constexpr size_t kSnapshotBudget = 1100; // bytes of entries per EntitySnapshot packet
constexpr size_t kSnapshotHeader = 6; // u32 time, u16 entries
constexpr size_t kSnapshotCountAt = 4;

u64 g_last_snapshot = 0;
// The host's packet, one creature's entry and its attachments, kept from snapshot to snapshot: their buffers keep their capacity.
Writer g_snapshot_packet(kSnapshotHeader + kSnapshotBudget + 128);
Writer g_snapshot_entry(128);
attachments::State g_snapshot_attachments;
u64 g_attachment_incomplete = 0, g_attachment_applies = 0, g_attachment_retries = 0;
u64 g_hands_waits = 0, g_hands_put_away = 0; // applies that waited for a copy's swap or hide; items put away
// Waits of the copies' hands (xrCoop/coop_hands_rules.h). Slow ones (hands::wait_is_slow), each of which ends in one
// of three ways or is still under way: with the hands as the host's, dropped (the host's item is not with the copy
// here), or unseen (its track went first: HandsTrack's destructor). Stuck ones (hands::wait_is_stuck) and those of them
// that ended with the hands as the host's. Waits with a check late across a frame stall (hands::wait_had_stall) and
// with one late while frames ran without the copy's update (hands::wait_was_starved). The longest slow wait that ended
// with the hands as the host's, the latest check.
u64 g_hands_slow = 0, g_hands_slow_ended = 0, g_hands_slow_dropped = 0;
u64 g_hands_stuck = 0, g_hands_stuck_ended = 0, g_hands_stalled = 0, g_hands_starved = 0;
u32 g_hands_longest_ms = 0, g_hands_latest_ms = 0;
// Copies whose first snapshot came a grace or more after they were first driven here without a track (their hands as
// their spawn or their last track left them till then: hands::first_snapshot_wait_ms), and the longest such gap.
u64 g_hands_unfed = 0;
u32 g_hands_unfed_ms = 0;
// Stalker copies driven here without a track, and the first time of each such stretch (note_trackless).
xr_map<u16, u64> g_hands_trackless;
// "[coop-hands]" lines of this session, a budget for each kind (log_allowed): the error line of a stuck wait, the
// put-away, the end of a slow wait, a copy's first snapshot coming late.
u32 g_hands_log = 0, g_hands_notes = 0, g_hands_ends = 0, g_hands_unfed_lines = 0;

// Ambient scripts run only on the host. Replicate their real attachments, not a list of known prop sections. `state` is
// refilled (its list keeps its capacity across snapshots).
void capture_attachments(CGameObject* obj, attachments::State& state)
{
	state.complete = true;
	state.items.clear();
	CInventoryOwner* owner = obj->cast_inventory_owner();
	if (!owner) return;
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
				return;
			}
			state.items.push_back(std::move(item));
		}
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

// Audit F5b, F5d (2026-10-31): what only a monster's AI on the host knows about its look. A puppet finds no enemy, so its
// update_invisibility always landed on full visibility: a client never saw a bloodsucker or a psysucker cloak. A psy dog's
// phantom shows itself at its leap.
enemy_rules::MonsterLook monster_look(CGameObject* obj)
{
	using enemy_rules::MonsterLook;
	if (!obj->getVisible())
		return MonsterLook::Hidden;
	if (const CAI_Bloodsucker* sucker = smart_cast<const CAI_Bloodsucker*>(obj))
	{
		const CAI_Bloodsucker::visibility_t state = sucker->get_visibility_state();
		if (state == CAI_Bloodsucker::no_visibility)
			return MonsterLook::CloakFull;
		if (state == CAI_Bloodsucker::partial_visibility)
			return MonsterLook::CloakPartial;
	}
	return MonsterLook::Shown;
}

void apply_monster_look(CGameObject* obj, enemy_rules::MonsterLook look)
{
	using enemy_rules::MonsterLook;
	const bool hidden = look == MonsterLook::Hidden;
	if (!!obj->getVisible() == hidden)
		obj->setVisible(hidden ? FALSE : TRUE);
	if (CAI_Bloodsucker* sucker = smart_cast<CAI_Bloodsucker*>(obj))
		sucker->force_visibility_state(look == MonsterLook::CloakFull ? CAI_Bloodsucker::no_visibility :
		    look == MonsterLook::CloakPartial ? CAI_Bloodsucker::partial_visibility : CAI_Bloodsucker::full_visibility);
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
		return obj->cast_base_monster() ? enemy_rules::look_flags(monster_look(obj)) : flags;
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

// Client: the torch the host wears on the puppet's head, attached first, else one of the sections the puppet may wear
// (sr_light's device_torch). The host's flag comes from an attached torch only; a lamp in the backpack stays dark.
CTorch* puppet_torch(CGameObject* obj)
{
	if (CTorch* t = attached_torch(obj))
		return t;
	CInventoryOwner* owner = smart_cast<CInventoryOwner*>(obj);
	if (!owner)
		return nullptr;
	for (PIItem item : owner->inventory().m_all)
		if (CTorch* t = item ? smart_cast<CTorch*>(item) : nullptr)
			if (owner->may_attach(t->cNameSect()))
				return t;
	return nullptr;
}

constexpr u8 kHandsLinesPerPuppet = 3;
constexpr u32 kHandsRetryMs = 100; // a swap or hide under way is looked at again shortly (it ends within a second)
constexpr u32 kFlagsRetryMs = 1000; // an item or a torch not here yet

u32 ms_since(u64 since, u64 now) { return now > since ? u32(_min(now - since, u64(u32(-1)))) : 0u; }

// A wait of the copy's hands ends: with the hands as the host's (`matched`), or dropped because the item the host holds
// is not with the copy here. A slow one says so in a plain line (the first few of each copy, within the session's
// budget) and is counted.
void end_wait(const CGameObject* obj, HandsTrack& track, u64 now, bool matched)
{
	track.waiting = false;
	if (!track.slow)
		return;
	const u32 waited = ms_since(track.waiting_since, now);
	if (matched)
	{
		++g_hands_slow_ended;
		if (track.stuck)
			++g_hands_stuck_ended;
		g_hands_longest_ms = _max(g_hands_longest_ms, waited);
	}
	else
		++g_hands_slow_dropped;
	if (track.ends < kHandsLinesPerPuppet && log_allowed(g_hands_ends))
	{
		++track.ends;
		const hands::Wait& w = track.wait;
		::Msg("[coop-hands] npc %u %s after %u ms, %u checks (%u ms of its own steps; the latest check %u ms late over %u "
		      "frame(s); an item the host does not hold seen %u ms)",
		    u32(obj->ID()), matched ? "hands as the host's" : "wait dropped, the host's item is not with it here", waited,
		    w.checks, w.own_ms, w.late_ms, w.late_frames, w.wrong_ms);
	}
}

// True when the puppet shows what the host reported; false when an item has not arrived yet (EntitySpawn): try again
// later, or while its planner still swaps or hides the item in its hands (HandsTrack::retry_ms: shortly). The item in
// hands goes through the puppet's object handler, as an avatar's does (coop_equip apply_equip).
bool apply_creature_flags(CGameObject* obj, u8 flags, u16 held, u64 now, HandsTrack& track)
{
	CAI_Stalker* stalker = obj->cast_stalker();
	if (!stalker)
	{
		if (obj->cast_base_monster())
			apply_monster_look(obj, enemy_rules::look_of(flags));
		return true;
	}
	// This check after the one before it: how long, over how many frames, against the retry that one asked for.
	const u32 gap = track.checked_at ? ms_since(track.checked_at, now) : 0u;
	const u32 frames = Device.dwFrame - track.checked_frame;
	const u32 due = track.retry_ms;
	track.checked_at = now;
	track.checked_frame = Device.dwFrame;
	track.retry_ms = kFlagsRetryMs;
	bool done = true;
	const bool lit = (flags & CreatureTorchLit) != 0;
	if (CTorch* t = puppet_torch(obj))
	{
		// The host's Lua wears a lit torch (enable_attachable_item -> CObjectHandler::attach); a puppet runs no binder, so
		// the copy wears it here as the host does. A torch lit in the backpack gets no head transform and shines from
		// wherever the item last was.
		if (lit != stalker->attached(t))
			t->enable(lit); // on: the head bone carries it, CObjectHandler::attach lights it; off: detach puts it out
		const bool worn = stalker->attached(t);
		if (lit && !worn)
			t->enable(false); // refused (CAI_Stalker::can_attach: a dead puppet): clean again for the next try
		if (t->torch_active() != (lit && worn))
			t->Switch(lit && worn);
		if (lit && !worn && stalker->g_Alive())
			done = false;
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
		{
			// not in its inventory here yet: the hands clocks start again with the item (a stale clock would put the next
			// item away at once and lengthen the next wait)
			track.wrong = false;
			if (track.waiting)
				end_wait(obj, track, now, false);
			return false;
		}
	}
	ClientApply scope; // local inventory moves of the puppet pass the client router
	const bool strapped = (flags & CreatureStrapped) != 0;
	stalker->CObjectHandler::set_goal(strapped ? MonsterSpace::eObjectActionStrapped : MonsterSpace::eObjectActionIdle, item);

	// The puppet's active item here, for the rule (coop_hands_rules.h).
	PIItem active = stalker->inventory().ActiveItem();
	CHudItem* active_hud = active ? active->cast_hud_item() : nullptr;
	CWeapon* active_weapon = active ? smart_cast<CWeapon*>(active) : nullptr;
	hands::Seen seen;
	seen.want_holds = (flags & CreatureHolds) != 0;
	seen.want_strapped = strapped;
	seen.held_id = item ? item->ID() : hands::kNoId;
	seen.active_id = active ? active->object().ID() : hands::kNoId;
	seen.active_hidden = active_hud && active_hud->IsHidden();
	seen.active_can_strap = active_weapon && active_weapon->can_be_strapped();
	// The held item: strappable, and one the object planner can put in hands at all (its operators are for weapons and
	// missiles only, as coop_equip's "planned" for avatars); another never becomes active and must not be waited for.
	PIItem held_item = item ? item->cast_inventory_item() : nullptr;
	CWeapon* held_weapon = held_item ? held_item->cast_weapon() : nullptr;
	seen.held_can_strap = held_weapon && held_weapon->can_be_strapped();
	seen.held_planned = held_item && (held_item->cast_weapon() || held_item->cast_missile());
	// The put-away clock runs while an item the host does not hold is visible in the puppet's hands; the right item, or
	// the item hidden or gone, stops it.
	const bool wrong = hands::wrong_item_shown(seen);
	if (wrong && !track.wrong)
		track.wrong_since = now;
	track.wrong = wrong;
	seen.ms_wrong_item_visible = wrong ? ms_since(track.wrong_since, now) : 0u;
	const hands::Decision rule = hands::decide(seen);

	// The weapon goes on the back only when the torso animation of strapping ends (CObjectActionStrapping::on_animation_end
	// sets eWorldPropertyStrapped, which CObjectHandler::weapon_bones reads); a puppet plays the host's motions, so that
	// end never comes and the weapon would stay in the right hand. The host's word sets the planner's world state itself.
	// The property is one for all of the stalker's weapons, so it says strapped only once the item the host straps is the
	// one shown here. Forced from the host's bit while another item is still active, it makes that item "strapped" too:
	// Hide needs it false, and the only operator left, Unstrapping, waits for an animation end a puppet never gets. Until
	// then the planner swaps the items.
	CPropertyStorage& world = stalker->CObjectHandler::planner().m_storage;
	world.set_property(ObjectHandlerSpace::eWorldPropertyStrapped, rule.strap);
	world.set_property(ObjectHandlerSpace::eWorldPropertyStrapped2Idle, false);

	// An item the host does not hold (its hands empty, or another item held) is still seen in the puppet's a grace later:
	// the planner's hide (CObjectActionHide: Activate(NO_ACTIVE_SLOT), then the hide motion's timer) did not end it. It is
	// put away here:
	// the stock hide request, then the end of the hide motion now (CWeaponMagazined::OnAnimationEnd(eHiding) and
	// CHudItem::OnMoveToRuck do SwitchState(eHidden)); its next inventory update clears the active slot. The weapon state
	// events stay local under this ClientApply, as a puppet's at death do (coop_remote.cpp on_puppet_die).
	if (rule.put_away && active)
	{
		stalker->inventory().Activate(NO_ACTIVE_SLOT);
		if (active_hud && !active_hud->IsHidden())
			active_hud->SwitchState(CHUDState::eHidden);
		track.wrong_since = now; // if this one does not take, the next push comes a grace later
		++g_hands_put_away;
	}

	if (!rule.done && !track.waiting)
	{
		track.waiting = true;
		track.waiting_since = now;
		track.wait = hands::Wait();
		track.slow = track.stuck = track.stalled = track.starved = false;
	}
	if (track.waiting)
	{
		hands::wait_check(track.wait, gap, due, frames, seen.ms_wrong_item_visible);
		g_hands_latest_ms = _max(g_hands_latest_ms, track.wait.late_ms);
		if (!track.slow && hands::wait_is_slow(ms_since(track.waiting_since, now)))
		{
			track.slow = true;
			++g_hands_slow;
		}
		if (!track.stalled && hands::wait_had_stall(track.wait))
		{
			track.stalled = true;
			++g_hands_stalled;
		}
		if (!track.starved && hands::wait_was_starved(track.wait))
		{
			track.starved = true;
			++g_hands_starved;
		}
	}
	if (rule.done)
	{
		if (track.waiting)
			end_wait(obj, track, now, true);
	}
	else
	{
		const u32 waited = ms_since(track.waiting_since, now);
		// A put-away is the rule's remedy, not a fault: a plain line (the first few of each copy, within the session's
		// budget) and the count in coop_status.
		if (rule.put_away && track.notes < kHandsLinesPerPuppet && log_allowed(g_hands_notes))
		{
			++track.notes;
			::Msg("[coop-hands] npc %u put away %u: the host holds %u (strap_wanted %u), %u ms and %u checks into this wait",
			    u32(obj->ID()), u32(seen.active_id), u32(seen.held_id), u32(seen.want_strapped), waited, track.wait.checks);
		}
		// The error line: the copy had its steps and its hands are still not the host's. Once per wait, the first few of
		// each copy, within the session's budget.
		if (!track.stuck && hands::wait_is_stuck(track.wait))
		{
			track.stuck = true;
			++g_hands_stuck;
			if (track.lines < kHandsLinesPerPuppet && log_allowed(g_hands_log))
			{
				++track.lines;
				const hands::Wait& w = track.wait;
				::Msg("! [coop-hands] npc %u held %u active %u strap_wanted %u put_away %u (strap %u, active hidden %u, "
				      "%u ms waiting, %u checks, %u ms of its own steps, the latest check %u ms late over %u frame(s), an "
				      "item the host does not hold seen %u ms, line %u)",
				    u32(obj->ID()), u32(seen.held_id), u32(seen.active_id), u32(seen.want_strapped), u32(rule.put_away),
				    u32(rule.strap), u32(seen.active_hidden), waited, w.checks, w.own_ms, w.late_ms, w.late_frames,
				    w.wrong_ms, g_hands_log);
			}
		}
	}

	if (!done)
		return false; // the torch: try again in a second
	if (!rule.done)
	{
		++g_hands_waits;
		track.retry_ms = kHandsRetryMs;
		return false;
	}
	return true;
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

} // namespace

void host_send_snapshots()
{
	PROF_EVENT("coop/coop_snapshots/host_send_snapshots");
	const u64 now = mgr().now_ms();
	if (now - g_last_snapshot < kSnapshotIntervalMs)
		return;
	g_last_snapshot = now;

	// The packet is written in place: its header first, the count patched in when it goes.
	Writer& w = g_snapshot_packet;
	const auto start = [&]()
	{
		w.clear();
		w.u32(u32(now));
		w.u16(0);
	};
	start();
	u16 in_packet = 0;
	auto flush = [&]()
	{
		if (!in_packet)
			return;
		w.patch_u16(kSnapshotCountAt, in_packet);
		net::host_broadcast(Msg::EntitySnapshot, w, Lane::Unreliable);
		++g_snapshots_sent;
		start();
		in_packet = 0;
	};

	Writer& entry = g_snapshot_entry;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CGameObject* obj = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
		if (!obj || obj->getDestroy() || !remote::is_puppet_type(obj) || world::is_player_proxy(obj->ID()))
			continue;
		CEntityAlive* alive = smart_cast<CEntityAlive*>(obj);
		float h, p, b;
		obj->XFORM().getHPB(h, p, b);
		const Fvector& pos = obj->Position();

		entry.clear();
		entry.u16(obj->ID());
		float v[3] = {pos.x, pos.y, pos.z};
		entry.vec3(v);
		entry.angle16(angle_normalize(h));
		entry.u8(u8(clampr(alive ? alive->GetfHealth() : 1.f, 0.f, 1.f) * 255.f));
		AnimState a;
		capture_anims(obj, a);
		u16 held;
		const u8 flags = creature_flags(obj, held);
		entry.u8(pack_anim_byte(a.count, flags)); // creature flags in the high nibble
		for (u8 k = 0; k < a.count; ++k)
		{
			entry.u32(a.motion[k]);
			entry.u16(a.time_ms[k]);
		}
		if (flags & CreatureHolds)
			entry.u16(held);
		if (flags & CreatureAims)
			write_aim(entry, *obj->cast_stalker());
		capture_attachments(obj, g_snapshot_attachments);
		g_snapshot_attachments.write(entry);
		if (in_packet && w.size() - kSnapshotHeader + entry.size() > kSnapshotBudget) flush();
		w.bytes(entry.ptr(), entry.size());
		if (++in_packet >= 255 || w.size() - kSnapshotHeader >= kSnapshotBudget)
			flush();
	}
	flush();
}

namespace
{
// What a copy shows at a render time: place, yaw and aim blended between the two samples around it; the rest of `sample` (health,
// motions, attachments) is the nearer one's, read in place.
struct Shown
{
	const MirrorSample* sample = nullptr;
	Fvector pos{};
	float yaw = 0.f;
	Fquaternion aim[3];

	void take(const MirrorSample& s)
	{
		sample = &s;
		pos = s.pos;
		yaw = s.yaw;
		for (int i = 0; i < 3; ++i)
			aim[i] = s.aim[i];
	}
};

bool interpolate(const MirrorTrack& t, u64 render_time, Shown& out)
{
	if (t.samples.empty())
		return false;
	if (t.samples.size() == 1 || render_time <= t.samples.front().local_time)
	{
		out.take(t.samples.front());
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
		out.sample = k < 0.5f ? &a : &b;
		out.pos.lerp(a.pos, b.pos, k);
		out.yaw = angle_lerp(a.yaw, b.yaw, k);
		for (int i = 0; i < 3; ++i)
			out.aim[i].slerp(a.aim[i], b.aim[i], clampr(k, 0.f, 1.f));
		return true;
	}
	out.take(t.samples.back());
	return true;
}
} // namespace

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
		if (!t.first_sample_at)
			t.first_sample_at = now;
		t.samples.push_back(std::move(s));
		while (t.samples.size() > 24)
			t.samples.pop_front();
		g_snapshot_in_at = now; // P101 review: the stream reaches this process (a taken sample, not a packet that broke)
		g_pose_gap_said = false;
	}
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

// This skeleton's spawn placeholder (CCharacterPhysicsSupport::in_NetSpawn plays "death_init" by name on a live NPC), looked up
// once per skeleton; kNoMotion when the model has none.
u32 placeholder_of(IKinematicsAnimated* ka, mirror_pose::Book& book)
{
	if (!book.placeholder_known)
	{
		const MotionID m = ka->ID_Cycle_Safe("death_init");
		book.placeholder = m.valid() ? m.val : instrument::kNoMotion;
		book.placeholder_known = true;
	}
	return book.placeholder;
}

namespace
{
// Channel 0 of the partition plays nothing, or nothing but the placeholder.
bool part_bare(IKinematicsAnimated* ka, u16 part, u32 placeholder)
{
	for (u32 i = 0, n = ka->LL_PartBlendsCount(part); i < n; ++i)
	{
		const CBlend* b = ka->LL_PartBlend(part, i);
		if (b && instrument::mirrored_channel(b->channel) && b->blend_state() != CBlend::eFREE_SLOT &&
		    b->motionID.val != placeholder)
			return false;
	}
	return true;
}

// P101: a stalker copy's partition no usable host motion reaches plays the copy's own standing idle (mirror_pose::fallback_motion,
// looked up by name in this model, never a host id), on channel 0 like the host motions that replace it later. Tried once
// per partition and skeleton; monsters and traders have no such motions (the line tells about them).
void play_own_idle(CGameObject* obj, IKinematicsAnimated* ka, u16 part, mirror_pose::Book& book)
{
	const u8 b = mirror_pose::bit(part);
	if (book.fallback_tried & b)
		return;
	book.fallback_tried |= b;
	if (!obj->cast_stalker() || !(parts_of(ka) & b))
		return;
	const char* name = mirror_pose::fallback_motion(ka->partitions().part(part)->Name.c_str());
	const MotionID m = name ? ka->ID_Cycle_Safe(name) : MotionID();
	if (m.valid() && ka->LL_PlayCycle(part, m, TRUE, nullptr, nullptr))
	{
		book.fallback |= b;
		++g_pose_fallbacks;
	}
}

// Every partition of the copy: the decider's motion where this model has it (as before P101), else keep, or the own idle
// for a partition still in the spawn placeholder (mirror_pose::part_play). `s` is null when no snapshot of the id is here.
void drive_parts(CGameObject* obj, IKinematicsAnimated* ka, mirror_pose::Book& book, const MirrorSample* s,
                 MirrorTrack* track, u64 now)
{
	const bool settled = mirror_pose::settled(book, now);
	const u32 placeholder = placeholder_of(ka, book);
	const u8 count = s ? s->anims.count : 0;
	for (u8 part = 0; part < kMaxPartitions; ++part)
	{
		const u32 value = part < count ? s->anims.motion[part] : instrument::kNoMotion;
		const u8 b = mirror_pose::bit(part);
		mirror_pose::PartFacts f;
		f.named = instrument::motion_present(value);
		f.valid = motion_valid(ka, value);
		f.placeholder = f.valid && value == placeholder;
		f.driven = (book.driven & b) != 0;
		if (!(f.named && f.valid) && !f.driven)
			f.bare = part_bare(ka, part, placeholder);
		switch (mirror_pose::part_play(f, settled))
		{
		case mirror_pose::PartPlay::Host:
		{
			MotionID m;
			m.val = value;
			CBlend* playing = nullptr;
			for (u32 i=0,n=ka->LL_PartBlendsCount(part); i<n; ++i)
			{
				CBlend* blend=ka->LL_PartBlend(part,i);
				if (blend && instrument::mirrored_channel(blend->channel) && blend->motionID==m && blend->blend_state()==CBlend::eAccrue) { playing=blend; break; }
			}
			if (!playing) playing=ka->LL_PlayCycle(u16(part),m,TRUE,nullptr,nullptr);
			if (playing)
			{
				const float phase=clampr(float(s->anims.time_ms[part])/1000.f,0.f,_max(0.f,playing->timeTotal-.001f));
				if (instrument::phase_needs_sync(phase,playing->timeCurrent)) playing->timeCurrent=phase;
				if (track)
					track->applied[part]=value;
				if (!f.placeholder)
					book.driven |= b; // the decider's placeholder drives nothing: the copy is still a T-pose suspect
			}
			break;
		}
		case mirror_pose::PartPlay::Fallback:
			play_own_idle(obj, ka, part, book);
			break;
		case mirror_pose::PartPlay::Keep:
			break;
		}
	}
}

// A stalker copy driven here without a track: the first time of each such stretch, where its wait for a first snapshot
// begins (note_first_snapshot ends it).
void note_trackless(CGameObject* obj, u64 now)
{
	if (!obj->cast_stalker())
		return;
	if (g_hands_trackless.find(obj->ID()) == g_hands_trackless.end())
		g_hands_trackless.insert(mk_pair(obj->ID(), now));
}

// The first check of a copy's hands on this track. A copy whose first snapshot came a grace or more after it was first
// driven here without a track had its hands as its spawn or its last track left them until then: a plain line and a
// count, apart from the waits of a swap or a hide.
void note_first_snapshot(const CGameObject* obj, const MirrorTrack& track, u64 appeared_at, u64 now)
{
	const auto it = g_hands_trackless.find(obj->ID());
	const u64 trackless_at = it != g_hands_trackless.end() ? it->second : 0;
	if (it != g_hands_trackless.end())
		g_hands_trackless.erase(it);
	const u32 unfed = hands::first_snapshot_wait_ms(appeared_at, trackless_at, track.first_sample_at);
	if (!hands::wait_is_slow(unfed))
		return;
	++g_hands_unfed;
	g_hands_unfed_ms = _max(g_hands_unfed_ms, unfed);
	if (log_allowed(g_hands_unfed_lines))
		::Msg("[coop-hands] npc %u had no snapshot for %u ms after it came here without one; its hands are checked from "
		      "now, %u ms after the first one came",
		    u32(obj->ID()), unfed, ms_since(track.first_sample_at, now));
}
} // namespace

bool drive_mirror(CGameObject* obj, mirror_pose::Book& book)
{
	PROF_EVENT("coop/coop_snapshots/drive_mirror");
	IKinematicsAnimated* ka = obj->Visual() ? smart_cast<IKinematicsAnimated*>(obj->Visual()) : nullptr;
	const u64 now = mgr().now_ms();
	++book.frames;
	auto it = g_tracks.find(obj->ID());
	Shown s;
	if (it == g_tracks.end() || !interpolate(it->second, now - kInterpDelayMs, s))
	{
		// P101: no snapshot of this id here: nothing places or animates the copy. Its stalker partitions still in the spawn
		// placeholder (the bind pose, arms out) get its own idle once it settled, and the line says why (before P101 this
		// returned at once, silently).
		MirrorTrack* track = it != g_tracks.end() ? &it->second : nullptr;
		if (!track)
			note_trackless(obj, now);
		if (ka)
			drive_parts(obj, ka, book, nullptr, track, now);
		watch_pose(obj, ka, book, track, nullptr, now);
		return false;
	}
	Fmatrix xform;
	xform.setHPB(s.yaw, 0.f, 0.f);
	xform.c.set(s.pos);
	obj->XFORM().set(xform);
	remote::set_health(obj, s.sample->health); // snapshot health (never kills, see remote::on_host_death)
	if (CAI_Stalker* stalker = obj->cast_stalker())
	{
		Fmatrix bones[3];
		for (int i = 0; i < 3; ++i)
			bones[i].rotation(s.aim[i]);
		stalker->sight().coop_set_rotations(bones[0], bones[1], bones[2]);
	}

	// State, not motion (no interpolation): the attachments of the shown sample, the creature flags and the item in hands of
	// the newest one.
	MirrorTrack& track = it->second;
	const attachments::State& attached = s.sample->attachments;
	if (((track.attachments_pending || !(attached == track.attachments_shown)) && now >= track.attachments_retry)
		|| now >= track.attachments_check)
	{
		track.attachments_check = now + 2000;
		if (apply_attachments(obj, attached))
		{
			track.attachments_pending = false;
			track.attachments_shown = attached; track.attachments_retry = 0; ++g_attachment_applies;
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
		if (!track.hands.checked_at && obj->cast_stalker())
			note_first_snapshot(obj, track, book.since_ms, now);
		if (apply_creature_flags(obj, want, want_held, now, track.hands))
		{
			track.flags_shown = want;
			track.held_shown = want_held;
			track.flags_retry = 0;
		}
		else
			track.flags_retry = now + track.hands.retry_ms;
	}

	if (ka)
	{
		drive_parts(obj, ka, book, s.sample, &track, now);
		watch_pose(obj, ka, book, &track, s.sample, now);
	}
	return true;
}

void reset_snapshots()
{
	g_tracks.clear();
	g_last_snapshot = 0;
	g_pose_fallbacks = 0;
	g_snapshot_in_at = 0;
	g_pose_gap_said = false;
	// The copies' hands: counts and line budgets are the session's.
	g_hands_trackless.clear();
	g_hands_waits = g_hands_put_away = g_hands_slow = g_hands_slow_ended = g_hands_slow_dropped = g_hands_slow_unseen = 0;
	g_hands_stuck = g_hands_stuck_ended = g_hands_stalled = g_hands_starved = g_hands_unfed = 0;
	g_hands_longest_ms = g_hands_latest_ms = g_hands_unfed_ms = 0;
	g_hands_log = g_hands_notes = g_hands_ends = g_hands_unfed_lines = 0;
}

void print_snapshot_status()
{
	::Msg("[coop]   NPC attachments: %llu incomplete host samples, %llu client applies, %llu retries",
		g_attachment_incomplete, g_attachment_applies, g_attachment_retries);
	u64 slow_under_way = 0;
	for (const auto& it : g_tracks)
		if (it.second.hands.waiting && it.second.hands.slow)
			++slow_under_way;
	::Msg("[coop]   NPC hands (R71): %llu applies waited for a swap or hide, %llu items put away; waits: %llu slow (%llu ended "
	      "as the host's, the longest %u ms; %llu dropped; %llu ended unseen; %llu under way), %llu stuck (%llu ended as the "
	      "host's), %llu stretched by a frame stall, %llu starved of the copy's own updates while frames ran (the latest "
	      "check %u ms late); %llu copies with no snapshot for a while after they came here without one (the longest %u ms)",
	    g_hands_waits, g_hands_put_away, g_hands_slow, g_hands_slow_ended, g_hands_longest_ms, g_hands_slow_dropped,
	    g_hands_slow_unseen, slow_under_way, g_hands_stuck, g_hands_stuck_ended, g_hands_stalled, g_hands_starved,
	    g_hands_latest_ms, g_hands_unfed, g_hands_unfed_ms);
}
} // namespace entities
} // namespace coop
