#include "stdafx.h"
#include "coop_sync.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_players.h"
#include "coop_entities.h"
#include "coop_net.h"
#include "coop_worldstate.h"
#include "coop_script.h"
#include "coop_story.h"
#include "coop_events.h"
#include "coop_requests.h"
#include "coop_dialog.h"
#include "coop_trade.h"
#include "coop_survival.h"
#include "coop_presence.h"
#include "coop_items.h"
#include "coop_objects.h"
#include "coop_remote.h"

#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "ui/UIInventoryUtilities.h"

namespace coop
{
namespace sync
{
namespace
{
// Host: peers that are loading the world. Reliable world changes for them are queued and
// flushed after WorldLoaded (P6: deferred, not lost and not applied too early).
xr_set<PeerId> g_joining;
} // namespace

void install(Session& s)
{
	players::install(s);
	entities::install(s);
	worldstate::install(s);
	script::install(s);
	story::install(s);
	events::install(s);
	requests::install(s);
	dialog::install(s);
	trade::install(s);
	survival::install(s);
	presence::install(s);
	items::install(s);
	objects::install(s);
}

void on_session_started()
{
	players::reset();
	entities::reset();
	worldstate::reset();
	script::reset();
	story::reset();
	events::reset();
	requests::reset();
	dialog::reset();
	trade::reset();
	survival::reset();
	presence::reset();
	items::reset();
	objects::reset();
	g_joining.clear();
}

void on_peer_joined(const PeerInfo&) {}

void on_peer_left(PeerId id)
{
	players::on_peer_left(id);
	entities::on_peer_left(id);
	dialog::on_peer_left(id);
	trade::on_peer_left(id);
	net::host_drop(id);
	g_joining.erase(id);
}

void reset()
{
	players::reset();
	entities::reset();
	world::reset();
	net::reset();
	worldstate::reset();
	script::reset();
	story::reset();
	events::reset();
	requests::reset();
	dialog::reset();
	trade::reset();
	survival::reset();
	presence::reset();
	items::reset();
	objects::reset();
	g_joining.clear();
}

void host_begin_join(PeerId id)
{
	g_joining.insert(id);
	entities::host_begin_join(id);
}

void host_end_join(PeerId id)
{
	entities::host_end_join(id);
	net::host_flush(id);
	g_joining.erase(id);
}

bool host_peer_joining(PeerId id) { return g_joining.count(id) != 0; }

void on_client_live()
{
	players::reset();
	entities::on_client_live();
}

void frame()
{
	players::frame();
	survival::frame();
	presence::frame();
	items::frame();
	entities::frame();
	events::frame(); // after entities: a spawn goes out before its parent change
	objects::frame();
	requests::frame();
	dialog::frame();
	trade::frame();
	worldstate::frame();
	script::frame();
	story::frame();
}

void print_status()
{
	world::print_status();
	players::print_status();
	entities::print_status();
	remote::print_status();
	worldstate::print_status();
	script::print_status();
	story::print_status();
	events::print_status();
	requests::print_status();
	dialog::print_status();
	trade::print_status();
	survival::print_status();
	presence::print_status();
	items::print_status();
	objects::print_status();
}

void dump_state(LPCSTR args)
{
	if (!g_pGameLevel)
	{
		::Msg("[coop-dump] not in a level");
		return;
	}
	const bool summary = args && !xr_strcmp(args, "summary");
	const bool one = args && *args && !summary;
	const u16 only = one ? u16(atoi(args)) : 0xffff;
	::Msg("[coop-dump] role %s, game time %s", phase_name(mgr().phase()), *InventoryUtilities::GetGameTimeAsString(InventoryUtilities::etpTimeToMinutes));
	u32 count = 0;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CObject* o = Level().Objects.o_get_by_iterator(i);
		CGameObject* go = smart_cast<CGameObject*>(o);
		if (!go || (one && go->ID() != only))
			continue;
		++count;
		if (summary)
			continue;
		const u16 host_id = join::to_host(go->ID());
		const Fvector& p = go->Position();
		::Msg("[coop-dump] %5u %-32s parent %5u pos [%8.2f %8.2f %8.2f]", host_id, go->cName().c_str(),
		    go->H_Parent() ? join::to_host(go->H_Parent()->ID()) : 0xffff, p.x, p.y, p.z);
	}
	::Msg("[coop-dump] %u online objects (ids in host numbering)", count);
}

bool is_remote_driven(const CGameObject* obj)
{
	if (!obj || smart_cast<const CActor*>(obj))
		return false; // the local actor is always ours
	// A player character never thinks on its own, in a session or not: outside a session (host plays
	// alone, or after the session ended) it must stay inert and unharmable, or it would wander off
	// with the player's inventory and end up dead in the next save.
	if (!xr_strcmp(obj->cNameSect(), "coop_player_avatar"))
		return true;
	if (!mgr().active())
		return false;
	const u16 id = obj->ID();
	if (world::is_player_proxy(id))
		return true; // a player character is always driven by its player
	// On a client every creature belongs to the host; items, doors, physics stay local.
	return mgr().is_client() && entities::is_mirrored(id) && smart_cast<const CEntityAlive*>(obj) != nullptr;
}

bool alife_frozen() { return mgr().is_client(); }

bool intercept_local_spawn(CSE_Abstract* entity) { return entities::intercept_local_spawn(entity); }
} // namespace sync
} // namespace coop
