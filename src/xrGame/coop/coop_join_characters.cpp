#include "stdafx.h"
#include "pch_script.h"
#include "coop_join_impl.h"
#include "coop_death.h"
#include "coop_game_settings.h"
#include "coop_manager.h"
#include "coop_world.h"
#include "coop_alife.h"
#include "xrCoop/coop_death_rules.h"
#include "xrCoop/coop_ledger.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_nametag.h"

#include "ai_space.h"
#include "script_engine.h"
#include "xrCoop/coop_lua_fn.h"
#include "level_graph.h"
#include "Level.h"
#include "Actor.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "character_community.h"

namespace coop
{
namespace join
{
CharState g_char;

namespace
{
// ---------------------------------------------------------------- the choice window's script
// scripts\coop_menu_char.script: the host checks a choice and picks the start there, the client shows the offer and
// gives the choice back (the functions are listed in its header). Called on the main thread, from the session's
// handlers and join::frame, as coop_script.cpp calls coop_main.
constexpr const char* kCharScript = "coop_menu_char";

enum class LuaCall : u8
{
	Done,
	Missing, // the script or the function is not there
	Failed, // it raised an error (logged)
};

// Protected call of <module>.<fn>(what `push` pushes; it returns how many) with `nres` results, which
// `read(L, index of the first)` reads. Never throws, leaves the stack as it was; the module's lookup is protected too (the first
// one loads its script). A Lua error is logged.
template <typename Push, typename Read>
LuaCall call_lua(const char* module, const char* fn, Push push, int nres, Read read)
{
	std::string error;
	const luafn::Fail fail = luafn::call_named(ai().script_engine().lua(), module, fn, &luafn::traceback, push, nres,
	    [&](lua_State* L) { read(L, lua_gettop(L) - nres + 1); }, error);
	if (fail == luafn::Fail::None)
		return LuaCall::Done;
	if (fail != luafn::Fail::LuaError)
		return LuaCall::Missing;
	::Msg("! [coop] lua error in %s.%s: %s", module, fn, error.c_str());
	return LuaCall::Failed;
}

int push_nothing(lua_State*) { return 0; }

std::string lua_string_at(lua_State* L, int index)
{
	if (lua_type(L, index) != LUA_TSTRING)
		return std::string();
	size_t n = 0;
	const char* s = lua_tolstring(L, index, &n);
	return std::string(s, n);
}

std::string lua_string_field(lua_State* L, int table, const char* key)
{
	lua_getfield(L, table, key);
	std::string out = lua_string_at(L, -1);
	lua_pop(L, 1);
	return out;
}

bool lua_number_field(lua_State* L, int table, const char* key, lua_Number& out)
{
	lua_getfield(L, table, key);
	const bool ok = lua_type(L, -1) == LUA_TNUMBER;
	if (ok)
		out = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return ok;
}
} // namespace

// A player's character keeps in its custom data the SHA-256 of its player's key, never the key itself: the world save goes
// to every client, and a key is all it takes to play someone's character. Format, strict parse and migration of saves
// from before: coop::find_key_value / migrate_key_value (xrCoop/coop_session.h).
//
// The character of the player whose key has this digest, nullptr if he has none yet. On the way every character
// from an older save gets the digest of its key instead of the key: the world save made for the joining
// player right after carries nobody's key.
CSE_ALifeHumanStalker* find_proxy_by_key(const std::string& digest)
{
	CSE_ALifeHumanStalker* found = nullptr;
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!world::is_player_character(e) || !e->m_ini_string.size())
			continue;
		std::string ini = e->m_ini_string.c_str();
		if (migrate_key_value(ini))
		{
			e->m_ini_string = ini.c_str();
			::Msg("[coop] character %u: the player key in its custom data is replaced by its digest (older save)", e->ID);
		}
		size_t at = 0, len = 0;
		if (!found && find_key_value(ini, at, len) && ini.compare(at, len, digest) == 0)
			found = smart_cast<CSE_ALifeHumanStalker*>(e);
	}
	return found;
}

// A player character's name lives in m_character_name_str, the field saves and spawn packets carry: every STATE_Read (a
// save, the copy each game object spawns from, a mirrored EntitySpawn) rebuilds m_character_name from it, so a name put
// into m_character_name alone turns back into the stock name of the avatar's profile (sim_default_stalker_0,
// GENERATE_NAME, a random "first last" name). nametag::character_name keeps it readable by TranslateName, the same text
// coop_equip.cpp apply_name shows; nothing left of the nick keeps the name the character has.
void set_player_name(CSE_ALifeTraderAbstract& who, const std::string& nick)
{
	char name[nametag::kMaxCharacterName];
	if (!nametag::character_name(name, sizeof(name), nick.c_str()))
		return;
	who.m_character_name_str = name;
	who.m_character_name = TranslateName(who.m_character_name_str.c_str());
}

