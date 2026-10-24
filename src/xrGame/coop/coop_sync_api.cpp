#include "stdafx.h"
#include "pch_script.h"
#include "coop_sync_api.h"

#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_events.h"
#include "coop_sync.h"
#include "xrCoop/coop_limits.h"
#include "xrCoop/coop_syncnet.h"

#include "ai_space.h"
#include "script_engine.h"
#include "Level.h"
#include "Actor.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <luabind/detail/class_rep.hpp>
#include <luabind/detail/object_rep.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace coop
{
namespace syncapi
{
namespace
{
namespace ss = syncstore;
namespace sn = syncnet;
using ss::ScopeId;
using ss::SetResult;
using ss::Value;
using ss::ValueType;

constexpr u16 kNone = 0xffff;
constexpr size_t kMaxOutbox = 1024; // client: messages for the host waiting for the pacer
constexpr size_t kMaxCalls = 4096; // Lua calls waiting for frame()
constexpr u32 kCallsPerFrame = 256;
constexpr size_t kMaxHostCalls = 256; // host: requests waiting for its Lua
constexpr u32 kHostCallsPerFrame = 64;
constexpr size_t kMaxWatches = 1024; // on_change handlers
constexpr size_t kMaxHandlers = 256; // request handlers (= RequestLimits::kMaxRules)
constexpr size_t kMaxEffectNames = 256;
constexpr size_t kMaxEffectFns = 16; // on() handlers per name
constexpr float kContainerReach = 15.f; // as coop_requests kReachRange: a player at a box he takes from
constexpr float kMaxRadius = 100000.f;
constexpr int kExportFormat = 1;
constexpr size_t kMaxScopeText = 32; // "player:4294967294" and the like

// A message waiting in frame(): a client's for the host (paced), the host's for clients.
struct Out
{
	bool everyone = false; // host: to every client in the world but `except`
	PeerId peer = kInvalidPeer; // host: to this one
	PeerId except = kInvalidPeer;
	Msg type = Msg::SyncSet;
	Writer payload;
};

// A Lua function waiting for an answer: a registry reference of the Lua state it was made in.
struct Waiting
{
	int ref = LUA_NOREF;
	u32 gen = 0;
};

// A call of Lua that frame() makes on the main thread, in the order queued.
struct Call
{
	enum Kind : u8
	{
		Changed, // on_change handlers whose scope and prefix match
		Answer, // a request's callback
		Effect, // on() handlers of a broadcast
	} kind = Changed;
	sn::Change change; // Changed
	Waiting fn; // Answer
	bool ok = false; // Answer
	Value value; // Answer: the result; Effect: the args
	std::string name; // Effect
	PeerId from = kHostPeer; // Effect
};

// Host: a request waiting for the host's Lua.
struct HostCall
{
	PeerId from = kHostPeer; // the player who asked (F3a T7: kHostPeer is the host player, over the internal channel)
	u32 id = 0;
	std::string name;
	Value args;
	bool here = false; // a request of this machine's own Lua outside a session (single player): answered here
	Waiting fn; // `here`: its callback
};

struct Watch
{
	sn::ScopeName scope;
	std::string prefix;
	int ref = LUA_NOREF;
};

struct Stats
{
	u64 lua_sets = 0, lua_refused = 0; // coop.sync.set on this machine
	u64 sets_out = 0, requests_out = 0, held = 0; // client: sent to the host; frames the pacer held something back
	u64 requests_answered = 0, requests_timed_out = 0, requests_ended = 0, answers_unknown = 0; // client
	u64 host_requests = 0, no_handler = 0, rate_limited = 0, busy = 0, not_in_world = 0, bad_requests = 0;
	u64 handler_errors = 0, bad_results = 0, results_out = 0; // host
	u64 client_sets = 0, client_sets_not_in_world = 0; // host: changes of clients taken
	u64 client_refused = 0, host_keys_refused = 0; // host: client changes refused (fix 1: of them the host's keys)
	u64 casts = 0, cast_machines = 0, casts_in = 0, effects = 0; // broadcast
	u64 calls = 0, lua_errors = 0, calls_dropped = 0, calls_stale = 0;
	u64 bad_messages = 0;
	u64 copies = 0, copy_bytes = 0, resyncs = 0;
	u64 exports = 0, imports = 0, imports_bad = 0;
	u64 destroyed = 0; // object scopes dropped with their object
};

sn::ClientCore g_core; // the store (the host's and a client's) and a client's own changes over it
bool g_as_client = false; // the role the store was last used in
sn::RequestLimits g_limits; // host
sn::Requests<Waiting> g_requests; // client: requests in flight
limits::Pacer g_pacer; // client
std::deque<Out> g_out;
std::deque<Writer> g_player_out; // F3a T7, host: the host player's SyncRequests for his world server (sent by frame())
std::deque<Call> g_calls;
std::deque<HostCall> g_host_calls;
std::vector<Watch> g_watches;
std::map<std::string, int, std::less<>> g_handlers; // host: request name -> function
std::map<std::string, std::vector<int>, std::less<>> g_effects; // effect name -> functions
std::map<PeerId, std::string> g_peer_digest; // host: player key digests of connected players
std::string g_own_digest;
u32 g_lua_gen = 0; // register_lua calls: a reference of another generation belongs to a closed Lua state
bool g_resync = false; // host: import replaced the store: every client gets a new full copy
bool g_was_live = false; // client: in the world at the last frame
u32 g_join_saves = 0; // fix 1: SavingForJoin guards alive (the host writes a join save)
Stats g_stats;

bool log_allowed(u64 count) { return count <= 10 || count % 100 == 0; }

// --- roles and players ----------------------------------------------------------------------------

bool client() { return mgr().is_client(); }
PeerId self_peer() { return client() ? mgr().self() : kHostPeer; }
u16 host_id_of(u16 local) { return client() ? join::to_host(local) : local; }
u16 local_id_of(u32 host) { return client() ? join::to_local(u16(host)) : u16(host); }

void forget_world_calls()
{
	for (auto it = g_calls.begin(); it != g_calls.end();)
		it = it->kind == Call::Answer ? std::next(it) : g_calls.erase(it);
}

void queue_call(Call&& c);

// Requests in flight end: their callbacks get false, `why` (delivered when Lua is ready).
void end_requests(const char* why)
{
	for (auto& w : g_requests.take_all())
	{
		Call c;
		c.kind = Call::Answer;
		c.fn = w.second;
		c.ok = false;
		c.value = ss::make_string(why);
		queue_call(std::move(c));
		++g_stats.requests_ended;
	}
}

// A machine that turns into a client (or back) keeps nothing of the other role's store.
void follow_role()
{
	const bool c = client();
	if (c == g_as_client)
		return;
	g_as_client = c;
	g_core.clear();
	g_out.clear();
	g_player_out.clear();
	g_pacer.reset();
	g_peer_digest.clear();
	g_resync = false;
	g_was_live = false;
	end_requests("session ended");
	forget_world_calls();
}

const std::string& own_digest()
{
	if (g_own_digest.empty() && !key_digest_hex(mgr().player_key(), g_own_digest))
		g_own_digest.clear();
	return g_own_digest;
}

// The player key digest of `peer`: this machine's own, or (host) a connected player's.
bool peer_digest(PeerId peer, std::string& out)
{
	if (peer == self_peer())
	{
		out = own_digest();
		return !out.empty();
	}
	if (!mgr().is_host())
		return false;
	const auto it = g_peer_digest.find(peer);
	if (it != g_peer_digest.end())
	{
		out = it->second;
		return true;
	}
	const PeerInfo* p = mgr().session().peer(peer);
	if (!p || !key_digest_hex(p->key, out))
		return false;
	g_peer_digest[peer] = out;
	return true;
}

// Host: the connected player whose Player scope this is; kInvalidPeer for none (the host's own, an absent player, not a
// Player scope).
PeerId player_peer_of(const ScopeId& s)
{
	if (s.kind != ScopeId::Player || !mgr().is_host())
		return kInvalidPeer;
	for (const PeerInfo& p : mgr().session().peers())
	{
		std::string d;
		if (p.id != kHostPeer && p.state == PeerState::Joined && peer_digest(p.id, d) && d == s.player_key)
			return p.id;
	}
	return kInvalidPeer;
}

// Host: a client that is in the world (not loading it). F3a T7: the host player (peer 0, over the internal channel)
// while the host's level and actor are up.
bool in_world(PeerId peer)
{
	if (peer == kHostPeer)
		return mgr().is_host() && mgr().session().local() && g_pGameLevel && g_actor;
	const PeerInfo* p = mgr().session().peer(peer);
	return p && p->state == PeerState::Joined && !sync::host_peer_joining(peer);
}

// The character of `peer` in this machine's numbering (0 = our own actor), kNone if unknown.
u16 character_of(PeerId peer) { return peer == self_peer() ? u16(0) : world::avatar_of(peer); }

// Where an object is: its top holder's place (an item in a pocket is where the pocket is).
bool position_of(u16 id, Fvector& out)
{
	u16 root = events::root_of(id);
	if (root == kNone)
		root = id;
	if (g_pGameLevel)
		if (CObject* o = Level().Objects.net_Find(root))
		{
			out = o->Position();
			return true;
		}
	if (CSE_Abstract* e = events::find_entity(root))
	{
		out = e->o_Position;
		return true;
	}
	return false;
}

bool within(PeerId peer, u16 id, float dist)
{
	const u16 who = character_of(peer);
	Fvector a, b;
	return who != kNone && position_of(who, a) && position_of(id, b) && a.distance_to(b) <= dist;
}

bool owns(PeerId peer, u16 item)
{
	const u16 who = character_of(peer);
	return who != kNone && item != kNone && events::find_entity(item) && events::root_of(item) == who;
}

bool is_container(CSE_Abstract* e)
{
	if (smart_cast<CSE_ALifeInventoryBox*>(e))
		return true;
	return smart_cast<CSE_ALifeTraderAbstract*>(e) && !world::is_player_character(e); // a corpse, a trader
}

bool in_container(PeerId peer, u16 item, u16 box)
{
	CSE_Abstract* e = item == kNone ? nullptr : events::find_entity(item);
	CSE_Abstract* b = box == kNone ? nullptr : events::find_entity(box);
	return e && b && e->ID_Parent == box && is_container(b) && within(peer, box, kContainerReach);
}

// Host: `peer` holds object `id` (host numbering), which exists.
bool held_by(u32 id, PeerId peer)
{
	if (id >= kNone)
		return false;
	const u16 proxy = join::proxy_of_peer(peer);
	return proxy != kNone && events::find_entity(u16(id)) && events::root_of(u16(id)) == proxy;
}

// The player scope of a character (host numbering): by the key on its custom data (sn::player_scope_of); 0 is the
// host's own actor, whose scope is the one of the host's own key.
bool character_scope(u16 character, ScopeId& out)
{
	if (character == 0)
	{
		const std::string& own = own_digest();
		if (!ss::player_key_ok(own))
			return false;
		out = ScopeId::player(own);
		return true;
	}
	CSE_Abstract* e = events::find_entity(character);
	return e && e->m_ini_string.size() && sn::player_scope_of(e->m_ini_string.c_str(), out);
}

// The store scope a Lua scope name stands for on this machine.
bool resolve(const sn::ScopeName& n, ScopeId& out, const char*& why)
{
	switch (n.kind)
	{
	case sn::ScopeName::World: out = ScopeId::world(); return true;
	case sn::ScopeName::Session: out = ScopeId::session(); return true;
	case sn::ScopeName::Player:
	case sn::ScopeName::PlayerOf:
	{
		std::string d;
		if (!peer_digest(n.kind == sn::ScopeName::Player ? self_peer() : n.peer, d))
		{
			why = n.kind == sn::ScopeName::Player ? "no player key" :
			    client()                           ? "another player's scope is the host's" :
			                                         "no such player";
			return false;
		}
		out = ScopeId::player(std::move(d));
		return true;
	}
	case sn::ScopeName::Object: out = ScopeId::object(host_id_of(n.id)); return true;
	case sn::ScopeName::AnyObject: why = "\"object\" names every object: on_change only"; return false;
	}
	why = "bad scope";
	return false;
}

const char* result_why(SetResult r)
{
	switch (r)
	{
	case SetResult::Ok:
	case SetResult::Same: return "";
	case SetResult::BadKey: return "bad key";
	case SetResult::BadValue: return "bad value";
	case SetResult::TooBig: return "value too large";
	case SetResult::ScopeFull: return "scope full";
	case SetResult::NotAllowed: return "not allowed";
	case SetResult::Stale: return "stale";
	}
	return "?";
}

// --- values between Lua and the store -------------------------------------------------------------

int abs_index(lua_State* L, int idx) { return idx < 0 && idx > LUA_REGISTRYINDEX ? lua_gettop(L) + idx + 1 : idx; }

// A luabind `vector` (Fvector) at idx; nullptr for anything else. Only raw reads: nothing of Lua runs.
const Fvector* as_vector(lua_State* L, int idx)
{
	idx = abs_index(L, idx);
	if (lua_type(L, idx) != LUA_TUSERDATA || !lua_getmetatable(L, idx))
		return nullptr;
	lua_pushstring(L, "__luabind_class");
	lua_rawget(L, -2);
	const bool luabind_object = lua_toboolean(L, -1) != 0;
	lua_pop(L, 2);
	if (!luabind_object)
		return nullptr;
	const luabind::detail::object_rep* o = static_cast<const luabind::detail::object_rep*>(lua_touserdata(L, idx));
	const luabind::detail::class_rep* c = o ? o->crep() : nullptr;
	// Fix 1 (review M6): the C++ class Fvector itself, not a class that is only named "vector" (a Lua class could be).
	if (!c || c->get_class_type() != luabind::detail::class_rep::cpp_class ||
	    !(LUABIND_TYPE_INFO_EQUAL(c->type(), LUABIND_TYPEID(Fvector))) || !o->ptr())
		return nullptr;
	return static_cast<const Fvector*>(o->ptr());
}

void push_vector(lua_State* L, const float v[3])
{
	Fvector f;
	f.set(v[0], v[1], v[2]);
	luabind::detail::convert_to_lua(L, f);
}

void push_value(lua_State* L, const Value& v)
{
	switch (v.type)
	{
	case ValueType::Nil: lua_pushnil(L); return;
	case ValueType::Number: lua_pushnumber(L, lua_Number(v.num)); return;
	case ValueType::String: lua_pushlstring(L, v.str.data(), v.str.size()); return;
	case ValueType::Bool: lua_pushboolean(L, v.b ? 1 : 0); return;
	case ValueType::Vec3: push_vector(L, v.v); return;
	case ValueType::Table:
		lua_createtable(L, 0, int(v.table.size()));
		for (const auto& f : v.table)
		{
			int64_t index = 0;
			if (sn::int_key_of(f.first, index))
				lua_pushnumber(L, lua_Number(index));
			else
				lua_pushlstring(L, f.first.data(), f.first.size());
			push_value(L, f.second);
			lua_rawset(L, -3);
		}
		return;
	}
	lua_pushnil(L);
}

// A key of a Lua table: a string (not starting with the index tag), or a whole number up to 2^53 (an array index).
bool table_key(lua_State* L, int idx, std::string& out, std::string& why)
{
	switch (lua_type(L, idx))
	{
	case LUA_TSTRING:
	{
		size_t n = 0;
		const char* s = lua_tolstring(L, idx, &n); // a string: nothing is converted (lua_next stays valid)
		if (n > ss::kMaxKey)
		{
			why = "bad value: a table key longer than 128 bytes";
			return false;
		}
		out.assign(s, n);
		if (!sn::text_key_ok(out))
		{
			why = "bad value: a table key that starts with byte 1";
			return false;
		}
		return true;
	}
	case LUA_TNUMBER:
	{
		const double d = double(lua_tonumber(L, idx));
		if (!(d == std::floor(d)) || std::fabs(d) > double(sn::kMaxIntKey))
		{
			why = "bad value: a table index that is not a whole number up to 2^53";
			return false;
		}
		out = sn::int_key(int64_t(d));
		return true;
	}
	default:
		why = std::string("bad value: a table key of type ") + lua_typename(L, lua_type(L, idx));
		return false;
	}
}

// A scalar at idx (number, string, boolean, vector; nil only at the top). `size` += its encoded size; `copy`: build it
// (false once the value is known to be too large: only its size is counted then).
bool scalar(lua_State* L, int idx, Value& out, size_t& size, bool copy, std::string& why)
{
	switch (lua_type(L, idx))
	{
	case LUA_TNONE: // fix 1 (review I2): a value left out, coop.sync.request("x") / set(scope, key): nil, as in Lua
	case LUA_TNIL:
		out = Value();
		size += 1;
		return true;
	case LUA_TBOOLEAN:
		out = ss::make_bool(lua_toboolean(L, idx) != 0);
		size += 2;
		return true;
	case LUA_TNUMBER:
	{
		const double d = double(lua_tonumber(L, idx));
		if (!std::isfinite(d))
		{
			why = "bad value: a number that is NaN or infinite";
			return false;
		}
		out = ss::make_number(d);
		size += 9;
		return true;
	}
	case LUA_TSTRING:
	{
		size_t n = 0;
		const char* s = lua_tolstring(L, idx, &n);
		size += 1 + ss::varu_size(n) + n;
		if (copy && n <= ss::kMaxValueBytes)
			out = ss::make_string(std::string(s, n));
		return true;
	}
	case LUA_TUSERDATA:
		if (const Fvector* v = as_vector(L, idx))
		{
			const float f[3] = {v->x, v->y, v->z};
			if (!validate::finite3(f))
			{
				why = "bad value: a vector that is not finite";
				return false;
			}
			out = ss::make_vec3(v->x, v->y, v->z);
			size += 13;
			return true;
		}
		why = "bad value: userdata that is not a vector";
		return false;
	default:
		why = std::string("bad value: a ") + lua_typename(L, lua_type(L, idx));
		return false;
	}
}

// The Lua value at idx as a store value, checked as the store checks it and sized as it is encoded (without copying
// past kMaxValueBytes). Only raw reads: nothing of Lua runs, nothing can raise.
bool to_value(lua_State* L, int idx, Value& out, std::string& why)
{
	idx = abs_index(L, idx);
	Value v;
	size_t size = 0;
	if (lua_type(L, idx) == LUA_TTABLE)
	{
		v.type = ValueType::Table;
		size = 1;
		size_t fields = 0;
		lua_pushnil(L);
		while (lua_next(L, idx))
		{
			std::string key;
			Value f;
			if (!table_key(L, -2, key, why))
			{
				lua_pop(L, 2);
				return false;
			}
			if (lua_type(L, -1) == LUA_TTABLE)
			{
				why = "bad value: a table inside a table (only flat tables)";
				lua_pop(L, 2);
				return false;
			}
			size_t field = 0;
			if (!scalar(L, -1, f, field, size <= ss::kMaxValueBytes, why))
			{
				lua_pop(L, 2);
				return false;
			}
			size += ss::varu_size(key.size()) + key.size() + field;
			++fields;
			if (size <= ss::kMaxValueBytes)
				v.table.emplace_back(std::move(key), std::move(f));
			lua_pop(L, 1);
		}
		size += ss::varu_size(fields);
	}
	else if (!scalar(L, idx, v, size, true, why))
		return false;
	if (size > ss::kMaxValueBytes)
	{
		why = "value too large (" + std::to_string(size) + " > " + std::to_string(ss::kMaxValueBytes) + ")";
		return false;
	}
	ss::canonicalize(v);
	const SetResult form = ss::check_value(v);
	if (form != SetResult::Ok)
	{
		why = form == SetResult::TooBig ?
		    "value too large (" + std::to_string(ss::encoded_size(v)) + " > " + std::to_string(ss::kMaxValueBytes) + ")" :
		    std::string("bad value");
		return false;
	}
	out = std::move(v);
	return true;
}

// --- calling Lua ------------------------------------------------------------------------------------

int traceback(lua_State* L)
{
	const char* msg = lua_tostring(L, 1);
	if (!msg)
		msg = "(error object is not a string)";
	lua_pushstring(L, "debug");
	lua_rawget(L, LUA_GLOBALSINDEX);
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

lua_State* lua() { return ai().script_engine().lua(); }

// Lua may be called: a level with the actor; a client only once it is in the world (P6).
bool lua_ready()
{
	if (!g_pGameLevel || !g_actor || !lua())
		return false;
	return !client() || mgr().is_live_client();
}

// Calls the registry function `ref` with what push(L) pushed, protected, with `nres` results for read(L) (on success
// only). An error is logged and counted; the stack is left as it was.
template <class Push, class Read>
bool pcall_ref(int ref, const char* what, const char* name, Push push, int nres, Read read)
{
	lua_State* L = lua();
	if (!L || ref == LUA_NOREF || ref == LUA_REFNIL)
		return false;
	const int top = lua_gettop(L);
	if (!lua_checkstack(L, 24))
		return false;
	lua_pushcfunction(L, &traceback);
	const int errfunc = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, top);
		return false;
	}
	const int nargs = push(L);
	const bool ok = lua_pcall(L, nargs, nres, errfunc) == 0;
	if (ok)
		read(L);
	else
	{
		++g_stats.lua_errors;
		if (log_allowed(g_stats.lua_errors))
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] lua error in coop.sync %s '%s' (#%llu, session continues): %s", what, name, g_stats.lua_errors,
			    err ? err : "?");
		}
	}
	lua_settop(L, top);
	return ok;
}

