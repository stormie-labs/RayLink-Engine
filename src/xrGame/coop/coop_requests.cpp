#include "stdafx.h"
#include "pch_script.h"
#include "coop_requests.h"
#include "coop_events.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_entities.h"
#include "coop_alife.h"
#include "coop_trade.h"
#include "coop_items.h"
#include "coop_money.h"
#include "coop_dialog.h"
#include "coop_callgate.h" // F2 T12
#include "xrCoop/coop_ledger.h"
#include "xrCoop/coop_spawn.h"
#include "xrCoop/coop_validate.h"
#include "xrCoop/coop_hits.h"
#include "xrCoop/coop_moves.h" // Ruling F2-32 (F2 T12 round 1)
#include "xrCoop/coop_id_lease.h" // F2 T12b

#include <cstring>

#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "Entity.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "InventoryBox.h" // Ruling F2-32: box locks
#include "Hit.h"
#include "xrServer.h"
#include "ai/stalker/ai_stalker.h"
#include "CustomRocket.h"
#include "RocketLauncher.h"
#include "Explosive.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrMessages.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xrEngine/XR_IOConsole.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "script_hit.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "script_engine.h"

int g_coop_friendly_fire = 1;
int g_coop_client_hit_perks = 1;

namespace coop
{
namespace requests
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u16 kActor = 0; // the local actor on every machine; on the host also the host player
constexpr u8 kMoveLocal = 1; // already applied on the client (thrown missile, launched rocket)
constexpr u8 kMoveTrade = 2;
constexpr u8 kMovePos = 4; // the drop goes to `pos` exactly (drop at a chosen place, script teleport)
constexpr float kDropPosRange = 100.f;
constexpr float kReachRange = 15.f; // player character <-> item/container, with lag slack
constexpr float kMeleeRange = 100.f; // strike/wound hits (knife, thrown physics)
constexpr float kExplosionRange = 150.f;
constexpr float kFireRange = 1500.f;
constexpr float kMaxHitPower = 100.f;
constexpr float kMaxArmorPiercing = 100.f; // stock ammo k_ap: 0.02 .. 5
constexpr float kMaxImpulse = 10000.f;
constexpr u32 kResultTimeoutMs = 10000;
constexpr u32 kChildSpawnHoldMs = 3000;
constexpr u32 kLogBurst = 20;

u32 g_log_count = 0;
bool log_allowed() { return ++g_log_count <= kLogBurst || (g_log_count % 100) == 0; }

// R3 В5-В8: values a fair client never sends. Counted (coop_status), the first 10 of each kind logged.
enum Bad : u8
{
	BadBone,
	BadSection,
	BadNumber,
	BadSpawnState,
	BadMoveRange,
	BadUpgrade, // task 10b: an upgrade the next spawn of the item would be FATAL on
	BadCount
};
constexpr u64 kBadLogFirst = 10;
u64 g_bad[BadCount] = {};
bool note_bad(Bad kind) { return ++g_bad[kind] <= kBadLogFirst; }

// R3 В6: a section from a peer (or a script) is a printable name the host knows and that names an entity class:
// F_entity_Create is FATAL for a section without `class`.
bool spawnable_section(const char* section)
{
	return validate::resource_name_ok(section, spawn::kMaxSection) && pSettings->section_exist(section) &&
	    pSettings->line_exist(section, "class");
}

// F2 T12b round 2 (review N3): `clone` is a variant of `source` (lease::clone_family over their parent_section lines).
bool clone_variant(LPCSTR source, LPCSTR clone)
{
	if (!source || !clone || !pSettings->section_exist(source) || !pSettings->section_exist(clone))
		return false;
	const auto parent = [](LPCSTR sec) -> LPCSTR
	{
		if (!pSettings->line_exist(sec, "parent_section"))
			return "";
		const LPCSTR v = pSettings->r_string(sec, "parent_section");
		return v ? v : "";
	};
	return lease::clone_family(source, parent(source), clone, parent(clone));
}

// F2 T2 fix round 1 (Ruling F2-17): a section as spawn::missing_line asks about it (xrCoop/coop_spawn.h When). It never
// reads a missing line, nor a number or flag of an empty value (r_s32 / r_bool of one would itself be FATAL here).
struct SectionIni
{
	LPCSTR section;

	LPCSTR value(LPCSTR line) const
	{
		return pSettings->line_exist(section, line) ? pSettings->r_string(section, line) : nullptr;
	}
	bool has(LPCSTR line) const { return !!pSettings->line_exist(section, line); }
	long long number(LPCSTR line) const // as r_s32 / r_u32 read it
	{
		const LPCSTR v = value(line);
		return v ? atoi(v) : 0;
	}
	bool flag(LPCSTR line) const { return value(line) && !!pSettings->r_bool(section, line); }
	bool ref_exists(LPCSTR line) const
	{
		const LPCSTR v = value(line);
		return v && v[0] && pSettings->section_exist(v);
	}
	bool ref_has(LPCSTR line, LPCSTR other) const
	{
		return ref_exists(line) && !!pSettings->line_exist(value(line), other);
	}
};

// R3 В6 (review), F2 T2: the lines the constructors of `rule`'s server entity and game object (and their bases and
// Loads) read unchecked are there (xrCoop/coop_spawn.h missing_line). `section` passed spawnable_section.
spawn::Missing missing_item_line(const char* section, const spawn::ClassRule& rule)
{
	return spawn::missing_line(rule, SectionIni{section});
}

u16 to_host_id(u16 id) { return id == kNoId ? kNoId : join::to_host(id); }
u16 to_local_id(u16 id) { return id == kNoId ? kNoId : join::to_local(id); }

CGameObject* find_go(u16 id)
{
	if (id == kNoId || !g_pGameLevel)
		return nullptr;
	CGameObject* go = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return go && !go->getDestroy() ? go : nullptr;
}

CGameObject* valid_go(u16 id)
{
	CGameObject* go = find_go(id);
	return go && !go->object_removed() ? go : nullptr;
}

Fvector position_of(u16 id)
{
	if (CGameObject* go = find_go(id))
		return go->Position();
	if (CSE_Abstract* e = events::find_entity(id))
		return e->o_Position;
	return Fvector().set(0.f, 0.f, 0.f);
}

bool is_player(u16 id) { return id == kActor || world::is_player_proxy(id); }

bool is_container(u16 id)
{
	CSE_Abstract* e = events::find_entity(id);
	return smart_cast<CSE_ALifeTraderAbstract*>(e) || smart_cast<CSE_ALifeInventoryBox*>(e);
}

void inject_item_event(u16 type, u16 dest, u16 item)
{
	NET_Packet P;
	CGameObject::u_EventGen(P, type, dest);
	P.w_u16(item);
	CGameObject::u_EventSend(P);
}

// ---------------------------------------------------------------- hit on the wire

void write_hit(Writer& w, const SHit& h, u16 who, u16 weapon)
{
	w.u16(who);
	w.u16(weapon);
	const float dir[3] = {h.dir.x, h.dir.y, h.dir.z};
	w.vec3(dir);
	w.f32(h.power);
	w.u16(h.boneID);
	const float pb[3] = {h.p_in_bone_space.x, h.p_in_bone_space.y, h.p_in_bone_space.z};
	w.vec3(pb);
	w.f32(h.impulse);
	w.u8(h.aim_bullet ? 1 : 0);
	w.u16(u16(h.hit_type));
	w.f32(h.armor_piercing);
	w.u32(h.BulletID);
}

// whoID/weaponID stay in wire numbering.
void read_hit(Reader& r, SHit& h)
{
	h.whoID = r.u16();
	h.weaponID = r.u16();
	float v[3];
	r.vec3(v);
	h.dir.set(v[0], v[1], v[2]);
	h.power = r.f32();
	h.boneID = r.u16();
	r.vec3(v);
	h.p_in_bone_space.set(v[0], v[1], v[2]);
	h.impulse = r.f32();
	h.aim_bullet = r.u8() != 0;
	h.hit_type = ALife::EHitType(r.u16());
	h.armor_piercing = r.f32();
	h.BulletID = r.u32();
	h.PACKET_TYPE = GE_HIT;
	h.add_wound = false; // as the engine's own GE_HIT event path delivers it
}

// R3 В5: every number of a hit through coop::validate (the bone is checked against the target's skeleton).
bool hit_sane(const SHit& h)
{
	return h.hit_type >= ALife::eHitTypeBurn && h.hit_type < ALife::eHitTypeMax && validate::finite(h.power) &&
	    h.power >= 0.f && h.power <= kMaxHitPower && validate::finite(h.impulse) && _abs(h.impulse) <= kMaxImpulse &&
	    validate::vec_finite(h.dir) && validate::vec_finite(h.p_in_bone_space) &&
	    validate::finite(h.armor_piercing) && h.armor_piercing >= 0.f && h.armor_piercing <= kMaxArmorPiercing;
}

float hit_range(ALife::EHitType type)
{
	switch (type)
	{
	case ALife::eHitTypeWound:
	case ALife::eHitTypeWound_2:
	case ALife::eHitTypeStrike:
		return kMeleeRange;
	case ALife::eHitTypeExplosion:
		return kExplosionRange;
	default:
		return kFireRange;
	}
}

// R3 В2: who set off a hit on `victim` (a player's character or the host's actor), for the rules in coop_hits.h.
hits::Source hit_source(const CGameObject* victim, const CGameObject* who)
{
	if (!who)
		return hits::Source::Missing;
	if (who == victim)
		return hits::Source::Self;
	if (is_player(who->ID()))
		return hits::Source::Player;
	return smart_cast<const CEntity*>(who) ? hits::Source::Creature : hits::Source::Object;
}

// R3 В2: blast waves and fragments name the explosive as the weapon (CExplosive::Explode). Found while it is being
// destroyed too (net_Find, as the engine's own GE_HIT does): late fragments can land after it asked to be removed.
bool weapon_is_explosive(u16 weapon_id, const CGameObject* victim)
{
	if (weapon_id == kNoId || !g_pGameLevel)
		return false;
	CGameObject* weapon = smart_cast<CGameObject*>(Level().Objects.net_Find(weapon_id));
	return weapon && weapon != victim && weapon->cast_explosive();
}

// Damage for the local actor through CActor::Hit, exactly what the engine's GE_HIT event does
// (SetHitInfo + Hit): armour, artefacts, hit callbacks and HUD marks all apply. Synchronous, so the
// apply scope tells our own CActor::Hit hook that this hit is authorised.
void apply_to_actor(SHit hit, CObject* who, CObject* weapon)
{
	CActor* actor = Actor();
	if (!actor || !actor->g_Alive())
		return;
	if (!who)
		who = actor; // the source is not online here: keep the damage, lose the attribution
	hit.who = who;
	hit.whoID = who->ID();
	hit.weaponID = weapon ? weapon->ID() : hit.whoID;
	hit.DestID = actor->ID();
	hit.PACKET_TYPE = GE_HIT;
	hit.Time = Level().timeServer();
	// The bone is from the character model the hit landed on elsewhere (avatar/proxy); the engine
	// indexes this actor's bones with it unchecked (HitSignal, HitScale, wallmarks).
	IKinematics* k = smart_cast<IKinematics*>(actor->Visual());
	if (hit.boneID != BI_NONE && (!k || hit.boneID >= k->LL_BoneCount()))
		hit.boneID = k ? k->LL_GetBoneRoot() : u16(BI_NONE);
	entities::ClientApply scope;
	actor->SetHitInfo(who, weapon, s16(hit.boneID), hit.p_in_bone_space, hit.dir);
	actor->Hit(&hit);
}

// ---------------------------------------------------------------- client side

enum class Act : u8
{
	Spawn,
	Destroy,
	Use,
	Move,
	Hit,
	ItemEvent,
	Died,
};

struct Action
{
	Act kind = Act::Destroy;
	u16 item = kNoId, from = kNoId, to = kNoId; // local numbering
	Fvector pos{};
	u8 flags = 0;
	bool open = false; // a reject a take of the same item can still complete (box -> actor)
	u16 ge_type = 0;
	SHit hit;
	xr_vector<u8> bytes; // ItemEvent: event bytes; Spawn: item state (coop_items blob)
	xr_string section; // Spawn
	// ItemEvent (F2 T5): the upgrade's mechanic (local numbering, kNoId: none), what was paid for it, and the key of
	// that payment held out of the money shown until the host answers (money::client_hold, 0: none)
	u16 mechanic = kNoId;
	u32 paid = 0, hold = 0;
	// Spawn (F2 T12b): the leased id our item has here already (kNoId: the request path, nil to Lua), and the weapon a
	// clone of ours replaces (local numbering, kNoId: none)
	u16 lease = kNoId, source = kNoId;
};

struct Inflight
{
	Msg type;
	u16 item;
	u64 sent;
	u32 hold = 0; // F2 T5: released by the answer (or the timeout)
};

// The router may run on the game thread; sending happens in frame() on the main thread.
xrCriticalSection g_out_lock;
xr_vector<Action> g_outbox;
xr_map<xr_string, u64> g_child_spawns; // "section#holder" -> time asked (fake missiles, rockets)
xr_map<u32, Inflight> g_inflight;
u32 g_seq = 0;
xr_vector<CSE_Abstract*> g_lua_temps; // alife():create(..., false) results waiting for register()
xr_set<CSE_Abstract*> g_lua_forwarded;

struct ClientCounters
{
	u64 sent[16] = {};
	u64 accepted = 0, rejected = 0, timeouts = 0, not_live = 0, foreign_moves = 0, child_dups = 0, hits_in = 0;
	u64 lua_forwarded = 0, lua_refused = 0;
	u64 hits_perked = 0, hits_cancelled = 0, perk_errors = 0; // W5 C2.1
} g_cl;

// ---------------------------------------------------------------- F2 T12b: leased ids, client state
// Our own items are created here at once under ids the host lent us (xrCoop/coop_id_lease.h). Under g_out_lock: Lua
// creates may run on the game thread, the answers and the frame come on the main thread.
lease::ClientPool g_pool; // ids reserved in our generator, ready for a create
lease::SpecBook g_specs; // our creates the host has not answered yet
xr_map<u16, Action> g_spec_actions; // the request of each of them, sent again after RateLimited
struct LeasedTemp
{
	u16 id = kNoId;
	u16 source = kNoId; // a clone's weapon (local numbering)
};
xr_map<CSE_Abstract*, LeasedTemp> g_temp_leases; // alife():create / clone_weapon (.., false) objects that hold an id
xr_vector<u16> g_give_back; // ids for the next IdLeaseAck
xr_vector<u16> g_give_back_wait; // ids of refused creates whose object is still here (its destroy takes a frame)
u32 g_pool_world = 0; // join::world_generation() the ids are reserved in
u64 g_last_ack = 0;
// Round 1 (review C1): requests about our items the host has not created yet wait for its answer (lease::Held)
lease::Held<Action> g_held;
// Round 2 (review N1): our ReqSpawn go no faster than the host takes them; the rest waits here, in order
lease::SpawnPacer g_spawn_pacer;
xr_deque<Action> g_spawn_backlog;
struct LeaseCounters
{
	u64 created = 0, temps = 0, clones = 0, confirmed = 0, retried = 0, rolled_back = 0, released_with_clone = 0;
	u64 acks = 0, given_back = 0, bad_leases = 0, stale_leases = 0, not_created = 0;
	u64 held = 0, held_dropped = 0, clones_refused = 0, temps_left = 0, quarantine_left = 0; // round 1
	u64 queued = 0, resent = 0, unanswered = 0, paced = 0, not_variant = 0; // round 2
} g_lc;
// Round 1 (review I2): the world (join::world_generation) the temporary objects of g_lua_temps were made in
u32 g_temps_world = 0;

// Rolled-back items (research f2-client-create-ids.md §4.3): released from our ALife and server like any destroyed item
// (on_unregister cleans the mods' data under the id), but their entity is freed only kQuarantineMs later, as a script
// may still hold the se_obj it got from the create - and only in the world it was released in (round 1, review I2:
// lease::Quarantine).
constexpr u64 kQuarantineMs = 120000;
xrCriticalSection g_quarantine_lock; // entity_Destroy runs where the release runs
xr_set<CSE_Abstract*> g_quarantine_marks;
lease::Quarantine<CSE_Abstract*> g_quarantine;

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

// W5 C2.1: the engine's before-hit Lua entry for the victim's class (as CAI_Stalker::Hit / CBaseMonster::Hit call it).
LPCSTR before_hit_entry(CGameObject* victim)
{
	if (victim->cast_stalker())
		return "CAI_Stalker__BeforeHitCallback";
	if (victim->cast_base_monster())
		return "CBaseMonster__BeforeHitCallback";
	return nullptr;
}

// Main thread (client frame). Runs `_G.<entry>(victim, hit, bone)` like the engine does before it applies a hit:
// the Lua chain (npc_on_before_hit / monster_on_before_hit) may change power, impulse, direction and type, or
// return false to ignore the hit. Returns false if the hit must not be sent. A missing entry, a missing object or
// a Lua error leave the hit as it was (logged, never fatal: R-11.4).
bool client_hit_perks(u16 local_target, SHit& h)
{
	if (!g_coop_client_hit_perks || !g_pGameLevel || !g_actor)
		return true;
	if (is_player(local_target))
		return true; // another player: npc_on_before_hit never runs for players (their own actor_on_before_hit does)
	CGameObject* victim = valid_go(local_target);
	CEntityAlive* alive = victim ? victim->cast_entity_alive() : nullptr;
	if (!alive || !alive->g_Alive())
		return true;
	LPCSTR entry = before_hit_entry(victim);
	CGameObject* who = valid_go(h.whoID); // our actor or an item of ours (local numbering)
	lua_State* L = entry && who ? ai().script_engine().lua() : nullptr;
	if (!L || !lua_checkstack(L, 8))
		return true;
	SHit probe = h;
	probe.who = who;
	CScriptHit lua_hit(&probe);
	const int top = lua_gettop(L);
	lua_getglobal(L, entry);
	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, top);
		return true;
	}
	::luabind::object(L, victim->lua_game_object()).pushvalue();
	::luabind::object(L, &lua_hit).pushvalue();
	lua_pushnumber(L, lua_Number(h.boneID));
	if (lua_pcall(L, 3, 1, 0) != 0)
	{
		++g_cl.perk_errors;
		if (log_allowed())
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] lua error in %s for our hit on %u (sent unchanged): %s", entry, local_target, err ? err : "?");
		}
		lua_settop(L, top);
		return true;
	}
	const bool keep = lua_toboolean(L, -1) != 0;
	lua_settop(L, top);
	if (!keep)
	{
		++g_cl.hits_cancelled;
		return false;
	}
	// What SHit::ApplyScriptHit copies, minus the attribution: the host decides who and with what (its proxy and
	// an item of the proxy), so a script that renames the draftsman cannot blame someone else.
	if (_valid(lua_hit.m_fPower) && lua_hit.m_fPower >= 0.f)
		h.power = lua_hit.m_fPower;
	if (_valid(lua_hit.m_fImpulse))
		h.impulse = lua_hit.m_fImpulse;
	if (_valid(lua_hit.m_tDirection) && lua_hit.m_tDirection.square_magnitude() > EPS_S)
		h.dir = lua_hit.m_tDirection;
	if (lua_hit.m_tHitType >= int(ALife::eHitTypeBurn) && lua_hit.m_tHitType < int(ALife::eHitTypeMax))
		h.hit_type = ALife::EHitType(lua_hit.m_tHitType);
	++g_cl.hits_perked;
	return true;
}

