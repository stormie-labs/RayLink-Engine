// Anomaly Coop — creature sounds across machines (W5 C2). See coop_sound.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_sound.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_remote.h"
#include "coop_players.h"

#include "../xrEngine/XR_IOConsole.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "../xrEngine/GameMtlLib.h"
#include "../Include/xrRender/Kinematics.h"
#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "material_manager.h"
#include "actor_defs.h"
#include "ai_space.h"
#include "script_engine.h"

namespace coop
{
namespace sound
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr size_t kMaxOutQueue = 256; // host: sounds recorded between two frames
constexpr u64 kDedupMs = 150; // host: same object + same file again within this: dropped
constexpr float kRatePerSec = 30.f; // host: per client
constexpr float kBurst = 40.f;
constexpr u8 kMaxPerPacket = 24;
constexpr float kMinRadius = 20.f;
constexpr float kMaxRadius = 300.f;
constexpr size_t kMaxPlaying = 48; // client
constexpr u32 kMaxPerObject = 4;
constexpr size_t kMaxExistCache = 2048;
constexpr size_t kMaxLuaQueue = 64;
constexpr u32 kLuaPerFrame = 32;
constexpr u64 kLuaCheckMs = 2000; // how long "is the callback registered" is trusted
constexpr u64 kGroundCacheMs = 400;
constexpr float kGroundCacheMove = 0.3f;
constexpr float kGroundRay = 2.f;
constexpr u32 kLogBurst = 20;

int g_npc_sounds = 1; // console coop_npc_sounds 0/1
float g_sound_dist = 100.f; // console coop_npc_sound_dist: hearing range of a sound that does not tell its own

// ---------------------------------------------------------------- host state
struct Outgoing
{
	NpcSound s;
	Fvector pos{};
	float radius = 100.f;
};
xrCriticalSection g_out_lock; // the hooks may run on the game thread
xr_vector<Outgoing> g_out;
struct Recent
{
	u16 id = kNoId;
	u32 hash = 0;
	u64 t = 0;
};
Recent g_recent[64];
u32 g_recent_next = 0;
bool g_have_clients = false; // at least one client in the world (frame() keeps it current)

struct Bucket
{
	float tokens = kBurst;
	u64 last = 0;
};
xr_map<PeerId, Bucket> g_buckets;

// ---------------------------------------------------------------- client state
struct Playing
{
	u16 id = kNoId; // local numbering
	ref_sound snd;
	u16 bone = BI_NONE;
	u32 mask = 0;
	Fvector offset{};
	float volume = 1.f;
	u64 start_at = 0;
	bool started = false;
	bool follow = false;
};
// Heap: ref_sounds must be released in reset(), never by static teardown after the sound device is gone.
xr_vector<Playing>* g_playing = nullptr;
xr_map<xr_string, bool>* g_exists = nullptr;

// ---------------------------------------------------------------- Lua callbacks
enum class LuaKind : u8
{
	AvatarStep,
	NpcSound,
};
struct LuaEvent
{
	LuaKind kind = LuaKind::AvatarStep;
	u16 id = kNoId;
	xr_string a; // material / path
	xr_string b; // move state
	float power = 0.f;
};
xr_vector<LuaEvent> g_lua;
struct Registered
{
	bool yes = false;
	u64 checked = 0;
	bool valid = false;
};
Registered g_reg_step, g_reg_snd;

// ---------------------------------------------------------------- footsteps
struct Ground
{
	Fvector pos{};
	u64 t = 0;
	u16 mtl = GAMEMTL_NONE_IDX;
};
xr_map<u16, Ground> g_ground;

struct Stats
{
	// host
	u64 recorded = 0, deduped = 0, queue_full = 0, sent = 0, packets = 0, too_far = 0, rate_dropped = 0;
	// client
	u64 received = 0, played = 0, malformed = 0, no_object = 0, missing_files = 0, cut = 0, muted_local = 0;
	// footsteps and Lua
	u64 ground_picks = 0, avatar_steps = 0, avatar_jumps = 0, lua_fired = 0, lua_unregistered = 0, lua_errors = 0;
} g_st;
u32 g_log_count = 0;
bool log_allowed() { return ++g_log_count <= kLogBurst || (g_log_count % 100) == 0; }

CGameObject* object_here(u16 local_id)
{
	if (!g_pGameLevel || local_id == kNoId)
		return nullptr;
	CGameObject* o = smart_cast<CGameObject*>(Level().Objects.net_Find(local_id));
	return o && !o->getDestroy() ? o : nullptr;
}

u32 fnv(const char* s, u16 id)
{
	u32 h = 2166136261u ^ id;
	for (; *s; ++s)
	{
		h ^= u8(*s);
		h *= 16777619u;
	}
	return h;
}

// ---------------------------------------------------------------- host: recording

// A creature whose sounds the clients get: NPCs, monsters and traders, never a player character.
CGameObject* relay_source(CObject* owner)
{
	CGameObject* go = owner ? smart_cast<CGameObject*>(owner) : nullptr;
	if (!go || go->ID() == 0 || go->getDestroy())
		return nullptr;
	if (!remote::is_puppet_type(go) || world::is_player_proxy(go->ID()))
		return nullptr;
	return go;
}

bool host_recording() { return g_npc_sounds && g_have_clients && g_pGameLevel && mgr().is_host(); }

float radius_of(ref_sound& snd)
{
	if (const CSound_params* p = snd.get_params())
		if (_valid(p->max_distance) && p->max_distance > 0.f)
			return clampr(p->max_distance, kMinRadius, kMaxRadius);
	return clampr(g_sound_dist, kMinRadius, kMaxRadius);
}

// Mono file name of a sound, nullptr for stereo (always 2D in this engine) or nameless sounds.
LPCSTR relay_path(const ref_sound& snd)
{
	const CSound_source* src = snd._handle();
	if (!src || src->channels_num() != 1)
		return nullptr;
	LPCSTR name = src->file_name();
	return name && name[0] ? name : nullptr;
}

void record(CGameObject* go, NpcSound& s, float radius)
{
	if (!sound_path_valid(s.path))
		return; // level sounds or odd names: the client could not open them anyway
	const u32 hash = fnv(s.path.c_str(), s.object);
	const u64 now = mgr().now_ms();
	xrCriticalSectionGuard guard(&g_out_lock);
	for (const Recent& r : g_recent)
		if (r.id == s.object && r.hash == hash && now - r.t < kDedupMs)
		{
			++g_st.deduped;
			return;
		}
	if (g_out.size() >= kMaxOutQueue)
	{
		++g_st.queue_full;
		return;
	}
	g_recent[g_recent_next++ & 63] = {s.object, hash, now};
	Outgoing o;
	o.s = std::move(s);
	o.pos = go->Position();
	o.radius = radius;
	g_out.push_back(std::move(o));
	++g_st.recorded;
}

// ---------------------------------------------------------------- host: sending

void host_frame()
{
	g_have_clients = false;
	struct Target
	{
		PeerId id;
		Fvector pos;
	};
	xr_vector<Target> targets;
	if (g_pGameLevel)
	{
		for (const PeerInfo& p : mgr().session().peers())
		{
			if (p.state != PeerState::Joined || sync::host_peer_joining(p.id))
				continue;
			const u16 proxy = join::proxy_of_peer(p.id);
			CObject* o = proxy != kNoId ? Level().Objects.net_Find(proxy) : nullptr;
			if (o)
				targets.push_back({p.id, o->Position()});
		}
	}
	g_have_clients = !targets.empty() && g_npc_sounds;
	xr_vector<Outgoing> out;
	{
		xrCriticalSectionGuard guard(&g_out_lock);
		out.swap(g_out);
	}
	if (out.empty() || targets.empty())
		return;
	const u64 now = mgr().now_ms();
	for (const Target& t : targets)
	{
		Bucket& b = g_buckets[t.id];
		b.tokens = _min(kBurst, b.tokens + float(now - b.last) * kRatePerSec / 1000.f);
		b.last = now;
		Writer body(256);
		u8 count = 0;
		auto flush = [&]()
		{
			if (!count)
				return;
			Writer m(body.size() + 1);
			m.u8(count);
			m.bytes(body.ptr(), body.size());
			net::host_send(t.id, Msg::EntitySound, m, Lane::Unreliable);
			++g_st.packets;
			body.clear();
			count = 0;
		};
		for (const Outgoing& o : out)
		{
			if (o.pos.distance_to(t.pos) > o.radius)
			{
				++g_st.too_far;
				continue;
			}
			if (b.tokens < 1.f)
			{
				++g_st.rate_dropped;
				continue;
			}
			b.tokens -= 1.f;
			o.s.write(body);
			++g_st.sent;
			if (++count >= kMaxPerPacket)
				flush();
		}
		flush();
	}
}

// ---------------------------------------------------------------- client: playing

xr_vector<Playing>& playing()
{
	if (!g_playing)
		g_playing = xr_new<xr_vector<Playing>>();
	return *g_playing;
}

void release(Playing& p)
{
	if (p.snd._feedback())
		p.snd.stop();
	p.snd.destroy();
}

void stop_all()
{
	if (!g_playing)
		return;
	for (Playing& p : *g_playing)
		release(p);
	g_playing->clear();
}

bool sound_exists(const xr_string& path)
{
	if (!g_exists)
		g_exists = xr_new<xr_map<xr_string, bool>>();
	auto it = g_exists->find(path);
	if (it != g_exists->end())
		return it->second;
	string_path fn;
	const bool yes = !!FS.exist(fn, "$game_sounds$", path.c_str(), ".ogg");
	if (g_exists->size() >= kMaxExistCache)
		g_exists->clear();
	(*g_exists)[path] = yes;
	if (!yes && log_allowed())
		::Msg("! [coop] relayed NPC sound '%s' is not in this game's files", path.c_str());
	return yes;
}

Fvector point_of(const Playing& p, CGameObject* go)
{
	if (p.bone != BI_NONE)
		if (IKinematics* k = smart_cast<IKinematics*>(go->Visual()))
			if (p.bone < k->LL_BoneCount())
			{
				Fmatrix m;
				m.mul_43(go->XFORM(), k->LL_GetBoneInstance(p.bone).mTransform);
				return m.c;
			}
	Fvector v = go->Position();
	v.add(p.offset);
	return v;
}

void start(Playing& p, CGameObject* go)
{
	p.snd.play_at_pos(go, point_of(p, go), 0, 0.f);
	if (p.volume != 1.f)
		p.snd.set_volume(p.volume);
	p.started = true;
	++g_st.played;
}

void queue_lua(LuaEvent&& e)
{
	if (g_lua.size() >= kMaxLuaQueue)
		g_lua.erase(g_lua.begin());
	g_lua.push_back(std::move(e));
}

void client_play(const NpcSound& s)
{
	const u16 id = join::to_local(s.object);
	CGameObject* go = object_here(id);
	if (!go || id == 0 || world::is_player_proxy(id) || !remote::is_remote_creature(go))
	{
		++g_st.no_object; // not online here (yet), or not a host creature
		return;
	}
	const xr_string path = s.path.c_str();
	if (!sound_exists(path))
	{
		++g_st.missing_files;
		return;
	}
	xr_vector<Playing>& list = playing();
	const u32 mask = (s.flags & NpcSound::Mask) ? s.mask : 0;
	u32 of_object = 0;
	for (size_t i = 0; i < list.size();)
	{
		Playing& p = list[i];
		// CSoundPlayer::play on the host removed the creature's sounds with an overlapping synchro mask
		if (p.id == id && mask && (p.mask & mask))
		{
			release(p);
			list.erase(list.begin() + i);
			++g_st.cut;
			continue;
		}
		if (p.id == id)
			++of_object;
		++i;
	}
	if (of_object >= kMaxPerObject || list.size() >= kMaxPlaying)
	{
		for (size_t i = 0; i < list.size(); ++i)
			if (list.size() >= kMaxPlaying || list[i].id == id)
			{
				release(list[i]);
				list.erase(list.begin() + i);
				++g_st.cut;
				break;
			}
	}
	Playing p;
	p.id = id;
	p.snd.create(path.c_str(), st_Effect, 0); // game type 0: no AI sound event for a creature that does not think
	if (!p.snd._handle())
		return;
	p.bone = (s.flags & NpcSound::Bone) ? s.bone : u16(BI_NONE);
	p.mask = mask;
	p.offset.set(s.offset[0], s.offset[1], s.offset[2]);
	p.volume = (s.flags & NpcSound::Volume) ? float(s.volume) / 100.f : 1.f;
	p.follow = (s.flags & NpcSound::Follow) != 0;
	p.start_at = mgr().now_ms() + s.delay_ms;
	list.push_back(p);
	if (!s.delay_ms)
		start(list.back(), go);

	LuaEvent e;
	e.kind = LuaKind::NpcSound;
	e.id = id;
	e.a = path;
	queue_lua(std::move(e));
}

void client_on_sound(PeerId, Reader& r)
{
	if (!mgr().is_live_client() || !g_pGameLevel || !g_npc_sounds)
		return;
	const u8 count = r.u8();
	for (u8 i = 0; i < count && r.ok(); ++i)
	{
		NpcSound s;
		if (!s.read(r))
		{
			++g_st.malformed;
			return;
		}
		++g_st.received;
		client_play(s);
	}
}

void client_frame()
{
	if (!g_playing)
		return;
	xr_vector<Playing>& list = *g_playing;
	const u64 now = mgr().now_ms();
	for (size_t i = 0; i < list.size();)
	{
		Playing& p = list[i];
		CGameObject* go = object_here(p.id);
		bool drop = !go;
		if (go)
		{
			if (!p.started)
			{
				if (now >= p.start_at)
					start(p, go);
			}
			else if (!p.snd._feedback())
				drop = true; // finished
			else if (p.follow)
				p.snd.set_position(point_of(p, go));
		}
		if (drop)
		{
			release(p);
			list.erase(list.begin() + i);
		}
		else
			++i;
	}
}

// ---------------------------------------------------------------- footsteps

// Ground material under a creature (static geometry only), GAMEMTL_NONE_IDX if nothing is below.
u16 ground_under(CGameObject* go)
{
	const u64 now = mgr().now_ms();
	Ground& g = g_ground[go->ID()];
	if (g.t && now - g.t < kGroundCacheMs && g.pos.distance_to(go->Position()) < kGroundCacheMove)
		return g.mtl;
	g.pos = go->Position();
	g.t = now;
	g.mtl = GAMEMTL_NONE_IDX;
	Fvector from = go->Position();
	from.y += 0.5f;
	collide::rq_result R;
	if (Level().ObjectSpace.RayPick(from, Fvector().set(0.f, -1.f, 0.f), kGroundRay, collide::rqtStatic, R, go) &&
	    !R.O && R.element >= 0)
	{
		const CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + R.element;
		const u16 mtl = u16(tri->material);
		if (GMLib.GetMaterialByIdx(mtl))
			g.mtl = mtl;
	}
	++g_st.ground_picks;
	return g.mtl;
}

SGameMtlPair* pair_under(CEntityAlive* creature)
{
	const u16 ground = ground_under(creature);
	if (ground == GAMEMTL_NONE_IDX)
		return nullptr;
	return GMLib.GetMaterialPair(creature->material().self_material_idx(), ground);
}

LPCSTR material_name(SGameMtlPair* pair)
{
	if (pair)
		if (SGameMtl* mt = GMLib.GetMaterialByID(pair->GetMtl1()))
			return *mt->m_Name;
	return "default";
}

LPCSTR move_state(u32 mstate)
{
	using namespace ACTOR_DEFS;
	if (mstate & mcCrouch)
		return "crouch";
	if (mstate & mcSprint)
		return "sprint";
	if (mstate & mcAccel)
		return "walk";
	return "run";
}

void queue_step(CGameObject* avatar, SGameMtlPair* pair, float power, LPCSTR state)
{
	LuaEvent e;
	e.kind = LuaKind::AvatarStep;
	e.id = avatar->ID();
	e.a = material_name(pair);
	e.b = state;
	e.power = power;
	queue_lua(std::move(e));
}

// ---------------------------------------------------------------- Lua

bool lua_ready() { return g_pGameLevel && g_actor && (mgr().is_host() || mgr().is_live_client()); }

// True if a script did AddScriptCallback(name): axr_main keeps the names in its local `intercepts` table (an
// upvalue of make_callback); calling SendScriptCallback for an unknown name prints an error with a call stack.
bool callback_registered(lua_State* L, const char* name)
{
	const int top = lua_gettop(L);
	bool yes = false;
	lua_getglobal(L, "axr_main");
	if (lua_istable(L, -1))
	{
		lua_getfield(L, -1, "make_callback");
		if (lua_isfunction(L, -1))
		{
			for (int i = 1; i < 256; ++i)
			{
				const char* up = lua_getupvalue(L, -1, i);
				if (!up)
					break;
				if (!xr_strcmp(up, "intercepts"))
				{
					if (lua_istable(L, -1))
					{
						lua_getfield(L, -1, name);
						yes = lua_istable(L, -1);
					}
					break;
				}
				lua_pop(L, 1);
			}
		}
	}
	lua_settop(L, top);
	return yes;
}

bool registered(lua_State* L, Registered& r, const char* name)
{
	const u64 now = mgr().now_ms();
	if (!r.valid || now - r.checked > kLuaCheckMs)
	{
		r.yes = callback_registered(L, name);
		r.checked = now;
		r.valid = true;
	}
	return r.yes;
}

bool fire(lua_State* L, const LuaEvent& e)
{
	const char* name = e.kind == LuaKind::AvatarStep ? "coop_avatar_step" : "coop_npc_sound";
	if (!registered(L, e.kind == LuaKind::AvatarStep ? g_reg_step : g_reg_snd, name))
	{
		++g_st.lua_unregistered;
		return false;
	}
	const int top = lua_gettop(L);
	if (!lua_checkstack(L, 8))
		return false;
	lua_getglobal(L, "SendScriptCallback");
	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, top);
		return false;
	}
	lua_pushstring(L, name);
	lua_pushnumber(L, lua_Number(e.id));
	lua_pushlstring(L, e.a.data(), e.a.size());
	int nargs = 3;
	if (e.kind == LuaKind::AvatarStep)
	{
		lua_pushnumber(L, lua_Number(e.power));
		lua_pushlstring(L, e.b.data(), e.b.size());
		nargs = 5;
	}
	bool ok = true;
	if (lua_pcall(L, nargs, 0, 0) != 0)
	{
		ok = false;
		++g_st.lua_errors;
		if (log_allowed())
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] lua error in %s (session continues): %s", name, err ? err : "?");
		}
	}
	lua_settop(L, top);
	if (ok)
		++g_st.lua_fired;
	return ok;
}

