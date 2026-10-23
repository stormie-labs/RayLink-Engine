// Anomaly Coop — player survival (WP8, F3 Task 8). See coop_survival.h for the life cycle and the wire layouts.
#include "stdafx.h"
#include "coop_survival.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_net.h"
#include "coop_script.h"
#include "coop_death.h"
#include "coop_checkpoints.h"
#include "coop_money.h"
#include "xrCoop/coop_checkpoint_store.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "../../xrEngine/xr_input.h"
#include "../../xrEngine/GameFont.h"
#include "Level.h"
#include "Actor.h"
#include "ActorCondition.h"
#include "ActorEffector.h"
#include "CameraEffector.h"
#include "actor_input_handler.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryOwner.h"
#include "UIGameCustom.h"
#include "ui/UIPdaWnd.h"
#include "ui/UIActorMenu.h"
#include "ui_base.h"
#include "MainMenu.h"
#include "string_table.h"
#include "xr_level_controller.h"
#include "ai_space.h"
#include "level_graph.h"
#include "ai_object_location.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <string>
#include <vector>

namespace coop
{
namespace survival
{
namespace
{
// ---------------------------------------------------------------- tuning

constexpr float kDownHealth = 0.01f; // health of a player who is down: alive for the engine, 1% on the HUD
constexpr u32 kReviveHoldMs = 3000; // hold USE on a wounded teammate
constexpr u32 kGiveUpHoldMs = 3000; // wounded player holds USE to bleed out at once
constexpr float kReviveRange = 2.5f; // reviver side
constexpr float kHostReviveRange = 4.5f; // host check (avatars are rendered 100 ms in the past)
constexpr u32 kStateRefreshMs = 5000; // own life state and (host) rules / roster resend: P5, late joiners
constexpr u32 kRevivePendingMs = 5000; // host: one medkit per fall, even if the order is slow
constexpr u32 kDamageMsPerHealth = 100000; // damage taken while down shortens bleed-out (0.1 health = 10 s)
constexpr u32 kRespawnSettleMs = 300; // owner: the Respawning state goes out before the teleport
constexpr u32 kRespawnRetryMs = 2000; // host: no body to respawn right now (not in the world yet), try again
constexpr float kRespawnLift = 0.2f; // a respawn point is on the ground: appear this much above it
constexpr u32 kStaleRemoteMs = 15000; // client: forget players the host stopped reporting
constexpr u32 kGuardMs = 1000;
constexpr float kCamDrop = 1.1f; // eye height drop while lying
constexpr float kOverheadHeight = 6.f; // evacuated with nobody alive: camera this high above the body
constexpr float kOverheadTurn = 0.08f; // ... turning this many radians a second
// The respawn a character is owed lives with him in the world (fix round 1, M-1 / M-3): [coop_death] respawn = 1 in his
// custom data, saved with him like his checkpoint slot (coop_checkpoints). death::debt_after_report decides it.
const char* const kDebtSection = "coop_death";
const char* const kDebtKey = "respawn";
// F3 Task 10 (T8 review): out of a hardcore run, kept the same way ([coop_death] out = 1; death::learn_out).
const char* const kOutKey = "out";
constexpr u32 kNewCharHoldMs = kGiveUpHoldMs; // an out player holds USE this long to start a new character
constexpr u32 kNewCharRetryMs = 10000; // no answer from the host (refused, or still retiring): the prompt comes back

enum Op : u8
{
	OpRequest = 0,
	OpRevive = 1,
	OpRespawn = 2,
	OpSpectate = 3,
	OpRefused = 4,
	OpEvacuate = 5,
};

enum Refusal : u8
{
	RefNone = 0,
	RefNotWounded = 1,
	RefTooFar = 2,
	RefNoMedkit = 3,
	RefBusy = 4,
	RefYouAreDown = 5,
	RefSelf = 6,
};

const char* refusal_text(u8 r)
{
	switch (r)
	{
	case RefNotWounded: return "the player is no longer wounded";
	case RefTooFar: return "too far away";
	case RefNoMedkit: return "no medkit";
	case RefBusy: return "somebody is already reviving this player";
	case RefYouAreDown: return "you are down yourself";
	case RefSelf: return "you cannot revive yourself";
	default: return "refused";
	}
}

// Items a reviver uses, by section prefix, in order of preference.
const char* const kReviveItems[] = {"medkit", "stimpack"};

// Stalker animations for a player lying on the ground, by preference (stock stalker_animation set;
// the first one the avatar's visual has is used; none = the owner's motions keep playing).
const char* const kWoundedPoses[] = {"wounded_heavy_1_idle_0", "wounded_1_idle_0", "wounded_idle_0", "wounded_heavy_idle_0"};
const char* const kDeadPoses[] = {"wounded_heavy_2_idle_0", "wounded_heavy_3_idle_0", "wounded_heavy_1_idle_0",
                                  "wounded_1_idle_0", "wounded_idle_0"};

// ---------------------------------------------------------------- console (host rules)

u32 g_cv_death_mode = u32(death::Mode::Checkpoint);
u32 g_cv_death_drop = u32(death::Drop::Drop);
float g_cv_bleedout_s = 60.f;
float g_cv_revive_health = 0.3f;
float g_cv_respawn_delay_s = 30.f;
float g_cv_contusion_min = 10.f;
float g_cv_death_money = 10.f; // percentages: CCC_Float like the other host rules (the options page reads floats)
float g_cv_death_wear = 10.f;
float g_cv_death_rank = 0.f;

// The WP8 modes respawn / respawn_keep live on in old user.ltx files: both mean checkpoint now (plan decision 11).
// CCC_Token shows and saves the first name of a value, so they come back as "checkpoint".
xr_token g_death_mode_tokens[] = {
	{"checkpoint", int(death::Mode::Checkpoint)},
	{"hardcore", int(death::Mode::Hardcore)},
	{"respawn", int(death::Mode::Checkpoint)},
	{"respawn_keep", int(death::Mode::Checkpoint)},
	{nullptr, 0},
};

xr_token g_death_drop_tokens[] = {
	{"drop", int(death::Drop::Drop)},
	{"corpse", int(death::Drop::Corpse)},
	{"lose", int(death::Drop::Lose)},
	{nullptr, 0},
};

const char* mode_name(death::Mode m) { return m == death::Mode::Hardcore ? "hardcore" : "checkpoint"; }

const char* drop_name(death::Drop d)
{
	switch (d)
	{
	case death::Drop::Corpse: return "corpse";
	case death::Drop::Lose: return "lose";
	default: return "drop";
	}
}

u8 console_pct(float v) { return u8(iFloor(clampr(v, 0.f, 100.f) + 0.5f)); }

death::Rules console_rules()
{
	death::Rules r;
	r.mode = g_cv_death_mode == u32(death::Mode::Hardcore) ? death::Mode::Hardcore : death::Mode::Checkpoint;
	r.drop = death::Drop(_min(g_cv_death_drop, u32(death::Drop::Lose)));
	r.bleedout_ms = u32(g_cv_bleedout_s * 1000.f);
	r.revive_health = g_cv_revive_health;
	r.delay_ms = u32(clampr(g_cv_respawn_delay_s, 0.f, 300.f) * 1000.f);
	r.contusion_s = u32(clampr(g_cv_contusion_min, 0.f, 60.f) * 60.f + 0.5f);
	r.money_pct = console_pct(g_cv_death_money);
	r.wear_pct = console_pct(g_cv_death_wear);
	r.rank_pct = console_pct(g_cv_death_rank);
	return r;
}

death::Rules g_rules; // in force: host = console, client = last PlayerRules from the host
bool g_rules_from_host = false;

// ---------------------------------------------------------------- state

// This machine's own player.
struct Local
{
	CActor* actor = nullptr;
	Life life = Life::Alive;
	u16 seq = 0; // falls of this character
	u64 since = 0;
	u64 bleed_end = 0;
	u64 last_sent = 0;
	// controls taken away while down
	bool input_installed = false;
	bool slots_blocked = false;
	bool inv_blocked = false;
	bool inv_prev = false;
	u8 cam = 0; // 0 none, 1 lying, 2 spectator, 3 overhead
	// evacuation (checkpoint mode): the host's countdown as seen here, 0 = no Evacuate for this death yet
	u64 evac_end = 0;
	// respawn
	Fvector respawn_pos{};
	checkpoint::Kind respawn_kind = checkpoint::Kind::None;
	u64 respawn_at = 0;
	// give up while wounded (armed once USE was seen released after the fall)
	u64 giveup_start = 0;
	bool giveup_armed = false;
	// reviving somebody else
	u16 revive_target = 0xffff; // local id of the wounded avatar
	u16 revive_seq = 0;
	u16 revive_item = 0xffff; // host numbering
	u64 revive_start = 0;
	// spectating
	u16 spectate = 0xffff; // local id of the watched avatar
	bool next_was_down = false, prev_was_down = false;
	// out of a hardcore run (F3 Task 10): holding USE for a new character (armed once USE was seen released)
	u64 newchar_start = 0;
	bool newchar_armed = false;
	u64 newchar_asked = 0; // when the request went out (Msg::NewCharacter, or the host's own), 0 = not asked
};
Local g_me;

// Other players as reported (host numbering, never me).
struct Remote
{
	Life life = Life::Alive;
	u16 seq = 0;
	u64 bleed_end = 0; // our clock
	u64 updated = 0;
	xr_string nick;
};
xr_map<u16, Remote> g_players;

// Host bookkeeping per character (host numbering, 0 = host actor).
struct HostRec
{
	death::Tracker tracker; // one decision per death; the evacuation countdown; out of the run (hardcore)
	u64 revive_pending_until = 0;
	bool has_anchor = false;
	Fvector anchor{}; // where the player was first seen alive in this world (respawn fallback)
	// Ruling F3-4: the slot and belt items of the owner's Dead report of death `worn_seq`
	bool has_worn = false;
	u16 worn_seq = death::kNoSeq;
	xr_vector<u16> worn;
	u16 ordered_seq = death::kNoSeq; // a respawn order for this fall is out (cleared by any report but Alive, and by a leave)
	bool debt = false; // the respawn debt as last read / written (coop_status only: the world keeps the real one)
};
xr_map<u16, HostRec> g_host;
// F3 Task 10: characters out of a hardcore run that retire, their things on the way into a stash (death::retire_step).
struct Retirement
{
	u64 since = 0;
	u16 stash = 0xffff; // death::host_move_to_stash; 0xffff: none was made (nothing to move, or it all dropped)
};
xr_map<u16, Retirement> g_retire;
bool g_wipe_fired = false; // coop_group_wipe went out; again only after somebody stood up
u32 g_epoch = 0xffffffff;
u64 g_last_roster = 0;
u64 g_last_guard = 0;

// Avatar poses (local ids).
struct Pose
{
	Life life = Life::Alive;
	MotionID motion;
	bool missing = false; // the visual has none of the candidate motions
};
xr_map<u16, Pose> g_poses;

// Written by the condition hook (maybe on the GameThread worker), consumed by frame().
std::atomic<u8> g_hook_life{u8(Life::Alive)};
std::atomic<const CActorCondition*> g_hook_owner{nullptr}; // condition of the actor that is down (a load makes a new one)
std::atomic<bool> g_hook_fell{false};
std::atomic<u32> g_hook_damage{0}; // milli-health lost while down

// HUD lines prepared by frame() for render_hud().
struct HudLine
{
	xr_string text;
	u32 color = 0xffffffff;
	bool big = false;
};
xr_vector<HudLine> g_hud;
struct FeedLine
{
	xr_string text;
	u64 until = 0;
};
xr_deque<FeedLine> g_feed;

// Spectator camera target, prepared by frame(), read by the effector (main thread).
struct SpecCam
{
	bool valid = false;
	Fvector pos{}, look{};
};
SpecCam g_spec;

// Overhead camera over the own body (evacuated, nobody alive to watch), prepared by frame().
struct OverCam
{
	bool valid = false;
	Fvector body{};
	float height = kOverheadHeight; // cut by a ceiling
};
OverCam g_over;

struct Stats
{
	u64 falls = 0, deaths = 0, revives = 0, refused = 0, respawns = 0, spectates = 0, evacuations = 0;
	u64 states_out = 0, states_in = 0, orders_in = 0, requests_in = 0;
	u64 kills_intercepted = 0, proxy_kills_refused = 0, guard_fixes = 0, stale_orders = 0;
	u64 wipes = 0, worn_reports = 0, worn_refused = 0;
	u64 newchar_asked = 0, newchar_refused = 0, retired = 0; // F3 Task 10
} g_stat;
xr_set<u16> g_guard_logged;

// ---------------------------------------------------------------- helpers

bool live() { return mgr().is_host() || mgr().is_live_client(); }
u64 now() { return mgr().now_ms(); }
u16 my_host_id() { return mgr().is_host() ? u16(0) : join::my_proxy(); }

CActor* local_actor() { return g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr; }

CGameObject* object_here(u16 local_id)
{
	if (!g_pGameLevel || local_id == 0xffff)
		return nullptr;
	CGameObject* o = smart_cast<CGameObject*>(Level().Objects.net_Find(local_id));
	return o && !o->getDestroy() ? o : nullptr;
}

void feed(const char* fmt, ...)
{
	string512 buf;
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	buf[sizeof(buf) - 1] = 0;
	::Msg("[coop] %s", buf);
	FeedLine l;
	l.text = buf;
	l.until = now() + 7000;
	g_feed.push_back(l);
	while (g_feed.size() > 5)
		g_feed.pop_front();
}

// A HUD string of text/*/st_coop_survival.xml, `fallback` while the table does not have it.
xr_string tr(const char* id, const char* fallback)
{
	const shared_str s = CStringTable().translate(id);
	return s.size() && xr_strcmp(s.c_str(), id) != 0 ? xr_string(s.c_str()) : xr_string(fallback);
}

// A translated format with exactly one conversion `conv` (%d, %s) and no other '%' (death::one_conversion): a broken
// translation must not reach xr_sprintf with the wrong arguments.
xr_string tr_format(const char* id, const char* fallback, char conv)
{
	const xr_string f = tr(id, fallback);
	return death::one_conversion(f.c_str(), conv) ? f : xr_string(fallback);
}

bool action_down(EGameActions a)
{
	if (!pInput || (Console && Console->bVisible) || (MainMenu() && MainMenu()->IsActive()))
		return false;
	for (int i = 0; i < 2; ++i)
	{
		const int dik = get_action_dik(a, i);
		if (dik > 0 && pInput->iGetAsyncKeyState(dik))
			return true;
	}
	return false;
}

bool is_revive_section(const shared_str& sect)
{
	for (const char* prefix : kReviveItems)
		if (sect.size() && strstr(sect.c_str(), prefix) == sect.c_str())
			return true;
	return false;
}

PIItem find_revive_item(CActor* a)
{
	for (const char* prefix : kReviveItems)
		for (PIItem it : a->inventory().m_all)
			if (it && !it->object().getDestroy() && strstr(it->object().cNameSect().c_str(), prefix) ==
			    it->object().cNameSect().c_str())
				return it;
	return nullptr;
}

Remote* remote(u16 host_id)
{
	auto it = g_players.find(host_id);
	return it == g_players.end() ? nullptr : &it->second;
}

Life life_by_host_id(u16 host_id)
{
	if (host_id == my_host_id())
		return g_me.life;
	const Remote* p = remote(host_id);
	return p ? p->life : Life::Alive;
}

u16 seq_by_host_id(u16 host_id)
{
	if (host_id == my_host_id())
		return g_me.seq;
	const Remote* p = remote(host_id);
	return p ? p->seq : 0;
}

// ---------------------------------------------------------------- camera effectors

ECamEffectorType eff_down() { return (ECamEffectorType)0x434F4F01; }
ECamEffectorType eff_spectate() { return (ECamEffectorType)0x434F4F02; }
ECamEffectorType eff_overhead() { return (ECamEffectorType)0x434F4F03; }

// First-person camera of a player lying on the ground.
class DownCam : public CEffectorCam
{
public:
	DownCam() : CEffectorCam(eff_down(), 100000.f) { SetHudAffect(false); }
	BOOL ProcessCam(SCamEffectorInfo& info) override
	{
		fLifeTime = 100000.f;
		m_drop += (kCamDrop - m_drop) * _min(1.f, Device.fTimeDelta * 2.5f);
		info.p.y -= m_drop;
		Fmatrix roll;
		roll.rotation(info.d, m_drop * 0.2f);
		roll.transform_dir(info.n);
		return TRUE;
	}

private:
	float m_drop = 0.f;
};

// Third-person camera behind the watched teammate (hardcore spectator).
class SpectateCam : public CEffectorCam
{
public:
	SpectateCam() : CEffectorCam(eff_spectate(), 100000.f) { SetHudAffect(false); }
	bool AbsolutePositioning() override { return true; }
	BOOL ProcessCam(SCamEffectorInfo& info) override
	{
		fLifeTime = 100000.f;
		if (!g_spec.valid)
			return TRUE;
		const float k = m_init ? _min(1.f, Device.fTimeDelta * 5.f) : 1.f;
		m_pos.lerp(m_pos, g_spec.pos, k);
		m_look.lerp(m_look, g_spec.look, k);
		m_init = true;
		Fvector d;
		d.sub(m_look, m_pos);
		if (d.square_magnitude() < EPS_L)
			return TRUE;
		d.normalize();
		info.p.set(m_pos);
		info.d.set(d);
		info.n.set(0.f, 1.f, 0.f);
		return TRUE;
	}

private:
	bool m_init = false;
	Fvector m_pos{}, m_look{};
};

// Evacuated with nobody alive to watch: straight down at the own body from kOverheadHeight, slowly turning.
class OverheadCam : public CEffectorCam
{
public:
	OverheadCam() : CEffectorCam(eff_overhead(), 100000.f) { SetHudAffect(false); }
	bool AbsolutePositioning() override { return true; }
	BOOL ProcessCam(SCamEffectorInfo& info) override
	{
		fLifeTime = 100000.f;
		if (!g_over.valid)
			return TRUE;
		m_yaw += kOverheadTurn * Device.fTimeDelta;
		if (m_yaw > PI_MUL_2)
			m_yaw -= PI_MUL_2;
		info.p.set(g_over.body.x, g_over.body.y + g_over.height, g_over.body.z);
		info.d.set(0.f, -1.f, 0.f);
		info.n.set(_sin(m_yaw), 0.f, _cos(m_yaw)); // "up" on the screen turns: the view rotates about the body
		return TRUE;
	}

private:
	float m_yaw = 0.f;
};

void set_cam(CActor* a, u8 cam)
{
	if (g_me.cam == cam)
		return;
	if (a)
	{
		if (g_me.cam == 1)
			a->Cameras().RemoveCamEffector(eff_down());
		else if (g_me.cam == 2)
			a->Cameras().RemoveCamEffector(eff_spectate());
		else if (g_me.cam == 3)
			a->Cameras().RemoveCamEffector(eff_overhead());
		if (cam == 1)
			a->Cameras().AddCamEffector(xr_new<DownCam>());
		else if (cam == 2)
		{
			g_spec.valid = false;
			a->Cameras().AddCamEffector(xr_new<SpectateCam>());
		}
		else if (cam == 3)
			a->Cameras().AddCamEffector(xr_new<OverheadCam>());
	}
	g_me.cam = cam;
}

// ---------------------------------------------------------------- controls

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
	float mouse_scale_factor() override { return g_me.life == Life::Wounded ? 0.5f : 1.f; }
};
DownInput g_input;

void close_menus()
{
	CUIGameCustom* ui = CurrentGameUI();
	if (!ui)
		return;
	if (ui->GetActorMenu().IsShown())
		ui->HideActorMenu();
	if (ui->GetPdaMenu().IsShown())
		ui->GetPdaMenu().HideDialog();
}

void hold_controls(CActor* a)
{
	if (!a->input_external_handler_installed())
	{
		g_input.install(a); // clears movement wishes, releases the trigger
		g_me.input_installed = true;
	}
	if (!g_me.slots_blocked)
	{
		a->inventory().SetSlotsBlocked(INV_STATE_BLOCK_ALL, true); // weapon goes away (local, no event)
		g_me.slots_blocked = true;
	}
	if (!g_me.inv_blocked)
	{
		g_me.inv_prev = a->inventory_disabled();
		a->set_inventory_disabled(true);
		g_me.inv_blocked = true;
	}
	CActorCondition& c = a->conditions();
	g_hook_owner.store(&c);
	c.ClearWounds(); // the bleed-out timer is the bleeding: wounds would only shorten it twice
	if (c.GetHealth() != kDownHealth)
		c.SetHealth(kDownHealth); // GodMode or no condition update this frame
}

void release_controls(CActor* a)
{
	if (g_me.input_installed)
	{
		if (a && a->input_external_handler_installed())
			g_input.release();
		g_me.input_installed = false;
	}
	if (g_me.slots_blocked)
	{
		if (a)
			a->inventory().SetSlotsBlocked(INV_STATE_BLOCK_ALL, false);
		g_me.slots_blocked = false;
	}
	if (g_me.inv_blocked)
	{
		if (a)
			a->set_inventory_disabled(g_me.inv_prev);
		g_me.inv_blocked = false;
	}
	set_cam(a, 0);
	g_spec.valid = false;
	g_over.valid = false;
}

// ---------------------------------------------------------------- messages out

void write_state(Writer& w, u16 host_id, Life life, u16 seq, u32 left, const char* nick)
{
	w.u16(host_id);
	w.u8(u8(life));
	w.u16(seq);
	w.u32(left);
	w.str(nick ? nick : "");
}

// Ruling F3-4: the tail of a client's Dead report (death::write_worn): the items in its slots and on its belt in host
// numbering, each once. The host's proxy cannot show them (an NPC has no outfit or belt).
void write_worn(Writer& w, CActor& a)
{
	std::vector<u16> ids;
	const auto add = [&ids](PIItem it)
	{
		if (!it || it->object().getDestroy())
			return;
		const u16 id = join::to_host(it->object().ID());
		if (id != 0xffff && std::find(ids.begin(), ids.end(), id) == ids.end())
			ids.push_back(id);
	};
	CInventory& inv = a.inventory();
	for (u16 slot = inv.FirstSlot(); slot <= inv.LastSlot(); ++slot)
		add(inv.ItemFromSlot(slot));
	for (PIItem it : inv.m_belt)
		add(it);
	death::write_worn(w, ids); // the first death::kMaxWornIds
}

void host_on_state(u16 id, Life life, u16 seq);

void send_my_state(u64 t)
{
	g_me.last_sent = t;
	const u16 id = my_host_id();
	if (id == 0xffff)
		return;
	const u32 left = g_me.life == Life::Wounded && g_me.bleed_end > t ? u32(g_me.bleed_end - t) : 0;
	Writer w(32);
	write_state(w, id, g_me.life, g_me.seq, left, mgr().nick());
	if (mgr().is_host())
	{
		net::host_broadcast(Msg::PlayerWounded, w);
		host_on_state(0, g_me.life, g_me.seq);
	}
	else
	{
		if (g_me.life == Life::Dead && g_me.actor)
			write_worn(w, *g_me.actor); // the host takes the death's losses on this report (no actor: no list)
		net::client_send(Msg::PlayerWounded, w);
	}
	++g_stat.states_out;
}

void apply_order(u8 op, Reader& r);

// Host: deliver an order to the owner of `target` (the host itself: applied at once).
void host_send_order(u16 target, const Writer& w)
{
	if (target == 0)
	{
		Reader r(w.ptr(), w.size());
		const u8 op = r.u8();
		apply_order(op, r);
		return;
	}
	const PeerId peer = join::peer_of_proxy(target);
	if (peer != kInvalidPeer)
		net::host_send(peer, Msg::PlayerRevive, w);
}

void host_order_spectate(u16 target, u16 seq)
{
	Writer w(8);
	w.u8(OpSpectate);
	w.u16(target);
	w.u16(seq);
	host_send_order(target, w);
}

void host_order_evacuate(u16 target, u16 seq, u32 delay_ms)
{
	Writer w(12);
	w.u8(OpEvacuate);
	w.u16(target);
	w.u16(seq);
	w.u32(delay_ms);
	host_send_order(target, w);
}

// ---------------------------------------------------------------- own life

void enter_wounded(CActor* a, u64 t)
{
	g_me.life = Life::Wounded;
	g_hook_life.store(u8(Life::Wounded));
	if (++g_me.seq == 0xffff) // 0xffff is death::Tracker's "no death decided"
		g_me.seq = 0;
	g_me.since = t;
	g_me.bleed_end = t + g_rules.bleedout_ms;
	g_me.giveup_start = 0;
	g_me.giveup_armed = false;
	g_me.revive_target = 0xffff;
	g_hook_damage.store(0);
	close_menus();
	hold_controls(a);
	set_cam(a, 1);
	++g_stat.falls;
	feed("You are down! A teammate can revive you with a medkit (%u s).", g_rules.bleedout_ms / 1000);
	send_my_state(t);
}

void enter_dead(CActor* a, u64 t)
{
	g_me.life = Life::Dead;
	g_hook_life.store(u8(Life::Dead));
	g_me.since = t;
	g_me.giveup_start = 0;
	g_me.evac_end = 0;
	hold_controls(a);
	set_cam(a, 1);
	++g_stat.deaths;
	feed("You died.");
	send_my_state(t); // the host decides by its death mode
}

void stand_up(CActor* a, float health, bool fresh, u64 t, bool announce = true)
{
	g_me.life = Life::Alive;
	g_hook_life.store(u8(Life::Alive));
	g_hook_damage.store(0);
	g_me.since = t;
	g_me.respawn_at = 0;
	g_me.evac_end = 0;
	g_me.spectate = 0xffff;
	if (a)
	{
		CActorCondition& c = a->conditions();
		c.ClearWounds();
		if (fresh)
			c.SetRadiation(0.f);
		c.SetHealth(clampr(health, kDownHealth, c.GetMaxHealth()));
	}
	release_controls(a);
	if (announce)
		send_my_state(t);
}

// The wear of a death (coop_death_wear): the weapons, the outfit and the helmet in the owner's slots lose `pct` % of
// their condition. The owner owns the condition of its items: WP9's ItemState carries it to the host and the others.
u32 wear_worn_items(CActor* a, u8 pct)
{
	if (!pct)
		return 0;
	static const u16 kWornSlots[] = {KNIFE_SLOT, INV_SLOT_2, INV_SLOT_3, OUTFIT_SLOT, HELMET_SLOT};
	u32 worn = 0;
	for (u16 slot : kWornSlots)
	{
		if (slot > a->inventory().LastSlot())
			continue;
		PIItem it = a->inventory().ItemFromSlot(slot);
		if (!it || it->object().getDestroy() || !it->IsUsingCondition())
			continue;
		it->ChangeCondition(-float(pct) / 100.f);
		++worn;
	}
	return worn;
}

void apply_revive(u16 seq, u16 reviver, float health, PeerId reviver_peer, const char* medkit)
{
	CActor* a = local_actor();
	if (!a || g_me.life != Life::Wounded || seq != g_me.seq)
	{
		++g_stat.stale_orders;
		return;
	}
	++g_stat.revives;
	stand_up(a, health, false, now());
	feed("%s revived you.", nick_of(reviver).c_str());
	if (reviver_peer == kInvalidPeer) // a host without the W5 tail: derive it from the character id
		reviver_peer = reviver == 0 ? kHostPeer
			: (mgr().is_host() ? join::peer_of_proxy(reviver) : world::avatar_peer(join::to_local(reviver)));
	script::on_actor_revived(reviver_peer, medkit); // Lua coop_actor_revived, from the frame queue
}

// The host's Evacuate: this death costs what it costs (the host took it already), the player waits `delay_ms` for
// the respawn and watches a living teammate meanwhile (local_frame).
void apply_evacuate(u16 seq, u32 delay_ms)
{
	CActor* a = local_actor();
	if (!a || g_me.life != Life::Dead || seq != g_me.seq || g_me.evac_end)
	{
		++g_stat.stale_orders;
		return;
	}
	g_me.evac_end = now() + delay_ms;
	if (!g_me.evac_end)
		g_me.evac_end = 1; // 0 means "no Evacuate yet"
	g_me.spectate = 0xffff;
	close_menus();
	++g_stat.evacuations;
}

// The host's Respawn: back to life at `pos`, the player's checkpoint of `kind`. A Dead owner gets it when its
// evacuation is over; an Alive one when it came back to the world while dead (a rejoin, a world change) and the
// host still owes it the respawn (its seq is the one it reported alive). The items were taken once, by the host,
// when the death was decided.
void apply_respawn(u16 seq, const Fvector& pos, checkpoint::Kind kind)
{
	CActor* a = local_actor();
	if (!a || (g_me.life != Life::Dead && g_me.life != Life::Alive) || seq != g_me.seq)
	{
		++g_stat.stale_orders;
		return;
	}
	const u64 t = now();
	if (g_me.life == Life::Alive)
	{
		close_menus();
		hold_controls(a); // as every Respawning frame (local_frame); the camera stays where it is
	}
	g_me.life = Life::Respawning;
	g_hook_life.store(u8(Life::Respawning));
	g_me.since = t;
	g_me.respawn_pos = pos;
	g_me.respawn_kind = kind;
	g_me.respawn_at = t + kRespawnSettleMs;
	const u32 worn = wear_worn_items(a, g_rules.wear_pct);
	if (worn)
		::Msg("[coop] death wear: %u worn item(s) lose %u%% condition", worn, u32(g_rules.wear_pct));
	send_my_state(t);
}

void finish_respawn(CActor* a, u64 t)
{
	Fmatrix m = a->XFORM();
	m.c.set(g_me.respawn_pos);
	a->ForceTransform(m);
	++g_stat.respawns;
	const checkpoint::Kind kind = g_me.respawn_kind;
	stand_up(a, a->conditions().GetMaxHealth(), true, t);
	if (kind == checkpoint::Kind::None)
		feed("You respawned (no camp of yours on this level).");
	else
		feed("You are back at your camp (%s).", checkpoint::kind_name(kind));
	// Lua (F3 Task 15): contusion, rank; the money is the host's (death::host_take_money), Lua only tells it.
	script::queue_callback("coop_respawned", {script::cb_str(checkpoint::kind_name(kind)),
		script::cb_int(s64(g_rules.contusion_s)), script::cb_int(s64(g_rules.money_pct)),
		script::cb_int(s64(g_rules.rank_pct))});
}

void apply_spectate(u16 seq)
{
	CActor* a = local_actor();
	if (!a || seq != g_me.seq || g_me.life == Life::Spectating)
	{
		++g_stat.stale_orders;
		return;
	}
	const u64 t = now();
	g_me.life = Life::Spectating;
	g_hook_life.store(u8(Life::Spectating));
	g_me.since = t;
	g_me.spectate = 0xffff;
	g_me.newchar_start = 0; // F3 Task 10: the hold for a new character starts over (armed once USE is released)
	g_me.newchar_armed = false;
	g_me.newchar_asked = 0;
	close_menus();
	hold_controls(a);
	set_cam(a, 2);
	++g_stat.spectates;
	feed("Hardcore: your character is gone. Hold USE to start a new one.");
	send_my_state(t);
}

void apply_order(u8 op, Reader& r)
{
	++g_stat.orders_in;
	const u16 target = r.u16();
	if (op == OpRefused)
	{
		const u8 reason = r.u8();
		if (!r.ok())
			return;
		++g_stat.refused;
		feed("Revive of %s refused: %s.", nick_of(target).c_str(), refusal_text(reason));
		return;
	}
	const u16 seq = r.u16();
	if (!r.ok() || target != my_host_id())
		return;
	switch (op)
	{
	case OpRevive:
	{
		const u16 reviver = r.u16();
		const float health = r.f32();
		if (!r.ok() || !_valid(health))
			break;
		PeerId reviver_peer = kInvalidPeer;
		std::string medkit;
		if (r.remaining() > 0) // W5 tail; a malformed one does not cancel the revive
		{
			const PeerId p = r.u32();
			std::string s = r.str();
			if (r.ok() && s.size() <= 128)
			{
				reviver_peer = p;
				medkit = std::move(s);
			}
		}
		apply_revive(seq, reviver, health, reviver_peer, medkit.c_str());
		break;
	}
	case OpRespawn:
	{
		float p[3];
		r.vec3(p);
		const u8 kind = r.u8();
		if (!r.ok() || !_valid(p[0]) || !_valid(p[1]) || !_valid(p[2]))
			break;
		const bool known = kind <= u8(checkpoint::Kind::Mod); // a newer kind: still a respawn, shown as "none"
		apply_respawn(seq, Fvector().set(p[0], p[1], p[2]), known ? checkpoint::Kind(kind) : checkpoint::Kind::None);
		break;
	}
	case OpSpectate: apply_spectate(seq); break;
	case OpEvacuate:
	{
		const u32 delay = r.u32();
		if (r.ok())
			apply_evacuate(seq, _min(delay, u32(300000))); // death::Rules caps the delay at 300 s
		break;
	}
	default: break;
	}
}

// ---------------------------------------------------------------- other players

void announce(u16 id, Life from, Life to)
{
	const xr_string who = nick_of(id);
	switch (to)
	{
	case Life::Wounded: feed("%s is down! Hold USE on them with a medkit to revive.", who.c_str()); break;
	case Life::Dead: feed("%s died.", who.c_str()); break;
	case Life::Spectating: feed("%s is out of the run (hardcore).", who.c_str()); break;
	case Life::Alive:
		if (from == Life::Wounded)
			feed("%s is back on their feet.", who.c_str());
		else if (from == Life::Respawning || from == Life::Dead)
			feed("%s respawned.", who.c_str());
		break;
	default: break;
	}
}

void apply_remote_state(u16 id, Life life, u16 seq, u32 left, const char* nick)
{
	const bool known = g_players.find(id) != g_players.end();
	Remote& p = g_players[id];
	const Life old = known ? p.life : Life::Alive;
	p.life = life;
	p.seq = seq;
	p.bleed_end = life == Life::Wounded ? now() + left : 0;
	p.updated = now();
	if (nick && *nick)
		p.nick = nick;
	if (old != life)
		announce(id, old, life);
	++g_stat.states_in;
}

// ---------------------------------------------------------------- host

// Where a character stands on the ground: the position of its AI vertex when it has a valid one.
Fvector ground_point(CGameObject* o)
{
	Fvector p = o->Position();
	if (ai().get_level_graph())
	{
		const u32 v = o->ai_location().level_vertex_id();
		if (ai().level_graph().valid_vertex_id(v))
			p = ai().level_graph().vertex_position(v);
	}
	return p;
}

// The server entity that carries a character's custom data: the host's actor (0) or a player character (as
// coop_checkpoints keeps the slot); nullptr without ALife or for anything else.
CSE_Abstract* character_entity(u16 character)
{
	if (!ai().get_alife())
		return nullptr;
	if (character == 0)
		return ai().alife().graph().actor();
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(character, true);
	return e && world::is_player_character(e) ? e : nullptr;
}

// Fix round 1 (M-1 / M-3): the respawn debt lives with the character in the world, so it reaches exactly the worlds
// saved while he owed it (a level change, an autosave) and no new game, no other save, no other entity under his id.
// F3 Task 10 (T8 review): so does "out of a hardcore run" (kOutKey, death::learn_out). [coop_death] <key> = 1.
bool flag_read(u16 character, const char* key)
{
	const CSE_Abstract* e = character_entity(character);
	if (!e || !e->m_ini_string.size())
		return false;
	std::string v;
	return checkpoint::ini_get(e->m_ini_string.c_str(), kDebtSection, key, v) && v == "1";
}

void flag_write(u16 character, const char* key, bool on)
{
	CSE_Abstract* e = character_entity(character);
	if (!e)
	{
		if (on)
			::Msg("! [coop] [%s] %s of %u not kept: no character entity (a world change will not bring it back)",
			    kDebtSection, key, character);
		return;
	}
	std::string text = e->m_ini_string.size() ? e->m_ini_string.c_str() : "";
	const std::string before = text;
	if (on)
		checkpoint::ini_set(text, kDebtSection, key, "1");
	else
		checkpoint::ini_erase(text, kDebtSection, key);
	if (text != before)
		e->m_ini_string = text.c_str();
}

// F3 Task 8 step 3: after a decision, the lives of everybody present in the host's world (the host, the players that
// report, without the parked characters of players who left). Every one of them down for good: coop_group_wipe, once
// until somebody stands up again (host_on_state Alive).
void host_check_wipe()
{
	std::vector<u8> lives;
	lives.push_back(u8(g_me.life));
	for (const auto& it : g_players)
		if (!world::is_parked(it.first))
			lives.push_back(u8(it.second.life));
	if (g_wipe_fired || !death::is_wipe(lives))
		return;
	g_wipe_fired = true;
	++g_stat.wipes;
	feed("%s", tr("st_coop_evac_no_allies", "Nobody is left alive. The world is not rolled back.").c_str());
	script::queue_callback("coop_group_wipe", {});
}

// The respawn of `id` at his own checkpoint (coop_checkpoints::host_respawn_point: his camp, the previous one, the
// anchor, the host's arrival point, else the body). `seq` is the fall the owner is at: the death after an evacuation,
// or the seq it reported alive with when it came back. False when the character has no object here to start from.
bool host_order_respawn(u16 id, u16 seq)
{
	CGameObject* body = object_here(id);
	if (!body)
		return false;
	HostRec& h = g_host[id];
	h.ordered_seq = seq; // before the order: the host's own Respawning comes back inside host_send_order and clears it
	Fvector pos;
	checkpoint::Kind kind = checkpoint::Kind::None;
	if (!checkpoints::host_respawn_point(id, h.has_anchor ? &h.anchor : nullptr, body->Position(), pos, kind))
		::Msg("! [coop] respawn of %s: no checkpoint store in this world, back where the body lies", nick_of(id).c_str());
	pos.y += kRespawnLift;
	::Msg("[coop] respawn of %s (fall #%u): %s at (%.1f, %.1f, %.1f)", nick_of(id).c_str(), seq,
	    checkpoint::kind_name(kind), pos.x, pos.y, pos.z);
	Writer w(24);
	w.u8(OpRespawn);
	w.u16(id);
	w.u16(seq);
	const float p[3] = {pos.x, pos.y, pos.z};
	w.vec3(p);
	w.u8(u8(kind));
	host_send_order(id, w);
	return true;
}

// The host's answer to a death (death::Tracker: once per death). Checkpoint: the items (coop_death, by
// coop_death_drop) and the money go now, the owner is evacuated for coop_respawn_delay, host_frame respawns him.
// Hardcore: he is out and watches. The host's own actor (0) takes the same path: host_send_order applies to itself.
death::Action host_on_died(u16 id, u16 seq)
{
	HostRec& h = g_host[id];
	const death::Action a = death::on_died(h.tracker, seq, now(), g_rules);
	switch (a)
	{
	case death::Action::None: return a; // decided already (state repeats are normal)
	case death::Action::Spectate: host_order_spectate(id, seq); break;
	case death::Action::LossesAndEvacuate:
	{
		const u32 items = death::host_apply_losses(id, seq, g_rules.drop);
		const u32 money = death::host_take_money(id, g_rules.money_pct);
		::Msg("[coop] death of %s (fall #%u): %u item(s) (%s), %u RU; respawn in %u s", nick_of(id).c_str(), seq, items,
		    drop_name(g_rules.drop), money, g_rules.delay_ms / 1000);
		host_order_evacuate(id, seq, g_rules.delay_ms);
		break;
	}
	}
	host_check_wipe();
	return a;
}

// Every life report of a character's owner (the host's own too). The respawn debt follows the report
// (death::debt_after_report) and is kept in the world, and so is "out of a hardcore run" (F3 Task 10); an Alive report
// is answered by death::on_alive_report: a respawn owed after a leave or a world change (Review Focus 1 / 2), a hardcore
// spectator sent back to watching (Ruling F3-7) until he starts a new character.
void host_on_state(u16 id, Life life, u16 seq)
{
	HostRec& h = g_host[id];
	// F3 Task 10 (T8 review): the world's word on "out" first; a death of an out character then costs nothing again
	const bool out_before = flag_read(id, kOutKey);
	death::learn_out(h.tracker, out_before);
	const death::Action decided = life == Life::Dead ? host_on_died(id, seq) : death::Action::None;
	if (h.tracker.hardcore_out != out_before)
		flag_write(id, kOutKey, h.tracker.hardcore_out);
	const bool before = flag_read(id, kDebtKey);
	const bool owed = death::debt_after_report(before, u8(life), decided);
	if (owed != before)
		flag_write(id, kDebtKey, owed);
	h.debt = owed;
	if (life != Life::Alive)
	{
		h.ordered_seq = death::kNoSeq; // a new fall, or the order arrived (Respawning)
		return;
	}
	h.revive_pending_until = 0;
	g_wipe_fired = false; // somebody stands: the next wipe is a new one
	if (!h.has_anchor)
		if (CGameObject* o = object_here(id))
		{
			h.anchor = ground_point(o);
			h.has_anchor = true;
		}
	h.has_worn = false;
	switch (death::on_alive_report(h.tracker, owed, h.ordered_seq == seq))
	{
	case death::AliveStep::Spectate: host_order_spectate(id, seq); break; // still out (rejoined, or the mode changed)
	case death::AliveStep::Respawn:
		if (!host_order_respawn(id, seq))
			::Msg("[coop] respawn of %s waits: no body here yet (the next report tries again)", nick_of(id).c_str());
		break;
	case death::AliveStep::None: break;
	}
}

void host_on_revive_request(u16 reviver, PeerId reply_to, u16 target, u16 seq, u16 item)
{
	++g_stat.requests_in;
	const u64 t = now();
	HostRec& h = g_host[target];
	CGameObject* r_obj = object_here(reviver);
	CGameObject* t_obj = object_here(target);
	CGameObject* i_obj = object_here(item);
	u8 reason = RefNone;
	if (reviver == target)
		reason = RefSelf;
	else if (life_by_host_id(target) != Life::Wounded || seq_by_host_id(target) != seq)
		reason = RefNotWounded;
	else if (is_down(life_by_host_id(reviver)))
		reason = RefYouAreDown;
	else if (t < h.revive_pending_until)
		reason = RefBusy;
	else if (!r_obj || !t_obj || r_obj->Position().distance_to(t_obj->Position()) > kHostReviveRange)
		reason = RefTooFar;
	else if (!i_obj || !i_obj->H_Parent() || i_obj->H_Parent()->ID() != reviver || !is_revive_section(i_obj->cNameSect()))
		reason = RefNoMedkit;

	if (reason != RefNone)
	{
		::Msg("[coop] revive of %u by %u refused: %s", target, reviver, refusal_text(reason));
		Writer w(8);
		w.u8(OpRefused);
		w.u16(target);
		w.u8(reason);
		if (reply_to == kHostPeer)
		{
			Reader r(w.ptr(), w.size());
			const u8 op = r.u8();
			apply_order(op, r);
		}
		else
			net::host_send(reply_to, Msg::PlayerRevive, w);
		return;
	}

	i_obj->DestroyObject(); // the medkit is a world item: only the host removes it, clients follow (P7)
	h.revive_pending_until = t + kRevivePendingMs;
	::Msg("[coop] %s revives %s with %s", nick_of(reviver).c_str(), nick_of(target).c_str(), i_obj->cNameSect().c_str());
	// W5 tail (readers without it still work): u32 reviver peer, str medkit section -> Lua coop_actor_revived.
	Writer w(48);
	w.u8(OpRevive);
	w.u16(target);
	w.u16(seq);
	w.u16(reviver);
	w.f32(g_rules.revive_health);
	w.u32(reviver == 0 ? kHostPeer : join::peer_of_proxy(reviver));
	w.str(i_obj->cNameSect().c_str());
	host_send_order(target, w);
}

// ---------------------------------------------------------------- hardcore: a new character (F3 Task 10)

// Where the host's new character stands up: his arrival point on this level (coop_checkpoints keeps it in the actor's
// custom data, F3 Task 6), else where a respawn would put him (his camp, the previous one, where he stands).
Fvector host_new_place(CActor* a, const char*& where)
{
	const CSE_Abstract* e = character_entity(0);
	checkpoint::Record r;
	if (e && e->m_ini_string.size() && checkpoint::arrival_from_ini(std::string(e->m_ini_string.c_str()), r) &&
	    r.valid() && r.level == Level().name().c_str())
	{
		where = "his arrival point";
		return Fvector().set(r.pos[0], r.pos[1], r.pos[2]);
	}
	Fvector pos = a->Position();
	checkpoint::Kind kind = checkpoint::Kind::None;
	checkpoints::host_respawn_point(0, nullptr, a->Position(), pos, kind);
	where = kind == checkpoint::Kind::None ? "where he stands (no arrival point on this level)" : checkpoint::kind_name(kind);
	return pos;
}

// Step 4, the host's own new character, his things in the stash now (host_retire_frame): the actor stands up at his
// arrival point as a fresh one - health 1, no radiation, no bleeding, fed, rested, 0 RU - out of the run no more, and the
// host's Lua (coop_menu_char) lets him choose faction and kit: coop_host_new_character(). He is his own authority.
void host_finish_own(u64 t)
{
	CActor* a = local_actor();
	if (!a)
		return;
	HostRec& h = g_host[0];
	h.tracker = death::Tracker(); // a new character: not out, no death decided, nothing owed
	h.ordered_seq = death::kNoSeq;
	h.has_worn = false;
	h.has_anchor = false; // the anchor is where this character is first seen alive
	h.debt = false;
	flag_write(0, kOutKey, false); // before stand_up: its Alive report reads the world's word (learn_out)
	flag_write(0, kDebtKey, false);
	const char* where = "";
	Fvector pos = host_new_place(a, where);
	pos.y += kRespawnLift;
	Fmatrix m = a->XFORM();
	m.c.set(pos);
	a->ForceTransform(m);
	CActorCondition& c = a->conditions();
	c.SetSatiety(1.f);
	c.SetPower(1.f);
	if (!money::host_set(kHostPeer, 0, "new character"))
		::Msg("! [coop] new character of the host: his money is not set to 0 (no balance to write)");
	stand_up(a, 1.f, true, t); // health 1 (up to his maximum), radiation 0, bleeding gone, controls back; Alive goes out
	::Msg("[coop] the host starts a new character at %s (%.1f, %.1f, %.1f)", where, pos.x, pos.y, pos.z);
	feed("A new character. Choose faction and gear.");
	script::queue_callback("coop_host_new_character", {});
}

// Step 2 for a client's character, his stash done (host_retire_frame): the key goes from his custom data and
// `retired = 1` comes in (death::retire_custom_data), his player joins the world again - with no character for his key
// he chooses a new one (protocol 8: CharOffer, coop_menu_char, create_proxy) - and the old character leaves the world.
void host_finish_client(u16 character)
{
	if (CSE_Abstract* e = character_entity(character))
	{
		std::string text = e->m_ini_string.size() ? e->m_ini_string.c_str() : "";
		size_t at = 0, len = 0;
		if (find_key_value(text, at, len))
			join::host_key_retired(text.substr(at, len)); // Ruling F3-9: his new character is not a new key
		death::retire_custom_data(text);
		e->m_ini_string = text.c_str();
	}
	const PeerId peer = join::peer_of_proxy(character);
	if (peer == kInvalidPeer)
		::Msg("[coop] retired character %u: his player is away, his next join makes a new character", character);
	else if (!join::host_rejoin_peer(peer, "new character"))
	{
		// cannot happen (a peer of a proxy is a connected player of this world); his machine still plays this
		// character, so it stays in the world, keyless: his next join makes a new one
		::Msg("! [coop] retired character %u: his player #%u could not be sent the world again, kept", character, peer);
		g_host.erase(character);
		return;
	}
	death::host_release_character(character);
	world::host_forget_proxy(character); // the id is nobody's character now: ALife may give it to anything
	g_host.erase(character);
	g_players.erase(character);
}

// Review R2 of T9 (death::retire_step): a retiring character's key, his player and his release wait until his stash
// is no longer pending (death::host_stash_pending). The stash knows its owner by the key, or by the player while the
// key is off; changing either before it is filled leaves it without an owner, the things stay on the old character and
// go with him. Only then step 2 (a client's character) or step 4 (the host's own) goes on.
void host_retire_frame(u64 t)
{
	for (auto it = g_retire.begin(); it != g_retire.end();)
	{
		const u16 character = it->first;
		const bool here = character == 0 ? local_actor() != nullptr : character_entity(character) != nullptr;
		switch (death::retire_step(here, death::host_stash_pending(character)))
		{
		case death::RetireStep::Wait: ++it; continue;
		case death::RetireStep::Void:
			::Msg("! [coop] retirement of character %u dropped: he is gone", character);
			break;
		case death::RetireStep::Finish:
			if (character == 0)
				host_finish_own(t);
			else
				host_finish_client(character);
			++g_stat.retired;
			break;
		}
		it = g_retire.erase(it);
	}
}

// Steps 2 and 4: the owner of `character` (0: the host himself) asks for a new character. The tracker, with the world's
// word (learn_out), must say he is out of the hardcore run; then everything he has - worn and quest items included - goes
// into a stash at the body (DeathMarker when it is in), and the rest waits for it (host_retire_frame). He must be in the
// world: a character that is not online here has nothing moved, and his release would take his things with him. Ruling
// F3-9: coop_max_new_characters is not checked - his key had a character (join::host_key_retired).
void host_new_character(u16 character)
{
	HostRec& h = g_host[character];
	death::learn_out(h.tracker, flag_read(character, kOutKey));
	const xr_string who = nick_of(character);
	const char* refused = nullptr;
	switch (death::on_new_character(h.tracker, g_retire.find(character) != g_retire.end()))
	{
	case death::NewCharVerdict::Busy:
		::Msg("[coop] new character of %s: under way already", who.c_str());
		return;
	case death::NewCharVerdict::NotOut: refused = "he is not out of a hardcore run"; break;
	case death::NewCharVerdict::Start:
		if (character == 0 ? !local_actor() : !object_here(character))
			refused = "his character is not in the world here";
		break;
	}
	if (refused)
	{
		++g_stat.newchar_refused;
		::Msg("! [coop] new character of %s (character %u) refused: %s", who.c_str(), character, refused);
		return;
	}
	Retirement r;
	r.since = now();
	r.stash = death::host_move_to_stash(character, true); // everything, worn and quest items included
	g_retire[character] = r;
	if (r.stash != 0xffff)
		::Msg("[coop] %s starts a new character: his things go into stash %u first", who.c_str(), r.stash);
	else
		::Msg("[coop] %s starts a new character (no stash: nothing to keep, or it dropped at the body)", who.c_str());
}

void host_broadcast_rules()
{
	Writer w(16);
	g_rules.write(w);
	net::host_broadcast(Msg::PlayerRules, w);
}

void host_frame(u64 t)
{
	const u32 epoch = join::host_world_epoch();
	if (epoch != g_epoch)
	{
		if (g_epoch != 0xffffffff)
			g_me.actor = nullptr; // the new actor may reuse the old one's address: frame() must still start it over
		// A load / level change replaced the world: deaths and anchors start over (the new actors count their falls
		// from 0). Review Focus 2: whoever was dead or evacuated when this world was saved carries his respawn debt in
		// it (flag_read) and is put on his checkpoint at his first Alive report here; the losses were taken already.
		// F3 Task 10: whoever was out of a hardcore run carries that too (kOutKey) and watches again. A retirement of
		// the old world is void (its stash is released empty, death::abandon_stashes): he may ask again here.
		g_epoch = epoch;
		g_host.clear();
		if (!g_retire.empty())
			::Msg("[coop] a new world: %u retirement(s) of the old one dropped", u32(g_retire.size()));
		g_retire.clear();
		g_wipe_fired = false;
	}

	const death::Rules r = console_rules();
	if (!(r == g_rules))
	{
		const bool mode_changed = r.mode != g_rules.mode;
		g_rules = r;
		host_broadcast_rules();
		if (mode_changed)
			feed("Death mode: %s.", mode_name(r.mode));
	}

	// players that left: their characters are parked. Dead or evacuated: the countdown stops and the respawn waits for
	// their return (death::on_left, Review Focus 1); dead with the respawn order already out but not applied: the debt
	// in the world brings it on the return. An order of the fall he left at is void.
	for (auto it = g_players.begin(); it != g_players.end();)
	{
		if (world::is_parked(it->first) || join::peer_of_proxy(it->first) == kInvalidPeer)
		{
			HostRec& h = g_host[it->first];
			death::on_left(h.tracker); // hardcore_out stays: leaving does not bring a dead player back
			h.ordered_seq = death::kNoSeq;
			h.revive_pending_until = 0;
			it = g_players.erase(it);
		}
		else
			++it;
	}

	host_retire_frame(t); // F3 Task 10: retirements whose stash is done

	// evacuations that are over (orders after the walk: the host's own respawn re-enters host_on_state)
	xr_vector<u16> due;
	for (auto& it : g_host)
		if (death::due(it.second.tracker, t))
			due.push_back(it.first);
	for (u16 id : due)
	{
		HostRec& h = g_host[id];
		if (!host_order_respawn(id, h.tracker.decided_seq))
			h.tracker.respawn_at = t + kRespawnRetryMs; // no body here right now: still waiting (on_left sees it)
	}

	if (t - g_last_roster >= kStateRefreshMs)
	{
		g_last_roster = t;
		host_broadcast_rules();
		for (const auto& it : g_players)
		{
			const u32 left = it.second.life == Life::Wounded && it.second.bleed_end > t ? u32(it.second.bleed_end - t) : 0;
			Writer w(32);
			write_state(w, it.first, it.second.life, it.second.seq, left, nick_of(it.first).c_str());
			net::host_broadcast(Msg::PlayerWounded, w);
		}
	}
}

// ---------------------------------------------------------------- handlers

// Host, Ruling F3-4: the slot / belt tail of a client's Dead report (death::read_worn: at most kMaxWornIds, a short
// packet or a longer count drops the list whole). Of the ids only items the proxy carries right now stay (each once,
// sorted). Kept per death: the losses of that death read it (host_worn_items).
void host_read_worn(u16 proxy, u16 seq, Reader& r)
{
	if (r.remaining() == 0)
		return; // the report without the tail: the layout stays unknown
	HostRec& h = g_host[proxy];
	if (h.has_worn && h.worn_seq == seq)
		return; // the 5 s repeat of the same death: the first list counts
	std::vector<u16> ids;
	if (death::read_worn(r, ids) != death::WornTail::Ok)
	{
		++g_stat.worn_refused;
		return;
	}
	ids.erase(std::remove_if(ids.begin(), ids.end(), [proxy](u16 id)
	{
		CGameObject* o = object_here(id);
		return !o || !smart_cast<CInventoryItem*>(o) || !o->H_Parent() || o->H_Parent()->ID() != proxy;
	}), ids.end());
	h.has_worn = true;
	h.worn_seq = seq;
	h.worn.assign(ids.begin(), ids.end());
	++g_stat.worn_reports;
}

void on_state_msg(PeerId from, Reader& r)
{
	u16 id = r.u16();
	const u8 life = r.u8();
	const u16 seq = r.u16();
	const u32 left = r.u32();
	const std::string nick = r.str();
	if (!r.ok() || life > u8(Life::Respawning))
		return;
	if (mgr().is_host())
	{
		id = join::proxy_of_peer(from); // never trust the id a client claims
		if (id == 0xffff)
			return;
		const PeerInfo* p = mgr().session().peer(from);
		const xr_string name = p ? p->nick.c_str() : nick.c_str();
		if (Life(life) == Life::Dead)
			host_read_worn(id, seq, r); // before the decision: the losses of this death read it
		apply_remote_state(id, Life(life), seq, left, name.c_str());
		host_on_state(id, Life(life), seq);
		Writer w(32);
		write_state(w, id, Life(life), seq, left, name.c_str());
		net::host_broadcast(Msg::PlayerWounded, w, Lane::Reliable, from);
		return;
	}
	if (!mgr().is_live_client() || from != kHostPeer || id == my_host_id())
		return; // our own state is ours
	apply_remote_state(id, Life(life), seq, left, nick.c_str());
}

void on_revive_msg(PeerId from, Reader& r)
{
	const u8 op = r.u8();
	if (!r.ok())
		return;
	if (mgr().is_host())
	{
		if (op != OpRequest)
			return; // orders come only from the host
		const u16 reviver = join::proxy_of_peer(from);
		const u16 target = r.u16();
		const u16 seq = r.u16();
		const u16 item = r.u16();
		if (!r.ok() || reviver == 0xffff)
			return;
		host_on_revive_request(reviver, from, target, seq, item);
		return;
	}
	if (mgr().is_live_client() && from == kHostPeer && op != OpRequest)
		apply_order(op, r);
}

// Host, F3 Task 10 step 2: Msg::NewCharacter (no payload; bytes after are a newer client's) from a client out of a
// hardcore run. His character is the one of his peer, never an id he names.
void on_new_character_msg(PeerId from, Reader&)
{
	if (!mgr().is_host())
		return; // ClientOnly: the session refuses it from the host before any handler
	const u16 proxy = join::proxy_of_peer(from);
	if (proxy == 0xffff)
	{
		++g_stat.newchar_refused;
		::Msg("! [coop] NewCharacter from #%u refused: no character of his in this world", from);
		return;
	}
	host_new_character(proxy);
}

void on_rules_msg(PeerId from, Reader& r)
{
	if (mgr().is_host() || from != kHostPeer)
		return;
	death::Rules rules = g_rules;
	if (!rules.read(r))
		return;
	if (!g_rules_from_host || !(rules == g_rules))
		::Msg("[coop] host survival rules: death mode %s, drop %s, bleed-out %u s, revive health %.2f, respawn delay "
		      "%u s, contusion %u s, money %u%%, wear %u%%, rank %u%%",
		    mode_name(rules.mode), drop_name(rules.drop), rules.bleedout_ms / 1000, rules.revive_health,
		    rules.delay_ms / 1000, rules.contusion_s, u32(rules.money_pct), u32(rules.wear_pct), u32(rules.rank_pct));
	g_rules = rules;
	g_rules_from_host = true;
}

// ---------------------------------------------------------------- per-frame parts

void send_revive_request(u16 target_host, u16 seq, u16 item)
{
	if (mgr().is_host())
	{
		host_on_revive_request(0, kHostPeer, target_host, seq, item);
		return;
	}
	Writer w(8);
	w.u8(OpRequest);
	w.u16(target_host);
	w.u16(seq);
	w.u16(item);
	net::client_send(Msg::PlayerRevive, w);
}

void reviver_frame(CActor* a, u64 t)
{
	if (g_me.revive_target == 0xffff)
		return;
	const u16 target = g_me.revive_target;
	CGameObject* o = object_here(target);
	const char* cancel = nullptr;
	if (!a || is_down(g_me.life) || !o)
		cancel = "";
	else if (life_of(target) != Life::Wounded || seq_by_host_id(join::to_host(target)) != g_me.revive_seq)
		cancel = "";
	else if (o->Position().distance_to(a->Position()) > kReviveRange)
		cancel = "too far";
	else if (!action_down(kUSE))
		cancel = "released";
	if (cancel)
	{
		if (*cancel)
			feed("Revive cancelled (%s).", cancel);
		g_me.revive_target = 0xffff;
		return;
	}
	if (t - g_me.revive_start < kReviveHoldMs)
		return;
	g_me.revive_target = 0xffff;
	send_revive_request(join::to_host(target), g_me.revive_seq, g_me.revive_item);
}

void spectate_frame(u64)
{
	xr_vector<PlayerRef> players;
	collect_players(players, false);
	xr_vector<u16> watchable;
	for (const PlayerRef& p : players)
	{
		const Life l = life_by_host_id(p.host_id);
		if ((l == Life::Alive || l == Life::Wounded || l == Life::Respawning) && object_here(p.local_id))
			watchable.push_back(p.local_id);
	}
	if (watchable.empty())
	{
		g_me.spectate = 0xffff;
		g_spec.valid = false;
		return;
	}
	auto cur = std::find(watchable.begin(), watchable.end(), g_me.spectate);
	const bool next = action_down(kWPN_FIRE), prev = action_down(kWPN_ZOOM);
	size_t idx = cur == watchable.end() ? 0 : size_t(cur - watchable.begin());
	if (next && !g_me.next_was_down)
		idx = (idx + 1) % watchable.size();
	else if (prev && !g_me.prev_was_down)
		idx = (idx + watchable.size() - 1) % watchable.size();
	g_me.next_was_down = next;
	g_me.prev_was_down = prev;
	g_me.spectate = watchable[idx];

	CGameObject* o = object_here(g_me.spectate);
	Fvector head = o->Position();
	head.y += 1.6f;
	Fvector fwd = o->XFORM().k;
	fwd.y = 0.f;
	if (fwd.square_magnitude() < EPS_L)
		fwd.set(0.f, 0.f, 1.f);
	fwd.normalize();
	Fvector back;
	back.set(-fwd.x, 0.35f, -fwd.z);
	back.normalize();
	float dist = 3.2f;
	collide::rq_result R;
	if (Level().ObjectSpace.RayPick(head, back, dist, collide::rqtStatic, R, nullptr))
		dist = _max(0.4f, R.range - 0.3f);
	g_spec.pos.mad(head, back, dist);
	g_spec.look = head;
	g_spec.valid = true;
}

// Evacuated with nobody to watch: the overhead camera's target, the own body, and how high it may go (a ceiling).
void overhead_frame(CActor* a)
{
	Fvector from = a->Position();
	from.y += 0.5f;
	float dist = kOverheadHeight;
	collide::rq_result R;
	if (Level().ObjectSpace.RayPick(from, Fvector().set(0.f, 1.f, 0.f), dist, collide::rqtStatic, R, nullptr))
		dist = _max(1.f, R.range - 0.3f);
	g_over.body = a->Position();
	g_over.height = _min(kOverheadHeight, dist + 0.5f);
	g_over.valid = true;
}

// F3 Task 10, step 1: out of a hardcore run, ask for a new character (the hold of USE, console coop_new_character). A
// client asks the host (Msg::NewCharacter, no payload); the host starts its own retirement at once. The answer is the
// world sent again (a client) or standing up (the host); a refusal is only in the host's log, so after kNewCharRetryMs
// the prompt comes back. False while a request is that fresh.
bool request_new_character(u64 t)
{
	if (g_me.life != Life::Spectating || (g_me.newchar_asked && t - g_me.newchar_asked < kNewCharRetryMs))
		return false;
	g_me.newchar_asked = t;
	g_me.newchar_start = 0;
	g_me.newchar_armed = false;
	++g_stat.newchar_asked;
	::Msg("[coop] asking for a new character");
	if (mgr().is_host())
		host_new_character(0);
	else
		net::client_send(Msg::NewCharacter, Writer());
	return true;
}

// Spectating: hold USE kNewCharHoldMs (as kGiveUpHoldMs), armed once USE was seen released after the order.
void newchar_frame(u64 t)
{
	if (g_me.newchar_asked)
	{
		if (t - g_me.newchar_asked < kNewCharRetryMs)
			return;
		g_me.newchar_asked = 0; // no answer: the host refused (its log says why), or it is still at it
	}
	if (!action_down(kUSE))
	{
		g_me.newchar_armed = true;
		g_me.newchar_start = 0;
		return;
	}
	if (!g_me.newchar_armed)
		return;
	if (!g_me.newchar_start)
		g_me.newchar_start = t;
	else if (t - g_me.newchar_start >= kNewCharHoldMs)
		request_new_character(t);
}

// Dead and evacuated: watch a living teammate (Fire / Aim switch), nobody alive: look down at the body.
void evacuated_frame(CActor* a, u64 t)
{
	spectate_frame(t);
	const u8 cam = g_spec.valid ? 2 : 3;
	if (g_me.cam != cam)
	{
		set_cam(a, cam);
		if (cam == 2)
			spectate_frame(t); // set_cam starts the spectator camera without a target
	}
	if (cam == 3)
		overhead_frame(a);
	else
		g_over.valid = false;
}

void local_frame(CActor* a, u64 t)
{
	if (g_hook_fell.exchange(false) && g_me.life == Life::Alive)
		enter_wounded(a, t);

	const u32 damage = g_hook_damage.exchange(0);
	if (damage && g_me.life == Life::Wounded)
	{
		const u64 cut = u64(damage) * kDamageMsPerHealth / 1000;
		g_me.bleed_end = g_me.bleed_end > t + cut ? g_me.bleed_end - cut : t;
	}

	switch (g_me.life)
	{
	case Life::Alive: break;
	case Life::Wounded:
		hold_controls(a);
		if (!action_down(kUSE))
			g_me.giveup_armed = true;
		if (g_me.giveup_armed && action_down(kUSE))
		{
			if (!g_me.giveup_start)
				g_me.giveup_start = t;
			else if (t - g_me.giveup_start >= kGiveUpHoldMs)
				g_me.bleed_end = t;
		}
		else
			g_me.giveup_start = 0;
		if (t >= g_me.bleed_end)
			enter_dead(a, t);
		break;
	case Life::Dead:
		hold_controls(a);
		if (g_me.evac_end)
			evacuated_frame(a, t);
		break;
	case Life::Spectating:
		hold_controls(a);
		spectate_frame(t);
		newchar_frame(t);
		break;
	case Life::Respawning:
		hold_controls(a);
		if (t >= g_me.respawn_at)
			finish_respawn(a, t);
		break;
	}

	if (t - g_me.last_sent >= kStateRefreshMs)
		send_my_state(t);
}

bool blend_playing(IKinematicsAnimated* ka, MotionID m)
{
	for (u32 part = 0; part < 4; ++part)
		for (u32 i = 0, n = ka->LL_PartBlendsCount(part); i < n; ++i)
			if (CBlend* b = ka->LL_PartBlend(part, i))
				if (b->motionID == m)
					return true;
	return false;
}

MotionID find_pose(IKinematicsAnimated* ka, Life life)
{
	if (life == Life::Wounded)
	{
		for (const char* name : kWoundedPoses)
		{
			const MotionID m = ka->ID_Cycle_Safe(name);
			if (m.valid())
				return m;
		}
	}
	else
	{
		for (const char* name : kDeadPoses)
		{
			const MotionID m = ka->ID_Cycle_Safe(name);
			if (m.valid())
				return m;
		}
	}
	return MotionID();
}

// Players' characters must never lose health on this machine: each owner computes its own damage
// (proxies on the host and avatars on clients are puppets; WP1 makes them unharmable, this keeps it so).
void guard_frame(u64 t)
{
	if (t - g_last_guard < kGuardMs)
		return;
	g_last_guard = t;
	xr_vector<PlayerRef> players;
	collect_players(players, false);
	for (const PlayerRef& p : players)
	{
		CEntityAlive* e = smart_cast<CEntityAlive*>(object_here(p.local_id));
		if (!e)
			continue;
		if (e->conditions().CanBeHarmed())
		{
			e->conditions().SetCanBeHarmedState(false);
			++g_stat.guard_fixes;
		}
		if (!e->g_Alive() && g_guard_logged.insert(p.local_id).second)
			::Msg("! [coop] character of player '%s' (%u) is dead on this machine: it must never die here",
			    p.nick.c_str(), p.host_id);
	}
}

void build_hud(u64 t)
{
	g_hud.clear();
	auto line = [](const char* text, u32 color, bool big)
	{
		HudLine l;
		l.text = text;
		l.color = color;
		l.big = big;
		g_hud.push_back(l);
	};
	string256 buf;
	switch (g_me.life)
	{
	case Life::Wounded:
	{
		line("YOU ARE DOWN", 0xffe03c3c, true);
		const u32 left = g_me.bleed_end > t ? u32((g_me.bleed_end - t + 999) / 1000) : 0;
		xr_sprintf(buf, "Bleeding out: %u s. A teammate can revive you with a medkit.", left);
		line(buf, 0xfff0f0f0, false);
		if (g_me.giveup_start)
		{
			xr_sprintf(buf, "Giving up... %u%%", u32(_min<u64>(100, (t - g_me.giveup_start) * 100 / kGiveUpHoldMs)));
			line(buf, 0xffffa040, false);
		}
		else
			line("Hold USE to give up.", 0xffa0a0a0, false);
		break;
	}
	case Life::Dead:
		if (!g_me.evac_end)
		{
			line("YOU DIED", 0xffe03c3c, true);
			line("Waiting for the host...", 0xfff0f0f0, false);
			break;
		}
		{
			line(tr("st_coop_evac_title", "You were pulled out...").c_str(), 0xffe03c3c, true);
			const int left = g_me.evac_end > t ? int((g_me.evac_end - t + 999) / 1000) : 0;
			xr_sprintf(buf, tr_format("st_coop_evac_wait", "Back at your camp in %d s", 'd').c_str(), left);
			line(buf, 0xfff0f0f0, false);
			if (g_spec.valid && g_me.spectate != 0xffff)
			{
				xr_sprintf(buf, tr_format("st_coop_evac_watch", "Watching: %s", 's').c_str(),
				    nick_of(join::to_host(g_me.spectate)).c_str());
				line(buf, 0xfff0f0f0, false);
				line("Fire: next player   Aim: previous player", 0xffa0a0a0, false);
			}
			else
				line(tr("st_coop_evac_no_allies", "Nobody is left alive. The world is not rolled back.").c_str(),
				    0xffa0a0a0, false);
		}
		break;
	case Life::Respawning: line("Respawning...", 0xfff0f0f0, true); break;
	case Life::Spectating: // out of a hardcore run: watch the others, or start a new character (F3 Task 10)
	{
		line(tr("st_coop_hardcore_out", "Your character is gone.").c_str(), 0xffe03c3c, true);
		if (g_me.spectate != 0xffff)
		{
			xr_sprintf(buf, tr_format("st_coop_evac_watch", "Watching: %s", 's').c_str(),
			    nick_of(join::to_host(g_me.spectate)).c_str());
			line(buf, 0xfff0f0f0, false);
			line("Fire: next player   Aim: previous player", 0xffa0a0a0, false);
		}
		else
			line(tr("st_coop_evac_no_allies", "Nobody is left alive. The world is not rolled back.").c_str(),
			    0xffa0a0a0, false);
		if (g_me.newchar_asked)
			line("Starting a new character...", 0xffffa040, false);
		else if (g_me.newchar_start)
		{
			xr_sprintf(buf, "New character... %u%%", u32(_min<u64>(100, (t - g_me.newchar_start) * 100 / kNewCharHoldMs)));
			line(buf, 0xffffa040, false);
		}
		else
			line(tr("st_coop_hardcore_new_hold", "Hold USE for 3 s to start a new character").c_str(), 0xfff0f0f0, false);
		break;
	}
	default: break;
	}
	if (g_me.revive_target != 0xffff)
	{
		xr_sprintf(buf, "Reviving %s... %u%%", nick_of(join::to_host(g_me.revive_target)).c_str(),
		    u32(_min<u64>(100, (t - g_me.revive_start) * 100 / kReviveHoldMs)));
		line(buf, 0xff80ff80, false);
	}
	while (!g_feed.empty() && g_feed.front().until <= t)
		g_feed.pop_front();
}

void forget_local(CActor* next = nullptr)
{
	// The old actor is gone (level change, load): never call into it.
	g_input.reinit();
	g_me = Local();
	if (!next || g_hook_owner.load() != &next->conditions())
	{
		g_hook_life.store(u8(Life::Alive));
		g_hook_owner.store(nullptr);
		g_hook_fell.store(false);
	}
	else
		g_hook_life.store(u8(Life::Alive)); // the new actor fell before we saw it: frame() makes it wounded
	g_hook_damage.store(0);
	g_spec.valid = false;
	g_over.valid = false;
	g_poses.clear();
	g_players.clear();
}

void leave_session()
{
	CActor* a = local_actor();
	if (a && a == g_me.actor && is_down(g_me.life))
		stand_up(a, kDownHealth * 5.f, false, now(), false); // session over: back on its feet, barely alive
	else if (a && a == g_me.actor)
		release_controls(a);
	forget_local();
	g_host.clear(); // the respawn debts stay in the world (custom data), not here
	g_retire.clear(); // a stash still waiting is released empty (death::reset); the character stays out
	g_wipe_fired = false;
	g_rules_from_host = false;
	g_hud.clear();
}

// F3 Task 10: the hold of USE from the console (out of a hardcore run).
class CCC_CoopNewCharacter : public IConsole_Command
{
public:
	CCC_CoopNewCharacter(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		if (!local_actor() || !live())
		{
			::Msg("! [coop] coop_new_character: only in a co-op session, in a level");
			return;
		}
		if (g_me.life != Life::Spectating)
		{
			::Msg("! [coop] coop_new_character: only out of a hardcore run (spectating), now %s", life_name(g_me.life));
			return;
		}
		if (!request_new_character(now()))
			::Msg("[coop] coop_new_character: asked %llu s ago, waiting for the host", (now() - g_me.newchar_asked) / 1000);
	}
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "hardcore, out of the run: start a new character (as holding USE for 3 s)");
	}
};

