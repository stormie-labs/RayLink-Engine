#include "stdafx.h"
#include "pch_script.h"
#include "coop_join.h"
#include "coop_manager.h"
#include "coop_sync.h"
#include "coop_world.h"
#include "coop_entities.h"
#include "coop_players.h"
#include "coop_objects.h"
#include "coop_alife.h"
#include "coop_net.h"
#include "coop_script.h"
#include "xrCoop/coop_shapes.h"
#include "xrCoop/coop_limits.h"
#include "xrCoop/coop_link_hold.h"
#include "xrCoop/coop_newchar.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "ai_space.h"
#include "script_engine.h"
#include "level_graph.h"
#include "Level.h"
#include "level_changer.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/xrMessages.h"
#include "game_graph.h"
#include "character_community.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

extern string_path g_last_saved_game;

int g_coop_max_new_characters = int(coop::limits::kNewCharactersDefault);

namespace coop
{
namespace diag
{
namespace
{
std::atomic<u32> g_destroyed_object_stacks{0}; // armed and not printed yet
} // namespace

void arm_destroyed_object_stacks()
{
	g_destroyed_object_stacks.store(kDestroyedObjectStacks, std::memory_order_relaxed);
	::Msg("[coop] diagnostics: the next %u accesses to a destroyed object print a C++ stack (a player left)",
	    kDestroyedObjectStacks);
}

bool take_destroyed_object_stack()
{
	u32 left = g_destroyed_object_stacks.load(std::memory_order_relaxed);
	while (left != 0 && !g_destroyed_object_stacks.compare_exchange_weak(left, left - 1, std::memory_order_relaxed))
	{
	}
	return left != 0;
}
} // namespace diag

namespace join
{
namespace
{
constexpr LPCSTR kProxySection = "coop_player_avatar";
constexpr LPCSTR kClientSaveName = "coop_session";
constexpr u16 kNoId = 0xffff;

struct HostPeer
{
	u16 proxy = kNoId;
	bool loading = false; // world sent, waiting for WorldLoaded
	u64 sent_at = 0;
};

struct State
{
	// host
	xr_map<PeerId, HostPeer> peers;
	// client
	u16 my_proxy = kNoId;
	u64 receive_started = 0;
	bool load_issued = false;
	bool swap_done = false;
	u64 last_progress_log = 0;
	// host: the world we serve (world_generation() when it was seen); a load or a level change
	// replaces it (epoch++ in on_world_created, the session's world epoch with it: R2 I1)
	u32 world_seen = 0;
	u32 epoch = 1;
	bool reset_pending = false;
	// host: R1 I4, R2 I4: one join in flight per player, one per 5 s
	JoinGate gate;
	// host: JoinRequests that came while the host's own level was still coming up (host_precaching), sent after it
	xr_vector<PeerId> precache_wait;
	// host: how long they wait for it (review of protocol 8: the precache of an unfocused window never ends)
	link_hold::PrecacheWait precache_clock;
	// host: Ruling F2-10 b: the keys this session made a new character for (coop_max_new_characters); a new session
	// starts over (on_host_started)
	limits::NewCharacters new_characters;
	u32 characters_refused = 0; // diagnostics: new keys refused over the limit (the first few logged)
	u32 joins_refused = 0; // diagnostics: refused JoinRequest / WorldLoaded / TransitionVote (the first few logged)
	u32 loaded_ignored = 0;
	u32 votes_refused = 0;
	u32 owners_bad = 0; // client: malformed AvatarList (R3 В3)
	// host: level exit asked by a client (R2 I3: the id of a level changer of the host's own level), waiting for
	// coop_accept_transition
	u16 pending_changer = kNoId;
	PeerId transition_from = kInvalidPeer;
	// host: when each player's vote last reached Lua and the chat (kVoteNoticeMs)
	xr_map<PeerId, u64> vote_noticed;
	// host, protocol 8: the players with a new key who are choosing their character before the world
	newchar::Choosers choosers;
	u32 choices_ignored = 0; // diagnostics: CharChoice nobody asked for, or unreadable
	u32 choices_refused = 0; // choices the host's Lua refused (the player was asked again)
	u32 characters_made = 0; // new characters made at their faction's start before the world
	// client, protocol 8: the host's offer of a new character, in the choice window until it gives a choice
	enum class CharStep : u8
	{
		None,
		Offered, // to be shown (coop_menu_char.join_offer)
		Shown, // the window is open: its choice is taken by join_choice
		Sent, // the CharChoice is on its way: the world, or the offer again with a refusal
	} char_step = CharStep::None;
	// client, protocol 8: join_offer ran (the window may be open) and join_closed has not since. Apart from char_step:
	// a SessionReset voids the offer (char_step None) and leaves the window open for the host's next offer, but the
	// world or the end of the session may come instead (review of protocol 8).
	bool char_window = false;
	CharOfferMsg char_offer;
	u64 char_next_try = 0;
	// client, protocol 8: the branch of client_char_frame the log named last (char_note): one line per change
	std::string char_note = "idle";
} g;

constexpr u32 kRefusalsLogged = 5;
// Host: what the session drops when a client sends too much of it (Session::limit_rate, per client): the table and
// why each value is in xrCoop/coop_limits.h (F2 T3); coop_status shows how many of each were dropped.

// T12 review (Ruling 18): a player's vote reaches the host's Lua and the chat at most once per 10 s, whatever exit he
// stands at: two exits side by side, or a modified client alternating them, do not flood the chat.
constexpr u64 kVoteNoticeMs = 10000;
// T12 review (Ruling 18): how near a player must be to a level exit to vote for it, metres from its nearest shape.
// Stock: an exit takes the actor when its shapes touch his bounding sphere (CCF_Shape::Contact, about 1 m around the
// middle of the body). The host sees the player's character where the player was 0.1-0.3 s ago (interpolation delay
// and one-way latency): 2-3 m back at a sprint. 5 m covers both.
constexpr float kVoteReachM = 5.f;

u32 g_world_generation = 0; // CALifeSimulator constructions in this process (outlives State)

// ---------------------------------------------------------------- protocol 8: the choice window's script
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

int char_traceback(lua_State* L)
{
	const char* msg = lua_tostring(L, 1);
	if (!msg)
		msg = "(error object is not a string)";
	lua_getglobal(L, "debug");
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_pushstring(L, msg);
		return 1;
	}
	lua_getfield(L, -1, "traceback");
	if (!lua_isfunction(L, -1))
	{
		lua_pop(L, 2);
		lua_pushstring(L, msg);
		return 1;
	}
	lua_pushstring(L, msg);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

// coop_menu_char.<name at 1>, or nil. Run in a protected call (call_char): the first lookup loads the script through
// _G.__index (gamedata/scripts/<name>.script), the function's lookup goes through the module's __index to _G, and
// anything on that way (a script that fails to load, an addon's strict _G metatable) may raise. Review of protocol 8:
// the lookup ran unprotected on the host for every new key.
int char_lookup(lua_State* L)
{
	const char* fn = lua_tostring(L, 1);
	lua_getglobal(L, kCharScript);
	if (!fn || !lua_istable(L, -1))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_getfield(L, -1, fn);
	return 1;
}

// Protected call of coop_menu_char.<fn>(what `push` pushes; it returns how many) with `nres` results, which
// `read(L, index of the first)` reads. Never throws, leaves the stack as it was; the module's lookup is protected too.
template <typename Push, typename Read>
LuaCall call_char(const char* fn, Push push, int nres, Read read)
{
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 24))
		return LuaCall::Missing;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &char_traceback);
	const int errfunc = lua_gettop(L);
	lua_pushcfunction(L, &char_lookup);
	lua_pushstring(L, fn);
	if (lua_pcall(L, 1, 1, errfunc) != 0)
	{
		const char* err = lua_tostring(L, -1);
		::Msg("! [coop] lua error looking up %s.%s: %s", kCharScript, fn, err ? err : "?");
		lua_settop(L, top);
		return LuaCall::Failed;
	}
	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, top);
		return LuaCall::Missing;
	}
	const int nargs = push(L);
	if (lua_pcall(L, nargs, nres, errfunc) != 0)
	{
		const char* err = lua_tostring(L, -1);
		::Msg("! [coop] lua error in %s.%s: %s", kCharScript, fn, err ? err : "?");
		lua_settop(L, top);
		return LuaCall::Failed;
	}
	read(L, errfunc + 1);
	lua_settop(L, top);
	return LuaCall::Done;
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

