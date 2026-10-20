// Anomaly Coop — presence of other players (WP8). See coop_presence.h.
#include "stdafx.h"
#include "coop_presence.h"
#include "coop_survival.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_net.h"
#include "coop_sync.h"
#include "coop_items.h"
#include "coop_sound.h"
#include "coop_requests.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "../../xrEngine/GameFont.h"
#include "../../xrEngine/GameMtlLib.h"
#include "../xrServerEntities/ai_sounds.h"
#include "Level.h"
#include "Level_Bullet_Manager.h"
#include "Actor.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "Weapon.h"
#include "WeaponAmmo.h"
#include "WeaponMagazinedWGrenade.h"
#include "ParticlesObject.h"
#include "GamePersistent.h"
#include "ui_base.h"

namespace coop
{
namespace presence
{
namespace
{
constexpr u8 kMaxShotsPerEvent = 32;
constexpr u32 kMaxBulletsPerEvent = 16;
constexpr float kTagRange = 400.f;
constexpr float kMuzzleSnap = 3.f; // use the avatar weapon's own fire point when it is this close

enum Kind : u8
{
	KindShot = 0,
	KindReload = 1,
	KindLauncher = 2,
};

enum Flags : u8
{
	FlagSilenced = 1,
};

struct ShotEvent
{
	u16 shooter = 0xffff; // host numbering
	u16 weapon = 0xffff; // host numbering
	u8 kind = KindShot;
	u8 count = 1;
	u8 ammo = 0;
	u8 flags = 0;
	Fvector pos{};
	Fvector dir{};