class CCC_CoopWoundMe : public IConsole_Command
{
public:
	CCC_CoopWoundMe(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		CActor* a = local_actor();
		if (!a || !live())
		{
			::Msg("! [coop] coop_wound_me: only in a co-op session, in a level");
			return;
		}
		a->conditions().SetHealth(0.f); // the condition hook turns this into the wounded state
	}
	void Info(TInfo& I) override { xr_strcpy(I, "test: drop my health to zero (falls wounded in a co-op session)"); }
};
} // namespace

// ---------------------------------------------------------------- public

const char* life_name(Life l)
{
	switch (l)
	{
	case Life::Alive: return "alive";
	case Life::Wounded: return "wounded";
	case Life::Dead: return "dead";
	case Life::Spectating: return "spectating";
	case Life::Respawning: return "respawning";
	default: return "?";
	}
}

void collect_players(xr_vector<PlayerRef>& out, bool include_self)
{
	out.clear();
	if (!mgr().active())
		return;
	if (include_self)
	{
		PlayerRef me;
		me.peer = mgr().self();
		me.host_id = my_host_id();
		me.local_id = 0;
		me.nick = mgr().nick();
		me.self = true;
		out.push_back(me);
	}
	if (mgr().is_host())
	{
		for (const PeerInfo& p : mgr().session().peers())
		{
			if (p.state != PeerState::Joined)
				continue;
			const u16 proxy = join::proxy_of_peer(p.id);
			if (proxy == 0xffff)
				continue;
			PlayerRef ref;
			ref.peer = p.id;
			ref.host_id = ref.local_id = proxy;
			ref.nick = p.nick.c_str();
			out.push_back(ref);
		}
		return;
	}
	// Client: the host reports every player character (PlayerWounded roster, every 5 s).
	const u16 me = my_host_id();
	for (const auto& it : g_players)
	{
		if (it.first == me)
			continue;
		PlayerRef ref;
		ref.host_id = it.first;
		ref.local_id = join::to_local(it.first);
		ref.peer = it.first == 0 ? kHostPeer : world::avatar_peer(ref.local_id);
		ref.nick = it.second.nick.empty() ? "?" : it.second.nick;
		out.push_back(ref);
	}
}