// R1 I1: a player's character keeps in its custom data the SHA-256 of its player's key, never the key itself: the
// world save goes to every client, and a key is all it takes to play someone's character. Format, strict parse
// and migration of saves from before: coop::find_key_value / migrate_key_value (xrCoop/coop_session.h).

bool read_file(LPCSTR path, std::vector<u8>& out)
{
	out.clear();
	FILE* f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size > 0)
	{
		out.resize(size_t(size));
		if (std::fread(out.data(), 1, out.size(), f) != out.size())
			out.clear();
	}
	std::fclose(f);
	return size >= 0;
}

bool write_file(LPCSTR path, const u8* data, size_t size)
{
	// FS writer creates missing folders (a fresh appdata has no savedgames yet).
	IWriter* w = FS.w_open(path);
	if (!w)
		return false;
	w->w(data, u32(size));
	FS.w_close(w);
	return FS.exist(path) != nullptr;
}

void save_path(string_path& out, LPCSTR name, LPCSTR ext)
{
	string_path file;
	xr_sprintf(file, "%s%s", name, ext);
	FS.update_path(out, "$game_saves$", file);
}

// The character of the player whose key has this digest, nullptr if he has none yet. On the way every character
// from an older save gets the digest of its key instead of the key (R1 I1): the world save made for the joining
// player right after carries nobody's key.
CSE_ALifeHumanStalker* find_proxy_by_key(const std::string& digest)
{
	CSE_ALifeHumanStalker* found = nullptr;
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!e || xr_strcmp(e->s_name, kProxySection) || !e->m_ini_string.size())
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

// Protocol 8: where a new character appears: the point the host's Lua picked (its faction's start or camp, checked by
// host_place), else next to the host.
struct StartPoint
{
	bool set = false;
	Fvector pos{};
	u32 lvid = u32(-1);
	GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
	std::string where; // "start:<location>" / "camp:<smart>" (the log)
};

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
	CSE_Abstract* e = alife_rw().spawn_item(kProxySection, pos, node, graph, kNoId);
	CSE_ALifeHumanStalker* proxy = smart_cast<CSE_ALifeHumanStalker*>(e);
	if (!proxy)
	{
		::Msg("! [coop] section [%s] is not a stalker — check gamedata (mod_system_coop.ltx)", kProxySection);
		if (e)
			alife_rw().release(e, true);
		return nullptr;
	}
	string512 ini;
	xr_sprintf(ini, "[coop]\nkey = %s\nnick = %s\n", key_digest.c_str(), p.nick.c_str()); // R1 I1: the digest
	proxy->m_ini_string = ini;
	proxy->m_character_name = p.nick.c_str();
	::Msg("[coop] new character for '%s': proxy id %u at [%.1f %.1f %.1f] (%s)", p.nick.c_str(), proxy->ID, pos.x, pos.y,
	    pos.z, at.set ? at.where.c_str() : "next to the host");
	return proxy;
}

// Protocol 8: the point the host's Lua picked for a new character, if it can be used. Until split levels (F3a,
// newchar::kOwnLevels) it must be on the host's level; its level vertex can only be checked there (the level graph of
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
	if (!newchar::point_usable(valid, here))
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

// Protocol 8: a fresh character carries its NPC profile's supplies (spawn_supplies): a player gets his faction's kit
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

// Protocol 8: the character's community is its faction's (the NPC community; the client's actor becomes actor_<faction>
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
	::Msg("[coop] character of '%s' was on another level, moved to the group", nick);
}

// W9: a player who is not in the session has no character in the world. The proxy keeps its place, inventory
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

