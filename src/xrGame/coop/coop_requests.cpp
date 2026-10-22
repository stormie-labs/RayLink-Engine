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
#include "xrCoop/coop_ledger.h"
#include "xrCoop/coop_spawn.h"
#include "xrCoop/coop_validate.h"
#include "xrCoop/coop_hits.h"

#include <cstring>

#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "Entity.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
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
};

struct Inflight
{
	Msg type;
	u16 item;
	u64 sent;
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
		// R3 В6: section + item state, the host builds the entity itself
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
		Writer w(a.bytes.size() + 16);
		w.u32(++g_seq);
		w.u16(to_host_id(a.item));
		w.u16(a.ge_type);
		w.u16(u16(a.bytes.size()));
		w.bytes(a.bytes.data(), a.bytes.size());
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

void client_flush()
{
	xr_vector<Action> out;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		out.swap(g_outbox);
		const u64 now = mgr().now_ms();
		for (auto it = g_child_spawns.begin(); it != g_child_spawns.end();)
			it = now - it->second > kChildSpawnHoldMs ? g_child_spawns.erase(it) : std::next(it);
	}
	for (const Action& a : out)
		send_action(a);
	const u64 now = mgr().now_ms();
	for (auto it = g_inflight.begin(); it != g_inflight.end();)
	{
		if (now - it->second.sent > kResultTimeoutMs)
		{
			++g_cl.timeouts;
			it = g_inflight.erase(it);
		}
		else
			++it;
	}
}