void unref(int ref)
{
	if (lua_State* L = lua())
		if (ref != LUA_NOREF && ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

// --- the frame pump outside a session ---------------------------------------------------------------

// Manager::frame (and so sync::frame) does not run outside a session; single player work goes on through this.
class Pump : public pureFrame
{
public:
	void _BCL OnFrame() override;
};
Pump g_pump;
bool g_pump_on = false;

void pump_sync()
{
	const bool need = !mgr().active() && (!g_calls.empty() || !g_host_calls.empty());
	if (need && !g_pump_on)
	{
		Device.seqFrame.Add(&g_pump, REG_PRIORITY_LOW);
		g_pump_on = true;
	}
	else if (!need && g_pump_on)
	{
		Device.seqFrame.Remove(&g_pump); // deferred by CRegistrator when called from inside OnFrame
		g_pump_on = false;
	}
}

void _BCL Pump::OnFrame()
{
	if (!mgr().active())
		frame();
	else
		pump_sync();
}

void queue_call(Call&& c)
{
	if (g_calls.size() >= kMaxCalls)
	{
		g_calls.pop_front();
		++g_stats.calls_dropped;
		if (log_allowed(g_stats.calls_dropped))
			::Msg("! [coop] coop.sync: the queue of Lua calls is full, the oldest one dropped");
	}
	g_calls.push_back(std::move(c));
	pump_sync();
}

void queue_change(sn::Change&& ch)
{
	if (g_watches.empty())
		return;
	Call c;
	c.kind = Call::Changed;
	c.change = std::move(ch);
	queue_call(std::move(c));
}

void queue_changes(std::vector<sn::Change>& changes)
{
	for (sn::Change& e : changes)
		queue_change(std::move(e));
}

// --- messages ---------------------------------------------------------------------------------------

void push_out(Out&& o) { g_out.push_back(std::move(o)); }

void out_to(PeerId peer, Msg type, Writer&& w)
{
	Out o;
	o.peer = peer;
	o.type = type;
	o.payload = std::move(w);
	push_out(std::move(o));
}

// Host: one SyncSet of host_set / host_drop, queued for frame() (in order with everything else).
void queue_delivery(const sn::Delivery& d)
{
	Writer w(64 + d.msg.key.size() + ss::encoded_size(d.msg.value));
	sn::write_host_set(w, d.msg);
	Out o;
	o.everyone = d.to == sn::Delivery::Everyone;
	o.peer = d.peer;
	o.except = d.except;
	o.type = Msg::SyncSet;
	o.payload = std::move(w);
	push_out(std::move(o));
}

void queue_deliveries(const std::vector<sn::Delivery>& out)
{
	if (!mgr().is_host())
		return; // single player: nobody to tell
	for (const sn::Delivery& d : out)
		queue_delivery(d);
}

// Host (and single player): one change made by `by`, stamped and told to whom it concerns.
SetResult host_change(const ScopeId& scope, std::string_view key, Value value, PeerId by, bool by_owner, const u32* ack)
{
	std::vector<sn::Delivery> out;
	sn::Change event;
	const SetResult r =
	    sn::host_set(g_core.store(), scope, key, std::move(value), by, by_owner, ack, player_peer_of(scope), out, &event);
	queue_deliveries(out);
	if (r == SetResult::Ok)
		queue_change(std::move(event));
	return r;
}

void send_result(PeerId to, u32 id, bool ok, Value&& value)
{
	sn::Result m;
	m.id = id;
	m.ok = ok;
	m.value = std::move(value);
	Writer w(32 + ss::encoded_size(m.value));
	sn::write_result(w, m);
	out_to(to, Msg::SyncResult, std::move(w));
	++g_stats.results_out;
}

void answer_refused(PeerId to, u32 seq, SetResult why)
{
	sn::Delivery d;
	d.to = sn::Delivery::One;
	d.peer = to;
	d.msg.flags = sn::kAck | sn::kRefused;
	d.msg.by = to;
	d.msg.ack = seq;
	d.msg.refused = why;
	queue_delivery(d);
}

void bad_message(PeerId from, const char* what)
{
	++g_stats.bad_messages;
	if (log_allowed(g_stats.bad_messages))
		::Msg("! [coop] coop.sync: malformed %s from #%u dropped", what, from);
}

// Host: everything queued goes out, in order (a player loading the world gets it after his WorldLoaded).
void host_flush()
{
	if (!mgr().is_host())
	{
		g_out.clear();
		return;
	}
	while (!g_out.empty())
	{
		Out o = std::move(g_out.front());
		g_out.pop_front();
		if (o.everyone)
			net::host_broadcast(o.type, o.payload, Lane::Reliable, o.except);
		else if (o.peer != kHostPeer || o.type == Msg::SyncResult) // F3a T7: the host player's answers over the link
			net::host_send(o.peer, o.type, o.payload, Lane::Reliable);
	}
}

// Client: what waits for the host goes out as fast as the host takes it (Ruling F2-14), once we are in the world.
void client_flush(u64 now)
{
	if (!mgr().is_live_client())
		return;
	while (!g_out.empty())
	{
		Out& o = g_out.front();
		if (!g_pacer.take(o.type, now))
		{
			++g_stats.held;
			break;
		}
		net::client_send(o.type, o.payload, Lane::Reliable);
		g_out.pop_front();
	}
}

// F3a T7: the host player's requests go to his world server over the internal channel (main thread); one not answered in
// time gets false, "timeout", as a client's.
void host_player_frame(u64 now)
{
	while (!g_player_out.empty())
	{
		net::client_send(Msg::SyncRequest, g_player_out.front(), Lane::Reliable);
		g_player_out.pop_front();
	}
	for (auto& w : g_requests.expire(now))
	{
		Call c;
		c.kind = Call::Answer;
		c.fn = w.second;
		c.ok = false;
		c.value = ss::make_string("timeout");
		queue_call(std::move(c));
		++g_stats.requests_timed_out;
	}
}

std::vector<Writer> full_copy_for(PeerId peer)
{
	std::string digest;
	if (!peer_digest(peer, digest))
		digest.clear(); // no Player scope then
	return sn::encode_full(g_core.store(), digest);
}

void count_copy(const std::vector<Writer>& parts)
{
	++g_stats.copies;
	for (const Writer& p : parts)
		g_stats.copy_bytes += p.size();
}

// Host: import replaced the store: every client in the session gets a new copy, in order with the rest (queued for one
// that loads the world; a rejoin after a load gets its own at its world).
void resync_all()
{
	for (const PeerInfo& p : mgr().session().peers())
	{
		if (p.id == kHostPeer || p.state != PeerState::Joined)
			continue;
		const std::vector<Writer> parts = full_copy_for(p.id);
		for (const Writer& part : parts)
			net::host_send(p.id, Msg::SyncFull, part, Lane::Reliable);
		count_copy(parts);
		++g_stats.resyncs;
	}
}

// --- network handlers -------------------------------------------------------------------------------

void host_on_set(PeerId from, Reader& r)
{
	Reader peek = r;
	const u32 seq = peek.u32();
	sn::ClientSet m;
	if (!sn::read_client_set(r, m))
	{
		bad_message(from, "SyncSet");
		if (peek.ok())
			answer_refused(from, seq, SetResult::BadValue); // his change waits for an answer
		return;
	}
	if (!in_world(from))
	{
		++g_stats.client_sets_not_in_world;
		answer_refused(from, m.seq, SetResult::NotAllowed);
		return;
	}
	bool owner = false;
	if (m.scope.kind == ScopeId::Player)
	{
		std::string d;
		owner = peer_digest(from, d) && d == m.scope.player_key;
	}
	else if (m.scope.kind == ScopeId::Object)
		owner = held_by(m.scope.arg, from);
	++g_stats.client_sets;
	const bool reserved = sn::host_key(m.scope, m.key); // F2 T14 fix 1 (Ruling F2-31)
	const std::string key = reserved ? m.key : std::string();
	const SetResult result = host_change(m.scope, m.key, std::move(m.value), from, owner, &m.seq);
	if (result == SetResult::Ok || result == SetResult::Same)
		return;
	++g_stats.client_refused;
	if (reserved)
	{
		++g_stats.host_keys_refused; // an honest client refuses it itself (l_set): a modified one
		if (log_allowed(g_stats.host_keys_refused))
			::Msg("! [coop] coop.sync: #%u wrote '%s' of his player scope, a key of the host's (coop.*): refused", from,
			    key.c_str());
	}
}

void on_set(PeerId from, Reader& r)
{
	follow_role();
	if (mgr().is_host())
	{
		host_on_set(from, r);
		return;
	}
	if (!client() || from != kHostPeer)
		return;
	sn::HostSet m;
	if (!sn::read_host_set(r, m))
	{
		bad_message(from, "SyncSet");
		return;
	}
	std::vector<sn::Change> changes;
	g_core.apply(m, changes);
	queue_changes(changes);
}

void on_full(PeerId from, Reader& r)
{
	follow_role();
	if (!client() || from != kHostPeer)
		return;
	if (!g_core.apply_full(r))
		bad_message(from, "SyncFull");
}

void on_request(PeerId from, Reader& r)
{
	follow_role();
	if (!mgr().is_host())
		return;
	Reader peek = r;
	const u32 id = peek.u32();
	sn::Request q;
	if (!sn::read_request(r, q))
	{
		bad_message(from, "SyncRequest");
		++g_stats.bad_requests;
		if (peek.ok())
			send_result(from, id, false, ss::make_string("bad request"));
		return;
	}
	++g_stats.host_requests;
	const char* refusal = nullptr;
	if (!in_world(from))
	{
		refusal = "not in the world";
		++g_stats.not_in_world;
	}
	else if (g_handlers.find(q.name) == g_handlers.end())
	{
		refusal = "no handler"; // before the limits: a name nobody handles makes no bucket
		++g_stats.no_handler;
	}
	else if (!g_limits.allow(from, q.name, mgr().now_ms()))
	{
		refusal = "rate limited";
		++g_stats.rate_limited;
	}
	else if (g_host_calls.size() >= kMaxHostCalls)
	{
		refusal = "busy";
		++g_stats.busy;
	}
	if (refusal)
	{
		send_result(from, q.id, false, ss::make_string(refusal));
		return;
	}
	HostCall h;
	h.from = from;
	h.id = q.id;
	h.name = std::move(q.name);
	h.args = std::move(q.args);
	g_host_calls.push_back(std::move(h));
}

// A player: the answer to his request (F3a T7: the host player's over the internal channel).
void on_result(PeerId from, Reader& r)
{
	follow_role();
	if (!(client() || mgr().is_host()) || from != kHostPeer)
		return;
	sn::Result m;
	if (!sn::read_result(r, m))
	{
		bad_message(from, "SyncResult");
		return;
	}
	Waiting w;
	if (!g_requests.take(m.id, w))
	{
		++g_stats.answers_unknown; // timed out, or from before a reset
		return;
	}
	++g_stats.requests_answered;
	Call c;
	c.kind = Call::Answer;
	c.fn = w;
	c.ok = m.ok;
	c.value = std::move(m.value);
	queue_call(std::move(c));
}

void on_cast(PeerId from, Reader& r)
{
	follow_role();
	if (!client() || from != kHostPeer)
		return;
	sn::Cast m;
	if (!sn::read_cast(r, m))
	{
		bad_message(from, "SyncCast");
		return;
	}
	++g_stats.casts_in;
	if (g_effects.find(m.name) == g_effects.end())
		return;
	Call c;
	c.kind = Call::Effect;
	c.name = std::move(m.name);
	c.value = std::move(m.args);
	c.from = kHostPeer;
	queue_call(std::move(c));
}

// --- Lua calls from frame() -------------------------------------------------------------------------

bool same_scope(const sn::ScopeName& a, const sn::ScopeName& b)
{
	return a.kind == b.kind && a.peer == b.peer && a.id == b.id;
}

bool watch_matches(const Watch& w, const sn::Change& ch, u16& local_id)
{
	if (ch.key.compare(0, w.prefix.size(), w.prefix) != 0)
		return false;
	switch (w.scope.kind)
	{
	case sn::ScopeName::World: return ch.scope.kind == ScopeId::World;
	case sn::ScopeName::Session: return ch.scope.kind == ScopeId::Session;
	case sn::ScopeName::Player:
	case sn::ScopeName::PlayerOf:
	{
		std::string d;
		return ch.scope.kind == ScopeId::Player &&
		    peer_digest(w.scope.kind == sn::ScopeName::Player ? self_peer() : w.scope.peer, d) && d == ch.scope.player_key;
	}
	case sn::ScopeName::Object: return ch.scope.kind == ScopeId::Object && ch.scope.arg == host_id_of(w.scope.id);
	case sn::ScopeName::AnyObject:
		if (ch.scope.kind != ScopeId::Object)
			return false;
		local_id = local_id_of(ch.scope.arg);
		return true;
	}
	return false;
}

bool watching(int ref)
{
	for (const Watch& w : g_watches)
		if (w.ref == ref)
			return true;
	return false;
}

void deliver_change(const sn::Change& ch)
{
	// A handler may add or remove handlers: the matching ones are taken first, each checked again before its call.
	std::vector<std::pair<int, u16>> calls;
	for (const Watch& w : g_watches)
	{
		u16 id = kNone;
		if (watch_matches(w, ch, id))
			calls.emplace_back(w.ref, w.scope.kind == sn::ScopeName::AnyObject ? id : kNone);
	}
	for (const auto& c : calls)
	{
		if (!watching(c.first))
			continue;
		pcall_ref(c.first, "on_change", ch.key.c_str(),
		    [&](lua_State* L) {
			    lua_pushlstring(L, ch.key.data(), ch.key.size());
			    push_value(L, ch.value);
			    push_value(L, ch.old);
			    lua_pushnumber(L, lua_Number(ch.by));
			    if (c.second == kNone)
				    return 4;
			    lua_pushnumber(L, lua_Number(c.second));
			    return 5;
		    },
		    0, [](lua_State*) {});
	}
}

void deliver_answer(Call& c)
{
	if (c.fn.ref == LUA_NOREF)
		return; // a request without a callback
	if (c.fn.gen != g_lua_gen)
	{
		++g_stats.calls_stale; // made in a Lua state that is gone
		return;
	}
	pcall_ref(c.fn.ref, "request callback", "", [&](lua_State* L) {
		lua_pushboolean(L, c.ok ? 1 : 0);
		push_value(L, c.value);
		return 2;
	}, 0, [](lua_State*) {});
	unref(c.fn.ref);
}

void deliver_effect(const Call& c)
{
	const auto it = g_effects.find(c.name);
	if (it == g_effects.end())
		return;
	const std::vector<int> refs = it->second;
	for (int ref : refs)
	{
		const auto now = g_effects.find(c.name);
		if (now == g_effects.end() || std::find(now->second.begin(), now->second.end(), ref) == now->second.end())
			continue;
		++g_stats.effects;
		pcall_ref(ref, "effect", c.name.c_str(), [&](lua_State* L) {
			push_value(L, c.value);
			lua_pushnumber(L, lua_Number(c.from));
			return 2;
		}, 0, [](lua_State*) {});
	}
}

void deliver_calls()
{
	u32 budget = kCallsPerFrame;
	while (!g_calls.empty() && budget-- > 0)
	{
		Call c = std::move(g_calls.front());
		g_calls.pop_front();
		++g_stats.calls;
		switch (c.kind)
		{
		case Call::Changed: deliver_change(c.change); break;
		case Call::Answer: deliver_answer(c); break;
		case Call::Effect: deliver_effect(c); break;
		}
	}
}

// Host: requests run their handlers (the players', the host player's too; single player: its own), answers go back.
void run_host_calls()
{
	u32 budget = kHostCallsPerFrame;
	while (!g_host_calls.empty() && budget-- > 0)
	{
		HostCall h = std::move(g_host_calls.front());
		g_host_calls.pop_front();
		bool ok = false;
		Value result;
		const auto it = g_handlers.find(h.name);
		if (it == g_handlers.end())
		{
			result = ss::make_string("no handler");
			++g_stats.no_handler;
		}
		else
		{
			std::string why;
			bool read = true;
			const bool ran = pcall_ref(it->second, "handler", h.name.c_str(), [&](lua_State* L) {
				lua_pushnumber(L, lua_Number(h.from));
				push_value(L, h.args);
				return 2;
			}, 2, [&](lua_State* L) {
				ok = lua_toboolean(L, -2) != 0;
				read = to_value(L, -1, result, why);
			});
			if (!ran)
			{
				ok = false;
				result = ss::make_string("handler error");
				++g_stats.handler_errors;
			}
			else if (!read)
			{
				ok = false;
				result = ss::make_string("bad result: " + why);
				++g_stats.bad_results;
				if (log_allowed(g_stats.bad_results))
					::Msg("! [coop] coop.sync handler '%s' returned a result that cannot travel: %s", h.name.c_str(),
					    why.c_str());
			}
		}
		if (h.here)
		{
			Call c;
			c.kind = Call::Answer;
			c.fn = h.fn;
			c.ok = ok;
			c.value = std::move(result);
			queue_call(std::move(c));
		}
		else
			send_result(h.from, h.id, ok, std::move(result)); // F3a T7: the host player's over the link
	}
}

// --- Lua API ----------------------------------------------------------------------------------------

int fail(lua_State* L, const char* why)
{
	lua_pushboolean(L, 0);
	lua_pushstring(L, why);
	return 2;
}

int fail(lua_State* L, const std::string& why) { return fail(L, why.c_str()); }

int done(lua_State* L)
{
	lua_pushboolean(L, 1);
	return 1;
}

bool arg_text(lua_State* L, int idx, std::string& out, size_t max)
{
	if (lua_type(L, idx) != LUA_TSTRING)
		return false;
	size_t n = 0;
	const char* s = lua_tolstring(L, idx, &n);
	if (n > max)
		return false;
	out.assign(s, n);
	return true;
}

bool arg_scope(lua_State* L, int idx, sn::ScopeName& out)
{
	std::string s;
	return arg_text(L, idx, s, kMaxScopeText) && sn::parse_scope(s, out);
}

bool arg_key(lua_State* L, int idx, std::string& out) { return arg_text(L, idx, out, ss::kMaxKey) && ss::key_ok(out); }

// A key prefix: a string of at most kMaxKey bytes, nil for "".
bool arg_prefix(lua_State* L, int idx, std::string& out)
{
	if (lua_isnoneornil(L, idx))
	{
		out.clear();
		return true;
	}
	return arg_text(L, idx, out, ss::kMaxKey);
}

bool arg_name(lua_State* L, int idx, std::string& out) { return arg_text(L, idx, out, sn::kMaxName) && sn::name_ok(out); }

bool arg_number(lua_State* L, int idx, double& out)
{
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	out = double(lua_tonumber(L, idx));
	return std::isfinite(out);
}

bool arg_whole(lua_State* L, int idx, double lo, double hi, double& out)
{
	return arg_number(L, idx, out) && out == std::floor(out) && out >= lo && out <= hi;
}

// The Lua function at idx as a registry reference of this state; LUA_NOREF for nil.
bool arg_function(lua_State* L, int idx, int& ref)
{
	ref = LUA_NOREF;
	if (lua_isnoneornil(L, idx))
		return true;
	if (lua_type(L, idx) != LUA_TFUNCTION)
		return false;
	lua_pushvalue(L, idx);
	ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return true;
}

bool same_function(lua_State* L, int ref, int idx)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	const bool same = lua_rawequal(L, -1, idx) != 0;
	lua_pop(L, 1);
	return same;
}

// coop.sync.set(scope, key, value) -> true | false, why
int l_set(lua_State* L)
{
	follow_role();
	sn::ScopeName n;
	ScopeId scope;
	std::string key, why;
	const char* bad = nullptr;
	Value v;
	if (!arg_scope(L, 1, n))
		return fail(L, "bad scope");
	if (!resolve(n, scope, bad))
		return fail(L, bad);
	if (!arg_key(L, 2, key))
		return fail(L, "bad key (1..128 bytes)");
	if (!to_value(L, 3, v, why))
	{
		++g_stats.lua_refused;
		return fail(L, why);
	}
	if (!client())
	{
		if (scope.kind == ScopeId::Object && !events::find_entity(u16(scope.arg)))
			return fail(L, "no such object");
		const SetResult r = host_change(scope, key, std::move(v), kHostPeer, false, nullptr);
		if (r != SetResult::Ok && r != SetResult::Same)
		{
			++g_stats.lua_refused;
			return fail(L, result_why(r));
		}
		++g_stats.lua_sets;
		return done(L);
	}
	// F2 T14 fix 1 (Ruling F2-31): keys coop.* of a player scope are the host's.
	if (sn::host_key(scope, key))
	{
		++g_stats.lua_refused;
		return fail(L, "not allowed: coop.* keys of a player scope are the host's");
	}
	// Client: his own player scope, or an object he holds.
	const bool owner = (scope.kind == ScopeId::Player) ||
	    (scope.kind == ScopeId::Object && events::find_entity(n.id) && events::root_of(n.id) == 0);
	if (!ss::may_set(scope.kind, false, owner))
	{
		++g_stats.lua_refused;
		return fail(L, scope.kind == ScopeId::Object && !events::find_entity(n.id) ? "no such object" : "not allowed");
	}
	if (!g_core.view().pending(scope, key))
	{
		const ss::Value* has = g_core.store().get(scope, key);
		ss::Value cmp = v;
		ss::canonicalize(cmp);
		if (has ? ss::same(*has, cmp) : cmp.type == ValueType::Nil)
			return done(L); // nothing to change
	}
	if (g_core.full() || g_out.size() >= kMaxOutbox)
	{
		++g_stats.lua_refused;
		return fail(L, "too many changes waiting for the host");
	}
	sn::ClientSet m;
	m.scope = scope;
	m.key = key;
	m.value = v;
	std::vector<sn::Change> changes;
	m.seq = g_core.local_set(scope, key, std::move(v), self_peer(), mgr().now_ms(), changes);
	queue_changes(changes);
	Writer w(64 + m.key.size() + ss::encoded_size(m.value));
	sn::write_client_set(w, m);
	out_to(kHostPeer, Msg::SyncSet, std::move(w));
	++g_stats.lua_sets;
	++g_stats.sets_out;
	return done(L);
}

// coop.sync.get(scope, key) -> value | nil
int l_get(lua_State* L)
{
	follow_role();
	sn::ScopeName n;
	ScopeId scope;
	std::string key;
	const char* why = nullptr;
	if (!arg_scope(L, 1, n) || !resolve(n, scope, why) || !arg_key(L, 2, key))
	{
		lua_pushnil(L);
		return 1;
	}
	push_value(L, g_core.shown(scope, key));
	return 1;
}

// coop.sync.keys(scope, prefix) -> {key = value} | nil, why
int l_keys(lua_State* L)
{
	follow_role();
	sn::ScopeName n;
	ScopeId scope;
	std::string prefix;
	const char* why = nullptr;
	if (!arg_scope(L, 1, n) || !arg_prefix(L, 2, prefix))
	{
		lua_pushnil(L);
		lua_pushstring(L, "bad arguments");
		return 2;
	}
	if (!resolve(n, scope, why))
	{
		lua_pushnil(L);
		lua_pushstring(L, why);
		return 2;
	}
	const std::vector<std::pair<std::string, Value>> kv = g_core.keys(scope, prefix);
	lua_createtable(L, 0, int(kv.size()));
	for (const auto& e : kv)
	{
		lua_pushlstring(L, e.first.data(), e.first.size());
		push_value(L, e.second);
		lua_rawset(L, -3);
	}
	return 1;
}

// coop.sync.on_change(scope, prefix, fn | nil) -> true | false, why
int l_on_change(lua_State* L)
{
	follow_role();
	sn::ScopeName n;
	std::string prefix;
	if (!arg_scope(L, 1, n) || !arg_prefix(L, 2, prefix))
		return fail(L, "bad arguments");
	if (lua_isnoneornil(L, 3))
	{
		for (auto it = g_watches.begin(); it != g_watches.end();)
		{
			if (same_scope(it->scope, n) && it->prefix == prefix)
			{
				unref(it->ref);
				it = g_watches.erase(it);
			}
			else
				++it;
		}
		return done(L);
	}
	if (lua_type(L, 3) != LUA_TFUNCTION)
		return fail(L, "bad arguments");
	for (const Watch& w : g_watches)
		if (same_scope(w.scope, n) && w.prefix == prefix && same_function(L, w.ref, 3))
			return done(L); // already
	if (g_watches.size() >= kMaxWatches)
		return fail(L, "too many on_change handlers");
	Watch w;
	w.scope = n;
	w.prefix = prefix;
	arg_function(L, 3, w.ref);
	g_watches.push_back(std::move(w));
	return done(L);
}

// coop.sync.handler(name, fn | nil) -> true | false, why            host
int l_handler(lua_State* L)
{
	follow_role();
	std::string name;
	if (!arg_name(L, 1, name) || !(lua_isnoneornil(L, 2) || lua_type(L, 2) == LUA_TFUNCTION))
		return fail(L, "bad arguments");
	if (client())
		return fail(L, "host only");
	const auto it = g_handlers.find(name);
	if (lua_isnoneornil(L, 2))
	{
		if (it != g_handlers.end())
		{
			unref(it->second);
			g_handlers.erase(it);
		}
		return done(L);
	}
	if (it == g_handlers.end() && g_handlers.size() >= kMaxHandlers)
		return fail(L, "too many handlers");
	int ref = LUA_NOREF;
	arg_function(L, 2, ref);
	if (it != g_handlers.end())
	{
		unref(it->second);
		it->second = ref;
	}
	else
		g_handlers.emplace(std::move(name), ref);
	return done(L);
}

// coop.sync.request(name, args, fn(ok, result) | nil) -> true | false, why
int l_request(lua_State* L)
{
	follow_role();
	std::string name, why;
	Value args;
	if (!arg_name(L, 1, name))
		return fail(L, "bad name");
	if (!to_value(L, 2, args, why))
		return fail(L, why);
	if (!(lua_isnoneornil(L, 3) || lua_type(L, 3) == LUA_TFUNCTION))
		return fail(L, "bad arguments");
	// A player asks the world server: a client over the net, F3a T7 the host player over the internal channel (sent by
	// frame()); its handler, its limits and its answer are the same. Outside a session the machine runs its own handler.
	const bool player = client() || mgr().is_host();
	if (player)
	{
		if (g_requests.full())
			return fail(L, "too many requests waiting");
		if ((client() ? g_out.size() : g_player_out.size()) >= kMaxOutbox)
			return fail(L, "too many messages waiting for the host");
	}
	else if (g_host_calls.size() >= kMaxHostCalls)
		return fail(L, "busy");
	Waiting w;
	w.gen = g_lua_gen;
	arg_function(L, 3, w.ref);
	if (player)
	{
		sn::Request q;
		q.id = g_requests.add(mgr().now_ms(), w);
		q.name = std::move(name);
		q.args = std::move(args);
		Writer out(32 + q.name.size() + ss::encoded_size(q.args));
		sn::write_request(out, q);
		if (client())
			out_to(kHostPeer, Msg::SyncRequest, std::move(out));
		else
			g_player_out.push_back(std::move(out));
		++g_stats.requests_out;
		return done(L);
	}
	HostCall h;
	h.from = kHostPeer;
	h.here = true;
	h.name = std::move(name);
	h.args = std::move(args);
	h.fn = w;
	g_host_calls.push_back(std::move(h));
	pump_sync();
	return done(L);
}

// coop.sync.broadcast(name, args, {to = peer} | {pos = vector, radius = m} | nil) -> true, machines | false, why  host
int l_broadcast(lua_State* L)
{
	follow_role();
	std::string name, why;
	Value args;
	if (!arg_name(L, 1, name))
		return fail(L, "bad name");
	if (!to_value(L, 2, args, why))
		return fail(L, why);
	if (client())
		return fail(L, "host only");
	enum
	{
		All,
		To,
		Radius
	} mode = All;
	double to = 0, radius = 0;
	Fvector pos;
	pos.set(0.f, 0.f, 0.f);
	if (lua_type(L, 3) == LUA_TTABLE)
	{
		lua_pushstring(L, "to");
		lua_rawget(L, 3);
		const bool has_to = !lua_isnil(L, -1);
		const bool to_ok = has_to && arg_whole(L, -1, 0, double(kInvalidPeer - 1), to);
		lua_pop(L, 1);
		lua_pushstring(L, "pos");
		lua_rawget(L, 3);
		const Fvector* p = as_vector(L, -1);
		if (p)
			pos = *p;
		lua_pop(L, 1);
		lua_pushstring(L, "radius");
		lua_rawget(L, 3);
		const bool radius_ok = arg_number(L, -1, radius) && radius > 0 && radius <= kMaxRadius;
		lua_pop(L, 1);
		if (has_to)
		{
			if (!to_ok)
				return fail(L, "bad target");
			mode = To;
		}
		else if (p && radius_ok && validate::finite3(&pos.x))
			mode = Radius;
		else
			return fail(L, "bad target");
	}
	else if (!lua_isnoneornil(L, 3))
		return fail(L, "bad target");

	u32 machines = 0;
	bool here = false;
	sn::Cast m;
	m.name = name;
	m.args = args;
	Writer w(32 + m.name.size() + ss::encoded_size(m.args));
	sn::write_cast(w, m);
	const bool session = mgr().is_host();
	if (mode == To)
	{
		const PeerId peer = PeerId(to);
		if (peer == kHostPeer)
			here = true;
		else if (!session || !in_world(peer))
			return fail(L, "no such player");
		else
		{
			out_to(peer, Msg::SyncCast, std::move(w));
			++machines;
		}
	}
	else
	{
		Fvector at;
		here = mode == All || (position_of(0, at) && at.distance_to(pos) <= float(radius));
		if (session)
			for (const PeerInfo& p : mgr().session().peers())
			{
				if (p.id == kHostPeer || !in_world(p.id))
					continue;
				if (mode == Radius)
				{
					const u16 who = character_of(p.id);
					if (who == kNone || !position_of(who, at) || at.distance_to(pos) > float(radius))
						continue;
				}
				out_to(p.id, Msg::SyncCast, Writer(w));
				++machines;
			}
	}
	if (here)
	{
		++machines;
		if (g_effects.find(name) != g_effects.end())
		{
			Call c;
			c.kind = Call::Effect;
			c.name = name;
			c.value = std::move(args);
			c.from = kHostPeer;
			queue_call(std::move(c));
		}
	}
	++g_stats.casts;
	g_stats.cast_machines += machines;
	lua_pushboolean(L, 1);
	lua_pushnumber(L, lua_Number(machines));
	return 2;
}

// coop.sync.on(name, fn | nil) -> true | false, why
int l_on(lua_State* L)
{
	follow_role();
	std::string name;
	if (!arg_name(L, 1, name) || !(lua_isnoneornil(L, 2) || lua_type(L, 2) == LUA_TFUNCTION))
		return fail(L, "bad arguments");
	auto it = g_effects.find(name);
	if (lua_isnoneornil(L, 2))
	{
		if (it != g_effects.end())
		{
			for (int ref : it->second)
				unref(ref);
			g_effects.erase(it);
		}
		return done(L);
	}
	if (it != g_effects.end())
		for (int ref : it->second)
			if (same_function(L, ref, 2))
				return done(L); // already
	if (it == g_effects.end() && g_effects.size() >= kMaxEffectNames)
		return fail(L, "too many effect names");
	if (it != g_effects.end() && it->second.size() >= kMaxEffectFns)
		return fail(L, "too many handlers of this effect");
	int ref = LUA_NOREF;
	arg_function(L, 2, ref);
	g_effects[name].push_back(ref);
	return done(L);
}

// coop.sync.near(peer, obj_id, dist) -> bool
int l_near(lua_State* L)
{
	double peer = 0, id = 0, dist = 0;
	const bool ok = arg_whole(L, 1, 0, double(kInvalidPeer - 1), peer) && arg_whole(L, 2, 0, double(kNone - 1), id) &&
	    arg_number(L, 3, dist) && dist >= 0;
	lua_pushboolean(L, ok && within(PeerId(peer), u16(id), float(dist)) ? 1 : 0);
	return 1;
}

// coop.sync.owns(peer, item_id) -> bool
int l_owns(lua_State* L)
{
	double peer = 0, id = 0;
	const bool ok = arg_whole(L, 1, 0, double(kInvalidPeer - 1), peer) && arg_whole(L, 2, 0, double(kNone - 1), id);
	lua_pushboolean(L, ok && owns(PeerId(peer), u16(id)) ? 1 : 0);
	return 1;
}

// coop.sync.in_container(peer, item_id, box_id) -> bool
int l_in_container(lua_State* L)
{
	double peer = 0, id = 0, box = 0;
	const bool ok = arg_whole(L, 1, 0, double(kInvalidPeer - 1), peer) && arg_whole(L, 2, 0, double(kNone - 1), id) &&
	    arg_whole(L, 3, 0, double(kNone - 1), box);
	lua_pushboolean(L, ok && in_container(PeerId(peer), u16(id), u16(box)) ? 1 : 0);
	return 1;
}

// coop.sync.limit(name, burst, per_sec) -> true | false, why        host
int l_limit(lua_State* L)
{
	follow_role();
	std::string name;
	double burst = 0, per_sec = 0;
	if (!arg_name(L, 1, name) || !arg_whole(L, 2, 1, sn::RequestLimits::kMaxBurst, burst) || !arg_number(L, 3, per_sec))
		return fail(L, "bad arguments");
	if (client())
		return fail(L, "host only");
	if (!g_limits.set_rule(name, u32(burst), per_sec))
		return fail(L, "bad arguments or too many limits");
	return done(L);
}

// The scopes a save keeps: everything but the session.
bool decode_saved(ss::Store& into, std::string_view data)
{
	Reader r(reinterpret_cast<const u8*>(data.data()), data.size());
	const u64 count = r.varu();
	if (!r.ok() || count > r.remaining() / sn::kMinScopeWire)
		return false;
	for (u64 i = 0; i < count; ++i)
	{
		ScopeId id;
		if (!into.decode_scope(r, id) || id.kind == ScopeId::Session)
			return false;
	}
	return r.remaining() == 0;
}

// coop.sync.export() -> {format = 1, scopes = n, data = string} | nil            host: m_data.coop_sync
int l_export(lua_State* L)
{
	follow_role();
	if (client())
	{
		lua_pushnil(L);
		return 1;
	}
	const ss::Store& store = g_core.store();
	Writer body;
	u64 count = 0;
	for (const ScopeId& s : store.scopes())
		if (s.kind != ScopeId::Session)
		{
			store.encode_scope(body, s);
			++count;
		}
	Writer w(body.size() + 10);
	w.varu(count);
	if (body.size())
		w.bytes(body.ptr(), body.size());
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, lua_Number(kExportFormat));
	lua_setfield(L, -2, "format");
	lua_pushnumber(L, lua_Number(count));
	lua_setfield(L, -2, "scopes");
	lua_pushlstring(L, reinterpret_cast<const char*>(w.ptr()), w.size());
	lua_setfield(L, -2, "data");
	++g_stats.exports;
	return 1;
}

