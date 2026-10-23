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
#include "coop_sync_api.h" // F2 T14: coop.sync
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
#include "coop_ai.h" // scheduler viewers
#include "coop_money.h"
#include "coop_checkpoints.h" // F3
#include "coop_death.h"
#include "coop_rest.h"
#include "coop_autosave.h"
#include "coop_router.h" // F2 T10
#include "coop_callgate.h" // F2 T12
#include "coop_levels.h" // F3a

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

// F3a T4: what the old install(Session&) set up besides the handlers, once per process, in its order
void init()
{
	story::init();
	requests::init();
	money::init(); // F2 T1
	presence::init();
	router::install(); // F2 T10
	callgate::install(); // F2 T12
}

void install_server(Session& s)
{
	players::install_server(s);
	entities::install_server(s);
	worldstate::install_server(s);
	script::install_server(s);
	syncapi::install_server(s); // F2 T14
	story::install_server(s);
	events::install_server(s);
	requests::install_server(s);
	dialog::install_server(s);
	trade::install_server(s);
	money::install_server(s); // F2 T1
	survival::install_server(s);
	checkpoints::install_server(s); // F3
	death::install_server(s);
	rest::install_server(s);
	autosave::install_server(s);
	presence::install_server(s);
	items::install_server(s);
	objects::install_server(s);
	levels::install_server(s); // F3a
}

void install_client(Endpoint& e)
{
	players::install_client(e);
	entities::install_client(e);
	worldstate::install_client(e);
	script::install_client(e);
	syncapi::install_client(e); // F2 T14
	story::install_client(e);
	events::install_client(e);
	requests::install_client(e);
	dialog::install_client(e);
	trade::install_client(e);
	money::install_client(e); // F2 T1
	survival::install_client(e);
	checkpoints::install_client(e); // F3
	death::install_client(e);
	rest::install_client(e);
	autosave::install_client(e);
	presence::install_client(e);
	items::install_client(e);
	objects::install_client(e);
	levels::install_client(e); // F3a
}

void on_session_started()
{
	players::reset();
	entities::reset();
	worldstate::reset();
	script::reset();
	syncapi::reset(); // F2 T14: the host drops the session scope, a client everything
	story::reset();
	events::reset();
	requests::reset();
	dialog::reset();
	trade::reset();
	money::on_session_started(); // F2 T1 (round 2: characters from the save keep full allowances, Ruling F2-10)
	survival::reset();
	checkpoints::reset(); // F3
	death::reset();
	rest::reset();
	autosave::reset();
	presence::reset();
	items::reset();
	objects::reset();
	router::reset(); // F2 T10: call counters
	callgate::reset(); // F2 T12: gated call counters, the log budget
	levels::reset(); // F3a
	g_joining.clear();
}

void on_peer_joined(const PeerInfo&) {}

void on_peer_left(PeerId id)
{
	players::on_peer_left(id);
	entities::on_peer_left(id);
	dialog::on_peer_left(id);
	trade::on_peer_left(id);
	money::on_peer_left(id); // F2 T1
	requests::on_peer_left(id); // F2 T2: his queued spawn requests (F2-21)
	syncapi::on_peer_left(id); // F2 T14: his request limits
	levels::on_peer_left(id); // F3a
	net::host_drop(id);
	g_joining.erase(id);
}

void reset()
{
	npc_ai::viewers_clear(); // the scheduler looks at the host's camera alone again
	players::reset();
	entities::reset();
	world::reset();
	net::reset();
	worldstate::reset();
	script::reset();
	syncapi::reset(); // F2 T14: the host drops the session scope, a client everything
	story::reset();
	events::reset();
	requests::reset();
	dialog::reset();
	trade::reset();
	money::reset(); // F2 T1
	survival::reset();
	checkpoints::reset(); // F3
	death::reset();
	rest::reset();
	autosave::reset();
	presence::reset();
	items::reset();
	objects::reset();
	router::reset(); // F2 T10: call counters
	callgate::reset(); // F2 T12: gated call counters, the log budget
	levels::reset(); // F3a
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
	money::host_on_peer_in_world(id); // F2 T1 fix round 1: the balance after the queued changes, once he is live
}

bool host_peer_joining(PeerId id) { return g_joining.count(id) != 0; }

void on_client_live()
{
	players::reset();
	entities::on_client_live();
	money::on_client_live(); // F2 T1: the actor's money is the host's balance
}

void frame()
{
	// Level-change review (owner 2026-10-23, xrCoop/coop_world_gate.h; live crash 2026-10-23 20:00: a pass met a host
	// whose level was being built). g_pGameLevel is set at the START of a level's construction and ai().get_alife() at the
	// start of the simulator's, so neither says that the world is there. A pass that reads or changes the world runs
	// only while it is UP (world_up: the level is loaded, the simulator and the local actor exist); one that also
	// cleans up "no level at all" on its own runs unless a level is in the making (!world_gap). Passes below that
	// check the world themselves (a Lua frame asks lua_ready, story ready, requests host_world_ready) or touch none
	// (money sends, worldstate asks alife_ready) are called as before; entities and events drop what they recorded for a
	// world that is not up.
	const bool up = world_up();
	const bool gap = world_gap();
	// Owner, build 10: the host updates the world by its distance to the host's camera alone, so far from the host the NPCs
	// next to a client think every 625 ms and walk at 1/6 of their speed. The other players' characters are viewers too.
	if (up)
		npc_ai::viewers_frame();
	else
		npc_ai::viewers_clear();
	if (up)
		players::frame();
	if (!gap)
		survival::frame(); // leaves the session's state when there is no level
	if (up)
		checkpoints::frame(); // F3
	if (!gap)
		death::frame();
	if (up)
		rest::frame();
	if (!gap)
		autosave::frame();
	if (!gap)
		presence::frame(); // sound::frame inside: stops what plays when there is no level
	if (!gap)
		items::frame();
	entities::frame();
	events::frame(); // after entities: a spawn goes out before its parent change
	if (up)
		objects::frame();
	requests::frame();
	if (up)
		dialog::frame();
	if (up)
		trade::frame();
	money::frame(); // F2 T1
	worldstate::frame();
	script::frame();
	syncapi::frame(); // F2 T14
	story::frame();
	levels::frame(); // F3a: the registry and the map servers (T10, T9)
}

void print_status()
{
	world::print_status();
	players::print_status();
	entities::print_status();
	remote::print_status();
	worldstate::print_status();
	script::print_status();
	syncapi::print_status(); // F2 T14
	router::print_status(); // F2 T10
	callgate::print_status(); // F2 T12
	levels::print_status(); // F3a
	story::print_status();
	events::print_status();
	requests::print_status();
	dialog::print_status();
	trade::print_status();
	money::print_status(); // F2 T1
	survival::print_status();
	checkpoints::print_status(); // F3
	death::print_status();
	rest::print_status();
	autosave::print_status();
	presence::print_status();
	items::print_status();
	objects::print_status();
}

void dump_state(LPCSTR args)
{
	if (!world_up()) // level-change review: a level in the making has no objects to count
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
