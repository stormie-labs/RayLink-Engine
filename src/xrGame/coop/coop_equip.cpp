// Anomaly Coop — what a player looks like and how a player feels.
//
// PlayerEquip (reliable, on change + every 5 s): the owner's visual, item in hands, torch.
//   Applied to the player's avatar everywhere; on the host the proxy keeps the visual in its
//   server entity, so the character looks right after a save/load (P4).
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

struct Equip
{
	xr_string visual;
	u16 active_item = 0xffff; // host numbering
	bool torch = false;

	bool operator==(const Equip& o) const
	{
		return visual == o.visual && active_item == o.active_item && torch == o.torch;
	}
	void write(Writer& w) const
	{
		w.str(visual.c_str());
		w.u16(active_item);
		w.boolean(torch);
	}
	bool read(Reader& r)
	{
		visual = r.str().c_str();
		active_item = r.u16();
		torch = r.boolean();
		return r.ok();
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
		c.ClearWounds(); // a new character: nothing of the host's state
		c.SetRadiation(0.f);
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

void equip_reset()
{
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
}
} // namespace players
} // namespace coop
