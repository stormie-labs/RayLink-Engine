#include "stdafx.h"
#include "coop_entities.h"
#include "coop_entities_impl.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_world.h"
#include "xrCoop/coop_mirror_pose_rules.h"

#include "Level.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "alife_simulator.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

namespace coop
{
namespace entities
{
// ---------------------------------------------------------------- poses in the log (P101, P103; xrCoop/coop_mirror_pose_rules.h)

// The partitions of a skeleton (bits); none for a model without motions (its partition table is not set then).
u8 parts_of(IKinematicsAnimated* ka)
{
	if (!ka || !ka->LL_MotionsSlotCount())
		return 0;
	u8 parts = 0;
	for (u16 p = 0; p < kMaxPartitions; ++p)
		if (ka->partitions().part(p))
			parts |= mirror_pose::bit(p);
	return parts;
}

namespace
{
// The name of a MotionID value in this model, or "-" (a walk over the slot's name table: for the log lines only).
const char* motion_name(IKinematicsAnimated* ka, u32 value)
{
	if (!motion_valid(ka, value))
		return "-";
	MotionID m;
	m.val = value;
	const accel_map* names = const_cast<shared_motions&>(ka->LL_MotionsSlot(m.slot)).motion_map();
	if (names)
		for (const auto& it : *names)
			if (it.second == m.idx)
				return it.first.c_str();
	return "?";
}

// "N slot(s): omf:motions ..." of this model, for the lines (P101: an id names another motion when the tables differ).
void describe_slots(IKinematicsAnimated* ka, string512& out)
{
	out[0] = 0;
	if (!ka)
	{
		xr_strcpy(out, "no animated model");
		return;
	}
	const u16 n = ka->LL_MotionsSlotCount();
	xr_sprintf(out, "%u slot(s):", u32(n));
	for (u16 i = 0; i < n && i < 6; ++i)
	{
		shared_motions& slot = const_cast<shared_motions&>(ka->LL_MotionsSlot(i));
		const MotionDefVec* defs = slot.motion_defs();
		string128 one;
		xr_sprintf(one, " %s:%u", slot.id().c_str() ? slot.id().c_str() : "?", defs ? u32(defs->size()) : 0u);
		xr_strcat(out, one);
	}
}

// The decider (the Coop host, the map server of a level): what it streams itself, once a round (mirror_pose::kRoundMs).
// Bounded lines; nothing here changes play. A round, not every snapshot: the name lookups stay off the 66 ms path, and what
// the lines look for lasts (a creature stuck in its placeholder, a seat taken for minutes, an object that never came).
struct RoundSeen
{
	const void* what = nullptr; // the object (or server entity) the streak is about: an id goes to another one later
	u8 streak = 0; // rounds in a row
};
u64 g_decider_round_at = 0;
xr_map<u16, RoundSeen> g_decider_unposed; // live creatures with a partition in the spawn placeholder or with no motion
xr_set<u16> g_decider_rootctl; // creatures the animation movement controller moved at the last round (P103)
xr_map<u16, RoundSeen> g_bodiless; // creature entities online here without a game object
u32 g_decider_lines = 0; // "[coop-anim] decider ... spawn placeholder" lines of this session (mirror_pose::line_allowed)
u32 g_rootctl_lines = 0; // "[coop-anim] decider ... movement controller" lines (P103)
u32 g_bodiless_lines = 0; // "[coop-anim] decider ... no game object" lines

// The streak of `id` this round: one more when the same thing was met at the last round, else a new one.
bool next_round(const xr_map<u16, RoundSeen>& last, xr_map<u16, RoundSeen>& this_round, u16 id, const void* what)
{
	const auto old = last.find(id);
	RoundSeen seen;
	seen.what = what;
	seen.streak = old != last.end() && old->second.what == what ? old->second.streak : u8(0);
	const bool say = mirror_pose::second_round(seen.streak);
	this_round[id] = seen;
	return say;
}

// One stalker of the decider in the round.
void watch_creature(CGameObject* obj, xr_map<u16, RoundSeen>& unposed, xr_set<u16>& rootctl)
{
	IKinematicsAnimated* ka = obj->Visual() ? smart_cast<IKinematicsAnimated*>(obj->Visual()) : nullptr;
	if (!ka)
		return;
	const u16 id = obj->ID();
	AnimState a;
	capture_anims(obj, a); // what the snapshots of this creature name
	// P103 (Kholod sat sunk into a table on a player's screen; not seen in game): an animation under the movement controller
	// (an animpoint or a smart cover that gives the motion a place, a root-mover motion) moves the object by the root key and
	// holds the root bone at identity (animation_movement_controller RootBoneCallback). A copy takes the object's place and
	// yaw from the snapshot and plays the same motion with no controller (make_remote destroys it), so its root bone plays the
	// root key on top. This line says whether the NPC that sinks plays such an animation on the decider.
	if (obj->animation_movement_controlled())
	{
		rootctl.insert(id);
		if (!g_decider_rootctl.count(id) && mirror_pose::line_allowed(g_rootctl_lines))
		{
			const u32 shown = a.count > 1 ? a.motion[1] : (a.count ? a.motion[0] : instrument::kNoMotion);
			::Msg("[coop-anim] decider: %u [%s] plays [%s] (%08x) under the animation movement controller: the root key "
			      "moves the object, its root bone stays (a copy plays that root key on top of the streamed place, P103) "
			      "(%u such line(s))",
			    u32(id), obj->cName().c_str(), motion_name(ka, shown), shown, g_rootctl_lines);
		}
	}
	// P101 (d): a live creature that names its spawn placeholder "death_init", or no motion, on a partition of its model in
	// two rounds in a row does not run its own animation update; its copies mirror that as a T-pose.
	CEntityAlive* alive = smart_cast<CEntityAlive*>(obj);
	if (!alive || !alive->g_Alive())
		return;
	const MotionID placeholder = ka->ID_Cycle_Safe("death_init");
	const u8 parts = parts_of(ka);
	u8 in_placeholder = 0, none = 0;
	for (u8 part = 0; part < kMaxPartitions; ++part)
	{
		if (!(parts & mirror_pose::bit(part)))
			continue;
		const u32 value = part < a.count ? a.motion[part] : instrument::kNoMotion;
		if (!instrument::motion_present(value))
			none |= mirror_pose::bit(part);
		else if (placeholder.valid() && value == placeholder.val)
			in_placeholder |= mirror_pose::bit(part);
	}
	if (!(in_placeholder | none) || !next_round(g_decider_unposed, unposed, id, obj) ||
	    !mirror_pose::line_allowed(g_decider_lines))
		return;
	::Msg("! [coop-anim] decider: %u [%s] visual [%s], alive, plays its spawn placeholder death_init on parts %x and no "
	      "motion on parts %x in two rounds %u s apart (parts %x; motions %08x %08x %08x %08x): its own animation update does "
	      "not run, its copies show that as a T-pose (P101) (%u such line(s))",
	    u32(id), obj->cName().c_str(), obj->cNameVisual().c_str(), u32(in_placeholder), u32(none),
	    u32(mirror_pose::kRoundMs / 1000), u32(parts), a.count > 0 ? a.motion[0] : instrument::kNoMotion,
	    a.count > 1 ? a.motion[1] : instrument::kNoMotion, a.count > 2 ? a.motion[2] : instrument::kNoMotion,
	    a.count > 3 ? a.motion[3] : instrument::kNoMotion, g_decider_lines);
}

// The round's last part, P101: a creature's server entity online on this decider's level with no game object (a lost or
// refused net_Spawn): its copies get no snapshot and stand in their spawn pose, a talk with it answers "gone"
// (coop_dialog.cpp find_owner). Two rounds in a row, so that a spawn under way is not counted.
void watch_entity(CSE_ALifeDynamicObject* e, xr_map<u16, RoundSeen>& bodiless)
{
	if (!e || !e->m_bOnline || e->ID == 0 || bodiless.count(e->ID)) // met this round already: once per round
		return;
	CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(e);
	if (!creature || !online_here(e) || game_object_here(e->ID))
		return;
	if (next_round(g_bodiless, bodiless, e->ID, e) && mirror_pose::line_allowed(g_bodiless_lines))
		::Msg("! [coop-anim] decider: %u [%s] (%s, %s) is online here with no game object in two rounds %u s apart: its "
		      "copies get no snapshot and stand in their spawn pose, a talk with it answers \"gone\" (P101) (%u such line(s))",
		    u32(e->ID), e->name_replace(), e->s_name.c_str(), creature->g_Alive() ? "alive" : "dead",
		    u32(mirror_pose::kRoundMs / 1000), g_bodiless_lines);
}

u32 g_pose_lines = 0; // "[coop-anim] copy ... may stand in its spawn pose" lines of this session (mirror_pose::line_allowed)
u32 g_copy_state_lines = 0; // "[coop-anim] copy ... :" lines asked for by other modules (describe_copy)
u32 g_pose_gap_lines = 0;
// Since when this client can take a snapshot sample (mirror_pose::stream_open_at), 0 while it cannot.
u64 g_stream_open_at = 0;
} // namespace

// The round.
void watch_decider(u64 now)
{
	PROF_EVENT("coop/coop_pose_diag/watch_decider");
	if (!mode().leads_level() || now < g_decider_round_at || !ai().get_alife() || !g_pGameLevel)
		return;
	g_decider_round_at = now + mirror_pose::kRoundMs;
	xr_map<u16, RoundSeen> unposed;
	xr_set<u16> rootctl;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CGameObject* obj = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
		// Stalkers (traders and monsters stream too, but their partitions need not all play channel 0, and animpoints and
		// smart covers are stalkers' only): the NPCs of P101 and P103.
		if (obj && !obj->getDestroy() && obj->cast_stalker() && !world::is_player_proxy(obj->ID()))
			watch_creature(obj, unposed, rootctl);
	}
	g_decider_unposed.swap(unposed);
	g_decider_rootctl.swap(rootctl);