// Host: the store becomes `data` (nullptr: empty); the session scope stays. Clients get a new copy (frame).
void replace_saved(const std::string_view* data)
{
	ss::Store& store = g_core.store();
	Writer session;
	const bool keep_session = store.bytes(ScopeId::session()) != 0;
	if (keep_session)
		store.encode_scope(session, ScopeId::session());
	store.clear();
	if (keep_session)
	{
		Reader r(session.ptr(), session.size());
		ScopeId id;
		store.decode_scope(r, id);
	}
	if (data)
		decode_saved(store, *data); // checked on a copy before: it cannot fail here
	g_resync = mgr().is_host();
}

// coop.sync.import(t | nil) -> true | false, why            host: m_data.coop_sync at load; nil: nothing saved
int l_import(lua_State* L)
{
	follow_role();
	if (client())
		return fail(L, "host only");
	if (lua_isnoneornil(L, 1))
	{
		replace_saved(nullptr);
		++g_stats.imports;
		return done(L);
	}
	if (lua_type(L, 1) != LUA_TTABLE)
		return fail(L, "bad data");
	lua_pushstring(L, "format");
	lua_rawget(L, 1);
	double format = 0;
	const bool format_ok = arg_number(L, -1, format) && format == double(kExportFormat);
	lua_pop(L, 1);
	lua_pushstring(L, "data");
	lua_rawget(L, 1);
	std::string data;
	const bool data_ok = lua_type(L, -1) == LUA_TSTRING;
	if (data_ok)
	{
		size_t n = 0;
		const char* s = lua_tolstring(L, -1, &n);
		data.assign(s, n);
	}
	lua_pop(L, 1);
	ss::Store check;
	if (!format_ok || !data_ok || !decode_saved(check, data))
	{
		++g_stats.imports_bad;
		::Msg("! [coop] coop.sync: the saved data cannot be read (format %g): nothing imported", format);
		return fail(L, "bad data");
	}
	const std::string_view view(data);
	replace_saved(&view);
	++g_stats.imports;
	return done(L);
}

