// Anomaly Coop — what a player looks like and how a player feels.
//
// PlayerEquip (reliable, on change + every 5 s): the owner's visual, item in hands, torch.
//   Applied to the player's avatar everywhere; on the host the proxy keeps the visual in its
//   server entity, so the character looks right after a save/load (P4).
//   W5 tail: handheld flashlight on + its light section (drawn here by our own lights, see HandLight)
//   and safe mode (weapon lowered, coop.player_safemode).
// PlayerVitals (reliable, every 2 s, client -> host): health, satiety, radiation, psy, power,
//   bleeding. The host stores them in the proxy's custom data ([coop] section), i.e. inside the
//   host save (P4). On join the client restores its actor from there.
#include "stdafx.h"
#include "coop_players.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_entities.h"
#include "coop_net.h"
#include "coop_alife.h"

#include "Level.h"
#include "Actor.h"
#include "ActorCondition.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "Torch.h"
#include "Flashlight.h"
#include "../xrEngine/LightAnimLibrary.h"
#include "../Include/xrRender/Kinematics.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrMessages.h"
#include "ai_monster_space.h"

namespace coop
{
namespace players
{
namespace
{
constexpr u32 kEquipCheckMs = 500;
constexpr u32 kEquipRefreshMs = 5000;
constexpr u32 kVitalsMs = 2000;

constexpr size_t kMaxLightSection = 64;

struct Equip
{
	xr_string visual;
	u16 active_item = 0xffff; // host numbering
	bool torch = false; // head torch (CTorch)
	// W5 tail: u8 flags (1 hand light, 2 safe mode), str light section. Appended to the old payload:
	// a sender without it reads as "no hand light, safe mode unknown".
	bool tail = false;
	bool hand_light = false; // handheld flashlight (CFlashlight in DETECTOR_SLOT) is on
	bool safemode = false; // weapon lowered (Anomaly safe mode)
	xr_string light_section; // the flashlight's light section, as CFlashlight::Load reads it

	bool operator==(const Equip& o) const
	{
		return visual == o.visual && active_item == o.active_item && torch == o.torch && tail == o.tail &&
			hand_light == o.hand_light && safemode == o.safemode && light_section == o.light_section;
	}
	void write(Writer& w) const
	{
		w.str(visual.c_str());
		w.u16(active_item);
		w.boolean(torch);
		if (!tail)
			return;
		w.u8(u8((hand_light ? 1 : 0) | (safemode ? 2 : 0)));
		w.str(hand_light ? light_section.c_str() : "");
	}
	bool read(Reader& r)
	{
		visual = r.str().c_str();
		active_item = r.u16();
		torch = r.boolean();
		if (!r.ok())
			return false;
		tail = hand_light = safemode = false;
		light_section.clear();
		if (r.remaining() > 0)
		{
			const u8 flags = r.u8();
			const std::string sect = r.str();
			if (r.ok() && sect.size() <= kMaxLightSection)
			{
				tail = true;
				safemode = (flags & 2) != 0;
				hand_light = (flags & 1) != 0 && !sect.empty();
				if (hand_light)
					light_section = sect.c_str();
			}
		}
		return true; // a broken tail does not cost the visual and the item in hands
	}
};

struct Vitals
{
	float health = 1.f, satiety = 1.f, radiation = 0.f, psy = 1.f, power = 1.f, bleeding = 0.f;
};

Equip g_last_sent;
u64 g_last_check = 0, g_last_refresh = 0, g_last_vitals = 0;
// Equipment waiting for its avatar to come online (P6), keyed by local avatar id.
xr_map<u16, Equip> g_pending;
xr_map<u16, Equip> g_applied;
bool g_vitals_restored = false;
u64 g_equip_in = 0, g_equip_out = 0, g_vitals_out = 0, g_vitals_in = 0;

bool capture_equip(Equip& e)
{
	CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
	if (!actor || !actor->g_Alive())
		return false;
	e.visual = actor->cNameVisual().c_str();
	PIItem active = actor->inventory().ActiveItem();
	e.active_item = active ? join::to_host(active->object_id()) : 0xffff;
	e.torch = false;
	for (PIItem item : actor->inventory().m_all)
	{
		const CTorch* torch = smart_cast<const CTorch*>(item);
		if (torch && torch->torch_active())
		{
			e.torch = true;
			break;
		}
	}
	// W5: the handheld flashlight lights only from our HUD; the others draw it themselves (hand_light_frame).
	e.tail = true;
	e.safemode = actor->is_safemode();
	e.hand_light = false;
	e.light_section.clear();
	if (PIItem det = actor->inventory().ItemFromSlot(DETECTOR_SLOT))
	{
		CFlashlight* fl = det->cast_flashlight();
		if (fl && fl->torch_active())
		{
			LPCSTR sect = det->object().cNameSect().c_str();
			LPCSTR light = READ_IF_EXISTS(pSettings, r_string, sect, "light_section", "torch_definition");
			if (light && *light && xr_strlen(light) <= kMaxLightSection)
			{
				e.hand_light = true;
				e.light_section = light;
			}
		}
	}
	return true;
}

// Returns false if the avatar is not online yet (keep it pending).
bool apply_equip(u16 avatar_id, const Equip& e)
{
	CAI_Stalker* av = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(avatar_id));
	if (!av || av->getDestroy())
		return false;