void send_action(const Action& a)
{
	switch (a.kind)
	{
	case Act::Spawn:
	{
		const u32 seq = track(Msg::ReqSpawn, a.item);
		Writer w(a.bytes.size() + a.section.size() + 32);
		const float p[3] = {a.pos.x, a.pos.y, a.pos.z};
		if (a.lease != kNoId)
		{
			// F2 T12b: we have the item under this id already; the host creates it with the same one or refuses
			spawn::write_leased(w, seq, to_host_id(a.item), to_host_id(a.lease), to_host_id(a.source), p,
			    a.section.c_str(), a.bytes.data(), a.bytes.size());
			xrCriticalSectionGuard guard(g_out_lock);
			g_specs.sent(a.lease, seq, mgr().now_ms());
		}
		else // R3 В6: section + item state, the host builds the entity itself
			spawn::write_state(w, seq, to_host_id(a.item), p, a.section.c_str(), a.bytes.data(), a.bytes.size());
		net::client_send(Msg::ReqSpawn, w);
		++g_cl.sent[0];
		break;
	}
	case Act::Destroy:
	case Act::Use:
	{
		const Msg type = a.kind == Act::Use ? Msg::ReqUse : Msg::ReqDestroy;
		Writer w(8);
		w.u32(track(type, a.item));
		w.u16(to_host_id(a.item));
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
		Writer w(24);
		w.u32(track(type, a.item));
		w.u16(to_host_id(a.item));
		w.u16(to_host_id(a.from));
		w.u16(to_host_id(a.to));
		const float p[3] = {a.pos.x, a.pos.y, a.pos.z};
		w.vec3(p);
		w.u8(a.flags);
		net::client_send(type, w);
		++g_cl.sent[3];
		break;
	}
	case Act::Hit:
	{
		SHit hit = a.hit;
		if (!client_hit_perks(a.item, hit)) // W5 C2.1: our perks and skills, before the host sees the hit
			break;
		Writer w(64);
		w.u32(++g_seq); // the host answers hits only when it refuses them
		w.u16(to_host_id(a.item));
		const Fvector me = Actor() ? Actor()->Position() : Fvector().set(0.f, 0.f, 0.f);
		const float p[3] = {me.x, me.y, me.z};
		w.vec3(p);
		write_hit(w, hit, to_host_id(hit.whoID), to_host_id(hit.weaponID));
		net::client_send(Msg::ReqHit, w);
		++g_cl.sent[4];
		break;
	}
	case Act::ItemEvent:
	{
		// F2 T5: tracked, so that the host's answer releases the payment held out of the money shown
		const u32 seq = track(Msg::ReqItemEvent, a.item);
		g_inflight[seq].hold = a.hold;
		Writer w(a.bytes.size() + 16 + ledger::kUpgradePaymentSize);
		w.u32(seq);
		w.u16(to_host_id(a.item));
		w.u16(a.ge_type);
		w.u16(u16(a.bytes.size()));
		w.bytes(a.bytes.data(), a.bytes.size());
		ledger::write(w, ledger::UpgradePayment{to_host_id(a.mechanic), a.paid}); // F2 T5
		net::client_send(Msg::ReqItemEvent, w);
		++g_cl.sent[6];
		break;
	}
	case Act::Died:
	{
		Writer w(8);
		w.u16(to_host_id(kActor));
		w.u16(to_host_id(a.item));
		net::client_send(Msg::PlayerDied, w);
		++g_cl.sent[7];
		break;
	}
	}
}

// F2 T12b round 1 (review C1): the create of ours an action waits for - an item it names the host has not made yet (in
// its budget queue, or asked again after RateLimited): sent before it, the host would refuse the action (Missing) and
// make the item later, a duplicate. A leased spawn never waits for itself; a spawn into a pending item does. kNoId: none.
// Under g_out_lock.
u16 held_by_locked(const Action& a)
{
	switch (a.kind)
	{
	case Act::Spawn:
	case Act::Destroy:
	case Act::Use:
	case Act::ItemEvent: return g_specs.first_pending({a.item});
	case Act::Move: return g_specs.first_pending({a.item, a.from, a.to});
	case Act::Hit:
	case Act::Died: return kNoId;
	}
	return kNoId;
}

void client_flush()
{
	xr_vector<Action> out;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		out.swap(g_outbox);
		const u64 now = mgr().now_ms();
		for (auto it = g_child_spawns.begin(); it != g_child_spawns.end();)
			it = now - it->second > kChildSpawnHoldMs ? g_child_spawns.erase(it) : std::next(it);
		if (g_specs.size())
		{
			// F2 T12b round 1 (review C1): what names an item the host has not made yet waits for its answer
			xr_vector<Action> now_out;
			now_out.reserve(out.size());
			for (Action& a : out)
			{
				const u16 by = held_by_locked(a);
				if (by == kNoId)
					now_out.push_back(std::move(a));
				else
				{
					g_held.hold(by, std::move(a));
					++g_lc.held;
				}
			}
			out.swap(now_out);
		}
		// round 2 (review N1): spawns no faster than the host takes them (its CatSpawn bucket, the session's row that
		// drops what is over it without an answer); the others go now, the spawns keep their order
		xr_vector<Action> paced_out;
		paced_out.reserve(out.size());
		for (Action& a : out)
		{
			if (a.kind == Act::Spawn)
				g_spawn_backlog.push_back(std::move(a));
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
		if (now - it->second.sent > kResultTimeoutMs)
		{
			++g_cl.timeouts;
			money::client_release(it->second.hold); // F2 T5: the next balance from the host decides
			it = g_inflight.erase(it);
		}
		else
			++it;
	}
}

// ---------------------------------------------------------------- F2 T5 fix round 1: a refused upgrade (client)
// The host refused an upgrade this client installed on its item already (the engine installs before the request goes).
// An installed upgrade cannot leave a live object: its properties were added to the ones the item loaded, and a list is
// installed only on a fresh object (CInventoryItem::net_Spawn_install_upgrades). So the item is built again from the
// host's entity, which keeps the host's list: it is destroyed here as a host command (the local server does it,
// nothing goes to the host), and once it is gone it is asked for again with its children (StateDumpRequest; the
// host's EntitySpawn creates it in our inventory with the same id). Bounded: kMaxRebuilds at a time, a few tries.
constexpr size_t kMaxRebuilds = 16;
constexpr size_t kMaxRebuildChildren = 16;
constexpr u64 kRebuildDestroyMs = 5000; // the local destroy chain takes a frame or two
constexpr u64 kRebuildAskMs = 3000; // no EntitySpawn yet: ask again
constexpr u32 kRebuildTries = 3;

struct Rebuild
{
	u16 host_id = kNoId;
	xr_vector<u16> children; // host numbering
	u32 epoch = 0; // the world it belongs to
	u64 at = 0; // destroyed / last asked
	u32 asked = 0;
};
xr_map<u16, Rebuild> g_rebuilds; // local id -> (main thread)
u64 g_rebuilt = 0, g_rebuild_lost = 0;

void client_rebuild_item(u16 local_item)
{
	if (local_item == kNoId || local_item == kActor || g_rebuilds.size() >= kMaxRebuilds ||
	    g_rebuilds.count(local_item) || !mgr().is_live_client())
		return;
	CSE_Abstract* e = events::find_entity(local_item);
	CGameObject* go = find_go(local_item);
	if (!e || !go || go->getDestroy() || events::root_of(local_item) != kActor)
		return; // not ours, not online here: nothing of ours to correct
	Rebuild& rb = g_rebuilds[local_item];
	rb.host_id = to_host_id(local_item);
	for (u16 child : e->children)
		if (rb.children.size() < kMaxRebuildChildren)
			rb.children.push_back(to_host_id(child));
	rb.epoch = mgr().session().world_epoch();
	rb.at = mgr().now_ms();
	entities::ClientApply apply; // a host command: nothing goes to the host
	NET_Packet packet;
	packet.w_begin(M_EVENT);
	packet.w_u32(Level().timeServer());
	packet.w_u16(GE_DESTROY);
	packet.w_u16(local_item);
	Level().Send(packet, net_flags(TRUE, TRUE));
	++g_rebuilt;
	if (log_allowed())
		::Msg("[coop] the host refused an upgrade of item %u: it is built again from the host's copy", local_item);
}

void client_ask_rebuild(const Rebuild& rb)
{
	Writer w(4);
	w.u16(rb.host_id); // the holder before its children
	net::client_send(Msg::StateDumpRequest, w);
	for (u16 child : rb.children)
	{
		Writer wc(4);
		wc.u16(child);
		net::client_send(Msg::StateDumpRequest, wc);
	}
}

void client_pump_rebuilds()
{
	if (g_rebuilds.empty())
		return;
	const u64 now = mgr().now_ms();
	const u32 epoch = mgr().session().world_epoch();
	for (auto it = g_rebuilds.begin(); it != g_rebuilds.end();)
	{
		Rebuild& rb = it->second;
		const bool here = events::find_entity(it->first) != nullptr;
		if (rb.epoch != epoch || (rb.asked && here))
		{
			it = g_rebuilds.erase(it); // another world (it came with the host's state), or back from the host
			continue;
		}
		if (!rb.asked)
		{
			if (here)
			{
				if (now - rb.at > kRebuildDestroyMs)
				{
					++g_rebuild_lost; // the local destroy did not happen: keep it as it is
					it = g_rebuilds.erase(it);
					continue;
				}
				++it;
				continue;
			}
			client_ask_rebuild(rb); // gone here: the host's copy
			rb.asked = 1;
			rb.at = now;
		}
		else if (now - rb.at > kRebuildAskMs)
		{
			if (rb.asked >= kRebuildTries)
			{
				++g_rebuild_lost; // the host has it no more (or does not answer): its relays decide
				it = g_rebuilds.erase(it);
				continue;
			}
			client_ask_rebuild(rb);
			++rb.asked;
			rb.at = now;
		}
		++it;
	}
}

// ---------------------------------------------------------------- F2 T12b: leased ids, client
// The ids the host lent us (Msg::IdLease) are reserved in our generator at once; an own item a script creates
// (alife():create into our actor, create(.., false) + register, clone_weapon) takes one and is created here at once,
// registered and online in our inventory, and asked for with the same id (ReqSpawn 0xC7). The host's answer: accepted -
// nothing to do (its EntitySpawn finds the item here); RateLimited - asked again; refused - our item goes (rollback) and
// the id goes back (Msg::IdLeaseAck).

bool lease_world_ready() { return mgr().is_live_client() && g_pGameLevel && ai().get_alife() && Level().Server; }

// The ids belong to the generator of the world they were reserved in: another world here (the host's load, a rejoin of
// ours: join::world_generation) leaves nothing of them, nor of the creates in flight. Under g_out_lock.
void lease_sync_world_locked()
{
	const u32 world = join::world_generation();
	if (world == g_pool_world)
		return;
	g_pool_world = world;
	g_pool.clear();
	g_specs.clear();
	g_spec_actions.clear();
	g_give_back.clear();
	g_give_back_wait.clear();
	for (auto& it : g_temp_leases)
		it.first->ID = kNoId; // still a temporary object (g_lua_temps): its register takes the request path
	g_temp_leases.clear();
	g_held.clear([](Action&) {}); // requests about items of the old world (its money holds went with it)
	xrCriticalSectionGuard guard(g_quarantine_lock);
	g_quarantine_marks.clear(); // their entities went with the old world, or wait in the quarantine
}

// Round 1 (review I2): the temporary objects of an old world were made by its Lua VM, which the new world closed: freeing
// one now runs a luabind destructor in a closed lua_State. They are forgotten (a small leak), not freed.
void temps_sync_world()
{
	const u32 world = join::world_generation();
	if (world == g_temps_world)
		return;
	g_temps_world = world;
	g_lc.temps_left += g_lua_temps.size();
	g_lua_temps.clear();
	g_lua_forwarded.clear();
	xrCriticalSectionGuard guard(g_out_lock);
	g_temp_leases.clear(); // keys are those objects; their ids went with the old pool
}

// What the host's ReqSpawn takes into a player's character (an item class with every line it reads unchecked, not a
// launcher's rocket, no default upgrades its own tree refuses - FATAL on a new object here as there): anything else
// takes the request path and its refusal, as before.
bool lease_section_ok(const char* section)
{
	if (!section || !spawnable_section(section))
		return false;
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(section, "class"));
	return rule && spawn::character_takes(rule->game) && !missing_item_line(section, *rule).line &&
	    items::section_defaults_ok(shared_str(section));
}

// A leased id for a new item of `section` in `parent` (local numbering), kNoId for the request path: not live, not
// into our actor, a section the host refuses there, no id left.
u16 take_lease(const char* section, u16 parent)
{
	if (parent != kActor || !lease_world_ready() || !lease_section_ok(section))
		return kNoId;
	xrCriticalSectionGuard guard(g_out_lock);
	lease_sync_world_locked();
	return g_pool.take();
}

void give_back(u16 id)
{
	xrCriticalSectionGuard guard(g_out_lock);
	g_give_back.push_back(id);
}

// The name CALifeSimulatorBase::spawn_item gives a new object: its section and its id, at least 4 digits (the host
// names a leased item the same way).
void name_with_id(CSE_Abstract* e)
{
	string256 name;
	xr_sprintf(name, "%s%04u", e->s_name.c_str(), u32(e->ID));
	e->set_name_replace(name);
}

// Our holder is online, so the new item comes online in it at once: the steps of entities' bring_child_online for an
// item the host made (add_online_impl for a child).
void bring_online_in_holder(CSE_ALifeDynamicObject* e)
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

// `e` (a temporary object of make_temp, its parent our actor) becomes our item now under leased `id`: registered in our
// ALife, online in our inventory, asked for with the same id. `source`: the weapon a clone replaces (local numbering).
// False: not created (`e` untouched; the caller gives the id back).
bool create_leased(CSE_Abstract* e, u16 id, u16 source)
{
	CSE_ALifeDynamicObject* d = smart_cast<CSE_ALifeDynamicObject*>(e);
	CSE_ALifeDynamicObject* holder = lease_world_ready() ? ai().alife().objects().object(e->ID_Parent, true) : nullptr;
	xrServer* server = Level().Server;
	// the id is ours: reserved in our generator (never free) and nothing here holds it
	if (!d || !holder || !server || server->IsFreeID(id) || server->ID_to_entity(id) ||
	    ai().alife().objects().object(id, true))
	{
		++g_lc.not_created;
		if (log_allowed())
			::Msg("! [coop] our new [%s] could not take its leased id %u here: asked for without it",
			    e->s_name.c_str(), id);
		return false;
	}
	Action a;
	a.kind = Act::Spawn;
	a.item = e->ID_Parent;
	a.pos = holder->o_Position;
	a.section = e->s_name.c_str() ? e->s_name.c_str() : "";
	a.lease = id;
	a.source = source;
	std::vector<u8> state;
	if (items::capture_spawn_state(e, state)) // the state the script gave it, before our world touches it
		a.bytes.assign(state.begin(), state.end());
	{
		entities::ClientApply apply; // our own world takes it as the host's would: nothing of it is a request
		e->ID = id;
		name_with_id(e);
		e->ID_Phantom = kNoId;
		d->o_Position = holder->o_Position;
		d->m_tNodeID = holder->m_tNodeID;
		d->m_tGraphID = holder->m_tGraphID;
		d->m_bOnline = false;
		alife_rw().register_object(d, true);
		bring_online_in_holder(d);
	}
	xrCriticalSectionGuard guard(g_out_lock);
	g_specs.add(id, source);
	g_spec_actions[id] = a;
	g_outbox.push_back(std::move(a));
	++g_lc.created;
	g_lc.clones += source != kNoId ? 1 : 0;
	return true;
}

