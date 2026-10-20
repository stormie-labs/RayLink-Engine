// Anomaly Coop — player survival (WP8). See coop_survival.h for the life cycle and the wire layouts.
#include "stdafx.h"
#include "coop_survival.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_net.h"
#include "coop_script.h"

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
#include "xr_level_controller.h"
#include "ai_space.h"
#include "level_graph.h"
#include "ai_object_location.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

#include <atomic>
#include <cstdarg>

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
constexpr u32 kDropWaitMs = 1500; // respawn with item loss: let the drops land before the teleport
constexpr u32 kStaleRemoteMs = 15000; // client: forget players the host stopped reporting
constexpr u32 kGuardMs = 1000;
constexpr float kCamDrop = 1.1f; // eye height drop while lying

enum Op : u8
{
	OpRequest = 0,
	OpRevive = 1,
	OpRespawn = 2,
	OpSpectate = 3,
	OpRefused = 4,
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

u32 g_cv_death_mode = u32(DeathMode::RespawnKeep);
float g_cv_bleedout_s = 60.f;
float g_cv_revive_health = 0.3f;
float g_cv_respawn_delay_s = 10.f;

xr_token g_death_mode_tokens[] = {
	{"hardcore", int(DeathMode::Hardcore)},
	{"respawn", int(DeathMode::Respawn)},
	{"respawn_keep", int(DeathMode::RespawnKeep)},
	{nullptr, 0},
};

const char* mode_name(DeathMode m)
{
	switch (m)
	{
	case DeathMode::Hardcore: return "hardcore";
	case DeathMode::Respawn: return "respawn";
	default: return "respawn_keep";
	}
}

struct Rules
{
	DeathMode mode = DeathMode::RespawnKeep;
	u32 bleedout_ms = 60000;
	float revive_health = 0.3f;
	u32 respawn_delay_ms = 10000;

	bool operator==(const Rules& o) const
	{
		return mode == o.mode && bleedout_ms == o.bleedout_ms && fsimilar(revive_health, o.revive_health) &&
		    respawn_delay_ms == o.respawn_delay_ms;
	}
	void write(Writer& w) const
	{
		w.u8(u8(mode));
		w.u32(bleedout_ms);
		w.f32(revive_health);
		w.u32(respawn_delay_ms);
	}
	bool read(Reader& r)
	{
		const u8 m = r.u8();
		bleedout_ms = r.u32();
		revive_health = r.f32();
		respawn_delay_ms = r.u32();
		if (!r.ok() || m > u8(DeathMode::RespawnKeep) || !_valid(revive_health))
			return false;
		mode = DeathMode(m);
		clamp(bleedout_ms, u32(5000), u32(600000));
		clamp(revive_health, 0.05f, 1.f);
		clamp(respawn_delay_ms, u32(0), u32(120000));
		return true;
	}
};

Rules console_rules()
{
	Rules r;
	r.mode = DeathMode(_min(g_cv_death_mode, u32(DeathMode::RespawnKeep)));
	r.bleedout_ms = u32(g_cv_bleedout_s * 1000.f);
	r.revive_health = g_cv_revive_health;
	r.respawn_delay_ms = u32(g_cv_respawn_delay_s * 1000.f);
	return r;
}

Rules g_rules; // in force: host = console, client = last PlayerRules from the host
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
	u8 cam = 0; // 0 none, 1 lying, 2 spectator
	// respawn
	Fvector respawn_pos{};
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
	u16 died_seq = 0xffff; // last fall that ended in death and got a decision
	u64 respawn_at = 0;
	u16 respawn_seq = 0;
	bool respawn_drop = false;
	u64 revive_pending_until = 0;
	bool hardcore_dead = false; // stays dead in this world even after a rejoin
	bool has_anchor = false;
	Fvector anchor{}; // where the player was first seen alive in this world (respawn fallback)
};
xr_map<u16, HostRec> g_host;
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

