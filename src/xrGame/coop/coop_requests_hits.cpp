// Anomaly Coop - requests: hits. A client's hit on a host creature (ReqHit), the damage the decider passes on to a
// player's own actor (PlayerHit), and the hit hooks of the player characters (CAI_Stalker::Hit, CActor::Hit).
#include "stdafx.h"
#include "pch_script.h"
#include "coop_requests_impl.h"
#include "coop_events.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_entities.h"
#include "coop_debug.h"
#include "coop_players.h"
#include "xrCoop/coop_validate.h"

#include "Level.h"
#include "Actor.h"
#include "ActorCondition.h"
#include "GameObject.h"
#include "Entity.h"
#include "entity_alive.h"
#include "ai/stalker/ai_stalker.h"
#include "Explosive.h"
#include "../xrServerEntities/xrMessages.h"
#include "../Include/xrRender/Kinematics.h"
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
constexpr float kMeleeRange = 100.f; // strike/wound hits (knife, thrown physics)
constexpr float kExplosionRange = 150.f;
constexpr float kFireRange = 1500.f;
constexpr float kMaxHitPower = 100.f;
constexpr float kMaxArmorPiercing = 100.f; // stock ammo k_ap: 0.02 .. 5
constexpr float kMaxImpulse = 10000.f;

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

// Every number of a hit through coop::validate (the bone is checked against the target's skeleton).
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

// Who set off a hit on `victim` (a player's character or the host's actor), for the rules in coop_hits.h.
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

// Blast waves and fragments name the explosive as the weapon (CExplosive::Explode). Found while it is being
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

// The engine's before-hit Lua entry for the victim's class (as CAI_Stalker::Hit / CBaseMonster::Hit call it).
LPCSTR before_hit_entry(CGameObject* victim)
{
	if (victim->cast_stalker())
		return "CAI_Stalker__BeforeHitCallback";
	if (victim->cast_base_monster())
		return "CBaseMonster__BeforeHitCallback";
	return nullptr;
}

Reason host_hit(const Req& q, u16 target, SHit& hit)
{
	if (target == q.proxy)
		return Reason::NotAllowed; // a player hurts itself on its own machine
	if (!hit_sane(hit))
		return Reason::Malformed;
	CGameObject* victim = world::object_here(target);
	if (!victim)
		return world::server_entity(target) ? Reason::Offline : Reason::Missing;
	// the bone indexes the victim's skeleton unchecked (physics shell, hit scales, wounds: an AV here). A
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
	CGameObject* me = world::object_here(q.proxy);
	if (!me)
		return Reason::Offline;
	if (me->Position().distance_to(victim->Position()) > hit_range(hit.hit_type))
		return Reason::TooFar;
	CGameObject* weapon = nullptr;
	if (hit.weaponID != kNoId && events::root_of(hit.weaponID) == q.proxy)
		weapon = world::object_here(hit.weaponID);
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

// Every hit passed on to a player is journaled here (hits::journal_note: the first ones of 15 s, then the
// count and the sum): who, the type and power, and where his character stands here against where his machine says he is.
void journal_sent_hit(u16 id, PeerId peer, const SHit& hit, hits::Source source, const CGameObject* who, const Fvector& here)
{
	static xr_map<u16, hits::Journal> journals;
	const hits::JournalLine l = hits::journal_note(journals[id], mgr().now_ms(), hit.power);
	const char* role = mode().decides_world() ? "host" : "map server";
	if (l.summary)
		::Msg("[coop] hits -> player #%u (character %u): %u more hit(s) in the last 15 s, power %.3f in all (%s)", peer, id,
		    l.quiet_hits, l.quiet_power, role);
	if (!l.single)
		return;
	Fvector reported;
	u64 age = 0;
	const bool known = players::reported_position(id, reported, age);
	::Msg("[coop] hit -> player #%u (character %u): %s %u [%s], type %d, power %.3f; his character here (%.1f %.1f %.1f), his last report %s"
	      "(%.1f %.1f %.1f) %.1f m away, %u ms old (%s)",
	    peer, id, hits::source_name(source), hit.whoID, who ? who->cNameSect().c_str() : "-", int(hit.hit_type),
	    hit.power, here.x, here.y, here.z, known ? "" : "NONE ", known ? reported.x : 0.f, known ? reported.y : 0.f,
	    known ? reported.z : 0.f, known ? here.distance_to(reported) : -1.f, u32(_min<u64>(age, 0xffffffffull)), role);
}
} // namespace

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