// A temporary object of ours holds a leased id (create / clone_weapon (.., false)): taken out of g_temp_leases.
bool take_temp_lease(CSE_Abstract* e, LeasedTemp& out)
{
	xrCriticalSectionGuard guard(g_out_lock);
	lease_sync_world_locked();
	const auto it = g_temp_leases.find(e);
	if (it == g_temp_leases.end())
		return false;
	out = it->second;
	g_temp_leases.erase(it);
	return true;
}

// A pending clone of ours replaces item `id` (local numbering): the host releases it with the clone's creation, and keeps
// it when it refuses the clone. So a release of it now is that clone's part, not a request of its own. Under g_out_lock.
bool clone_source_locked(u16 id)
{
	if (id == kNoId)
		return false;
	if (g_specs.is_source(id))
		return true;
	for (const auto& it : g_temp_leases)
		if (it.second.source == id)
			return true;
	return false;
}

bool clone_source(u16 id)
{
	xrCriticalSectionGuard guard(g_out_lock);
	return clone_source_locked(id);
}

// Round 1 (review C1): item `id` waits for the host - its create is not answered yet, or a clone of ours replaces it. A
// clone of it now would be refused or take the source the other clone took (and its scope would go for nothing).
bool lease_waiting(u16 id)
{
	if (id == kNoId)
		return false;
	xrCriticalSectionGuard guard(g_out_lock);
	return g_specs.pending(id) || clone_source_locked(id);
}

// Round 1 (review M1): a leased id a create could not use. Reserved here and held by nothing: back into the pool (given to
// the host, it could hand it out again while our generator has it taken: FATAL in client_on_spawn). Free here: the host's
// again. Held here by an object (should not happen): neither.
void return_unused_lease(u16 id)
{
	xrServer* server = g_pGameLevel ? Level().Server : nullptr;
	if (!server)
		return;
	if (server->IsFreeID(id))
	{
		give_back(id);
		return;
	}
	if (server->ID_to_entity(id) || (ai().get_alife() && ai().alife().objects().object(id, true)))
		return;
	xrCriticalSectionGuard guard(g_out_lock);
	g_pool.put_back(id);
}

// The host refused our create under `id` (or kept rate-limiting it): our item goes - released from our ALife like any
// destroyed item, its entity kept a while (the quarantine) - and the id goes back to the host once it is gone here.
void rollback_leased(u16 id, const char* why)
{
	++g_lc.rolled_back;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		g_give_back_wait.push_back(id);
	}
	CSE_ALifeDynamicObject* e = lease_world_ready() ? ai().alife().objects().object(id, true) : nullptr;
	if (!e)
		return;
	if (log_allowed())
		::Msg("! [coop] the host refused our new [%s] %u (%s): taken back here", e->s_name.c_str(), id, why);
	{
		xrCriticalSectionGuard guard(g_quarantine_lock);
		g_quarantine_marks.insert(e);
	}
	entities::ClientApply apply; // a destroy of our own world: no request of it goes to the host
	if (e->m_bOnline)
	{
		NET_Packet packet;
		packet.w_begin(M_EVENT);
		packet.w_u32(Level().timeServer());
		packet.w_u16(GE_DESTROY);
		packet.w_u16(id);
		Level().Send(packet, net_flags(TRUE, TRUE));
	}
	else
		alife_rw().release(e, true);
}

// Rolled-back entities are freed kQuarantineMs after their release (all of them when the session goes).
void quarantine_expire(bool all)
{
	xr_vector<CSE_Abstract*> done;
	{
		xrCriticalSectionGuard guard(g_quarantine_lock);
		// round 1 (review I2): one of another world is left: its script object lives in a Lua state that world closed
		g_quarantine.expire(mgr().now_ms(), kQuarantineMs, join::world_generation(), all,
		    [&done](CSE_Abstract* e) { done.push_back(e); }, [](CSE_Abstract*) { ++g_lc.quarantine_left; });
		if (all)
			g_quarantine_marks.clear();
	}
	for (CSE_Abstract* e : done)
		F_entity_Destroy(e);
}

// Client frame, before the outbox goes: the retries due, the ids to give back (paced: kAckGapMs), the quarantine.
void client_pump_leases()
{
	quarantine_expire(false);
	if (!lease_world_ready())
		return;
	const u64 now = mgr().now_ms();
	lease::IdsMsg ack;
	xr_vector<u16> unanswered; // round 2 (review N1): creates the host never answered
	xr_vector<u32> holds;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		lease_sync_world_locked();
		const lease::SpecBook::Due due = g_specs.due(now);
		for (u16 id : due.resend)
		{
			const auto it = g_spec_actions.find(id);
			if (it == g_spec_actions.end())
			{
				g_specs.forget(id);
				continue;
			}
			g_specs.sent(id, 0, now); // in our outbox (paced): not due again before it goes out and is answered
			g_outbox.push_back(it->second);
			++g_lc.resent;
		}
		for (u16 id : due.rollback)
		{
			// round 2 (review N1): kMaxResends requests without an answer: no waiting for ever - taken back, and the
			// requests that waited for it go too
			g_spec_actions.erase(id);
			g_lc.held_dropped += g_held.drop(id, [&holds](Action& a) { holds.push_back(a.hold); });
			unanswered.push_back(id);
		}
		for (auto it = g_give_back_wait.begin(); it != g_give_back_wait.end();)
		{
			if (ai().alife().objects().object(*it, true) || Level().Server->ID_to_entity(*it))
				++it; // its destroy has not run yet
			else
			{
				g_give_back.push_back(*it);
				it = g_give_back_wait.erase(it);
			}
		}
		if (!g_give_back.empty() && now - g_last_ack >= lease::kAckGapMs)
		{
			const size_t n = _min<size_t>(g_give_back.size(), lease::kMaxIdsPerMsg);
			for (size_t i = 0; i < n; ++i)
				ack.ids.push_back(to_host_id(g_give_back[i]));
			g_give_back.erase(g_give_back.begin(), g_give_back.begin() + n);
			g_last_ack = now;
		}
	}
	for (u32 hold : holds)
		money::client_release(hold);
	for (u16 id : unanswered)
		rollback_leased(id, "no answer from the host"); // its id goes back to the host (IdLeaseAck) once it is gone
	if (!unanswered.empty())
	{
		g_lc.unanswered += unanswered.size();
		dialog::hud_message("st_coop_lease_lost", "Co-op: the host did not answer, a new item was taken back");
	}
	if (ack.ids.empty())
		return;
	ack.epoch = mgr().session().world_epoch();
	Writer w(8 + 2 * ack.ids.size());
	ack.write(w);
	net::client_send(Msg::IdLeaseAck, w);
	++g_lc.acks;
	g_lc.given_back += ack.ids.size();
}

// Msg::IdLease: ids added to our lease. Each one is reserved in our generator, or given back when an object of ours holds
// it (a bolt, a fake missile: the same class as R2's "ID already used").
void client_on_lease(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_client())
		return;
	lease::IdsMsg m;
	if (!m.read(r))
	{
		++g_lc.bad_leases;
		return;
	}
	if (!lease_world_ready() || m.epoch != mgr().session().world_epoch())
	{
		++g_lc.stale_leases; // another world's: the host voided them with it
		return;
	}
	xrServer& server = alife_rw().server();
	for (u16& id : m.ids)
		id = to_local_id(id); // identity: a leased id is neither 0 nor our proxy (both are taken in the host's world)
	std::vector<u16> rejected;
	xrCriticalSectionGuard guard(g_out_lock);
	lease_sync_world_locked();
	g_pool.add(m.epoch, m.ids,
	    [&server](u16 id)
	    {
		    if (!server.IsFreeID(id))
			    return false; // an object of ours holds it
		    server.PerformIDgen(id); // reserved: no object of ours takes it now
		    return true;
	    },
	    rejected);
	g_give_back.insert(g_give_back.end(), rejected.begin(), rejected.end());
}


void client_on_result(PeerId, Reader& r)
{
	const u32 seq = r.u32();
	const bool accepted = r.boolean();
	const u8 reason = r.u8();
	const u16 subject = r.u16();
	if (!r.ok())
		return;
	// F2 T12b round 2 (review N1): the host keeps a create of ours in its budget queue - not an answer yet (it comes
	// when the create is made or refused); no watchdog for it
	if (reason == u8(Reason::Queued))
	{
		bool ours = false;
		{
			xrCriticalSectionGuard guard(g_out_lock);
			ours = g_specs.queued(seq);
		}
		g_lc.queued += ours ? 1 : 0;
		g_inflight.erase(seq);
		return;
	}
	// F2 T12b: the answer to a create of ours under a leased id
	u16 leased = kNoId;
	lease::Outcome outcome = lease::Outcome::Unknown;
	xr_vector<u32> holds; // round 1: money held for upgrades of a rolled-back item
	{
		xrCriticalSectionGuard guard(g_out_lock);
		outcome = g_specs.answer(seq, accepted, reason == u8(Reason::RateLimited), mgr().now_ms(), leased);
		if (outcome == lease::Outcome::Confirmed || outcome == lease::Outcome::RolledBack)
			g_spec_actions.erase(leased);
		if (outcome == lease::Outcome::Confirmed)
		{
			// round 1 (review C1): what waited for the item goes out first with the next flush, in its order (an action
			// that names another pending item waits again there)
			std::vector<Action> back = g_held.release(leased);
			g_outbox.insert(g_outbox.begin(), std::make_move_iterator(back.begin()), std::make_move_iterator(back.end()));
		}
		else if (outcome == lease::Outcome::RolledBack)
			g_lc.held_dropped += g_held.drop(leased, [&holds](Action& a) { holds.push_back(a.hold); });
	}
	for (u32 hold : holds)
		money::client_release(hold);
	switch (outcome)
	{
	case lease::Outcome::Confirmed:
		++g_lc.confirmed;
		items::client_resend_state(leased); // what changed on it while the host had not made it yet
		break;
	case lease::Outcome::Retry: ++g_lc.retried; break;
	case lease::Outcome::RolledBack: rollback_leased(leased, reason_name(Reason(_min<u8>(reason, u8(Reason::Count))))); break;
	case lease::Outcome::Unknown: break;
	}
	Msg type = Msg::ReqResult;
	u16 local_item = kNoId;
	auto it = g_inflight.find(seq);
	if (it != g_inflight.end())
	{
		type = it->second.type;
		local_item = it->second.item;
		// F2 T5: an upgrade's answer. The host sends the balance right after it (charged or not), so the money shown
		// stays as it is until then.
		money::client_release(it->second.hold);
		g_inflight.erase(it);
	}
	if (accepted)
	{
		++g_cl.accepted;
		return;
	}
	++g_cl.rejected;
	// F2 T5 fix round 1 (review 1): our item has the upgrade the host refused; it is built again from the host's
	// entity. Not for Offline / NoProxy: our world is being replaced (or we have no character), and the next world
	// comes with the host's state of our items.
	if (type == Msg::ReqItemEvent && reason != u8(Reason::Offline) && reason != u8(Reason::NoProxy))
		client_rebuild_item(local_item);
	if (type == Msg::ReqTake || type == Msg::ReqDrop || type == Msg::ReqTransfer)
		trade::invalidate_views(); // an open box/body window shows the real content again
	if (log_allowed())
		::Msg("[coop] host refused %s #%u: %s (object %u)", type == Msg::ReqResult ? "request" : msg_name(type), seq,
		    reason_name(Reason(_min<u8>(reason, u8(Reason::Count)))), subject);
}

void client_on_player_hit(PeerId, Reader& r)
{
	if (!mgr().is_live_client())
		return;
	const u16 character = r.u16();
	SHit hit;
	read_hit(r, hit);
	if (!r.ok() || !hit_sane(hit))
		return;
	(void)character; // always our own character (host numbering)
	apply_to_actor(hit, find_go(to_local_id(hit.whoID)), find_go(to_local_id(hit.weaponID)));
	++g_cl.hits_in;
}

// Item-like temporary entity without an id: what CALifeSimulatorBase::spawn_item builds, minus
// the id allocation, registration and supplies (only the host does those).
CSE_Abstract* make_temp(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex, u16 parent)
{
	if (!spawnable_section(section)) // R3 В6
		return nullptr;
	CSE_Abstract* e = F_entity_Create(section);
	if (!e)
		return nullptr;
	e->s_name = section;
	e->set_name_replace(section);
	e->s_RP = 0xff;
	e->ID = kNoId;
	e->ID_Parent = parent;
	e->ID_Phantom = kNoId;
	e->o_Position = pos;
	e->m_wVersion = SPAWN_VERSION;
	if (CSE_ALifeDynamicObject* d = smart_cast<CSE_ALifeDynamicObject*>(e))
	{
		d->m_tNodeID = level_vertex;
		d->m_tGraphID = game_vertex;
		d->m_tSpawnID = ALife::_SPAWN_ID(-1);
	}
	if (CSE_ALifeItemWeapon* weapon = smart_cast<CSE_ALifeItemWeapon*>(e))
		weapon->a_elapsed = weapon->get_ammo_magsize();
	return e;
}

CSE_Abstract* lua_forward(CSE_Abstract* e, const char* name)
{
	if (client_forward_spawn(e))
		++g_cl.lua_forwarded;
	else
	{
		// F2 T12: the call gate already refused a parent that is not ours; this is the backstop, counted with it
		string256 why;
		xr_sprintf(why, "[%s] is a world spawn, the host's job", e->s_name.c_str());
		lua_refuse(name, why);
	}
	return nullptr;
}

// ---------------------------------------------------------------- host side

enum Cat : u8
{
	CatSpawn,
	CatMove,
	CatDestroy,
	CatHit,
	CatMisc,
	CatCount
};
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

struct HostCounters
{
	u64 results[u32(Reason::Count)] = {};
	u64 dropped_joining = 0, malformed = 0, hits_applied = 0, proxy_hits = 0, player_hits_sent = 0, spawns_local = 0;
	u64 explosion_hits_sent = 0; // R3 В2: blasts / fragments no creature set off (a barrel, a mine, an anomaly)
	u64 friendly_fire_dropped = 0; // hits of one player on another dropped at the character (proxy or host actor)
	u64 explosions_as_player = 0; // charges set off as their planter (coop.explode_as)
} g_h;

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

struct Req
{
	PeerId from = kInvalidPeer;
	u32 seq = 0;
	u16 proxy = kNoId;
	Msg type = Msg::ReqResult;
};

// F2 T12b round 2 (review N1): a leased create waits in the budget queue: not an answer (that comes when it is made or
// refused), so the asker's watchdog does not ask again - a second request with its id would queue twice.
u64 g_queued_notices = 0;
void notify_queued(const Req& q, u16 id)
{
	Writer w(12);
	w.u32(q.seq);
	w.boolean(false);
	w.u8(u8(Reason::Queued));
	w.u16(id);
	net::host_send(q.from, Msg::ReqResult, w);
	++g_queued_notices;
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
	if (why != Reason::Ok && log_allowed())
		::Msg("[coop] refused %s #%u from player #%u: %s (object %u)", msg_name(q.type), q.seq, q.from, reason_name(why),
		    subject);
}

// Common checks of every request. False = drop (already answered when an answer makes sense).
// Precaching: the world was just loaded. The session pump runs before join::frame() notices the
// new world and marks the peers as joining, so requests made in the old world would land here.
bool host_drops_requests_of(PeerId from) { return sync::host_peer_joining(from) || Device.dwPrecacheFrame; }