// W9: the player has loaded the world: his character comes in now, through the switch manager's own checks
// (coop::switch_vote lets it, observer_distance keeps it online), instead of on its next pass, so it is there
// before the player's first request needs it. Until this moment it was offline, also in the save he loaded.
void bring_online(u16 proxy)
{
	if (proxy == kNoId || !ai().get_alife())
		return;
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

// R3 В3, host: every client learns who plays each character in the world (Msg::AvatarList): the host's own actor and
// the character of every player who has loaded the world. Sent whole whenever that changes (a player is in the
// world, a player left); a client that is still loading gets it queued (net::host_broadcast) or ignores it, and the
// table of its own WorldLoaded follows.
void host_send_owners()
{
	AvatarListMsg m;
	AvatarOwner host;
	host.avatar = 0;
	host.peer = kHostPeer;
	m.owners.push_back(host);
	const bool served = g.world_seen == g_world_generation; // else the proxies are ids of the old world
	for (const auto& it : g.peers)
	{
		const u16 proxy = it.second.proxy;
		if (!served || it.second.loading || proxy == kNoId || proxy == 0)
			continue;
		bool listed = false;
		for (const AvatarOwner& o : m.owners)
			listed = listed || o.avatar == proxy;
		if (listed || it.first == kHostPeer || it.first >= kMaxPlayers || m.owners.size() >= kMaxPlayers)
			continue; // cannot happen (one character per player key, slots 1..7): clients would refuse the whole table
		AvatarOwner o;
		o.avatar = proxy;
		o.peer = it.first;
		m.owners.push_back(o);
	}
	Writer w(1 + m.owners.size() * 6);
	m.write(w);
	net::host_broadcast(Msg::AvatarList, w);
}

// --- host: JoinRequest ---
// The digest of a player's key (R1 I1), or false (logged, he is kicked: the host cannot tell his character).
bool host_key_digest(PeerId from, const PeerInfo& p, std::string& out)
{
	if (key_digest_hex(p.key, out))
		return true;
	::Msg("! [coop] cannot hash the player key of '%s' (BCrypt)", p.nick.c_str());
	mgr().session().kick(from, RejectReason::HostNotReady, "host could not check your player key");
	return false;
}

// Ruling F2-10 b: the client picks its key; every new one used to get a character (starting kit, money) that stays in
// the save for good. A session makes coop_max_new_characters (F2 T3 fix round 1: a host setting, read at each join); a
// returning player costs nothing. False: he is kicked with the reason.
bool host_admits_new_key(PeerId from, const PeerInfo& p, const std::string& key_digest)
{
	const u32 max_new = u32(g_coop_max_new_characters);
	if (g.new_characters.admits(key_digest, max_new))
		return true;
	if (++g.characters_refused <= kRefusalsLogged)
		::Msg("! [coop] '%s' has no character in this world, and this session has made %u new characters "
		      "(coop_max_new_characters %u): refused",
		    p.nick.c_str(), u32(g.new_characters.count()), max_new);
	mgr().session().kick(from, limits::kNoNewCharacterReason, limits::no_new_character_text(max_new));
	return false;
}

bool host_send_save(PeerId from, const PeerInfo& p, CSE_ALifeHumanStalker& proxy_ref);

// Protocol 8: player `from` has no character in this world: he chooses one before the world (Msg::CharOffer; again
// after a refused choice, `refusal` is its reason code). False if the host's game has no choice window
// (coop_menu_char.script): his character is made next to the host as before.
bool host_offer_character(PeerId from, const PeerInfo& p, const char* refusal)
{
	lua_Number eco = 1, points = 0;
	bool story = false;
	const LuaCall call = call_char("join_terms", &push_nothing, 3, [&](lua_State* L, int at) {
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
	m.flags = story ? CharOfferMsg::StoryMode : 0;
	const xr_string level = g_pGameLevel ? Level().name().c_str() : "";
	if (char_name_valid(level.c_str(), CharOfferMsg::kMaxLevel))
		m.level = level.c_str();
	if (refusal && *refusal)
		m.refusal = char_name_valid(refusal, CharOfferMsg::kMaxRefusal) ? refusal : "busy";
	Writer w(96);
	m.write(w);
	mgr().session().send(from, Lane::Reliable, Msg::CharOffer, w);
	g.choosers.offer(from, mgr().now_ms());
	::Msg("[coop] '%s' has no character in this world: he chooses one before the world (economy %u, %u points, %s)%s%s",
	    p.nick.c_str(), u32(m.eco), m.points, m.level.empty() ? "?" : m.level.c_str(), m.refusal.empty() ? "" : ", refused: ",
	    m.refusal.c_str());
	return true;
}

// The world for a player whose join the gate admitted: his character (found by key) and a fresh save of the world, or
// (protocol 8) the offer of a new character, whose choice brings the world (host_on_char_choice). false if neither
// could be sent (the player is gone or was kicked with the reason).
bool host_send_world(PeerId from)
{
	const PeerInfo* p = mgr().session().peer(from);
	if (!p)
		return false;
	if (!host_can_start())
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "host is not in a game right now, try again later");
		return false;
	}

	std::string key_digest;
	if (!host_key_digest(from, *p, key_digest))
		return false;
	CSE_ALifeHumanStalker* proxy = find_proxy_by_key(key_digest);
	if (proxy)
	{
		::Msg("[coop] '%s' returns to character proxy id %u", p->nick.c_str(), proxy->ID);
		bring_proxy_to_level(*proxy, p->nick.c_str());
		return host_send_save(from, *p, *proxy);
	}
	if (!host_admits_new_key(from, *p, key_digest))
		return false;
	if (host_offer_character(from, *p, nullptr))
		return true; // the join stays in flight while he chooses
	proxy = create_proxy(*p, key_digest, StartPoint());
	if (!proxy)
	{
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not create a character for you");
		return false;
	}
	g.new_characters.add(key_digest);
	return host_send_save(from, *p, *proxy);
}

// Protocol 8: a refused choice: the offer again with the reason. If even that fails (the host's script broke meanwhile),
// the player cannot choose: he is dropped with the reason instead of waiting for the time limit.
void host_ask_again(PeerId from, const PeerInfo& p, const char* refusal)
{
	if (host_offer_character(from, p, refusal))
		return;
	g.choosers.forget(from);
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

// Protocol 8: player `from` chose. The host's Lua checks the choice and picks the start of the faction; the engine
// makes the character there (offline: its player is not in the world yet), strips the NPC supplies, sets the faction;
// the Lua gives the kit and the money; then the world save. A refused choice is offered again with the reason.
void host_make_new_character(PeerId from, const PeerInfo& p, const CharChoiceMsg& m)
{
	if (!host_can_start())
	{
		g.choosers.forget(from);
		mgr().session().kick(from, RejectReason::HostNotReady, "host is not in a game right now, try again later");
		return;
	}
	std::string key_digest;
	if (!host_key_digest(from, p, key_digest))
	{
		g.choosers.forget(from);
		return;
	}
	if (CSE_ALifeHumanStalker* had = find_proxy_by_key(key_digest)) // cannot happen: one connection per key
	{
		g.choosers.forget(from);
		::Msg("[coop] '%s' has a character after all (proxy id %u): no new one", p.nick.c_str(), had->ID);
		bring_proxy_to_level(*had, p.nick.c_str());
		if (!host_send_save(from, p, *had))
			g.gate.forget(from);
		return;
	}
	if (!host_admits_new_key(from, p, key_digest)) // the setting may have been lowered while he chose
	{
		g.choosers.forget(from);
		return;
	}

	const std::string buy = newchar::buy_csv(m);
	auto push_choice = [&](lua_State* L) {
		lua_pushnumber(L, lua_Number(from));
		lua_pushlstring(L, m.faction.data(), m.faction.size());
		lua_pushlstring(L, buy.data(), buy.size());
		return 3;
	};
	CharPlan plan;
	const LuaCall planned = call_char("host_plan_character", push_choice, 1, [&](lua_State* L, int at) {
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
		++g.choices_refused;
		::Msg("[coop] '%s' chose %s: refused (%s), asked again", p.nick.c_str(), m.faction.c_str(),
		    planned == LuaCall::Done ? plan.reason.c_str() : "the host's script failed");
		host_ask_again(from, p, plan.reason.c_str());
		return;
	}

	CSE_ALifeHumanStalker* proxy = create_proxy(p, key_digest, host_place(plan.have_point, plan.xyz, plan.lvid,
	                                                                           plan.gvid, plan.where));
	if (!proxy)
	{
		g.choosers.forget(from);
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not create a character for you");
		return;
	}
	const u32 stripped = strip_supplies(*proxy);
	if (!set_faction(*proxy, m.faction))
		::Msg("! [coop] community '%s' is not in game_relations.ltx: the character keeps its profile's", m.faction.c_str());
	// money::host_set (coop.money_set) finds a player's character by his peer: his entry knows it from here
	const bool had_entry = g.peers.find(from) != g.peers.end();
	HostPeer& hp = g.peers[from];
	hp.proxy = proxy->ID;
	hp.loading = true;
	bool made = false;
	std::string why = "busy";
	const u16 proxy_id = proxy->ID;
	const LuaCall given = call_char("host_make_character",
	    [&](lua_State* L) {
		    lua_pushnumber(L, lua_Number(from));
		    lua_pushnumber(L, lua_Number(proxy_id));
		    lua_pushlstring(L, m.faction.data(), m.faction.size());
		    lua_pushlstring(L, buy.data(), buy.size());
		    return 4;
	    },
	    2, [&](lua_State* L, int at) {
		    made = lua_toboolean(L, at) != 0;
		    const std::string reason = lua_string_at(L, at + 1);
		    if (!made && !reason.empty())
			    why = reason;
	    });
	if (given != LuaCall::Done || !made)
	{
		::Msg("! [coop] new character of '%s' not made (%s): released, asked again", p.nick.c_str(), why.c_str());
		if (had_entry)
			hp.proxy = kNoId;
		else
			g.peers.erase(from);
		alife_rw().release(proxy, true);
		host_ask_again(from, p, why.c_str());
		return;
	}
	g.new_characters.add(key_digest);
	g.choosers.forget(from);
	++g.characters_made;
	::Msg("[coop] '%s' chose %s: character %u made before the world (%u NPC item(s) stripped, %.0f RU), sending the "
	      "world",
	    p.nick.c_str(), m.faction.c_str(), u32(proxy_id), stripped, plan.money);
	if (!host_send_save(from, p, *proxy))
		g.gate.forget(from);
}

// Protocol 8, host: a player's choice of his new character.
void host_on_char_choice(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return; // the session drops it on a client already (msg_sender)
	const PeerInfo* p = mgr().session().peer(from);
	if (!p)
		return;
	CharChoiceMsg m;
	const bool readable = m.read(r);
	if (!g.choosers.take(from))
	{
		if (++g.choices_ignored <= kRefusalsLogged)
			::Msg("! [coop] '%s' sends a character choice nobody asked for: ignored", p->nick.c_str());
		return;
	}
	if (!readable)
	{
		if (++g.choices_ignored <= kRefusalsLogged)
			::Msg("! [coop] '%s' sends an unreadable character choice: asked again", p->nick.c_str());
		host_ask_again(from, *p, "items");
		return;
	}
	host_make_new_character(from, *p, m);
}

// The save of the world for player `from`, whose character is `proxy`, streamed to him (the join is in flight until
// his WorldLoaded). false if it could not be sent (he was kicked with the reason).
bool host_send_save(PeerId from, const PeerInfo& p_ref, CSE_ALifeHumanStalker& proxy_ref)
{
	const PeerInfo* p = &p_ref;
	CSE_ALifeHumanStalker* proxy = &proxy_ref;
	HostPeer& hp = g.peers[from];
	hp.proxy = proxy->ID;
	hp.loading = true; // W9: not in the world before WorldLoaded: the save he gets has his character offline
	world::host_mark_proxy(proxy->ID, from);

	// Fresh world save: engine part (.scop) and Anomaly's script part (.scoc).
	string64 name;
	xr_sprintf(name, "coop_join_%u", from);
	// W9: writing and reading back the save blocks this frame (seconds on a big world); the others must not time
	// us out meanwhile (the save hook arms it too; reading the files back is ours).
	on_blocking_work();
	{
		// save() records every name it writes as the last save; this file is deleted below, and
		// "load last save" must keep pointing at the host's real one.
		string_path last_save;
		xr_strcpy(last_save, g_last_saved_game);
		NET_Packet packet;
		packet.w_begin(M_SAVE_GAME);
		packet.w_stringZ(name);
		packet.w_u8(0); // keep the host's own current save name
		Level().Send(packet, net_flags(TRUE));
		xr_strcpy(g_last_saved_game, last_save);
	}
	string_path scop, scoc;
	save_path(scop, name, ".scop");
	save_path(scoc, name, ".scoc");
	std::vector<u8> engine_part, script_part;
	const bool saved = read_file(scop, engine_part) && !engine_part.empty();
	if (saved)
		read_file(scoc, script_part); // absent if Anomaly's script storage is disabled
	// W9: the hold is not let go here. Manager::frame decides on the next frame (link_hold::keep_held): a join that
	// comes while the host's own level is still coming up must not cut the hold of that load short.
	if (!saved)
	{
		::Msg("! [coop] world save failed (%s)", scop);
		mgr().session().kick(from, RejectReason::HostNotReady, "host could not save the world");
		return false;
	}

	xr_vector<u16> online;
	entities::host_collect_online(online);

	Writer w(engine_part.size() + script_part.size() + online.size() * 2 + 64);
	w.u16(proxy->ID);
	w.u32((u32)online.size());
	for (u16 id : online)
		w.u16(id);
	w.u32((u32)engine_part.size());
	w.bytes(engine_part.data(), engine_part.size());
	w.u32((u32)script_part.size());
	if (!script_part.empty())
		w.bytes(script_part.data(), script_part.size());
	std::vector<u8> blob(w.data().begin(), w.data().end());
	// R1 I4: a world save still queued or streaming to this player is replaced by this one (Session::send_blob)
	const u32 id = mgr().session().send_blob(from, BlobWorldSave, std::move(blob));
	hp.loading = true;
	hp.sent_at = mgr().now_ms();
	net::host_drop(from); // anything queued before this snapshot is already inside it
	sync::host_begin_join(from);
	::Msg("[coop] world for '%s': %u + %u bytes, blob %u", p->nick.c_str(), (u32)engine_part.size(),
	    (u32)script_part.size(), id);

	FS.file_delete(scop);
	FS.file_delete(scoc);
	return true;
}

// The host's own level is still coming up (precache frames, the first update of every script): a world save now
// would stall those frames further, and they are held by the link hold of that load (Manager::frame). For
// link_hold::PrecacheWait::kLimitMs at most: precache counts down only in the focused window and starts again on a
// device reset, and an unfocused host served no join (review of protocol 8: the client dropped after 5 min).
bool host_precaching()
{
	const bool precaching = g_pGameLevel && Device.dwPrecacheFrame != 0;
	const bool waits = g.precache_clock.waiting(precaching, mgr().now_ms());
	if (g.precache_clock.take_gave_up())
		::Msg("! [coop] the host's level is still precaching after %u s (window without focus?): joins go on",
		    u32(link_hold::PrecacheWait::kLimitMs / 1000));
	return waits;
}

// The gate for a JoinRequest of `from` that arrives now (or waited for the host's load, host_due_joins).
void host_join_request(PeerId from, const PeerInfo& peer)
{
	const PeerInfo* p = &peer;
	// Review of the live fixes: a join during the host's own load waits like a Later until the host plays normally.
	if (host_precaching())
	{
		if (std::find(g.precache_wait.begin(), g.precache_wait.end(), from) == g.precache_wait.end())
			g.precache_wait.push_back(from);
		::Msg("[coop] '%s' asks for the world while the host's level is loading: it is sent when that is over",
		    p->nick.c_str());
		return;
	}
	// R1 I4, R2 I4: every join costs a full world save on this thread. (A JoinRequest made in a world that is gone
	// never gets here: the session drops it by the world epoch, R2 I1.)
	switch (g.gate.request(from, mgr().now_ms()))
	{
	case JoinGate::Verdict::Admit:
		if (!host_send_world(from))
			g.gate.forget(from);
		return;
	case JoinGate::Verdict::Later:
		::Msg("[coop] '%s' asks for the world again within %u s of the last time: it is sent when that is over",
		    p->nick.c_str(), JoinGate::kIntervalMs / 1000);
		return;
	case JoinGate::Verdict::Refused:
		if (++g.joins_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' asks for the world while his join is under way: ignored", p->nick.c_str());
		return;
	}
}

void host_on_join_request(PeerId from, Reader&)
{
	if (!mgr().is_host())
		return; // F0 final review: the session drops it on a client already (msg_sender); handlers check it too
	if (const PeerInfo* p = mgr().session().peer(from))
		host_join_request(from, *p);
}

// Host: joins that waited for their turn (JoinGate) or for the host's own load, and whose time has come.
void host_due_joins()
{
	if (host_precaching())
		return; // the gate keeps its waiting ones until the host plays normally
	if (!g.precache_wait.empty())
	{
		const xr_vector<PeerId> waited = g.precache_wait;
		g.precache_wait.clear();
		for (PeerId id : waited)
			if (const PeerInfo* p = mgr().session().peer(id))
				host_join_request(id, *p);
	}
	for (PeerId id : g.gate.due(mgr().now_ms()))
		if (!host_send_world(id))
			g.gate.forget(id);
}

// --- host: WorldLoaded ---
void host_on_world_loaded(PeerId from, Reader&)
{
	if (!mgr().is_host())
		return; // F0 final review: as host_on_join_request
	auto it = g.peers.find(from);
	// R2 I4: only the end of a join in flight. A repeated, unasked-for or old world's WorldLoaded would run the
	// catch-up again (or bring an id of the old world online).
	// Review of protocol 8: whether a world was sent to him is checked before the gate, since gate.loaded ends his join
	// in flight. A forged WorldLoaded while he chooses his character (no world sent yet) ended it, and his JoinRequest
	// was admitted again, with a new CharOffer, every 5 s.
	const bool world_sent = it != g.peers.end() && it->second.loading && it->second.proxy != kNoId &&
	    !g.choosers.in_choice(from);
	if (!world_sent || !g.gate.loaded(from))
	{
		if (++g.loaded_ignored <= kRefusalsLogged)
			::Msg("! [coop] #%u says it loaded the world, but no world is being sent to it: ignored", from);
		return;
	}
	it->second.loading = false;
	const PeerInfo* p = mgr().session().peer(from);
	::Msg("[coop] '%s' is in the world (load took %.1f s)", p ? p->nick.c_str() : "?",
	    float(mgr().now_ms() - it->second.sent_at) / 1000.f);
	bring_online(it->second.proxy); // W9: his character was kept out of the world while he was away or loading
	sync::host_end_join(from);
	mgr().session().send(from, Lane::Reliable, Msg::JoinComplete, Writer());
	host_send_owners(); // R3 В3: everybody learns whose character this is, the newcomer who plays the others
	players::equip_resend(); // the newcomer needs everybody's look
	objects::host_on_peer_ready(from); // zone phases, door/physics poses: not all of it is in the save
}

// --- client ---
void client_on_join_complete(PeerId from, Reader&)
{
	if (from != kHostPeer || !mgr().is_client())
		return; // F0 final review: only the host sends it (the session kicks a client that does, msg_sender)
	::Msg("[coop] join complete, live sync on");
	if (Device.Paused()) // a pause from before the session; in a session nothing can pause (G18)
		Device.Pause(FALSE, TRUE, TRUE, "coop_live");
	players::equip_resend();
}

void client_on_session_reset(PeerId from, Reader& r)
{
	// F0 final review (Critical): from a client this wiped the host's sync state (avatars included), made the host a
	// client in ReceivingWorld with the client's epoch, and a 2-byte one stopped the session for everybody. The session
	// kicks a client that sends it (msg_sender); the handler checks it as well.
	if (from != kHostPeer || !mgr().is_client())
		return;
	const std::string why = r.str();
	const u32 epoch = r.u32();
	if (!r.ok())
	{
		::Msg("! [coop] malformed SessionReset from the host");
		mgr().stop("bad data from the host");
		return;
	}
	::Msg("[coop] host replaced the world (%s, epoch %u), rejoining", why.c_str(), epoch);
	g.my_proxy = kNoId;
	g.load_issued = false;
	g.swap_done = false;
	// protocol 8: an offer of the old world is void; the window stays open until the host offers again (join_offer)
	g.char_step = State::CharStep::None;
	sync::reset();
	g.receive_started = mgr().now_ms(); // the join time limit starts again
	mgr().set_phase(Phase::ReceivingWorld);
	// R2 I1: the session tells the host we are in the new world before our JoinRequest; a save of the old world
	// still on its way is dropped when it completes.
	mgr().session().set_world_epoch(epoch);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::JoinRequest, Writer());
}

// R3 В3, client: who plays each player character, from the host (host numbering). With it every machine knows the
// owner of every avatar, not only of the host's: coop.players() avatar_id, coop.peer_of_avatar, map markers, colours,
// nick tags, giving items.
void client_on_avatar_list(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_live_client())
		return; // not in the world yet: the table of our own join comes after WorldLoaded
	AvatarListMsg m;
	if (!m.read(r))
	{
		if (++g.owners_bad <= kRefusalsLogged)
			::Msg("! [coop] malformed AvatarList from the host: ignored");
		return;
	}
	const PeerId self = mgr().self();
	xr_vector<AvatarOwner> owners;
	for (const AvatarOwner& o : m.owners)
	{
		const u16 local = to_local(o.avatar);
		if (local == 0 || o.peer == self)
			continue; // our own character is the local actor
		AvatarOwner mine = o;
		mine.avatar = local;
		owners.push_back(mine);
	}
	world::client_set_owners(owners);
}

// Protocol 8, client: the host has no character for our key: we choose one before the world. The offer goes to the
// choice window from join::frame (client_char_frame); again after a refused choice, with the reason.
void client_on_char_offer(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_client())
		return; // only the host sends it (the session kicks a client that does, msg_sender)
	CharOfferMsg m;
	if (!m.read(r))
	{
		::Msg("! [coop] malformed CharOffer from the host");
		mgr().stop("bad data from the host");
		return;
	}
	if (mgr().phase() != Phase::ReceivingWorld || g.load_issued)
	{
		::Msg("! [coop] an offer of a new character arrived while no world was awaited (%s): ignored",
		    phase_name(mgr().phase()));
		return;
	}
	g.char_offer = m;
	g.char_step = State::CharStep::Offered;
	g.char_next_try = 0;
	g.receive_started = mgr().now_ms(); // the time to choose is the host's (newchar::kChoiceTimeoutMs)
	::Msg("[coop] the host has no character for us: choose faction and gear (economy %u, %u points, level %s)%s%s",
	    u32(m.eco), m.points, m.level.empty() ? "?" : m.level.c_str(), m.refusal.empty() ? "" : ", last choice refused: ",
	    m.refusal.c_str());
}

// Protocol 8, client: names the branch client_char_frame took when it is not the one named last, so every exit is in
// the log once per change and not once per frame (the frames between two tries keep the branch of the last try). Night
// run 1 (2026-10-23): the offer sat for 15 min and nothing in the log told where it stood.
void char_note(const char* key, const char* text)
{
	if (g.char_note == key)
		return;
	g.char_note = key;
	::Msg("[coop] new character: %s", text);
}

// Protocol 8, client (join::frame): the offer to the window, the window's choice to the host.
void client_char_frame()
{
	if (g.char_step == State::CharStep::None)
	{
		char_note("idle", "no offer to answer (none came, or it was closed)"); // silent until a first offer
		return;
	}
	if (g.char_step == State::CharStep::Sent)
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
	if (now < g.char_next_try)
		return; // between two tries: the branch of the last try stands
	if (g.char_step == State::CharStep::Offered)
	{
		g.char_next_try = now + 500; // no dialog can be opened yet (precache, no menu): again soon
		const CharOfferMsg& o = g.char_offer;
		bool shown = false;
		const LuaCall call = call_char("join_offer",
		    [&o](lua_State* L) {
			    lua_pushnumber(L, lua_Number(o.eco));
			    lua_pushnumber(L, lua_Number(o.points));
			    lua_pushlstring(L, o.level.data(), o.level.size());
			    lua_pushlstring(L, o.refusal.data(), o.refusal.size());
			    lua_pushnumber(L, lua_Number(o.flags));
			    return 5;
		    },
		    1, [&shown](lua_State* L, int at) { shown = lua_toboolean(L, at) != 0; });
		if (call != LuaCall::Missing)
			g.char_window = true; // shown, filled, or broken half-way: join_closed closes whatever is there
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
			g.char_step = State::CharStep::Shown;
			g.char_next_try = now;
		}
		else
			char_note("not shown", "coop_menu_char.join_offer returned false: no window yet, tried again every 500 ms "
			                       "(the [coop-char] lines say why)");
		return;
	}
	g.char_next_try = now + 100;
	std::string faction, buy;
	bool chose = false;
	const LuaCall call = call_char("join_choice", &push_nothing, 2, [&](lua_State* L, int at) {
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
		g.char_note = "unreadable"; // the next try of the offer is named again
		g.char_offer.refusal = "items";
		g.char_step = State::CharStep::Offered;
		return;
	}
	Writer w(64 + buy.size() + 2 * m.buy.size());
	m.write(w);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::CharChoice, w);
	g.char_step = State::CharStep::Sent;
	g.char_note = "sent";
	g.receive_started = now; // the join time limit starts again: the world follows the choice
	::Msg("[coop] character chosen: %s, %u item(s) bought; waiting for the world", m.faction.c_str(), u32(m.buy.size()));
}