void client_on_result(PeerId, Reader& r)
{
	const u32 seq = r.u32();
	const bool accepted = r.boolean();
	const u8 reason = r.u8();
	const u16 subject = r.u16();
	if (!r.ok())
		return;
	Msg type = Msg::ReqResult;
	auto it = g_inflight.find(seq);
	if (it != g_inflight.end())
	{
		type = it->second.type;
		g_inflight.erase(it);
	}
	if (accepted)
	{
		++g_cl.accepted;
		return;
	}
	++g_cl.rejected;
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

CSE_Abstract* lua_forward(CSE_Abstract* e, const char* what)
{
	if (client_forward_spawn(e))
		++g_cl.lua_forwarded;
	else
	{
		++g_cl.lua_refused;
		::Msg("[coop] %s [%s] refused on a client: world spawns are the host's job", what, e->s_name.c_str());
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
bool host_begin(PeerId from, Reader& r, Msg type, Req& q, bool has_seq = true)
{
	if (!mgr().is_host())
		return false;
	// Precaching: the world was just loaded. The session pump runs before join::frame() notices the
	// new world and marks the peers as joining, so requests made in the old world would land here.
	if (sync::host_peer_joining(from) || Device.dwPrecacheFrame)
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
Reason host_spawn(CSE_Abstract* e, u16 parent, u16& created)
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
	e->ID = kNoId;
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
	CSE_Abstract* result = Level().Server->Process_spawn(packet, client);
	if (!result)
		return Reason::Engine;
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
	return rule && spawn::child_allowed(rule->game, holder->s_name.c_str(), child.game, child_section);
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

// ---------------------------------------------------------------- Ruling F2-21: requests waiting for the budget
// A request over its character's budget waits here (xrCoop/coop_spawn.h SpawnQueue) and is created and answered when
// the budget has refilled for it. Its player leaving, or the world replaced (SessionReset), drops it unanswered.
struct QueuedSpawn
{
	CSE_Abstract* entity = nullptr; // built from the host's section with the checked item state; the queue owns it
	Req q;
	u16 parent = kNoId;
	u32 cost = 0; // `cost` of the section; SpawnQueue charges charge_of(cost)
	u32 epoch = 0; // join::host_world_epoch() when it was asked
};
spawn::SpawnQueue<QueuedSpawn> g_spawn_queue;
u64 g_spawn_queued = 0, g_spawn_dequeued = 0, g_spawn_queue_dropped = 0;

void drop_queued(QueuedSpawn& s)
{
	F_entity_Destroy(s.entity);
	s.entity = nullptr;
	++g_spawn_queue_dropped;
}

// A waiting request the budget holds now: its holder is checked again (the player may have moved or dropped the
// parent meanwhile), then it is created and answered. False: refused, its charge goes back.
bool create_queued(QueuedSpawn& s)
{
	const Reason holder = host_spawn_holder(s.q, s.parent);
	if (holder != Reason::Ok)
	{
		F_entity_Destroy(s.entity);
		s.entity = nullptr;
		reply(s.q, holder, s.parent);
		return false;
	}
	u16 created = kNoId;
	const Reason why = host_spawn(s.entity, s.parent, created); // consumes the entity
	s.entity = nullptr;
	reply(s.q, why, why == Reason::Ok ? created : s.parent);
	return why == Reason::Ok;
}

// Host frame: drops what belongs to a player who left or to a replaced world, creates what the budgets hold now.
void host_pump_spawns()
{
	if (!g_spawn_queue.total() || !g_pGameLevel || !ai().get_alife() || !Level().Server || Device.dwPrecacheFrame)
		return;
	const u32 epoch = join::host_world_epoch();
	const u64 now = mgr().now_ms();
	const auto gone = [epoch](const QueuedSpawn& s)
	{ return s.epoch != epoch || join::proxy_of_peer(s.q.from) != s.q.proxy; };
	for (const std::string& key : g_spawn_queue.keys())
	{
		g_spawn_queue.drop_if(key, gone, drop_queued);
		g_spawn_dequeued += g_spawn_queue.pump(key, g_spawn_budgets.of(key), g_spawn_policy, now, create_queued);
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
		// Task 10b: the section's own installed_upgrades break its tree: init_install is FATAL on the new object.
		if (!items::section_defaults_ok(shared_str(section)))
		{
			if (note_bad(BadSection))
				::Msg("! [coop] ReqSpawn #%u from player #%u refused: [%s] has default upgrades its own upgrade tree "
				      "refuses (broken data)",
				    q.seq, from, section);
			return reply(q, Reason::BadSection, s.parent);
		}
		e = make_temp(section, Fvector().set(s.pos[0], s.pos[1], s.pos[2]), u32(-1), GameGraph::_GRAPH_ID(-1),
		    s.parent);
		if (!e)
			return reply(q, Reason::Engine, s.parent);
		if (s.state_size)
		{
			// The item state goes through the ItemState checks (WP9): counts, upgrades in an order the engine takes,
			// ammo types.
			const char* why = nullptr;
			if (!items::host_apply_spawn_state(e, s.state, s.state_size, why))
			{
				if (note_bad(BadSpawnState))
					::Msg("! [coop] ReqSpawn #%u of [%s] from player #%u refused: %s", q.seq, section, from,
					    why ? why : "item state");
				F_entity_Destroy(e);
				return reply(q, Reason::Malformed, s.parent);
			}
		}
	}
	// F2 T2: deny/allow lists, quest items, the character's cost budget; after the item state, so a malformed one
	// costs nothing. No rule or a missing line ends here too (judge steps 1-2, nothing was constructed).
	spawn::SectionFacts facts = spawn_facts(section);
	// Review round 2, Important 1: into one of his items only what that item takes.
	facts.parent_ok = s.parent == q.proxy || !rule || parent_takes(s.parent, *rule, section);
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
	// Ruling F2-21: over the budget it waits (`e` was built: OverBudget needs a rule and no missing line); answered
	// when it is created (host_pump_spawns). Refused only when his queue is full.
	if (verdict == spawn::SpawnVerdict::OverBudget && e &&
	    g_spawn_queue.push(key, QueuedSpawn{e, q, s.parent, facts.cost, join::host_world_epoch()}))
	{
		g_spawn_quest_gifts += gift ? 1 : 0;
		if (++g_spawn_queued <= kSpawnLogFirst)
			::Msg("[coop] ReqSpawn #%u of [%s] from player #%u waits for the cost budget of character %u (%u in his "
			      "queue): answered when created",
			    q.seq, section, from, q.proxy, u32(g_spawn_queue.size(key)));
		return;
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
	const Reason why = host_spawn(e, s.parent, created);
	if (why != Reason::Ok && !facts.allow_listed)
		spawn::refund(budget, facts.cost, g_spawn_policy); // Ruling F2-21 (3): the engine refused what was charged
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

void host_on_item_event(PeerId from, Reader& r)
{
	Req q;
	if (!host_begin(from, r, Msg::ReqItemEvent, q))
		return;
	const u16 item = r.u16();
	const u16 type = r.u16();
	const u16 size = r.u16();
	u8 data[512];
	if (!r.ok() || size > sizeof(data) || !r.bytes(data, size))
		return reply(q, Reason::Malformed, kNoId);
	if (!allow(from, CatMisc))
		return reply(q, Reason::RateLimited, item);
	if (type != GE_INSTALL_UPGRADE)
		return reply(q, Reason::NotAllowed, item);
	if (!events::find_entity(item))
		return reply(q, Reason::Missing, item);
	if (events::root_of(item) != q.proxy)
		return reply(q, Reason::NotOwner, item);
	{
		// R3 В6 (task 10b): the upgrade id ends inside the event bytes (r_stringZ reads up to a zero) and is a name.
		if (!size || !std::memchr(data, 0, size))
			return reply(q, Reason::Malformed, item);
		NET_Packet probe;
		probe.B.count = size;
		CopyMemory(probe.B.data, data, size);
		probe.r_pos = 0;
		shared_str upgrade;
		probe.r_stringZ(upgrade);
		if (!validate::resource_name_ok(upgrade.c_str(), spawn::kMaxSection))
			return reply(q, Reason::Malformed, item);
		// The server entity keeps it in the host save, and its next net_Spawn from scratch installs the section
		// defaults, its list, then it with loading = true: FATAL on a taken group or a missing parent group. A
		// repeat must not reach CSE_ALifeInventoryItem::add_upgrade either (FATAL on duplicates).
		CSE_Abstract* e = ai().alife().objects().object(item, true);
		const char* why = nullptr;
		switch (items::host_check_added_upgrade(e, upgrade, why))
		{
		case items::AddedUpgrade::AlreadyThere: return;
		case items::AddedUpgrade::Refused:
			if (note_bad(BadUpgrade))
				::Msg("! [coop] upgrade [%s] of item %u from player #%u refused: %s", upgrade.c_str(), item, from,
				    why ? why : "?");
			return reply(q, Reason::NotAllowed, item);
		case items::AddedUpgrade::Relay: break;
		}
	}
	NET_Packet P;
	CGameObject::u_EventGen(P, type, item);
	P.w(data, size);
	CGameObject::u_EventSend(P); // the server entity keeps the upgrade in the host save
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
xr_set<shared_str> g_lua_refused_once;

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
	g_limits.clear();
	g_spawn_budgets.clear(); // F2 T2: a session starts or ends (Ruling F2-11: kept across a peer leaving and rejoining)
	g_spawn_queue.clear(drop_queued); // Ruling F2-21: nothing of the old session is created
	g_last_launch.clear(); // W3a
	g_lua_refused_once.clear(); // W3a
	g_log_count = 0;
	// Lua may still hold these pointers; reset runs when a session ends, long after any script used them.
	for (CSE_Abstract* e : g_lua_temps)
		F_entity_Destroy(e);
	g_lua_temps.clear();
	g_lua_forwarded.clear();
}

void frame()
{
	if (mgr().is_host())
		host_pump_spawns(); // Ruling F2-21
	if (!mgr().is_client())
		return;
	client_flush_launches(); // W3a: before the outbox (a launch usually precedes a drop of the launcher)
	client_flush();
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
	CSE_Abstract* e = make_temp(section, pos, level_vertex, game_vertex, parent);
	if (!e)
	{
		::Msg("! [coop] alife():create: unknown section [%s]", section ? section : "");
		return nullptr;
	}
	if (ammo >= 0)
		if (CSE_ALifeItemAmmo* box = smart_cast<CSE_ALifeItemAmmo*>(e))
			box->a_elapsed = u16(_min(ammo, int(box->m_boxSize)));
	if (!register_now)
	{
		g_lua_temps.push_back(e); // the script edits it, then calls alife():register(se)
		return e;
	}
	lua_forward(e, "alife():create");
	F_entity_Destroy(e);
	return nullptr;
}

CSE_Abstract* lua_clone(CSE_Abstract* source, const char* section, const Fvector& pos, u32 level_vertex,
    u16 game_vertex, u16 parent, bool register_now)
{
	CSE_ALifeItemWeaponMagazined* from = smart_cast<CSE_ALifeItemWeaponMagazined*>(source);
	if (!from)
		return nullptr; // as the engine: only magazined weapons are cloned
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
	if (!register_now)
	{
		g_lua_temps.push_back(e);
		return e;
	}
	lua_forward(e, "alife():clone_weapon");
	F_entity_Destroy(e);
	return nullptr;
}

CSE_Abstract* lua_register_spawn(CSE_Abstract* object)
{
	const bool temp = std::find(g_lua_temps.begin(), g_lua_temps.end(), object) != g_lua_temps.end();
	if (!temp)
		return lua_refuse("alife():register of an object the host did not create");
	if (!g_lua_forwarded.insert(object).second)
		return lua_refuse("second alife():register of one object"); // would be a duplicate
	return lua_forward(object, "alife():register");
}

void lua_release(CSE_Abstract* object)
{
	if (!object || std::find(g_lua_temps.begin(), g_lua_temps.end(), object) != g_lua_temps.end())
		return; // never created: nothing to release
	const u16 id = object->ID;
	if (id != kNoId && id != kActor && events::root_of(id) == kActor)
	{
		client_destroy(id, false); // ours: the host releases it and tells everybody
		return;
	}
	++g_cl.lua_refused;
	::Msg("[coop] alife():release of [%s] %u refused on a client: the world is the host's", object->name_replace(), id);
}

CSE_Abstract* lua_refuse(const char* what)
{
	++g_cl.lua_refused;
	::Msg("[coop] %s refused on a client: the host creates world objects", what);
	return nullptr;
}

bool lua_client_refuse(const char* what)
{
	if (!lua_client())
		return false;
	++g_cl.lua_refused;
	if (g_lua_refused_once.insert(shared_str(what)).second)
		::Msg("[coop] %s refused on a client: the host's world is only mirrored here (logged once)", what);
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