xr_string nick_of(u16 host_id)
{
	if (!mgr().active())
		return "?";
	if (host_id == my_host_id())
		return mgr().nick();
	if (mgr().is_host())
	{
		const PeerId peer = join::peer_of_proxy(host_id);
		const PeerInfo* p = peer != kInvalidPeer ? mgr().session().peer(peer) : nullptr;
		return p ? xr_string(p->nick.c_str()) : xr_string("?");
	}
	if (const Remote* r = remote(host_id))
		if (!r->nick.empty())
			return r->nick;
	if (host_id == 0)
		if (const PeerInfo* p = mgr().session().peer(kHostPeer))
			return p->nick.c_str();
	return "?";
}

Life life_of(u16 local_id)
{
	if (local_id == 0)
		return g_me.life;
	const Remote* p = remote(join::to_host(local_id));
	return p ? p->life : Life::Alive;
}

u32 bleed_left_ms(u16 local_id)
{
	const u64 t = now();
	if (local_id == 0)
		return g_me.life == Life::Wounded && g_me.bleed_end > t ? u32(g_me.bleed_end - t) : 0;
	const Remote* p = remote(join::to_host(local_id));
	return p && p->life == Life::Wounded && p->bleed_end > t ? u32(p->bleed_end - t) : 0;
}

