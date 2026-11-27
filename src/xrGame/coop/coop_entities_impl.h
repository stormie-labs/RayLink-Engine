// The entities module inside (coop_entities.cpp, coop_snapshots.cpp, coop_roster.cpp, coop_pose_diag.cpp): what its files
// share. Other modules use coop_entities.h.
#pragma once

#include "coop_entities.h"
#include "coop_instrument.h"
#include "xrCoop/coop_ids.h"
#include "xrCoop/coop_attachments.h"
#include "xrCoop/coop_hands_rules.h"
#include "xrCoop/coop_reconcile.h"

class CSE_ALifeOnlineOfflineGroup;

namespace coop
{
namespace entities
{
constexpr u32 kMaxPartitions = 4;

extern u64 g_hands_slow_unseen; // HandsTrack counts a slow wait it takes along (coop_snapshots.cpp)

// Generic animation state: per bone partition the newest blend's motion and phase.
struct AnimState
{
	u32 motion[kMaxPartitions] = {instrument::kNoMotion, instrument::kNoMotion, instrument::kNoMotion, instrument::kNoMotion};
	u16 time_ms[kMaxPartitions] = {};
	u8 count = 0;
};

// A copy's hands as this machine follows the host's (MirrorTrack::hands, xrCoop/coop_hands_rules.h).
struct HandsTrack
{
	bool wrong = false; // its active item is visible and is not the one the host holds (hands::wrong_item_shown)
	u64 wrong_since = 0; // since then: the put-away clock (a put-away restarts it)
	u64 checked_at = 0; // the last check of the hands (0: none yet) and its frame
	u32 checked_frame = 0;
	u32 retry_ms = 0; // the next check the last one asked for, that long after it
	bool waiting = false; // the last check waited for a swap or a hide
	u64 waiting_since = 0;
	hands::Wait wait; // its checks
	bool slow = false, stuck = false; // this wait was counted slow, stuck
	bool stalled = false, starved = false; // and late across a frame stall, late while frames ran without the copy
	u8 lines = 0; // error lines of this copy (a stuck wait)
	u8 notes = 0; // put-away lines of this copy
	u8 ends = 0; // lines of this copy's slow waits ending

	HandsTrack() = default;
	HandsTrack(const HandsTrack&) = delete; // one per MirrorTrack: a copy would count its wait twice
	HandsTrack& operator=(const HandsTrack&) = delete;
	// The track goes (the copy offline or removed, reparented, the client going live, the session's end) with a slow
	// wait under way: that wait's end is not seen here, and it is counted as such.
	~HandsTrack()
	{
		if (waiting && slow)
			++g_hands_slow_unseen;
	}
};
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
	HandsTrack hands; // the put-away clock and the waits of its hands
	u64 first_sample_at = 0; // when the first snapshot sample of it came
	attachments::State attachments_shown;
	bool attachments_pending = true;
	u64 attachments_retry = 0, attachments_check = 0;
};

// ---------------------------------------------------------------- coop_entities.cpp
extern xr_set<u16> g_online; // local ids the host has online
extern u64 g_replaced, g_stale_spawns, g_squads_whole; // client_on_spawn counts them, the roster's status line prints them
bool online_here(const CSE_ALifeDynamicObject* e);
bool game_object_here(u16 id);
bool log_allowed(u32& n);
void write_switch(Writer& w, CSE_ALifeDynamicObject* e, bool online);
bool send_subtree(PeerId to, CSE_ALifeDynamicObject* root);
bool group_bound(const CSE_ALifeOnlineOfflineGroup& g);
void leave_group(CSE_ALifeDynamicObject* e);

// ---------------------------------------------------------------- coop_snapshots.cpp
extern xr_map<u16, MirrorTrack> g_tracks;
extern u64 g_snapshots_sent, g_pose_fallbacks, g_snapshot_in_at;
extern bool g_pose_gap_said;
void capture_anims(CGameObject* obj, AnimState& a);
u32 placeholder_of(IKinematicsAnimated* ka, mirror_pose::Book& book);
void host_send_snapshots();
void client_on_snapshot(PeerId, Reader& r);
void reset_snapshots();
void print_snapshot_status();

// ---------------------------------------------------------------- coop_roster.cpp
extern reconcile::Retry g_id_retry;
void host_on_resync(PeerId from, Reader& r);
void host_on_entity_request(PeerId from, Reader& r);
void client_on_hash(PeerId, Reader& r);
void client_on_roster(PeerId, Reader& r);
void client_on_traits(PeerId, Reader& r);
void client_on_squad(PeerId, Reader& r);
void reset_roster();
void print_roster_host();
void print_roster_client();

// ---------------------------------------------------------------- coop_pose_diag.cpp
u8 parts_of(IKinematicsAnimated* ka);
void watch_decider(u64 now);
void watch_pose(CGameObject* obj, IKinematicsAnimated* ka, mirror_pose::Book& book, const MirrorTrack* track,
                const MirrorSample* shown, u64 now);
void reset_pose_diag();
void print_decider_poses();
void print_copy_poses();
} // namespace entities
} // namespace coop

