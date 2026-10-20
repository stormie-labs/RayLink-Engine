// Anomaly Coop — presence of other players (WP8). See coop_presence.h.
#include "stdafx.h"
#include "coop_presence.h"
#include "coop_survival.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_net.h"
#include "coop_sync.h"
#include "coop_items.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "../../xrEngine/GameFont.h"
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

void build_tags()
{
	g_tags.clear();
	if (!g_nametags)
		return;
	xr_vector<survival::PlayerRef> players;
	survival::collect_players(players, false);
	const Fvector& cam = Device.vCameraPosition;
	for (const survival::PlayerRef& p : players)
	{
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
		string256 text;
		xr_strcpy(text, p.nick.c_str());
		if (g_nametags_dist)
		{
			string32 d;
			xr_sprintf(d, "  %.0f m", dist);
			xr_strcat(text, d);
		}
		u32 rgb = 0xc8f0c8;
		switch (life)
		{
		case survival::Life::Wounded:
		{
			string64 s;
			xr_sprintf(s, "  [DOWN %u s]", (survival::bleed_left_ms(p.local_id) + 999) / 1000);
			xr_strcat(text, s);
			rgb = 0xff9a3c;
			break;
		}
		case survival::Life::Dead:
		case survival::Life::Spectating:
		case survival::Life::Respawning:
			xr_strcat(text, "  [DEAD]");
			rgb = 0xa0a0a0;
			break;
		default: break;
		}
		const u32 alpha = dist > 100.f ? 0x90 : 0xf0;
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
}

void register_console()
{
	CMD4(CCC_Integer, "coop_nametags", &g_nametags, 0, 1);
	CMD4(CCC_Integer, "coop_nametags_dist", &g_nametags_dist, 0, 1);
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
	xr_delete(g_cache);
}

void frame()
{
	if (!g_pGameLevel || !live())
	{
		g_tags.clear();
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