CSE_ALifeHumanStalker* create_proxy(const PeerInfo& p, const std::string& key_digest, const StartPoint& at)
{
	CSE_ALifeCreatureActor* actor = ai().alife().graph().actor();
	if (!actor)
		return nullptr;
	Fvector pos = actor->o_Position;
	u32 node = actor->m_tNodeID;
	GameGraph::_GRAPH_ID graph = actor->m_tGraphID;
	if (at.set)
	{
		pos = at.pos;
		node = at.lvid;
		graph = at.gvid;
	}
	else
		pos.x += 1.5f;
	CSE_Abstract* e = alife_rw().spawn_item(world::avatar_section().c_str(), pos, node, graph, kNoId);
	CSE_ALifeHumanStalker* proxy = smart_cast<CSE_ALifeHumanStalker*>(e);
	if (!proxy)
	{
		::Msg("! [coop] section [%s] is not a stalker: check gamedata (mod_system_coop.ltx)", world::avatar_section().c_str());
		if (e)
			alife_rw().release(e, true);
		return nullptr;
	}
	string512 ini;
	xr_sprintf(ini, "[coop]\nkey = %s\nnick = %s\n", key_digest.c_str(), p.nick.c_str()); // the digest
	proxy->m_ini_string = ini;
	set_player_name(*proxy, p.nick);
	::Msg("[coop] new character for '%s': proxy id %u at [%.1f %.1f %.1f] (%s)", p.nick.c_str(), proxy->ID, pos.x, pos.y,
	    pos.z, at.set ? at.where.c_str() : "next to the host");
	return proxy;
}

namespace
{
// The point the host's Lua picked for a new character, if it can be used. Until a player may start on his own level
// (newchar::kOwnLevels) it must be on the host's level; its level vertex can only be checked there (the level graph of
// another level is not loaded).
StartPoint host_place(bool have, const lua_Number (&xyz)[3], lua_Number lvid, lua_Number gvid, const std::string& where)
{
	StartPoint sp;
	if (!have)
		return sp;
	const CGameGraph& gg = ai().game_graph();
	const bool graph_ok = gvid >= 0 && gvid < lua_Number(0xFFFF) && gg.valid_vertex_id(u32(gvid));
	const bool here = graph_ok &&
	    gg.vertex(GameGraph::_GRAPH_ID(gvid))->level_id() == ai().alife().graph().level().level_id();
	bool valid = graph_ok && _valid(float(xyz[0])) && _valid(float(xyz[1])) && _valid(float(xyz[2])) && lvid >= 0 &&
	    lvid < lua_Number(u32(-1));
	if (valid && here)
		valid = ai().level_graph().valid_vertex_id(u32(lvid));
	// The newchar::point_usable rule (xrCoop/coop_newchar.h) requires a valid point in this world. If mode().leads_level()
	// is true, the point must be on the decider's level. The world server leads no player level, so a valid point on
	// any level is usable even though the server itself runs on the service level.
	const bool usable = valid && (here || !mode().leads_level());
	if (!usable)
	{
		::Msg("! [coop] new character: %s (game vertex %.0f, level vertex %.0f) %s, next to the host instead", where.c_str(),
		    gvid, lvid, valid ? "is on another level (every player is on the host's level)" : "is not a point of this world");
		return sp;
	}
	sp.set = true;
	sp.pos.set(float(xyz[0]), float(xyz[1]), float(xyz[2]));
	sp.lvid = u32(lvid);
	sp.gvid = GameGraph::_GRAPH_ID(gvid);
	sp.where = where;
	return sp;
}

// A fresh character carries its NPC profile's supplies (spawn_supplies): a player gets his faction's kit
// instead (coop_menu_char.host_make_character), as the in-world choice replaced the inventory.
u32 strip_supplies(CSE_ALifeHumanStalker& proxy)
{
	const xr_vector<ALife::_OBJECT_ID> kids = proxy.children;
	u32 n = 0;
	for (const ALife::_OBJECT_ID id : kids)
	{
		if (CSE_ALifeDynamicObject* item = ai().alife().objects().object(id, true))
		{
			alife_rw().release(item, true);
			++n;
		}
	}
	return n;
}

// The character's community is its faction's (the NPC community; the client's actor becomes actor_<faction>
// in the role swap, on_alife_objects_loaded).
bool set_faction(CSE_ALifeHumanStalker& proxy, const std::string& faction)
{
	const CHARACTER_COMMUNITY_INDEX index =
	    CHARACTER_COMMUNITY::IdToIndex(shared_str(faction.c_str()), NO_COMMUNITY_INDEX, true);
	if (index == NO_COMMUNITY_INDEX)
		return false;
	proxy.m_community_index = index;
	return true;
}
} // namespace

// A character the host puts on a level it has no
// ground of (the movers of a transition, a late one brought to the group) carries `placed = 1` in its [coop] block; the
// client puts its actor on the ground of the new level (on_alife_registered, xrCoop/coop_arrival.h). WorldLoaded clears it.
void mark_arrival(u16 proxy)
{
	CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(proxy, true) : nullptr;
	if (!e || !world::is_player_proxy(proxy))
		return;
	std::string text = e->m_ini_string.size() ? e->m_ini_string.c_str() : "";
	arrival::mark_placed(text);
	e->m_ini_string = text.c_str();
}

