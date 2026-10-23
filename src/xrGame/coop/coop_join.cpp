#include "stdafx.h"
#include "pch_script.h"
#include "coop_join.h"
#include "coop_death.h" // Ruling F3-10: host_new_kit
#include "coop_game_settings.h" // protocol 9: game::world_faction
#include "coop_manager.h"
#include "coop_sync.h"
#include "coop_world.h"
#include "coop_entities.h"
#include "coop_players.h"
#include "coop_objects.h"
#include "coop_alife.h"
#include "coop_net.h"
#include "coop_script.h"
#include "coop_survival.h" // transition wait: who is alive
#include "coop_sync_api.h" // F2 T14
#include "xrCoop/coop_shapes.h"
#include "xrCoop/coop_death_rules.h" // Ruling F3-9: retired keys, new_key_charge
#include "xrCoop/coop_limits.h"
#include "xrCoop/coop_link_hold.h"
#include "xrCoop/coop_newchar.h"
#include "xrCoop/coop_transition.h" // protocol 10: the waiting point of a level change

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/CameraBase.h" // transition: the view of a player put back at the edge
#include "ai_space.h"
#include "script_engine.h"
#include "level_graph.h"
#include "Level.h"
#include "Actor.h" // protocol 9: host_actor_faction
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
extern xr_vector<CLevelChanger*> g_lchangers; // level_changer.cpp: the exits of the level that are online

int g_coop_max_new_characters = int(coop::limits::kNewCharactersDefault);
int g_coop_diag_stacks = 0;

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
	if (!g_coop_diag_stacks)
		return; // a stack walk stalls this thread for minutes on its first use (console coop_diag_stacks 1)
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
	// starts over (on_host_started). Ruling F3-9: only keys that had no character spend it (death::new_key_charge).
	limits::NewCharacters new_characters;
	// host, Ruling F3-9: the keys that had a character in this session (a returning player, a new character made, a
	// retired one); with the retired keys of the world (host_retired_in_world) they neither check nor spend the limit
	std::set<std::string> had_character;
	// host, Ruling F3-10: the keys whose character retired in this session (with [coop_retired] of the world: a bare new
	// character unless coop_hardcore_new_kit is on)
	std::set<std::string> retired;
	u32 characters_refused = 0; // diagnostics: new keys refused over the limit (the first few logged)
	u32 joins_refused = 0; // diagnostics: refused JoinRequest / WorldLoaded / TransitionVote (the first few logged)
	u32 loaded_ignored = 0;
	u32 votes_refused = 0;
	u32 owners_bad = 0; // client: malformed AvatarList (R3 В3)
	// The waiting point of a level change (protocol 10, xrCoop/coop_transition.h).
	// host: who is ready for which exit; the group goes when the board says so (host_board_step)
	transition::Board board;
	u64 board_ticked = 0; // host: when the board was run last
	u64 board_sent_at = 0; // host: when TransitionState went out last
	u32 board_sent_sig = 0; // host: what it showed (state, exit, who is ready): a change goes out at once
	bool board_sent_idle = true;
	bool board_lead_known = false; // host: the last tick had a leading exit (coop_accept_transition goes through it)
	u32 board_lead = 0;
	bool releasing = false; // host: the level change in flight is the board's own (host_gate_level_change lets it by)
	u64 released_at = 0; // host: when the group was sent off; the old world lives a few frames more, nobody claims in them
	// host: a level change the host's own game asked for with no exit zone behind it (a guide, jump_to_level): what the
	// group goes to when everybody said yes
	struct GuideTrip
	{
		bool set = false;
		GameGraph::_GRAPH_ID graph = 0;
		u32 node = 0;
		Fvector position = {0.f, 0.f, 0.f};
		Fvector angles = {0.f, 0.f, 0.f};
		std::string level;
	} guide;
	// both: the host's board as this machine's placard shows it (the host's own board on the host)
	transition::StateMsg view;
	u64 view_at = 0;
	u32 view_bad = 0; // client: unreadable TransitionState
	// both: this player. `want` is what he does (stands in an exit's zone, or said yes to a guide trip).
	bool want = false;
	u32 want_goal = 0;
	bool zone_in = false; // his actor touches an enabled exit
	u16 zone_changer = kNoId; // ... that one (host numbering)
	u16 withdrawn_from = kNoId; // he cancelled at this exit with the key: no claim until he leaves its zone
	bool guide_yes = false; // client: he said yes to the host's guide trip
	u64 zone_scanned = 0;
	float my_dist = -1.f; // metres from him to the leading exit (< 0 unknown)
	Fvector exit_pos = {0.f, 0.f, 0.f}; // the leading exit, this machine's numbering
	bool exit_known = false;
	bool held_note = false; // the "you cannot go on" line of the log was printed for this wait
	u32 votes_sent = 0, holds_applied = 0; // diagnostics
	transition::Intent intent;
	transition::Hold hold;
	limits::Pacer vote_pacer;
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

// T12 review (Ruling 18): how near a player must be to a level exit to claim it, metres from its nearest shape.
// Stock: an exit takes the actor when its shapes touch his bounding sphere (CCF_Shape::Contact, about 1 m around the
// middle of the body). The host sees the player's character where the player was 0.1-0.3 s ago (interpolation delay
// and one-way latency): 2-3 m back at a sprint. 5 m covers both (transition::kReachM, tested).
constexpr float kVoteReachM = transition::kReachM;
// Host: after the group is sent off the old world still runs a few frames (the level change is a message the world takes in
// the next frames); nobody claims an exit in them, or the countdown would start over for a world that is about to go.
constexpr u64 kAfterReleaseMs = 4000;

