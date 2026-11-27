// Anomaly Coop — player survival: the own life, the world server's decisions on deaths and revives, the messages. See
// coop_survival.h for the life cycle and the wire layouts; the cameras, the HUD and the new character are coop_survival_cam.cpp,
// coop_survival_hud.cpp and coop_survival_newchar.cpp.
#include "stdafx.h"
#include "coop_survival_state.h"
#include "coop_levels.h"
#include "coop_net.h"
#include "coop_script.h"
#include "coop_autosave.h"
#include "coop_ai.h"
#include "coop_requests.h" // g_coop_friendly_fire
#include "coop_wire_vec.h"
#include "coop_health_ledger.h" // the co-op's writes of the own actor's health
#include "xrCoop/coop_checkpoint_store.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "Level.h"
#include "Actor.h"
#include "ActorCondition.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryOwner.h"
#include "ai_space.h"
#include "level_graph.h"
#include "ai_object_location.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <string>
#include <vector>

namespace coop
{
namespace survival
{
// ---------------------------------------------------------------- state (coop_survival_state.h)

Local g_me;
xr_map<u16, Remote> g_players;
xr_map<u16, HostRec> g_host;
death::Rules g_rules; // this machine's player's
std::atomic<const CActorCondition*> g_hook_owner{nullptr}; // condition of the actor that is down (a load makes a new one)
Stats g_stat;

// ---------------------------------------------------------------- helpers

bool live() { return mode().decides_world() || mgr().is_live_client(); }
u16 my_host_id() { return mode().decides_world() ? u16(0) : join::my_proxy(); }

CActor* local_actor() { return g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr; }

namespace
{
Remote* remote(u16 host_id)
{
	auto it = g_players.find(host_id);
	return it == g_players.end() ? nullptr : &it->second;
}

u16 seq_by_host_id(u16 host_id)
{
	if (host_id == my_host_id())
		return g_me.seq;
	const Remote* p = remote(host_id);
	return p ? p->seq : 0;
}
} // namespace

Life life_by_host_id(u16 host_id)
{
	if (host_id == my_host_id())
		return g_me.life;
	const Remote* p = remote(host_id);
	return p ? p->life : Life::Alive;
}

// The transport peer driving the character `host_id` (host numbering; 0 = the host's actor), kInvalidPeer if unknown.
PeerId peer_of_host_id(u16 host_id)
{
	if (host_id == 0)
		return kHostPeer;
	if (host_id == my_host_id())
		return mgr().self();
	return mode().decides_world() ? join::peer_of_proxy(host_id) : world::avatar_peer(join::to_local(host_id));
}

// The object id of the character `host_id` on this machine (the host numbers its own objects, a client maps them).
u16 local_of_host_id(u16 host_id) { return mode().decides_world() ? host_id : join::to_local(host_id); }

// Host: an order to the owner of `target`. The host player's goes over the internal channel like a client's and is applied by
// his handler at the next pump (client_on_revive), never here.
void host_send_order(u16 target, const Writer& w)
{
	const PeerId peer = peer_of_character(target);
	if (peer != kInvalidPeer)
		net::host_send(peer, Msg::PlayerRevive, w);
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

// The respawn debt lives with the character in the world, so it reaches exactly the worlds saved while he owed it (a level
// change, an autosave) and no new game, no other save, no other entity under his id. So does "out of a hardcore run" (kOutKey,
// death::learn_out). [coop_death] <key> = 1.
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

namespace
{
constexpr float kReviveRange = 2.5f; // reviver side
constexpr float kHostReviveRange = 4.5f; // host check (avatars are rendered 100 ms in the past)
constexpr u32 kStateRefreshMs = 5000; // own life state and (host) rules / roster resend: late joiners
constexpr u32 kRaisedRefreshMs = 1000; // own life state while a teammate raises me (my time stands still)
constexpr u32 kRevivePendingMs = 5000; // host: one medkit per fall, even if the order is slow
constexpr u32 kDamageMsPerHealth = 100000; // damage taken while down shortens bleed-out (0.1 health = 10 s)
constexpr u32 kRespawnSettleMs = 300; // owner: the Respawning state goes out before the teleport
constexpr u32 kRespawnRetryMs = 2000; // host: no body to respawn right now (not in the world yet), try again
constexpr u32 kStaleRemoteMs = 15000; // client: forget players the host stopped reporting
constexpr u32 kGuardMs = 1000;
constexpr u32 kRollbackDelayMs = 5000; // rollback mode: everybody reads who died, then the host loads

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

const char* refusal_id(u8 r)
{
	switch (r)
	{
	case RefNotWounded: return "st_coop_refusal_not_wounded";
	case RefTooFar: return "st_coop_refusal_too_far";
	case RefNoMedkit: return "st_coop_refusal_no_medkit";
	case RefBusy: return "st_coop_refusal_busy";
	case RefYouAreDown: return "st_coop_refusal_you_are_down";
	case RefSelf: return "st_coop_refusal_self";
	default: return "st_coop_refusal_other";
	}
}

// Items a reviver uses, by section prefix, in order of preference.
const char* const kReviveItems[] = {"medkit", "stimpack"};

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
float g_cv_death_share = death::kShareDefaultPct; // the loss shares of drop and lose x this / 100 (0..200 %)

// The old modes respawn / respawn_keep live on in user.ltx files: both mean checkpoint. CCC_Token shows and saves the first name of
// a value, so they come back as "checkpoint".
xr_token g_death_mode_tokens[] = {
	{"checkpoint", int(death::Mode::Checkpoint)},
	{"hardcore", int(death::Mode::Hardcore)},
	{"rollback", int(death::Mode::Rollback)},
	{"respawn", int(death::Mode::Checkpoint)},
	{"respawn_keep", int(death::Mode::Checkpoint)},
	{nullptr, 0},
};

xr_token g_death_drop_tokens[] = {
	{"drop", int(death::Drop::Drop)},
	{"corpse", int(death::Drop::Corpse)},
	{"lose", int(death::Drop::Lose)},
	{"keep", int(death::Drop::Keep)}, // nothing is lost
	{nullptr, 0},
};

const char* mode_name(death::Mode m)
{
	return m == death::Mode::Hardcore ? "hardcore" : m == death::Mode::Rollback ? "rollback" : "checkpoint";
}

const char* mode_id(death::Mode m)
{
	return m == death::Mode::Hardcore   ? "st_coop_game_death_mode_hardcore"
	       : m == death::Mode::Rollback ? "st_coop_game_death_mode_rollback"
	                                    : "st_coop_game_death_mode_checkpoint";
}

u8 console_pct(float v) { return u8(iFloor(clampr(v, 0.f, 100.f) + 0.5f)); }

// coop_death_share as Rules::share_pct: 0..200, rounded; not a number (the console reads it with atof, and a NaN passes
// the range check of CCC_Float) is 0, nothing is lost.
u8 console_share(float v)
{
	if (!(v > 0.f))
		return 0;
	return u8(iFloor(_min(v, death::kShareMaxPct) + 0.5f));
}

death::Rules console_rules()
{
	death::Rules r;
	r.mode = death::Mode(_min(g_cv_death_mode, u32(death::Mode::Rollback)));
	r.drop = death::Drop(_min(g_cv_death_drop, u32(death::Drop::Keep)));
	r.bleedout_ms = u32(g_cv_bleedout_s * 1000.f);
	r.revive_health = g_cv_revive_health;
	r.delay_ms = u32(clampr(g_cv_respawn_delay_s, 0.f, 300.f) * 1000.f);
	r.contusion_s = u32(clampr(g_cv_contusion_min, 0.f, 60.f) * 60.f + 0.5f);
	r.money_pct = console_pct(g_cv_death_money);
	r.wear_pct = console_pct(g_cv_death_wear);
	r.rank_pct = console_pct(g_cv_death_rank);
	r.share_pct = console_share(g_cv_death_share);
	r.friendly_fire = g_coop_friendly_fire != 0;
	r.finish_downed = npc_ai::finish_downed();
	return r;
}

// Two copies, as a listen server has them. g_world_rules are the world server's (its console; its decisions use
// them, it sends them as PlayerRules); g_rules are this machine's player's: the world server's last PlayerRules - the host
// player takes them over the internal channel like a client - and the console's own until they come.
death::Rules g_world_rules; // the world server's (the host's process)
bool g_rules_from_host = false;

bool g_wipe_fired = false; // coop_group_wipe went out; again only after somebody stood up
u64 g_rollback_at = 0; // rollback mode: when the host goes back to the last save, 0 = not on the way
u32 g_epoch = 0xffffffff;
u64 g_last_roster = 0;
u64 g_last_guard = 0;
// Host: join::world_generation() as of the last frame of this module (after join::frame served it). A world
// created since then is not served yet - the same guard join::host_player_in_world keeps for a client (its world_seen,
// set by host_watch_world): player_in_world(0) waits for it as well.
u32 g_world_served = 0;

// Written by the condition hook (maybe on the GameThread worker), consumed by frame().
std::atomic<u8> g_hook_life{u8(Life::Alive)};
float g_hook_rad = 0.f; // the dose at the fall: main thread, as the hook
std::atomic<bool> g_hook_fell{false};
std::atomic<u32> g_hook_damage{0}; // milli-health lost while down
xr_set<u16> g_guard_logged;

// The place in kReviveItems of the first prefix `section` starts with, -1 for none.
int revive_rank(const char* section)
{
	if (section && *section)
		for (int i = 0; i < int(std::size(kReviveItems)); ++i)
			if (strstr(section, kReviveItems[i]) == section)
				return i;
	return -1;
}

// The revive item `a` uses: of the most preferred kind he carries, the first one of his inventory.
PIItem find_revive_item(CActor* a)
{
	PIItem best = nullptr;
	int best_rank = int(std::size(kReviveItems));
	for (PIItem it : a->inventory().m_all)
	{
		if (!it || it->object().getDestroy())
			continue;
		const int rank = revive_rank(it->object().cNameSect().c_str());
		if (rank >= 0 && rank < best_rank)
		{
			best = it;
			best_rank = rank;
		}
	}
	return best;
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

// The tail of a client's Dead report (death::write_worn): the items in its slots and on its belt in host
// numbering, each once. The host's proxy cannot show them (an NPC has no outfit or belt).
void write_worn(Writer& w, CActor& a)
{
	std::vector<u16> ids;
	const auto add = [&ids](PIItem it)
	{
		if (!it || it->object().getDestroy())
			return;
		const u16 id = join::to_host(it->object().ID());
		if (id != kNoId && std::find(ids.begin(), ids.end(), id) == ids.end())
			ids.push_back(id);
	};
	CInventory& inv = a.inventory();
	for (u16 slot = inv.FirstSlot(); slot <= inv.LastSlot(); ++slot)
		add(inv.ItemFromSlot(slot));
	for (PIItem it : inv.m_belt)
		add(it);
	death::write_worn(w, ids); // the first death::kMaxWornIds
}

// My life to the world server, which decides on it and relays it to the others. The host player's too, over the internal
// channel (net::client_send in the host's process): the world takes his report as a client's, at the next pump.
void send_my_state(u64 t)
{
	g_me.last_sent = t;
	const u16 id = my_host_id();
	if (id == kNoId)
		return;
	const u32 left = g_me.life == Life::Wounded && g_me.bleed_end > t ? u32(g_me.bleed_end - t) : 0;
	Writer w(32);
	write_state(w, id, g_me.life, g_me.seq, left, mgr().nick());
	// A client's proxy does not show the world what he wears: the losses of his death read this list. The
	// host's actor is the world's own object: its slots and belt are read there (no actor: no list).
	if (g_me.life == Life::Dead && g_me.actor && is_mirror())
		write_worn(w, *g_me.actor);
	net::client_send(Msg::PlayerWounded, w);
	++g_stat.states_out;
}

void apply_order(u8 op, Reader& r);
void stop_revive_hold(bool notify);


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
	if (a && g_hook_life.load() == u8(Life::Alive))
		g_hook_rad = a->conditions().GetRadiation();
	g_me.life = Life::Wounded;
	g_hook_life.store(u8(Life::Wounded));
	g_me.seq = death::next_seq(g_me.seq); // never death::kNoSeq, the tracker's "no death decided"
	g_me.since = t;
	g_me.bleed_end = t + g_rules.bleedout_ms;
	g_me.giveup_start = 0;
	g_me.giveup_armed = false;
	stop_revive_hold(true); // fell while holding USE on a teammate: the host is told it ended
	g_me.lift.clear();
	g_hook_damage.store(0);
	close_menus();
	hold_controls(a);
	set_cam(a, Cam::Down);
	++g_stat.falls;
	feed_tr_d("st_coop_feed_you_down", "You are down! A teammate can revive you with a medkit (%d s).",
	    int(g_rules.bleedout_ms / 1000));
	send_my_state(t);
}

void enter_dead(CActor* a, u64 t)
{
	if (a && g_hook_life.load() == u8(Life::Alive))
		g_hook_rad = a->conditions().GetRadiation();
	g_me.life = Life::Dead;
	g_hook_life.store(u8(Life::Dead));
	g_me.since = t;
	g_me.giveup_start = 0;
	g_me.lift.clear();
	g_me.evac_end = 0;
	hold_controls(a);
	set_cam(a, Cam::Down);
	++g_stat.deaths;
	feed_tr("st_coop_feed_you_died", "You died.");
	send_my_state(t); // the host decides by its death mode
}

void end_spectate_fade()
{
	if (g_me.fading)
		::Sound->set_master_volume(1.f);
	g_me.fading = false;
}

void stand_up(CActor* a, float health, bool fresh, u64 t, bool announce = true)
{
	end_spectate_fade();
	g_me.life = Life::Alive;
	g_hook_life.store(u8(Life::Alive));
	g_hook_damage.store(0);
	g_me.lift.clear();
	g_me.since = t;
	g_me.respawn_at = 0;
	g_me.evac_end = 0;
	g_me.spectate = kNoId;
	if (a)
	{
		CActorCondition& c = a->conditions();
		c.ClearWounds();
		if (fresh)
			c.SetRadiation(0.f);
		health_ledger::coop_set_health(c, clampr(health, kDownHealth, c.GetMaxHealth()));
	}
	g_me.shield_until = fresh ? t + death::kRespawnShieldMs : 0;
	release_controls(a);
	if (announce)
		send_my_state(t);
}

// The wear of a death (coop_death_wear): the weapons, the outfit and the helmet in the owner's slots lose `pct` % of
// their condition. The owner owns the condition of its items: the ItemState carries it to the host and the others.
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
	stand_up(a, health, false, mgr().now_ms());
	feed_tr_s("st_coop_feed_revived_by", "%s revived you.", nick_of(reviver).c_str());
	if (reviver_peer == kInvalidPeer) // a malformed tail: derive it from the character id
		reviver_peer = peer_of_host_id(reviver);
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
	g_me.evac_end = mgr().now_ms() + delay_ms;
	if (!g_me.evac_end)
		g_me.evac_end = 1; // 0 means "no Evacuate yet"
	g_me.spectate = kNoId;
	close_menus();
	++g_stat.evacuations;
}

// The host's Respawn: back to life at `pos`, the player's checkpoint of `kind`. A Dead owner gets it when its
// evacuation is over; an Alive one when it came back to the world while dead (a rejoin, a world change) and the
// host still owes it the respawn (its seq is the one it reported alive). The items were taken once, by the host,
// when the death was decided.
// A Spectating owner gets it only as his new character after a hardcore run (out of the run he is never respawned): the host
// player's, whose actor stays in the world (host_order_new_life; a client's new character comes by a new join instead). Nothing of
// the death is charged then (no wear, no coop_respawned). He takes it whatever fall it names, and only he does
// (death::take_respawn).
void apply_respawn(u16 seq, const Fvector& pos, checkpoint::Kind kind)
{
	CActor* a = local_actor();
	// death::take_respawn: a spectator takes it whatever fall it names - the world's records start over with a
	// world and may not know his; the new-life order names none (death::kNewLifeSeq), so a standing owner never takes it.
	const death::RespawnTake take = death::take_respawn(u8(g_me.life), g_me.seq, seq);
	const bool fresh = take == death::RespawnTake::NewLife;
	if (!a || take == death::RespawnTake::Stale)
	{
		++g_stat.stale_orders;
		return;
	}
	const u64 t = mgr().now_ms();
	if (g_me.life == Life::Alive)
	{
		close_menus();
		hold_controls(a); // as every Respawning frame (local_frame); the camera stays where it is
	}
	if (fresh)
	{
		g_me.spectate = kNoId;
		g_me.newchar_start = 0;
		g_me.newchar_armed = false;
		g_me.newchar_asked = 0;
	}
	g_me.life = Life::Respawning;
	g_hook_life.store(u8(Life::Respawning));
	g_me.since = t;
	g_me.respawn_pos = pos;
	g_me.respawn_kind = kind;
	g_me.respawn_at = t + kRespawnSettleMs;
	g_me.respawn_fresh = fresh;
	const u32 worn = fresh ? 0 : wear_worn_items(a, g_rules.wear_pct);
	if (worn)
		::Msg("[coop] death wear: %u worn item(s) lose %u%% condition", worn, u32(g_rules.wear_pct));
	send_my_state(t);
}

// The host player's new character after a hardcore run, ordered by the world server (host_order_new_life): his
// actor stands up where the order says as a fresh one - health 1 (up to his maximum), no radiation, no bleeding, fed and
// rested, controls back; his money is 0 already (the world set it). The world's Lua offers him faction and kit.
void finish_new_life(CActor* a, u64 t)
{
	CActorCondition& c = a->conditions();
	c.SetSatiety(1.f);
	c.SetPower(1.f);
	stand_up(a, 1.f, true, t); // Alive goes out: the world's tracker starts over with it
	::Msg("[coop] my new character stands up at (%.1f, %.1f, %.1f)", g_me.respawn_pos.x, g_me.respawn_pos.y,
	    g_me.respawn_pos.z);
	feed_tr("st_coop_feed_new_char", "A new character. Choose faction and gear.");
}

void finish_respawn(CActor* a, u64 t)
{
	Fmatrix m = a->XFORM();
	m.c.set(g_me.respawn_pos);
	a->ForceTransform(m);
	if (g_me.respawn_fresh)
	{
		g_me.respawn_fresh = false;
		finish_new_life(a, t);
		return;
	}
	++g_stat.respawns;
	const checkpoint::Kind kind = g_me.respawn_kind;
	const u16 seq = g_me.seq; // the fall this respawn answers (apply_respawn took it from the host's order)
	stand_up(a, a->conditions().GetMaxHealth(), true, t);
	// The player reads it in Lua's tip st_coop_respawned_<kind> (coop_death.script); here only the log.
	if (kind == checkpoint::Kind::None)
		::Msg("[coop] You respawned (no camp of yours on this level).");
	else
		::Msg("[coop] You are back at your camp (%s).", checkpoint::kind_name(kind));
	// Lua: contusion, rank; the money is the host's (death::host_take_money), Lua only tells it. The fifth argument is the number
	// of the fall, so that Lua acts once on a repeated order and on every real fall, however soon after the previous one. The
	// numbers belong to one actor (forget_local starts them again): Lua forgets the ones it has seen when a world is entered.
	script::queue_callback("coop_respawned", {script::cb_str(checkpoint::kind_name(kind)),
		script::cb_int(s64(g_rules.contusion_s)), script::cb_int(s64(g_rules.money_pct)),
		script::cb_int(s64(g_rules.rank_pct)), script::cb_int(s64(seq))});
}

void apply_spectate(u16 seq)
{
	CActor* a = local_actor();
	if (!a || seq != g_me.seq || g_me.life == Life::Spectating)
	{
		++g_stat.stale_orders;
		return;
	}
	const u64 t = mgr().now_ms();
	g_me.life = Life::Spectating;
	g_hook_life.store(u8(Life::Spectating));
	g_me.since = t;
	g_me.spectate = kNoId;
	g_me.newchar_start = 0; // the hold for a new character starts over (armed once USE is released)
	g_me.newchar_armed = false;
	g_me.newchar_asked = 0;
	close_menus();
	hold_controls(a);
	set_cam(a, Cam::Spectate);
	++g_stat.spectates;
	feed_tr("st_coop_feed_hardcore_out", "Hardcore: your character is gone. Hold USE to start a new one.");
	send_my_state(t);
}

// The orders of the revive hold (coop_revive_hold.h): Reviving and Stopped reach the wounded's owner, Done the reviver.
void apply_hold_order(u8 op, Reader& r)
{
	switch (op)
	{
	case OpReviving:
	{
		revive::RevivingMsg m;
		if (!m.read(r) || m.target != my_host_id())
			return;
		if (g_me.life != Life::Wounded || m.seq != g_me.seq)
		{
			++g_stat.stale_orders;
			return;
		}
		if (g_me.lift.on_reviving(mgr().now_ms(), m.seq, m.reviver, m.reviver_peer, m.total_ms, m.held_ms))
			++g_stat.holds_shown;
		break;
	}
	case OpStopped:
	{
		revive::StoppedMsg m;
		if (!m.read(r) || m.target != my_host_id())
			return;
		g_me.lift.on_stopped(m.seq, m.reviver);
		break;
	}
	case OpDone:
	{
		revive::DoneMsg m;
		if (!m.read(r))
			return;
		if (g_me.sent_target == kNoId || m.target != g_me.sent_target || m.seq != g_me.sent_seq)
		{
			++g_stat.stale_orders;
			return;
		}
		const xr_string section = g_me.sent_item_sect;
		g_me.sent_target = kNoId;
		++g_stat.dones;
		const PeerId peer = peer_of_host_id(m.target);
		// Lua coop_revive_done(target_peer, medkit_section) on the reviver's machine: the revive went through
		script::queue_callback("coop_revive_done", {peer == kInvalidPeer ? script::cb_nil() : script::cb_int(s64(peer)),
			script::cb_str(section.empty() ? nullptr : section.c_str())});
		break;
	}
	default: break;
	}
}

void apply_order(u8 op, Reader& r)
{
	++g_stat.orders_in;
	if (op == OpReviving || op == OpStopped || op == OpDone)
	{
		apply_hold_order(op, r);
		return;
	}
	const u16 target = r.u16();
	if (op == OpRefused)
	{
		const u8 reason = r.u8();
		if (!r.ok())
			return;
		++g_stat.refused;
		{
			string256 head;
			xr_sprintf(head, tr_format("st_coop_feed_revive_refused", "Revive of %s refused", 's').c_str(),
			    nick_of(target).c_str());
			string512 shown, logged;
			xr_sprintf(shown, "%s: %s.", head, tr(refusal_id(reason), refusal_text(reason)).c_str());
			xr_sprintf(logged, "Revive of %s refused: %s.", nick_of(target).c_str(), refusal_text(reason));
			feed_line(shown, logged);
		}
		// A Begin the host refused: the hold cannot end well (it would be refused again), so it ends here; the host has
		// none open for it, nothing to cancel. After a refused Request the hold is over already: nothing happens.
		if (g_me.revive_target != kNoId && join::to_host(g_me.revive_target) == target)
			stop_revive_hold(false);
		if (g_me.sent_target == target)
			g_me.sent_target = kNoId; // the Request of my finished hold was refused: no Done will come
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
		// the reviver's peer and the medkit's section; a malformed tail does not cancel the revive
		PeerId reviver_peer = kInvalidPeer;
		std::string medkit;
		const PeerId p = r.u32();
		std::string s = r.str();
		if (r.ok() && s.size() <= 128)
		{
			reviver_peer = p;
			medkit = std::move(s);
		}
		apply_revive(seq, reviver, health, reviver_peer, medkit.c_str());
		break;
	}
	case OpRespawn:
	{
		const Fvector p = get_vec(r);
		const u8 kind = r.u8();
		if (!r.ok() || !_valid(p.x) || !_valid(p.y) || !_valid(p.z))
			break;
		const bool known = kind <= u8(checkpoint::Kind::Mod); // a newer kind: still a respawn, shown as "none"
		apply_respawn(seq, p, known ? checkpoint::Kind(kind) : checkpoint::Kind::None);
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
	case Life::Wounded:
		feed_tr_s("st_coop_feed_peer_down", "%s is down! Hold USE on them with a medkit to revive.", who.c_str());
		break;
	case Life::Dead: feed_tr_s("st_coop_feed_peer_died", "%s died.", who.c_str()); break;
	case Life::Spectating: feed_tr_s("st_coop_feed_peer_out", "%s is out of the run (hardcore).", who.c_str()); break;
	case Life::Alive:
		if (from == Life::Wounded)
			feed_tr_s("st_coop_feed_peer_up", "%s is back on their feet.", who.c_str());
		else if (from == Life::Respawning || from == Life::Dead)
			feed_tr_s("st_coop_feed_peer_respawned", "%s respawned.", who.c_str());
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
	p.bleed_end = life == Life::Wounded ? mgr().now_ms() + left : 0;
	p.updated = mgr().now_ms();
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


// After a decision, the lives of everybody present in the host's world (the host, the players that
// report, without the parked characters of players who left). Every one of them down for good: coop_group_wipe, once
// until somebody stands up again (host_on_state Alive).
void host_check_wipe()
{
	if (g_world_rules.mode == death::Mode::Rollback)
		return; // the group is on its way back, nothing to stay and watch
	std::vector<u8> lives;
	if (mgr().session().local()) // a dedicated host has no life of its own in the group
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
	// A player on another level has no object here. He comes back on his own level, from where his server entity
	// stands; the anchor is a place on the level the host knows him on, it means nothing there.
	const CSE_ALifeDynamicObject* away = !body && mode().decider_of(id) == Decider::Other && ai().get_alife() ?
	    ai().alife().objects().object(id, true) :
	    nullptr;
	if (!body && !away)
		return false;
	HostRec& h = g_host[id];
	h.ordered_seq = seq; // before the order: the owner's Respawning report clears it (the host player's too)
	Fvector pos;
	checkpoint::Kind kind = checkpoint::Kind::None;
	if (!checkpoints::host_respawn_point(id, body && h.has_anchor ? &h.anchor : nullptr, body ? body->Position() : away->o_Position, pos, kind))
		::Msg("! [coop] respawn of %s: no checkpoint store in this world, back where the body lies", nick_of(id).c_str());
	pos.y += kRespawnLift;
	::Msg("[coop] respawn of %s (fall #%u): %s at (%.1f, %.1f, %.1f)", nick_of(id).c_str(), seq,
	    checkpoint::kind_name(kind), pos.x, pos.y, pos.z);
	Writer w(24);
	w.u8(OpRespawn);
	w.u16(id);
	w.u16(seq);
	put_vec(w, pos);
	w.u8(u8(kind));
	host_send_order(id, w);
	return true;
}

// Checkpoint death: the items (coop_death, by coop_death_drop) and the money go now, the owner is evacuated for
// coop_respawn_delay, host_frame respawns him.
void host_take_losses_and_evacuate(u16 id, u16 seq)
{
	Writer w(8);
	w.u8(OpLosses);
	w.u16(id);
	w.u16(seq);
	w.u8(u8(g_world_rules.drop));
	w.u8(g_world_rules.share_pct);
	u32 items = 0;
	if (!mode().to_level_of(peer_of_character(id), Msg::PlayerRevive, w, Lane::Reliable))
		items = death::host_apply_losses(id, seq, g_world_rules.drop, g_world_rules.share_pct);
	const u32 money = death::host_take_money(id, g_world_rules.money_pct);
	::Msg("[coop] death of %s (fall #%u): %u item(s) (%s, share %u%%), %u RU; respawn in %u s", nick_of(id).c_str(), seq,
	    items, death::drop_name(g_world_rules.drop), u32(g_world_rules.share_pct), money, g_world_rules.delay_ms / 1000);
	host_order_evacuate(id, seq, g_world_rules.delay_ms);
}

// The host's answer to a death (death::Tracker: once per death). Checkpoint: host_take_losses_and_evacuate. Hardcore: he
// is out and watches. Rollback: nothing is taken, the group goes back to the last save after kRollbackDelayMs, the delay
// counted here because the script's time events may stand while the host is dead. The host player (character 0) takes
// the same path: his orders go to him over the internal channel.
death::Action host_on_died(u16 id, u16 seq)
{
	HostRec& h = g_host[id];
	const death::Action a = death::on_died(h.tracker, seq, mgr().now_ms(), g_world_rules);
	switch (a)
	{
	case death::Action::None: return a; // decided already (state repeats are normal)
	case death::Action::Spectate: host_order_spectate(id, seq); break;
	case death::Action::LossesAndEvacuate: host_take_losses_and_evacuate(id, seq); break;
	case death::Action::Rollback:
		::Msg("[coop] death of %s (fall #%u): the group goes back to the last save in %u s", nick_of(id).c_str(), seq,
		    kRollbackDelayMs / 1000);
	{
		const bool first = g_rollback_at == 0;
		if (first)
			g_rollback_at = mgr().now_ms() + kRollbackDelayMs;
		script::queue_callback("coop_group_rollback",
		    {script::cb_int(id), script::cb_int(seq), script::cb_str(nick_of(id).c_str()), script::cb_int(kRollbackDelayMs / 1000),
		        script::cb_int(first ? 1 : 0)});
		break;
	}
	}
	host_check_wipe();
	return a;
}

// coop.rollback_failed: no save to go back to, the death goes the checkpoint way.
void host_rollback_failed(u16 id, u16 seq)
{
	HostRec& h = g_host[id];
	if (!death::rollback_refused(h.tracker, seq, mgr().now_ms(), g_world_rules))
		return;
	host_take_losses_and_evacuate(id, seq);
	flag_write(id, kDebtKey, true);
	h.debt = true;
}

// Every life report of a character's owner (the host player's too, over the internal channel). The respawn debt follows
// the report (death::debt_after_report) and is kept in the world, and so is "out of a hardcore run"; an Alive report is
// answered by death::on_alive_report: a respawn owed after a leave or a world change, a hardcore spectator sent back to
// watching until he starts a new character.

void host_on_state(u16 id, Life life, u16 seq)
{
	HostRec& h = g_host[id];
	if (life != Life::Wounded)
		h.hold.close(); // nobody holds him any more (stood up, or bled out)
	// the world's word on "out" first; a death of an out character then costs nothing again
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
		// The host player's new life was ordered (not out of the run, no retirement under way) and he still
		// reports Spectating: the order did not reach him (refused, or dropped with a world). Nothing else would heal it
		// (his NewCharacter is refused now: "not out"): it goes again (death::new_life_lost: at once in a new record of
		// him, else every kNewCharRetryMs; it names no fall, so it cannot move him once he stands).
		if (id == 0 &&
		    death::new_life_lost(u8(life), h.tracker.hardcore_out, g_retire.find(id) != g_retire.end(), h.new_life, mgr().now_ms(),
		        kNewCharRetryMs))
		{
			::Msg("! [coop] %s still watches though his new character was ordered: the order goes again",
			    nick_of(id).c_str());
			if (!host_send_new_life(id))
				::Msg("! [coop] the new character of %s waits: his actor is not here", nick_of(id).c_str());
		}
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

// The host's answer to the reviver's peer: a client over the net, the host player over the internal channel.
void host_answer(PeerId reply_to, const Writer& w) { net::host_send(reply_to, Msg::PlayerRevive, w); }

void host_refuse(PeerId reply_to, u16 target, u8 reason)
{
	Writer w(8);
	w.u8(OpRefused);
	w.u16(target);
	w.u8(reason);
	host_answer(reply_to, w);
}

// The host's check of a revive, for a Begin and for a Request alike (revive::verdict keeps the order of the checks):
// what is found in the world, `busy` from the caller. `item_obj` is the medkit when it is the reviver's own.
Refusal host_revive_verdict(u16 reviver, u16 target, u16 seq, u16 item, bool busy, CGameObject*& item_obj)
{
	CGameObject* r_obj = object_here(reviver);
	CGameObject* t_obj = object_here(target);
	item_obj = object_here(item);
	revive::Facts f;
	f.self = reviver == target;
	f.target_wounded = life_by_host_id(target) == Life::Wounded && seq_by_host_id(target) == seq;
	f.reviver_down = is_down(life_by_host_id(reviver));
	f.busy = busy;
	f.in_range = r_obj && t_obj && r_obj->Position().distance_to(t_obj->Position()) <= kHostReviveRange;
	f.has_item = item_obj && item_obj->H_Parent() && item_obj->H_Parent()->ID() == reviver &&
	    revive_rank(item_obj->cNameSect().c_str()) >= 0;
	return revive::verdict(f);
}

// The wounded's owner is told the hold on him ended without a revive (the host himself: applied at once).
void host_tell_stopped(u16 target, u16 seq, u16 reviver)
{
	revive::StoppedMsg m;
	m.target = target;
	m.seq = seq;
	m.reviver = reviver;
	Writer w(8);
	m.write(w);
	host_send_order(target, w);
}

// A record of `id` if the host has one (a request about a character nobody knows does not make one).
const HostRec* host_rec_if(u16 id)
{
	const auto it = g_host.find(id);
	return it == g_host.end() ? nullptr : &it->second;
}

// The `busy` of a Begin and of a Request alike: a revive of this fall is in order already, or another reviver's hold (his
// Begin's lock) is on the wounded. The reviver's own lock does not turn him away.
bool host_revive_busy(const HostRec* rec, u64 t, u16 seq, u16 reviver)
{
	return rec && (t < rec->revive_pending_until || rec->hold.busy_for(t, seq, reviver));
}

void host_on_revive_request(u16 reviver, PeerId reply_to, u16 target, u16 seq, u16 item)
{
	++g_stat.requests_in;
	const u64 t = mgr().now_ms();
	const HostRec* known = host_rec_if(target);
	CGameObject* i_obj = nullptr;
	// a Request meets the lock of another reviver's Begin, as the header promises
	const Refusal reason = host_revive_verdict(reviver, target, seq, item, host_revive_busy(known, t, seq, reviver), i_obj);

	if (reason != RefNone)
	{
		::Msg("[coop] revive of %u by %u refused: %s", target, reviver, refusal_text(reason));
		host_refuse(reply_to, target, reason);
		if (known)
		{
			HostRec& h = g_host[target];
			if (h.hold.stop(t, seq, reviver)) // the ring on the wounded ends: his Request was no good
				host_tell_stopped(target, seq, reviver);
		}
		return;
	}

	HostRec& h = g_host[target];
	h.hold.close(); // the wounded stands up and loses the ring with the Revive
	i_obj->DestroyObject(); // the medkit is a world item: only the host removes it, clients follow
	h.revive_pending_until = t + kRevivePendingMs;
	::Msg("[coop] %s revives %s with %s", nick_of(reviver).c_str(), nick_of(target).c_str(), i_obj->cNameSect().c_str());
	// u32 reviver peer, str medkit section -> Lua coop_actor_revived.
	Writer w(48);
	w.u8(OpRevive);
	w.u16(target);
	w.u16(seq);
	w.u16(reviver);
	w.f32(g_world_rules.revive_health);
	w.u32(peer_of_character(reviver));
	w.str(i_obj->cNameSect().c_str());
	host_send_order(target, w);
	// the reviver is told it went through (Lua coop_revive_done: the sound, the flash)
	revive::DoneMsg done;
	done.target = target;
	done.seq = seq;
	Writer d(8);
	done.write(d);
	host_answer(reply_to, d);
}

// A reviver's hold has gone on for a while (Begin). The same checks as a Request; a hold that passes is told
// to the wounded's owner with how long it was held, so his ring fills in the reviver's colour in step with the reviver's.
void host_on_revive_begin(u16 reviver, PeerId reply_to, const revive::BeginMsg& m)
{
	++g_stat.begins_in;
	const u64 t = mgr().now_ms();
	const HostRec* known = host_rec_if(m.target);
	CGameObject* i_obj = nullptr;
	const Refusal reason =
	    host_revive_verdict(reviver, m.target, m.seq, m.item, host_revive_busy(known, t, m.seq, reviver), i_obj);
	if (reason != RefNone)
	{
		++g_stat.begins_refused;
		host_refuse(reply_to, m.target, reason);
		return;
	}
	HostRec& h = g_host[m.target];
	// the lock of the same reviver is not renewed for ever (HostHold: the chain's end), a Begin
	// closer than the pace or just after his chain ran out is Busy, and the wounded hears nothing of it
	if (h.hold.begin(t, m.seq, reviver, m.held_ms) == revive::HostHold::Begin::Busy)
	{
		++g_stat.begins_refused;
		host_refuse(reply_to, m.target, RefBusy);
		return;
	}
	revive::RevivingMsg out;
	out.target = m.target;
	out.seq = m.seq;
	out.reviver = reviver;
	out.reviver_peer = peer_of_character(reviver);
	out.total_ms = revive::kHoldMs;
	out.held_ms = m.held_ms;
	Writer w(24);
	out.write(w);
	host_send_order(m.target, w);
}

// The reviver let go, went away or fell: the ring on the wounded ends if this reviver's hold was the one on him.
void host_on_revive_cancel(u16 reviver, u16 target, u16 seq)
{
	if (!host_rec_if(target))
		return;
	if (g_host[target].hold.stop(mgr().now_ms(), seq, reviver))
		host_tell_stopped(target, seq, reviver);
}

// A reviver went away in the middle of a hold. The lock he held on anybody is dropped and that
// wounded's owner is told, as for his Cancel (the lock and the ring would hang for up to kHoldMs + kGraceMs otherwise).
void host_drop_reviver(u16 reviver)
{
	struct Dropped
	{
		u16 target, seq;
	};
	xr_vector<Dropped> dropped; // told after the walk: the orders may touch the records
	for (auto& it : g_host)
		if (it.second.hold.drop_reviver(reviver))
			dropped.push_back({it.first, it.second.hold.seq()});
	for (const Dropped& d : dropped)
		host_tell_stopped(d.target, d.seq, reviver);
}

void host_broadcast_rules()
{
	Writer w(16);
	g_world_rules.write(w);
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
		// from 0). Whoever was dead or evacuated when this world was saved carries his respawn debt in
		// it (flag_read) and is put on his checkpoint at his first Alive report here; the losses were taken already.
		// Whoever was out of a hardcore run carries that too (kOutKey) and watches again. A retirement of
		// the old world is void (its stash is released empty, death::abandon_stashes): he may ask again here.
		g_epoch = epoch;
		g_host.clear();
		const u32 dropped = u32(g_retire.size());
		g_retire.clear();
		if (dropped)
			::Msg("[coop] a new world: %u retirement(s) of the old one dropped", dropped);
		g_wipe_fired = false;
		g_rollback_at = 0;
	}

	if (g_rollback_at)
	{
		autosave::suspend(true); // a save of the dead would bring the death back
		if (t >= g_rollback_at)
		{
			g_rollback_at = 0;
			script::queue_callback("coop_group_rollback_go", {});
		}
	}

	const death::Rules r = console_rules();
	if (!(r == g_world_rules))
	{
		const bool mode_changed = r.mode != g_world_rules.mode;
		g_world_rules = r;
		host_broadcast_rules();
		if (mode_changed)
			feed_tr_s("st_coop_feed_death_mode", "Death mode: %s.", tr(mode_id(r.mode), mode_name(r.mode)).c_str());
	}

	// players that left: their characters are parked. Dead or evacuated: the countdown stops and the respawn waits for
	// their return (death::on_left); dead with the respawn order already out but not applied: the debt
	// in the world brings it on the return. An order of the fall he left at is void. And a lock
	// he held on a wounded in the middle of a hold goes too, the wounded's owner told (host_drop_reviver).
	xr_vector<u16> departed;
	for (auto it = g_players.begin(); it != g_players.end();)
	{
		if (world::is_parked(it->first) || join::peer_of_proxy(it->first) == kInvalidPeer)
		{
			HostRec& h = g_host[it->first];
			death::on_left(h.tracker); // hardcore_out stays: leaving does not bring a dead player back
			h.ordered_seq = death::kNoSeq;
			h.revive_pending_until = 0;
			h.hold.close();
			departed.push_back(it->first);
			it = g_players.erase(it);
		}
		else
			++it;
	}
	for (const u16 id : departed)
		host_drop_reviver(id);

	host_retire_frame(t); // retirements whose stash is done

	// evacuations that are over (orders after the walk: an order may touch the records)
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

// Host: the slot / belt tail of a client's Dead report (death::read_worn: at most kMaxWornIds, a short
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

// A map server takes the tunnel's copy, in step with the player's stream. Only the world decides and relays it.
void map_on_state(PeerId from, Reader& r)
{
	r.u16(); // the claimed id is unused
	const u8 life = r.u8();
	const u16 seq = r.u16();
	const u32 left = r.u32();
	const u64 nick_bytes = r.varu();
	if (!r.ok() || life > u8(Life::Respawning) || nick_bytes > Writer::kMaxString || !r.skip(size_t(nick_bytes)))
		return; // skip the wire nickname without allocating it; the worn list can follow
	const u16 own = character_of_peer(from);
	if (own == kNoId)
		return;
	const PeerInfo* p = mgr().session().peer(from);
	if (Life(life) == Life::Dead)
		host_read_worn(own, seq, r); // the losses are taken here on the world server's word (OpLosses)
	// The session supplies the name, and death::Rules caps bleed-out at 600 s.
	apply_remote_state(own, Life(life), seq, std::min(left, u32(600000)), p ? p->nick.c_str() : "?");
}

// The world decider: a player's life report (the host player's comes over the internal channel from peer 0). His
// character is the one of his peer, never the id he claims; the report is decided on (host_on_state) and relayed to the
// others. A remote player's record is kept here (apply_remote_state: the world's view, which is the host player's view of
// him too: one process); the host player's own life is g_me, which his report comes from.
void server_on_state(PeerId from, Reader& r)
{
	r.u16(); // the id a player claims: never trusted
	const u8 life = r.u8();
	const u16 seq = r.u16();
	const u32 left = r.u32();
	const std::string nick = r.str();
	if (!r.ok() || life > u8(Life::Respawning))
		return;
	if (!mode().decides_world())
		return;
	const u16 id = character_of_peer(from);
	if (id == kNoId)
		return;
	const PeerInfo* p = mgr().session().peer(from);
	const xr_string name = p ? p->nick.c_str() : nick.c_str();
	if (Life(life) == Life::Dead)
		host_read_worn(id, seq, r); // before the decision: the losses of this death read it
	if (id != 0)
		apply_remote_state(id, Life(life), seq, left, name.c_str());
	host_on_state(id, Life(life), seq);
	Writer w(32);
	write_state(w, id, Life(life), seq, left, name.c_str());
	net::host_broadcast(Msg::PlayerWounded, w, Lane::Reliable, from);
}

// A client: the world server's relay of another player's life. In the host's process the world server applied
// it already (server_on_state: one record of every player in the process), so the host player's copy is not applied
// twice: only a remote client (a mirror) takes it.
void client_on_state(PeerId from, Reader& r)
{
	const u16 id = r.u16();
	const u8 life = r.u8();
	const u16 seq = r.u16();
	const u32 left = r.u32();
	const std::string nick = r.str();
	if (!r.ok() || life > u8(Life::Respawning))
		return;
	if (!mgr().is_live_client() || from != kHostPeer || id == my_host_id())
		return; // our own state is ours
	apply_remote_state(id, Life(life), seq, left, nick.c_str());
}

void map_on_state_relay(PeerId from, Reader& r)
{
	Reader header = r;
	const u16 id = header.u16();
	if (header.ok() && peer_of_character(id) != kInvalidPeer)
		return; // a player of this level: his own report came through the tunnel
	client_on_state(from, r);
}

// The world server: a reviver's request, hold or cancel (the host player's over the internal channel).
void server_on_revive(PeerId from, Reader& r)
{
	const u8 op = r.u8();
	if (!r.ok() || (!mode().decides_world() && !mode().leads_level()) || !revive::is_client_op(op))
		return; // orders come only from the world server
	// Peer 0 still goes through character lookup, which requires an attached local-player link.
	if (mode().decides_world() && from != kHostPeer && mode().where(from) == Where::Away)
		return; // the bodies stand on a map server's level: its copy is judged there
	const u16 reviver = character_of_peer(from);
	if (reviver == kNoId)
		return;
	switch (op)
	{
	case OpRequest:
	{
		const u16 target = r.u16();
		const u16 seq = r.u16();
		const u16 item = r.u16();
		if (!r.ok())
			return;
		host_on_revive_request(reviver, from, target, seq, item);
		break;
	}
	case OpBegin:
	{
		revive::BeginMsg m;
		if (m.read(r))
			host_on_revive_begin(reviver, from, m);
		break;
	}
	case OpCancel:
	{
		revive::CancelMsg m;
		if (m.read(r))
			host_on_revive_cancel(reviver, m.target, m.seq);
		break;
	}
	default: break;
	}
}

// A player: the world server's order to his owner (the host player takes his over the internal channel).
void client_on_revive(PeerId from, Reader& r)
{
	const u8 op = r.u8();
	if (op == OpLosses || op == OpStash)
		return; // these orders are for the process leading the body
	if (r.ok() && live() && from == kHostPeer && !revive::is_client_op(op))
		apply_order(op, r);
}

void map_on_revive(PeerId from, Reader& r)
{
	Reader body = r;
	const u8 op = body.u8();
	if (!body.ok() || from != kHostPeer)
		return;
	if (op == OpLosses)
	{
		const u16 character = body.u16();
		const u16 seq = body.u16();
		const u8 drop = body.u8();
		const u8 share = body.u8();
		if (body.ok() && drop <= u8(death::Drop::Keep))
			death::host_apply_losses(character, seq, death::Drop(drop), share);
		return;
	}
	if (op == OpStash)
	{
		const u16 character = body.u16();
		if (body.ok() && std::find(g_stashing.begin(), g_stashing.end(), character) == g_stashing.end())
		{
			death::host_move_to_stash(character, true);
			g_stashing.push_back(character);
		}
		return;
	}
	client_on_revive(from, r);
}


// The map server uses the world rules when it judges interactions on its level.
void map_on_rules(PeerId from, Reader& r)
{
	if (from != kHostPeer)
		return;
	death::Rules rules = g_world_rules;
	if (!rules.read(r))
	{
		::Msg("! [coop] map server: malformed PlayerRules ignored, the rules stay as they were");
		return;
	}
	static bool told = false; // the host sends them every few seconds
	const bool changed = !told || !(rules == g_world_rules);
	told = true;
	g_world_rules = rules;
	g_coop_friendly_fire = rules.friendly_fire ? 1 : 0; // no game record here: its console is the machine's
	npc_ai::set_finish_downed(rules.finish_downed);
	if (changed)
		::Msg("[coop] map server: the world's rules: friendly fire %s, stalkers finish a downed player %s",
		    rules.friendly_fire ? "on" : "off", rules.finish_downed ? "on" : "off");
}

// A player: the world decider's rules (the host player takes them over the internal channel like a client).
void on_rules_msg(PeerId from, Reader& r)
{
	if (!is_player() || from != kHostPeer)
		return;
	death::Rules rules = g_rules;
	if (!rules.read(r))
		return;
	if (!g_rules_from_host || !(rules == g_rules))
		::Msg("[coop] host survival rules: death mode %s, drop %s (share %u%%), bleed-out %u s, revive health %.2f, "
		      "respawn delay %u s, contusion %u s, money %u%%, wear %u%%, rank %u%%",
		    mode_name(rules.mode), death::drop_name(rules.drop), u32(rules.share_pct), rules.bleedout_ms / 1000,
		    rules.revive_health, rules.delay_ms / 1000, rules.contusion_s, u32(rules.money_pct), u32(rules.wear_pct),
		    u32(rules.rank_pct));
	g_rules = rules;
	g_rules_from_host = true;
}

// ---------------------------------------------------------------- per-frame parts

// The reviver's requests go to the world server: the host player's over the internal channel like a client's.
void send_revive_request(u16 target_host, u16 seq, u16 item)
{
	Writer w(8);
	w.u8(OpRequest);
	w.u16(target_host);
	w.u16(seq);
	w.u16(item);
	net::client_send(Msg::PlayerRevive, w);
}

// The hold goes on long enough to be told to the host (and through it to the wounded's owner), and ends
// without a revive (Cancel).
void send_revive_begin(u16 target_host, u16 seq, u16 item, u32 held_ms)
{
	revive::BeginMsg m;
	m.target = target_host;
	m.seq = seq;
	m.item = item;
	m.held_ms = u16(_min(held_ms, u32(0xffff)));
	Writer w(12);
	m.write(w);
	net::client_send(Msg::PlayerRevive, w);
}

void send_revive_cancel(u16 target_host, u16 seq)
{
	revive::CancelMsg m;
	m.target = target_host;
	m.seq = seq;
	Writer w(8);
	m.write(w);
	net::client_send(Msg::PlayerRevive, w);
}

// The revive hold on a teammate ends without a revive (let go, too far, fell, refused): the host is told when it knew of
// the hold (`notify`: not when the host itself refused the Begin, it has no hold open then).
void stop_revive_hold(bool notify)
{
	const bool cancel = g_me.announce.stop() == revive::Announcer::Act::Cancel;
	const u16 target = g_me.revive_target == kNoId ? kNoId : join::to_host(g_me.revive_target);
	g_me.revive_target = kNoId;
	if (cancel && notify && target != kNoId)
		send_revive_cancel(target, g_me.revive_seq);
}

void reviver_frame(CActor* a, u64 t)
{
	if (g_me.revive_target == kNoId)
		return;
	const u16 target = g_me.revive_target;
	CGameObject* o = object_here(target);
	const char* cancel = nullptr;
	const char* cancel_id = "";
	if (!a || is_down(g_me.life) || !o)
		cancel = "";
	else if (life_of(target) != Life::Wounded || seq_by_host_id(join::to_host(target)) != g_me.revive_seq)
		cancel = "";
	else if (o->Position().distance_to(a->Position()) > kReviveRange)
	{
		cancel = "too far";
		cancel_id = "st_coop_cancel_too_far";
	}
	else if (!action_down(kUSE))
	{
		cancel = "released";
		cancel_id = "st_coop_cancel_released";
	}
	if (cancel)
	{
		if (*cancel)
			feed_tr_s("st_coop_feed_revive_cancel", "Revive cancelled (%s).", tr(cancel_id, cancel).c_str());
		stop_revive_hold(true);
		return;
	}
	const u32 held = u32(_min<u64>(t - g_me.revive_start, 0xffffffffu));
	if (g_me.announce.step(t, held) == revive::Announcer::Act::Begin)
		send_revive_begin(join::to_host(target), g_me.revive_seq, g_me.revive_item, held);
	if (held < kReviveHoldMs)
		return;
	g_me.announce.done(); // the Request below ends the hold on the host
	g_me.sent_target = join::to_host(target); // its Done answers this one
	g_me.sent_seq = g_me.revive_seq;
	g_me.sent_at = t;
	g_me.sent_item_sect = g_me.revive_item_sect;
	g_me.revive_target = kNoId;
	send_revive_request(join::to_host(target), g_me.revive_seq, g_me.revive_item);
}

void local_frame(CActor* a, u64 t)
{
	if (g_hook_fell.exchange(false) && g_me.life == Life::Alive)
		enter_wounded(a, t);

	// While another player raises me, my time stops running for as long as I am being raised. The time is mine to keep in both
	// modes (the host relays the hold, LiftView); it is moved on before the
	// damage and the giving up below, so those still end it. The others see what is left from my reports: one a second
	// while I am raised (the host's roster refresh alone would let their count run down for kStateRefreshMs).
	if (g_me.life == Life::Wounded)
	{
		g_me.bleed_end = g_me.bleed_pause.step(t, g_me.lift.active(t, g_me.seq), g_me.bleed_end);
		if (g_me.bleed_pause.on() && t - g_me.last_sent >= kRaisedRefreshMs)
			send_my_state(t);
	}
	else
		g_me.bleed_pause.clear();

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

// Players' characters must never lose health on this machine: each owner computes its own damage
// (proxies on the host and avatars on clients are puppets, made unharmable when they are made; this keeps it so).
void guard_frame(u64 t)
{
	if (t - g_last_guard < kGuardMs)
		return;
	g_last_guard = t;
	each_player([](PeerId, u16 host_id, u16 local_id, const char* nick)
	{
		CEntityAlive* e = smart_cast<CEntityAlive*>(object_here(local_id));
		if (!e)
			return;
		if (e->conditions().CanBeHarmed())
		{
			e->conditions().SetCanBeHarmedState(false);
			++g_stat.guard_fixes;
		}
		if (!e->g_Alive() && g_guard_logged.insert(local_id).second)
			::Msg("! [coop] character of player '%s' (%u) is dead on this machine: it must never die here", nick, host_id);
	});
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
	g_my_pose = Pose(); // the old actor's pose and twist are gone with it (never called into)
	g_pose_actor.store(nullptr);
	g_my_twist_off = false;
	g_players.clear();
}

void leave_session()
{
	CActor* a = local_actor();
	if (a && a == g_me.actor && is_down(g_me.life))
		stand_up(a, kDownHealth * 5.f, false, mgr().now_ms(), false); // session over: back on its feet, barely alive
	else if (a && a == g_me.actor)
		release_controls(a);
	forget_local();
	g_host.clear(); // the respawn debts stay in the world (custom data), not here
	g_retire.clear(); // a stash still waiting is released empty (death::reset); the character stays out
	g_stashing.clear();
	g_wipe_fired = false;
	g_rollback_at = 0;
	g_rules_from_host = false;
	g_hud.clear();
}

// The hold of USE from the console (out of a hardcore run).
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
		if (!request_new_character(mgr().now_ms()))
			::Msg("[coop] coop_new_character: asked %llu s ago, waiting for the host", (mgr().now_ms() - g_me.newchar_asked) / 1000);
	}
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "hardcore, out of the run: start a new character (as holding USE for 3 s)");
	}
};

#ifdef DEBUG
// A test command: debug builds only.
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
		health_ledger::coop_set_health(a->conditions(), 0.f); // the condition hook turns this into the wounded state
	}
	void Info(TInfo& I) override { xr_strcpy(I, "test: drop my health to zero (falls wounded in a co-op session)"); }
};
#endif
} // namespace

