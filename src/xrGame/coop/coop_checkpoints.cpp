// Anomaly Coop — per-player checkpoints on the host (F3 Task 6). See coop_checkpoints.h for the store, the message and
// the Lua / console surface; the record rules are xrCoop/coop_checkpoint_rules.h, the pure store parts (the cache and
// queue of the host, visibility, the client's mirror, the custom-data form) xrCoop/coop_checkpoint_store.h.
#include "stdafx.h"
#include "coop_checkpoints.h"
#include "coop_checkpoint_sources.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_survival.h"
#include "coop_script.h"
#include "xrCoop/coop_checkpoint_store.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "Level.h"
#include "GameObject.h"
#include "ai_space.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "game_graph.h"
#include "game_level_cross_table.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_time_manager.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <cmath>
#include <string>

namespace coop
{
namespace checkpoints
{
namespace
{
using checkpoint::Kind;
using checkpoint::Record;
using checkpoint::Slot;
using checkpoint::kNoSource;

constexpr u16 kNoId = 0xffff;
constexpr const char* kChangedCallback = "coop_checkpoint_changed";
// F3 T6 R1 I-1: how often the host reads every cached slot again, so a character that is gone or whose id another
// character got is announced as cleared (the reads are a few hundred bytes of custom data each).
constexpr u64 kSweepMs = 1000;

// Host setting (console coop_checkpoints_group): every player sees everyone's checkpoints (1) or only his own (0).
int g_group = 1;

struct Host
{
	bool loaded = false; // the store was read from the world of `epoch`
	u32 epoch = 0;
	checkpoint::HostStore store; // cache of the slots (host numbering) and the changes frame() announces
	Record arrival; // the host's own arrival point (actor custom data, key `arrival`)
	xr_set<u16> in_world; // characters whose players were in the world at the last frame (at most kMaxPlayers)
	int group_served = -1; // coop_checkpoints_group the players in the world were last served with
	u64 next_sweep_ms = 0;
	xr_set<u16> unreadable_told; // R1 M-2: characters whose unreadable record was logged in this world (<= kMaxSlots)
};
Host g_host;

// Client: what the host shares, by local id (sources stay in host numbering, get() converts them).
checkpoint::Mirror g_mirror;
u32 g_mirror_world = 0; // join::world_generation() of the world the mirror belongs to

struct Stats
{
	u64 sets = 0, refused = 0, arrivals = 0, stories = 0, rollbacks = 0;
	u64 sent = 0, reloads = 0, fixes = 0, unreadable = 0, vanished = 0;
	u64 received = 0, malformed = 0, mirror_full = 0, refusals = 0; // refusals: F3 T7 round 1
};
Stats g_stat;

bool log_allowed(u64 count) { return count <= 10 || count % 100 == 0; }

// ---------------------------------------------------------------- world helpers

// Level of a game vertex; empty if the vertex is not in the game graph.
std::string level_of(GameGraph::_GRAPH_ID gv)
{
	if (!ai().get_game_graph())
		return std::string();
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(gv))
		return std::string();
	const char* name = gg.header().level(gg.vertex(gv)->level_id()).name().c_str();
	return name ? std::string(name) : std::string();
}

// The level this machine has loaded; empty outside a level.
std::string current_level()
{
	if (!ai().get_game_graph() || !ai().get_level_graph())
		return std::string();
	const char* name = ai().game_graph().header().level(ai().level_graph().level_id()).name().c_str();
	return name ? std::string(name) : std::string();
}

u64 game_time_ms() { return ai().get_alife() ? u64(ai().alife().time_manager().game_time()) : 0; }

// The online object of a character here, nullptr if it is not in the world.
CGameObject* object_here(u16 id)
{
	if (!g_pGameLevel || id == kNoId)
		return nullptr;
	CGameObject* o = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return o && !o->getDestroy() ? o : nullptr;
}

// The server entity that carries a character's custom data: the host's actor (0) or a player character; nullptr if
// there is none (released, or never a player's).
CSE_Abstract* character_entity(u16 character)
{
	if (!ai().get_alife())
		return nullptr;
	if (character == 0)
		return ai().alife().graph().actor();
	CSE_ALifeDynamicObject* e = ai().alife().objects().object(character, true);
	return e && world::is_player_character(e) ? e : nullptr;
}

std::string custom_data(const CSE_Abstract* e) { return e->m_ini_string.size() ? e->m_ini_string.c_str() : ""; }

void set_custom_data(CSE_Abstract* e, const std::string& before, const std::string& after)
{
	if (after != before)
		e->m_ini_string = after.c_str();
}

// ---------------------------------------------------------------- storage
// The only functions that know where a slot lives (the character's custom data). F2's player scope replaces these
// (plan F3, "Временное хранилище"); the text form stays checkpoint::encode.

bool load_slot(u16 character, Slot& out)
{
	const CSE_Abstract* e = character_entity(character);
	if (!e)
		return false;
	unsigned corrupt = 0;
	out = checkpoint::slot_from_ini(custom_data(e), &corrupt);
	// R1 M-2: an unreadable line stays until the slot is written again; it is said once per character and world, not
	// on every read.
	if (corrupt && !g_host.unreadable_told.count(character) && g_host.unreadable_told.size() < checkpoint::kMaxSlots)
	{
		g_host.unreadable_told.insert(character);
		++g_stat.unreadable;
		::Msg("! [coop] checkpoints: character %u has %u unreadable record(s) in its custom data, ignored", character,
		    corrupt);
	}
	return true;
}

bool store_slot(u16 character, const Slot& s)
{
	CSE_Abstract* e = character_entity(character);
	if (!e)
		return false;
	const std::string before = custom_data(e);
	std::string text = before;
	checkpoint::slot_to_ini(text, s);
	set_custom_data(e, before, text);
	return true;
}

bool load_arrival(Record& out)
{
	const CSE_Abstract* e = character_entity(0);
	bool corrupt = false;
	if (e && checkpoint::arrival_from_ini(custom_data(e), out, &corrupt))
		return true;
	if (corrupt)
	{
		++g_stat.unreadable;
		::Msg("! [coop] checkpoints: the host's arrival point in the actor's custom data is unreadable, ignored");
	}
	return false;
}

void store_arrival(const Record& r)
{
	CSE_Abstract* e = character_entity(0);
	if (!e)
		return;
	const std::string before = custom_data(e);
	std::string text = before;
	checkpoint::arrival_to_ini(text, r);
	set_custom_data(e, before, text);
}

// ---------------------------------------------------------------- the store

// The slot of a character read again from where it is kept; the cache follows, and if what the players were told no
// longer holds (the character is gone, or its id belongs to another one now: R1 I-1) the change is queued. nullptr if
// he has no checkpoint or is no player's character.
const Slot* slot_of(u16 character)
{
	Slot s;
	if (!load_slot(character, s))
		s = Slot();
	if (g_host.store.observe(character, s))
	{
		++g_stat.vanished;
		if (log_allowed(g_stat.vanished))
			::Msg("[coop] checkpoints: the record of character %u changed under the store (%s), announced", character,
			    s.cur.valid() ? "another record" : "none any more");
	}
	return g_host.store.find(character);
}

// A new record into the character's slot, written through and queued for the players.
bool apply_set(u16 character, const Record& r)
{
	Slot s;
	if (!load_slot(character, s))
		return false;
	if (!g_host.store.find(character) && g_host.store.slots().size() >= checkpoint::kMaxSlots)
	{
		::Msg("! [coop] checkpoints: the store keeps %u characters, character %u gets none", u32(checkpoint::kMaxSlots),
		    character);
		return false;
	}
	checkpoint::set(s, r);
	return store_slot(character, s) && g_host.store.change(character, s, false);
}

// An arrival spot written before its level was loaded, checked on this level's AI map (plan F3 Task 6, step 3): on
// the map it takes the vertex under it, the height of that vertex's plane and its game vertex (R1 M-5); off the map it
// moves to `fallback` (the host's own arrival point) when there is one. True if the record changed.
bool fix_spot(Record& r, const Record* fallback)
{
	const CLevelGraph& lg = ai().level_graph();
	Fvector p;
	p.set(r.pos[0], r.pos[1], r.pos[2]);
	const u32 v = lg.valid_vertex_position(p) ? lg.vertex_id(p) : u32(-1);
	if (lg.valid_vertex_id(v))
	{
		const float y = lg.vertex_plane_y(v, p.x, p.z);
		GameGraph::_GRAPH_ID gv = r.game_vertex;
		if (ai().get_cross_table())
			gv = ai().cross_table().vertex(v).game_vertex_id();
		const bool changed = r.level_vertex != v || std::fabs(r.pos[1] - y) > 0.01f || r.game_vertex != gv;
		r.level_vertex = v;
		r.pos[1] = y;
		r.game_vertex = gv;
		return changed;
	}
	if (!fallback)
		return false;
	bool changed = r.level_vertex != fallback->level_vertex || r.game_vertex != fallback->game_vertex;
	for (int i = 0; i < 3; ++i)
	{
		changed = changed || r.pos[i] != fallback->pos[i];
		r.pos[i] = fallback->pos[i];
	}
	r.level_vertex = fallback->level_vertex;
	r.game_vertex = fallback->game_vertex;
	return changed;
}

// The arrival records of a slot on this level, fixed (see fix_spot). True if the slot changed.
bool fix_arrivals(Slot& s, const std::string& level, const Record* fallback)
{
	bool changed = false;
	for (Record* r : {&s.cur, &s.prev})
		if (r->valid() && r->kind == Kind::Arrival && r->level == level && fix_spot(*r, fallback))
			changed = true;
	return changed;
}

// Reads a character's slot of a newly served world into the cache, its arrival spots fixed first.
void load_into_cache(u16 character, const std::string& level, const Record* fallback)
{
	Slot s;
	if (!load_slot(character, s))
		return;
	if (fix_arrivals(s, level, fallback))
	{
		store_slot(character, s);
		++g_stat.fixes;
	}
	g_host.store.observe(character, s); // first met: cached, nothing announced (the players get whole lists)
}

// The store belongs to the served world: a load or a level change (join::host_world_epoch) reads it again from the
// characters' custom data. False when there is no host world to read (yet).
bool ensure_loaded()
{
	if (!mgr().is_host() || !ai().get_alife() || !ai().get_game_graph() || !ai().get_level_graph() ||
	    !ai().alife().graph().actor())
		return false;
	const u32 epoch = join::host_world_epoch();
	if (g_host.loaded && g_host.epoch == epoch)
		return true;
	g_host.loaded = true;
	g_host.epoch = epoch;
	g_host.store.clear_cache(); // changes still queued go out (the host's own arrival of the level change)
	g_host.in_world.clear(); // every player in this world gets the whole list again
	g_host.unreadable_told.clear();
	const std::string level = current_level();
	g_host.arrival = Record();
	if (load_arrival(g_host.arrival) && g_host.arrival.level == level && fix_spot(g_host.arrival, nullptr))
	{
		store_arrival(g_host.arrival); // R1 M-5: the host's own point takes the vertex under it as well
		++g_stat.fixes;
	}
	const bool host_spot = g_host.arrival.valid() && g_host.arrival.level == level &&
		ai().level_graph().valid_vertex_id(g_host.arrival.level_vertex);
	const Record* fallback = host_spot ? &g_host.arrival : nullptr;
	load_into_cache(0, level, fallback);
	for (const auto& it : ai().alife().objects().objects())
		if (it.first != 0 && it.second && world::is_player_character(it.second))
			load_into_cache(it.first, level, fallback);
	++g_stat.reloads;
	::Msg("[coop] checkpoints: %u character(s) with a checkpoint in this world (epoch %u), host arrival %s",
	    u32(g_host.store.slots().size()), epoch, g_host.arrival.valid() ? g_host.arrival.level.c_str() : "none");
	return true;
}

std::string describe(const Record& r)
{
	if (!r.valid())
		return "-";
	string256 buf;
	xr_sprintf(buf, "%s on %s [%.1f %.1f %.1f] lv %u gv %u src %d", checkpoint::kind_name(r.kind), r.level.c_str(),
	    r.pos[0], r.pos[1], r.pos[2], r.level_vertex, u32(r.game_vertex),
	    r.source == kNoSource ? -1 : int(r.source));
	return buf;
}

// ---------------------------------------------------------------- telling the players

// coop_checkpoint_changed(local_id, kind, source_local_id | -1, rollback, level | nil) on this machine. A player with
// no checkpoint (rolled back to nothing, gone, no longer shared) gets kind "none", source -1, level nil (R1 M-8).
void fire_changed(u16 local_id, bool rollback, const Record& r, u16 source_local)
{
	using namespace script;
	if (r.valid())
		queue_callback(kChangedCallback, {cb_int(local_id), cb_str(checkpoint::kind_name(r.kind)),
		    cb_int(source_local == kNoSource ? -1 : s64(source_local)), cb_bool(rollback), cb_str(r.level.c_str())});
	else
		queue_callback(kChangedCallback, {cb_int(local_id), cb_str(checkpoint::kind_name(Kind::None)), cb_int(-1),
		    cb_bool(rollback), cb_nil()});
}

void send_state(PeerId to, u16 character, bool rollback, const Record& cur)
{
	Writer w;
	checkpoint::write_state(w, character, rollback, cur);
	if (to == kInvalidPeer)
		net::host_broadcast(Msg::CheckpointState, w);
	else
		net::host_send(to, Msg::CheckpointState, w);
	++g_stat.sent;
}

// Reads every cached slot again (the cache follows; what changed under it is queued, R1 I-1).
void refresh_all()
{
	xr_vector<u16> ids;
	ids.reserve(g_host.store.slots().size());
	for (const auto& it : g_host.store.slots())
		ids.push_back(it.first);
	for (u16 id : ids)
		slot_of(id);
}

// The whole list for one player in the world (checkpoint::HostStore::view_for): what he may see, his own id cleared
// when he has none.
void send_view(u16 proxy, bool group, bool clear_others)
{
	const PeerId peer = join::peer_of_proxy(proxy);
	if (peer == kInvalidPeer)
		return;
	for (const checkpoint::Outgoing& o : g_host.store.view_for(proxy, group, clear_others))
		send_state(peer, o.character, o.rollback, o.cur);
}

void flush_outbox(bool group)
{
	for (const checkpoint::Pending& p : g_host.store.take_pending())
	{
		Record cur;
		if (const Slot* s = slot_of(p.character))
			cur = s->cur; // the state now: several changes of one frame go out as one
		if (group)
			send_state(kInvalidPeer, p.character, p.rollback, cur);
		else if (p.character != 0)
		{
			const PeerId owner = join::peer_of_proxy(p.character);
			if (owner != kInvalidPeer)
				send_state(owner, p.character, p.rollback, cur);
		}
		if (checkpoint::may_see(0, p.character, group)) // on the host, host numbering is local numbering
			fire_changed(p.character, p.rollback, cur, cur.valid() ? cur.source : kNoSource);
	}
}

void host_frame()
{
	if (!ensure_loaded())
		return;
	const bool group = g_group != 0;
	const u64 now_ms = mgr().now_ms();
	if (now_ms >= g_host.next_sweep_ms)
	{
		g_host.next_sweep_ms = now_ms + kSweepMs;
		refresh_all();
	}
	// Players who came into the world since the last frame (join::host_player_in_world turned true) get the whole
	// list; with the group view switched everyone in the world is served again.
	xr_set<u16> now;
	for (const PeerInfo& p : mgr().session().peers())
	{
		const u16 proxy = join::proxy_of_peer(p.id);
		if (proxy != kNoId && join::host_player_in_world(proxy))
			now.insert(proxy);
	}
	const bool switched = g_host.group_served != -1 && g_host.group_served != int(group);
	if (switched)
	{
		::Msg("[coop] checkpoints: the group view is %s", group ? "on" : "off");
		refresh_all();
		// R1 M-8: the host's own Lua learns it as the clients do (the others' records, or "none")
		for (const auto& it : g_host.store.slots())
			if (it.first != 0)
				fire_changed(it.first, false, group ? it.second.cur : Record(),
				    group && it.second.cur.valid() ? it.second.cur.source : kNoSource);
	}
	for (u16 proxy : now)
		if (switched || !g_host.in_world.count(proxy))
		{
			if (!switched)
				slot_of(proxy); // his own slot as it is now (his copy may hold the camp of a former owner of the id)
			send_view(proxy, group, switched && !group);
		}
	g_host.in_world.swap(now);
	g_host.group_served = int(group);
	sources::host_frame(now_ms); // F3 Task 7: campfires, tents, mod objects, lost sources; its changes go out below
	flush_outbox(group);
}

// ---------------------------------------------------------------- client

// The mirror belongs to one world: a newly loaded world starts empty (the host sends the whole list when this player
// is in it).
void client_sync_world()
{
	const u32 gen = join::world_generation();
	if (gen != g_mirror_world)
	{
		g_mirror.clear();
		g_mirror_world = gen;
	}
}

void client_on_state(PeerId from, Reader& rd)
{
	if (!mgr().is_client() || from != kHostPeer)
		return; // HostOnly: the session kicks a client that sends it
	client_sync_world();
	checkpoint::StateMsg m;
	if (!checkpoint::read_state(rd, m))
	{
		++g_stat.malformed;
		if (log_allowed(g_stat.malformed))
			::Msg("! [coop] checkpoints: malformed CheckpointState from the host ignored");
		return;
	}
	++g_stat.received;
	const u16 local = join::to_local(m.character);
	if (m.refused()) // F3 T7 round 1 (Ruling F3-5): the answer to my own claim; the mirror does not change
	{
		if (local != 0)
		{
			++g_stat.malformed;
			if (log_allowed(g_stat.malformed))
				::Msg("! [coop] checkpoints: a refused claim of character %u is not mine, ignored", local);
			return;
		}
		++g_stat.refusals;
		script::queue_callback("coop_checkpoint_refused", {script::cb_str(checkpoint::refusal_name(m.reason)),
		    script::cb_str(checkpoint::claim_what_name(m.what))});
		return;
	}
	if (!g_mirror.apply(local, m))
	{
		++g_stat.mirror_full;
		if (log_allowed(g_stat.mirror_full))
			::Msg("! [coop] checkpoints: %u checkpoints kept, the one of character %u is ignored",
			    u32(checkpoint::kMaxSlots), local);
		return;
	}
	const Record cur = m.cleared() ? Record() : m.record;
	const u16 source = cur.valid() && cur.source != kNoSource ? join::to_local(cur.source) : kNoSource;
	fire_changed(local, m.rollback(), cur, source);
}

// ---------------------------------------------------------------- console

class CCC_CoopCheckpointStatus : public IConsole_Command
{
public:
	CCC_CoopCheckpointStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { print_status(); }
	void Info(TInfo& I) override { xr_strcpy(I, "co-op: the players' checkpoints this machine knows"); }
};

class CCC_CoopCheckpointHere : public IConsole_Command
{
public:
	CCC_CoopCheckpointHere(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		if (!mgr().is_host())
		{
			::Msg("! [coop] coop_checkpoint_here: host only, in a co-op session");
			return;
		}
		const CGameObject* me = object_here(0);
		if (me && host_set(0, record_here(me, Kind::Rest, kNoSource)))
			::Msg("[coop] coop_checkpoint_here: a rest checkpoint is set where you stand");
		else
			::Msg("! [coop] coop_checkpoint_here: refused (see above)");
	}
	void Info(TInfo& I) override { xr_strcpy(I, "co-op host, debug: a rest checkpoint where you stand"); }
};
} // namespace

// ---------------------------------------------------------------- lifecycle

void install_server(Session& s)
{
	sources::install_server(s); // F3 Task 7: CheckpointClaim
}

void install_client(Endpoint& e)
{
	e.on(Msg::CheckpointState, &client_on_state);
	sources::install_client(e);
}

void reset()
{
	sources::reset();
	g_host = Host();
	g_mirror.clear();
	g_mirror_world = 0;
	g_stat = Stats();
}

void frame()
{
	if (mgr().is_host())
		host_frame();
	else if (mgr().is_client())
		client_sync_world();
}

void print_status()
{
	if (mgr().is_host())
	{
		const checkpoint::HostStore& st = g_host.store;
		::Msg("[coop]   checkpoints: %u character(s) with one, group view %s, host arrival %s", u32(st.slots().size()),
		    g_group ? "on" : "off", describe(g_host.arrival).c_str());
		::Msg("[coop]   checkpoints: sets %llu, refused %llu, arrivals %llu, story %llu, rollbacks %llu, sent %llu, "
		      "reloads %llu, arrival fixes %llu, changed under the store %llu, unreadable %llu, not kept %llu, "
		      "not announced %llu",
		    g_stat.sets, g_stat.refused, g_stat.arrivals, g_stat.stories, g_stat.rollbacks, g_stat.sent, g_stat.reloads,
		    g_stat.fixes, g_stat.vanished, g_stat.unreadable, st.not_kept(), st.not_announced());
		for (const auto& it : st.slots())
			::Msg("[coop]   checkpoint of %u: %s | prev %s", it.first, describe(it.second.cur).c_str(),
			    describe(it.second.prev).c_str());
	}
	else if (mgr().is_client())
	{
		::Msg("[coop]   checkpoints: %u known, received %llu (claims refused %llu), malformed %llu, over the bound %llu",
		    u32(g_mirror.size()), g_stat.received, g_stat.refusals, g_stat.malformed, g_stat.mirror_full);
		for (const auto& it : g_mirror.records())
			::Msg("[coop]   checkpoint of %u: %s", it.first, describe(it.second).c_str());
	}
	sources::print_status();
}

void register_console()
{
	CMD4(CCC_Integer, "coop_checkpoints_group", &g_group, 0, 1);
	CMD1(CCC_CoopCheckpointStatus, "coop_checkpoint_status");
	CMD1(CCC_CoopCheckpointHere, "coop_checkpoint_here");
}

// ---------------------------------------------------------------- host API

checkpoint::Record record_here(const CGameObject* character, Kind kind, u16 source)
{
	Record r;
	if (!character || !ai().get_level_graph() || !ai().get_game_graph())
		return r;
	const CLevelGraph& lg = ai().level_graph();
	const Fvector& p = character->Position();
	u32 lv = character->ai_location().level_vertex_id();
	if (lg.valid_vertex_position(p))
	{
		const u32 v = lg.vertex_id(p);
		if (lg.valid_vertex_id(v))
			lv = v; // where it stands now, not where its AI last looked
	}
	GameGraph::_GRAPH_ID gv = character->ai_location().game_vertex_id();
	if (!ai().game_graph().valid_vertex_id(gv) && lg.valid_vertex_id(lv) && ai().get_cross_table())
		gv = ai().cross_table().vertex(lv).game_vertex_id();
	r.level = ai().game_graph().valid_vertex_id(gv) ? level_of(gv) : current_level();
	r.pos[0] = p.x;
	r.pos[1] = p.y;
	r.pos[2] = p.z;
	r.level_vertex = lv;
	r.game_vertex = gv;
	r.kind = kind;
	r.source = source;
	r.game_time_ms = game_time_ms();
	return r;
}

bool host_set(u16 character, const Record& r)
{
	if (!ensure_loaded())
		return false;
	const char* why = nullptr;
	if (!r.valid())
		why = "not a valid record";
	else if (r.level != current_level())
		why = "not on this level";
	else if (!ai().level_graph().valid_vertex_id(r.level_vertex))
		why = "no AI map vertex there";
	else if (!ai().game_graph().valid_vertex_id(r.game_vertex))
		why = "no game graph vertex"; // R1 M-6: a transition by game vertex (Task 14) needs one
	else if (!character_entity(character))
		why = "not a player's character";
	else if (survival::life_of(character) != survival::Life::Alive)
		why = "not alive";
	else if (!apply_set(character, r))
		why = "not stored";
	if (why)
	{
		++g_stat.refused;
		if (log_allowed(g_stat.refused))
			::Msg("[coop] checkpoint of character %u refused: %s (%s)", character, why, describe(r).c_str());
		return false;
	}
	++g_stat.sets;
	if (log_allowed(g_stat.sets)) // R1 M-3: a mod may set them in a loop
		::Msg("[coop] checkpoint of character %u: %s", character, describe(r).c_str());
	return true;
}

void host_source_lost(u16 source)
{
	if (source == kNoSource || !ensure_loaded())
		return;
	xr_vector<u16> ids;
	ids.reserve(g_host.store.slots().size());
	for (const auto& it : g_host.store.slots())
		ids.push_back(it.first);
	for (u16 id : ids)
	{
		Slot s;
		if (!load_slot(id, s) || !checkpoint::source_lost(s, source))
		{
			slot_of(id); // unchanged, or the character is gone (announced as such)
			continue;
		}
		store_slot(id, s);
		g_host.store.change(id, s, true);
		++g_stat.rollbacks;
		if (log_allowed(g_stat.rollbacks))
			::Msg("[coop] checkpoint of character %u rolled back (source %u is gone): %s", id, source,
			    describe(s.cur).c_str());
	}
}

void host_set_arrival(u16 character, const Record& r)
{
	if (!ensure_loaded())
		return;
	Record a = r;
	a.kind = Kind::Arrival;
	a.source = kNoSource;
	if (!a.valid() || !apply_set(character, a))
	{
		++g_stat.refused;
		if (log_allowed(g_stat.refused))
			::Msg("! [coop] arrival point of character %u not set (%s)", character, describe(a).c_str());
		return;
	}
	if (character == 0)
	{
		g_host.arrival = a;
		store_arrival(a);
	}
	++g_stat.arrivals;
}

void host_on_level_change(const CSE_ALifeCreatureActor& actor)
{
	if (!ensure_loaded())
		return;
	Record host;
	host.level = level_of(actor.m_tGraphID);
	host.pos[0] = actor.o_Position.x;
	host.pos[1] = actor.o_Position.y;
	host.pos[2] = actor.o_Position.z;
	host.level_vertex = actor.m_tNodeID;
	host.game_vertex = actor.m_tGraphID;
	host.kind = Kind::Arrival;
	host.game_time_ms = game_time_ms();
	if (!host.valid())
	{
		::Msg("! [coop] checkpoints: no arrival points, the level change target is not readable (gv %u)",
		    u32(actor.m_tGraphID));
		return;
	}
	host_set_arrival(0, host);
	// The players in the world as of the last frame (this runs inside the server's message handling, not between
	// frames); xr_set keeps them in id order, so the spots do not depend on who connected first.
	xr_vector<u16> players;
	for (u16 proxy : g_host.in_world)
		players.push_back(proxy);
	unsigned n = 0;
	for (u16 proxy : players)
	{
		Record r = host;
		checkpoint::arrival_spot(host.pos, ++n, r.pos);
		host_set_arrival(proxy, r);
	}
	::Msg("[coop] checkpoints: arrival on %s for the host and %u player(s)", host.level.c_str(), n);
}

bool host_story(const char* reason)
{
	if (!ensure_loaded())
		return false;
	xr_vector<u16> characters;
	characters.push_back(0);
	for (u16 proxy : g_host.in_world)
		characters.push_back(proxy);
	unsigned set = 0;
	for (u16 c : characters)
	{
		if (survival::life_of(c) != survival::Life::Alive || (c != 0 && world::is_parked(c)))
			continue;
		const CGameObject* o = object_here(c);
		if (o && host_set(c, record_here(o, Kind::Story, kNoSource)))
			++set;
	}
	++g_stat.stories;
	if (log_allowed(g_stat.stories)) // R1 M-3: coop.checkpoint.story is open to every addon
		::Msg("[coop] checkpoints: story checkpoint '%.64s' for %u player(s)", reason ? reason : "", set);
	return set != 0;
}

bool host_respawn_point(u16 character, const Fvector* anchor, const Fvector& body, Fvector& out, Kind& kind)
{
	out = body;
	kind = Kind::None;
	if (!ensure_loaded())
		return false;
	const std::string level = current_level();
	const CLevelGraph& lg = ai().level_graph();
	Slot s;
	if (const Slot* have = slot_of(character))
		s = *have;
	// a record on this level whose vertex is not on its AI map is no place to come back to (a corrupt save)
	const auto usable = [&](const Record& r) { return r.valid() && (r.level != level || lg.valid_vertex_id(r.level_vertex)); };
	if (!usable(s.prev))
		s.prev = Record();
	if (!usable(s.cur))
	{
		s.cur = s.prev;
		s.prev = Record();
	}
	float a[3] = {0, 0, 0};
	if (anchor)
	{
		a[0] = anchor->x;
		a[1] = anchor->y;
		a[2] = anchor->z;
	}
	const bool arrival = g_host.arrival.valid() && g_host.arrival.level == level &&
		lg.valid_vertex_id(g_host.arrival.level_vertex);
	const float b[3] = {body.x, body.y, body.z};
	const checkpoint::Choice c = checkpoint::resolve(s, level, anchor ? a : nullptr, arrival ? g_host.arrival.pos : nullptr, b);
	out.set(c.pos[0], c.pos[1], c.pos[2]);
	switch (c.pick)
	{
	case checkpoint::Pick::Current: kind = s.cur.kind; break;
	case checkpoint::Pick::Previous: kind = s.prev.kind; break;
	case checkpoint::Pick::Arrival: kind = Kind::Arrival; break;
	case checkpoint::Pick::Anchor:
	case checkpoint::Pick::Body:
	default: kind = Kind::None; break;
	}
	return true;
}

const checkpoint::Slot* host_slot(u16 character) { return ensure_loaded() ? slot_of(character) : nullptr; }

void host_record_sources(xr_vector<SourceRef>& out)
{
	out.clear();
	if (!ensure_loaded())
		return;
	for (const auto& it : g_host.store.slots()) // the cache, read again every kSweepMs (refresh_all)
		for (const Record* r : {&it.second.cur, &it.second.prev})
			if (r->valid() && r->source != kNoSource)
			{
				SourceRef ref;
				ref.character = it.first;
				ref.source = r->source;
				ref.kind = r->kind;
				out.push_back(ref);
			}
}

// ---------------------------------------------------------------- any machine

bool get(u16 local_id, Record& out)
{
	if (local_id == kNoId)
		return false;
	if (mgr().is_host())
	{
		if (!checkpoint::may_see(0, local_id, g_group != 0))
			return false;
		// R1 M-1: read where the slot lives; the cache serves the whole lists and the announcements (frame()).
		Slot s;
		if (!ensure_loaded() || !load_slot(local_id, s) || !s.cur.valid())
			return false;
		out = s.cur;
		return true;
	}
	if (!mgr().is_client())
		return false;
	client_sync_world();
	const Record* r = g_mirror.get(local_id);
	if (!r)
		return false;
	out = *r;
	if (out.source != kNoSource)
		out.source = join::to_local(out.source);
	return true;
}
} // namespace checkpoints
} // namespace coop