void transition_world_replaced(); // below, with the waiting point of a level change

u32 g_world_generation = 0; // CALifeSimulator constructions in this process (outlives State)
u32 g_mirror_generation = 0; // B2: g_world_generation of the world loaded from a host's save (0: none), outlives State

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

// Ruling F3-9: the key's character retired in this world (a hardcore character, F3 Task 10): the host actor's custom data
// keeps it (death::note_retired_key, host_key_retired), also for a later session.
bool host_retired_in_world(const std::string& key_digest)
{
	const CSE_ALifeCreatureActor* a = ai().get_alife() ? ai().alife().graph().actor() : nullptr;
	return a && a->m_ini_string.size() && death::retired_key(a->m_ini_string.c_str(), key_digest);
}

// Ruling F3-10: the key's character retired - in this session, or in this world (the newest kMaxRetiredKeys).
bool host_key_was_retired(const std::string& key_digest)
{
	return g.retired.count(key_digest) != 0 || host_retired_in_world(key_digest);
}

// Ruling F3-10: the new character of this key starts with nothing (no kit, no faction money).
bool host_bare_character(const std::string& key_digest)
{
	return death::new_character_bare(host_key_was_retired(key_digest), death::host_new_kit());
}

// Ruling F3-9: the key had a character - in this session (returned to one, got a new one, retired one) or one that
// retired in this world. Such a key is the same player starting again: coop_max_new_characters is not his.
bool host_key_had_character(const std::string& key_digest)
{
	return g.had_character.count(key_digest) != 0 || host_retired_in_world(key_digest);
}

// The host made a character for a key the world had none for: it spends the limit only if the key never had one.
void host_count_new_character(const std::string& key_digest)
{
	if (death::new_key_charge(host_key_had_character(key_digest), true).spends)
		g.new_characters.add(key_digest);
	g.had_character.insert(key_digest);
}

// Ruling F2-10 b: the client picks its key; every new one used to get a character (starting kit, money) that stays in
// the save for good. A session makes coop_max_new_characters (F2 T3 fix round 1: a host setting, read at each join) for
// keys without a character; a returning player costs nothing, and neither does a key that had a character in this
// session or retired one in this world (Ruling F3-9: a hardcore player starts again). False: he is kicked with the
// reason.
bool host_admits_new_key(PeerId from, const PeerInfo& p, const std::string& key_digest)
{
	const u32 max_new = u32(g_coop_max_new_characters);
	if (death::new_key_charge(host_key_had_character(key_digest), g.new_characters.admits(key_digest, max_new)).admitted)
		return true;
	if (++g.characters_refused <= kRefusalsLogged)
		::Msg("! [coop] '%s' has no character in this world, and this session has made %u new characters "
		      "(coop_max_new_characters %u): refused",
		    p.nick.c_str(), u32(g.new_characters.count()), max_new);
	mgr().session().kick(from, limits::kNoNewCharacterReason, limits::no_new_character_text(max_new));
	return false;
}

bool host_send_save(PeerId from, const PeerInfo& p, CSE_ALifeHumanStalker& proxy_ref);