bool host_worn_items(u16 character, u16 seq, xr_vector<u16>& out)
{
	out.clear();
	if (!mgr().is_host())
		return false;
	const auto it = g_host.find(character);
	if (it == g_host.end() || !it->second.has_worn || it->second.worn_seq != seq)
		return false;
	out = it->second.worn;
	return true;
}

void install(Session& s)
{
	s.on(Msg::PlayerWounded, &on_state_msg);
	s.on(Msg::PlayerRevive, &on_revive_msg);
	s.on(Msg::PlayerRules, &on_rules_msg);
	s.on(Msg::NewCharacter, &on_new_character_msg); // F3 Task 10
}

void register_console()
{
	CMD3(CCC_Token, "coop_death_mode", &g_cv_death_mode, g_death_mode_tokens);
	CMD3(CCC_Token, "coop_death_drop", &g_cv_death_drop, g_death_drop_tokens);
	CMD4(CCC_Float, "coop_bleedout", &g_cv_bleedout_s, 10.f, 600.f);
	CMD4(CCC_Float, "coop_revive_health", &g_cv_revive_health, 0.05f, 1.f);
	CMD4(CCC_Float, "coop_respawn_delay", &g_cv_respawn_delay_s, 0.f, 300.f); // s
	CMD4(CCC_Float, "coop_contusion", &g_cv_contusion_min, 0.f, 60.f); // min
	CMD4(CCC_Float, "coop_death_money", &g_cv_death_money, 0.f, 100.f); // %
	CMD4(CCC_Float, "coop_death_wear", &g_cv_death_wear, 0.f, 100.f); // %
	CMD4(CCC_Float, "coop_death_rank", &g_cv_death_rank, 0.f, 100.f); // %
	CMD1(CCC_CoopWoundMe, "coop_wound_me");
	CMD1(CCC_CoopNewCharacter, "coop_new_character"); // F3 Task 10
}