namespace
{
void unmark_arrival(u16 proxy)
{
	CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(proxy, true) : nullptr;
	if (!e || !e->m_ini_string.size())
		return;
	std::string text = e->m_ini_string.c_str();
	if (!arrival::placed_by_host(text))
		return;
	arrival::clear_placed(text);
	e->m_ini_string = text.c_str();
}

// The group lives on one level. A character left behind (its player was away when the host
// changed level) is moved next to the host: the client loads the level of its character.
void bring_proxy_to_level(CSE_ALifeHumanStalker& proxy, LPCSTR nick)
{
	CSE_ALifeCreatureActor* actor = ai().alife().graph().actor();
	if (!actor || proxy.ID_Parent != kNoId)
		return;
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(proxy.m_tGraphID) ||
	    gg.vertex(proxy.m_tGraphID)->level_id() == gg.vertex(actor->m_tGraphID)->level_id())
		return;
	Fvector pos = actor->o_Position;
	pos.x += 1.5f;
	alife_rw().teleport_object(proxy.ID, actor->m_tGraphID, actor->m_tNodeID, pos);
	mark_arrival(proxy.ID); // put there blind, his machine puts him on the ground of the new level
	::Msg("[coop] character of '%s' was on another level, moved to the group", nick);
}
} // namespace

// A player whose level cannot be his now (levels::host_player_loads: the group is one, or no process can lead his level) goes to the
// decider's level; a decider that leads no level (the world server) has none for him: he is told and stays out, his character keeps its
// place. False: he was kicked.
bool host_fall_back_to_level(PeerId from, CSE_ALifeHumanStalker& proxy, LPCSTR nick)
{
	if (!mode().leads_level())
	{
		::Msg("! [coop] no map server for the level of '%s' now", nick);
		mgr().session().kick(from, RejectReason::HostNotReady, "the server cannot run your level now, try again later");
		return false;
	}
	bring_proxy_to_level(proxy, nick);
	return true;
}

// A player who is not in the session has no character in the world. The proxy keeps its place, inventory
// and vitals in ALife (and in the save); coop::switch_vote keeps it offline until its player is back in the
// world. Taking it out now instead of waiting for the switch manager's next pass over it: the EntitySwitch the
// host sends makes it vanish on every client in the same moment.
void take_offline(u16 proxy)
{
	if (proxy == kNoId || !ai().get_alife())
		return;
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(proxy, true);
	if (!e || !e->m_bOnline || e->ID_Parent != kNoId)
		return;
	alife_rw().switch_offline(e);
	::Msg("[coop] character %u is out of the world until its player returns", proxy);
}

// The player has loaded the world: his character comes in now, through the switch manager's own checks
// (coop::switch_vote lets it, observer_distance keeps it online), instead of on its next pass, so it is there
// before the player's first request needs it. Until this moment it was offline, also in the save he loaded.
void bring_online(u16 proxy)
{
	if (proxy == kNoId || !ai().get_alife())
		return;
	unmark_arrival(proxy); // his machine placed him (or he is back where he stood); no later save moves him
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(proxy, true);
	if (!e || e->m_bOnline || e->ID_Parent != kNoId)
		return;
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(e->m_tGraphID) || gg.vertex(e->m_tGraphID)->level_id() != ai().alife().graph().level().level_id())
		return; // not on this level (cannot happen after bring_proxy_to_level): ALife keeps it where it is
	alife_rw().switch_object(e);
	if (e->m_bOnline)
		::Msg("[coop] character %u is in the world", proxy);
}

// --- host: JoinRequest ---
// The digest of a player's key, or false (logged, he is kicked: the host cannot tell his character).
bool host_key_digest(PeerId from, const PeerInfo& p, std::string& out)
{
	if (key_digest_hex(p.key, out))
		return true;
	::Msg("! [coop] cannot hash the player key of '%s' (BCrypt)", p.nick.c_str());
	mgr().session().kick(from, RejectReason::HostNotReady, "host could not check your player key");
	return false;
}

namespace
{
// The key's character retired in this world (a hardcore character): the host actor's custom data
// keeps it (death::note_retired_key, host_key_retired), also for a later session.
bool host_retired_in_world(const std::string& key_digest)
{
	const CSE_ALifeCreatureActor* a = ai().get_alife() ? ai().alife().graph().actor() : nullptr;
	return a && a->m_ini_string.size() && death::retired_key(a->m_ini_string.c_str(), key_digest);
}

// The key's character retired - in this session, or in this world (the newest kMaxRetiredKeys).
bool host_key_was_retired(const std::string& key_digest)
{
	return g_char.retired.count(key_digest) != 0 || host_retired_in_world(key_digest);
}

// The new character of this key starts with nothing (no kit, no faction money).
bool host_bare_character(const std::string& key_digest)
{
	return death::new_character_bare(host_key_was_retired(key_digest), death::host_new_kit());
}

// The key had a character - in this session (returned to one, got a new one, retired one) or one that
// retired in this world. Such a key is the same player starting again: coop_max_new_characters is not his.
bool host_key_had_character(const std::string& key_digest)
{
	return g_char.had_character.count(key_digest) != 0 || host_retired_in_world(key_digest);
}
} // namespace