void lua_frame()
{
	if (g_lua.empty())
		return;
	if (!lua_ready())
	{
		g_lua.clear();
		return;
	}
	lua_State* L = ai().script_engine().lua();
	if (!L)
	{
		g_lua.clear();
		return;
	}
	xr_vector<LuaEvent> now;
	now.swap(g_lua); // a handler may cause new events: they wait for the next frame
	u32 budget = kLuaPerFrame;
	for (const LuaEvent& e : now)
	{
		if (!budget--)
			break;
		fire(L, e);
	}
}
} // namespace

// ---------------------------------------------------------------- API

void install(Session& s)
{
	s.on(Msg::EntitySound, &client_on_sound);
}

void reset()
{
	{
		xrCriticalSectionGuard guard(&g_out_lock);
		g_out.clear();
		for (Recent& r : g_recent)
			r = Recent();
	}
	g_have_clients = false;
	g_buckets.clear();
	stop_all();
	xr_delete(g_playing);
	xr_delete(g_exists);
	g_lua.clear();
	g_reg_step = Registered();
	g_reg_snd = Registered();
	g_ground.clear();
	g_log_count = 0;
}

void frame()
{
	if (mgr().is_host())
		host_frame();
	else
		g_have_clients = false;
	if (mgr().is_live_client() && g_pGameLevel)
		client_frame();
	else
		stop_all(); // out of the world: nothing of ours may outlive the level's objects
	lua_frame();
	if (g_ground.size() > 256)
		g_ground.clear();
}