// Protocol 8, client: the world arrives or the session ends: the choice window closes. Every time, also after a
// SessionReset voided the offer (char_window): before, only the window's own FollowSession closed it then.
void client_char_closed(const char* why)
{
	const bool open = g.char_window || g.char_step != State::CharStep::None;
	g.char_step = State::CharStep::None;
	g.char_window = false;
	if (!open)
		return;
	call_char("join_closed",
	    [why](lua_State* L) {
		    lua_pushstring(L, why);
		    return 1;
	    },
	    0, [](lua_State*, int) {});
}

void client_load_world(std::vector<u8>& data)
{
	Reader r(data.data(), data.size());
	const u16 proxy = r.u16();
	const u32 online_count = r.u32();
	xr_vector<u16> online;
	online.reserve(_min<u32>(online_count, 65536));
	for (u32 i = 0; i < online_count && r.ok(); ++i)
		online.push_back(r.u16());
	const u32 engine_size = r.u32();
	const u8* engine_part = r.cursor();
	r.skip(engine_size);
	const u32 script_size = r.u32();
	const u8* script_part = r.cursor();
	r.skip(script_size);
	if (!r.ok() || !engine_size)
	{
		::Msg("! [coop] world blob is malformed");
		mgr().stop("bad world data");
		return;
	}

	string_path scop, scoc;
	save_path(scop, kClientSaveName, ".scop");
	save_path(scoc, kClientSaveName, ".scoc");
	if (!write_file(scop, engine_part, engine_size))
	{
		::Msg("! [coop] cannot write %s", scop);
		mgr().stop("cannot write the world save");
		return;
	}
	if (script_size)
		write_file(scoc, script_part, script_size);
	else
		FS.file_delete(scoc);

	g.my_proxy = proxy;
	entities::client_set_initial_online(online); // needs my_proxy for the id mapping
	g.swap_done = false;
	g.load_issued = true;
	mgr().set_phase(Phase::LoadingWorld);
	::Msg("[coop] world received (%u + %u bytes), my character is proxy %u in host numbering, loading", engine_size,
	    script_size, proxy);

	client_char_closed("world"); // protocol 8: the choice window (main menu) closes before the load
	on_blocking_work(); // W9: the load below does not pump the network; the engine hooks re-arm it per stage
	if (g_pGameLevel)
		Console->Execute("disconnect");
	// As the stock new game / load do: a pause left by the menu would outlive the load ("Paused" after join)
	if (Device.Paused())
		Device.Pause(FALSE, TRUE, TRUE, "coop_join_load");
	string256 cmd;
	xr_sprintf(cmd, "start server(%s/single/alife/load) client(localhost)", kClientSaveName);
	Console->Execute(cmd);
	Console->Execute("main_menu off");
}