// The host made a character for a key the world had none for: it spends the limit only if the key never had one.
void host_count_new_character(const std::string& key_digest)
{
	if (death::new_key_charge(host_key_had_character(key_digest), true).spends)
		g_char.new_characters.add(key_digest);
	g_char.had_character.insert(key_digest);
}

// The client picks its key, and every new one would get a character (starting kit, money) that stays in the save for
// good. A session makes coop_max_new_characters (a host setting, read at each join) for keys without a character; a
// returning player costs nothing, and neither does a key that had a character in this session or retired one in this
// world (a hardcore player starts again). False: he is kicked with the
// reason.
bool host_admits_new_key(PeerId from, const PeerInfo& p, const std::string& key_digest)
{
	const u32 max_new = u32(g_coop_max_new_characters);
	if (death::new_key_charge(host_key_had_character(key_digest), g_char.new_characters.admits(key_digest, max_new)).admitted)
		return true;
	if (++g_char.characters_refused <= kRefusalsLogged)
		::Msg("! [coop] '%s' has no character in this world, and this session has made %u new characters "
		      "(coop_max_new_characters %u): refused",
		    p.nick.c_str(), u32(g_char.new_characters.count()), max_new);
	mgr().session().kick(from, limits::kNoNewCharacterReason, limits::no_new_character_text(max_new));
	return false;
}

namespace
{
// The faction of the host's own character, as the stock new game wrote it into the community of the actor
// (actor_<faction>): the second source of a new character's faction when the world has none (an old save,
// newchar::character_faction). "" when there is none to read: no level, no actor yet (a CharOffer can come in the main
// menu or while the world is still loading) or a community that is not a faction.
// The level first, then Actor(): Actor() asserts on GameID(), which dereferences Level().game (a dead pointer without a
// level: access violation). A hostile client must not get another faction
// out of a failing read: character_faction ignores what the client sent and falls back to stalker.
std::string host_actor_faction()
{
	if (!g_pGameLevel || !Level().game || !g_actor)
		return std::string();
	const CActor* actor = Actor();
	if (!actor)
		return std::string();
	const shared_str id = actor->CharacterInfo().Community().id();
	if (!id.size())
		return std::string();
	static const char kActorPrefix[] = "actor_";
	const size_t prefix = sizeof(kActorPrefix) - 1;
	const std::string community = id.c_str();
	if (community.compare(0, prefix, kActorPrefix) == 0)
		return community.substr(prefix); // may be empty: "actor_" alone is no faction
	return community == "actor" ? std::string() : community; // the bare hero community names no faction
}
} // namespace

// Player `from` has no character in this world: he chooses one before the world (Msg::CharOffer; again
// after a refused choice, `refusal` is its reason code). False if the host's game has no choice window
// (coop_menu_char.script): his character is made next to the host as before.
bool host_offer_character(PeerId from, const PeerInfo& p, const char* refusal)
{
	lua_Number eco = 1, points = 0;
	bool story = false;
	const LuaCall call = call_lua(kCharScript, "join_terms", &push_nothing, 3, [&](lua_State* L, int at) {
		if (lua_type(L, at) == LUA_TNUMBER)
			eco = lua_tonumber(L, at);
		if (lua_type(L, at + 1) == LUA_TNUMBER)
			points = lua_tonumber(L, at + 1);
		story = lua_toboolean(L, at + 2) != 0;
	});
	if (call != LuaCall::Done)
	{
		::Msg("! [coop] %s.join_terms %s: '%s' gets a character next to the host, without a choice", kCharScript,
		    call == LuaCall::Missing ? "is missing" : "failed", p.nick.c_str());
		return false;
	}
	CharOfferMsg m;
	m.eco = u8(eco >= 3 ? 3 : (eco >= 2 ? 2 : 1));
	m.points = points > 0 ? u32(_min(points, lua_Number(1e9))) : 0u;
	// A retired key's new character starts with nothing. The offer says it with 0 points (a tier never
	// gives 0: coop_menu_char reads a join offer of 0 points as a bare one) - no protocol change; a flag of its own in
	// CharOfferMsg::Flags would be the clean way.
	std::string key_digest;
	const bool bare = key_digest_hex(p.key, key_digest) && host_bare_character(key_digest);
	if (bare)
		m.points = 0;
	m.flags = story ? CharOfferMsg::StoryMode : 0;
	const xr_string level = g_pGameLevel && mode().leads_level() ? Level().name().c_str() : ""; // the world server's service level is nobody's
	if (char_name_valid(level.c_str(), CharOfferMsg::kMaxLevel))
		m.level = level.c_str();
	if (refusal && *refusal)
		m.refusal = char_name_valid(refusal, CharOfferMsg::kMaxRefusal) ? refusal : "busy";
	// The faction is the world's; the player does not choose it. Never empty on the wire: the client refuses
	// an offer without a name.
	m.faction = newchar::character_faction(coop::game::world_faction(), host_actor_faction(), std::string());
	Writer w(96);
	m.write(w);
	mgr().session().send(from, Lane::Reliable, Msg::CharOffer, w);
	g_char.choosers.offer(from, mgr().now_ms());
	::Msg("[coop] '%s' has no character in this world: he chooses one before the world (economy %u, %u points, %s, "
	      "world faction %s)%s%s%s",
	    p.nick.c_str(), u32(m.eco), m.points, m.level.empty() ? "?" : m.level.c_str(), m.faction.c_str(),
	    bare ? ", hardcore: no kit, no money" : "", m.refusal.empty() ? "" : ", refused: ", m.refusal.c_str());
	return true;
}