// coop.saving_for_join() -> bool (F2 T14 fix 1, review I1)
int l_saving_for_join(lua_State* L)
{
	lua_pushboolean(L, g_join_saves != 0 ? 1 : 0);
	return 1;
}

// A new Lua state: every registration of the old one goes (its references died with it).
void forget_lua()
{
	++g_lua_gen;
	g_watches.clear();
	g_handlers.clear();
	g_effects.clear();
	g_limits.clear();
	for (auto it = g_calls.begin(); it != g_calls.end();)
		it = it->kind == Call::Answer && it->fn.gen != g_lua_gen ? g_calls.erase(it) : std::next(it);
	for (HostCall& h : g_host_calls)
		h.fn = Waiting(); // a host request of the old state: its answer has nobody to go to
}
} // namespace

void install_server(Session& s)
{
	s.on(Msg::SyncSet, &on_set);
	s.on(Msg::SyncRequest, &on_request);
}

void install_client(Endpoint& e)
{
	e.on(Msg::SyncSet, &on_set);
	e.on(Msg::SyncFull, &on_full);
	e.on(Msg::SyncResult, &on_result);
	e.on(Msg::SyncCast, &on_cast);
}

void reset()
{
	follow_role();
	const bool was_client = client();
	g_out.clear();
	g_player_out.clear();
	g_pacer.reset();
	g_resync = false;
	g_was_live = false;
	g_limits.clear_buckets();
	g_peer_digest.clear();
	end_requests("session ended");
	for (auto it = g_host_calls.begin(); it != g_host_calls.end();)
		it = !it->here ? g_host_calls.erase(it) : std::next(it); // their players are gone (the host player too)
	if (was_client)
	{
		g_core.clear(); // nothing of the host's stays
		forget_world_calls();
	}
	else
		g_core.store().drop(ScopeId::session()); // the game's data stays, the session's goes
	pump_sync();
}