template <typename T>
void swap_values(T& a, T& b)
{
	T t = a;
	a = b;
	b = t;
}

// R2 I3, host: a level changer of the level the host is on, from its own ALife, nullptr if `id` is none (or one the
// host's scripts have switched off). The level change is built from its data, never from a client's bytes.
CSE_ALifeLevelChanger* level_changer_here(u16 id)
{
	if (id == kNoId || !g_pGameLevel || !ai().get_alife())
		return nullptr;
	CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(ai().alife().objects().object(id, true));
	if (!lc)
		return nullptr;
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(lc->m_tGraphID) || !gg.valid_vertex_id(lc->m_tNextGraphID) ||
	    gg.vertex(lc->m_tGraphID)->level_id() != ai().alife().graph().level().level_id())
		return nullptr;
	CObject* object = Level().Objects.net_Find(id);
	const CLevelChanger* changer = object ? object->cast_level_changer() : nullptr;
	if (changer && !changer->IsLevelChangerEnabled())
		return nullptr;
	return lc;
}

// R2 I3, client: the level changer of this level whose destination our engine wrote into a level change packet
// (CLevelChanger and its dialog write the changer's own m_tNext* values), kNoId if none (jump_to_level and the like).
u16 level_changer_of(NET_Packet& P)
{
	constexpr u32 kBody = sizeof(GameGraph::_GRAPH_ID) + sizeof(u32) + 2 * sizeof(Fvector);
	if (!ai().get_alife() || P.B.count < sizeof(u16) + kBody)
		return kNoId;
	const u32 read_at = P.r_pos;
	P.r_pos = sizeof(u16); // after the message type
	GameGraph::_GRAPH_ID graph;
	u32 node;
	Fvector position, angles;
	P.r(&graph, sizeof(graph));
	P.r(&node, sizeof(node));
	P.r_vec3(position);
	P.r_vec3(angles);
	P.r_pos = read_at;
	const CGameGraph& gg = ai().game_graph();
	const GameGraph::_LEVEL_ID here = ai().alife().graph().level().level_id();
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(it.second);
		if (lc && lc->m_tNextGraphID == graph && lc->m_dwNextNodeID == node && lc->m_tNextPosition.similar(position) &&
		    lc->m_tAngles.similar(angles) && gg.valid_vertex_id(lc->m_tGraphID) &&
		    gg.vertex(lc->m_tGraphID)->level_id() == here)
			return lc->ID;
	}
	return kNoId;
}