struct Stats
{
	u64 falls = 0, deaths = 0, revives = 0, refused = 0, respawns = 0, spectates = 0;
	u64 states_out = 0, states_in = 0, orders_in = 0, requests_in = 0;
	u64 kills_intercepted = 0, proxy_kills_refused = 0, guard_fixes = 0, stale_orders = 0;
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
		if (cam == 1)
			a->Cameras().AddCamEffector(xr_new<DownCam>());
		else if (cam == 2)
		{
			g_spec.valid = false;
			a->Cameras().AddCamEffector(xr_new<SpectateCam>());
		}
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
		net::client_send(Msg::PlayerWounded, w);
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

// ---------------------------------------------------------------- own life

void enter_wounded(CActor* a, u64 t)
{
	g_me.life = Life::Wounded;
	g_hook_life.store(u8(Life::Wounded));
	++g_me.seq;
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

// Respawn with item loss: every carried item except quest items and the PDA is dropped through the normal
// drop path (GE_OWNERSHIP_REJECT, as the inventory UI does). On a client the coop router turns it into a
// drop request and the host performs it, so the host world gets the items where the player died.
u32 drop_carried_items(CActor* a)
{
	xr_vector<u16> ids;
	for (PIItem it : a->inventory().m_all)
	{
		if (!it || it->object().getDestroy() || it->IsQuestItem() || it->BaseSlot() == PDA_SLOT)
			continue;
		ids.push_back(it->object().ID());
	}
	for (u16 id : ids)
	{
		CGameObject* o = object_here(id);
		if (!o || !o->H_Parent() || o->H_Parent()->ID() != a->ID())
			continue;
		NET_Packet P;
		o->u_EventGen(P, GE_OWNERSHIP_REJECT, a->ID());
		P.w_u16(id);
		o->u_EventSend(P);
	}
	return u32(ids.size());
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

void apply_respawn(u16 seq, const Fvector& pos, bool drop)
{
	CActor* a = local_actor();
	if (!a || g_me.life != Life::Dead || seq != g_me.seq)
	{
		++g_stat.stale_orders;
		return;
	}
	const u64 t = now();
	g_me.life = Life::Respawning;
	g_hook_life.store(u8(Life::Respawning));
	g_me.since = t;
	g_me.respawn_pos = pos;
	u32 dropped = 0;
	if (drop)
		dropped = drop_carried_items(a);
	g_me.respawn_at = t + (dropped ? kDropWaitMs : 300);
	if (dropped)
		feed("Your gear stays where you died (%u items).", dropped);
	send_my_state(t);
}

void finish_respawn(CActor* a, u64 t)
{
	Fmatrix m = a->XFORM();
	m.c.set(g_me.respawn_pos);
	a->ForceTransform(m);
	++g_stat.respawns;
	stand_up(a, a->conditions().GetMaxHealth(), true, t);
	feed("You respawned.");
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
	close_menus();
	hold_controls(a);
	set_cam(a, 2);
	++g_stat.spectates;
	feed("Hardcore: you are out until the host loads a save.");
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
		const bool drop = r.boolean();
		if (r.ok() && _valid(p[0]) && _valid(p[1]) && _valid(p[2]))
			apply_respawn(seq, Fvector().set(p[0], p[1], p[2]), drop);
		break;
	}
	case OpSpectate: apply_spectate(seq); break;
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
	case Life::Spectating: feed("%s is out until the host loads a save.", who.c_str()); break;
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

Fvector ground_point(CGameObject* o)
{
	Fvector p = o->Position();
	if (ai().get_level_graph())
	{
		const u32 v = o->ai_location().level_vertex_id();
		if (ai().level_graph().valid_vertex_id(v))
			p = ai().level_graph().vertex_position(v);
		// stand beside the teammate, not inside its capsule, when that spot is walkable
		Fvector side = p;
		side.x += 0.8f;
		if (ai().level_graph().valid_vertex_id(ai().level_graph().vertex_id(side)))
			p = side;
	}
	p.y += 0.2f;
	return p;
}

// Respawn point: beside the living teammate nearest to the body; else where the player was first seen
// alive in this world; else where it fell.
bool pick_respawn_point(u16 dead, Fvector& out)
{
	CGameObject* body = object_here(dead);
	float best = flt_max;
	bool found = false;
	auto consider = [&](u16 id)
	{
		if (id == dead)
			return;
		CGameObject* o = object_here(id);
		if (!o)
			return;
		const float d = body ? o->Position().distance_to_sqr(body->Position()) : 0.f;
		if (d < best)
		{
			best = d;
			out = ground_point(o);
			found = true;
		}
	};
	if (g_me.life == Life::Alive)
		consider(0);
	for (const auto& it : g_players)
		if (it.second.life == Life::Alive && !world::is_parked(it.first))
			consider(it.first);
	if (found)
		return true;
	const HostRec& h = g_host[dead];
	if (h.has_anchor)
	{
		out = h.anchor;
		return true;
	}
	if (body)
	{
		out = body->Position();
		return true;
	}
	return false;
}

void host_on_died(u16 id, u16 seq)
{
	HostRec& h = g_host[id];
	if (h.died_seq == seq)
		return; // decided already (state repeats are normal)
	h.died_seq = seq;
	switch (g_rules.mode)
	{
	case DeathMode::Hardcore:
		h.hardcore_dead = true;
		host_order_spectate(id, seq);
		break;
	case DeathMode::Respawn:
	case DeathMode::RespawnKeep:
		h.respawn_at = now() + g_rules.respawn_delay_ms;
		h.respawn_seq = seq;
		h.respawn_drop = g_rules.mode == DeathMode::Respawn;
		break;
	}
}

void host_on_state(u16 id, Life life, u16 seq)
{
	HostRec& h = g_host[id];
	switch (life)
	{
	case Life::Alive:
		h.revive_pending_until = 0;
		h.respawn_at = 0;
		h.died_seq = 0xffff; // that death is over (a rejoined player counts its falls from 0 again)
		if (!h.has_anchor)
			if (CGameObject* o = object_here(id))
			{
				h.anchor = ground_point(o);
				h.has_anchor = true;
			}
		if (h.hardcore_dead && g_rules.mode == DeathMode::Hardcore)
			host_order_spectate(id, seq); // rejoined after dying: still out in this world
		break;
	case Life::Dead: host_on_died(id, seq); break;
	default: break;
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
		g_epoch = epoch; // a load / level change replaced the world: deaths and anchors start over
		g_host.clear();
	}

	const Rules r = console_rules();
	if (!(r == g_rules))
	{
		const bool mode_changed = r.mode != g_rules.mode;
		g_rules = r;
		host_broadcast_rules();
		if (mode_changed)
			feed("Death mode: %s.", mode_name(r.mode));
	}

	// players that left: their characters are parked, nothing to decide for them
	for (auto it = g_players.begin(); it != g_players.end();)
	{
		if (world::is_parked(it->first) || join::peer_of_proxy(it->first) == kInvalidPeer)
		{
			HostRec& h = g_host[it->first];
			h.respawn_at = 0;
			h.revive_pending_until = 0;
			h.died_seq = 0xffff; // hardcore_dead stays: leaving does not bring a dead player back
			it = g_players.erase(it);
		}
		else
			++it;
	}

	for (auto& it : g_host)
	{
		HostRec& h = it.second;
		if (!h.respawn_at || t < h.respawn_at)
			continue;
		Fvector pos;
		if (!pick_respawn_point(it.first, pos))
		{
			h.respawn_at = t + 2000;
			continue;
		}
		h.respawn_at = 0;
		Writer w(24);
		w.u8(OpRespawn);
		w.u16(it.first);
		w.u16(h.respawn_seq);
		const float p[3] = {pos.x, pos.y, pos.z};
		w.vec3(p);
		w.boolean(h.respawn_drop);
		host_send_order(it.first, w);
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

void on_rules_msg(PeerId from, Reader& r)
{
	if (mgr().is_host() || from != kHostPeer)
		return;
	Rules rules;
	if (!rules.read(r))
		return;
	if (!g_rules_from_host || rules.mode != g_rules.mode)
		::Msg("[coop] host survival rules: death mode %s, bleed-out %u s, revive health %.2f, respawn delay %u s",
		    mode_name(rules.mode), rules.bleedout_ms / 1000, rules.revive_health, rules.respawn_delay_ms / 1000);
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
	case Life::Dead: hold_controls(a); break;
	case Life::Spectating:
		hold_controls(a);
		spectate_frame(t);
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
		line("YOU DIED", 0xffe03c3c, true);
		if (g_rules.mode == DeathMode::Hardcore)
			line("Waiting for the host...", 0xfff0f0f0, false);
		else
		{
			const u64 at = g_me.since + g_rules.respawn_delay_ms;
			xr_sprintf(buf, "Respawn in %u s%s", at > t ? u32((at - t + 999) / 1000) : 0,
			    g_rules.mode == DeathMode::Respawn ? " (your gear stays here)" : "");
			line(buf, 0xfff0f0f0, false);
		}
		break;
	case Life::Respawning: line("Respawning...", 0xfff0f0f0, true); break;
	case Life::Spectating:
	{
		bool anyone = false;
		if (g_me.spectate != 0xffff)
		{
			xr_sprintf(buf, "SPECTATING %s", nick_of(join::to_host(g_me.spectate)).c_str());
			line(buf, 0xfff0f0f0, true);
			line("Fire: next player   Aim: previous player", 0xffa0a0a0, false);
			anyone = true;
		}
		if (!anyone)
			line("All players are down. The host must load a save.", 0xffe03c3c, true);
		else
			line("You are dead until the host loads a save.", 0xffa0a0a0, false);
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
	g_host.clear();
	g_rules_from_host = false;
	g_hud.clear();
}

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

void install(Session& s)
{
	s.on(Msg::PlayerWounded, &on_state_msg);
	s.on(Msg::PlayerRevive, &on_revive_msg);
	s.on(Msg::PlayerRules, &on_rules_msg);
}

void register_console()
{
	CMD3(CCC_Token, "coop_death_mode", &g_cv_death_mode, g_death_mode_tokens);
	CMD4(CCC_Float, "coop_bleedout", &g_cv_bleedout_s, 10.f, 600.f);
	CMD4(CCC_Float, "coop_revive_health", &g_cv_revive_health, 0.05f, 1.f);
	CMD4(CCC_Float, "coop_respawn_delay", &g_cv_respawn_delay_s, 0.f, 120.f);
	CMD1(CCC_CoopWoundMe, "coop_wound_me");
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
	::Msg("[coop]   survival: me %s (fall #%u), death mode %s%s, bleed-out %u s, revive health %.2f",
	    life_name(g_me.life), g_me.seq, mode_name(g_rules.mode),
	    mgr().is_host() || g_rules_from_host ? "" : " (host rules not received yet)", g_rules.bleedout_ms / 1000,
	    g_rules.revive_health);
	::Msg("[coop]   survival: falls %llu, deaths %llu, revived %llu, refused %llu, respawns %llu, spectate %llu",
	    g_stat.falls, g_stat.deaths, g_stat.revives, g_stat.refused, g_stat.respawns, g_stat.spectates);
	::Msg("[coop]   survival: states out %llu in %llu, orders in %llu, requests in %llu, stale orders %llu, kills "
	      "intercepted %llu, proxy kills refused %llu, unharmable fixes %llu",
	    g_stat.states_out, g_stat.states_in, g_stat.orders_in, g_stat.requests_in, g_stat.stale_orders,
	    g_stat.kills_intercepted, g_stat.proxy_kills_refused, g_stat.guard_fixes);
	const u64 t = now();
	for (const auto& it : g_players)
		::Msg("[coop]   player %u '%s': %s (fall #%u)%s", it.first, it.second.nick.c_str(), life_name(it.second.life),
		    it.second.seq, it.second.life == Life::Wounded && it.second.bleed_end > t ? ", bleeding out" : "");
	for (const auto& it : g_host)
		if (it.second.respawn_at || it.second.hardcore_dead)
			::Msg("[coop]   host: character %u%s%s", it.first, it.second.respawn_at ? " respawn pending" : "",
			    it.second.hardcore_dead ? " out (hardcore)" : "");
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