void reset()
{
	leave_session();
	g_rules = console_rules();
	g_epoch = 0xffffffff;
	g_last_roster = g_last_guard = 0;
	g_feed.clear();
	g_guard_logged.clear();
}

void frame()
{
	if (!g_pGameLevel || !live())
	{
		if (g_me.actor || !g_players.empty() || !g_host.empty())
			leave_session();
		return;
	}
	const u64 t = now();
	if (mgr().is_host())
		host_frame(t);
	else if (!g_rules_from_host)
		g_rules = console_rules(); // until the host's rules arrive

	CActor* a = local_actor();
	if (a != g_me.actor)
	{
		forget_local(a);
		g_me.actor = a;
		if (a)
			send_my_state(t); // tell everybody this character is alive (rejoin, new world)
	}
	if (a)
		local_frame(a, t);
	reviver_frame(a, t);

	if (mgr().is_live_client())
		for (auto it = g_players.begin(); it != g_players.end();)
			it = t - it->second.updated > kStaleRemoteMs ? g_players.erase(it) : std::next(it);

	// Avatar poses also from here, so a missing drive_avatar hook only costs a one-frame flicker.
	xr_vector<PlayerRef> players;
	collect_players(players, false);
	for (const PlayerRef& p : players)
		if (CAI_Stalker* av = smart_cast<CAI_Stalker*>(object_here(p.local_id)))
			avatar_pose_override(av);

	guard_frame(t);
	build_hud(t);
}