void register_console()
{
	CMD4(CCC_Integer, "coop_npc_sounds", &g_npc_sounds, 0, 1);
	CMD4(CCC_Float, "coop_npc_sound_dist", &g_sound_dist, kMinRadius, kMaxRadius);
}

void print_status()
{
	if (mgr().is_host())
		::Msg("[coop]   npc sounds out (%s): %llu recorded, %llu deduped, %llu queue full; %llu sent in %llu packets, "
		      "%llu out of range, %llu over the rate",
		    g_npc_sounds ? "on" : "off", g_st.recorded, g_st.deduped, g_st.queue_full, g_st.sent, g_st.packets,
		    g_st.too_far, g_st.rate_dropped);
	else
		::Msg("[coop]   npc sounds in (%s): %llu received, %llu played, %u playing, %llu cut, %llu no object, %llu "
		      "missing files, %llu malformed, %llu local creature sounds muted",
		    g_npc_sounds ? "on" : "off", g_st.received, g_st.played, g_playing ? (u32)g_playing->size() : 0u,
		    g_st.cut, g_st.no_object, g_st.missing_files, g_st.malformed, g_st.muted_local);
	::Msg("[coop]   steps: %llu ground picks, avatar steps %llu, jumps/landings %llu; lua callbacks %llu fired, %llu "
	      "not registered, %llu errors",
	    g_st.ground_picks, g_st.avatar_steps, g_st.avatar_jumps, g_st.lua_fired, g_st.lua_unregistered,
	    g_st.lua_errors);
}