	void write(Writer& w) const
	{
		w.u16(shooter);
		w.u16(weapon);
		w.u8(kind);
		w.u8(count);
		w.u8(ammo);
		w.u8(flags);
		const float p[3] = {pos.x, pos.y, pos.z};
		w.vec3(p);
		const float d[3] = {dir.x, dir.y, dir.z};
		w.vec3(d);
	}
	bool read(Reader& r)
	{
		shooter = r.u16();
		weapon = r.u16();
		kind = r.u8();
		count = r.u8();
		ammo = r.u8();
		flags = r.u8();
		float p[3], d[3];
		r.vec3(p);
		r.vec3(d);
		if (!r.ok() || kind > KindLauncher)
			return false;
		pos.set(p[0], p[1], p[2]);
		dir.set(d[0], d[1], d[2]);
		if (!_valid(pos) || !_valid(dir) || dir.square_magnitude() < EPS_L)
			return false;
		dir.normalize();
		count = _min(count, kMaxShotsPerEvent);
		return true;
	}
};

// Owner side: shots recorded by the weapon hook (maybe on the GameThread worker).
xrCriticalSection g_lock;
xr_vector<ShotEvent> g_pending;
u16 g_reload_weapon = 0xffff;
u32 g_reload_state = 0;

// Engine resources live on the heap and are released in reset(): never destroyed by static teardown after
// the sound device or the string container are gone.
struct Cache
{
	xr_map<shared_str, ref_sound> sounds; // empty ref_sound = file missing, do not retry
	xr_map<shared_str, CCartridge> cartridges;
};
Cache* g_cache = nullptr;

Cache& cache()
{
	if (!g_cache)
		g_cache = xr_new<Cache>();
	return *g_cache;
}

int g_nametags = 1;
int g_nametags_dist = 1;

struct Tag
{
	Fvector pos{};
	xr_string text;
	u32 color = 0xffffffff;
};
xr_vector<Tag> g_tags;

// W5 C2.3: a tag behind static geometry is hidden; one ray per tag every ~150-200 ms.
constexpr u32 kOcclusionCheckMs = 150;
constexpr u32 kOcclusionStaggerMs = 10; // + (id % 6) * 10: tags do not all cast in the same frame
constexpr u32 kOcclusionMaxHits = 4; // see-through surfaces skipped before giving up
struct Sight
{
	bool hidden = false;
	u64 next = 0;
};
xr_map<u16, Sight> g_sight; // by local id of the player character

// W5: shared player palette (W6 "allies UI", system.ltx section [coop_player_colors] in mod_system_coop.ltx):
// color_0..color_7 = "r,g,b", slot = the player's session peer id % 8 (host 0); optional slot_rule = order (index
// among the session's players sorted by peer id); down / dead = colors of a wounded / dead player. Read once per
// session; without the section (or a key) the tags keep their old colors.
constexpr u32 kTagDefaultRgb = 0xc8f0c8;
constexpr u32 kTagDownRgb = 0xff9a3c;
constexpr u32 kTagDeadRgb = 0xa0a0a0;
constexpr u32 kPaletteSize = 8;
struct Palette
{
	bool loaded = false;
	bool by_order = false;
	bool have[kPaletteSize] = {};
	u32 rgb[kPaletteSize] = {};
	u32 down = kTagDownRgb;
	u32 dead = kTagDeadRgb;
} g_palette;

struct Stats
{
	u64 shots_out = 0, reloads_out = 0, events_in = 0, shots_in = 0, reloads_in = 0, bullets = 0;
	u64 no_avatar = 0, no_weapon = 0, sounds_missing = 0;
} g_stat;

bool live() { return mgr().is_host() || mgr().is_live_client(); }

CGameObject* object_here(u16 local_id)
{
	if (!g_pGameLevel || local_id == 0xffff)
		return nullptr;
	CGameObject* o = smart_cast<CGameObject*>(Level().Objects.net_Find(local_id));
	return o && !o->getDestroy() ? o : nullptr;
}

// ---------------------------------------------------------------- effects

ref_sound* sound(LPCSTR name, int game_type)
{
	const shared_str key = name;
	auto& sounds = cache().sounds;
	auto it = sounds.find(key);
	if (it == sounds.end())
	{
		ref_sound& s = sounds[key];
		string_path fn;
		if (FS.exist(fn, "$game_sounds$", name, ".ogg"))
			s.create(name, st_Effect, game_type);
		else
		{
			++g_stat.sounds_missing;
			::Msg("! [coop] avatar sound '%s' not found", name);
		}
		return s._handle() ? &s : nullptr;
	}
	return it->second._handle() ? &it->second : nullptr;
}

// Weapon sound lines look like "weapons\ak74\ak74_shot" or "weapons\x, 1.0, 0.0" (file, volume, delay).
bool play_weapon_sound(const shared_str& sect, LPCSTR key, LPCSTR fallback_key, CObject* source, const Fvector& pos,
                       int game_type)
{
	LPCSTR line_key = pSettings->line_exist(sect, key) ? key : fallback_key;
	if (!line_key || !pSettings->line_exist(sect, line_key))
		return false;
	string_path name;
	_GetItem(pSettings->r_string(sect, line_key), 0, name);
	_Trim(name);
	if (!name[0])
		return false;
	ref_sound* s = sound(name, game_type);
	if (!s)
		return false;
	Fvector at = pos;
	s->play_no_feedback(source, 0, 0.f, &at);
	return true;
}

void play_particles(const shared_str& sect, LPCSTR key, const Fvector& pos, const Fvector& dir)
{
	if (!pSettings->line_exist(sect, key))
		return;
	LPCSTR name = pSettings->r_string(sect, key);
	if (!name || !name[0])
		return;
	Fmatrix m;
	m.identity();
	m.k.set(dir);
	Fvector::generate_orthonormal_basis(m.k, m.j, m.i);
	m.c.set(pos);
	intrusive_ptr<CParticlesObject> ps = Particles::Details::Create(name, TRUE);
	if (!ps)
		return;
	ps->UpdateParent(m, zero_vel);
	GamePersistent().ps_needtoplay.push_back(ps);
}

CCartridge* cartridge(const shared_str& ammo, u8 type)
{
	auto& carts = cache().cartridges;
	auto it = carts.find(ammo);
	if (it == carts.end())
	{
		if (!pSettings->section_exist(ammo))
			return nullptr;
		CCartridge& c = carts[ammo];
		c.Load(ammo.c_str(), type);
		return &c;
	}
	return &it->second;
}

// Cosmetic bullets: tracers, impact marks and ricochets like the real shot, never a hit (SendHit false).
void fire_bullets(CAI_Stalker* avatar, CWeapon* w, const ShotEvent& e, const Fvector& origin)
{
	if (w->m_ammoTypes.empty())
		return;
	const u8 type = e.ammo < w->m_ammoTypes.size() ? e.ammo : 0;
	CCartridge* c = cartridge(w->m_ammoTypes[type], type);
	if (!c || c->bullet_material_idx == u16(-1))
		return;
	const shared_str& sect = w->cNameSect();
	const float speed = READ_IF_EXISTS(pSettings, r_float, sect, "bullet_speed", 900.f);
	const float range = READ_IF_EXISTS(pSettings, r_float, sect, "fire_distance", 600.f);
	const float air = READ_IF_EXISTS(pSettings, r_float, sect, "air_resistance_factor", 1.f);
	const float disp = deg2rad(READ_IF_EXISTS(pSettings, r_float, sect, "fire_dispersion_base", 0.3f));
	const u32 pellets = u32(_max(1, c->param_s.buckShot));
	const u32 n = _min(u32(_max<u8>(e.count, 1)) * pellets, kMaxBulletsPerEvent);
	for (u32 i = 0; i < n; ++i)
	{
		Fvector d = e.dir;
		if (pellets > 1 || i > 0)
			d.random_dir(e.dir, pellets > 1 ? disp * 3.f : disp);
		Level().BulletManager().AddBullet(origin, d, speed, 0.f, 0.f, avatar->ID(), w->ID(), ALife::eHitTypeFireWound,
		    range, *c, air, false);
		++g_stat.bullets;
	}
}

void play(u16 avatar_id, const ShotEvent& e)
{
	CAI_Stalker* av = smart_cast<CAI_Stalker*>(object_here(avatar_id));
	if (!av)
	{
		++g_stat.no_avatar;
		return;
	}
	CWeapon* w = smart_cast<CWeapon*>(object_here(join::to_local(e.weapon)));
	if (!w)
	{
		++g_stat.no_weapon;
		return;
	}
	const shared_str& sect = w->cNameSect();
	Fvector body = av->Position();
	body.y += 1.2f;
	if (e.kind == KindReload)
	{
		play_weapon_sound(sect, "snd_reload", nullptr, av, body, SOUND_TYPE_WEAPON_RECHARGING);
		++g_stat.reloads_in;
		return;
	}

	Fvector origin = e.pos;
	if (w->H_Parent() == av)
	{
		const Fvector fp = w->get_CurrentFirePoint();
		if (_valid(fp) && fp.distance_to(av->Position()) < kMuzzleSnap)
			origin = fp; // third-person muzzle of the avatar's own weapon
	}
	++g_stat.shots_in;
	if (e.kind == KindLauncher)
	{
		// the grenade itself is a world entity (thrown items, WP7): only the launch is heard here
		play_weapon_sound(sect, "snd_shoot_grenade", "snd_shoot", av, origin, SOUND_TYPE_WEAPON_SHOOTING);
		return;
	}
	const bool silenced = (e.flags & FlagSilenced) != 0;
	play_weapon_sound(sect, silenced ? "snd_silncer_shot" : "snd_shoot", "snd_shoot", av, origin,
	    SOUND_TYPE_WEAPON_SHOOTING);
	play_particles(sect, silenced ? "silencer_flame_particles" : "flame_particles", origin, e.dir);
	play_particles(sect, silenced ? "silencer_smoke_particles" : "smoke_particles", origin, e.dir);
	fire_bullets(av, w, e, origin);
}

// ---------------------------------------------------------------- network

void send(ShotEvent& e)
{
	e.shooter = mgr().is_host() ? u16(0) : join::my_proxy();
	if (e.shooter == 0xffff)
		return;
	Writer w(40);
	e.write(w);
	if (mgr().is_host())
		net::host_broadcast(Msg::PlayerShot, w, Lane::Unreliable);
	else
		net::client_send(Msg::PlayerShot, w, Lane::Unreliable);
}

void on_shot_msg(PeerId from, Reader& r)
{
	ShotEvent e;
	if (!e.read(r))
		return;
	++g_stat.events_in;
	u16 avatar;
	if (mgr().is_host())
	{
		avatar = join::proxy_of_peer(from); // never trust the shooter a client claims
		if (avatar == 0xffff || sync::host_peer_joining(from))
			return;
		e.shooter = avatar;
		Writer w(40);
		e.write(w);
		net::host_broadcast(Msg::PlayerShot, w, Lane::Unreliable, from);
	}
	else
	{
		if (!mgr().is_live_client() || from != kHostPeer)
			return;
		avatar = join::to_local(e.shooter);
		if (avatar == 0)
			return; // our own shot echoed back
	}
	play(avatar, e);
}

void send_pending()
{
	xr_vector<ShotEvent> shots;
	{
		xrCriticalSectionGuard guard(&g_lock);
		shots.swap(g_pending);
	}
	for (ShotEvent& e : shots)
	{
		send(e);
		g_stat.shots_out += e.count;
	}
}

void poll_reload()
{
	CActor* a = smart_cast<CActor*>(Level().CurrentEntity());
	CWeapon* w = a ? smart_cast<CWeapon*>(a->inventory().ActiveItem()) : nullptr;
	const u16 id = w ? w->ID() : u16(0xffff);
	const u32 state = w ? w->GetState() : 0;
	if (w && state == CWeapon::eReload && (id != g_reload_weapon || g_reload_state != CWeapon::eReload))
	{
		ShotEvent e;
		e.kind = KindReload;
		e.count = 0;
		e.weapon = join::to_host(id);
		e.pos = a->Position();
		e.dir.set(0.f, 0.f, 1.f);
		send(e);
		++g_stat.reloads_out;
	}
	g_reload_weapon = id;
	g_reload_state = state;
}

// ---------------------------------------------------------------- name tags

void load_palette()
{
	if (g_palette.loaded)
		return;
	g_palette = Palette();
	g_palette.loaded = true;
	LPCSTR sect = "coop_player_colors";
	if (!pSettings->section_exist(sect))
		return;
	if (pSettings->line_exist(sect, "slot_rule"))
	{
		LPCSTR rule = pSettings->r_string(sect, "slot_rule");
		g_palette.by_order = rule && (!xr_strcmp(rule, "order") || !xr_strcmp(rule, "join_order") ||
		    !xr_strcmp(rule, "index"));
	}
	auto read = [sect](LPCSTR key, u32& out)
	{
		if (!pSettings->line_exist(sect, key))
			return false;
		int r = -1, g = -1, b = -1;
		if (sscanf(pSettings->r_string(sect, key), "%d , %d , %d", &r, &g, &b) != 3 || r < 0 || r > 255 || g < 0 ||
		    g > 255 || b < 0 || b > 255)
		{
			::Msg("! [coop] [coop_player_colors] %s is not \"r,g,b\" (0..255): ignored", key);
			return false;
		}
		out = (u32(r) << 16) | (u32(g) << 8) | u32(b);
		return true;
	};
	for (u32 i = 0; i < kPaletteSize; ++i)
	{
		string32 key;
		xr_sprintf(key, "color_%u", i);
		g_palette.have[i] = read(key, g_palette.rgb[i]);
	}
	read("down", g_palette.down);
	read("dead", g_palette.dead);
}

// Color of the player with `peer`; `all` = every player of the session (self included), for slot_rule = order.
u32 player_rgb(PeerId peer, const xr_vector<survival::PlayerRef>& all)
{
	load_palette();
	if (peer == kInvalidPeer)
		return kTagDefaultRgb;
	u32 slot = peer;
	if (g_palette.by_order)
	{
		slot = 0;
		for (const survival::PlayerRef& p : all)
			if (p.peer != kInvalidPeer && p.peer < peer)
				++slot;
	}
	slot %= kPaletteSize;
	return g_palette.have[slot] ? g_palette.rgb[slot] : kTagDefaultRgb;
}

// True if static geometry that blocks sight is between the camera and `to`. Glass, nets, foliage and other
// passable or see-through materials do not count.
bool occluded(const Fvector& from, const Fvector& to)
{
	Fvector dir;
	dir.sub(to, from);
	float range = dir.magnitude();
	if (range < 1.f)
		return false;
	dir.div(range);
	Fvector start = from;
	for (u32 i = 0; i < kOcclusionMaxHits; ++i)
	{
		collide::rq_result R;
		if (!Level().ObjectSpace.RayPick(start, dir, range, collide::rqtStatic, R, nullptr) || R.O || R.element < 0)
			return false;
		const CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + R.element;
		const SGameMtl* mtl = GMLib.GetMaterialByIdx(u16(tri->material));
		if (!mtl || !mtl->Flags.test(SGameMtl::flPassable | SGameMtl::flTransparent | SGameMtl::flShootable))
			return true;
		const float step = R.range + 0.05f;
		start.mad(dir, step);
		range -= step;
		if (range <= 0.f)
			return false;
	}
	return true; // a stack of see-through layers: treat as a wall
}

bool tag_hidden(u16 id, const Fvector& from, const Fvector& to)
{
	const u64 now = mgr().now_ms();
	Sight& s = g_sight[id];
	if (now >= s.next)
	{
		s.hidden = occluded(from, to);
		s.next = now + kOcclusionCheckMs + (id % 6) * kOcclusionStaggerMs;
	}
	return s.hidden;
}

void build_tags()
{
	g_tags.clear();
	if (!g_nametags)
		return;
	xr_vector<survival::PlayerRef> players;
	survival::collect_players(players, true); // self too: slot_rule = order counts every player
	const Fvector& cam = Device.vCameraPosition;
	for (const survival::PlayerRef& p : players)
	{
		if (p.self)
			continue;
		CGameObject* o = object_here(p.local_id);
		if (!o || !o->getVisible())
			continue;
		const survival::Life life = survival::life_of(p.local_id);
		Tag tag;
		tag.pos = o->Position();
		tag.pos.y += survival::is_down(life) ? 0.8f : 2.05f;
		const float dist = cam.distance_to(tag.pos);
		if (dist > kTagRange)
			continue;
		const bool behind_wall = tag_hidden(p.local_id, cam, tag.pos);
		if (behind_wall && life != survival::Life::Wounded)
			continue; // a downed player stays findable through walls (revive), dimmed
		string256 text;
		xr_strcpy(text, p.nick.c_str());
		if (g_nametags_dist)
		{
			string32 d;
			xr_sprintf(d, "  %.0f m", dist);
			xr_strcat(text, d);
		}
		u32 rgb = player_rgb(p.peer, players);
		switch (life)
		{
		case survival::Life::Wounded:
		{
			string64 s;
			xr_sprintf(s, "  [DOWN %u s]", (survival::bleed_left_ms(p.local_id) + 999) / 1000);
			xr_strcat(text, s);
			rgb = g_palette.down;
			break;
		}
		case survival::Life::Dead:
		case survival::Life::Spectating:
		case survival::Life::Respawning:
			xr_strcat(text, "  [DEAD]");
			rgb = g_palette.dead;
			break;
		default: break;
		}
		const u32 alpha = behind_wall ? 0x70 : dist > 100.f ? 0x90 : 0xf0;
		tag.color = (alpha << 24) | rgb;
		tag.text = text;
		g_tags.push_back(tag);
	}
}
} // namespace

void on_weapon_shot(CWeapon* w)
{
	if (!w || !g_pGameLevel || !live())
		return;
	CObject* parent = w->H_Parent();
	if (!parent || parent != Level().CurrentEntity())
		return; // only the local actor's own weapon: avatars and NPCs are someone else's
	ShotEvent e;
	e.weapon = join::to_host(w->ID());
	if (CWeaponMagazinedWGrenade* gl = smart_cast<CWeaponMagazinedWGrenade*>(w))
		if (gl->GetGrenadeLauncherMode())
			e.kind = KindLauncher;
	e.ammo = w->m_ammoType;
	e.flags = w->IsSilencerAttached() ? FlagSilenced : 0;
	e.pos = w->get_LastFP();
	e.dir = w->get_LastFD();
	xrCriticalSectionGuard guard(&g_lock);
	if (!g_pending.empty())
	{
		ShotEvent& last = g_pending.back();
		if (last.weapon == e.weapon && last.kind == e.kind && last.count < kMaxShotsPerEvent)
		{
			++last.count; // same burst in the same frame: one event
			return;
		}
	}
	g_pending.push_back(e);
}

void install(Session& s)
{
	s.on(Msg::PlayerShot, &on_shot_msg);
	items::set_weapon_fired_handler(&on_weapon_shot);
	sound::install(s); // W5 C2: NPC sounds and avatar steps ride on presence (no own line in coop_sync)
}

void register_console()
{
	CMD4(CCC_Integer, "coop_nametags", &g_nametags, 0, 1);
	CMD4(CCC_Integer, "coop_nametags_dist", &g_nametags_dist, 0, 1);
	sound::register_console();
	requests::register_console(); // W5 C2.1 (coop_client_hit_perks)
}

void reset()
{
	{
		xrCriticalSectionGuard guard(&g_lock);
		g_pending.clear();
	}
	g_reload_weapon = 0xffff;
	g_reload_state = 0;
	g_tags.clear();
	g_sight.clear();
	g_palette = Palette(); // re-read: the next session may run with another mod list
	xr_delete(g_cache);
	sound::reset();
}

void frame()
{
	sound::frame();
	if (!g_pGameLevel || !live())
	{
		g_tags.clear();
		g_sight.clear();
		return;
	}
	send_pending();
	poll_reload();
	build_tags();
}

void print_status()
{
	::Msg("[coop]   presence: shots out %llu, reloads out %llu; in: %llu events, %llu shots, %llu reloads, %llu "
	      "bullets; no avatar %llu, no weapon %llu, missing sounds %llu; name tags %s",
	    g_stat.shots_out, g_stat.reloads_out, g_stat.events_in, g_stat.shots_in, g_stat.reloads_in, g_stat.bullets,
	    g_stat.no_avatar, g_stat.no_weapon, g_stat.sounds_missing, g_nametags ? "on" : "off");
	sound::print_status();
}

void render_hud()
{
	if (g_tags.empty() || !g_nametags)
		return;
	CGameFont* f = UI().Font().pFontArial14;
	if (!f)
		return;
	f->SetAligment(CGameFont::alCenter);
	for (const Tag& tag : g_tags)
	{
		Fvector4 v;
		Device.mFullTransform.transform(v, tag.pos);
		if (v.z < 0.f || v.w < 0.f || v.x < -1.f || v.x > 1.f || v.y < -1.f || v.y > 1.f)
			continue;
		const float x = (1.f + v.x) * 0.5f * float(Device.dwWidth);
		const float y = (1.f - v.y) * 0.5f * float(Device.dwHeight);
		f->SetColor(tag.color);
		f->Out(x, y, "%s", tag.text.c_str());
	}
	f->SetAligment(CGameFont::alLeft); // shared UI font: leave it as found (queued lines keep their alignment)
}
} // namespace presence
} // namespace coop