// Main thread (client frame). Runs `_G.<entry>(victim, hit, bone)` like the engine does before it applies a hit:
// the Lua chain (npc_on_before_hit / monster_on_before_hit) may change power, impulse, direction and type, or
// return false to ignore the hit. Returns false if the hit must not be sent. A missing entry, a missing object or
// a Lua error leave the hit as it was (logged, never fatal).
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
		if (g_log.allow())
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
	CObject* who = world::object_here(join::to_local(hit.whoID));
	apply_to_actor(hit, who, world::object_here(join::to_local(hit.weaponID)));
	++g_cl.hits_in;
	// the damage the authority of this level (the host or a map server) passed on, in the client's own log.
	static hits::Journal journal;
	const hits::JournalLine l = hits::journal_note(journal, mgr().now_ms(), hit.power);
	if (l.summary)
		::Msg("[coop] hits <- the authority of my level: %u more hit(s) in the last 15 s, power %.3f in all", l.quiet_hits,
		    l.quiet_power);
	if (l.single)
	{
		const Fvector& me = Actor() ? Actor()->Position() : Fvector().set(0.f, 0.f, 0.f);
		::Msg("[coop] hit <- the authority of my level: from %u [%s] (local %u), type %d, power %.3f, health now %.3f, I stand at "
		      "(%.1f %.1f %.1f)",
		    hit.whoID, who ? who->cNameSect().c_str() : "not here", join::to_local(hit.whoID), int(hit.hit_type), hit.power,
		    Actor() ? Actor()->conditions().GetHealth() : -1.f, me.x, me.y, me.z);
	}
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

void local_actor_hit(const SHit& hit, CObject* who, CObject* weapon)
{
	if (mgr().active() && !mode().decides_world())
		apply_to_actor(hit, who, weapon);
}

bool stalker_hit(CAI_Stalker* npc, SHit* hit)
{
	if (!npc || !hit || !mgr().active())
		return false;
	if (is_mirror())
	{
		// host creatures and other players' avatars take damage on their owner's machine only
		if (entities::client_applying())
			return false;
		return sync::is_remote_driven(npc);
	}
	if (!mode().decides_here())
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
	// The player's own grenade/rocket explodes on the host with this character as initiator:
	// blast and fragments name the explosive as the weapon. Clients deal no explosion damage
	// locally, so this is the only way the player gets hurt by it.
	h.own_weapon = hits::names_own_weapon(hit->weaponID, id);
	// the same for every explosion, whoever set it off: a barrel or mine that went off by itself or from an
	// anomaly (the initiator is not a creature), or one whose initiator is no longer online.
	h.explosion = hits::is_explosion(weapon_is_explosive(hit->weaponID, npc), who != nullptr, hit->whoID, hit->weaponID);
	if (!hits::to_owner(h, g_coop_friendly_fire != 0))
	{
		if (h.source == hits::Source::Player)
			++g_h.friendly_fire_dropped;
		return true; // friendly fire off; or anomalies, falls, physics: the player's own game computes them
	}
	if (debug::host_drops_hit(peer))
		return true; // the server's debug god (docs/DEBUG_DESIGN.md); his own machine has AF_GODMODE for the rest
	Writer w(64);
	w.u16(id);
	write_hit(w, *hit, who ? who->ID() : kNoId, hit->weaponID); // no initiator: the owner keeps the damage (apply_to_actor)
	net::host_send(peer, Msg::PlayerHit, w);
	++g_h.player_hits_sent;
	journal_sent_hit(id, peer, *hit, h.source, who, npc->Position());
	if (h.source == hits::Source::Object || h.source == hits::Source::Missing)
		++g_h.explosion_hits_sent;
	return true; // the character itself is never damaged or killed by the host
}

bool host_actor_hit_dropped(const CActor* actor, const SHit* hit)
{
	if (!actor || !hit || !mode().decides_here())
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
	if (!mode().decides_here() || !g_pGameLevel || !is_player(initiator) || !valid_go(initiator))
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