namespace
{
void host_mark_creator_made(const char* nick);

// A refused choice: the offer again with the reason. If even that fails (the host's script broke meanwhile),
// the player cannot choose: he is dropped with the reason instead of waiting for the time limit.
void host_ask_again(PeerId from, const PeerInfo& p, const char* refusal)
{
	if (host_offer_character(from, p, refusal))
		return;
	g_char.choosers.forget(from);
	mgr().session().kick(from, RejectReason::HostNotReady, "the host's character window failed, see the host's log");
}

// What the host's Lua answered about a choice (coop_menu_char.host_plan_character).
struct CharPlan
{
	bool ok = false;
	std::string reason = "busy";
	bool have_point = false;
	lua_Number xyz[3] = {0, 0, 0};
	lua_Number lvid = -1, gvid = -1;
	std::string where;
	lua_Number money = 0;
};

// Player `from` chose. The host's Lua checks the choice and picks the start of the faction; the engine
// makes the character there (offline: its player is not in the world yet), strips the NPC supplies, sets the faction;
// the Lua gives the kit and the money; then the world save. A refused choice is offered again with the reason.
// `exact` is the creator's character (host_make_creator): the Lua gives exactly the record's loadout, money and start
// (host_plan_character / host_make_character with exact), and the record's creator is marked used after success. If it fails he gets the
// offer, as anybody: the window is then his first choice.
void host_make_new_character(PeerId from, const PeerInfo& p, const CharChoiceMsg& m, bool exact = false)
{
	if (!host_can_start())
	{
		g_char.choosers.forget(from);
		mgr().session().kick(from, RejectReason::HostNotReady, "host is not in a game right now, try again later");
		return;
	}
	std::string key_digest;
	if (!host_key_digest(from, p, key_digest))
	{
		g_char.choosers.forget(from);
		return;
	}
	if (CSE_ALifeHumanStalker* had = find_proxy_by_key(key_digest)) // cannot happen: one connection per key
	{
		g_char.choosers.forget(from);
		::Msg("[coop] '%s' has a character after all (proxy id %u): no new one", p.nick.c_str(), had->ID);
		if (!host_fall_back_to_level(from, *had, p.nick.c_str()) || !host_send_save(from, p, *had))
			g_join.gate.forget(from);
		return;
	}
	if (!host_admits_new_key(from, p, key_digest)) // the setting may have been lowered while he chose
	{
		g_char.choosers.forget(from);
		return;
	}

	// The faction is part of creating the world, a joining client does not choose it: the character gets the world's
	// faction. What the client sent in m.faction is only logged, never used - not for the plan,
	// not for set_faction, not for host_make_character.
	const std::string faction =
	    newchar::character_faction(coop::game::world_faction(), host_actor_faction(), m.faction);
	if (m.faction != faction)
		::Msg("! [coop] '%s' sent %s, the world's faction is %s: ignored", p.nick.c_str(), m.faction.c_str(),
		    faction.c_str());

	const std::string buy = newchar::buy_csv(m);
	auto push_choice = [&](lua_State* L) {
		lua_pushnumber(L, lua_Number(from));
		lua_pushlstring(L, faction.data(), faction.size());
		lua_pushlstring(L, buy.data(), buy.size());
		lua_pushboolean(L, exact ? 1 : 0);
		return 4;
	};
	CharPlan plan;
	const LuaCall planned = call_lua(kCharScript, "host_plan_character", push_choice, 1, [&](lua_State* L, int at) {
		if (!lua_istable(L, at))
			return;
		lua_getfield(L, at, "ok");
		plan.ok = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		const std::string reason = lua_string_field(L, at, "reason");
		if (!reason.empty())
			plan.reason = reason;
		lua_number_field(L, at, "money", plan.money);
		plan.where = lua_string_field(L, at, "where");
		plan.have_point = lua_number_field(L, at, "x", plan.xyz[0]) && lua_number_field(L, at, "y", plan.xyz[1]) &&
		    lua_number_field(L, at, "z", plan.xyz[2]) && lua_number_field(L, at, "lvid", plan.lvid) &&
		    lua_number_field(L, at, "gvid", plan.gvid);
	});
	if (planned != LuaCall::Done || !plan.ok)
	{
		++g_char.choices_refused;
		::Msg("[coop] '%s' chose %s: refused (%s), asked again%s", p.nick.c_str(), faction.c_str(),
		    planned == LuaCall::Done ? plan.reason.c_str() : "the host's script failed",
		    exact ? " (the creator's character from the staging file: the window instead)" : "");
		host_ask_again(from, p, plan.reason.c_str());
		return;
	}

	const StartPoint at = host_place(plan.have_point, plan.xyz, plan.lvid, plan.gvid, plan.where);
	if (!at.set && !mode().leads_level())
	{
		g_char.choosers.forget(from); // "next to the host" is the service level
		mgr().session().kick(from, RejectReason::HostNotReady, "the server has no place for a new character of this faction");
		return;
	}
	CSE_ALifeHumanStalker* proxy = create_proxy(p, key_digest, at);
	if (!proxy)
	{
		g_char.choosers.forget(from);
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not create a character for you");
		return;
	}
	const u32 stripped = strip_supplies(*proxy);
	if (!set_faction(*proxy, faction))
		::Msg("! [coop] community '%s' is not in game_relations.ltx: the character keeps its profile's", faction.c_str());
	// money::host_set (coop.money_set) finds a player's character by his peer: his entry knows it from here
	const bool had_entry = g_join.peers.find(from) != g_join.peers.end();
	HostPeer& hp = g_join.peers[from];
	hp.proxy = proxy->ID;
	hp.loading = true;
	bool made = false;
	std::string why = "busy";
	const u16 proxy_id = proxy->ID;
	const bool kit = !host_bare_character(key_digest); // a retired key's new character starts with nothing
	const LuaCall given = call_lua(kCharScript, "host_make_character",
	    [&](lua_State* L) {
		    lua_pushnumber(L, lua_Number(from));
		    lua_pushnumber(L, lua_Number(proxy_id));
		    lua_pushlstring(L, faction.data(), faction.size());
		    lua_pushlstring(L, buy.data(), buy.size());
		    lua_pushboolean(L, kit);
		    lua_pushboolean(L, exact ? 1 : 0);
		    return 6;
	    },
	    2, [&](lua_State* L, int at) {
		    made = lua_toboolean(L, at) != 0;
		    const std::string reason = lua_string_at(L, at + 1);
		    if (!made && !reason.empty())
			    why = reason;
	    });
	if (given != LuaCall::Done || !made)
	{
		::Msg("! [coop] new character of '%s' not made (%s): released, asked again%s", p.nick.c_str(), why.c_str(),
		    exact ? " (the creator's character from the staging file: the window instead)" : "");
		if (had_entry)
			hp.proxy = kNoId;
		else
			g_join.peers.erase(from);
		alife_rw().release(proxy, true);
		host_ask_again(from, p, why.c_str());
		return;
	}
	host_count_new_character(key_digest);
	g_char.choosers.forget(from);
	++g_char.characters_made;
	::Msg("[coop] '%s' chose %s: character %u made before the world (%u NPC item(s) stripped, %.0f RU), sending the "
	      "world%s",
	    p.nick.c_str(), faction.c_str(), u32(proxy_id), stripped, plan.money,
	    exact ? " (the creator's character from the staging file, no window)" : "");
	if (exact)
		host_mark_creator_made(p.nick.c_str());
	if (!host_send_save(from, p, *proxy))
		g_join.gate.forget(from);
}

// ---------------------------------------------------------------- the creator's character
// The record's creator (coop_game_settings, the record's own data, the same in both modes): the operator's character made
// from the staging file, no window.

// The record's creator is made: the next save keeps creator.used, a restart never makes him a second character from the file.
void host_mark_creator_made(const char* nick)
{
	bool marked = false;
	const LuaCall call = call_lua("coop_game_settings", "creator_made", &push_nothing, 1,
	    [&](lua_State* L, int at) { marked = lua_toboolean(L, at) != 0; });
	if (call != LuaCall::Done || !marked)
		::Msg("! [coop] the creator's character of '%s' is made, but the record's creator could not be marked used (%s): his key finds the "
		      "character from now on, the mark is only for a world without it",
		    nick, call == LuaCall::Done ? "no creator in the record" : "coop_game_settings.creator_made failed");
}
} // namespace