// Protocol 9: the faction of the host's own character, as the stock new game wrote it into the community of the actor
// (actor_<faction>): the second source of a new character's faction when the world has none (an old save,
// newchar::character_faction). "" when there is none to read: no level, no actor yet (a CharOffer can come in the main
// menu or while the world is still loading) or a community that is not a faction.
// Ruling GS-2: the level first, then Actor(): Actor() asserts on GameID(), which dereferences Level().game (a dead pointer
// without a level: access violation, as coop_money.cpp did before c3783bd). A hostile client must not get another faction
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
	// Ruling F3-10: a retired key's new character starts with nothing. The offer says it with 0 points (a tier never
	// gives 0: coop_menu_char reads a join offer of 0 points as a bare one) - no protocol change; a flag of its own in
	// CharOfferMsg::Flags would be the clean way.
	std::string key_digest;
	const bool bare = key_digest_hex(p.key, key_digest) && host_bare_character(key_digest);
	if (bare)
		m.points = 0;
	m.flags = story ? CharOfferMsg::StoryMode : 0;
	const xr_string level = g_pGameLevel ? Level().name().c_str() : "";
	if (char_name_valid(level.c_str(), CharOfferMsg::kMaxLevel))
		m.level = level.c_str();
	if (refusal && *refusal)
		m.refusal = char_name_valid(refusal, CharOfferMsg::kMaxRefusal) ? refusal : "busy";
	// Protocol 9: the faction is the world's; the player does not choose it. Never empty on the wire: the client refuses
	// an offer without a name.
	m.faction = newchar::character_faction(coop::game::world_faction(), host_actor_faction(), std::string());
	Writer w(96);
	m.write(w);
	mgr().session().send(from, Lane::Reliable, Msg::CharOffer, w);
	g.choosers.offer(from, mgr().now_ms());
	::Msg("[coop] '%s' has no character in this world: he chooses one before the world (economy %u, %u points, %s, "
	      "world faction %s)%s%s%s",
	    p.nick.c_str(), u32(m.eco), m.points, m.level.empty() ? "?" : m.level.c_str(), m.faction.c_str(),
	    bare ? ", hardcore: no kit, no money" : "", m.refusal.empty() ? "" : ", refused: ", m.refusal.c_str());
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
		g.had_character.insert(key_digest); // Ruling F3-9: his next new character costs nothing
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
	host_count_new_character(key_digest);
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

	// Protocol 9 (owner, 2026-10-23: the faction is part of creating the world, other clients do not choose it): the
	// character gets the world's faction. What the client sent in m.faction is only logged, never used - not for the plan,
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
		::Msg("[coop] '%s' chose %s: refused (%s), asked again", p.nick.c_str(), faction.c_str(),
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
	if (!set_faction(*proxy, faction))
		::Msg("! [coop] community '%s' is not in game_relations.ltx: the character keeps its profile's", faction.c_str());
	// money::host_set (coop.money_set) finds a player's character by his peer: his entry knows it from here
	const bool had_entry = g.peers.find(from) != g.peers.end();
	HostPeer& hp = g.peers[from];
	hp.proxy = proxy->ID;
	hp.loading = true;
	bool made = false;
	std::string why = "busy";
	const u16 proxy_id = proxy->ID;
	const bool kit = !host_bare_character(key_digest); // Ruling F3-10
	const LuaCall given = call_char("host_make_character",
	    [&](lua_State* L) {
		    lua_pushnumber(L, lua_Number(from));
		    lua_pushnumber(L, lua_Number(proxy_id));
		    lua_pushlstring(L, faction.data(), faction.size());
		    lua_pushlstring(L, buy.data(), buy.size());
		    lua_pushboolean(L, kit);
		    return 5;
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
	host_count_new_character(key_digest);
	g.choosers.forget(from);
	++g.characters_made;
	::Msg("[coop] '%s' chose %s: character %u made before the world (%u NPC item(s) stripped, %.0f RU), sending the "
	      "world",
	    p.nick.c_str(), faction.c_str(), u32(proxy_id), stripped, plan.money);
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
		const syncapi::SavingForJoin join_save; // F2 T14 fix 1: this save goes to him, coop.sync stays out (save_state)
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
	syncapi::host_send_full(from); // F2 T14: his coop.sync copy, before the world (Reliable lane: ahead of his changes)
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
	transition_world_replaced(); // the host's board of the old world
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
	::Msg("[coop] the host has no character for us: choose gear (economy %u, %u points, level %s, the world's faction "
	      "%s)%s%s",
	    u32(m.eco), m.points, m.level.empty() ? "?" : m.level.c_str(), m.faction.c_str(),
	    m.refusal.empty() ? "" : ", last choice refused: ", m.refusal.c_str());
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
			    lua_pushlstring(L, o.faction.data(), o.faction.size()); // protocol 9: the world's faction
			    return 6;
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

// ---------------------------------------------------------------- the waiting point of a level change (protocol 10)
// xrCoop/coop_transition.h has the rule and the messages. Every machine runs transition_step() each frame: it finds the
// exit zone its player stands in (the stock zone of a CLevelChanger), says so (the host to its own board, a client with
// TransitionVote), keeps the player from walking off the edge of the level while he waits, and keeps the numbers the Lua
// placard reads (coop.transition()). The host also runs the board (host_board_step): who is ready, when the group goes
// (host_release, the same level change coop_accept_transition always made), and tells everybody (TransitionState).

// The stock name of the level game vertex `gv` is on ("l01_escape"), empty if unreadable.
std::string level_name_of(GameGraph::_GRAPH_ID gv)
{
	if (!ai().get_game_graph())
		return std::string();
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(gv))
		return std::string();
	const char* name = gg.header().level(gg.vertex(gv)->level_id()).name().c_str();
	return name ? std::string(name) : std::string();
}

// Host: is anybody else in the session? A player who has joined counts while he loads (the board waits for him, at most
// transition::kLoadWaitMs). With nobody else the host's level changes are the stock ones.
bool host_has_company()
{
	if (!mgr().is_host())
		return false;
	for (const PeerInfo& p : mgr().session().peers())
		if (p.state == PeerState::Joined)
			return true;
	return false;
}

// The group waits at exits on this machine: the stock "go?" dialog and the silent change of the zone stay off there.
bool group_waits_here()
{
	return mgr().is_host() ? host_has_company() : mgr().is_live_client();
}

// On the level's map: inside the AI map's box and the physics volume (past that the host refuses the player's state,
// "position outside the level"). The edge of the level is where a waiting player is held.
bool on_map(const Fvector& p)
{
	if (ai().get_level_graph() && !ai().level_graph().valid_vertex_position(p))
		return false;
	const Fbox& box = Level().ObjectSpace.GetBoundingVolume();
	return p.x >= box.x1 && p.x <= box.x2 && p.z >= box.z1 && p.z <= box.z2;
}

// The enabled exit whose zone this actor touches (the stock rule: CLevelChanger::feel_touch_contact), its id in HOST
// numbering, kNoId if none.
u16 exit_touched(CActor* a)
{
	for (CLevelChanger* lc : g_lchangers)
		if (lc && lc->IsLevelChangerEnabled() && lc->feel_touch_contact(a))
			return to_host(lc->ID());
	return kNoId;
}

// The level `goal` leads to, for the placard: an exit's own, or the guide trip's.
std::string goal_level_name(u32 goal)
{
	if (transition::is_guide(goal))
		return g.guide.level;
	const CSE_ALifeLevelChanger* lc = level_changer_here(u16(goal));
	return lc ? level_name_of(lc->m_tNextGraphID) : std::string();
}

// Host: the level change of `goal`, the one the stock exit sends (R2 I3: built from the host's own entity, or from what
// the host's game asked for in a guide trip). The gate lets it by (g.releasing).
bool host_release(u32 goal)
{
	NET_Packet P;
	P.w_begin(M_CHANGE_LEVEL);
	if (transition::is_guide(goal))
	{
		if (!g.guide.set)
		{
			::Msg("! [coop] transition: the guide trip is gone, nowhere to move the group");
			return false;
		}
		P.w(&g.guide.graph, sizeof(g.guide.graph));
		P.w(&g.guide.node, sizeof(g.guide.node));
		P.w_vec3(g.guide.position);
		P.w_vec3(g.guide.angles);
		::Msg("[coop] moving the group to '%s' (guide trip)", g.guide.level.c_str());
	}
	else
	{
		const CSE_ALifeLevelChanger* lc = level_changer_here(u16(goal));
		if (!lc)
		{
			::Msg("! [coop] the level exit %u is not usable any more", goal);
			return false;
		}
		P.w(&lc->m_tNextGraphID, sizeof(lc->m_tNextGraphID));
		P.w(&lc->m_dwNextNodeID, sizeof(lc->m_dwNextNodeID));
		P.w_vec3(lc->m_tNextPosition);
		P.w_vec3(lc->m_tAngles);
		::Msg("[coop] moving the group to '%s' (level exit %u)", level_name_of(lc->m_tNextGraphID).c_str(), goal);
	}
	g.releasing = true;
	Level().Send(P, net_flags(TRUE)); // the host's own level change; clients follow via SessionReset
	g.releasing = false;
	g.guide.set = false;
	g.released_at = mgr().now_ms();
	return true;
}

// Host: what the board shows changed (who is ready, the state, the exit)? The countdown and the distances do not count.
u32 board_signature(const transition::Status& st)
{
	u32 sig = 17u;
	sig = sig * 31u + u32(st.state);
	sig = sig * 31u + st.goal;
	sig = sig * 31u + st.ready;
	sig = sig * 31u + st.total;
	sig = sig * 31u + (st.load_ms != 0 ? 1u : 0u);
	for (const transition::Row& r : st.rows)
		sig = sig * 31u + r.id * 8u + u32(r.mark);
	return sig;
}

// Host: this machine's placard takes the board at once; the clients get it on a change (at most every 100 ms) and every
// kStateEveryMs while the board is not idle, and once when it goes idle.
void host_board_send(const transition::Status& st, u64 now)
{
	const bool idle = st.state == transition::State::Idle;
	g.view = transition::state_message(st, idle ? std::string() : goal_level_name(st.goal));
	g.view_at = now;
	const u32 sig = board_signature(st);
	if (idle)
	{
		if (g.board_sent_idle)
			return;
	}
	else
	{
		const bool changed = g.board_sent_idle || sig != g.board_sent_sig;
		if (now - g.board_sent_at < (changed ? 100u : u64(transition::kStateEveryMs)))
			return;
	}
	g.board_sent_idle = idle;
	g.board_sent_sig = sig;
	g.board_sent_at = now;
	Writer w(48 + 8 * g.view.rows.size());
	g.view.write(w);
	mgr().session().broadcast(Lane::Reliable, Msg::TransitionState, w); // not queued: a loading client ignores it
}

// Host, ten times a second: the players as the board sees them, the board's verdict, the group's move.
void host_board_step(u64 now)
{
	if (now - g.board_ticked < 100)
		return;
	g.board_ticked = now;
	if (g.released_at != 0 && now - g.released_at < kAfterReleaseMs)
		return; // the group is on its way: the old world is about to go
	if (!g_pGameLevel || !ai().get_alife() || g.world_seen != g_world_generation)
		return; // no world (a level change, a load): nothing to run, the board was cleared with the old world
	const bool company = host_has_company();
	if (!company && g.board.empty() && g.view.state == transition::State::Idle)
		return;
	if (!company)
	{
		g.board.clear(); // the others left: the host's changes are the stock ones again
		g.guide.set = false;
	}
	std::vector<transition::Member> members;
	{
		transition::Member h;
		h.id = 0;
		h.remote = false;
		CActor* a = g_actor ? Actor() : nullptr;
		h.in_world = a != nullptr;
		h.alive = a && a->g_Alive() && survival::life_of(0) == survival::Life::Alive;
		members.push_back(h);
	}
	for (const PeerInfo& p : mgr().session().peers())
	{
		if (p.state != PeerState::Joined)
			continue;
		transition::Member m;
		m.id = p.id;
		const u16 proxy = proxy_of_peer(p.id);
		m.in_world = !sync::host_peer_joining(p.id) && host_player_in_world(proxy);
		m.alive = !m.in_world || survival::life_of(proxy) == survival::Life::Alive;
		members.push_back(m);
	}
	const transition::DistFn dist = [](u32 id, u32 goal) -> float
	{
		if (transition::is_guide(goal))
			return -1.f;
		const CSE_ALifeLevelChanger* lc = level_changer_here(u16(goal));
		CObject* body = id == 0 ? (g_actor ? Actor() : nullptr) : Level().Objects.net_Find(proxy_of_peer(id));
		return lc && body ? distance_to_exit(*lc, body->Position()) : -1.f;
	};
	const transition::Status st = g.board.tick(members, dist, now);
	g.board_lead_known = st.state != transition::State::Idle;
	g.board_lead = st.goal;
	if (st.cancelled)
		::Msg("[coop] transition: cancelled (somebody stepped back or is not ready)");
	if (st.expired)
		::Msg("[coop] transition: the guide trip lapsed, not everybody said yes in %u s", transition::kGuideLifeMs / 1000);
	if (st.state == transition::State::Idle && !st.cancelled)
		g.guide.set = false;
	host_board_send(st, now);
	if (st.go)
		host_release(st.goal);
}

// Both roles, every frame. See the header of this section.
void transition_step(u64 now)
{
	CActor* a = (g_pGameLevel && g_actor && ai().get_alife()) ? Actor() : nullptr;
	if (!a || !group_waits_here())
	{
		g.want = false;
		g.zone_in = false;
		g.zone_changer = kNoId;
		g.withdrawn_from = kNoId;
		g.guide_yes = false;
		g.hold.reset();
		g.intent.reset();
		return; // the host's board drops his claim by itself (host_board_step)
	}
	const u16 touched = exit_touched(a);
	const bool alive = a->g_Alive() && survival::life_of(0) == survival::Life::Alive;
	if (touched == kNoId || touched != g.withdrawn_from)
		g.withdrawn_from = kNoId; // he left the zone he had withdrawn from (or stands in another one)
	g.zone_in = touched != kNoId && alive;
	g.zone_changer = touched;
	bool want = g.zone_in && touched != g.withdrawn_from;
	u32 goal = want ? transition::goal_of_exit(touched) : 0;
	if (mgr().is_client())
	{
		if (g.guide_yes && g.view.state != transition::State::Idle && g.view.kind == 1)
		{
			want = true;
			goal = transition::kGuideGoal;
		}
		else
			g.guide_yes = false;
	}
	if (want && !g.want)
		g.held_note = false;
	g.want = want;
	g.want_goal = goal;
	if (mgr().is_host())
	{
		// His own claim goes to the board; the claim of a guide trip is the board's own business (host_gate_level_change)
		uint32_t claimed_goal = 0;
		if (g.released_at != 0 && now - g.released_at < kAfterReleaseMs)
		{
			// the group is on its way, the old world is about to go: no new claim in it
		}
		else if (want)
			g.board.claim(0, goal, now);
		else if (g.board.claimed(0, &claimed_goal) && !transition::is_guide(claimed_goal))
			g.board.release(0);
	}
	else
	{
		const u32 self = mgr().self();
		bool view_ready = false;
		if (g.view.state != transition::State::Idle)
			for (const transition::Row& r : g.view.rows)
				view_ready = view_ready || (r.id == self && r.mark == transition::Mark::Ready);
		const u32 view_goal = g.view.kind == 1 ? transition::kGuideGoal : u32(g.view.changer);
		const transition::Send send = g.intent.wants(want, goal, view_ready, view_goal, now);
		if (send != transition::Send::Nothing && g.vote_pacer.take(Msg::TransitionVote, now))
		{
			transition::VoteMsg v;
			v.op = send == transition::Send::Ready ? transition::VoteReady : transition::VoteCancel;
			v.changer = (send == transition::Send::Ready && !transition::is_guide(goal)) ? u16(goal) : transition::kNoChanger;
			Writer w(8);
			v.write(w);
			net::client_send(Msg::TransitionVote, w);
			g.intent.sent(now);
			++g.votes_sent;
		}
	}
	// The hold: a player waiting at an exit does not walk off the edge of the level.
	const Fvector pos = a->Position();
	const float at[3] = {pos.x, pos.y, pos.z};
	float back[3] = {0.f, 0.f, 0.f};
	if (g.hold.step(want && !transition::is_guide(goal), g.zone_in, on_map(pos), at, back))
	{
		Fvector to;
		to.set(back[0], back[1], back[2]);
		CCameraBase* cam = a->cam_Active();
		Fvector dir;
		dir.set(cam ? -cam->pitch : 0.f, cam ? -cam->yaw : 0.f, 0.f); // the view stays as it is (MoveActor sets it)
		a->MoveActor(to, dir);
		++g.holds_applied;
		if (!g.held_note)
		{
			g.held_note = true;
			::Msg("[coop] transition: waiting at an exit, the edge of the level holds (put back %.1f m)", pos.distance_to(to));
		}
	}
	// The numbers the placard reads, ten times a second
	if (now - g.zone_scanned >= 100)
	{
		g.zone_scanned = now;
		g.exit_known = false;
		g.my_dist = -1.f;
		if (g.view.state != transition::State::Idle && g.view.kind == 0)
		{
			const CSE_ALifeLevelChanger* lc = level_changer_here(to_local(g.view.changer));
			if (lc)
			{
				g.exit_known = true;
				g.exit_pos = lc->o_Position;
				g.my_dist = distance_to_exit(*lc, pos);
			}
		}
	}
}

// A new world (a level change, a load, the end of the session): nothing of the old one counts.
void transition_world_replaced()
{
	g.released_at = 0; // the world the group was sent off from is gone
	g.board.clear();
	g.guide.set = false;
	g.board_lead_known = false;
	g.board_sent_idle = true;
	g.view = transition::StateMsg();
	g.want = false;
	g.zone_in = false;
	g.zone_changer = kNoId;
	g.withdrawn_from = kNoId;
	g.guide_yes = false;
	g.exit_known = false;
	g.my_dist = -1.f;
	g.hold.reset();
	g.intent.reset();
}

const char* mark_name(transition::Mark m)
{
	switch (m)
	{
	case transition::Mark::Ready: return "ready";
	case transition::Mark::Away: return "away";
	case transition::Mark::Loading: return "loading";
	case transition::Mark::Down: return "down";
	case transition::Mark::Elsewhere: return "elsewhere";
	}
	return "away";
}

void set_lua_number(lua_State* L, const char* key, lua_Number v)
{
	lua_pushnumber(L, v);
	lua_setfield(L, -2, key);
}

void set_lua_string(lua_State* L, const char* key, const char* v)
{
	lua_pushstring(L, v ? v : "");
	lua_setfield(L, -2, key);
}

void set_lua_bool(lua_State* L, const char* key, bool v)
{
	lua_pushboolean(L, v ? 1 : 0);
	lua_setfield(L, -2, key);
}

// coop.transition() -> nil | table, the board as this player's placard shows it (scripts/coop_hud_transition.script):
//   state "waiting" | "counting", kind "exit" | "guide", level (stock name, "l01_escape"), changer (exit id, host numbering),
//   ready, total, countdown_ms (left, counting), load_ms (left of the loaders' minute, 0 = none),
//   rows = { {peer, mark "ready"|"away"|"loading"|"down"|"elsewhere", dist (m, to the exit; left out if unknown)}, ... },
//   me = { peer, host, in_zone (I stand in an exit's zone), ready (the board has me ready), dist (m to the exit),
//          confirm (a guide trip I can say yes to) },
//   pos = {x, y, z} of the exit on this machine (left out for a guide trip).
// nil when nobody waits, outside a session, or when the host's board has been silent for 3 s.
int l_transition(lua_State* L)
{
	const u64 now = mgr().now_ms();
	const transition::StateMsg& v = g.view;
	const bool fresh = mgr().is_host() || now - g.view_at <= 3000;
	if (!mgr().active() || !group_waits_here() || v.state == transition::State::Idle || !fresh)
	{
		lua_pushnil(L);
		return 1;
	}
	const u64 age = now > g.view_at ? now - g.view_at : 0;
	const u32 self = mgr().self();
	lua_newtable(L);
	set_lua_string(L, "state", v.state == transition::State::Counting ? "counting" : "waiting");
	set_lua_string(L, "kind", v.kind == 1 ? "guide" : "exit");
	set_lua_string(L, "level", v.level.c_str());
	if (v.kind == 0)
		set_lua_number(L, "changer", lua_Number(v.changer));
	set_lua_number(L, "ready", lua_Number(v.ready));
	set_lua_number(L, "total", lua_Number(v.total));
	set_lua_number(L, "countdown_ms", lua_Number(v.countdown_ms > age ? v.countdown_ms - age : 0));
	set_lua_number(L, "load_ms", lua_Number(v.load_ms > age ? v.load_ms - age : 0));
	bool me_ready = false;
	lua_newtable(L);
	int index = 0;
	for (const transition::Row& r : v.rows)
	{
		lua_newtable(L);
		set_lua_number(L, "peer", lua_Number(r.id));
		set_lua_string(L, "mark", mark_name(r.mark));
		if (r.dist_m != transition::kUnknownDist)
			set_lua_number(L, "dist", lua_Number(r.dist_m));
		lua_rawseti(L, -2, ++index);
		me_ready = me_ready || (r.id == self && r.mark == transition::Mark::Ready);
	}
	lua_setfield(L, -2, "rows");
	lua_newtable(L);
	set_lua_number(L, "peer", lua_Number(self));
	set_lua_bool(L, "host", mgr().is_host());
	set_lua_bool(L, "in_zone", g.zone_in);
	set_lua_bool(L, "ready", me_ready);
	if (g.my_dist >= 0.f && g.my_dist < 1.0e6f)
		set_lua_number(L, "dist", lua_Number(g.my_dist));
	set_lua_bool(L, "confirm", v.kind == 1 && !me_ready && !g.guide_yes);
	lua_setfield(L, -2, "me");
	if (g.exit_known)
	{
		lua_newtable(L);
		set_lua_number(L, "x", lua_Number(g.exit_pos.x));
		set_lua_number(L, "y", lua_Number(g.exit_pos.y));
		set_lua_number(L, "z", lua_Number(g.exit_pos.z));
		lua_setfield(L, -2, "pos");
	}
	return 1;
}

} // namespace

// Host: a client's TransitionVote. Ready at an exit (checked: an exit of this level, the player is in the world and within
// kVoteReachM of it), Ready without an exit (a yes to the host's guide trip), Cancel.
void host_on_transition_vote(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return;
	transition::VoteMsg v;
	if (!v.read(r))
	{
		if (++g.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] a malformed TransitionVote from #%u: ignored", from);
		return;
	}
	const PeerInfo* p = mgr().session().peer(from);
	const char* nick = p ? p->nick.c_str() : "?";
	if (v.op == transition::VoteCancel)
	{
		if (g.board.release(from))
		{
			g.board_ticked = 0; // the board runs on the next frame
			::Msg("[coop] transition: '%s' is not ready any more", nick);
		}
		return;
	}
	if (!g_pGameLevel || !ai().get_alife())
		return;
	// T12 review (Ruling 18): only a player who is in the world claims, and only at that exit.
	const u16 proxy = proxy_of_peer(from);
	const bool in_world = !sync::host_peer_joining(from) && host_player_in_world(proxy);
	CObject* body = in_world ? Level().Objects.net_Find(proxy) : nullptr;
	if (!body)
	{
		if (++g.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' says he is ready at level exit %u but is not in the world: ignored", nick, v.changer);
		return;
	}
	const u64 now = mgr().now_ms();
	if (v.changer == transition::kNoChanger)
	{
		uint32_t goal = 0;
		if (!g.guide.set || !g.board.claimed(0, &goal) || !transition::is_guide(goal))
		{
			if (++g.votes_refused <= kRefusalsLogged)
				::Msg("! [coop] '%s' says yes to a guide trip but there is none: ignored", nick);
			return;
		}
		g.board.claim(from, transition::kGuideGoal, now);
		g.board_ticked = 0;
		return;
	}
	// R2 I3: the client names a level exit; the host goes only through a level changer of its own level.
	const CSE_ALifeLevelChanger* lc = level_changer_here(v.changer);
	if (!lc)
	{
		if (++g.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' says he is ready at object %u, which is no level exit here: ignored", nick, v.changer);
		return;
	}
	const float away = distance_to_exit(*lc, body->Position());
	if (!(away <= kVoteReachM))
	{
		if (++g.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' says he is ready at level exit %u from %.1f m away (at most %.0f m): ignored", nick,
			    v.changer, away, kVoteReachM);
		return;
	}
	uint32_t before = 0;
	const bool was = g.board.claimed(from, &before) && before == transition::goal_of_exit(v.changer);
	g.board.claim(from, transition::goal_of_exit(v.changer), now);
	g.board_ticked = 0;
	if (!was)
		::Msg("[coop] transition: '%s' is ready at level exit %u (%.1f m)", nick, v.changer, away);
}

// Client: the host's board. The placard reads it (coop.transition()).
void client_on_transition_state(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_client())
		return;
	transition::StateMsg m;
	if (!m.read(r))
	{
		if (++g.view_bad <= kRefusalsLogged)
			::Msg("! [coop] an unreadable TransitionState from the host: ignored");
		return;
	}
	g.view = m;
	g.view_at = mgr().now_ms();
	if (m.state == transition::State::Idle || m.kind != 1)
		g.guide_yes = false;
}

void install(Session& s)
{
	for (const limits::RateRule& rule : limits::kClientRateRules)
		s.limit_rate(rule.type, rule.burst, rule.refill_ms); // R2 I4, T12 review, F2 T3 (host only, per client)
	s.on(Msg::TransitionVote, &host_on_transition_vote);
	s.on(Msg::TransitionState, &client_on_transition_state); // protocol 10
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
	if (!world_up() || !ai().alife().graph().actor()) // world gate: a level half built is no game to host
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
	// The waiting point's fallback (the chat's /go, the console): the group goes at once through the exit somebody waits
	// at, whoever is missing (the host's own hold of the key does the same, transition_go_now).
	if (!mgr().is_host() || !g_pGameLevel || !ai().get_alife() || !g.board_lead_known)
	{
		::Msg("! [coop] no pending transition: nobody waits at a level exit");
		return false;
	}
	g.board.force(g.board_lead);
	g.board_ticked = 0; // the board runs on the next frame
	::Msg("[coop] the host moves the group now (exit %u)", g.board_lead);
	return true;
}

bool transition_go_now()
{
	uint32_t goal = 0;
	if (!mgr().is_host() || !g_pGameLevel || !ai().get_alife() || !g.board.claimed(0, &goal))
	{
		::Msg("! [coop] transition: you are not waiting at an exit");
		return false;
	}
	g.board.force(goal);
	g.board_ticked = 0;
	::Msg("[coop] the host holds the key: the group goes now (exit %u)", goal);
	return true;
}

bool transition_cancel()
{
	if (!group_waits_here())
		return false;
	g.guide_yes = false;
	if (g.zone_in)
		g.withdrawn_from = g.zone_changer; // no claim until he leaves this zone
	if (mgr().is_host())
	{
		g.board.release(0);
		g.board_ticked = 0;
	}
	g.intent.reset(); // the cancel goes out at the next frame
	return true;
}

bool transition_confirm()
{
	if (!mgr().is_client() || g.view.state == transition::State::Idle || g.view.kind != 1)
		return false;
	g.guide_yes = true;
	return true;
}

bool transition_owns_exit(bool exit_enabled)
{
	return exit_enabled && group_waits_here();
}

bool host_gate_level_change(NET_Packet& P)
{
	if (!mgr().is_host() || g.releasing || !host_has_company() || !ai().get_alife())
		return false; // the stock change: one player, or the board's own
	const u64 now = mgr().now_ms();
	const u16 changer = level_changer_of(P);
	if (changer != kNoId)
	{
		// The host's game went through an exit (a dialog opened before somebody joined, a script): the group waits at it.
		g.board.claim(0, transition::goal_of_exit(changer), now);
		g.board_ticked = 0;
		::Msg("[coop] transition: the host asks to go through level exit %u: the group waits for its players", changer);
		return true;
	}
	// No exit leads there: a guide, jump_to_level and the like. The trip is kept for when everybody has said yes.
	constexpr u32 kBody = sizeof(GameGraph::_GRAPH_ID) + sizeof(u32) + 2 * sizeof(Fvector);
	if (P.B.count < sizeof(u16) + kBody)
		return false; // not a level change we can read: the stock path
	const u32 read_at = P.r_pos;
	P.r_pos = sizeof(u16); // after the message type
	Fvector position, angles;
	P.r(&g.guide.graph, sizeof(g.guide.graph));
	P.r(&g.guide.node, sizeof(g.guide.node));
	P.r_vec3(position);
	P.r_vec3(angles);
	P.r_pos = read_at;
	g.guide.position = position;
	g.guide.angles = angles;
	g.guide.level = level_name_of(g.guide.graph);
	g.guide.set = true;
	g.board.start_guide(0, now);
	g.board_ticked = 0;
	::Msg("[coop] transition: the host's game asks to go to '%s' (a guide trip): the group waits for everybody's yes",
	    g.guide.level.c_str());
	return true;
}

void register_lua(lua_State* L)
{
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	lua_pushcfunction(L, &l_transition);
	lua_setfield(L, -2, "transition"); // coop.transition()
	lua_pop(L, 1); // coop
}

// F3 Task 10 step 3. The epoch does not move: client_on_session_reset takes the one it is in (Session::set_world_epoch is a
// no-op for it, no WorldEpoch goes out) and so does the host's session for his JoinRequest. The gate (R1 I4, R2 I4) is
// the one of every join: his join of this world ended with his WorldLoaded, forget voids one still in flight, and a
// JoinRequest within 5 s of his last admission waits (Later) until due() admits it - so nothing has to wait here.
bool host_rejoin_peer(PeerId peer, const char* why)
{
	if (!mgr().is_host() || peer == kHostPeer || !mgr().session().peer(peer))
		return false;
	auto it = g.peers.find(peer);
	if (it == g.peers.end())
		return false; // no join of his reached this world: there is nothing to do again
	g.gate.forget(peer);
	g.precache_wait.erase(std::remove(g.precache_wait.begin(), g.precache_wait.end(), peer), g.precache_wait.end());
	g.choosers.forget(peer);
	const u16 proxy = it->second.proxy;
	it->second.proxy = kNoId; // set again by his JoinRequest (his character by key, or a new one)
	it->second.loading = true;
	it->second.sent_at = mgr().now_ms(); // review R2 C1: the join timeout measures this rejoin
	if (proxy != kNoId)
		world::host_park_proxy(proxy); // nobody plays it now (the caller may release it right after)
	sync::host_begin_join(peer); // queue / drop world traffic to him until he is back
	Writer w(96);
	w.str(why && *why ? why : "the host sends you the world again");
	w.u32(g.epoch);
	mgr().session().send(peer, Lane::Reliable, Msg::SessionReset, w);
	host_send_owners(); // R3 В3: nobody plays that character any more
	const PeerInfo* p = mgr().session().peer(peer);
	::Msg("[coop] '%s' joins the world again (%s, epoch %u), his character %u is nobody's now", p ? p->nick.c_str() : "?",
	    why && *why ? why : "-", g.epoch, u32(proxy));
	return true;
}

void host_key_retired(const std::string& key_digest)
{
	if (!mgr().is_host() || key_digest.empty())
		return;
	g.had_character.insert(key_digest);
	g.retired.insert(key_digest);
	CSE_ALifeCreatureActor* a = ai().get_alife() ? ai().alife().graph().actor() : nullptr;
	if (!a)
		return;
	std::string text = a->m_ini_string.size() ? a->m_ini_string.c_str() : "";
	const std::string before = text;
	death::note_retired_key(text, key_digest);
	if (text != before)
		a->m_ini_string = text.c_str();
}

void client_request_level_change(NET_Packet& P)
{
	if (!mgr().is_live_client())
		return;
	// R2 I3: the host builds the level change itself; it only learns which of its level changers we stand in. The stock
	// zone does not get this far in a session (transition_owns_exit): this is a level change our own game asked for.
	const u16 changer = level_changer_of(P);
	if (changer == kNoId)
	{
		::Msg("! [coop] level exit: no level changer of this level leads there, the host is not asked");
		return;
	}
	transition::VoteMsg v;
	v.op = transition::VoteReady;
	v.changer = to_host(changer);
	Writer w(4);
	v.write(w);
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
	g.board.forget(id); // nobody of his stands at an exit any more
	g.board_ticked = 0;
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
	transition_world_replaced(); // a new session: no board of an earlier one
	g.my_proxy = kNoId;
	g.load_issued = false;
	g.char_step = State::CharStep::None;
	g.receive_started = mgr().now_ms();
	mgr().set_phase(Phase::ReceivingWorld);
	mgr().session().send(kHostPeer, Lane::Reliable, Msg::JoinRequest, Writer());
}

u16 my_proxy() { return g.my_proxy; }

bool client_loading_world() { return mgr().phase() == Phase::LoadingWorld; }

bool world_is_mirror()
{
	if (mgr().is_host())
		return false; // a mirror kept after a dropped session and hosted from: the world is this machine's now
	return client_loading_world() || (g_mirror_generation != 0 && g_mirror_generation == g_world_generation);
}

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
		transition_world_replaced(); // a wait at an exit of the old world means nothing here
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
	if (!g.reset_pending || !world_up() || host_precaching()) // world gate: bReady too, not just g_pGameLevel
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
		transition_step(mgr().now_ms()); // the host's own zone, his hold at the edge, the placard's numbers
		host_board_step(mgr().now_ms());
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
	if (mgr().is_live_client())
		transition_step(mgr().now_ms()); // his zone, his hold at the edge of the level, the placard's numbers
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
		if (g.view.state != transition::State::Idle)
			::Msg("[coop]   transition: %s at %s (exit %u), %u of %u ready%s", g.view.state == transition::State::Counting ? "counting down" : "waiting",
			    g.view.level.c_str(), u32(g.view.changer), u32(g.view.ready), u32(g.view.total),
			    g.view.kind == 1 ? " (a guide trip)" : "");
		for (const auto& it : g.peers)
			::Msg("[coop]   peer #%u proxy %u%s%s", it.first, it.second.proxy, it.second.loading ? " (loading)" : "",
			    g.gate.waiting(it.first) ? " (join waits for its turn)" : "");
	}
	else if (mgr().is_client())
	{
		::Msg("[coop]   my proxy (host numbering) %u, swap %s", g.my_proxy, g.swap_done ? "done" : "pending");
		::Msg("[coop]   transition: %s%s, %u vote(s) sent, held at the edge %u time(s)",
		    g.view.state == transition::State::Idle ? "nobody waits" : "the host's board is on",
		    g.want ? ", I am ready" : "", g.votes_sent, g.holds_applied);
		if (g.char_step != State::CharStep::None)
			::Msg("[coop]   new character: %s", g.char_step == State::CharStep::Sent ? "chosen, waiting for the world"
			                                                                       : "choosing (window)");
	}
}

void on_alife_objects_loaded(CALifeObjectRegistry& objects)
{
	// B2: the world being loaded is the host's; on_world_created, at the end of this simulator's constructor, gives it
	// the next generation. It stays a mirror after the session ends, until another world replaces it.
	if (client_loading_world())
		g_mirror_generation = g_world_generation + 1;
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