	entities::ClientApply scope; // our own local events below must pass the client router
	if (!e.visual.empty() && xr_strcmp(av->cNameVisual(), e.visual.c_str()))
	{
		NET_Packet P;
		av->u_EventGen(P, GE_CHANGE_VISUAL, av->ID());
		P.w_stringZ(e.visual.c_str());
		av->u_EventSend(P); // server entity keeps the visual (saved with the host world)
		av->ChangeVisual(e.visual.c_str());
	}

	CGameObject* in_hands = nullptr;
	if (e.active_item != 0xffff)
	{
		CObject* o = Level().Objects.net_Find(join::to_local(e.active_item));
		if (o && o->H_Parent() == av)
			in_hands = smart_cast<CGameObject*>(o);
	}
	av->CObjectHandler::set_goal(MonsterSpace::eObjectActionIdle, in_hands);

	for (PIItem item : av->inventory().m_all)
		if (CTorch* torch = smart_cast<CTorch*>(item))
			if (torch->torch_active() != e.torch)
				torch->Switch(e.torch);

	g_applied[avatar_id] = e;
	return true;
}

// --- handheld flashlight of other players (W5) ---
// CFlashlight lights only from its owner's HUD (UpdateCL needs a CActor parent and HUD firedeps), so on an
// avatar it shows nothing. Each machine draws it with its own lights, set up like CFlashlight::net_Spawn from
// the owner's light section, at the avatar's left hand, pointing where the model faces (the owner's pitch is
// not available outside coop_players.cpp). Main thread only (equip_frame).
struct HandLight
{
	xr_string section;
	ref_light spot, omni;
	ref_glow glow;
	CLAItem* lanim = nullptr;
	float brightness = 1.f;
	const IRenderVisual* visual = nullptr; // `bone` belongs to this visual
	u16 bone = BI_NONE;
	bool placed = false;
	bool dynamic = false; // spot and omni on (the avatar may use dynamic lights)
};
// Heap objects: a static map destroyed at process exit must not release render lights after the renderer.
xr_map<u16, HandLight*> g_lights; // local avatar id -> its hand light
u32 g_lights_world = 0; // join::world_generation() the lights were made in
u64 g_lights_made = 0, g_lights_bad = 0;

void destroy_light(HandLight* l)
{
	l->spot.destroy();
	l->omni.destroy();
	l->glow.destroy();
	xr_delete(l);
}

void destroy_lights()
{
	for (auto& it : g_lights)
		destroy_light(it.second);
	g_lights.clear();
}

CAI_Stalker* avatar_here(u16 id)
{
	if (!g_pGameLevel || id == 0 || id == 0xffff)
		return nullptr;
	CAI_Stalker* av = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(id));
	return av && !av->getDestroy() ? av : nullptr;
}

// The same keys as CFlashlight::net_Spawn (R2+ variants on R2/R3/R4); every key optional: the section
// comes from the network.
HandLight* make_light(const xr_string& section)
{
	LPCSTR s = section.c_str();
	const bool r2 = !!psDeviceFlags.test(rsR2) || !!psDeviceFlags.test(rsR3) || !!psDeviceFlags.test(rsR4);
	Fcolor white;
	white.set(1.f, 1.f, 1.f, 1.f);
	HandLight* l = xr_new<HandLight>();
	l->section = section;
	l->spot = ::Render->light_create();
	l->omni = ::Render->light_create();
	l->glow = ::Render->glow_create();

	l->lanim = LALib.FindItem(READ_IF_EXISTS(pSettings, r_string, s, "color_animator", ""));
	const Fcolor clr = READ_IF_EXISTS(pSettings, r_fcolor, s, r2 ? "color_r2" : "color", white);
	l->brightness = clr.intensity();
	l->spot->set_type((IRender_Light::LT)(READ_IF_EXISTS(pSettings, r_u8, s, "type", 2)));
	l->spot->set_color(clr);
	l->spot->set_range(READ_IF_EXISTS(pSettings, r_float, s, r2 ? "range_r2" : "range", 20.f));
	l->spot->set_hud_mode(false); // a world light here, never the HUD pass
	l->spot->set_cone(deg2rad(READ_IF_EXISTS(pSettings, r_float, s, "spot_angle", 60.f)));
	l->spot->set_texture(READ_IF_EXISTS(pSettings, r_string, s, "spot_texture", (LPCSTR)nullptr));
	l->spot->set_volumetric(!!READ_IF_EXISTS(pSettings, r_bool, s, "volumetric", FALSE));
	l->spot->set_volumetric_quality(READ_IF_EXISTS(pSettings, r_float, s, "volumetric_quality", 1.f));
	l->spot->set_volumetric_intensity(READ_IF_EXISTS(pSettings, r_float, s, "volumetric_intensity", 1.f));
	l->spot->set_volumetric_distance(READ_IF_EXISTS(pSettings, r_float, s, "volumetric_distance", 1.f));
	l->spot->set_shadow(true);
	l->spot->set_is_playerlight(false); // not this machine's player

	l->omni->set_type((IRender_Light::LT)(READ_IF_EXISTS(pSettings, r_u8, s, "omni_type", 1)));
	l->omni->set_color(READ_IF_EXISTS(pSettings, r_fcolor, s, r2 ? "omni_color_r2" : "omni_color", clr));
	l->omni->set_range(READ_IF_EXISTS(pSettings, r_float, s, r2 ? "omni_range_r2" : "omni_range", 1.f));
	l->omni->set_hud_mode(false);
	l->omni->set_shadow(false);
	l->omni->set_is_playerlight(false);

	l->glow->set_texture(READ_IF_EXISTS(pSettings, r_string, s, "glow_texture", "glow\\glow_torch_r2"));
	l->glow->set_color(clr);
	l->glow->set_radius(READ_IF_EXISTS(pSettings, r_float, s, "glow_radius", 0.2f));
	++g_lights_made;
	return l;
}

void place_light(HandLight* l, CAI_Stalker* av)
{
	IKinematics* K = av->Visual() ? smart_cast<IKinematics*>(av->Visual()) : nullptr;
	if (av->Visual() != l->visual)
	{
		l->visual = av->Visual(); // a new visual (GE_CHANGE_VISUAL) has its own bone ids
		l->bone = BI_NONE;
		if (K)
		{
			l->bone = K->LL_BoneID("bip01_l_hand");
			if (l->bone == BI_NONE)
				l->bone = K->LL_BoneID("bip01_head");
		}
	}
	const Fmatrix& X = av->XFORM();
	Fvector pos;
	if (K && l->bone != BI_NONE && l->bone < K->LL_BoneCount())
	{
		K->CalculateBones(FALSE); // once per frame at most
		Fmatrix M;
		M.mul_43(X, K->LL_GetTransform(l->bone));
		pos = M.c;
	}
	else
	{
		pos = X.c;
		pos.y += 1.2f;
	}
	Fvector dir = X.k, right = X.i; // model facing (drive_avatar sets it from the owner's body yaw)
	float view_yaw = 0.f, view_pitch = 0.f;
	if (avatar_view(av->ID(), view_yaw, view_pitch)) // W5: the beam follows where the player looks
	{
		dir.setHP(view_yaw, -view_pitch);
		right.crossproduct(Fvector().set(0.f, 1.f, 0.f), dir);
		if (right.square_magnitude() > EPS_S)
			right.normalize();
		else
			right = X.i; // looking straight up or down
	}
	pos.mad(dir, 0.1f); // just in front of the hand, not inside the model

	const bool dynamic = av->can_use_dynamic_lights();
	if (dynamic)
	{
		l->spot->set_position(pos);
		l->spot->set_rotation(dir, right);
		l->omni->set_position(pos);
		l->omni->set_rotation(dir, right);
	}
	l->glow->set_position(pos);
	l->glow->set_direction(dir);
	if (dynamic != l->dynamic || !l->placed)
	{
		l->spot->set_active(dynamic); // after the first position: activation registers it where it is
		l->omni->set_active(dynamic);
		l->dynamic = dynamic;
	}
	if (!l->placed)
	{
		l->glow->set_active(true);
		l->placed = true;
	}

	if (!l->lanim)
		return;
	int frame;
	const u32 c = l->lanim->CalculateBGR(Device.fTimeGlobal, frame);
	Fcolor f;
	f.set(float(color_get_B(c)), float(color_get_G(c)), float(color_get_R(c)), 1.f);
	f.mul_rgb(l->brightness / 255.f);
	if (dynamic)
	{
		l->spot->set_color(f);
		l->omni->set_color(f);
	}
	l->glow->set_color(f);
}

// Every frame (main thread): lights follow the avatars, appear and go with the owners' flashlights.
void hand_light_frame()
{
	const u32 world = join::world_generation();
	if (world != g_lights_world)
	{
		destroy_lights(); // a new world: the old avatars and render sectors are gone
		g_lights_world = world;
	}
	for (auto it = g_lights.begin(); it != g_lights.end();)
	{
		const auto e = g_applied.find(it->first);
		if (!avatar_here(it->first) || e == g_applied.end() || !e->second.hand_light ||
			e->second.light_section != it->second->section)
		{
			destroy_light(it->second);
			it = g_lights.erase(it);
		}
		else
			++it;
	}
	if (Device.dwPrecacheFrame)
		return;
	for (const auto& it : g_applied)
	{
		if (!it.second.hand_light)
			continue;
		CAI_Stalker* av = avatar_here(it.first);
		if (!av)
			continue;
		auto l = g_lights.find(it.first);
		if (l == g_lights.end())
		{
			if (!pSettings->section_exist(it.second.light_section.c_str()))
			{
				if (++g_lights_bad <= 10)
					::Msg("! [coop] hand light section '%s' is unknown here (different mods?)", it.second.light_section.c_str());
				continue;
			}
			l = g_lights.emplace(it.first, make_light(it.second.light_section)).first;
		}
		place_light(l->second, av);
	}
}

void on_equip(PeerId from, Reader& r)
{
	const u16 host_id = r.u16();
	Equip e;
	if (!e.read(r))
		return;
	++g_equip_in;
	u16 local_id;
	if (mgr().is_host())
	{
		local_id = join::proxy_of_peer(from); // never trust the id a client claims
		if (local_id == 0xffff)
			return;
		Writer w(128);
		w.u16(local_id);
		e.write(w);
		net::host_broadcast(Msg::PlayerEquip, w, Lane::Reliable, from);
	}
	else
	{
		local_id = join::to_local(host_id);
		if (local_id == 0)
			return;
	}
	if (!apply_equip(local_id, e))
		g_pending[local_id] = e;
	else
		g_pending.erase(local_id);
}

void send_equip(const Equip& e)
{
	Writer w(128);
	w.u16(mgr().is_host() ? u16(0) : join::my_proxy());
	e.write(w);
	if (mgr().is_host())
		net::host_broadcast(Msg::PlayerEquip, w);
	else
		net::client_send(Msg::PlayerEquip, w);
	g_last_sent = e;
	++g_equip_out;
}

// --- vitals ---

void write_vitals(Writer& w, const Vitals& v)
{
	w.f32(v.health);
	w.f32(v.satiety);
	w.f32(v.radiation);
	w.f32(v.psy);
	w.f32(v.power);
	w.f32(v.bleeding);
}

bool read_vitals(Reader& r, Vitals& v)
{
	v.health = r.f32();
	v.satiety = r.f32();
	v.radiation = r.f32();
	v.psy = r.f32();
	v.power = r.f32();
	v.bleeding = r.f32();
	return r.ok();
}

// Host: keep the player's vitals in the proxy custom data (inside the world save).
void store_vitals(u16 proxy, const Vitals& v)
{
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(proxy, true);
	if (!e || !world::is_player_proxy(proxy))
		return; // never rewrite the custom data (logic) of anything but a player character
	xr_string ini = e->m_ini_string.c_str();
	const size_t at = ini.find("vitals =");
	if (at != xr_string::npos)
	{
		const size_t end = ini.find('\n', at);
		ini.erase(at, end == xr_string::npos ? xr_string::npos : end - at + 1);
	}
	string256 line;
	xr_sprintf(line, "vitals = %.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", v.health, v.satiety, v.radiation, v.psy, v.power,
	           v.bleeding);
	ini += line;
	e->m_ini_string = ini.c_str();
}

void host_on_vitals(PeerId from, Reader& r)
{
	Vitals v;
	if (!mgr().is_host() || !read_vitals(r, v))
		return;
	const u16 proxy = join::proxy_of_peer(from);
	if (proxy == 0xffff)
		return;
	store_vitals(proxy, v);
	++g_vitals_in;
}

void client_send_vitals()
{
	CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
	if (!actor)
		return;
	CActorCondition& c = actor->conditions();
	Vitals v;
	v.health = c.GetHealth();
	v.satiety = c.GetSatiety();
	v.radiation = c.GetRadiation();
	v.psy = c.GetPsyHealth();
	v.power = c.GetPower();
	v.bleeding = c.BleedingSpeed();
	Writer w(32);
	write_vitals(w, v);
	net::client_send(Msg::PlayerVitals, w);
	++g_vitals_out;
}

// Client, once after join: our proxy (now the host avatar here) carries our saved vitals.
void client_restore_vitals()
{
	CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
	const u16 host_avatar = join::my_proxy();
	if (!actor || host_avatar == 0xffff)
		return;
	const CSE_ALifeDynamicObject* e = ai().alife().objects().object(host_avatar, true);
	g_vitals_restored = true;
	CActorCondition& c = actor->conditions();
	// The actor's client data in the save is the host's: radiation, wounds and the rest of the
	// host's condition came with it. Only health came from our character (role swap).
	const char* at = e && e->m_ini_string.size() ? strstr(e->m_ini_string.c_str(), "vitals =") : nullptr;
	Vitals v;
	if (!at || sscanf(at, "vitals = %f,%f,%f,%f,%f,%f", &v.health, &v.satiety, &v.radiation, &v.psy, &v.power,
	                  &v.bleeding) != 6)
	{
		// A new character: nothing of the host's state, fresh values as a new game starts them
		// (CEntityCondition / CActorCondition::reinit).
		c.ClearWounds();
		c.SetRadiation(0.f);
		c.SetSatiety(1.f);
		c.SetPsyHealth(1.f);
		c.SetMaxPower(1.f);
		c.MaxPower();
		::Msg("[coop] new character: fresh condition (satiety, psy health, power)");
		return;
	}
	if (v.bleeding <= 0.f)
		c.ClearWounds();
	c.SetHealth(clampr(v.health, 0.05f, c.GetMaxHealth()));
	c.SetSatiety(v.satiety);
	c.SetRadiation(v.radiation);
	c.ChangePsyHealth(v.psy - c.GetPsyHealth());
	c.ChangePower(v.power - c.GetPower());
	::Msg("[coop] restored my condition: health %.2f, satiety %.2f, radiation %.2f", v.health, v.satiety, v.radiation);
}
} // namespace