bool host_begin(PeerId from, Reader& r, Msg type, Req& q, bool has_seq = true)
{
	if (!mgr().is_host())
		return false;
	if (host_drops_requests_of(from))
	{
		++g_h.dropped_joining; // its ids belong to a world that was replaced (load, level change)
		return false;
	}
	q.from = from;
	q.type = type;
	q.seq = has_seq ? r.u32() : 0;
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

// ---------------------------------------------------------------- F2 T2: what a client may ask for
// The verdict is xrCoop/coop_spawn.h judge(). Host settings: configs\coop_spawn.ltx, read once per process. The cost
// budgets belong to the characters for the whole session (Ruling F2-11), by player key (Ruling F2-18): a peer that
// leaves and joins again as the same character refills nothing, nor does a reload that gives his character another
// id; reset() at the session's start and end clears them.
constexpr LPCSTR kSpawnSettings = "coop_spawn";
constexpr int kMaxSpawnSetting = 2000000000; // ltx values are ints
constexpr u32 kSpawnVerdicts = u32(spawn::SpawnVerdict::WrongParent) + 1;
constexpr u64 kSpawnLogFirst = 10;
spawn::SpawnPolicy g_spawn_policy;
xr_set<shared_str> g_spawn_allow, g_spawn_deny;
spawn::BudgetBook g_spawn_budgets;
u64 g_spawn_refused[kSpawnVerdicts] = {};
u64 g_spawn_refusals = 0;
u64 g_spawn_quest_gifts = 0; // Ruling F2-16: quest items let through for a talk

// Ruling F2-18: the key of a character's budget, the same as the money book's (coop_money character_key_of): the player
// key digest on the "key" line of the proxy's custom data (ledger::character_key); a character without one (none of
// coop_join's) is keyed by its id.
std::string spawn_budget_key(u16 proxy)
{
	const CSE_Abstract* e = events::find_entity(proxy);
	std::string key = e && e->m_ini_string.size() ? ledger::character_key(e->m_ini_string.c_str()) : std::string();
	if (key.empty())
	{
		char id[16];
		xr_sprintf(id, "#%u", u32(proxy));
		key = id;
	}
	return key;
}

u32 read_spawn_setting(CInifile& ini, LPCSTR key, u32 fallback)
{
	if (!ini.line_exist(kSpawnSettings, key))
		return fallback;
	return u32(_max(0, _min(ini.r_s32(kSpawnSettings, key), kMaxSpawnSetting)));
}

void read_section_list(CInifile& ini, LPCSTR list, xr_set<shared_str>& out)
{
	if (!ini.section_exist(list))
		return;
	const u32 n = ini.line_count(list);
	for (u32 i = 0; i < n; ++i)
	{
		LPCSTR name = nullptr;
		LPCSTR value = nullptr;
		if (ini.r_line(list, int(i), &name, &value) && name && name[0])
			out.insert(shared_str(name));
	}
}

void load_spawn_settings()
{
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;
	string_path path;
	FS.update_path(path, "$game_config$", "coop_spawn.ltx");
	if (!FS.exist(path))
	{
		::Msg("! [coop] configs\\coop_spawn.ltx not found: spawn cost budget is the default, no allow or deny lists");
		return;
	}
	CInifile ini(path, TRUE, TRUE, FALSE); // DLTX: mod_coop_spawn_*.ltx add to it
	if (ini.section_exist(kSpawnSettings))
	{
		g_spawn_policy.cost_burst = read_spawn_setting(ini, "cost_burst", g_spawn_policy.cost_burst);
		g_spawn_policy.cost_per_sec = read_spawn_setting(ini, "cost_per_sec", g_spawn_policy.cost_per_sec);
	}
	else
		::Msg("! [coop] configs\\coop_spawn.ltx has no [%s]: spawn cost budget is the default", kSpawnSettings);
	read_section_list(ini, "allow_sections", g_spawn_allow);
	read_section_list(ini, "deny_sections", g_spawn_deny);
}

// What the verdict needs to know of the section: `quest_item` exactly as CInventoryItem::Load reads it, `cost` as
// CSE_ALifeInventoryItem does (0 if missing: the class check refuses such a section first), the host's lists.
spawn::SectionFacts spawn_facts(const char* section)
{
	spawn::SectionFacts f;
	f.quest_item = !!READ_IF_EXISTS(pSettings, r_bool, section, "quest_item", FALSE);
	f.cost = pSettings->line_exist(section, "cost") ? pSettings->r_u32(section, "cost") : 0;
	const shared_str name(section);
	f.allow_listed = g_spawn_allow.find(name) != g_spawn_allow.end();
	f.deny_listed = g_spawn_deny.find(name) != g_spawn_deny.end();
	return f;
}

// A refused ReqSpawn: counted by verdict (coop_status), the first ten logged with the section and the player.
void refuse_spawn(const Req& q, spawn::SpawnVerdict v, const char* section, const spawn::Missing& missing, u16 parent,
    double budget_left)
{
	using spawn::SpawnVerdict;
	++g_spawn_refused[u32(v)];
	if (++g_spawn_refusals <= kSpawnLogFirst)
	{
		const PeerInfo* p = mgr().session().peer(q.from);
		string256 why;
		switch (v)
		{
		case SpawnVerdict::UnknownClass:
			xr_sprintf(why, "class %.16s is not an item class a client may ask for",
			    pSettings->r_string(section, "class"));
			break;
		case SpawnVerdict::MissingLine:
			if (missing.no_section)
				xr_sprintf(why, "its `%s` names no section (its class reads it unchecked)", missing.line);
			else if (missing.in)
				xr_sprintf(why, "no `%s` in the section its `%s` names (its class reads it unchecked)", missing.line,
				    missing.in);
			else
				xr_sprintf(why, "no `%s` (its class reads it unchecked)", missing.line ? missing.line : "?");
			break;
		case SpawnVerdict::Denied: xr_strcpy(why, "in [deny_sections] of coop_spawn.ltx");
			break;
		case SpawnVerdict::QuestItem: xr_strcpy(why, "a quest item");
			break;
		case SpawnVerdict::OverBudget: // only when the queue is full (Ruling F2-21)
			xr_sprintf(why, "over the cost budget of character %u (cost %u RU, %.0f RU left) and %u of his requests "
			                "already wait",
			    q.proxy, spawn_facts(section).cost, budget_left, u32(spawn::kMaxQueued));
			break;
		case SpawnVerdict::WrongParent:
			xr_sprintf(why, "the item %u it would go into does not take it (only a launcher its rocket, a missile its "
			                "fake missile)",
			    parent);
			break;
		default: xr_strcpy(why, "?");
			break;
		}
		::Msg("! [coop] ReqSpawn #%u of [%s] from player #%u <%s> refused: %s", q.seq, section, q.from,
		    p ? p->nick.c_str() : "?", why);
	}
	Reason reason = Reason::NotAllowed; // not an item class, a parent that does not take it, deny-listed, a quest item
	if (v == SpawnVerdict::MissingLine)
		reason = Reason::BadSection;
	else if (v == SpawnVerdict::OverBudget)
		reason = Reason::RateLimited;
	reply(q, reason, parent);
}

// ---------------------------------------------------------------- F2 T12b: leased ids, host
// The host takes free ids out of its generator and leases kLeaseSize of them to each player in the world (Msg::IdLease),
// a spent one replaced 1:1 (xrCoop/coop_id_lease.h). The leases belong to this world's generator: another world
// (join::world_generation) voids them, nothing is freed; a player who leaves or loads the world again gives his back.
lease::HostLeases g_leases;
struct HostLeaseCounters
{
	u64 messages = 0, spawns = 0, clones = 0, not_his = 0, acks = 0, ack_ids = 0, ack_not_his = 0, bad_acks = 0;
	u64 short_of_ids = 0;
} g_hl;

void host_sync_leases() { g_leases.set_epoch(join::world_generation()); }

bool host_world_ready() { return g_pGameLevel && ai().get_alife() && Level().Server && !Device.dwPrecacheFrame; }

// Ids the leases held go back to this world's generator. An id it has free already is left alone (a second free would
// put it into the free list twice).
void host_free_ids(const std::vector<u16>& ids)
{
	if (!g_pGameLevel || !Level().Server)
		return;
	xrServer& server = *Level().Server;
	for (u16 id : ids)
		if (lease::leasable(id) && !server.IsFreeID(id))
			server.FreeID(id, Device.TimerAsync());
}

// Round 1 (review M4): the player's leased creates still waiting for his budget go with his lease (it is freed when he
// loads the world again; a freed id may come back to him in his new pool, and the old request would create under it).
void drop_leased_queued(PeerId peer);

// Host frame: every player in the world holds a whole lease (one IdLease per player and frame with what he lacks); a
// player who loads the world again (a rejoin) gives his back: his machine starts a new pool with its new world.
void host_pump_leases()
{
	host_sync_leases();
	if (!host_world_ready())
		return;
	xrServer& server = *Level().Server;
	for (const PeerInfo& p : mgr().session().peers())
	{
		if (p.state != PeerState::Joined)
			continue;
		if (sync::host_peer_joining(p.id))
		{
			if (g_leases.count(p.id))
			{
				drop_leased_queued(p.id); // round 1 (review M4)
				host_free_ids(g_leases.drop(p.id));
			}
			continue;
		}
		const u16 proxy = join::proxy_of_peer(p.id);
		if (proxy == kNoId || !join::host_player_in_world(proxy))
			continue;
		const size_t want = g_leases.want(p.id);
		if (!want)
			continue;
		std::vector<u16> bad;
		lease::IdsMsg m;
		m.epoch = mgr().session().world_epoch();
		m.ids = g_leases.grant(p.id, want,
		    [&server]() -> u16
		    {
			    // a nearly full world keeps its last blocks (PerformIDgen(0xffff) asserts with none)
			    return server.AvailableIDBlocks() >= lease::kMinFreeBlocks ? server.PerformIDgen(kNoId) : kNoId;
		    },
		    bad);
		for (u16 id : bad)
			if (!g_leases.leased(id))
				server.FreeID(id, Device.TimerAsync()); // picked, not leased
		if (m.ids.empty())
		{
			g_hl.short_of_ids += want == lease::kLeaseSize ? 1 : 0;
			continue;
		}
		Writer w(8 + 2 * m.ids.size());
		m.write(w);
		net::host_send(p.id, Msg::IdLease, w);
		++g_hl.messages;
	}
}

// Msg::IdLeaseAck: ids the player gives back (busy on his machine, or his create was refused) go back to the generator.
void host_on_lease_ack(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return;
	lease::IdsMsg m;
	if (!m.read(r))
	{
		++g_hl.bad_acks;
		return;
	}
	host_sync_leases();
	if (m.epoch != mgr().session().world_epoch() || !host_world_ready())
		return; // another world's: voided with it
	++g_hl.acks;
	std::vector<u16> back;
	for (u16 id : m.ids)
	{
		if (g_leases.give_back(from, id))
			back.push_back(id);
		else
			++g_hl.ack_not_his; // not his, or spent: an entity has it
	}
	g_hl.ack_ids += back.size();
	host_free_ids(back);
}

// A request with a leased id: the id is the asker's (checked before anything touches the generator), it goes into his
// character, and a clone's source is a magazined weapon of his (`source_cost`: its cost, what the clone does not pay).
Reason host_lease_checks(const Req& q, u16 parent, u16 id, u16 source, const char* section, u32& source_cost)
{
	source_cost = 0;
	host_sync_leases();
	if (!g_leases.owns(q.from, id))
	{
		++g_hl.not_his;
		return Reason::NotOwner;
	}
	if (parent != q.proxy)
		return Reason::NotAllowed; // a leased create goes into his character only
	if (source == kNoId)
		return Reason::Ok;
	CSE_Abstract* src = events::find_entity(source);
	if (!src)
		return Reason::Missing;
	if (events::root_of(source) != q.proxy)
		return Reason::NotOwner;
	if (!smart_cast<CSE_ALifeItemWeaponMagazined*>(src))
		return Reason::NotAllowed;
	if (!clone_variant(src->s_name.c_str(), section))
		return Reason::NotAllowed; // round 2 (review N3): a variant of the same weapon only
	source_cost = pSettings->line_exist(src->s_name, "cost") ? pSettings->r_u32(src->s_name, "cost") : 0;
	return Reason::Ok;
}

Reason host_destroy(const Req& q, u16 item);

// Round 1 (review I1): the state a clone is built with - the host's own source, captured as the client's lua_clone
// copies it (condition, upgrades, addons, magazine), checked like a client's blob (host_apply_spawn_state). Empty when
// there is no source.
void clone_state(u16 source, std::vector<u8>& out)
{
	out.clear();
	if (source != kNoId)
		items::capture_spawn_state(events::find_entity(source), out);
}

// The leased create is made (`created` is `id`): the id is the world's now. A clone's source goes with it.
void host_leased_made(const Req& q, u16 id, u16 created, u16 source)
{
	g_leases.spend(q.from, id);
	++g_hl.spawns;
	if (created != id && log_allowed())
		::Msg("! [coop] ReqSpawn #%u of player #%u was created as %u, not as its leased id %u", q.seq, q.from, created, id);
	if (source == kNoId)
		return;
	++g_hl.clones;
	const Reason gone = host_destroy(q, source);
	if (gone != Reason::Ok && log_allowed())
		::Msg("! [coop] the clone %u of player #%u is made, its source %u stays: %s", created, q.from, source,
		    reason_name(gone));
}

// Who would hold a client's new item: checked before anything is constructed (R3 В6 review).
Reason host_spawn_holder(const Req& q, u16 parent)
{
	if (parent == kNoId)
		return Reason::NotAllowed; // world spawns are the host's
	if (events::root_of(parent) != q.proxy)
		return Reason::NotOwner;
	if (!events::find_entity(parent))
		return Reason::Missing;
	if (!Level().Server->ID_to_entity(parent))
		return Reason::Offline; // Process_spawn needs an online holder
	return Reason::Ok;
}

// Creates `e` (consumed) as a child of `parent` exactly like alife():create with an online parent:
// the server's Process_spawn with a host id; ALife registration announces it (EntitySpawn), a
// non-ALife child (fake missile, rocket) is announced with EntityLocal. The holder passed host_spawn_holder.
Reason host_spawn(CSE_Abstract* e, u16 parent, u16& created, u16 leased = kNoId)
{
	created = kNoId;
	const auto fail = [&](Reason why)
	{
		F_entity_Destroy(e);
		return why;
	};
	if (smart_cast<CSE_ALifeCreatureAbstract*>(e) ||
	    (!smart_cast<CSE_ALifeInventoryItem*>(e) && !smart_cast<CSE_Temporary*>(e)))
		return fail(Reason::NotAllowed); // clients create items for themselves, nothing else
	CSE_Abstract* holder = events::find_entity(parent);
	if (!holder)
		return fail(Reason::Missing);

	// what a client may not choose
	e->ID = leased; // F2 T12b: the id the client created it under already, else the server picks one
	if (leased != kNoId)
		name_with_id(e); // as the client named it (CALifeSimulatorBase::spawn_item's name)
	e->ID_Parent = parent;
	e->ID_Phantom = kNoId;
	e->RespawnTime = 0;
	e->s_RP = 0xff;
	e->m_tSpawnID = ALife::_SPAWN_ID(-1);
	e->o_Position = holder->o_Position;
	if (CSE_ALifeObject* ao = smart_cast<CSE_ALifeObject*>(e))
	{
		ao->m_story_id = INVALID_STORY_ID;
		ao->m_spawn_story_id = INVALID_SPAWN_STORY_ID;
	}
	CSE_ALifeDynamicObject* d = smart_cast<CSE_ALifeDynamicObject*>(e);
	CSE_ALifeDynamicObject* hd = smart_cast<CSE_ALifeDynamicObject*>(holder);
	if (d && hd)
	{
		d->m_tNodeID = hd->m_tNodeID;
		d->m_tGraphID = hd->m_tGraphID;
	}

	NET_Packet packet;
	e->Spawn_Write(packet, FALSE);
	F_entity_Destroy(e);
	u16 dummy;
	packet.r_begin(dummy);
	ClientID client;
	client.set(0xffff);
	xrServer& server = *Level().Server;
	// F2 T12b: a leased id is out of the generator's free list since its grant; back in it right before Process_spawn
	// takes it again (E->ID = PerformIDgen(E->ID)), as spawn_item2 does with the id spawn_item took
	if (leased != kNoId)
		server.FreeID(leased, 0);
	CSE_Abstract* result = server.Process_spawn(packet, client);
	if (!result)
	{
		if (leased != kNoId && server.IsFreeID(leased))
			server.PerformIDgen(leased); // still leased: out of the free list again
		return Reason::Engine;
	}
	created = result->ID;
	if (!ai().alife().objects().object(result->ID, true))
	{
		events::host_relay_local_spawn(result);
		++g_h.spawns_local;
	}
	return Reason::Ok;
}

// Review round 2, Important 1: the item `parent` (one of the asker's, not his character) takes the new entity: the host
// gives it GE_OWNERSHIP_TAKE, whose handlers cast the child unchecked (xrCoop/coop_spawn.h child_allowed).
bool parent_takes(u16 parent, const spawn::ClassRule& child, const char* child_section)
{
	const CSE_Abstract* holder = events::find_entity(parent);
	if (!holder || !holder->s_name.size() || !pSettings->section_exist(holder->s_name) ||
	    !pSettings->line_exist(holder->s_name, "class"))
		return false;
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(holder->s_name, "class"));
	// Round 4 (Important 1): only while it has room - the children it holds now, the magazine of its section.
	return rule &&
	    spawn::child_allowed(rule->game, holder->s_name.c_str(), child.game, child_section, holder->children.size(),
	        SectionIni{holder->s_name.c_str()}.number("ammo_mag_size"));
}

// Whether `parent` takes a new entity of `rule`'s class: the asker's character any item but a launcher's rocket
// (round 4 minor), one of his items only what it takes (parent_takes). No rule: the class check refuses first.
bool parent_ok_for(const Req& q, u16 parent, const spawn::ClassRule* rule, const char* section)
{
	if (!rule)
		return true;
	return parent == q.proxy ? spawn::character_takes(rule->game) : parent_takes(parent, *rule, section);
}

// Round 2 minor (Ruling F2-16): how far the asker's character stands from the NPC of his talk lock, as host_try_lock
// (coop_dialog.cpp) measures it: 0 for an NPC that talks from afar (NeedOsoznanieMode), flt_max when either is not in
// the world.
float talk_distance(u16 npc, u16 proxy)
{
	CObject* them = g_pGameLevel ? Level().Objects.net_Find(npc) : nullptr;
	CInventoryOwner* io = smart_cast<CInventoryOwner*>(them);
	if (io && io->NeedOsoznanieMode())
		return 0.f;
	CObject* me = g_pGameLevel ? Level().Objects.net_Find(proxy) : nullptr;
	if (!me || !them)
		return flt_max;
	return me->Position().distance_to(them->Position());
}