void frame()
{
	follow_role();
	const u64 now = mgr().now_ms();
	if (client())
	{
		const bool live = mgr().is_live_client();
		if (live && !g_was_live)
		{
			// Requests and changes made while loading the world leave now: they wait for their answers from now on.
			g_requests.touch_all(now);
			g_core.touch_all(now);
		}
		g_was_live = live;
		client_flush(now);
		if (live)
		{
			for (auto& w : g_requests.expire(now))
			{
				Call c;
				c.kind = Call::Answer;
				c.fn = w.second;
				c.ok = false;
				c.value = ss::make_string("timeout");
				queue_call(std::move(c));
				++g_stats.requests_timed_out;
			}
			std::vector<sn::Change> changes;
			g_core.expire(now, changes);
			queue_changes(changes);
		}
	}
	else
	{
		host_flush();
		if (g_resync && mgr().is_host())
			resync_all();
		g_resync = false;
		if (mgr().is_host())
			host_player_frame(now);
	}
	if (lua_ready())
	{
		if (!client())
			run_host_calls();
		deliver_calls();
	}
	if (!client())
		host_flush(); // answers of this frame's handlers
	pump_sync();
}

void print_status()
{
	follow_role();
	const ss::Store& store = g_core.store();
	u64 keys[ss::kKindCount] = {}, bytes[ss::kKindCount] = {}, scopes[ss::kKindCount] = {};
	for (const ScopeId& s : store.scopes())
	{
		const size_t k = size_t(s.kind);
		++scopes[k];
		keys[k] += store.keys(s, "").size();
		bytes[k] += store.bytes(s);
	}
	::Msg("[coop-sync] store (%s): world %llu keys %llu B | session %llu keys %llu B | players %llu scopes %llu keys %llu B | "
	      "objects %llu scopes %llu keys %llu B | pool %u B, newest version %llu",
	    client() ? "the host's copy" : "ours", keys[ScopeId::World], bytes[ScopeId::World], keys[ScopeId::Session],
	    bytes[ScopeId::Session], scopes[ScopeId::Player], keys[ScopeId::Player], bytes[ScopeId::Player],
	    scopes[ScopeId::Object], keys[ScopeId::Object], bytes[ScopeId::Object], u32(store.pool_bytes()), store.top_version());
	const ss::Stats& st = store.stats();
	::Msg("[coop-sync] changes: ok %llu, same %llu | refused: bad key %llu, bad value %llu, too big %llu, full %llu "
	      "(scope %llu, pool %llu, count %llu), not allowed %llu, stale %llu | freed records evicted %llu",
	    st.results[size_t(SetResult::Ok)], st.results[size_t(SetResult::Same)], st.results[size_t(SetResult::BadKey)],
	    st.results[size_t(SetResult::BadValue)], st.results[size_t(SetResult::TooBig)],
	    st.results[size_t(SetResult::ScopeFull)], st.full_scope, st.full_pool, st.full_count,
	    st.results[size_t(SetResult::NotAllowed)], st.results[size_t(SetResult::Stale)], st.freed_evicted);
	const Stats& s = g_stats;
	::Msg("[coop-sync] lua: sets %llu, refused %llu | objects dropped with them %llu | export %llu, import %llu (bad %llu)",
	    s.lua_sets, s.lua_refused, s.destroyed, s.exports, s.imports, s.imports_bad);
	if (client())
	{
		const sn::ClientCore::Stats& c = g_core.stats();
		::Msg("[coop-sync] client: changes sent %llu, waiting for an answer %u (shown %u), outbox %u, paced frames %llu | "
		      "answers %llu: refused %llu, lost %llu, given up %llu | host changes %llu (stale %llu), objects dropped %llu, "
		      "copy parts %llu (bad %llu)",
		    s.sets_out, u32(g_core.waiting()), u32(g_core.view().shown()), u32(g_out.size()), s.held, c.acks, c.refused,
		    c.lost, c.expired, c.applied, c.stale, c.dropped, c.full_parts, c.full_bad);
		::Msg("[coop-sync] requests: sent %llu, answered %llu, timed out %llu, ended %llu, waiting %u, unknown answers %llu",
		    s.requests_out, s.requests_answered, s.requests_timed_out, s.requests_ended, u32(g_requests.size()),
		    s.answers_unknown);
	}
	else
	{
		::Msg("[coop-sync] host: client changes %llu (not in the world %llu, refused %llu, of them the host's keys %llu) | "
		      "copies %llu (%llu B, after an import %llu) | outbox %u",
		    s.client_sets, s.client_sets_not_in_world, s.client_refused, s.host_keys_refused, s.copies, s.copy_bytes,
		    s.resyncs, u32(g_out.size()));
		::Msg("[coop-sync] requests: taken %llu, refused: no handler %llu, rate limited %llu, busy %llu, not in the world "
		      "%llu, malformed %llu | handler errors %llu, bad results %llu, answers sent %llu, waiting %u",
		    s.host_requests, s.no_handler, s.rate_limited, s.busy, s.not_in_world, s.bad_requests, s.handler_errors,
		    s.bad_results, s.results_out, u32(g_host_calls.size()));
		::Msg("[coop-sync] requests of the host player (over the internal channel): sent %llu, answered %llu, timed out "
		      "%llu, ended %llu, waiting %u, unknown answers %llu",
		    s.requests_out, s.requests_answered, s.requests_timed_out, s.requests_ended, u32(g_requests.size()),
		    s.answers_unknown);
	}
	::Msg("[coop-sync] effects: broadcasts %llu (to %llu machines), received %llu, handler calls %llu | lua calls %llu, "
	      "errors %llu, dropped %llu, stale %llu, waiting %u | on_change %u, handlers %u, effect names %u, limits %u | "
	      "malformed messages %llu",
	    s.casts, s.cast_machines, s.casts_in, s.effects, s.calls, s.lua_errors, s.calls_dropped, s.calls_stale,
	    u32(g_calls.size()), u32(g_watches.size()), u32(g_handlers.size()), u32(g_effects.size()), u32(g_limits.rules()),
	    s.bad_messages);
}