void host_send_rules(PeerId bridge)
{
	Writer w(16);
	g_world_rules.write(w);
	net::host_send(bridge, Msg::PlayerRules, w);
}

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
	each_player([&out](PeerId peer, u16 host_id, u16 local_id, const char* nick)
	{
		PlayerRef ref;
		ref.peer = peer;
		ref.host_id = host_id;
		ref.local_id = local_id;
		ref.nick = nick;
		out.push_back(ref);
	});
}

xr_string nick_of(u16 host_id)
{
	if (!mgr().active())
		return "?";
	if (host_id == my_host_id())
		return mgr().nick();
	if (mode().decides_world())
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

u16 character_of_peer(PeerId peer)
{
	if (LevelSide* level = mode().level())
		return level->level_character_of(peer);
	if (!mode().decides_world())
		return kNoId;
	if (peer == kHostPeer)
		return mgr().session().local() ? u16(0) : kNoId; // the host player, over the internal channel
	return join::proxy_of_peer(peer);
}

PeerId peer_of_character(u16 character)
{
	if (LevelSide* level = mode().level())
		return level->level_peer_of(character);
	if (!mode().decides_world() || character == kNoId)
		return kInvalidPeer;
	if (character == 0)
		return mgr().session().local() ? kHostPeer : kInvalidPeer;
	return join::peer_of_proxy(character);
}

bool out_of_run(u16 character) { return flag_read(character, kOutKey); }

bool player_in_world(u16 character)
{
	if (!mode().decides_world() || character == kNoId)
		return false;
	if (character == 0)
		return mgr().session().local() && g_pGameLevel && local_actor() && g_world_served == join::world_generation();
	return join::host_player_in_world(character);
}

bool camera_left_body()
{
	return g_me.cam == Cam::Spectate || g_me.cam == Cam::Overhead;
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
	const u64 t = mgr().now_ms();
	if (local_id == 0)
		return g_me.life == Life::Wounded && g_me.bleed_end > t ? u32(g_me.bleed_end - t) : 0;
	const Remote* p = remote(join::to_host(local_id));
	return p && p->life == Life::Wounded && p->bleed_end > t ? u32(p->bleed_end - t) : 0;
}

const char* revive_item_kind(const char* section)
{
	const int rank = revive_rank(section);
	return rank < 0 ? "" : kReviveItems[rank];
}

bool host_worn_items(u16 character, u16 seq, xr_vector<u16>& out)
{
	out.clear();
	if (!mode().decides_world() && !mode().leads_level())
		return false; // a map server takes the losses of its level and keeps the list of his Dead report (host_read_worn)
	const auto it = g_host.find(character);
	if (it == g_host.end() || !it->second.has_worn || it->second.worn_seq != seq)
		return false;
	out = it->second.worn;
	return true;
}

// The map server's mode (srv/coop_srv_mode.cpp, MapServerMode::install) installs these right after the Core's tables, on its
// virtual-player session and world endpoint respectively: they replace the Core's PlayerWounded / PlayerRevive / PlayerRules there.
void install_level_server(Session& s)
{
	s.on(Msg::PlayerWounded, &map_on_state);
}

void install_level_client(Endpoint& e)
{
	e.on(Msg::PlayerWounded, &map_on_state_relay);
	e.on(Msg::PlayerRevive, &map_on_revive);
	e.on(Msg::PlayerRules, &map_on_rules);
}

// PlayerWounded and PlayerRevive go both ways (Either): each side has its own function, so that the host
// player's copy on the internal channel never runs the world server's part.
// A map server's PlayerWounded is its mode's (install_level_server above).
void install_server(Session& s)
{
	s.on(Msg::PlayerWounded, &server_on_state);
	s.on(Msg::PlayerRevive, &server_on_revive);
	s.on(Msg::NewCharacter, &on_new_character_msg);
}

// A map server's three are its mode's (install_level_client above).
void install_client(Endpoint& e)
{
	e.on(Msg::PlayerWounded, &client_on_state);
	e.on(Msg::PlayerRevive, &client_on_revive);
	e.on(Msg::PlayerRules, &on_rules_msg);
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
	CMD4(CCC_Float, "coop_death_share", &g_cv_death_share, 0.f, 200.f); // % of the ltx loss shares
#ifdef DEBUG
	CMD1(CCC_CoopWoundMe, "coop_wound_me");
#endif
	CMD1(CCC_CoopNewCharacter, "coop_new_character");
}

void reset()
{
	end_spectate_fade();
	leave_session();
	g_rules = console_rules();
	g_world_rules = g_rules;
	g_epoch = 0xffffffff;
	g_world_served = 0;
	g_last_roster = g_last_guard = 0;
	g_feed.clear();
	g_guard_logged.clear();
}

void frame()
{
	PROF_EVENT("coop/coop_survival/frame");
	// join::frame runs before this one in Manager::frame and serves the world it finds (host_watch_world)
	g_world_served = mode().decides_world() && g_pGameLevel ? join::world_generation() : 0;
	if (!g_pGameLevel || !live())
	{
		if (g_me.actor || !g_players.empty() || !g_host.empty())
			leave_session();
		return;
	}
	const u64 t = mgr().now_ms();
	if (mode().decides_world())
		host_frame(t);
	else if (mode().level())
		level_stash_frame();
	if (!g_rules_from_host)
		g_rules = console_rules(); // until the world server's rules arrive (the host player's too, over the link)

	CActor* a = local_actor();
	if (a != g_me.actor)
	{
		forget_local(a);
		g_me.actor = a;
		if (a)
			send_my_state(t); // tell everybody this character is alive (rejoin, new world)
	}
	if (a)
	{
		local_frame(a, t);
		own_pose_frame(a); // my own body lies while I am down
	}
	reviver_frame(a, t);

	if (mgr().is_live_client())
		for (auto it = g_players.begin(); it != g_players.end();)
			it = t - it->second.updated > kStaleRemoteMs ? g_players.erase(it) : std::next(it);

	// Avatar poses also from here, so a missing drive_avatar hook only costs a one-frame flicker.
	each_player([](PeerId, u16, u16 local_id, const char*)
	{
		if (CAI_Stalker* av = smart_cast<CAI_Stalker*>(object_here(local_id)))
			avatar_pose_override(av);
	});

	guard_frame(t);
	build_hud(t);
}

void print_status()
{
	const death::Rules& shown = rules(); // the world server's on the host, the player's copy on a client
	::Msg("[coop]   survival: me %s (fall #%u), death mode %s%s, drop %s (share %u%%), bleed-out %u s, revive health "
	      "%.2f, respawn delay %u s, contusion %u s, money %u%%, wear %u%%, rank %u%%",
	    life_name(g_me.life), g_me.seq, mode_name(shown.mode),
	    mode().decides_world() || g_rules_from_host ? "" : " (host rules not received yet)", death::drop_name(shown.drop),
	    u32(shown.share_pct), shown.bleedout_ms / 1000, shown.revive_health, shown.delay_ms / 1000,
	    shown.contusion_s, u32(shown.money_pct), u32(shown.wear_pct), u32(shown.rank_pct));
	::Msg("[coop]   survival: falls %llu, deaths %llu, revived %llu, refused %llu, evacuated %llu, respawns %llu, "
	      "spectate %llu, wipes %llu",
	    g_stat.falls, g_stat.deaths, g_stat.revives, g_stat.refused, g_stat.evacuations, g_stat.respawns,
	    g_stat.spectates, g_stat.wipes);
	::Msg("[coop]   survival: states out %llu in %llu, orders in %llu, requests in %llu, stale orders %llu, kills "
	      "intercepted %llu, proxy kills refused %llu, unharmable fixes %llu, worn lists %llu (refused %llu)",
	    g_stat.states_out, g_stat.states_in, g_stat.orders_in, g_stat.requests_in, g_stat.stale_orders,
	    g_stat.kills_intercepted, g_stat.proxy_kills_refused, g_stat.guard_fixes, g_stat.worn_reports,
	    g_stat.worn_refused);
	::Msg("[coop]   survival: new characters asked %llu, refused %llu, retired %llu, cancelled %llu, %u retiring",
	    g_stat.newchar_asked, g_stat.newchar_refused, g_stat.retired, g_stat.retire_cancelled, u32(g_retire.size()));
	::Msg("[coop]   survival: revive holds: begins %llu (refused %llu), shown to me %llu, done %llu, Lua HUD %s",
	    g_stat.begins_in, g_stat.begins_refused, g_stat.holds_shown, g_stat.dones,
	    g_lua_polled && mgr().now_ms() - g_lua_polled < kLuaHudMs ? "polling" : "not polling");
	const u64 t = mgr().now_ms();
	for (const auto& it : g_players)
		::Msg("[coop]   player %u '%s': %s (fall #%u)%s", it.first, it.second.nick.c_str(), life_name(it.second.life),
		    it.second.seq, it.second.life == Life::Wounded && it.second.bleed_end > t ? ", bleeding out" : "");
	for (const auto& it : g_retire)
		::Msg("[coop]   host: character %u retires, %llu s, stash %u %s", it.first, (t - it.second.since) / 1000,
		    u32(it.second.stash), it.second.releasing ? "done, leaving the world" :
		    it.second.at_map_server && !it.second.answered ? "waits for the map server of his level" :
		    death::host_stash_pending(it.first) ? "waits for his things" : "is done");
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
			health_ledger::coop_set_health(c, kDownHealth);
		// One who is down takes no more dose: the actor stays in the world, and zones and artefacts went on adding to it, up
		// to a revive that kept it all. A respawn clears it in stand_up.
		c.SetRadiation(g_hook_rad);
		return;
	}
	if (h <= 0.f)
	{
		g_hook_rad = c.GetRadiation();
		health_ledger::coop_set_health(c, kDownHealth);
		g_hook_owner.store(&c);
		g_hook_life.store(u8(Life::Wounded)); // pinned from now on; frame() completes the fall
		g_hook_fell.store(true);
	}
}