bool player_sound_blocked(CObject* owner)
{
	if (!g_npc_sounds || !mgr().is_live_client() || !owner)
		return false;
	CGameObject* go = smart_cast<CGameObject*>(owner);
	if (!go || go->ID() == 0 || world::is_player_proxy(go->ID()))
		return false; // our actor; avatars play their own sounds (the player is not relayed)
	if (!remote::is_remote_creature(go))
		return false;
	++g_st.muted_local;
	return true;
}

void on_player_sound(CObject* owner, ref_sound& snd, u16 bone, u32 synchro_mask, u32 start_time)
{
	if (!host_recording())
		return;
	CGameObject* go = relay_source(owner);
	LPCSTR path = go ? relay_path(snd) : nullptr;
	if (!path)
		return;
	NpcSound s;
	s.object = go->ID();
	s.path = path;
	s.flags = NpcSound::Follow;
	if (bone != BI_NONE)
	{
		s.flags |= NpcSound::Bone;
		s.bone = bone;
	}
	if (synchro_mask)
	{
		s.flags |= NpcSound::Mask;
		s.mask = synchro_mask;
	}
	const u32 now = Device.dwTimeGlobal;
	s.delay_ms = start_time > now ? u16(_min<u32>(start_time - now, 0xffff)) : u16(0);
	record(go, s, radius_of(snd));
}