void host_send_full(PeerId peer)
{
	follow_role();
	if (!mgr().is_host() || peer == kHostPeer)
		return;
	// What changed before the copy goes out first: to a player still loading an older world it is queued with his world
	// changes, which his join drops right after this (the copy holds it all).
	host_flush();
	const std::vector<Writer> parts = full_copy_for(peer);
	size_t total = 0;
	for (const Writer& part : parts)
	{
		mgr().session().send(peer, Lane::Reliable, Msg::SyncFull, part);
		total += part.size();
	}
	count_copy(parts);
	::Msg("[coop] coop.sync: copy for #%u, %u part(s), %u B", peer, u32(parts.size()), u32(total));
}

void on_entity_destroyed(u16 id)
{
	if (client() || id == kNone)
		return;
	const ScopeId scope = ScopeId::object(id);
	if (!g_watches.empty())
		for (const auto& kv : g_core.store().keys(scope, ""))
		{
			sn::Change ch;
			ch.scope = scope;
			ch.key = kv.first;
			ch.old = *kv.second;
			queue_change(std::move(ch));
		}
	std::vector<sn::Delivery> out;
	if (sn::host_drop(g_core.store(), scope, out))
		++g_stats.destroyed;
	queue_deliveries(out);
}

SavingForJoin::SavingForJoin() { ++g_join_saves; }
SavingForJoin::~SavingForJoin() { --g_join_saves; }
bool saving_for_join() { return g_join_saves != 0; }