// The operator `from` (host_rights: the token of the session, loopback only) joins a world without a character for his key. If the world was
// staged from coop_new_world.ltx with a creator not made yet, and he chose a loadout (or to start bare) there, his character is made from it
// through host_make_new_character with `exact`: no window. True: done that way (the world or, on a failure, the offer went out); false: the
// caller offers the window as to anybody. Never for a peer without host_rights.
bool host_make_creator(PeerId from, const PeerInfo& p)
{
	newchar::CreatorFacts f;
	f.host_rights = p.host_rights;
	f.has_proxy = false; // host_send_world: no character for his key
	if (!f.host_rights)
		return false;
	std::string loadout;
	const LuaCall call = call_lua("coop_game_settings", "creator_facts", &push_nothing, 1, [&](lua_State* L, int at) {
		if (!lua_istable(L, at))
			return;
		lua_getfield(L, at, "creator");
		f.creator = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		lua_getfield(L, at, "used");
		f.used = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		lua_getfield(L, at, "bare");
		f.bare = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		lua_Number count = 0;
		if (lua_number_field(L, at, "count", count) && count > 0)
			f.loadout = size_t(_min(count, lua_Number(1e6)));
		loadout = lua_string_field(L, at, "loadout");
	});
	if (call != LuaCall::Done)
	{
		::Msg("! [coop] '%s' is the operator, but coop_game_settings.creator_facts %s: the character window", p.nick.c_str(),
		    call == LuaCall::Missing ? "is missing" : "failed");
		return false;
	}
	if (!newchar::make_creator(f))
	{
		if (f.creator)
			::Msg("[coop] '%s' is the operator; the world's creator %s: the character window", p.nick.c_str(),
			    f.used ? "was made already" : "chose no loadout");
		return false;
	}
	const std::string faction = newchar::character_faction(coop::game::world_faction(), host_actor_faction(), std::string());
	CharChoiceMsg m;
	if (!newchar::creator_choice(faction, loadout, m))
	{
		::Msg("! [coop] '%s' is the world's creator, but his loadout is no choice (%u entries, faction %s): the character window",
		    p.nick.c_str(), u32(f.loadout), faction.c_str());
		return false;
	}
	::Msg("[coop] '%s' is the operator and the world's creator: his character from the staging file (%u item(s)%s), no window",
	    p.nick.c_str(), u32(m.buy.size()), f.bare ? ", bare" : "");
	host_make_new_character(from, p, m, true);
	return true;
}