// Builds the entity of a section that passed the class and line checks, from the host's section with the client's item
// state (WP9 ItemState checks: counts, upgrades in an order the engine takes, ammo types). nullptr: refused, `why` says
// how (logged here).
CSE_Abstract* build_spawn(const Req& q, u16 parent, const char* section, const float pos[3], const u8* state,
    size_t state_size, Reason& why)
{
	// Task 10b: the section's own installed_upgrades break its tree: init_install is FATAL on the new object.
	if (!items::section_defaults_ok(shared_str(section)))
	{
		if (note_bad(BadSection))
			::Msg("! [coop] ReqSpawn #%u from player #%u refused: [%s] has default upgrades its own upgrade tree "
			      "refuses (broken data)",
			    q.seq, q.from, section);
		why = Reason::BadSection;
		return nullptr;
	}
	CSE_Abstract* e =
	    make_temp(section, Fvector().set(pos[0], pos[1], pos[2]), u32(-1), GameGraph::_GRAPH_ID(-1), parent);
	if (!e)
	{
		why = Reason::Engine;
		return nullptr;
	}
	const char* bad = nullptr;
	if (state_size && !items::host_apply_spawn_state(e, state, state_size, bad))
	{
		if (note_bad(BadSpawnState))
			::Msg("! [coop] ReqSpawn #%u of [%s] from player #%u refused: %s", q.seq, section, q.from,
			    bad ? bad : "item state");
		F_entity_Destroy(e);
		why = Reason::Malformed;
		return nullptr;
	}
	return e;
}

// ---------------------------------------------------------------- Ruling F2-21: requests waiting for the budget
// A request over its character's budget waits here (xrCoop/coop_spawn.h SpawnQueue) as data only (QueuedRequest: no
// entity - review round 4, Important 2: a script class's server entity holds Lua references that a save load closes),
// and is judged, built, created and answered when its turn comes. Its player leaving (on_peer_left), or the world
// replaced (SessionReset: another epoch), drops it unanswered.
spawn::SpawnQueue<spawn::QueuedRequest> g_spawn_queue;
u64 g_spawn_queued = 0, g_spawn_dequeued = 0, g_spawn_queue_dropped = 0;

void drop_queued(spawn::QueuedRequest&) { ++g_spawn_queue_dropped; }

void drop_leased_queued(PeerId peer)
{
	const auto leased = [peer](const spawn::QueuedRequest& r) { return PeerId(r.peer) == peer && r.id != kNoId; };
	for (const std::string& key : g_spawn_queue.keys())
		g_spawn_queue.drop_if(key, leased, drop_queued);
}

// The turn of the front request of `key`: everything checked again from what the host knows now (the section, its
// class and lines, the parent and what it holds, the lists, the item state), the budget charged by judge; Wait while it
// does not hold it. A refusal after the charge gives it back.
spawn::Turn create_queued(const std::string& key, spawn::QueuedRequest& r)
{
	Req q;
	q.from = PeerId(r.peer);
	q.seq = r.seq;
	q.proxy = r.character;
	q.type = Msg::ReqSpawn;
	const char* section = r.section.c_str();
	if (!spawnable_section(section))
	{
		reply(q, Reason::BadSection, r.parent);
		return spawn::Turn::Refused;
	}
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(section, "class"));
	const spawn::Missing missing = rule ? missing_item_line(section, *rule) : spawn::Missing();
	spawn::SectionFacts facts = spawn_facts(section);
	facts.parent_ok = parent_ok_for(q, r.parent, rule, section);
	facts.launch_child = r.parent != q.proxy && rule != nullptr && facts.parent_ok;
	if (r.id != kNoId)
	{
		// F2 T12b: still his id (a lease he gave back or lost meanwhile refuses it), a clone's source still his
		u32 source_cost = 0;
		const Reason leased = host_lease_checks(q, r.parent, r.id, r.source, r.section.c_str(), source_cost);
		if (leased != Reason::Ok)
		{
			reply(q, leased, r.parent);
			return spawn::Turn::Refused;
		}
		if (r.source != kNoId)
			facts.cost = lease::clone_cost(facts.cost, source_cost);
	}
	spawn::CostBudget& budget = g_spawn_budgets.of(key);
	const spawn::SpawnVerdict verdict =
	    spawn::judge_queued(rule, missing.line, facts, r, budget, g_spawn_policy, mgr().now_ms());
	if (verdict == spawn::SpawnVerdict::OverBudget)
		return spawn::Turn::Wait;
	if (verdict != spawn::SpawnVerdict::Allow)
	{
		refuse_spawn(q, verdict, section, missing, r.parent, budget.tokens);
		return spawn::Turn::Refused;
	}
	const auto refuse = [&](Reason why)
	{
		if (!spawn::budget_free(facts))
			spawn::refund(budget, facts.cost, g_spawn_policy); // Ruling F2-21 (3)
		reply(q, why, r.parent);
		return spawn::Turn::Refused;
	};
	// The player may have moved or dropped the parent meanwhile.
	const Reason holder = host_spawn_holder(q, r.parent);
	if (holder != Reason::Ok)
		return refuse(holder);
	Reason why = Reason::Ok;
	// round 1 (review I1): a clone from the host's source as it is now, never from the client's blob
	std::vector<u8> source_state;
	clone_state(r.source, source_state);
	const spawn::StateRef state = spawn::spawn_state(r.source, r.state.data(), r.state.size(), source_state);
	CSE_Abstract* e = build_spawn(q, r.parent, section, r.pos, state.data, state.size, why);
	if (!e)
		return refuse(why);
	if (r.source != kNoId && !smart_cast<CSE_ALifeItemWeaponMagazined*>(e))
	{
		F_entity_Destroy(e);
		return refuse(Reason::NotAllowed); // F2 T12b: a clone is a magazined weapon
	}
	u16 created = kNoId;
	why = host_spawn(e, r.parent, created, r.id); // consumes the entity
	if (why != Reason::Ok)
		return refuse(why);
	if (r.id != kNoId)
		host_leased_made(q, r.id, created, r.source);
	g_spawn_quest_gifts += facts.quest_item && !facts.allow_listed ? 1 : 0;
	reply(q, Reason::Ok, created);
	return spawn::Turn::Created;
}

// Host frame: drops what belongs to a replaced world (or to a player who left, should on_peer_left have missed it),
// gives the front requests their turns.
void host_pump_spawns()
{
	if (!g_spawn_queue.total() || !g_pGameLevel || !ai().get_alife() || !Level().Server || Device.dwPrecacheFrame)
		return;
	const u32 epoch = join::host_world_epoch();
	const auto gone = [epoch](const spawn::QueuedRequest& r)
	{ return r.epoch != epoch || join::proxy_of_peer(PeerId(r.peer)) != r.character; };
	for (const std::string& key : g_spawn_queue.keys())
	{
		g_spawn_queue.drop_if(key, gone, drop_queued);
		g_spawn_dequeued +=
		    g_spawn_queue.pump(key, [&key](spawn::QueuedRequest& r) { return create_queued(key, r); });
	}
}

void host_on_spawn(PeerId from, Reader& r)
{
	// Layouts and markers: xrCoop/coop_spawn.h.
	const spawn::Layout layout = spawn::detect(r.cursor(), r.remaining());
	Req q;
	if (!host_begin(from, r, Msg::ReqSpawn, q, spawn::has_seq(layout)))
		return;
	// R3 В6 (review): every spawn request costs a token first, whatever is wrong with it
	if (!allow(from, CatSpawn))
		return reply(q, Reason::RateLimited, kNoId);
	if (layout == spawn::Layout::Raw)
	{
		// R3 В6: raw M_SPAWN bytes (an older client) are never read: Spawn_Read trusts every byte (FATAL, AV).
		if (note_bad(BadSpawnState))
			::Msg("! [coop] ReqSpawn #%u from player #%u refused: raw spawn bytes (older client)", q.seq, from);
		return reply(q, Reason::Malformed, kNoId);
	}
	spawn::Request s;
	if (!spawn::read_body(r, layout, s))
		return reply(q, Reason::Malformed, kNoId);
	if (!validate::finite3(s.pos))
	{
		if (note_bad(BadNumber))
			::Msg("! [coop] ReqSpawn #%u from player #%u refused: position is not a number", q.seq, from);
		return reply(q, Reason::Malformed, s.parent);
	}
	const Reason holder = host_spawn_holder(q, s.parent);
	if (holder != Reason::Ok)
		return reply(q, holder, s.parent);
	// F2 T12b: a leased id is the asker's, before anything touches the generator; into his character; a clone's source
	// is his weapon
	u32 source_cost = 0;
	if (s.id != kNoId)
	{
		const Reason leased = host_lease_checks(q, s.parent, s.id, s.source, s.section.c_str(), source_cost);
		if (leased != Reason::Ok)
			return reply(q, leased, s.parent);
	}
	// R3 В6: a section the host knows, with a class; then the host builds the entity from its own section.
	const char* section = s.section.c_str();
	if (!spawnable_section(section))
	{
		if (note_bad(BadSection))
			::Msg("! [coop] ReqSpawn #%u from player #%u refused: [%.64s] is not a section with a class here", q.seq,
			    from, validate::resource_name_ok(section, spawn::kMaxSection) ? section : "<bad name>");
		return reply(q, Reason::BadSection, s.parent);
	}
	// F2 T2: an item class of the game with every line its server entity and game object read unchecked (Ruling
	// F2-17); anything else is never constructed (a missing line is FATAL on the host).
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(section, "class"));
	const spawn::Missing missing = rule ? missing_item_line(section, *rule) : spawn::Missing();
	CSE_Abstract* e = nullptr;
	if (rule && !missing.line)
	{
		Reason why = Reason::Ok;
		// F2 T12b round 1 (review I1): a clone is built from the host's own source (as lua_clone copies it on the
		// client), whatever the client's blob says; the budget pays only the price difference
		std::vector<u8> source_state;
		clone_state(s.source, source_state);
		const spawn::StateRef state = spawn::spawn_state(s.source, s.state, s.state_size, source_state);
		e = build_spawn(q, s.parent, section, s.pos, state.data, state.size, why);
		if (!e)
			return reply(q, why, s.parent);
		if (s.source != kNoId && !smart_cast<CSE_ALifeItemWeaponMagazined*>(e))
		{
			F_entity_Destroy(e);
			return reply(q, Reason::NotAllowed, s.parent); // F2 T12b: a clone is a magazined weapon
		}
	}
	// F2 T2: deny/allow lists, quest items, the character's cost budget; after the item state, so a malformed one
	// costs nothing. No rule or a missing line ends here too (judge steps 1-2, nothing was constructed).
	spawn::SectionFacts facts = spawn_facts(section);
	// F2 T12b: a clone replaces its source (released with it): the budget pays only what it is worth more
	if (s.id != kNoId && s.source != kNoId)
		facts.cost = lease::clone_cost(facts.cost, source_cost);
	// Review round 2 (Important 1), round 4: into his character any item but a launcher's rocket; into one of his items
	// only what that item takes, while it has room.
	facts.parent_ok = parent_ok_for(q, s.parent, rule, section);
	// Ruling F2-22: what an item of his takes is a shot (a rocket, a fake missile): no budget, no queue.
	facts.launch_child = s.parent != q.proxy && rule != nullptr && facts.parent_ok;
	// Ruling F2-16: a quest item into his own character while he holds a talk lock (the stock dialogs give it there)
	// and stands within talk reach of that NPC (round 2 minor).
	const u16 talk_npc = dialog::host_lock_of(from);
	facts.in_dialog_to_own_actor = spawn::quest_gift(talk_npc != kNoId,
	    talk_npc != kNoId ? talk_distance(talk_npc, q.proxy) : flt_max, s.parent, q.proxy);
	const std::string key = spawn_budget_key(q.proxy);
	facts.queue_waiting = g_spawn_queue.waiting(key); // Ruling F2-21: behind his earlier requests
	spawn::CostBudget& budget = g_spawn_budgets.of(key);
	const spawn::SpawnVerdict verdict = spawn::judge(rule, missing.line, facts, budget, g_spawn_policy, mgr().now_ms());
	const bool gift = facts.quest_item && !facts.allow_listed; // a quest item past judge is a quest gift
	// Ruling F2-21: over the budget it waits (OverBudget needs a rule and no missing line: `e` was built and its state
	// checked). The queue keeps data only (round 4): the entity goes now and is built again at its turn, answered then
	// (host_pump_spawns). Refused only when his queue is full.
	if (verdict == spawn::SpawnVerdict::OverBudget && e)
	{
		F_entity_Destroy(e);
		e = nullptr;
		if (g_spawn_queue.push(key, spawn::queued_request(q.seq, q.from, q.proxy, s.parent, s.pos, s.section,
		                                s.state, s.state_size, join::host_world_epoch(), facts.in_dialog_to_own_actor,
		                                s.id, s.source))) // F2 T12b: a leased id stays leased while it waits
		{
			if (++g_spawn_queued <= kSpawnLogFirst)
				::Msg("[coop] ReqSpawn #%u of [%s] from player #%u waits for the cost budget of character %u (%u in "
				      "his queue): answered when created",
				    q.seq, section, from, q.proxy, u32(g_spawn_queue.size(key)));
			if (s.id != kNoId)
				notify_queued(q, s.id); // F2 T12b round 2 (review N1): his watchdog waits for this one
			return;
		}
	}
	if (verdict != spawn::SpawnVerdict::Allow)
	{
		if (e)
			F_entity_Destroy(e);
		return refuse_spawn(q, verdict, section, missing, s.parent, budget.tokens);
	}
	g_spawn_quest_gifts += gift ? 1 : 0;
	// Allow needs a rule and no missing line: `e` was built above.
	u16 created = kNoId;
	const Reason why = host_spawn(e, s.parent, created, s.id);
	if (why != Reason::Ok && !spawn::budget_free(facts))
		spawn::refund(budget, facts.cost, g_spawn_policy); // Ruling F2-21 (3): the engine refused what was charged
	if (why == Reason::Ok && s.id != kNoId)
		host_leased_made(q, s.id, created, s.source);
	reply(q, why, why == Reason::Ok ? created : s.parent);
}

// Moves `item` from `src` to `dst` (0xffff = the world) through the engine: ownership events
// when everything is online (server entity, ALife and game objects stay consistent, and the
// server hook relays the change), ALife attach/detach when everything is offline.
Reason apply_move(u16 item, u16 src, u16 dst, const Fvector* drop_pos)
{
	CGameObject* gi = valid_go(item);
	const bool online = gi && (src == kNoId || valid_go(src)) && (dst == kNoId || valid_go(dst));
	if (online)
	{
		const bool transfer = src != kNoId && dst != kNoId;
		if (src != kNoId)
			inject_item_event(transfer ? GE_TRADE_SELL : GE_OWNERSHIP_REJECT, src, item);
		if (dst != kNoId)
			inject_item_event(transfer ? GE_TRADE_BUY : GE_OWNERSHIP_TAKE, dst, item);
		CSE_Abstract* e = events::find_entity(item);
		if (!e || e->ID_Parent != dst)
			return Reason::Engine;
		if (drop_pos && dst == kNoId)
		{
			NET_Packet P; // after the reject in the same queue: the item has its physics shell by then
			CGameObject::u_EventGen(P, GE_CHANGE_POS, item);
			P.w_vec3(*drop_pos);
			CGameObject::u_EventSend(P);
			events::host_set_drop_position(item, *drop_pos);
		}
		return Reason::Ok;
	}
	CSE_ALifeDynamicObject* d = ai().alife().objects().object(item, true);
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(d);
	if (!d || !ii || d->m_bOnline)
		return Reason::Offline;
	CSE_ALifeDynamicObject* from = src == kNoId ? nullptr : ai().alife().objects().object(src, true);
	CSE_ALifeDynamicObject* to = dst == kNoId ? nullptr : ai().alife().objects().object(dst, true);
	if ((src != kNoId && (!from || from->m_bOnline)) || (dst != kNoId && (!to || to->m_bOnline)))
		return Reason::Offline; // half online: the client asks again once it settled
	if (from)
	{
		if (std::find(from->children.begin(), from->children.end(), item) == from->children.end())
			return Reason::Engine;
		alife_rw().graph().detach(*from, ii, from->m_tGraphID, true, true);
	}
	if (to)
		alife_rw().graph().attach(*to, ii, d->m_tGraphID, true, true);
	else if (drop_pos)
		d->o_Position = *drop_pos;
	events::host_on_parent_changed(item, src, dst); // no server event ran: relay it ourselves
	if (drop_pos && !to)
		events::host_set_drop_position(item, *drop_pos);
	return Reason::Ok;
}

// Ruling F2-32b: under lua_pcall, arg 1 the NPC's game object; returns the id of the player character leading its
// squad as a companion, or nothing. The leaders are the host's Lua (coop_npc_companions: squad id -> proxy id, saved in
// m_data.coop_npc_leaders, dropped when the squad stops being a companion squad); leader_of answers for an NPC.
int lua_companion_leader_id(lua_State* L)
{
	lua_getglobal(L, "coop_npc_companions"); // _G.__index loads the module if it was not yet
	if (!lua_istable(L, -1))
		return 0;
	lua_getfield(L, -1, "leader_of");
	if (!lua_isfunction(L, -1))
		return 0;
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1); // the leader's game object or nil
	if (lua_isnil(L, -1))
		return 0;
	lua_getfield(L, -1, "id");
	lua_pushvalue(L, -2);
	lua_call(L, 1, 1);
	return 1;
}

// Ruling F2-32b: is the live NPC `npc` (host numbering) a companion of character `proxy` (his squad's leader)? False
// for an NPC without a game object here, without a leader, or when the Lua call fails (logged).
bool companion_of(u16 npc, u16 proxy)
{
	CGameObject* go = find_go(npc);
	lua_State* L = go && go->lua_game_object() ? ai().script_engine().lua() : nullptr;
	if (!L || !lua_checkstack(L, 8))
		return false;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &lua_companion_leader_id);
	::luabind::object(L, go->lua_game_object()).pushvalue();
	if (lua_pcall(L, 1, 1, 0) != 0)
	{
		if (log_allowed())
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] coop_npc_companions.leader_of(%u) failed (not a companion here): %s", npc, err ? err : "?");
		}
		lua_settop(L, top);
		return false;
	}
	const bool is = lua_isnumber(L, -1) && u16(lua_tonumber(L, -1)) == proxy;
	lua_settop(L, top);
	return is;
}