void on_script_sound(CObject* owner, ref_sound& snd, const Fvector* at, u32 flags, float delay, float volume)
{
	if (!host_recording() || (flags & (sm_2D | sm_Looped)))
		return;
	CGameObject* go = relay_source(owner);
	LPCSTR path = go ? relay_path(snd) : nullptr;
	if (!path)
		return;
	NpcSound s;
	s.object = go->ID();
	s.path = path;
	if (at && _valid(*at))
	{
		Fvector off;
		off.sub(*at, go->Position());
		if (off.magnitude() > 1000.f)
			return; // placed far away from its owner: not this creature's sound
		if (off.square_magnitude() > EPS_S)
		{
			s.flags |= NpcSound::Offset;
			s.offset[0] = off.x;
			s.offset[1] = off.y;
			s.offset[2] = off.z;
		}
	}
	if (_valid(volume) && _abs(volume - 1.f) > 0.01f)
	{
		s.flags |= NpcSound::Volume;
		s.volume = u8(iFloor(clampr(volume, 0.f, 2.55f) * 100.f + 0.5f));
	}
	s.delay_ms = _valid(delay) && delay > 0.f ? u16(iFloor(_min(delay, 65.f) * 1000.f)) : u16(0);
	record(go, s, radius_of(snd));
}

