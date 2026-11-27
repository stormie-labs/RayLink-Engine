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
#include "coop_sync_api.h"
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
#include "coop_checkpoints.h"
#include "coop_death.h"
#include "coop_rest.h"
#include "coop_autosave.h"
#include "coop_router.h"
#include "coop_callgate.h"
#include "coop_levels.h"
#include "coop_monster_fx.h"
#include "coop_debug.h"
#include "coop_vehicles.h"
#include "coop_vehicles_client.h" // the client half's coop_status lines
#include "coop_vehicles_state.h" // the state's coop_status lines
#include "coop_vehicles_ride.h" // the ride's coop_status lines
#include "coop_progress.h"
#include "coop_player_touch.h"
#include "xrCoop/coop_ids.h"
#include "xrCoop/coop_mode.h"

#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "ui/UIInventoryUtilities.h"
#include "ai_space.h"
#include "InventoryOwner.h"
#include "string_table.h"
#include "xrServer_Objects_ALife_Monsters.h"

namespace coop
{
namespace sync
{
namespace
{
// Host: peers that are loading the world. Reliable world changes for them are queued and flushed after WorldLoaded
// (deferred, not lost and not applied too early).
xr_set<PeerId> g_joining;

void forget_joining() { g_joining.clear(); }

void forget_joining_peer(PeerId id) { g_joining.erase(id); }

// The module table: every pass of the session's modules is a list of rows, called in the order of its rows. The orders of
// the passes differ (a reset parks the cars first, a frame runs the players before the world), and each pass keeps its own.
// A new module adds its row to every pass it takes part in.
using Step = void (*)();
using PeerStep = void (*)(PeerId);
using ServerInstall = void (*)(Session&);
using ClientInstall = void (*)(Endpoint&);

// When a row of the frame runs. A pass that reads or changes the world runs only while it is UP (world_up: the level is
// loaded, the simulator and the local actor exist); one that also cleans up "no level at all" on its own runs unless a
// level is in the making (!world_gap). g_pGameLevel is set at the START of a level's construction and ai().get_alife() at
// the start of the simulator's, so neither says that the world is there. A pass that checks the world itself (a Lua frame
// asks lua_ready, story ready, requests host_world_ready) or touches none (money sends, worldstate asks alife_ready) runs
// always; entities and events drop what they recorded for a world that is not up.
enum class Gate : u8
{
	Always,
	Up, // world_up()
	NotUp, // !world_up()
	NoGap, // !world_gap()
};

struct FrameStep
{
	Gate gate;
	Step run;
};

// Once per process, from the Manager's constructor: settings, the route and call-gate tables.
const Step kInit[] = {
	&join::prepare_client_cache,
	&requests::init,
	&money::init,
	&presence::init,
	&router::install,
	&callgate::install,
};

const ServerInstall kInstallServer[] = {
	&players::install_server,
	&entities::install_server,
	&worldstate::install_server,
	&script::install_server,
	&syncapi::install_server,
	&story::install_server,
	&events::install_server,
	&requests::install_server,
	&dialog::install_server,
	&trade::install_server,
	&money::install_server,
	&survival::install_server,
	&checkpoints::install_server,
	&death::install_server,
	&rest::install_server,
	&autosave::install_server,
	&presence::install_server,
	&items::install_server,
	&objects::install_server,
	&levels::install_server,
	&monster_fx::install_server,
	&debug::install_server,
	&vehicles::install_server, // VehicleReq, VehicleInput (the decider of the level: the co-op host, a map server)
};

const ClientInstall kInstallClient[] = {
	&players::install_client,
	&entities::install_client,
	&worldstate::install_client,
	&script::install_client,
	&syncapi::install_client,
	&story::install_client,
	&events::install_client,
	&requests::install_client,
	&dialog::install_client,
	&trade::install_client,
	&money::install_client,
	&survival::install_client,
	&checkpoints::install_client,
	&death::install_client,
	&rest::install_client,
	&autosave::install_client,
	&presence::install_client,
	&items::install_client,
	&objects::install_client,
	&levels::install_client,
	&monster_fx::install_client,
	&debug::install_client, // DebugState (the host player's over the link, a map server's on its bridge)
	&vehicles::install_client, // VehicleSeat, VehicleState (a player's machine; the host player's over the link)
};

const Step kSessionStart[] = {
	&players::condition_carry_drop, // a condition kept for a load of another session is nobody's here
	&players::reset,
	&entities::reset,
	&worldstate::reset,
	&script::reset,
	&syncapi::reset, // the host drops the session scope, a client everything
	&story::reset,
	&events::reset,
	&requests::reset,
	&dialog::reset,
	&trade::reset,
	&money::on_session_started, // characters from the save keep full allowances
	&survival::reset,
	&checkpoints::reset,
	&death::reset,
	&rest::reset,
	&autosave::reset,
	&presence::reset,
	&items::reset,
	&objects::reset,
	&router::reset, // call counters
	&callgate::reset, // gated call counters, the log budget
	&levels::reset,
	&forget_joining,
	&debug::on_session_started, // on with the key -dbg of this host, else off
};

const PeerStep kPeerLeft[] = {
	&players::on_peer_left,
	&entities::on_peer_left,
	&dialog::on_peer_left,
	&trade::on_peer_left,
	&money::on_peer_left,
	&requests::on_peer_left, // his queued spawn requests
	&syncapi::on_peer_left, // his request limits
	&levels::on_peer_left,
	&debug::on_peer_left, // his personal flags go
	&net::host_drop,
	&forget_joining_peer,
};

const Step kReset[] = {
	&vehicles::session_over, // the cars clients drove parked, their proxies on their feet, the seat table forgotten
	&vehicles::ride_session_over, // a passenger seated in this world stands up, the ride is forgotten
	&npc_ai::viewers_clear, // the scheduler looks at the host's camera alone again
	&player_touch::reset, // the players' bodies collide as in single player again
	&players::reset,
	&entities::reset,
	&world::reset,
	&net::reset,
	&worldstate::reset,
	&script::reset,
	&syncapi::reset, // the host drops the session scope, a client everything
	&story::reset,
	&events::reset,
	&requests::reset,
	&dialog::reset,
	&trade::reset,
	&money::reset,
	&progress::reset, // the keeper's book of a player's progress and its counters are this session's
	&survival::reset,
	&checkpoints::reset,
	&death::reset,
	&rest::reset,
	&autosave::reset,
	&presence::reset,
	&items::reset,
	&objects::reset,
	&router::reset, // call counters
	&callgate::reset, // gated call counters, the log budget
	&levels::reset,
	&debug::reset, // the state goes, the flags this machine set on its actor are taken back
	&forget_joining,
};

const FrameStep kFrame[] = {
	// The host updates the world by its distance to the host's camera alone, so far from the host the NPCs next to a
	// client would think every 625 ms and walk at 1/6 of their speed: the other players' characters are viewers too.
	{Gate::Up, &npc_ai::viewers_frame},
	{Gate::NotUp, &npc_ai::viewers_clear},
	{Gate::Up, &players::frame},
	{Gate::Up, &player_touch::frame}, // after the avatars were placed from their snapshots
	{Gate::NoGap, &survival::frame}, // leaves the session's state when there is no level
	{Gate::Up, &checkpoints::frame},
	{Gate::NoGap, &death::frame},
	{Gate::Up, &rest::frame},
	{Gate::NoGap, &autosave::frame},
	{Gate::NoGap, &presence::frame}, // sound::frame inside: stops what plays when there is no level
	{Gate::NoGap, &items::frame},
	{Gate::Always, &entities::frame},
	{Gate::Always, &events::frame}, // after entities: a spawn goes out before its parent change
	{Gate::Always, &entities::reconcile_frame}, // after events: a digest counts what this frame told the clients
	{Gate::Up, &objects::frame},
	// every process runs the four vehicle steps; each does nothing where it keeps no state (no seat decided here, no car
	// driven here)
	{Gate::Up, &vehicles::server_frame}, // the decider: releases, stale input, seated proxies parked at their seats
	{Gate::Up, &vehicles::state_frame}, // the decider: VehicleState of the cars with a sitter or a running engine, the dolls
	{Gate::Up, &vehicles::client_frame}, // a player's machine: VehicleInput of the car he drives, the answer timeout
	{Gate::Up, &vehicles::ride_frame}, // a passenger follows his seat, after objects::frame posed the copy of the car
	{Gate::Always, &requests::frame},
	{Gate::Up, &dialog::frame},
	{Gate::Up, &trade::frame},
	{Gate::Always, &money::frame},
	{Gate::Always, &worldstate::frame},
	{Gate::Always, &script::frame},
	{Gate::Always, &syncapi::frame},
	{Gate::Always, &story::frame},
	{Gate::Up, &progress::frame}, // the checks of a player's progress where his record is kept
	{Gate::Always, [] { mode().frame(); }}, // the mode's own frame (the server's: the levels, the map servers, a map's sky)
	{Gate::Up, [] { monster_fx::frame(true); }}, // abilities that came before their monster
	{Gate::NotUp, [] { monster_fx::frame(false); }},
	{Gate::Always, &debug::frame}, // a change of the server's debug goes out
};

const Step kPrintStatus[] = {
	&net::print_status,
	&world::print_status,
	&players::print_status,
	&player_touch::print_status,
	&entities::print_status,
	&remote::print_status,
	&worldstate::print_status,
	&script::print_status,
	&syncapi::print_status,
	&router::print_status,
	&callgate::print_status,
	&levels::print_status,
	&story::print_status,
	&events::print_status,
	&requests::print_status,
	&dialog::print_status,
	&trade::print_status,
	&money::print_status,
	&survival::print_status,
	&checkpoints::print_status,
	&death::print_status,
	&rest::print_status,
	&autosave::print_status,
	&presence::print_status,
	&items::print_status,
	&objects::print_status,
	&monster_fx::print_status,
	&debug::print_status,
	&vehicles::print_status, // seats, cars driven, requests by verdict, inputs
	&vehicles::client_print_status, // this machine's half: requests, answers, inputs, its own seat
	&vehicles::state_print_status, // VehicleState sent / played, the dolls
	&vehicles::ride_print_status, // this machine's ride, the places read
	&progress::print_status, // the checks of a player's progress
};

template <typename Pass, typename... Args>
void run(const Pass& pass, Args&... args)
{
	for (const auto step : pass)
		step(args...);
}

bool open(Gate gate, bool up, bool gap)
{
	switch (gate)
	{
	case Gate::Always: return true;
	case Gate::Up: return up;
	case Gate::NotUp: return !up;
	case Gate::NoGap: return !gap;
	}
	return false;
}
} // namespace

void init() { run(kInit); }

void install_server(Session& s) { run(kInstallServer, s); }

void install_client(Endpoint& e) { run(kInstallClient, e); }

void on_session_started() { run(kSessionStart); }

void on_peer_left(PeerId id) { run(kPeerLeft, id); }

void reset() { run(kReset); }

void host_begin_join(PeerId id)
{
	g_joining.insert(id);
}

void host_end_join(PeerId id)
{
	net::host_flush(id);
	g_joining.erase(id);
	items::host_on_peer_in_world(id); // what his game said of its items while he was joining (a map server: all of its first batch)
	money::host_on_peer_in_world(id); // the balance after the queued changes, once he is live
	debug::host_on_peer_in_world(id); // the server's debug, a map server's bridge too
}

bool host_peer_joining(PeerId id) { return g_joining.count(id) != 0; }

void on_client_live()
{
	players::reset();
	entities::on_client_live();
	money::on_client_live(); // the actor's money is the host's balance
}

void frame()
{
	PROF_EVENT("coop/coop_sync/frame");
	const bool up = world_up();
	const bool gap = world_gap();
	for (const FrameStep& step : kFrame)
		if (open(step.gate, up, gap))
			step.run();
}

void print_status() { run(kPrintStatus); }

void dump_state(LPCSTR args)
{
	if (!world_up()) // a level in the making has no objects to count
	{
		::Msg("[coop-dump] not in a level");
		return;
	}
	const bool summary = args && !xr_strcmp(args, "summary");
	const bool one = args && *args && !summary;
	const u16 only = one ? u16(atoi(args)) : kNoId;
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
		    go->H_Parent() ? join::to_host(go->H_Parent()->ID()) : kNoId, p.x, p.y, p.z);
		CInventoryOwner* owner = one ? smart_cast<CInventoryOwner*>(go) : nullptr;
		if (owner)
		{
			// The shown name, the entity's display and stored name, its character and visual, the language that translated
			// it: the same line from the host and a client tells which one differs
			CSE_ALifeTraderAbstract* tr = ai().get_alife()
			    ? smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(go->ID(), true))
			    : nullptr;
			const STRING_TABLE_DATA* st = CStringTable::getPData();
			auto text = [](const shared_str& v) { return v.size() ? v.c_str() : ""; };
			::Msg("[coop-dump]       name '%s' | entity '%s' stored '%s' character [%s] profile [%s] | visual [%s] | lang %s",
			    owner->Name(), tr ? tr->m_character_name.c_str() : "-", tr ? tr->m_character_name_str.c_str() : "-",
			    tr ? text(tr->m_SpecificCharacter) : "-", tr ? text(tr->m_sCharacterProfile) : "-",
			    text(go->cNameVisual()), st ? text(st->m_sLanguage) : "?");
		}
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
	if (world::is_player_character(obj))
		return true;
	if (!mgr().active())
		return false;
	const u16 id = obj->ID();
	if (world::is_player_proxy(id))
		return true; // a player character is always driven by its player
	// On a mirror every creature belongs to the host; items, doors, physics stay local. A map server computes its level.
	return entities::is_mirrored(id) && smart_cast<const CEntityAlive*>(obj) != nullptr;
}

// The stock hook (alife_update_manager.cpp) includes coop_sync.h alone: this is its name for "a client of the world".
bool alife_frozen() { return net::client(); }
} // namespace sync
} // namespace coop