// Ruling F2-32 (F2 T12 round 1): one side of a move for moves::check (xrCoop/coop_moves.h), from its top owner: the
// ground, the requester's own character, another player, a live NPC (and whether the requester holds its talk lock or
// leads its squad, Ruling F2-32b), a corpse or a box with their locks (the game object's flags, the server object's
// while offline), or anything else.
moves::Side move_side(const Req& q, u16 id)
{
	moves::Side s;
	if (id == kNoId)
		return s; // the ground
	const u16 root = events::root_of(id);
	if (root == q.proxy)
	{
		s.kind = moves::Side::Mine;
		return s;
	}
	if (is_player(root))
	{
		s.kind = moves::Side::Player;
		return s;
	}
	CSE_Abstract* e = events::find_entity(root);
	CGameObject* go = find_go(root);
	if (CSE_ALifeInventoryBox* box = smart_cast<CSE_ALifeInventoryBox*>(e))
	{
		s.kind = moves::Side::Box;
		const CInventoryBox* online = smart_cast<CInventoryBox*>(go);
		s.closed = online ? online->closed() : box->m_closed;
		s.can_take = online ? online->can_take() : box->m_can_take;
		return s;
	}
	if (CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(e))
	{
		const CEntityAlive* alive = smart_cast<CEntityAlive*>(go);
		const CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(e);
		if (alive ? !!alive->g_Alive() : (creature && creature->g_Alive()))
		{
			s.kind = moves::Side::LiveNpc;
			s.talking = dialog::host_lock_of(q.from) == root;
			s.companion = !s.talking && companion_of(root, q.proxy); // Ruling F2-32b
			return s;
		}
		s.kind = moves::Side::Corpse;
		const CInventoryOwner* owner = smart_cast<CInventoryOwner*>(go);
		s.closed = owner ? owner->deadbody_closed_status() : trader->m_deadbody_closed;
		s.can_take = owner ? owner->deadbody_can_take_status() : trader->m_deadbody_can_take;
		return s;
	}
	s.kind = moves::Side::Other;
	return s;
}

Reason host_move(const Req& q, u16 item, u16 src, u16 dst, u8 flags, const Fvector& pos)
{
	CSE_Abstract* e = events::find_entity(item);
	if (!e)
		return Reason::Missing;
	if (!smart_cast<CSE_ALifeInventoryItem*>(e) && !smart_cast<CSE_Temporary*>(e))
		return Reason::NotAllowed;
	if (e->ID_Parent != src)
		return Reason::Moved; // first valid request wins, this one is late
	if (src == dst)
		return Reason::Ok;
	const u16 root_src = src == kNoId ? kNoId : events::root_of(src);
	const u16 root_dst = dst == kNoId ? kNoId : events::root_of(dst);
	const bool mine_src = root_src == q.proxy;
	const bool mine_dst = root_dst == q.proxy;
	if (!mine_src && !mine_dst)
		return Reason::NotOwner;
	if (src != kNoId && !mine_src && is_player(root_src))
		return Reason::NotOwner; // never out of another player's pockets (gifts are given, R-6.7)
	if (dst != kNoId && !is_container(dst))
		return Reason::NotAllowed;
	// Ruling F2-32 (F2 T12 round 1): with Ruling F2-30 a client moves items out of another's inventory to its actor; a
	// live NPC's inventory, out or in, only under the requester's talk lock (a mechanic returning a repaired item, a
	// dialog) or as his own companion (Ruling F2-32b: the stock companion inventory has no dialog), box and corpse locks
	// as the stock UI keeps them. A stock script that moves against them is refused here, visibly.
	if (const moves::Verdict side = moves::check(move_side(q, src), move_side(q, dst)); side != moves::Verdict::Ok)
	{
		if (log_allowed())
			::Msg("[coop] %s #%u from player #%u: item %u from %u to %u refused: %s", msg_name(q.type), q.seq, q.from,
			    item, src, dst, moves::verdict_text(side));
		return side == moves::Verdict::NotOwner ? Reason::NotOwner : Reason::NotAllowed;
	}
	// R3 В8: the range is checked whatever the client's flags say. What a client applied locally (kMoveLocal: a
	// missile leaving its grenade, a rocket leaving our launcher) only ever leaves our own hands for the world, which
	// has no far side; with the flag on a take from the world or a container the range still holds.
	const u16 far_side = !mine_src ? (src == kNoId ? item : src) : (mine_dst ? kNoId : dst);
	if (far_side != kNoId && position_of(q.proxy).distance_to(position_of(far_side)) > kReachRange)
	{
		if ((flags & kMoveLocal) && note_bad(BadMoveRange))
			::Msg("! [coop] %s #%u from player #%u refused: item %u out of reach (claimed as already applied)",
			    msg_name(q.type), q.seq, q.from, item);
		return Reason::TooFar;
	}
	const bool exact = (flags & kMovePos) && dst == kNoId && validate::vec_finite(pos) &&
	    position_of(q.proxy).distance_to(pos) <= kDropPosRange;
	return apply_move(item, src, dst, exact ? &pos : nullptr);
}

void host_on_move(PeerId from, Reader& r, Msg type)
{
	Req q;
	if (!host_begin(from, r, type, q))
		return;
	const u16 item = r.u16();
	const u16 src = r.u16();
	const u16 dst = r.u16();
	float p[3];
	r.vec3(p);
	const u8 flags = r.u8();
	if (!r.ok())
		return reply(q, Reason::Malformed, kNoId);
	if (!validate::finite3(p)) // R3 В8: a fair client sends zeros when it has no drop position
	{
		if (note_bad(BadNumber))
			::Msg("! [coop] %s #%u from player #%u refused: position is not a number", msg_name(type), q.seq, from);
		return reply(q, Reason::Malformed, item);
	}
	if (!allow(from, CatMove))
		return reply(q, Reason::RateLimited, item);
	reply(q, host_move(q, item, src, dst, flags, Fvector().set(p[0], p[1], p[2])), item);
}

Reason host_destroy(const Req& q, u16 item)
{
	CSE_Abstract* e = events::find_entity(item);
	if (!e)
		return Reason::Missing;
	if (smart_cast<CSE_ALifeCreatureAbstract*>(e))
		return Reason::NotAllowed;
	const bool mine = events::root_of(item) == q.proxy ||
	    (e->ID_Parent == kNoId && events::host_last_holder(item) == q.proxy);
	if (!mine)
		return Reason::NotOwner;
	if (find_go(item))
	{
		// the standard chain (E3): the server releases it from ALife, the release hook tells clients
		NET_Packet P;
		CGameObject::u_EventGen(P, GE_DESTROY, item);
		CGameObject::u_EventSend(P);
		return events::find_entity(item) ? Reason::Engine : Reason::Ok;
	}
	if (CSE_ALifeDynamicObject* d = ai().alife().objects().object(item, true))
	{
		alife_rw().release(d, true);
		return Reason::Ok;
	}
	return Reason::Engine;
}

void host_on_destroy_req(PeerId from, Reader& r, Msg type)
{
	Req q;
	if (!host_begin(from, r, type, q))
		return;
	const u16 item = r.u16();
	if (!r.ok())
		return reply(q, Reason::Malformed, kNoId);
	if (!allow(from, CatDestroy))
		return reply(q, Reason::RateLimited, item);
	reply(q, host_destroy(q, item), item);
}

Reason host_hit(const Req& q, u16 target, SHit& hit)
{
	if (target == q.proxy)
		return Reason::NotAllowed; // a player hurts itself on its own machine
	if (!hit_sane(hit))
		return Reason::Malformed;
	CGameObject* victim = find_go(target);
	if (!victim)
		return events::find_entity(target) ? Reason::Offline : Reason::Missing;
	// R3 В5: the bone indexes the victim's skeleton unchecked (physics shell, hit scales, wounds: an AV here). A
	// model without a skeleton never uses it as a bone index (CPHShell::applyHit takes the rigid branch).
	if (IKinematics* k = smart_cast<IKinematics*>(victim->Visual()))
	{
		if (!validate::bone_ok(hit.boneID, k->LL_BoneCount()))
		{
			if (note_bad(BadBone))
				::Msg("! [coop] hit on %u [%s] from player #%u refused: bone %u, the model has %u", target,
				    victim->cNameSect().c_str(), q.from, u32(hit.boneID), u32(k->LL_BoneCount()));
			return Reason::Malformed;
		}
	}
	const bool player = is_player(target);
	if (player && !g_coop_friendly_fire)
		return Reason::FriendlyFire;
	if (player && target != kActor && world::is_parked(target))
		return Reason::NotAllowed;
	CGameObject* me = find_go(q.proxy);
	if (!me)
		return Reason::Offline;
	if (me->Position().distance_to(victim->Position()) > hit_range(hit.hit_type))
		return Reason::TooFar;
	CGameObject* weapon = nullptr;
	if (hit.weaponID != kNoId && events::root_of(hit.weaponID) == q.proxy)
		weapon = find_go(hit.weaponID);
	if (target == kActor)
	{
		apply_to_actor(hit, me, weapon); // the host player, same path as PlayerHit on a client
		return Reason::Ok;
	}
	// as the engine builds one (Actor.cpp fall damage): GE_HIT through the server, initiator = proxy
	hit.who = me;
	hit.whoID = q.proxy;
	hit.weaponID = weapon ? weapon->ID() : q.proxy;
	hit.GenHeader(GE_HIT, target);
	NET_Packet P;
	hit.Write_Packet(P);
	CGameObject::u_EventSend(P);
	return Reason::Ok;
}

void host_on_hit(PeerId from, Reader& r)
{
	Req q;
	if (!host_begin(from, r, Msg::ReqHit, q))
		return;
	const u16 target = r.u16();
	float shooter[3];
	r.vec3(shooter);
	SHit hit;
	read_hit(r, hit);
	if (!r.ok())
		return reply(q, Reason::Malformed, kNoId);
	if (!validate::finite3(shooter) || !hit_sane(hit))
	{
		if (note_bad(BadNumber))
			::Msg("! [coop] hit on %u from player #%u refused: a number out of range (type %u, power %.3g, impulse %.3g)",
			    target, from, u32(hit.hit_type), double(hit.power), double(hit.impulse));
		return reply(q, Reason::Malformed, target);
	}
	if (!allow(from, CatHit))
		return reply(q, Reason::RateLimited, target);
	const Reason why = host_hit(q, target, hit);
	if (why == Reason::Ok)
		++g_h.hits_applied; // hits are answered only when refused
	else
		reply(q, why, target);
}

// ---------------------------------------------------------------- upgrades at a mechanic (F2 T5)
// A client's upgrade is bought at a mechanic: the request names him and what the client's game charged for it
// (ledger::UpgradePayment), the host checks both (ledger::judge_upgrade) and charges the player's balance itself.
// Settings the stock upgrade scripts read (inventory_upgrades.script ini_file), each opened once per process like the
// Lua ones (CInifile: their DLTX mod files apply the same way) and kept for it.

struct UpgradeCounters
{
	u64 verdicts[ledger::kUpgradeVerdictCount] = {};
	u64 without_mechanic = 0; // requests that named no mechanic (a script installed the upgrade)
	u64 bought = 0, charged = 0, already = 0, engine = 0, no_cost = 0, not_his = 0, not_alive = 0, not_mechanic = 0;
	u64 answered_dropped = 0; // fix round 1 (review 5): requests host_begin drops silently, answered here
} g_upgrades;

// `missing`: what a missing file means, for the log (fix round 1, review 4: the line says what the host then does).
CInifile* open_game_config(LPCSTR name, LPCSTR missing)
{
	string_path path;
	FS.update_path(path, "$game_config$", name);
	if (!FS.exist(path))
	{
		::Msg("! [coop] configs\\%s not found: %s", name, missing);
		return nullptr;
	}
	return xr_new<CInifile>(path, TRUE, TRUE, FALSE);
}

// Mechanics: [<section>] (discount, tiers) and [<section>_upgr] (his own upgrade conditions).
CInifile* upgrade_info_ini()
{
	static CInifile* ini = open_game_config("misc\\stalkers_upgrade_info.ltx",
	    "no NPC is a mechanic, every client upgrade at one is refused");
	return ini;
}

// Upgrade prices as the Lua price reads them (char_ini): item_upgrades.ltx is not part of system.ltx.
CInifile* item_upgrades_ini()
{
	static CInifile* ini = open_game_config("item_upgrades.ltx",
	    "upgrade costs come from the game settings; an upgrade without one there is refused");
	return ini;
}

// A mechanic's section: the stock scripts read his discount and tiers from [<section>] of
// misc\stalkers_upgrade_info.ltx and his own conditions from [<section>_upgr]. Either one names him: the twelve
// Warfare mechanics (sim_default_*_mechanic) have only the first.
bool is_mechanic_section(const shared_str& section)
{
	CInifile* ini = upgrade_info_ini();
	if (!ini || !section.size() || section.size() > 200)
		return false;
	string256 upgr;
	xr_sprintf(upgr, "%s_upgr", section.c_str());
	return ini->section_exist(section) || ini->section_exist(upgr);
}

// `cost` of upgrade `upgrade` ([upgrade] section = <settings section>), read as the stock price does: from
// item_upgrades.ltx, else from the game settings. False: none (the stock price would fail on it).
bool upgrade_cost(const shared_str& upgrade, u32& cost)
{
	cost = 0;
	if (!pSettings->section_exist(upgrade) || !pSettings->line_exist(upgrade, "section"))
		return false;
	const LPCSTR sect = pSettings->r_string(upgrade, "section");
	if (!sect || !sect[0])
		return false;
	CInifile* ini = item_upgrades_ini();
	if (ini && ini->section_exist(sect) && ini->line_exist(sect, "cost"))
	{
		cost = ini->r_u32(sect, "cost");
		return true;
	}
	if (pSettings->section_exist(sect) && pSettings->line_exist(sect, "cost"))
	{
		cost = pSettings->r_u32(sect, "cost");
		return true;
	}
	return false;
}

// `mechanic` (host numbering) sells upgrades to player `from` now (ledger::check_mechanic): he holds that NPC's talk
// lock (the stock upgrade windows run inside the mechanic's dialog; coop_dialog); it is a live NPC with an inventory, a
// stalker or a trader class (fix round 1, review 3: the Forester, m_lesnik : m_trader, is a CAI_Trader), and not a
// player's character; the mechanics' settings name its section.
bool host_his_mechanic(PeerId from, u16 mechanic, const char*& why)
{
	ledger::MechanicFacts m;
	m.talk_lock = dialog::host_lock_of(from) == mechanic;
	CGameObject* go = m.talk_lock ? smart_cast<CGameObject*>(Level().Objects.net_Find(mechanic)) : nullptr;
	if (go && go->getDestroy())
		go = nullptr;
	CEntityAlive* npc = go ? smart_cast<CEntityAlive*>(go) : nullptr;
	m.inventory_owner = go && smart_cast<CInventoryOwner*>(go);
	m.alive = npc && npc->g_Alive();
	m.player = go && world::is_player_character(go);
	m.listed = go && is_mechanic_section(go->cNameSect());
	switch (ledger::check_mechanic(m))
	{
	case ledger::MechanicCheck::Ok: return true;
	case ledger::MechanicCheck::NotHisTalk:
		++g_upgrades.not_his;
		why = "he does not talk to him";
		return false;
	case ledger::MechanicCheck::NotAlive:
		++g_upgrades.not_alive;
		why = "not a live NPC with an inventory";
		return false;
	case ledger::MechanicCheck::NotMechanic:
		++g_upgrades.not_mechanic;
		why = "not a mechanic";
		return false;
	}
	return false;
}

const char* upgrade_verdict_name(ledger::UpgradeVerdict v)
{
	switch (v)
	{
	case ledger::UpgradeVerdict::Ok: return "ok";
	case ledger::UpgradeVerdict::Scripted: return "no mechanic named (scripted_upgrades = false)";
	case ledger::UpgradeVerdict::NoMechanic: return "no mechanic";
	case ledger::UpgradeVerdict::TooFar: return "too far from the mechanic";
	case ledger::UpgradeVerdict::UnknownCost: return "the upgrade's cost is unknown here";
	case ledger::UpgradeVerdict::Underpaid: return "underpaid";
	case ledger::UpgradeVerdict::Insufficient: return "not enough money on the host";
	}
	return "?";
}

// The brief's checks before the upgrade's own (host_check_added_upgrade): a mechanic, within kTradeReach of the
// player's character, a cost the host knows (fix round 1, review 4), paid at least floor(cost x upgrade_min_percent),
// and the player's balance on the host covers it. Refusals are NotAllowed on the wire, counted by cause.
Reason host_sell_upgrade(const Req& q, const shared_str& upgrade, const ledger::UpgradePayment& pay)
{
	ledger::UpgradeFacts f;
	f.mechanic_named = pay.mechanic != kNoId;
	f.scripted_allowed = money::scripted_upgrades();
	const char* not_mechanic = nullptr;
	f.mechanic_ok = f.mechanic_named && host_his_mechanic(q.from, pay.mechanic, not_mechanic);
	f.distance = f.mechanic_ok ? talk_distance(pay.mechanic, q.proxy) : flt_max;
	f.reach = trade::kTradeReach;
	f.paid = pay.paid;
	f.cost_known = upgrade_cost(upgrade, f.cost);
	if (f.mechanic_named && !f.cost_known)
		++g_upgrades.no_cost; // refused (UnknownCost) when the mechanic is right and near
	f.min_percent = money::upgrade_min_percent();
	f.balance = money::host_balance(q.from);
	if (!f.mechanic_named)
		++g_upgrades.without_mechanic;
	const ledger::UpgradeVerdict v = ledger::judge_upgrade(f);
	++g_upgrades.verdicts[u32(v)];
	if (v == ledger::UpgradeVerdict::Ok)
		return Reason::Ok;
	if (log_allowed())
		::Msg("! [coop] upgrade [%s] from player #%u refused: %s%s%s (mechanic %u, %.1f m; paid %u RU, cost %u RU x %.2f; "
		      "balance %u RU)",
		    upgrade.c_str(), q.from, upgrade_verdict_name(v), not_mechanic ? ": " : "", not_mechanic ? not_mechanic : "",
		    pay.mechanic, f.mechanic_ok ? double(f.distance) : -1.0, f.paid, f.cost, double(f.min_percent), f.balance);
	return Reason::NotAllowed;
}