bool suppress_death_effector() { return live(); }

bool ignores_hits()
{
	if (!live())
		return false;
	const Life l = g_me.life;
	return !death::hit_counts(l == Life::Dead || l == Life::Spectating || l == Life::Respawning, mgr().now_ms(), g_me.shield_until);
}

bool intercept_actor_kill(CEntity* victim, u16 killer_id)
{
	// a ghost is nobody: what the stock world does to "the actor" (an emission, a script) means nothing here
	if (victim && ghost_actor() && smart_cast<CActor*>(victim) == g_actor)
		return true;
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
		g_hook_rad = a->conditions().GetRadiation();
		health_ledger::coop_set_health(a->conditions(), kDownHealth);
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
		feed_tr_s("st_coop_feed_need_medkit", "You need a medkit to revive %s.", who.c_str());
		return true;
	}
	g_me.revive_target = o->ID();
	g_me.revive_seq = seq_by_host_id(join::to_host(o->ID()));
	g_me.revive_item = join::to_host(medkit->object().ID());
	g_me.revive_item_sect = medkit->object().cNameSect().c_str();
	g_me.revive_start = mgr().now_ms();
	g_me.announce.start();
	return true;
}

bool on_use_nearby()
{
	if (!live())
		return false;
	CActor* a = local_actor();
	if (!a || is_down(g_me.life))
		return false;
	static_assert(revive::kPickRange == kReviveRange, "the pick reaches as far as the hold keeps going");
	std::vector<revive::ReviveCandidate> candidates;
	each_player([a, &candidates](PeerId, u16 host_id, u16 local_id, const char*)
	{
		CGameObject* o = object_here(local_id);
		if (!o || o == a)
			return;
		const Fvector& pos = o->Position();
		candidates.push_back({local_id, {pos.x, pos.y, pos.z}, life_by_host_id(host_id) == Life::Wounded});
	});
	const Fvector& eye = Device.vCameraPosition;
	const Fvector& dir = Device.vCameraDirection;
	const Fvector& feet = a->Position();
	const u16 target = revive::pick_revive_target({eye.x, eye.y, eye.z}, {dir.x, dir.y, dir.z}, {feet.x, feet.y, feet.z},
	    candidates);
	if (target == revive::kNoTarget)
		return false;
	CInventoryOwner* person = smart_cast<CInventoryOwner*>(object_here(target));
	return person && on_use_person(person);
}

bool look_locked() { return live() && g_me.actor && death::look_locked(u8(g_me.life)); }

const death::Rules& rules() { return mode().decides_world() ? g_world_rules : g_rules; }

void rollback_failed(u16 character, u16 seq)
{
	if (mode().decides_world())
		host_rollback_failed(character, seq);
}

bool saves_open() { return mgr().active() && rules().mode == death::Mode::Rollback; }

bool rollback_pending() { return g_rollback_at != 0; }
} // namespace survival
} // namespace coop