// T12 review (Ruling 18), host: metres from `p` to the nearest shape of a level exit (shapes::kFar if it has none).
// The exit's transform is built as CGameObject::net_Spawn builds its XFORM, each box as CCF_Shape::Contact does.
float distance_to_exit(const CSE_ALifeLevelChanger& lc, const Fvector& p)
{
	Fmatrix xf;
	xf.setXYZ(lc.o_Angle);
	xf.c.set(lc.o_Position);
	const float at[3] = {p.x, p.y, p.z};
	float best = shapes::kFar;
	for (const CShapeData::shape_def& s : lc.shapes)
	{
		float d = shapes::kFar;
		if (s.type == CShapeData::cfSphere)
		{
			Fvector center;
			xf.transform_tiny(center, s.data.sphere.P);
			const float c[3] = {center.x, center.y, center.z};
			d = shapes::to_sphere(at, c, s.data.sphere.R);
		}
		else if (s.type == CShapeData::cfBox)
		{
			Fmatrix box;
			box.mul_43(xf, s.data.box);
			const float i[3] = {box.i.x, box.i.y, box.i.z};
			const float j[3] = {box.j.x, box.j.y, box.j.z};
			const float k[3] = {box.k.x, box.k.y, box.k.z};
			const float c[3] = {box.c.x, box.c.y, box.c.z};
			d = shapes::to_box(at, i, j, k, c);
		}
		if (d < best)
			best = d;
	}
	return best;
}

} // namespace