// Every upgrade request is answered, and when the client's game paid for it the player's balance follows the answer:
// his client holds the payment out of the money it shows until the answer (money::client_hold), so the next balance
// is exactly the host's, charged or not.
void answer_upgrade(const Req& q, Reason why, u16 item, u32 paid)
{
	reply(q, why, item);
	if (paid)
		money::host_send_balance(q.from);
}

// A ReqItemEvent after its seq: u16 item, u16 ge_type, u16 size, the event bytes, the payment (F2 T5).
struct ItemEventBody
{
	u16 item = kNoId, type = 0, size = 0;
	u8 data[512];
	ledger::UpgradePayment pay;
};

bool read_item_event(Reader& r, ItemEventBody& b)
{
	b.item = r.u16();
	b.type = r.u16();
	b.size = r.u16();
	return r.ok() && b.size <= sizeof(b.data) && r.bytes(b.data, b.size) && ledger::read(r, b.pay);
}

void host_on_item_event(PeerId from, Reader& r)
{
	// Fix round 1 (review 5): host_begin drops a request silently while its player joins or a world is precached. The
	// client of an upgrade holds what it paid until the answer, so this one is answered: Offline (his world is being
	// replaced; the new one brings the host's state of his items), with the balance after it.
	if (mgr().is_host() && host_drops_requests_of(from))
	{
		Reader peek = r;
		Req dropped;
		dropped.from = from;
		dropped.type = Msg::ReqItemEvent;
		dropped.seq = peek.u32();
		ItemEventBody b;
		if (read_item_event(peek, b))
		{
			++g_upgrades.answered_dropped;
			answer_upgrade(dropped, Reason::Offline, b.item, b.pay.paid);
		}
	}
	Req q;
	if (!host_begin(from, r, Msg::ReqItemEvent, q))
		return;
	ItemEventBody b;
	if (!read_item_event(r, b))
		return reply(q, Reason::Malformed, kNoId);
	const u16 item = b.item;
	const u16 type = b.type;
	const u16 size = b.size;
	const u8* data = b.data;
	const ledger::UpgradePayment& pay = b.pay; // F2 T5: the event bytes are followed by the payment
	const auto answer = [&q, item, &pay](Reason why) { answer_upgrade(q, why, item, pay.paid); };
	if (!allow(from, CatMisc))
		return answer(Reason::RateLimited); // fix round 1 (review 5): with the balance, as every other answer
	if (type != GE_INSTALL_UPGRADE)
		return answer(Reason::NotAllowed);
	if (!events::find_entity(item))
		return answer(Reason::Missing);
	if (events::root_of(item) != q.proxy)
		return answer(Reason::NotOwner);
	// R3 В6 (task 10b): the upgrade id ends inside the event bytes (r_stringZ reads up to a zero) and is a name.
	if (!size || !std::memchr(data, 0, size))
		return answer(Reason::Malformed);
	shared_str upgrade;
	{
		NET_Packet probe;
		probe.B.count = size;
		CopyMemory(probe.B.data, data, size);
		probe.r_pos = 0;
		probe.r_stringZ(upgrade);
	}
	if (!validate::resource_name_ok(upgrade.c_str(), spawn::kMaxSection))
		return answer(Reason::Malformed);
	// F2 T5: bought at a mechanic nearby and paid enough, with the money on the host
	const Reason sold = host_sell_upgrade(q, upgrade, pay);
	if (sold != Reason::Ok)
		return answer(sold);
	// The server entity keeps it in the host save, and its next net_Spawn from scratch installs the section
	// defaults, its list, then it with loading = true: FATAL on a taken group or a missing parent group. A
	// repeat must not reach CSE_ALifeInventoryItem::add_upgrade either (FATAL on duplicates).
	CSE_Abstract* e = ai().alife().objects().object(item, true);
	const char* why = nullptr;
	switch (items::host_check_added_upgrade(e, upgrade, why))
	{
	case items::AddedUpgrade::AlreadyThere:
		++g_upgrades.already;
		return answer(Reason::Ok); // nothing to do, nothing charged: the balance tells his client
	case items::AddedUpgrade::Refused:
		if (note_bad(BadUpgrade))
			::Msg("! [coop] upgrade [%s] of item %u from player #%u refused: %s", upgrade.c_str(), item, from,
			    why ? why : "?");
		return answer(Reason::NotAllowed);
	case items::AddedUpgrade::Relay: break;
	}
	NET_Packet P;
	CGameObject::u_EventGen(P, type, item);
	P.w(data, size);
	CGameObject::u_EventSend(P); // the server entity keeps the upgrade in the host save (local server: at once)
	// F2 T5: charged only for what the entity took; an engine refusal costs nothing.
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(e);
	if (!ii || !ii->has_upgrade(upgrade))
	{
		++g_upgrades.engine;
		return answer(Reason::Engine);
	}
	++g_upgrades.bought;
	// The answer first, then the charge, whose MoneyBalance is the first after it (one reliable lane).
	reply(q, Reason::Ok, item);
	if (pay.paid && money::host_adjust(from, -s64(pay.paid), "upgrade"))
		g_upgrades.charged += pay.paid;
	// Fix round 1 (review 1): the host's game object of the item takes it now (the owner's ItemState never adds one),
	// and the other players' copies get the host's state of the item.
	items::host_upgrade_sold(item, from);
}

// ---------------------------------------------------------------- rockets (W3a)
// A client's rocket / launcher grenade: the client launches its copy at once (a visual, it never detonates
// there, WP7) and sends the launch; the host launches its own copy the way the weapons do, so the host
// flies and detonates it (WP7 relays the explosion), and its destroy removes every copy (EntityLocal).

constexpr float kLaunchReach = 6.f; // player character <-> launch point, with lag slack
constexpr float kMaxLaunchSpeed = 400.f; // stock launchers: 32..121 m/s
constexpr float kMaxLaunchSpin = 50.f; // rad/s; stock launchers use 0
constexpr u32 kLaunchIntervalMs = 100;

struct Launch
{
	u16 launcher = kNoId, rocket = kNoId; // local numbering
	Fvector pos{}, dir{}, vel{}, ang{};
};
xr_vector<Launch> g_launches; // under g_out_lock: the router may run on the game thread
xr_map<PeerId, u64> g_last_launch;

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

void client_flush_launches()
{
	xr_vector<Launch> out;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		out.swap(g_launches);
	}
	for (const Launch& l : out)
	{
		Writer w(64);
		w.u32(track(Msg::ReqLaunch, l.rocket));
		w.u16(to_host_id(l.launcher));
		w.u16(to_host_id(l.rocket));
		put_vec(w, l.pos);
		put_vec(w, l.dir);
		put_vec(w, l.vel);
		put_vec(w, l.ang);
		net::client_send(Msg::ReqLaunch, w);
	}
}

// A refused launch: the requester's copy already flies, so the rocket must not stay in its launcher here.
// The destroy reaches every client (EntityLocal op 1) and removes their copies. Only while it is still
// in the requester's hands (a free one is already flying: a repeated request).
void host_discard_rocket(const Req& q, u16 rocket)
{
	CSE_Abstract* e = events::find_entity(rocket);
	if (!e || e->ID_Parent == kNoId || events::root_of(rocket) != q.proxy || !find_go(rocket))
		return;
	NET_Packet P;
	CGameObject::u_EventGen(P, GE_DESTROY, rocket);
	CGameObject::u_EventSend(P);
}

Reason host_launch(const Req& q, u16 launcher, u16 rocket, Fvector pos, Fvector dir, Fvector vel, Fvector ang)
{
	// Nothing from the wire reaches the physics world unchecked (R3: through coop::validate). The direction is a unit
	// vector: a huge one normalizes to zero and the launch matrix to NaN.
	if (!validate::vec_finite(pos) || !validate::vec_speed_ok(dir, validate::kMaxDirLength) ||
	    !validate::vec_finite(vel) || !validate::vec_finite(ang) || dir.magnitude() < EPS_L)
	{
		if (note_bad(BadNumber))
			::Msg("! [coop] ReqLaunch #%u of rocket %u from player #%u refused: a vector is not a number", q.seq, rocket,
			    q.from);
		return Reason::Malformed;
	}
	const u64 now = mgr().now_ms();
	u64& last = g_last_launch[q.from];
	if (now - last < kLaunchIntervalMs)
		return Reason::RateLimited;
	CSE_Abstract* re = events::find_entity(rocket);
	if (!re || !events::find_entity(launcher))
		return Reason::Missing;
	if (re->ID_Parent != launcher)
		return Reason::Moved; // launched already, or not loaded in that launcher
	if (events::root_of(launcher) != q.proxy)
		return Reason::NotOwner;
	CGameObject* launcher_go = valid_go(launcher);
	CCustomRocket* rocket_go = smart_cast<CCustomRocket*>(valid_go(rocket));
	CGameObject* me = find_go(q.proxy);
	if (!launcher_go || !rocket_go || !me)
		return Reason::Offline;
	if (!smart_cast<CRocketLauncher*>(launcher_go) || rocket_go->H_Parent() != launcher_go)
		return Reason::NotAllowed; // not a launcher, or the rocket is not loaded in it here
	// The character here trails the player by the snapshot delay; a launch point out of its reach starts at it.
	if (me->Position().distance_to(pos) > kLaunchReach)
		pos.set(me->Position()).y += 1.5f;
	dir.normalize();
	if (!validate::vec_speed_ok(vel, kMaxLaunchSpeed))
		vel.normalize_safe().mul(kMaxLaunchSpeed);
	if (!validate::vec_speed_ok(ang, kMaxLaunchSpin))
		ang.normalize_safe().mul(kMaxLaunchSpin);
	Fmatrix xf; // as the weapons build it (CWeaponRPG7::switch2_Fire, CWeaponMagazinedWGrenade::LaunchGrenade)
	xf.identity();
	xf.k.set(dir);
	Fvector::generate_orthonormal_basis(xf.k, xf.j, xf.i);
	xf.c.set(pos);
	// CRocketLauncher::LaunchRocket on this very rocket: the host's launcher copy may also hold a rocket it
	// spawned itself in net_Spawn, so its "current" rocket is not necessarily the client's. The launched-list
	// bookkeeping LaunchRocket adds only feeds the second detach branch in DetachRocket.
	rocket_go->SetLaunchParams(xf, vel, ang);
	if (CExplosive* ex = rocket_go->cast_explosive())
		ex->SetInitiator(q.proxy); // the blast is the player's (kills, PlayerHit for its own character)
	NET_Packet P;
	CGameObject::u_EventGen(P, GE_LAUNCH_ROCKET, launcher);
	P.w_u16(rocket);
	CGameObject::u_EventSend(P); // server: detach + EntityParent relay; launcher: DetachRocket(rocket, true)
	if (re->ID_Parent != kNoId)
		return Reason::Engine;
	last = now;
	events::host_relay_launch(rocket, launcher, pos, dir, vel, ang, q.from);
	return Reason::Ok;
}

void host_on_launch(PeerId from, Reader& r)
{
	Req q;
	if (!host_begin(from, r, Msg::ReqLaunch, q))
		return;
	const u16 launcher = r.u16();
	const u16 rocket = r.u16();
	const Fvector pos = get_vec(r);
	const Fvector dir = get_vec(r);
	const Fvector vel = get_vec(r);
	const Fvector ang = get_vec(r);
	if (!r.ok())
		return reply(q, Reason::Malformed, kNoId);
	const Reason why = host_launch(q, launcher, rocket, pos, dir, vel, ang);
	if (why != Reason::Ok)
		host_discard_rocket(q, rocket);
	reply(q, why, rocket);
}
} // namespace

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
	default: return "?";
	}
}

void install(Session& s)
{
	load_spawn_settings(); // F2 T2
	s.on(Msg::ReqSpawn, &host_on_spawn);
	s.on(Msg::ReqDestroy, [](PeerId from, Reader& r) { host_on_destroy_req(from, r, Msg::ReqDestroy); });
	s.on(Msg::ReqUse, [](PeerId from, Reader& r) { host_on_destroy_req(from, r, Msg::ReqUse); });
	s.on(Msg::ReqTake, [](PeerId from, Reader& r) { host_on_move(from, r, Msg::ReqTake); });
	s.on(Msg::ReqDrop, [](PeerId from, Reader& r) { host_on_move(from, r, Msg::ReqDrop); });
	s.on(Msg::ReqTransfer, [](PeerId from, Reader& r) { host_on_move(from, r, Msg::ReqTransfer); });
	s.on(Msg::ReqHit, &host_on_hit);
	s.on(Msg::ReqItemEvent, &host_on_item_event); // ReqMoney: coop_money (F2 T1)
	s.on(Msg::ReqLaunch, &host_on_launch); // W3a
	s.on(Msg::ReqResult, [](PeerId from, Reader& r) { if (mgr().is_client()) client_on_result(from, r); });
	s.on(Msg::IdLease, &client_on_lease); // F2 T12b
	s.on(Msg::IdLeaseAck, &host_on_lease_ack); // F2 T12b
	s.on(Msg::PlayerHit, [](PeerId from, Reader& r) { if (mgr().is_client()) client_on_player_hit(from, r); });
}

void reset()
{
	{
		xrCriticalSectionGuard guard(g_out_lock);
		g_outbox.clear();
		g_child_spawns.clear();
		g_launches.clear(); // W3a
	}
	g_inflight.clear();
	g_rebuilds.clear(); // F2 T5 fix round 1
	g_limits.clear();
	g_spawn_budgets.clear(); // F2 T2: a session starts or ends (Ruling F2-11: kept across a peer leaving and rejoining)
	g_spawn_queue.clear(drop_queued); // Ruling F2-21: nothing of the old session is created
	g_last_launch.clear(); // W3a
	g_log_count = 0;
	// Lua may still hold these pointers; reset runs when a session ends, long after any script used them. Round 1 (review
	// I2): only those of this world; an old world's Lua state is closed.
	if (g_temps_world == join::world_generation())
		for (CSE_Abstract* e : g_lua_temps)
			F_entity_Destroy(e);
	else
		g_lc.temps_left += g_lua_temps.size();
	g_lua_temps.clear();
	g_lua_forwarded.clear();
	// F2 T12b: the session's leases go; in the world they came from, back into its generator
	if (g_leases.total() && g_leases.epoch() == join::world_generation())
		host_free_ids(g_leases.drop_all());
	g_leases.clear();
	{
		xrCriticalSectionGuard guard(g_out_lock);
		g_pool.clear();
		g_specs.clear();
		g_spec_actions.clear();
		g_temp_leases.clear(); // their objects went with g_lua_temps
		g_give_back.clear();
		g_give_back_wait.clear();
		g_held.clear([](Action&) {}); // money holds go with the session (money::reset)
		g_spawn_backlog.clear(); // round 2
		g_spawn_pacer.reset();
	}
	quarantine_expire(true);
}

void on_peer_left(PeerId id)
{
	// Ruling F2-21, review round 4 minor: his waiting spawn requests go at once, unanswered.
	const auto his = [id](const spawn::QueuedRequest& r) { return PeerId(r.peer) == id; };
	for (const std::string& key : g_spawn_queue.keys())
		g_spawn_queue.drop_if(key, his, drop_queued);
	if (mgr().is_host())
	{
		// F2 T12b: what he still held goes back to this world's generator
		host_sync_leases();
		host_free_ids(g_leases.drop(id));
	}
}

void frame()
{
	if (mgr().is_host())
	{
		host_pump_spawns(); // Ruling F2-21
		host_pump_leases(); // F2 T12b
	}
	if (!mgr().is_client())
		return;
	client_flush_launches(); // W3a: before the outbox (a launch usually precedes a drop of the launcher)
	client_pump_leases(); // F2 T12b: retries go out with this frame's outbox
	client_flush();
	if (mgr().is_live_client())
		client_pump_rebuilds(); // F2 T5 fix round 1
}

void register_console()
{
	CMD4(CCC_Integer, "coop_client_hit_perks", &g_coop_client_hit_perks, 0, 1);
}