	// The server entities of this level only (P101 review: the whole ALife registry, every item of the world, was walked on the
	// decider's main thread). Only objects of the current level can be online here. A squad's members are not in the level
	// registry (CSE_ALifeOnlineOfflineGroup::register_member takes them out): they are met through their squad.
	xr_map<u16, RoundSeen> bodiless;
	if (ai().alife().graph().level_exists())
		for (const auto& it : ai().alife().graph().level().objects())
		{
			CSE_ALifeDynamicObject* e = it.second;
			if (CSE_ALifeOnlineOfflineGroup* squad = e ? smart_cast<CSE_ALifeOnlineOfflineGroup*>(e) : nullptr)
			{
				for (const auto& member : squad->squad_members())
					watch_entity(member.second, bodiless);
			}
			else
				watch_entity(e, bodiless);
		}
	g_bodiless.swap(bodiless);
}

// P101: one bounded line for a live copy that may stand in its spawn pose, once per episode (mirror_pose::say_now), with
// what tells the triggers apart: snapshots of the id here, their age, the decider's health, its motions per partition, whether
// this model has them, what they are called here, and this model's motion tables.
void watch_pose(CGameObject* obj, IKinematicsAnimated* ka, mirror_pose::Book& book, const MirrorTrack* track,
                const MirrorSample* shown, u64 now)
{
	// The client takes snapshots only live in an up world (its EntitySnapshot handler and the world gate drop them before): a
	// copy is judged from then on, not while it joins or leaves a level.
	g_stream_open_at = mirror_pose::stream_open_at(g_stream_open_at, mgr().is_live_client() && world_up(), now);
	CEntityAlive* alive = obj->cast_entity_alive();
	if (!alive || !alive->g_Alive())
		return;
	const bool due = mirror_pose::watch_due(book, g_stream_open_at, now);
	mirror_pose::WatchFacts f;
	f.stream = mirror_pose::stream_reaches(g_snapshot_in_at, book.since_ms, now);
	f.track = track != nullptr;
	f.samples = track ? u32(track->samples.size()) : 0u;
	const MirrorSample* newest = track && !track->samples.empty() ? &track->samples.back() : nullptr;
	f.newest_age_ms = newest && now > newest->local_time ? now - newest->local_time : 0;
	f.decider_dead = newest && newest->health <= 0.f;
	// Partitions count for stalkers only: a monster's or a trader's model need not play channel 0 on each of its partitions,
	// and they have no idle of ours. They have no line (P101 review: no idle partition, Cause::None); the decider's
	// "online here with no game object" line covers every creature.
	f.present = obj->cast_stalker() ? parts_of(ka) : u8(0);
	f.driven = book.driven;
	const u32 placeholder = ka ? placeholder_of(ka, book) : instrument::kNoMotion;
	u32 host[kMaxPartitions] = {instrument::kNoMotion, instrument::kNoMotion, instrument::kNoMotion, instrument::kNoMotion};
	for (u8 part = 0; shown && part < shown->anims.count && part < kMaxPartitions; ++part)
	{
		host[part] = shown->anims.motion[part];
		const u8 b = mirror_pose::bit(part);
		if (instrument::motion_present(host[part]))
			f.named |= b;
		if (motion_valid(ka, host[part]))
		{
			f.valid |= b;
			if (host[part] == placeholder)
				f.placeholder |= b;
		}
	}
	const mirror_pose::Cause cause = mirror_pose::cause_of(f);
	if (cause == mirror_pose::Cause::NoStream)
	{
		// No stream reaches this process (the decider loads or saves, or streams to nobody here yet): every copy of the level
		// is in the same gap, so one line for the gap, not one per copy (those would fill g_pose_lines and hide the copies the
		// decider leaves out while it streams the rest). A new gap starts with the next sample taken (client_on_snapshot).
		if (due && !g_pose_gap_said)
		{
			g_pose_gap_said = true;
			if (mirror_pose::line_allowed(g_pose_gap_lines))
				::Msg("! [coop-anim] no snapshot stream to this client: copy %u [%s] has %llu ms with this model here and %s, "
				      "the last snapshot sample of any creature came %lld ms ago (-1: none this session); stalker copies with no "
				      "host motion stand in their own idle meanwhile (P101) (%u such line(s))",
				    u32(obj->ID()), obj->cName().c_str(), now > book.since_ms ? now - book.since_ms : 0ull,
				    f.samples ? "only stale samples" : "no sample", g_snapshot_in_at ? s64(now) - s64(g_snapshot_in_at) : -1ll,
				    g_pose_gap_lines);
		}
		return;
	}
	if (!mirror_pose::say_now(book, cause, due) || !mirror_pose::line_allowed(g_pose_lines))
		return;
	string512 slots;
	describe_slots(ka, slots);
	::Msg("! [coop-anim] copy %u [%s] visual [%s] may stand in its spawn pose: %s (%llu ms with this model here, online here %s, "
	      "%u sample(s), the newest %llu ms old, host health %.2f); parts %x, driven by the host %x, own idle %x; host motions "
	      "%08x %08x %08x %08x, in this model %x, the placeholder %x; here they are [%s] [%s] [%s]; %s (%u such line(s))",
	    u32(obj->ID()), obj->cName().c_str(), obj->cNameVisual().c_str(), mirror_pose::cause_name(cause),
	    now > book.since_ms ? now - book.since_ms : 0ull, g_online.count(obj->ID()) ? "yes" : "no", f.samples,
	    f.newest_age_ms, newest ? newest->health : -1.f, u32(f.present), u32(book.driven), u32(book.fallback), host[0], host[1],
	    host[2], host[3], u32(f.valid), u32(f.placeholder), motion_name(ka, host[0]), motion_name(ka, host[1]),
	    motion_name(ka, host[2]), slots, g_pose_lines);
}

void describe_copy(u16 id, const char* why)
{
	if (!mirror_pose::line_allowed(g_copy_state_lines))
		return;
	CObject* o = g_pGameLevel ? Level().Objects.net_Find(id) : nullptr;
	CGameObject* obj = o && !o->getDestroy() ? smart_cast<CGameObject*>(o) : nullptr;
	IKinematicsAnimated* ka = obj && obj->Visual() ? smart_cast<IKinematicsAnimated*>(obj->Visual()) : nullptr;
	const auto it = g_tracks.find(id);
	const MirrorTrack* track = it != g_tracks.end() ? &it->second : nullptr;
	const MirrorSample* newest = track && !track->samples.empty() ? &track->samples.back() : nullptr;
	const u64 now = mgr().now_ms();
	// What channel 0 of each partition plays here now (the accruing blend): a T-pose copy shows death_init.
	const char* plays[3] = {"-", "-", "-"};
	for (u16 part = 0; ka && part < 3; ++part)
		for (u32 i = 0, n = ka->LL_PartBlendsCount(part); i < n; ++i)
		{
			const CBlend* b = ka->LL_PartBlend(part, i);
			if (b && instrument::mirrored_channel(b->channel) && b->blend_state() == CBlend::eAccrue)
				plays[part] = motion_name(ka, b->motionID.val);
		}
	const CEntityAlive* alive = obj ? obj->cast_entity_alive() : nullptr;
	::Msg("[coop-anim] copy %u [%s] (%s): %s here, online here %s, %u sample(s), the newest %lld ms old (host health %.2f), "
	      "plays [%s] [%s] [%s] (%u such line(s))",
	    u32(id), obj ? obj->cName().c_str() : "no game object", why, !obj ? "none" : (alive && alive->g_Alive() ? "alive" : "dead"),
	    g_online.count(id) ? "yes" : "no", track ? u32(track->samples.size()) : 0u,
	    newest ? s64(now) - s64(newest->local_time) : -1ll, newest ? newest->health : -1.f, plays[0], plays[1], plays[2],
	    g_copy_state_lines);
}

void reset_pose_diag()
{
	// P101/P103 (review): the copies' and the decider's books and line budgets are the session's. The rounds hold object
	// pointers (RoundSeen::what) of a world that is gone.
	g_pose_lines = g_copy_state_lines = g_pose_gap_lines = 0;
	g_stream_open_at = 0;
	g_decider_round_at = 0;
	g_decider_unposed.clear();
	g_decider_rootctl.clear();
	g_bodiless.clear();
	g_decider_lines = g_rootctl_lines = g_bodiless_lines = 0;
}

void print_decider_poses()
{
	::Msg("[coop]   poses here (P101/P103), at the last round: %u stalkers in their spawn placeholder or with no motion, "
	      "%u under the animation movement controller, %u creatures online with no game object (%u + %u + %u lines)",
	    (u32)g_decider_unposed.size(), (u32)g_decider_rootctl.size(), (u32)g_bodiless.size(), g_decider_lines,
	    g_rootctl_lines, g_bodiless_lines);
}

void print_copy_poses()
{
	::Msg("[coop]   copy poses (P101): %llu partitions given their own idle, %u spawn-pose lines, %u copy state lines, %u "
	      "no-stream lines, the last snapshot sample %lld ms ago (-1: none this session)",
	    g_pose_fallbacks, g_pose_lines, g_copy_state_lines, g_pose_gap_lines,
	    g_snapshot_in_at ? s64(mgr().now_ms()) - s64(g_snapshot_in_at) : -1ll);
}
} // namespace entities
} // namespace coop