SGameMtlPair* step_pair(CEntityAlive* creature, SGameMtlPair* engine_pair)
{
	if (!creature || !mgr().active() || !g_pGameLevel || creature->ID() == 0)
		return engine_pair;
	// Monsters keep the engine's pair: CMaterialManager never tracks their ground (same on the host).
	if (!creature->cast_stalker() || !remote::is_remote_creature(creature))
		return engine_pair;
	SGameMtlPair* p = pair_under(creature);
	return p ? p : engine_pair;
}

void on_step(CGameObject* creature, SGameMtlPair* pair, float power)
{
	if (!creature || !mgr().active() || creature->ID() == 0 || !world::is_player_proxy(creature->ID()))
		return;
	++g_st.avatar_steps;
	queue_step(creature, pair, power, move_state(players::avatar_mstate(creature->ID())));
}

void on_avatar_move(CGameObject* avatar, u32 before, u32 now)
{
	using namespace ACTOR_DEFS;
	if (!avatar || !g_pGameLevel || !mgr().active())
		return;
	const u32 air = mcJump | mcFall;
	LPCSTR state = nullptr;
	float power = 1.f;
	if (!(before & mcJump) && (now & mcJump))
		state = "jump";
	else if ((before & air) && !(now & air) && !(now & mcClimb))
	{
		state = "land";
		power = (now & mcLanding2) ? 1.5f : (now & mcLanding) ? 1.2f : 0.8f;
	}
	if (!state)
		return;
	CEntityAlive* alive = avatar->cast_entity_alive();
	SGameMtlPair* pair = alive ? pair_under(alive) : nullptr;
	if (!pair && alive)
		pair = alive->material().get_current_pair();
	++g_st.avatar_jumps;
	queue_step(avatar, pair, power, state);
}
} // namespace sound
} // namespace coop