void print_status()
{
	::Msg("[coop]   survival: me %s (fall #%u), death mode %s%s, drop %s, bleed-out %u s, revive health %.2f, "
	      "respawn delay %u s, contusion %u s, money %u%%, wear %u%%, rank %u%%",
	    life_name(g_me.life), g_me.seq, mode_name(g_rules.mode),
	    mgr().is_host() || g_rules_from_host ? "" : " (host rules not received yet)", drop_name(g_rules.drop),
	    g_rules.bleedout_ms / 1000, g_rules.revive_health, g_rules.delay_ms / 1000, g_rules.contusion_s,
	    u32(g_rules.money_pct), u32(g_rules.wear_pct), u32(g_rules.rank_pct));
	::Msg("[coop]   survival: falls %llu, deaths %llu, revived %llu, refused %llu, evacuated %llu, respawns %llu, "
	      "spectate %llu, wipes %llu",
	    g_stat.falls, g_stat.deaths, g_stat.revives, g_stat.refused, g_stat.evacuations, g_stat.respawns,
	    g_stat.spectates, g_stat.wipes);
	::Msg("[coop]   survival: states out %llu in %llu, orders in %llu, requests in %llu, stale orders %llu, kills "
	      "intercepted %llu, proxy kills refused %llu, unharmable fixes %llu, worn lists %llu (refused %llu)",
	    g_stat.states_out, g_stat.states_in, g_stat.orders_in, g_stat.requests_in, g_stat.stale_orders,
	    g_stat.kills_intercepted, g_stat.proxy_kills_refused, g_stat.guard_fixes, g_stat.worn_reports,
	    g_stat.worn_refused);
	::Msg("[coop]   survival: new characters asked %llu, refused %llu, retired %llu, %u retiring", g_stat.newchar_asked,
	    g_stat.newchar_refused, g_stat.retired, u32(g_retire.size()));
	const u64 t = now();
	for (const auto& it : g_players)
		::Msg("[coop]   player %u '%s': %s (fall #%u)%s", it.first, it.second.nick.c_str(), life_name(it.second.life),
		    it.second.seq, it.second.life == Life::Wounded && it.second.bleed_end > t ? ", bleeding out" : "");
	for (const auto& it : g_retire)
		::Msg("[coop]   host: character %u retires, %llu s, stash %u %s", it.first, (t - it.second.since) / 1000,
		    u32(it.second.stash), death::host_stash_pending(it.first) ? "waits for his things" : "is done");
	for (const auto& it : g_host)
	{
		const death::Tracker& k = it.second.tracker;
		if (k.respawn_at || k.respawn_due || k.hardcore_out || it.second.debt)
			::Msg("[coop]   host: character %u%s%s%s%s", it.first,
			    k.respawn_at ? (k.respawn_at > t ? " evacuated" : " respawn due") : "",
			    k.respawn_due ? " respawn on return" : "", k.hardcore_out ? " out (hardcore)" : "",
			    it.second.debt ? " owed a respawn (custom data)" : "");
	}
}