void host_on_transition_vote(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return;
	const u16 changer = r.u16();
	if (!r.ok())
		return;
	const PeerInfo* p = mgr().session().peer(from);
	const char* nick = p ? p->nick.c_str() : "?";
	// R2 I3: the client names a level exit; the host goes only through a level changer of its own level.
	const CSE_ALifeLevelChanger* lc = level_changer_here(changer);
	if (!lc)
	{
		if (++g.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' asks to leave through object %u, which is no level exit here: ignored", nick, changer);
		return;
	}
	// T12 review (Ruling 18): only a player who is in the world votes, and only at that exit. A vote that passes
	// replaces the one before it, whoever cast that.
	const u16 proxy = proxy_of_peer(from);
	const bool in_world = !sync::host_peer_joining(from) && host_player_in_world(proxy);
	CObject* body = in_world ? Level().Objects.net_Find(proxy) : nullptr;
	if (!body)
	{
		if (++g.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' asks to leave through level exit %u but is not in the world: ignored", nick, changer);
		return;
	}
	const float away = distance_to_exit(*lc, body->Position());
	if (!(away <= kVoteReachM))
	{
		if (++g.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' asks to leave through level exit %u from %.1f m away (at most %.0f m): ignored", nick,
			    changer, away, kVoteReachM);
		return;
	}
	g.pending_changer = changer;
	g.transition_from = from;
	const u64 now = mgr().now_ms();
	const auto noticed = g.vote_noticed.find(from);
	if (noticed != g.vote_noticed.end() && now - noticed->second < kVoteNoticeMs)
		return; // Lua and the chat know already (this exit or the one next to it)
	g.vote_noticed[from] = now;
	::Msg("[coop] '%s' stands at a level exit (%u) and wants the group to move. Host: coop_accept_transition to go",
	      nick, changer);
	script::on_transition_vote(nick); // the host's Lua (clients get the chat line below)
	Writer w(128);
	string256 text;
	xr_sprintf(text, "%s wants to move the group to the next level", p ? p->nick.c_str() : "a player");
	w.str(text);
	net::host_broadcast(Msg::Chat, w);
}

void install(Session& s)
{
	for (const limits::RateRule& rule : limits::kClientRateRules)
		s.limit_rate(rule.type, rule.burst, rule.refill_ms); // R2 I4, T12 review, F2 T3 (host only, per client)
	s.on(Msg::TransitionVote, &host_on_transition_vote);
	s.on(Msg::JoinRequest, &host_on_join_request);
	s.on(Msg::CharChoice, &host_on_char_choice); // protocol 8
	s.on(Msg::CharOffer, &client_on_char_offer);
	s.on(Msg::WorldLoaded, &host_on_world_loaded);
	s.on(Msg::JoinComplete, &client_on_join_complete);
	s.on(Msg::SessionReset, &client_on_session_reset);
	s.on(Msg::AvatarList, &client_on_avatar_list); // R3 В3
}

bool host_can_start()
{
	if (!g_pGameLevel || !ai().get_alife() || !ai().alife().graph().actor())
	{
		::Msg("! [coop] start or load a game first, then coop_host");
		return false;
	}
	if (ai().alife().graph().actor()->get_health() <= 0.f)
	{
		::Msg("! [coop] the actor is dead");
		return false;
	}
	return true;
}

void on_host_started()
{
	g = State();
	g.world_seen = g_world_generation;
	mgr().session().set_world_epoch(g.epoch); // R2 I1: clients learn it from Welcome
	world::host_scan_proxies();
}

u32 host_world_epoch() { return g.epoch; }

void on_world_created()
{
	++g_world_generation;
	// R2 I1: the host's world epoch moves on right here, before the first frame of the new world pumps the session
	// (coop::on_frame handles messages before join::frame sees the new world): what clients made in the old world
	// is dropped from now on, and nothing of the new world goes to a client before its SessionReset.
	if (mgr().is_host())
	{
		++g.epoch;
		mgr().session().set_world_epoch(g.epoch);
	}
}

u32 world_generation() { return g_world_generation; }

bool accept_transition()
{
	if (!mgr().is_host() || g.pending_changer == kNoId || !g_pGameLevel)
	{
		::Msg("! [coop] no pending transition");
		return false;
	}
	const u16 id = g.pending_changer;
	g.pending_changer = kNoId;
	const CSE_ALifeLevelChanger* lc = level_changer_here(id);
	if (!lc)
	{
		::Msg("! [coop] the level exit %u asked for is not usable any more", id);
		return false;
	}
	// R2 I3: the level change the stock level changer sends, made from the host's own entity.
	NET_Packet P;
	P.w_begin(M_CHANGE_LEVEL);
	P.w(&lc->m_tNextGraphID, sizeof(lc->m_tNextGraphID));
	P.w(&lc->m_dwNextNodeID, sizeof(lc->m_dwNextNodeID));
	P.w_vec3(lc->m_tNextPosition);
	P.w_vec3(lc->m_tAngles);
	::Msg("[coop] moving the group to the next level (level exit %u)", id);
	Level().Send(P, net_flags(TRUE)); // the host's own level change; clients follow via SessionReset
	return true;
}

void client_request_level_change(NET_Packet& P)
{
	if (!mgr().is_live_client())
		return;
	// R2 I3: the host builds the level change itself; it only learns which of its level changers we stand in.
	const u16 changer = level_changer_of(P);
	if (changer == kNoId)
	{
		::Msg("! [coop] level exit: no level changer of this level leads there, the host is not asked");
		return;
	}
	Writer w(4);
	w.u16(to_host(changer));
	net::client_send(Msg::TransitionVote, w);
	::Msg("[coop] level exit %u: asked the host to move the group", changer);
}

void on_peer_joined(const PeerInfo&) {}

void on_peer_left(PeerId id)
{
	// T12 review (Ruling 18): before the lookup below. A player who got Later and left before his world was sent has
	// no entry in g.peers; his waiting request must not outlive him (the next player in his slot would be Refused).
	g.gate.forget(id);
	g.precache_wait.erase(std::remove(g.precache_wait.begin(), g.precache_wait.end(), id), g.precache_wait.end());
	g.choosers.forget(id); // protocol 8: he left while choosing his character
	g.vote_noticed.erase(id);
	if (g.transition_from == id)
	{
		g.pending_changer = kNoId; // nobody stands at that exit any more
		g.transition_from = kInvalidPeer;
	}
	auto it = g.peers.find(id);
	if (it == g.peers.end())
		return;
	diag::arm_destroyed_object_stacks(); // F0 final review (task 14): his character and its items leave the world now
	const u16 proxy = it->second.proxy;
	world::host_park_proxy(proxy);
	g.peers.erase(it);
	take_offline(proxy); // W9: gone from the session = gone from the world (stays in the save)
	host_send_owners(); // R3 В3: nobody plays that character now
}

bool host_player_in_world(u16 proxy_id)
{
	// A world loaded since the last frame is not served yet (host_watch_world): its ids are not the peers' proxies.
	if (proxy_id == kNoId || g.world_seen != g_world_generation)
		return false;
	for (const auto& it : g.peers)
		if (it.second.proxy == proxy_id)
			return !it.second.loading;
	return false;
}

u16 proxy_of_peer(PeerId id)
{
	auto it = g.peers.find(id);
	return it == g.peers.end() ? kNoId : it->second.proxy;
}

PeerId peer_of_proxy(u16 proxy_id)
{
	for (const auto& it : g.peers)
		if (it.second.proxy == proxy_id)
			return it.first;
	return kInvalidPeer;
}

bool is_proxy(u16 id)
{
	return world::is_player_proxy(id);
}

void on_welcome(const WelcomeMsg&)
{
	g.my_proxy = kNoId;
	g.load_issued = false;
	g.char_step = State::CharStep::None;
	g.receive_started = mgr().now_ms();
	mgr().set_phase(Phase::ReceivingWorld);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::JoinRequest, Writer());
}

u16 my_proxy() { return g.my_proxy; }

bool client_loading_world() { return mgr().phase() == Phase::LoadingWorld; }

void on_session_lost(const char*)
{
	// F0 final review (task 14): the players leave with the session (a client that was in the host's world leaves
	// it, the host takes the characters of its players out below). Called before the phase goes Off.
	if (mgr().is_live_client() || (mgr().is_host() && !g.peers.empty()))
		diag::arm_destroyed_object_stacks();
	client_char_closed("session"); // protocol 8: a choice window of this session closes with it (none: nothing)
	// W9, host: the session is over, nobody else is in the world. Clients take the avatars out through their own
	// switch vote (coop::switch_vote: no session = no player in the world).
	xr_vector<u16> proxies;
	for (const auto& it : g.peers)
		proxies.push_back(it.second.proxy);
	g = State();
	for (u16 proxy : proxies)
		take_offline(proxy);
}

void on_blob_progress(PeerId, u32, u32 kind, u64 done, u64 total)
{
	if (kind != BlobWorldSave)
		return;
	const u64 now = mgr().now_ms();
	if (now - g.last_progress_log < 500 && done != total)
		return;
	g.last_progress_log = now;
	::Msg("[coop] downloading world: %llu / %llu KB", done / 1024, total / 1024);
}

void on_blob_complete(PeerId from, u32, u32 kind, std::vector<u8>& data)
{
	if (kind != BlobWorldSave || from != kHostPeer || !mgr().is_client())
		return;
	// R2 I1: only the world we asked for and are waiting for (the session already dropped saves of another epoch).
	if (mgr().phase() != Phase::ReceivingWorld || g.load_issued)
	{
		::Msg("! [coop] a world save arrived while none was awaited (%s): ignored", phase_name(mgr().phase()));
		return;
	}
	client_load_world(data);
}

// Host: a load or a level change replaced the world under the session. Everybody rejoins
// from a fresh snapshot of the new world; requests made in the old one are dropped (epoch).
void host_watch_world()
{
	// A generation counter, not the simulator pointer: a same-level quick load deletes and
	// recreates the simulator in one call, usually at the same address.
	if (g.world_seen != g_world_generation)
	{
		g.world_seen = g_world_generation;
		objects::reset(); // per-world state of physics/zones belongs to the old world
		// The epoch moved on in on_world_created (R2 I1). Joins into the old world are void, everybody rejoins
		// after SessionReset; a level exit asked for in it means nothing here.
		g.gate.world_replaced();
		g.precache_wait.clear(); // requests of the old world: everybody rejoins after SessionReset
		g.choosers.world_replaced(); // protocol 8: a player choosing is asked again after his SessionReset
		g.pending_changer = kNoId;
		g.reset_pending = true;
		for (auto& it : g.peers)
		{
			it.second.loading = true;
			it.second.sent_at = 0; // review R2 C1: the join timer restarts at SessionReset, not at the first join
			it.second.proxy = kNoId; // an id of the old world: set again by their JoinRequest
			sync::host_begin_join(it.first); // queue/drop world traffic until they rejoin
		}
	}
	// The same "level is up" as for joins (host_precaching): an unfocused host sent no SessionReset either.
	if (!g.reset_pending || !g_pGameLevel || !g_actor || host_precaching() || !ai().get_alife())
		return;
	g.reset_pending = false;
	// F0 final review (task 12): a join admitted since the world changed (a player who connected meanwhile) is void
	// as well: he rejoins after this SessionReset, and a join left in flight would refuse his JoinRequest.
	g.gate.world_replaced();
	world::host_scan_proxies();
	Writer w(64);
	w.str("the host loaded a save or changed level");
	w.u32(g.epoch); // R2 I1: the client's session tells it back before its JoinRequest
	const u64 now = mgr().now_ms();
	for (const PeerInfo& p : mgr().session().peers())
	{
		mgr().session().send(p.id, Lane::Reliable, Msg::SessionReset, w);
		auto it = g.peers.find(p.id);
		if (it != g.peers.end())
			it->second.sent_at = now; // the rejoin into the new world is what the timeout measures
	}
	::Msg("[coop] world replaced (epoch %u): %u player(s) rejoin", g.epoch, (u32)mgr().session().peers().size());
}

constexpr u64 kJoinTimeoutMs = 5 * 60 * 1000; // R-2.3: a join is limited in time

// Host: a client that never finishes loading the world is dropped with a clear reason.
void host_watch_joins()
{
	const u64 now = mgr().now_ms();
	xr_vector<PeerId> stuck;
	for (const auto& it : g.peers)
	{
		if (!it.second.loading || g.choosers.in_choice(it.first))
			continue; // protocol 8: a player choosing his character (after a world change) has the limit below
		if (net::host_queue_overflow(it.first) || (it.second.sent_at && now - it.second.sent_at > kJoinTimeoutMs))
			stuck.push_back(it.first);
	}
	for (PeerId id : stuck)
	{
		::Msg("! [coop] #%u did not finish loading the world in time, dropping it", id);
		mgr().session().kick(id, RejectReason::Timeout, "loading the host's world took too long, try to join again");
	}
	// Protocol 8: a player who keeps the choice window open holds a slot and a join in flight.
	for (PeerId id : g.choosers.expired(now))
	{
		::Msg("! [coop] #%u did not choose a character in %u min, dropping it", id, u32(newchar::kChoiceTimeoutMs / 60000));
		g.choosers.forget(id);
		mgr().session().kick(id, RejectReason::Timeout, "you did not choose a character in time, join again");
	}
}

void frame()
{
	if (mgr().is_host())
	{
		host_watch_world();
		host_watch_joins();
		host_due_joins();
		return;
	}
	// Client: downloading or loading the world must end in time (R-2.3). Protocol 8: choosing a character has the
	// host's limit (it drops us first; a minute more here for a host that is gone silent).
	const bool choosing = g.char_step == State::CharStep::Offered || g.char_step == State::CharStep::Shown;
	const u64 limit = choosing ? newchar::kChoiceTimeoutMs + 60000 : kJoinTimeoutMs;
	if ((mgr().phase() == Phase::ReceivingWorld || mgr().phase() == Phase::LoadingWorld) && g.receive_started &&
	    mgr().now_ms() - g.receive_started > limit)
	{
		::Msg("! [coop] joining the host's world took too long");
		mgr().stop("joining the host's world took too long");
		return;
	}
	client_char_frame(); // protocol 8: the offer of a new character and the choice
	if (!mgr().active())
		return; // the window is gone (stopped above)
	if (mgr().phase() != Phase::LoadingWorld || !g.swap_done)
		return;
	// Level is up and the local actor exists: tell the host.
	if (!g_pGameLevel || !Level().game || !g_actor || Device.dwPrecacheFrame)
		return;
	mgr().set_phase(Phase::Live);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::WorldLoaded, Writer());
	sync::on_client_live();
}