void print_status()
{
	if (mgr().is_host())
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
	// sent[5] was ReqMoney (now coop_money, F2 T1)
	::Msg("[coop]   requests out: spawn %llu, destroy %llu, use %llu, move %llu, hit %llu, item event %llu; "
	      "%llu accepted, %llu refused, %llu unanswered, %u in flight; %llu while not live, %llu foreign moves, %llu "
	      "duplicate child spawns; lua %llu forwarded / %llu refused; %llu hits received",
	    g_cl.sent[0], g_cl.sent[1], g_cl.sent[2], g_cl.sent[3], g_cl.sent[4], g_cl.sent[6],
	    g_cl.accepted, g_cl.rejected, g_cl.timeouts, (u32)g_inflight.size(), g_cl.not_live, g_cl.foreign_moves,
	    g_cl.child_dups, g_cl.lua_forwarded, g_cl.lua_refused, g_cl.hits_in);
	::Msg("[coop]   hit perks (%s): %llu hits through the before-hit Lua, %llu dropped by it, %llu lua errors",
	    g_coop_client_hit_perks ? "on" : "off", g_cl.hits_perked, g_cl.hits_cancelled, g_cl.perk_errors);
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
	{
		xrCriticalSectionGuard guard(g_out_lock);
		backlog = g_spawn_backlog.size();
	}
	::Msg("[coop]   leased ids, round 2: %u spawn requests wait for the pace (%llu frames paced), %llu creates in the "
	      "host's budget queue, %llu asked again for want of an answer, %llu taken back unanswered, %llu clones refused "
	      "as not a variant",
	    (u32)backlog, g_lc.paced, g_lc.queued, g_lc.resent, g_lc.unanswered, g_lc.not_variant);
}

// ---------------------------------------------------------------- client API

bool client_forward_spawn(CSE_Abstract* e)
{
	if (!e || !mgr().is_client())
		return false;
	const u16 parent = e->ID_Parent;
	if (parent == kNoId || events::root_of(parent) != kActor)
		return false; // world-caused: loot, population, effects are the host's job
	if (!mgr().is_live_client())
	{
		++g_cl.not_live;
		return true;
	}
	// A transient child of an item (fake missile in a grenade, rocket in a launcher) is asked for
	// once per holder until it arrives; the engine retries every throw start.
	CSE_Abstract* holder = events::find_entity(parent);
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
	// R3 В6: the host builds the entity from the section; the item state goes as a coop_items blob, which the host
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
	if (!mgr().is_live_client())
	{
		++g_cl.not_live;
		return;
	}
	Action a;
	a.kind = use ? Act::Use : Act::Destroy;
	a.item = local_item;
	push(std::move(a));
}

void client_move(u16 local_item, u16 from, u16 to, const Fvector* pos, u8 flags)
{
	if (!mgr().is_live_client())
	{
		++g_cl.not_live;
		return;
	}
	xrCriticalSectionGuard guard(g_out_lock);
	if (to != kNoId && !(flags & kMoveLocal))
	{
		// reject + take of one item right after each other is one transfer (box <-> actor, trade).
		// `from` of the take is the local holder, which lags behind our own requests still on the
		// way (box -> actor -> box before the first answer): the reject names the real source.
		for (auto it = g_outbox.rbegin(); it != g_outbox.rend(); ++it)
		{
			if (it->kind != Act::Move || it->item != local_item)
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
		if (it->kind == Act::Move && it->item == local_item && it->to == kNoId)
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
	if (!mgr().is_live_client())
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

void client_money(u32 money)
{
	money::client_on_actor_money(money); // F2 T1: a change with a cause, the host keeps the balance
}

void client_flush_outbox()
{
	frame(); // exactly what the end of this frame would send, in the same order
	money::client_flush(); // F2 T1: and the money changes made before
}

void client_item_event(u16 local_item, u16 ge_type, const void* data, u32 size)
{
	if (!mgr().is_live_client() || size > 512)
		return;
	Action a;
	a.kind = Act::ItemEvent;
	a.item = local_item;
	a.ge_type = ge_type;
	a.bytes.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
	// F2 T5: an upgrade bought at a mechanic carries him and its price (items::UpgradePurchase); else none, nothing
	if (ge_type == GE_INSTALL_UPGRADE)
		items::claim_upgrade_payment(local_item, a.mechanic, a.paid, a.hold);
	push(std::move(a));
}

void client_player_died(u16 local_killer)
{
	if (!mgr().is_live_client())
		return;
	Action a;
	a.kind = Act::Died;
	a.item = local_killer;
	push(std::move(a));
}

bool client_launch(u16 local_launcher, u16 local_rocket)
{
	if (!mgr().is_live_client())
	{
		++g_cl.not_live;
		return true; // nothing goes out before the world is live
	}
	CCustomRocket* rocket = smart_cast<CCustomRocket*>(find_go(local_rocket));
	if (!rocket)
		return false;
	// The launcher has just called LaunchRocket on it (the event follows the call in every weapon).
	Fmatrix xf;
	Launch l;
	rocket->coop_launch_params(xf, l.vel, l.ang);
	l.launcher = local_launcher;
	l.rocket = local_rocket;
	l.pos = xf.c;
	l.dir = xf.k;
	xrCriticalSectionGuard guard(g_out_lock);
	g_launches.push_back(l);
	return true;
}

// ---------------------------------------------------------------- Lua wrappers

bool lua_client()
{
	const Phase p = mgr().phase();
	return mgr().is_client() && (p == Phase::LoadingWorld || p == Phase::Live);
}

CSE_Abstract* lua_create(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex, u16 parent,
    bool register_now, int ammo)
{
	temps_sync_world(); // round 1 (review I2)
	CSE_Abstract* e = make_temp(section, pos, level_vertex, game_vertex, parent);
	if (!e)
	{
		::Msg("! [coop] alife():create: unknown section [%s]", section ? section : "");
		return nullptr;
	}
	if (ammo >= 0)
		if (CSE_ALifeItemAmmo* box = smart_cast<CSE_ALifeItemAmmo*>(e))
			box->a_elapsed = u16(_min(ammo, int(box->m_boxSize)));
	// F2 T12b: an item into our actor takes a leased id and is ours at once (nil only without one)
	const u16 id = take_lease(section, parent);
	if (!register_now)
	{
		g_lua_temps.push_back(e); // the script edits it, then calls alife():register(se)
		if (id != kNoId)
		{
			e->ID = id; // a script keys its data by it before the register (m_data, item parts)
			name_with_id(e); // round 1 (review M8): se_obj:name() as in single player
			xrCriticalSectionGuard guard(g_out_lock);
			g_temp_leases[e] = LeasedTemp{id, kNoId};
			++g_lc.temps;
		}
		return e;
	}
	if (id != kNoId)
	{
		if (create_leased(e, id, kNoId))
			return e;
		return_unused_lease(id); // round 1 (review M1)
	}
	lua_forward(e, "alife:create");
	F_entity_Destroy(e);
	return nullptr;
}

CSE_Abstract* lua_clone(CSE_Abstract* source, const char* section, const Fvector& pos, u32 level_vertex,
    u16 game_vertex, u16 parent, bool register_now)
{
	temps_sync_world(); // round 1 (review I2)
	CSE_ALifeItemWeaponMagazined* from = smart_cast<CSE_ALifeItemWeaponMagazined*>(source);
	if (!from)
		return nullptr; // as the engine: only magazined weapons are cloned
	// F2 T12b round 1 (review C1): a weapon the host has not made yet, or one a clone of ours replaces already, is not
	// cloned (nil; coop_mods_guard tells the player before the mod's own creates)
	if (lease_waiting(from->ID))
	{
		++g_lc.clones_refused;
		return lua_refuse("alife:clone_weapon", "its weapon waits for the host (a create or a clone of ours in flight)");
	}
	// round 2 (review N3): a variant of the same weapon only (the host refuses another: its source's rounds would become
	// another ammo); a plain create would leave the source beside the clone
	if (!section || !clone_variant(from->s_name.c_str(), section))
	{
		++g_lc.not_variant;
		return lua_refuse("alife:clone_weapon", "not a variant of its weapon (scope, magnifier)");
	}
	CSE_Abstract* e = make_temp(section, pos, level_vertex, game_vertex, parent);
	CSE_ALifeItemWeaponMagazined* clone = smart_cast<CSE_ALifeItemWeaponMagazined*>(e);
	if (!clone)
	{
		if (e)
			F_entity_Destroy(e);
		return nullptr;
	}
	clone->wpn_flags = from->wpn_flags;
	clone->m_addon_flags = from->m_addon_flags;
	clone->m_fCondition = from->m_fCondition;
	clone->ammo_type = from->ammo_type;
	clone->m_upgrades = from->m_upgrades;
	clone->set_ammo_elapsed(from->get_ammo_elapsed());
	// F2 T12b: a clone of our own weapon into our actor takes a leased id; the host releases the source with it
	const u16 id = callgate::is_own(from->ID) ? take_lease(section, parent) : kNoId;
	if (!register_now)
	{
		g_lua_temps.push_back(e);
		if (id != kNoId)
		{
			e->ID = id; // _g.alife_clone_weapon keys m_data and the parts by it before the register
			name_with_id(e); // round 1 (review M8)
			xrCriticalSectionGuard guard(g_out_lock);
			g_temp_leases[e] = LeasedTemp{id, from->ID};
			++g_lc.temps;
		}
		return e;
	}
	if (id != kNoId)
	{
		if (create_leased(e, id, from->ID))
			return e;
		return_unused_lease(id); // round 1 (review M1)
	}
	lua_forward(e, "alife:clone_weapon");
	F_entity_Destroy(e);
	return nullptr;
}

CSE_Abstract* lua_register_spawn(CSE_Abstract* object)
{
	temps_sync_world(); // round 1 (review I2)
	const auto temp = std::find(g_lua_temps.begin(), g_lua_temps.end(), object);
	if (temp == g_lua_temps.end())
		return lua_refuse("alife:register", "an object the host did not create");
	if (g_lua_forwarded.count(object))
		return lua_refuse("alife:register", "a second register of one object"); // would be a duplicate
	// F2 T12b: a temporary object with a leased id is ours at once: register answers it, as in single player
	LeasedTemp leased;
	if (take_temp_lease(object, leased))
	{
		if (create_leased(object, leased.id, leased.source))
		{
			g_lua_temps.erase(temp); // our ALife's entity now, not a temporary object to free at the session's end
			return object;
		}
		object->ID = kNoId;
		return_unused_lease(leased.id); // round 1 (review M1)
		if (leased.source != kNoId)
		{
			// round 1 (review M1): its source's release may have gone with it already (stock _g.alife_clone_weapon releases
			// before the register): a clone by the request path now would leave both
			g_lua_forwarded.insert(object);
			return lua_refuse("alife:register", "a clone whose leased id could not be used here");
		}
	}
	g_lua_forwarded.insert(object);
	return lua_forward(object, "alife:register");
}

bool lua_release(CSE_Abstract* object)
{
	if (!object)
		return true;
	temps_sync_world(); // round 1 (review I2)
	if (std::find(g_lua_temps.begin(), g_lua_temps.end(), object) != g_lua_temps.end())
	{
		// never created: nothing to release; F2 T12b: its leased id serves the next create
		LeasedTemp leased;
		if (take_temp_lease(object, leased))
		{
			object->ID = kNoId;
			xrCriticalSectionGuard guard(g_out_lock);
			g_pool.put_back(leased.id);
		}
		return true;
	}
	const u16 id = object->ID;
	// F2 T12b: the weapon a clone of ours replaces: the host releases it with the clone (and keeps it if it refuses the
	// clone), so this release is no request of its own (_g.alife_clone_weapon releases it before or after the register)
	if (clone_source(id))
	{
		++g_lc.released_with_clone;
		return true;
	}
	// F2 T12: the call gate decides; our own items (not our actor) have the client_destroy path. Round 1: as before
	// T12 it never runs the stock release while a host command is applied (the client's ALife copy is the host's).
	callgate::Args args;
	args.runs_while_applying = false;
	switch (callgate::client_gate_own("alife:release", calls::own_release(id == kActor, callgate::is_own(id)), args))
	{
	case calls::Gate::Run: return false; // an ltx override runs it here
	case calls::Gate::Refuse: ++g_cl.lua_refused; return true; // the gate logged it
	case calls::Gate::Request: break;
	}
	client_destroy(id, false); // ours: the host releases it and tells everybody
	return true;
}

CSE_Abstract* lua_refuse(const char* name, const char* why)
{
	++g_cl.lua_refused;
	callgate::note_refused(name, why); // F2 T12: counted and logged with the gate's refusals
	return nullptr;
}

bool keep_released(CSE_Abstract* entity)
{
	if (!entity)
		return false;
	xrCriticalSectionGuard guard(g_quarantine_lock);
	if (!g_quarantine_marks.erase(entity))
		return false;
	g_quarantine.keep(entity, mgr().now_ms(), join::world_generation());
	return true;
}

int lua_lease_waiting(lua_State* L)
{
	bool waiting = false;
	if (lua_isnumber(L, 1) && lease_world_ready())
	{
		const lua_Number id = lua_tonumber(L, 1);
		if (id >= 0 && id < lua_Number(kNoId))
			waiting = lease_waiting(u16(id));
	}
	lua_pushboolean(L, waiting ? 1 : 0);
	return 1;
}

int lua_lease_ids(lua_State* L)
{
	size_t n = 0;
	if (lease_world_ready())
	{
		xrCriticalSectionGuard guard(g_out_lock);
		lease_sync_world_locked();
		n = g_pool.size();
	}
	lua_pushnumber(L, lua_Number(n));
	return 1;
}

bool lua_client_refuse(const char* name, u16 target, bool runs_while_applying)
{
	if (!lua_client())
		return false;
	// F2 T12: the call gate decides. These calls have no request path, so a client refuses them unless an ltx override
	// runs them here (or, round 1, a host command is being applied and the caller allows it); the gate counts and logs.
	// Round 1 (review M4): the target must exist in this world, a missing id would crash the ALife call.
	callgate::Args args;
	args.id = target;
	args.runs_while_applying = runs_while_applying;
	if (callgate::client_gate_id(name, target, args) == calls::Gate::Run)
		return false;
	++g_cl.lua_refused;
	return true;
}

// ---------------------------------------------------------------- damage

bool stalker_hit(CAI_Stalker* npc, SHit* hit)
{
	if (!npc || !hit || !mgr().active())
		return false;
	if (mgr().is_client())
	{
		// host creatures and other players' avatars take damage on their owner's machine only
		if (entities::client_applying())
			return false;
		return sync::is_remote_driven(npc);
	}
	if (!mgr().is_host())
		return false;
	const u16 id = npc->ID();
	if (!world::is_player_proxy(id))
		return false;
	++g_h.proxy_hits;
	if (world::is_parked(id))
		return true; // an offline player's character takes no damage
	const PeerId peer = world::avatar_peer(id);
	if (peer == kInvalidPeer || sync::host_peer_joining(peer))
		return true;
	CGameObject* who = hit->who ? smart_cast<CGameObject*>(hit->who) : nullptr;
	hits::ProxyHit h;
	h.source = hit_source(npc, who);
	// The player's own grenade/rocket explodes on the host with this character as initiator (WP7):
	// blast and fragments name the explosive as the weapon. Clients deal no explosion damage
	// locally, so this is the only way the player gets hurt by it.
	h.own_weapon = hits::names_own_weapon(hit->weaponID, id);
	// R3 В2: the same for every explosion, whoever set it off: a barrel or mine that went off by itself or from an
	// anomaly (the initiator is not a creature), or one whose initiator is no longer online.
	h.explosion = hits::is_explosion(weapon_is_explosive(hit->weaponID, npc), who != nullptr, hit->whoID, hit->weaponID);
	if (!hits::to_owner(h, g_coop_friendly_fire != 0))
	{
		if (h.source == hits::Source::Player)
			++g_h.friendly_fire_dropped;
		return true; // friendly fire off; or anomalies, falls, physics: the player's own game computes them
	}
	Writer w(64);
	w.u16(id);
	write_hit(w, *hit, who ? who->ID() : kNoId, hit->weaponID); // no initiator: the owner keeps the damage (apply_to_actor)
	net::host_send(peer, Msg::PlayerHit, w);
	++g_h.player_hits_sent;
	if (h.source == hits::Source::Object || h.source == hits::Source::Missing)
		++g_h.explosion_hits_sent;
	return true; // the character itself is never damaged or killed by the host
}

bool host_actor_hit_dropped(const CActor* actor, const SHit* hit)
{
	if (!actor || !hit || !mgr().is_host())
		return false;
	const CGameObject* who = hit->who ? smart_cast<const CGameObject*>(hit->who) : nullptr;
	// Collision damage (Actor.cpp) names the contact object as both initiator and weapon.
	const bool collision = hit->whoID == hit->weaponID;
	if (!hits::host_actor_drops(hit_source(actor, who), collision, g_coop_friendly_fire != 0))
		return false;
	++g_h.friendly_fire_dropped;
	return true;
}

bool host_explode_as(u16 explosive, u16 initiator)
{
	if (!mgr().is_host() || !g_pGameLevel || !is_player(initiator) || !valid_go(initiator))
		return false;
	CGameObject* go = valid_go(explosive);
	CExplosive* ex = go ? go->cast_explosive() : nullptr;
	if (!ex || go->H_Parent())
		return false;
	// as CScriptGameObject::explode, with the player as the initiator instead of the charge itself
	Fvector normal;
	ex->FindNormal(normal);
	ex->SetInitiator(initiator);
	ex->GenExplodeEvent(go->Position(), normal);
	++g_h.explosions_as_player;
	return true;
}

int lua_explode_as(lua_State* L)
{
	u16 ids[2] = {kNoId, kNoId};
	for (int i = 0; i < 2; ++i)
	{
		if (lua_type(L, i + 1) != LUA_TNUMBER)
			break;
		const lua_Number v = lua_tonumber(L, i + 1);
		if (v >= 0 && v < lua_Number(kNoId) && v == lua_Number(u16(v))) // NaN fails the first test
			ids[i] = u16(v);
	}
	lua_pushboolean(L, ids[0] != kNoId && ids[1] != kNoId && host_explode_as(ids[0], ids[1]) ? 1 : 0);
	return 1;
}
} // namespace requests
} // namespace coop