void render_hud()
{
	if (!live() || (g_hud.empty() && g_feed.empty()))
		return;
	CGameFont* big = UI().Font().pFontGraffiti32Russian;
	CGameFont* normal = UI().Font().pFontLetterica18Russian;
	if (!big || !normal)
		return;
	const float cx = float(Device.dwWidth) * 0.5f;
	float y = float(Device.dwHeight) * 0.62f;
	for (const HudLine& l : g_hud)
	{
		CGameFont* f = l.big ? big : normal;
		f->SetAligment(CGameFont::alCenter);
		f->SetColor(l.color);
		f->Out(cx, y, "%s", l.text.c_str());
		y += f->GetHeight() * 1.4f + 4.f;
	}
	float fy = float(Device.dwHeight) * 0.22f;
	for (const FeedLine& l : g_feed)
	{
		normal->SetAligment(CGameFont::alLeft);
		normal->SetColor(0xffe8e8c8);
		normal->Out(float(Device.dwWidth) * 0.02f, fy, "%s", l.text.c_str());
		fy += normal->GetHeight() * 1.3f + 2.f;
	}
	// shared UI fonts: leave them as found (each queued line keeps its own alignment)
	big->SetAligment(CGameFont::alLeft);
	normal->SetAligment(CGameFont::alLeft);
}

