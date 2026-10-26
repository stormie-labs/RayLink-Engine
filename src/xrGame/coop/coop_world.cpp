#include "stdafx.h"
#include "coop_world.h"
#include "coop_manager.h"

#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "GameObject.h"

namespace coop
{
namespace world
{
namespace
{
struct Avatar
{
	PeerId peer = kInvalidPeer; // kInvalidPeer = parked (player offline)
};

xr_map<u16, Avatar> g_avatars;
bool g_owners_known = false; // client: an AvatarList was applied since reset() (F0 final review, task 8)

// Docked once: a section compare is then a pointer compare (the ALife switch asks for every object).
const shared_str& avatar_section()
{
	static const shared_str s("coop_player_avatar");
	return s;
}
} // namespace

void host_scan_proxies()
{
	g_avatars.clear();
	if (!ai().get_alife())
		return;
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		if (e && !xr_strcmp(e->s_name, "coop_player_avatar"))
			g_avatars[e->ID] = Avatar();
	}
	if (!g_avatars.empty())
		::Msg("[coop] %u player character(s) in this world (parked until their players join)", (u32)g_avatars.size());
}

void host_mark_proxy(u16 id, PeerId peer) { g_avatars[id].peer = peer; }

void host_park_proxy(u16 id)
{
	auto it = g_avatars.find(id);
	if (it != g_avatars.end())
		it->second.peer = kInvalidPeer;
}

void host_forget_proxy(u16 id) { g_avatars.erase(id); }

void client_mark_host_avatar(u16 id) { g_avatars[id].peer = kHostPeer; }

void client_mark_avatar(u16 id, PeerId peer) { g_avatars[id].peer = peer; }

void client_set_owners(const xr_vector<AvatarOwner>& local_owners)
{
	for (auto& it : g_avatars)
		it.second.peer = kInvalidPeer;
	for (const AvatarOwner& o : local_owners)
		g_avatars[o.avatar].peer = o.peer;
	g_owners_known = true;
}

bool owners_known() { return mgr().is_host() || g_owners_known; }

bool is_player_proxy(u16 id) { return g_avatars.find(id) != g_avatars.end(); }

bool is_player_character(const CSE_Abstract* e) { return e && e->s_name == avatar_section(); }

bool is_player_character(const CGameObject* obj) { return obj && obj->cNameSect() == avatar_section(); }

PeerId avatar_peer(u16 id)
{
	auto it = g_avatars.find(id);
	return it == g_avatars.end() ? kInvalidPeer : it->second.peer;
}

u16 avatar_of(PeerId peer)
{
	for (const auto& it : g_avatars)
		if (it.second.peer == peer)
			return it.first;
	return 0xffff;
}

bool is_parked(u16 id)
{
	auto it = g_avatars.find(id);
	return it != g_avatars.end() && it->second.peer == kInvalidPeer;
}

void reset()
{
	g_avatars.clear();
	g_owners_known = false;
}

void print_status()
{
	for (const auto& it : g_avatars)
	{
		if (it.second.peer == kInvalidPeer)
			::Msg("[coop]   avatar %u: parked", it.first);
		else
			::Msg("[coop]   avatar %u: player #%u", it.first, it.second.peer);
	}
}
} // namespace world
} // namespace coop