void on_peer_left(PeerId id)
{
	g_limits.forget_peer(id);
	g_peer_digest.erase(id);
}

void register_lua(lua_State* L)
{
	forget_lua();
	static const luaL_Reg fns[] = {
		{"set", &l_set},
		{"get", &l_get},
		{"keys", &l_keys},
		{"on_change", &l_on_change},
		{"handler", &l_handler},
		{"request", &l_request},
		{"broadcast", &l_broadcast},
		{"on", &l_on},
		{"near", &l_near},
		{"owns", &l_owns},
		{"in_container", &l_in_container},
		{"limit", &l_limit},
		{"export", &l_export},
		{"import", &l_import},
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	lua_createtable(L, 0, int(sizeof(fns) / sizeof(fns[0])));
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_setfield(L, -2, "sync");
	lua_pushcfunction(L, &l_saving_for_join); // fix 1: coop.saving_for_join() (scripts\coop_sync.script)
	lua_setfield(L, -2, "saving_for_join");
	lua_pop(L, 1);
}

std::optional<std::string> player_get(u16 character, std::string_view key)
{
	follow_role();
	ScopeId scope;
	if (client() || !character_scope(character, scope))
		return std::nullopt;
	const Value* v = g_core.store().get(scope, key);
	if (!v || v->type != ValueType::String)
		return std::nullopt;
	return v->str;
}

bool player_set(u16 character, std::string_view key, std::string_view value)
{
	follow_role();
	ScopeId scope;
	if (client() || !character_scope(character, scope))
		return false;
	const SetResult r = host_change(scope, key, ss::make_string(std::string(value)), kHostPeer, false, nullptr);
	return r == SetResult::Ok || r == SetResult::Same;
}
} // namespace syncapi
} // namespace coop