// A player's choice of his new character.
void host_on_char_choice(PeerId from, Reader& r)
{
	if (!mode().decides_world())
		return; // the session drops it on a client already (msg_sender)
	const PeerInfo* p = mgr().session().peer(from);
	if (!p)
		return;
	CharChoiceMsg m;
	const bool readable = m.read(r);
	if (!g_char.choosers.take(from))
	{
		if (++g_char.choices_ignored <= kRefusalsLogged)
			::Msg("! [coop] '%s' sends a character choice nobody asked for: ignored", p->nick.c_str());
		return;
	}
	if (!readable)
	{
		if (++g_char.choices_ignored <= kRefusalsLogged)
			::Msg("! [coop] '%s' sends an unreadable character choice: asked again", p->nick.c_str());
		host_ask_again(from, *p, "items");
		return;
	}
	host_make_new_character(from, *p, m);
}

// The host has no character for our key: we choose one before the world. The offer goes to the
// choice window from join::frame (client_char_frame); again after a refused choice, with the reason.
void client_on_char_offer(PeerId from, Reader& r)
{
	if (from != kHostPeer || mode().decides_world())
		return; // only the host sends it (the session kicks a client that does, msg_sender)
	CharOfferMsg m;
	if (!m.read(r))
	{
		::Msg("! [coop] malformed CharOffer from the host");
		mgr().stop("bad data from the host");
		return;
	}
	if (mgr().phase() != Phase::ReceivingWorld || g_join.load_issued)
	{
		::Msg("! [coop] an offer of a new character arrived while no world was awaited (%s): ignored",
		    phase_name(mgr().phase()));
		return;
	}
	g_char.char_offer = m;
	g_char.char_step = CharState::CharStep::Offered;
	g_char.char_next_try = 0;
	g_join.receive_started = mgr().now_ms(); // the time to choose is the host's (newchar::kChoiceTimeoutMs)
	::Msg("[coop] the host has no character for us: choose gear (economy %u, %u points, level %s, the world's faction "
	      "%s)%s%s",
	    u32(m.eco), m.points, m.level.empty() ? "?" : m.level.c_str(), m.faction.c_str(),
	    m.refusal.empty() ? "" : ", last choice refused: ", m.refusal.c_str());
}

// Names the branch client_char_frame took when it is not the one named last, so every exit is in
// the log once per change and not once per frame (the frames between two tries keep the branch of the last try). Night
// run 1: the offer sat for 15 min and nothing in the log told where it stood.
void char_note(const char* key, const char* text)
{
	if (g_char.char_note == key)
		return;
	g_char.char_note = key;
	::Msg("[coop] new character: %s", text);
}