// ---------------------------------------------------------------- engine hooks

void actor_condition_hook(CActorCondition& c)
{
	if (!live() || !c.object().Local())
		return;
	const float h = c.GetHealth();
	if (g_hook_life.load() != u8(Life::Alive) && g_hook_owner.load() == &c)
	{
		if (h < kDownHealth)
			g_hook_damage.fetch_add(u32(_min(kDownHealth - h, 100.f) * 1000.f + 0.5f)); // huge hits: no float->u32 overflow
		if (h != kDownHealth)
			c.SetHealth(kDownHealth);
		return;
	}
	if (h <= 0.f)
	{
		c.SetHealth(kDownHealth);
		g_hook_owner.store(&c);
		g_hook_life.store(u8(Life::Wounded)); // pinned from now on; frame() completes the fall
		g_hook_fell.store(true);
	}
}

bool suppress_death_effector() { return live(); }

bool intercept_actor_kill(CEntity* victim, u16 killer_id)
{
	if (!victim || !live())
		return false;
	if (world::is_player_proxy(victim->ID()))
	{
		++g_stat.proxy_kills_refused; // a player's character dies only on its owner's machine (and never there)
		::Msg("! [coop] kill of player character %u (killer %u) refused", victim->ID(), killer_id);
		return true;
	}
	CActor* a = smart_cast<CActor*>(victim);
	if (!a || a != local_actor())
		return false;
	if (g_hook_life.load() == u8(Life::Alive) || g_hook_owner.load() != &a->conditions())
	{
		a->conditions().SetHealth(kDownHealth);
		g_hook_owner.store(&a->conditions());
		g_hook_life.store(u8(Life::Wounded));
		g_hook_fell.store(true);
	}
	++g_stat.kills_intercepted;
	return true;
}

bool on_use_person(CInventoryOwner* person)
{
	if (!person || !live())
		return false;
	CGameObject* o = smart_cast<CGameObject*>(person);
	if (!o || !world::is_player_proxy(o->ID()))
		return false;
	const Life l = life_of(o->ID());
	if (l != Life::Wounded)
		return is_down(l); // a dead teammate: nothing to talk to or loot
	CActor* a = local_actor();
	if (!a || is_down(g_me.life))
		return true;
	const xr_string who = nick_of(join::to_host(o->ID()));
	PIItem medkit = find_revive_item(a);
	if (!medkit)
	{
		feed("You need a medkit to revive %s.", who.c_str());
		return true;
	}
	g_me.revive_target = o->ID();
	g_me.revive_seq = seq_by_host_id(join::to_host(o->ID()));
	g_me.revive_item = join::to_host(medkit->object().ID());
	g_me.revive_start = now();
	return true;
}

bool avatar_pose_override(CAI_Stalker* avatar)
{
	if (!avatar || !live())
		return false;
	const u16 id = avatar->ID();
	const Life l = life_of(id);
	auto it = g_poses.find(id);
	if (!is_down(l))
	{
		if (it != g_poses.end())
			g_poses.erase(it);
		return false;
	}
	IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(avatar->Visual());
	if (!ka)
		return false;
	const Life pose_life = l == Life::Wounded ? Life::Wounded : Life::Dead;
	Pose& p = g_poses[id];
	if (p.life != pose_life)
	{
		p.life = pose_life;
		p.motion = find_pose(ka, pose_life);
		p.missing = !p.motion.valid();
		if (p.missing)
			::Msg("! [coop] avatar %u: no lying animation in its visual, it keeps the player's motions", id);
	}
	if (p.missing)
		return false;
	if (!blend_playing(ka, p.motion))
		ka->PlayCycle(p.motion, TRUE);
	return true;
}
} // namespace survival
} // namespace coop