void print_status()
{
	if (mgr().is_host())
	{
		::Msg("[coop]   world epoch %u; refused: %u join(s), %u WorldLoaded, %u level exit(s); %u join(s) wait for the "
		      "host's load",
		    g.epoch, g.joins_refused, g.loaded_ignored, g.votes_refused, u32(g.precache_wait.size()));
		// T12 review (Ruling 18): the log names a dropped type once per client; the count is here every time
		for (const limits::RateRule& rule : limits::kClientRateRules)
			::Msg("[coop]   rate limit %s: %llu dropped (more than %u in a row, then one per %u ms)", msg_name(rule.type),
			    mgr().session().stats(u16(rule.type)).rate_dropped, rule.burst, rule.refill_ms);
		::Msg("[coop]   new characters this session: %u of %u (coop_max_new_characters), %u new key(s) refused",
		    u32(g.new_characters.count()), u32(g_coop_max_new_characters), g.characters_refused);
		::Msg("[coop]   new characters before the world (protocol 8): %u made, %u choosing now, %u choice(s) refused, "
		      "%u ignored",
		    g.characters_made, u32(g.choosers.count()), g.choices_refused, g.choices_ignored);
		if (g.pending_changer != kNoId)
			::Msg("[coop]   level exit %u asked for by #%u, waiting for coop_accept_transition", g.pending_changer,
			    g.transition_from);
		for (const auto& it : g.peers)
			::Msg("[coop]   peer #%u proxy %u%s%s", it.first, it.second.proxy, it.second.loading ? " (loading)" : "",
			    g.gate.waiting(it.first) ? " (join waits for its turn)" : "");
	}
	else if (mgr().is_client())
	{
		::Msg("[coop]   my proxy (host numbering) %u, swap %s", g.my_proxy, g.swap_done ? "done" : "pending");
		if (g.char_step != State::CharStep::None)
			::Msg("[coop]   new character: %s", g.char_step == State::CharStep::Sent ? "chosen, waiting for the world"
			                                                                       : "choosing (window)");
	}
}

void on_alife_objects_loaded(CALifeObjectRegistry& objects)
{
	if (!client_loading_world() || g.my_proxy == kNoId)
		return;

	CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(objects.object(0, true));
	CSE_ALifeHumanStalker* proxy = smart_cast<CSE_ALifeHumanStalker*>(objects.object(g.my_proxy, true));
	if (!actor || !proxy)
	{
		::Msg("! [coop] role swap impossible: actor %p, proxy %u %p", actor, g.my_proxy, proxy);
		mgr().stop("the world from the host has no character for us");
		return;
	}

	// Where we stand and what we look like. CActor::net_Spawn takes its view (r_model_yaw, camera)
	// from o_torso, not from o_Angle.
	swap_values(actor->o_Position, proxy->o_Position);
	swap_values(actor->o_Angle, proxy->o_Angle);
	swap_values(actor->o_model, proxy->o_model);
	swap_values(actor->o_torso, proxy->o_torso);
	swap_values(actor->m_tNodeID, proxy->m_tNodeID);
	swap_values(actor->m_tGraphID, proxy->m_tGraphID);
	{
		const float h = actor->get_health();
		actor->set_health(proxy->get_health());
		proxy->set_health(h);
	}
	// The host may sit in a vehicle; our character does not (net_Spawn would seat us in it).
	actor->m_holderID = u16(-1);

	// Who we are: money, community, rank, reputation, name.
	swap_values(actor->m_dwMoney, proxy->m_dwMoney);
	swap_values(actor->m_community_index, proxy->m_community_index);
	swap_values(actor->CSE_ALifeTraderAbstract::m_rank, proxy->CSE_ALifeTraderAbstract::m_rank);
	swap_values(actor->m_reputation, proxy->m_reputation);
	swap_values(actor->m_character_name, proxy->m_character_name);
	// The proxy is an NPC of community X; a player character uses actor_X (game_relations.ltx has
	// both, relations and scripts tell them apart).
	if (actor->m_community_index != NO_COMMUNITY_INDEX)
	{
		const shared_str npc = CHARACTER_COMMUNITY::IndexToId(actor->m_community_index, shared_str(), true);
		if (npc.size() && strncmp(npc.c_str(), "actor", 5))
		{
			string128 player;
			xr_sprintf(player, "actor_%s", npc.c_str());
			const CHARACTER_COMMUNITY_INDEX index =
			    CHARACTER_COMMUNITY::IdToIndex(shared_str(player), NO_COMMUNITY_INDEX, true);
			if (index != NO_COMMUNITY_INDEX)
				actor->m_community_index = index;
		}
	}

	// What we carry: items keep their ids, only parents swap. Children lists are rebuilt from
	// ID_Parent when ALife registers the loaded objects right after this hook.
	u32 to_proxy = 0, to_actor = 0;
	for (auto& it : objects.objects())
	{
		CSE_ALifeDynamicObject* e = it.second;
		if (!e)
			continue;
		if (e->ID_Parent == 0)
		{
			e->ID_Parent = g.my_proxy;
			++to_proxy;
		}
		else if (e->ID_Parent == g.my_proxy)
		{
			e->ID_Parent = 0;
			++to_actor;
		}
	}
	actor->children.clear();
	proxy->children.clear();

	g.swap_done = true;
	::Msg("[coop] role swap done: actor <-> proxy %u (%u items to me, %u to the host avatar)", g.my_proxy, to_actor,
	    to_proxy);
	world::client_mark_host_avatar(g.my_proxy);
}
} // namespace join
} // namespace coop
