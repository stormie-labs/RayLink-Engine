// Anomaly Coop — player survival, the state its files share (coop_survival.cpp, coop_survival_cam.cpp, coop_survival_hud.cpp,
// coop_survival_newchar.cpp). Private to them: nothing else includes it. Main thread unless a line says otherwise.
#pragma once

#include "coop_survival.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_death.h"
#include "coop_checkpoints.h"
#include "coop_hud_text.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_ids.h"
#include "xrCoop/coop_revive_hold.h"
#include "actor_input_handler.h"
#include "xr_level_controller.h"
#include "../Include/xrRender/KinematicsAnimated.h"

#include <atomic>

class CSE_Abstract;

namespace coop
{
namespace survival
{
// ---------------------------------------------------------------- tuning

constexpr float kDownHealth = 0.01f; // health of a player who is down: alive for the engine, 1% on the HUD
constexpr u32 kReviveHoldMs = revive::kHoldMs; // hold USE on a wounded teammate
constexpr u32 kGiveUpHoldMs = revive::kGiveUpMs; // wounded player holds USE to bleed out at once
constexpr u32 kLuaHudMs = 1000; // the Lua HUD (coop.revive_progress) polled within this long: it draws the down / revive text
constexpr float kRespawnLift = 0.2f; // a respawn point is on the ground: appear this much above it
constexpr float kOverheadHeight = 6.f; // evacuated with nobody alive: camera this high above the body
// The respawn a character is owed lives with him in the world: [coop_death] respawn = 1 in his custom data, saved with him like
// his checkpoint slot (coop_checkpoints). death::debt_after_report decides it.
const char* const kDebtSection = "coop_death";
const char* const kDebtKey = "respawn";
// Out of a hardcore run, kept the same way ([coop_death] out = 1; death::learn_out).
const char* const kOutKey = "out";
constexpr u32 kNewCharHoldMs = kGiveUpHoldMs; // an out player holds USE this long to start a new character
constexpr u32 kNewCharRetryMs = 10000; // no answer from the host (refused, or still retiring): the prompt comes back

// The ops of Msg::PlayerRevive and the refusals live in xrCoop/coop_revive_hold.h (with the hold's messages and machines).
using revive::OpRequest;
using revive::OpRevive;
using revive::OpRespawn;
using revive::OpSpectate;
using revive::OpRefused;
using revive::OpEvacuate;
using revive::OpBegin;
using revive::OpCancel;
using revive::OpLosses;
using revive::OpStash;
using revive::OpReviving;
using revive::OpStopped;
using revive::OpDone;
using revive::Refusal;
using revive::RefNone;
using revive::RefNotWounded;
using revive::RefTooFar;
using revive::RefNoMedkit;
using revive::RefBusy;
using revive::RefYouAreDown;
using revive::RefSelf;

// The text the player reads (coop_hud_text.h) and the objects of this machine (coop_world.h).
using hud_text::tr;
using hud_text::tr_format;
using world::object_here;

// ---------------------------------------------------------------- state

// What the camera of this machine's player shows (Local::cam; the log names it by its number).
enum class Cam : u8
{
	Own = 0, // his own eyes
	Down = 1, // from the ground he lies on (DownCam)
	Spectate = 2, // behind the teammate he watches (SpectateCam)
	Overhead = 3, // straight down at his body (OverheadCam)
};

// This machine's own player.
struct Local
{
	CActor* actor = nullptr;
	Life life = Life::Alive;
	u16 seq = 0; // falls of this character
	u64 since = 0;
	u64 bleed_end = 0;
	u64 last_sent = 0;
	u64 shield_until = 0; // death::hit_counts
	u64 fade_at = 0; // the watched player changed: death::spectate_volume
	bool fading = false;
	// controls taken away while down
	bool input_installed = false;
	bool slots_blocked = false;
	bool inv_blocked = false;
	bool inv_prev = false;
	Cam cam = Cam::Own;
	// evacuation (checkpoint mode): the host's countdown as seen here, 0 = no Evacuate for this death yet
	u64 evac_end = 0;
	// respawn
	Fvector respawn_pos{};
	checkpoint::Kind respawn_kind = checkpoint::Kind::None;
	u64 respawn_at = 0;
	bool respawn_fresh = false; // the respawn is a new character after a hardcore run (apply_respawn)
	// give up while wounded (armed once USE was seen released after the fall)
	u64 giveup_start = 0;
	bool giveup_armed = false;
	// reviving somebody else
	u16 revive_target = kNoId; // local id of the wounded avatar
	u16 revive_seq = 0;
	u16 revive_item = kNoId; // host numbering
	u64 revive_start = 0;
	xr_string revive_item_sect; // section of the medkit in hand (the HUD names it)
	revive::Announcer announce; // when the hold is told to the host (Begin / Cancel)
	// the hold I finished: the host's Done for this target and fall is its answer (host numbering)
	u16 sent_target = kNoId;
	u16 sent_seq = 0;
	u64 sent_at = 0;
	xr_string sent_item_sect;
	// somebody reviving me: the ring the host's Reviving started, on this machine's clock
	revive::LiftView lift;
	// while that ring is on, my time stands still (local_frame)
	revive::BleedPause bleed_pause;
	// spectating
	u16 spectate = kNoId; // local id of the watched avatar
	bool next_was_down = false, prev_was_down = false;
	// out of a hardcore run: holding USE for a new character (armed once USE was seen released)
	u64 newchar_start = 0;
	bool newchar_armed = false;
	u64 newchar_asked = 0; // when the request went out (Msg::NewCharacter, or the host's own), 0 = not asked
};
extern Local g_me;

// Other players as reported (host numbering, never me).
struct Remote
{
	Life life = Life::Alive;
	u16 seq = 0;
	u64 bleed_end = 0; // our clock
	u64 updated = 0;
	xr_string nick;
};
extern xr_map<u16, Remote> g_players;

// Host bookkeeping per character (host numbering, 0 = host actor).
struct HostRec
{
	death::Tracker tracker; // one decision per death; the evacuation countdown; out of the run (hardcore)
	u64 revive_pending_until = 0;
	revive::HostHold hold; // the reviver the host lets hold him (Begin / Cancel), while he is wounded
	bool has_anchor = false;
	Fvector anchor{}; // where the player was first seen alive in this world (respawn fallback)
	// the slot and belt items of the owner's Dead report of death `worn_seq`
	bool has_worn = false;
	u16 worn_seq = death::kNoSeq;
	xr_vector<u16> worn;
	u16 ordered_seq = death::kNoSeq; // a respawn order for this fall is out (cleared by any report but Alive, and by a leave)
	bool debt = false; // the respawn debt as last read / written (coop_status only: the world keeps the real one)
	death::NewLife new_life; // the last order of the host player's new life in this world (host_on_state's watch)
};
extern xr_map<u16, HostRec> g_host;
// Characters out of a hardcore run that retire, their things on the way into a stash (death::retire_step).
struct Retirement
{
	u64 since = 0;
	u16 stash = kNoId; // death::host_move_to_stash; kNoId: none was made (nothing to move, or it all dropped)
	// a client's character is retired (key off, his player sent the world again) and is asked to leave the world until his
	// entity is gone
	bool releasing = false;
	u64 next_try = 0;
	u32 tries = 0;
	// his body stands on a map server's level: the stash is made there (OpStash), its word comes back (host_stash_over)
	bool at_map_server; // set where a retirement starts (host_new_character)
	bool answered = false;
	bool things_stay = false;
};
extern xr_map<u16, Retirement> g_retire;
extern xr_vector<u16> g_stashing; // a map server: the retiring characters of its level whose stash is under way

// Lying poses (hold_pose): avatars by local id, and my own actor.
struct Pose
{
	Life life = Life::Alive;
	MotionID motion;
	bool missing = false; // the visual has none of the candidate motions
	const IKinematicsAnimated* visual = nullptr; // the visual `motion` was looked up in (compared, never called)
	bool logged = false; // a missing pose was told (once, until the character stands up)
	u64 since = 0; // when this pose began (a dead body lies still after its last move)
};
extern xr_map<u16, Pose> g_poses;
// My own actor's pose (own_pose_frame). While it holds, g_pose_actor names that actor: its own motions are off
// (CActor::g_SetAnimation asks own_pose_held, maybe on the GameThread worker: an atomic), and so is the aiming twist of
// its spine and head when g_my_twist_off (the bone callbacks follow the camera's pitch and would bend the lying body).
extern Pose g_my_pose;
extern std::atomic<const CActor*> g_pose_actor;
extern bool g_my_twist_off;

extern death::Rules g_rules; // this machine's player's (coop_survival.cpp: the world server's are g_world_rules)
extern std::atomic<const CActorCondition*> g_hook_owner; // condition of the actor that is down (a load makes a new one)

// HUD lines prepared by frame() for render_hud().
struct HudLine
{
	xr_string text;
	u32 color = 0xffffffff;
	bool big = false;
};
extern xr_vector<HudLine> g_hud;
extern u64 g_lua_polled; // when Lua last asked coop.revive_progress (0 = never): its HUD draws the down / revive text then
struct FeedLine
{
	xr_string text;
	u64 until = 0;
};
extern xr_deque<FeedLine> g_feed;

// Spectator camera target, prepared by frame(), read by the effector (main thread).
struct SpecCam
{
	bool valid = false;
	Fvector pos{}, look{};
};
extern SpecCam g_spec;

// Overhead camera over the own body (evacuated, nobody alive to watch), prepared by frame().
struct OverCam
{
	bool valid = false;
	Fvector body{};
	float height = kOverheadHeight; // cut by a ceiling
};
extern OverCam g_over;

struct Stats
{
	u64 falls = 0, deaths = 0, revives = 0, refused = 0, respawns = 0, spectates = 0, evacuations = 0;
	u64 states_out = 0, states_in = 0, orders_in = 0, requests_in = 0;
	u64 kills_intercepted = 0, proxy_kills_refused = 0, guard_fixes = 0, stale_orders = 0;
	u64 wipes = 0, worn_reports = 0, worn_refused = 0;
	u64 begins_in = 0, begins_refused = 0, holds_shown = 0, dones = 0; // the revive hold made visible
	u64 newchar_asked = 0, newchar_refused = 0, retired = 0, retire_cancelled = 0; // hardcore's new characters
};
extern Stats g_stat;

// While down the actor takes no movement, weapon or use commands (engine CActorInputHandler, the same
// mechanism a controller's psy grip uses); the torch and night vision still work while wounded.
class DownInput : public CActorInputHandler
{
public:
	bool authorized(int cmd) override
	{
		if (g_me.life == Life::Wounded)
			return cmd == kTORCH || cmd == kNIGHT_VISION;
		return false;
	}
	// CActor::IR_OnMouseMove divides the look speed by this (GetLookFactor): 2 = a wounded player turns at half speed
	float mouse_scale_factor() override { return g_me.life == Life::Wounded ? 2.f : 1.f; }
};
extern DownInput g_input;

// ---------------------------------------------------------------- coop_survival.cpp

bool live();
u16 my_host_id();
CActor* local_actor();
Life life_by_host_id(u16 host_id);
// The transport peer driving the character `host_id` (host numbering; 0 = the host's actor), kInvalidPeer if unknown.
PeerId peer_of_host_id(u16 host_id);
// The object id of the character `host_id` on this machine (the host numbers its own objects, a client maps them).
u16 local_of_host_id(u16 host_id);
// Host: an order to the owner of `target` (Msg::PlayerRevive).
void host_send_order(u16 target, const Writer& w);
// The server entity that carries a character's custom data: the host's actor (0) or a player character.
CSE_Abstract* character_entity(u16 character);
// [coop_death] <key> = 1 in the custom data of a character: the respawn debt (kDebtKey), out of a hardcore run (kOutKey).
bool flag_read(u16 character, const char* key);
void flag_write(u16 character, const char* key, bool on);
// The kind of revive item a section is: the prefix of kReviveItems it starts with ("medkit", "stimpack"), "" for none.
const char* revive_item_kind(const char* section);

// Every player known on this machine but me, in the order collect_players lists them, without copying anything:
// f(peer, host_id, local_id, nick). The nick lives as long as the call. f must not change the players or the session.
template <class F> void each_player(F&& f)
{
	if (!mgr().active())
		return;
	if (mode().decides_world())
	{
		mgr().session().for_each_joined([&f](const PeerInfo& p)
		{
			if (p.state != PeerState::Joined)
				return;
			const u16 proxy = join::proxy_of_peer(p.id);
			if (proxy != kNoId)
				f(p.id, proxy, proxy, p.nick.c_str());
		});
		return;
	}
	// Client: the host reports every player character (PlayerWounded roster, every 5 s).
	const u16 me = my_host_id();
	for (const auto& it : g_players)
	{
		if (it.first == me)
			continue;
		const u16 local = join::to_local(it.first);
		const PeerId peer = it.first == 0 ? kHostPeer : world::avatar_peer(local);
		f(peer, it.first, local, it.second.nick.empty() ? "?" : it.second.nick.c_str());
	}
}

// ---------------------------------------------------------------- coop_survival_cam.cpp

bool action_down(EGameActions a);
void set_cam(CActor* a, Cam cam);
void close_menus();
void hold_controls(CActor* a);
void release_controls(CActor* a);
void spectate_frame(u64 t);
void evacuated_frame(CActor* a, u64 t);
void own_pose_frame(CActor* a);

// ---------------------------------------------------------------- coop_survival_hud.cpp

void feed_line(const char* shown, const char* logged);
void feed(const char* fmt, ...);
void feed_tr(const char* id, const char* fallback);
void feed_tr_s(const char* id, const char* fallback, const char* value);
void feed_tr_d(const char* id, const char* fallback, int value);
void build_hud(u64 t);

// ---------------------------------------------------------------- coop_survival_newchar.cpp

bool host_send_new_life(u16 character);
void host_retire_frame(u64 t);
void on_new_character_msg(PeerId from, Reader&);
bool request_new_character(u64 t);
void newchar_frame(u64 t);
void level_stash_frame();
} // namespace survival
} // namespace coop