// Client (join::frame): the offer to the window, the window's choice to the host.
void client_char_frame()
{
	PROF_EVENT("coop/coop_join/client_char_frame");
	if (g_char.char_step == CharState::CharStep::None)
	{
		char_note("idle", "no offer to answer (none came, or it was closed)"); // silent until a first offer
		return;
	}
	if (g_char.char_step == CharState::CharStep::Sent)
	{
		char_note("sent", "choice sent, waiting for the world"); // named by "character chosen" already
		return;
	}
	if (mgr().phase() != Phase::ReceivingWorld)
	{
		string256 key, text;
		xr_sprintf(key, "held %s", phase_name(mgr().phase()));
		xr_sprintf(text, "the offer waits: phase %s (it is answered in receiving-world only)", phase_name(mgr().phase()));
		char_note(key, text);
		return;
	}
	const u64 now = mgr().now_ms();
	if (now < g_char.char_next_try)
		return; // between two tries: the branch of the last try stands
	if (g_char.char_step == CharState::CharStep::Offered)
	{
		g_char.char_next_try = now + 500; // no dialog can be opened yet (precache, no menu): again soon
		const CharOfferMsg& o = g_char.char_offer;
		bool shown = false;
		const LuaCall call = call_lua(kCharScript, "join_offer",
		    [&o](lua_State* L) {
			    lua_pushnumber(L, lua_Number(o.eco));
			    lua_pushnumber(L, lua_Number(o.points));
			    lua_pushlstring(L, o.level.data(), o.level.size());
			    lua_pushlstring(L, o.refusal.data(), o.refusal.size());
			    lua_pushnumber(L, lua_Number(o.flags));
			    lua_pushlstring(L, o.faction.data(), o.faction.size()); // the world's faction
			    return 6;
		    },
		    1, [&shown](lua_State* L, int at) { shown = lua_toboolean(L, at) != 0; });
		if (call != LuaCall::Missing)
			g_char.char_window = true; // shown, filled, or broken half-way: join_closed closes whatever is there
		if (call != LuaCall::Done)
		{
			::Msg("! [coop] %s.join_offer %s: cannot choose a character", kCharScript,
			    call == LuaCall::Missing ? "is missing" : "failed");
			mgr().stop(call == LuaCall::Missing ? "this game has no co-op character window (coop_menu_char.script)"
			                                    : "the co-op character window failed, see the log");
			return;
		}
		if (shown)
		{
			char_note("taken", "coop_menu_char.join_offer took the offer (the window is open, or -coop_autochoose "
			                   "answered it)");
			g_char.char_step = CharState::CharStep::Shown;
			g_char.char_next_try = now;
		}
		else
			char_note("not shown", "coop_menu_char.join_offer returned false: no window yet, tried again every 500 ms "
			                       "(the [coop-char] lines say why)");
		return;
	}
	g_char.char_next_try = now + 100;
	std::string faction, buy;
	bool chose = false;
	const LuaCall call = call_lua(kCharScript, "join_choice", &push_nothing, 2, [&](lua_State* L, int at) {
		chose = lua_type(L, at) == LUA_TSTRING;
		faction = lua_string_at(L, at);
		buy = lua_string_at(L, at + 1);
	});
	if (call != LuaCall::Done)
	{
		if (call == LuaCall::Missing)
			::Msg("! [coop] %s.join_choice is missing: cannot choose a character", kCharScript);
		mgr().stop("the co-op character window failed, see the log");
		return;
	}
	if (!chose)
	{
		char_note("waiting", "waiting for the window's choice (coop_menu_char.join_choice gives none yet)");
		return;
	}
	CharChoiceMsg m;
	if (!newchar::make_choice(faction, buy, m))
	{
		::Msg("! [coop] the character window gave a choice the host cannot read ('%s'): not sent", faction.c_str());
		g_char.char_note = "unreadable"; // the next try of the offer is named again
		g_char.char_offer.refusal = "items";
		g_char.char_step = CharState::CharStep::Offered;
		return;
	}
	Writer w(64 + buy.size() + 2 * m.buy.size());
	m.write(w);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::CharChoice, w);
	g_char.char_step = CharState::CharStep::Sent;
	g_char.char_note = "sent";
	g_join.receive_started = now; // the join time limit starts again: the world follows the choice
	::Msg("[coop] character chosen: %s, %u item(s) bought; waiting for the world", m.faction.c_str(), u32(m.buy.size()));
}

// The world arrives or the session ends: the choice window closes. Every time, also after a
// SessionReset voided the offer (char_window): before, only the window's own FollowSession closed it then.
void client_char_closed(const char* why)
{
	const bool open = g_char.char_window || g_char.char_step != CharState::CharStep::None;
	g_char.char_step = CharState::CharStep::None;
	g_char.char_window = false;
	if (!open)
		return;
	call_lua(kCharScript, "join_closed",
	    [why](lua_State* L) {
		    lua_pushstring(L, why);
		    return 1;
	    },
	    0, [](lua_State*, int) {});
}

void host_key_retired(const std::string& key_digest)
{
	if (!mode().decides_world() || key_digest.empty())
		return;
	g_char.had_character.insert(key_digest);
	g_char.retired.insert(key_digest);
	CSE_ALifeCreatureActor* a = ai().get_alife() ? ai().alife().graph().actor() : nullptr;
	if (!a)
		return;
	std::string text = a->m_ini_string.size() ? a->m_ini_string.c_str() : "";
	const std::string before = text;
	death::note_retired_key(text, key_digest);
	if (text != before)
		a->m_ini_string = text.c_str();
}
} // namespace join
} // namespace coop