void equip_install(Session& s)
{
	s.on(Msg::PlayerEquip, [](PeerId from, Reader& r)
	{
		if (mgr().is_host() || mgr().is_live_client())
			on_equip(from, r);
	});
	s.on(Msg::PlayerVitals, &host_on_vitals);
}

bool avatar_safemode(u16 avatar_id, bool& lowered)
{
	auto it = g_applied.find(avatar_id);
	const Equip* e = it != g_applied.end() ? &it->second : nullptr;
	if (!e)
	{
		auto p = g_pending.find(avatar_id);
		e = p != g_pending.end() ? &p->second : nullptr;
	}
	if (!e || !e->tail)
		return false; // not reported yet, or a sender without the W5 tail
	lowered = e->safemode;
	return true;
}

void equip_reset()
{
	destroy_lights();
	g_last_sent = Equip();
	g_pending.clear();
	g_applied.clear();
	g_last_check = g_last_refresh = g_last_vitals = 0;
	g_vitals_restored = false;
}

void equip_frame()
{
	const bool live = mgr().is_host() || mgr().is_live_client();
	if (!live || !g_pGameLevel)
		return;
	const u64 now = mgr().now_ms();

	if (mgr().is_live_client() && !g_vitals_restored)
		client_restore_vitals();

	if (now - g_last_check >= kEquipCheckMs)
	{
		g_last_check = now;
		Equip e;
		if (capture_equip(e) && (!(e == g_last_sent) || now - g_last_refresh >= kEquipRefreshMs))
		{
			g_last_refresh = now;
			send_equip(e);
		}
		for (auto it = g_pending.begin(); it != g_pending.end();)
		{
			if (apply_equip(it->first, it->second))
				it = g_pending.erase(it);
			else
				++it;
		}
	}

	if (mgr().is_live_client() && now - g_last_vitals >= kVitalsMs)
	{
		g_last_vitals = now;
		client_send_vitals();
	}

	hand_light_frame();
}

void equip_resend()
{
	g_last_sent = Equip();
	g_last_refresh = 0;
}

void equip_print_status()
{
	::Msg("[coop]   equipment: %llu out, %llu in, %u pending; vitals: %llu out, %llu in", g_equip_out, g_equip_in,
	      (u32)g_pending.size(), g_vitals_out, g_vitals_in);
	::Msg("[coop]   hand lights: %u on, %llu made, %llu unknown sections | my flashlight %s, safe mode %s",
	      (u32)g_lights.size(), g_lights_made, g_lights_bad, g_last_sent.hand_light ? g_last_sent.light_section.c_str() : "off",
	      g_last_sent.safemode ? "on" : "off");
}
} // namespace players
} // namespace coop
