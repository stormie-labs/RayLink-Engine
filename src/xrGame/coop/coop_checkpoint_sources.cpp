// Anomaly Coop — where a checkpoint may come from, on the host (F3 Task 7). See coop_checkpoint_sources.h; the pure
// rules (settings bounds, the claim message, the nearest source, mod objects, the book of tents and claims) are
// xrCoop/coop_checkpoint_source_rules.h, the dwell timer xrCoop/coop_checkpoint_rules.h.
#include "stdafx.h"
#include "coop_checkpoint_sources.h"
#include "coop_checkpoints.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_survival.h"
#include "coop_script.h"
#include "coop_alife.h"
#include "xrCoop/coop_checkpoint_source_rules.h"

#include "Level.h"
#include "GameObject.h"
#include "ZoneCampfire.h"
#include "space_restrictor.h"
#include "entity_alive.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "ai_space.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "game_graph.h"
#include "game_level_cross_table.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"

#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace coop
{
namespace checkpoints
{
namespace sources
{
namespace
{
using checkpoint::Kind;
using checkpoint::kNoSource;

constexpr u16 kNoId = 0xffff;
constexpr const char* kSection = "coop_checkpoints";
constexpr const char* kRestZonesSection = "coop_rest_zones";
constexpr const char* kCoverListSection = "list"; // of the file named by cover_list
constexpr const char* kRefusedCallback = "coop_checkpoint_refused";
constexpr u64 kTickMs = 500; // players against campfires and mod objects (plan F3 Task 7, step 3)
constexpr u64 kScanMs = 1000; // the level's campfires and tents, claims, lost sources (steps 2, 4, 5)
constexpr float kRestProbe = 0.3f; // the sphere tested against a rest restrictor (step 6)
constexpr size_t kMaxRestNames = 1024; // rest zones + surge covers (stock: 25 + 294)
constexpr size_t kMaxCampfires = 1024; // campfires of one level the scan keeps
// A client cannot create the tent himself, so the host does it on his word: this many tents of one player at most (a
// hacked client could otherwise fill the world at the CheckpointClaim rate). Stock has one tent item per player.
constexpr size_t kMaxPlayerTents = 8;
// A client sends a claim at most this often: the host takes 4 per 2000 ms (coop_limits.h), and a tent takes a 4 s
// animation, so one per 2 s never hits the host's limit.
constexpr u64 kClaimSpacingMs = 2000;

// ---------------------------------------------------------------- settings

struct Settings
{
	bool loaded = false;
	checkpoint::SourceSettings v;
	xr_vector<shared_str> tent_sections; // v.tent_sections, for the scan
	std::set<std::string> rest_names; // [coop_rest_zones] + [list] of cover_list
};
Settings g_set;

bool log_allowed(u64 count) { return count <= 10 || count % 100 == 0; }

float read_radius(LPCSTR key, float def)
{
	if (!pSettings->line_exist(kSection, key))
		return def;
	const float v = pSettings->r_float(kSection, key);
	if (checkpoint::radius_ok(v))
		return v;
	::Msg("! [coop] [%s] %s = %f is not a radius in (0, %.0f]: %.1f is used", kSection, key, v,
	    checkpoint::kMaxSourceRadius, def);
	return def;
}

u32 read_period(LPCSTR key, u32 def)
{
	if (!pSettings->line_exist(kSection, key))
		return def;
	const double v = double(pSettings->r_float(kSection, key));
	if (checkpoint::period_ok(v) && v == std::floor(v))
		return u32(v);
	::Msg("! [coop] [%s] %s = %f is not a whole number of ms in 0..%u: %u is used", kSection, key, v,
	    checkpoint::kMaxSourcePeriodMs, def);
	return def;
}

void add_rest_name(LPCSTR name)
{
	if (name && *name && g_set.rest_names.size() < kMaxRestNames)
		g_set.rest_names.insert(name);
}

// The stock surge covers: section [list] of the file named by cover_list (misc\surge_manager.ltx).
void read_cover_list()
{
	if (g_set.v.cover_list.empty())
		return;
	string_path path;
	FS.update_path(path, "$game_config$", g_set.v.cover_list.c_str());
	if (!FS.exist(path))
	{
		::Msg("! [coop] checkpoint sources: configs\\%s not found, the surge covers are no rest places",
		    g_set.v.cover_list.c_str());
		return;
	}
	CInifile ini(path, TRUE, TRUE, FALSE);
	if (!ini.section_exist(kCoverListSection))
	{
		::Msg("! [coop] checkpoint sources: configs\\%s has no [%s], the surge covers are no rest places",
		    g_set.v.cover_list.c_str(), kCoverListSection);
		return;
	}
	const u32 n = ini.line_count(kCoverListSection);
	for (u32 i = 0; i < n; ++i)
	{
		LPCSTR name = nullptr;
		LPCSTR value = nullptr;
		if (ini.r_line(kCoverListSection, int(i), &name, &value))
			add_rest_name(name);
	}
}

void load_settings()
{
	g_set = Settings();
	g_set.loaded = true;
	checkpoint::SourceSettings& v = g_set.v;
	if (!pSettings->section_exist(kSection))
		::Msg("! [coop] [%s] not found (configs\\mod_system_coop_checkpoints.ltx): the defaults apply", kSection);
	else
	{
		v.campfire_radius = read_radius("campfire_radius", v.campfire_radius);
		v.campfire_dwell_ms = read_period("campfire_dwell_ms", v.campfire_dwell_ms);
		v.combat_radius = read_radius("combat_radius", v.combat_radius);
		v.tent_rest_radius = read_radius("tent_rest_radius", v.tent_rest_radius);
		v.tent_claim_radius = read_radius("tent_claim_radius", v.tent_claim_radius);
		v.tent_claim_ms = read_period("tent_claim_ms", v.tent_claim_ms);
		if (pSettings->line_exist(kSection, "tent_sections"))
		{
			LPCSTR list = pSettings->r_string(kSection, "tent_sections");
			const std::vector<std::string> names = checkpoint::split_names(list ? list : "");
			if (names.empty())
				::Msg("! [coop] [%s] tent_sections names no section: the defaults are used", kSection);
			else
				v.tent_sections = names;
		}
		if (pSettings->line_exist(kSection, "cover_list"))
		{
			LPCSTR file = pSettings->r_string(kSection, "cover_list");
			v.cover_list = file ? file : "";
		}
	}
	for (const std::string& s : v.tent_sections)
		g_set.tent_sections.push_back(shared_str(s.c_str()));

	if (pSettings->section_exist(kRestZonesSection))
	{
		const u32 n = pSettings->line_count(kRestZonesSection);
		for (u32 i = 0; i < n; ++i)
		{
			LPCSTR name = nullptr;
			LPCSTR value = nullptr;
			if (pSettings->r_line(kRestZonesSection, int(i), &name, &value))
				add_rest_name(name);
		}
	}
	else
		::Msg("! [coop] [%s] not found: the stock sleep zones are no rest places", kRestZonesSection);
	read_cover_list();

	::Msg("[coop] checkpoint sources: campfire %.1f m / %u ms, fight %.1f m, tents %u section(s) (rest %.1f m, claim "
	      "%.1f m / %u ms), %u rest zone name(s)",
	    v.campfire_radius, v.campfire_dwell_ms, v.combat_radius, u32(v.tent_sections.size()), v.tent_rest_radius,
	    v.tent_claim_radius, v.tent_claim_ms, u32(g_set.rest_names.size()));
}

const checkpoint::SourceSettings& settings()
{
	if (!g_set.loaded)
		load_settings();
	return g_set.v;
}

// ---------------------------------------------------------------- state

struct State
{
	bool world = false; // the fields below belong to the world of `epoch`
	u32 epoch = 0;
	u64 next_tick_ms = 0;
	u64 next_scan_ms = 0;
	xr_vector<u16> campfires; // CZoneCampfire objects on this level (last scan)
	std::vector<checkpoint::TentSeen> tents; // tents of tent_sections on this level (last scan)
	checkpoint::TentBook book;
	xr_map<u16, checkpoint::Dwell> dwell; // by character (host numbering): the players checked at the last tick
	xr_map<u16, shared_str> rest_zones; // restrictor id -> name, from ALife when the world was served
};
State g;

// Objects mods declare as camps. Not part of the session: a mod registers when its world is loaded, which may be before
// the host starts the session (from the game menu), so they stay until another world is made (join::world_generation:
// every load or level change rebuilds the Lua state, and the mods register again).
struct Mods
{
	checkpoint::ModSources reg;
	u32 world = 0;
};
Mods g_mods;

checkpoint::ModSources& mods()
{
	if (g_mods.world != join::world_generation())
	{
		g_mods.reg.clear();
		g_mods.world = join::world_generation();
	}
	return g_mods.reg;
}

struct Client
{
	bool sent_any = false;
	u64 last_ms = 0;
};
Client g_client;

struct Stats
{
	u64 camps = 0, camps_refused = 0; // dwell done: host_set took it / refused it
	u64 claims = 0, malformed = 0, not_in_world = 0, claims_full = 0;
	u64 refused_dead = 0, refused_far = 0, refused_combat = 0, refused_limit = 0;
	u64 matched = 0, placed = 0, place_failed = 0, expired = 0, dropped_left = 0;
	u64 lost = 0, tents_over = 0;
	u64 mods_added = 0, mods_refused = 0;
	u64 sent = 0, held = 0; // client
};
Stats g_stat;

// The online object of `id` here, nullptr if it is not on this level.
CGameObject* object_here(u16 id)
{
	if (!g_pGameLevel || id == kNoId)
		return nullptr;
	CGameObject* o = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return o && !o->getDestroy() ? o : nullptr;
}

void to_array(const Fvector& v, float out[3])
{
	out[0] = v.x;
	out[1] = v.y;
	out[2] = v.z;
}

// A position a record may hold (checkpoint::Record: finite, within +-kMaxCoord).
bool pos_ok(const Fvector& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= checkpoint::kMaxCoord &&
		std::fabs(v.y) <= checkpoint::kMaxCoord && std::fabs(v.z) <= checkpoint::kMaxCoord;
}

bool alive(u16 character) { return survival::life_of(character) == survival::Life::Alive; }

bool exists(u16 id) { return id != kNoId && ai().get_alife() && ai().alife().objects().object(id, true) != nullptr; }

// The host and every player whose character is in the world now (host numbering).
xr_vector<u16> characters_in_world()
{
	xr_vector<u16> out;
	out.push_back(0);
	for (const PeerInfo& p : mgr().session().peers())
	{
		const u16 proxy = join::proxy_of_peer(p.id);
		if (proxy != kNoId && proxy != 0 && join::host_player_in_world(proxy))
			out.push_back(proxy);
	}
	return out;
}

// coop_checkpoint_refused(reason, peer) on the host; the host's Lua passes a client's on (see the header).
void refuse(u16 character, const char* reason, u64& count)
{
	++count;
	const PeerId peer = character == 0 ? kHostPeer : join::peer_of_proxy(character);
	if (log_allowed(count))
		::Msg("[coop] checkpoint sources: the tent claim of character %u is refused (%s)", character, reason);
	if (peer == kInvalidPeer)
		return; // he left meanwhile
	script::queue_callback(kRefusedCallback, {script::cb_str(reason), script::cb_int(s64(peer))});
}

// ---------------------------------------------------------------- the world

// Restrictor id -> name for the rest zones and surge covers of the served world (names are unique in ALife; a
// release build cannot find an object by name, so ALife is scanned once per world).
void build_rest_zones()
{
	g.rest_zones.clear();
	if (g_set.rest_names.empty() || !ai().get_alife())
		return;
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		if (!e)
			continue;
		LPCSTR name = e->name_replace();
		if (name && *name && g_set.rest_names.count(name))
			g.rest_zones.emplace(it.first, shared_str(name));
	}
}

// The campfires and tents on the level now; new tents enter the book.
void scan_level(u64 now_ms)
{
	g.campfires.clear();
	g.tents.clear();
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CGameObject* go = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
		if (!go || go->getDestroy())
			continue;
		if (smart_cast<CZoneCampfire*>(go))
		{
			if (g.campfires.size() < kMaxCampfires)
				g.campfires.push_back(go->ID());
			continue;
		}
		const shared_str& sect = go->cNameSect();
		for (const shared_str& t : g_set.tent_sections)
			if (sect == t)
			{
				if (g.tents.size() < checkpoint::kMaxTents)
				{
					checkpoint::TentSeen seen;
					seen.id = go->ID();
					to_array(go->Position(), seen.pos);
					g.tents.push_back(seen);
				}
				break;
			}
	}
	if (!g.book.observe(g.tents, now_ms))
	{
		++g_stat.tents_over;
		if (log_allowed(g_stat.tents_over))
			::Msg("! [coop] checkpoint sources: more than %u tents are known, the newest are not kept",
			    u32(checkpoint::kMaxTents));
	}
}

// A newly served world (a load or a level change): what belonged to the old one goes, mods register again.
void new_world(u32 epoch)
{
	g.world = true;
	g.epoch = epoch;
	g.next_tick_ms = 0;
	g.next_scan_ms = 0;
	g.campfires.clear();
	g.tents.clear();
	g.book.new_world();
	g.dwell.clear();
	build_rest_zones();
	::Msg("[coop] checkpoint sources: world epoch %u, %u rest zone(s) and surge cover(s) of %u name(s) found", epoch,
	    u32(g.rest_zones.size()), u32(g_set.rest_names.size()));
}

// False when there is no host world to look at.
bool ensure_world()
{
	if (!mgr().is_host() || !g_pGameLevel || !ai().get_alife() || !ai().get_level_graph() || !ai().get_game_graph())
		return false;
	settings();
	const u32 epoch = join::host_world_epoch();
	if (!g.world || g.epoch != epoch)
		new_world(epoch);
	return true;
}

// Lit campfires and online mod objects on this level, with their reach.
std::vector<checkpoint::SourceSpot> current_spots()
{
	std::vector<checkpoint::SourceSpot> spots;
	for (u16 id : g.campfires)
	{
		CZoneCampfire* c = smart_cast<CZoneCampfire*>(object_here(id));
		if (!c || !c->is_on())
			continue;
		checkpoint::SourceSpot s;
		s.id = id;
		s.kind = Kind::Campfire;
		to_array(c->Position(), s.pos);
		s.radius = g_set.v.campfire_radius;
		spots.push_back(s);
	}
	for (const auto& it : mods().all())
	{
		const CGameObject* o = object_here(it.first);
		if (!o)
			continue;
		checkpoint::SourceSpot s;
		s.id = it.first;
		s.kind = Kind::Mod;
		to_array(o->Position(), s.pos);
		s.radius = it.second;
		spots.push_back(s);
	}
	return spots;
}

// ---------------------------------------------------------------- campfires and mod objects (every kTickMs)

void tick_players(u64 now_ms)
{
	const std::vector<checkpoint::SourceSpot> spots = current_spots();
	xr_map<u16, checkpoint::Dwell> next;
	for (u16 c : characters_in_world())
	{
		if (!alive(c) || (c != 0 && world::is_parked(c)))
			continue; // a visit ends with death or leaving
		const CGameObject* obj = object_here(c);
		if (!obj)
			continue;
		float at[3];
		to_array(obj->Position(), at);
		const checkpoint::SourceSpot* s = checkpoint::nearest_spot(spots, at);
		checkpoint::Dwell d;
		const auto was = g.dwell.find(c);
		if (was != g.dwell.end())
			d = was->second;
		const bool fight = s && in_combat(obj); // only matters next to a source
		if (checkpoint::dwell_update(d, s ? s->id : kNoSource, fight, now_ms, g_set.v.campfire_dwell_ms))
		{
			if (host_set(c, record_here(obj, s->kind, s->id)))
				++g_stat.camps;
			else
				++g_stat.camps_refused; // host_set logs why
		}
		next.emplace(c, d);
	}
	g.dwell.swap(next);
}

// ---------------------------------------------------------------- tents, claims, lost sources (every kScanMs)

// Owners from the players' records: a Tent record makes its source his tent (after a load the book starts empty).
void owners_from_records(const xr_vector<SourceRef>& refs)
{
	for (const SourceRef& r : refs)
		if (r.kind == Kind::Tent)
			g.book.set_owner(r.source, r.character);
}

// Every source of a record, every tent known and every mod object is looked up in ALife; a gone one rolls its records
// back.
void sweep_lost(const xr_vector<SourceRef>& refs)
{
	std::set<u16> in_records;
	for (const SourceRef& r : refs)
		in_records.insert(r.source);
	std::set<u16> ids(in_records);
	for (const auto& it : mods().all())
		ids.insert(it.first);
	for (u16 id : g.book.known())
		ids.insert(id);
	for (u16 id : ids)
	{
		if (exists(id))
			continue;
		++g_stat.lost;
		if (log_allowed(g_stat.lost))
			::Msg("[coop] checkpoint sources: object %u is gone (%s%s%s)", id, in_records.count(id) ? "a camp" : "",
			    mods().has(id) ? " a mod source" : "", g.book.owner_of(id) != kNoId ? " a tent" : "");
		if (in_records.count(id))
			host_source_lost(id);
		mods().remove(id);
		g.book.forget(id);
	}
}

void on_tent_matched(const checkpoint::TentMatch& m)
{
	++g_stat.matched;
	const CGameObject* obj = object_here(m.character);
	if (!obj || !alive(m.character))
	{
		::Msg("[coop] checkpoint sources: tent %u is character %u's, but he is %s: no checkpoint", m.tent, m.character,
		    obj ? "not alive" : "not here");
		return;
	}
	if (host_set(m.character, record_here(obj, Kind::Tent, m.tent)))
		::Msg("[coop] checkpoint sources: tent %u is character %u's camp", m.tent, m.character);
}

void settle_claims(u64 now_ms)
{
	const xr_vector<u16> in_world = characters_in_world();
	// Polled, not told: sync::on_peer_left does not reach the F3 modules, and a player who only left the world (a
	// reload) must lose his claim too.
	for (u16 c : g.book.keep_claims_of(std::set<u16>(in_world.begin(), in_world.end())))
	{
		++g_stat.dropped_left;
		::Msg("[coop] checkpoint sources: the tent claim of character %u dropped, he left the world", c);
	}
	for (u16 c : g.book.expire(now_ms))
	{
		++g_stat.expired;
		if (log_allowed(g_stat.expired))
			::Msg("[coop] checkpoint sources: no new tent near the claim of character %u in %u ms", c,
			    g_set.v.tent_claim_ms);
	}
	for (const checkpoint::TentMatch& m : g.book.match(g.tents, g_set.v.tent_claim_radius, g_set.v.tent_claim_ms))
		on_tent_matched(m);
}

// A client's claim: the host puts the tent up (see the header) and makes it his camp.
void place_tent(u16 character, const CGameObject* obj, const Fvector& pos)
{
	const char* why = nullptr;
	const CLevelGraph& lg = ai().level_graph();
	u32 lv = obj->ai_location().level_vertex_id();
	if (lg.valid_vertex_position(pos))
	{
		const u32 v = lg.vertex_id(pos);
		if (lg.valid_vertex_id(v))
			lv = v; // the vertex under the tent, else the one under the player
	}
	GameGraph::_GRAPH_ID gv = obj->ai_location().game_vertex_id();
	if (lg.valid_vertex_id(lv) && ai().get_cross_table())
		gv = ai().cross_table().vertex(lv).game_vertex_id();
	LPCSTR section = g_set.tent_sections.empty() ? nullptr : g_set.tent_sections.front().c_str();
	if (!section || !pSettings->section_exist(section))
		why = "no tent section (tent_sections)";
	else if (!lg.valid_vertex_id(lv) || !ai().game_graph().valid_vertex_id(gv))
		why = "no level or game vertex there";
	CSE_Abstract* e = nullptr;
	if (!why)
	{
		e = alife_rw().spawn_item(section, pos, lv, gv, kNoId);
		if (!e)
			why = "spawn failed";
	}
	if (why)
	{
		++g_stat.place_failed;
		::Msg("! [coop] checkpoint sources: no tent for character %u at (%.1f, %.1f, %.1f): %s", character, pos.x,
		    pos.y, pos.z, why);
		return;
	}
	g.book.set_owner(e->ID, character);
	++g_stat.placed;
	::Msg("[coop] checkpoint sources: tent %u (%s) put up for character %u at (%.1f, %.1f, %.1f)", e->ID, section,
	    character, pos.x, pos.y, pos.z);
	host_set(character, record_here(obj, Kind::Tent, e->ID)); // refused only if he stands nowhere a camp can be
}

// ---------------------------------------------------------------- messages

void host_on_claim(PeerId from, Reader& rd)
{
	if (!mgr().is_host())
		return; // ClientOnly: only the host takes it
	float p[3];
	if (!checkpoint::read_claim(rd, p))
	{
		++g_stat.malformed;
		if (log_allowed(g_stat.malformed))
			::Msg("! [coop] checkpoint sources: malformed CheckpointClaim from #%u ignored", from);
		return;
	}
	const u16 proxy = join::proxy_of_peer(from); // never the packet's word
	if (proxy == kNoId || proxy == 0)
	{
		++g_stat.not_in_world;
		if (log_allowed(g_stat.not_in_world))
			::Msg("[coop] checkpoint sources: CheckpointClaim from #%u without a character ignored", from);
		return;
	}
	Fvector pos;
	pos.set(p[0], p[1], p[2]);
	host_claim_tent(proxy, pos);
}
} // namespace

// ---------------------------------------------------------------- lifecycle

const char* place_name(Place p)
{
	switch (p)
	{
	case Place::Rest: return "rest";
	case Place::Tent: return "tent";
	case Place::Campfire: return "campfire";
	case Place::Mod: return "mod";
	case Place::None:
	default: return "none";
	}
}

void install(Session& s) { s.on(Msg::CheckpointClaim, &host_on_claim); }

void host_frame(u64 now_ms)
{
	if (!ensure_world())
		return;
	if (now_ms >= g.next_scan_ms)
	{
		g.next_scan_ms = now_ms + kScanMs;
		scan_level(now_ms);
		xr_vector<SourceRef> refs;
		host_record_sources(refs);
		owners_from_records(refs);
		sweep_lost(refs);
		settle_claims(now_ms);
	}
	if (now_ms >= g.next_tick_ms)
	{
		g.next_tick_ms = now_ms + kTickMs;
		tick_players(now_ms);
	}
}

void reset()
{
	g = State();
	g_set = Settings(); // read again at the next session (a mod pack may have changed the ltx)
	g_client = Client();
	g_stat = Stats();
}

void print_status()
{
	if (mgr().is_host())
	{
		const checkpoint::SourceSettings& v = settings();
		::Msg("[coop]   checkpoint sources: campfire %.1f m / %u ms, fight %.1f m, tent rest %.1f m, claim %.1f m / %u "
		      "ms, %u tent section(s), %u rest name(s)",
		    v.campfire_radius, v.campfire_dwell_ms, v.combat_radius, v.tent_rest_radius, v.tent_claim_radius,
		    v.tent_claim_ms, u32(v.tent_sections.size()), u32(g_set.rest_names.size()));
		::Msg("[coop]   checkpoint sources: world %s (epoch %u), %u campfire(s), %u tent(s) here, %u known, %u "
		      "owned, %u claim(s) waiting, %u mod source(s), %u rest zone(s), %u player(s) timed",
		    g.world ? "served" : "none", g.epoch, u32(g.campfires.size()), u32(g.tents.size()), u32(g.book.tents()),
		    u32(g.book.owners()), u32(g.book.claims()), u32(mods().all().size()), u32(g.rest_zones.size()),
		    u32(g.dwell.size()));
		::Msg("[coop]   checkpoint sources: camps %llu (refused %llu); claims %llu: matched %llu, put up %llu (failed "
		      "%llu), expired %llu, left %llu, full %llu, malformed %llu, no character %llu; refused dead %llu, far "
		      "%llu, combat %llu, limit %llu; sources gone %llu; mods %llu (refused %llu); tents over the bound %llu",
		    g_stat.camps, g_stat.camps_refused, g_stat.claims, g_stat.matched, g_stat.placed, g_stat.place_failed,
		    g_stat.expired, g_stat.dropped_left, g_stat.claims_full, g_stat.malformed, g_stat.not_in_world,
		    g_stat.refused_dead, g_stat.refused_far, g_stat.refused_combat, g_stat.refused_limit, g_stat.lost,
		    g_stat.mods_added, g_stat.mods_refused, g_stat.tents_over);
	}
	else if (mgr().is_client())
		::Msg("[coop]   checkpoint sources: tent claims sent %llu, held back %llu", g_stat.sent, g_stat.held);
}

// ---------------------------------------------------------------- host API

bool in_combat(const CGameObject* character)
{
	if (!character || !g_pGameLevel || !mgr().is_host())
		return false;
	const float r = settings().combat_radius;
	const Fvector& at = character->Position();
	xr_vector<CObject*> near_objects;
	Level().ObjectSpace.GetNearest(near_objects, at, r, nullptr);
	for (CObject* o : near_objects)
	{
		if (!o || o->ID() == character->ID() || o->Position().distance_to(at) > r)
			continue;
		const CEntityAlive* enemy = nullptr;
		if (CAI_Stalker* st = smart_cast<CAI_Stalker*>(o))
		{
			if (!st->g_Alive() || world::is_player_character(st))
				continue; // players' characters are never the fight
			enemy = st->memory().enemy().selected();
		}
		else if (CBaseMonster* m = smart_cast<CBaseMonster*>(o))
		{
			if (!m->g_Alive())
				continue;
			enemy = m->EnemyMan.get_enemy();
		}
		if (enemy && enemy->ID() == character->ID())
			return true;
	}
	return false;
}

Place rest_place(u16 character, u16& source)
{
	source = kNoSource;
	if (!ensure_world())
		return Place::None;
	const CGameObject* obj = object_here(character);
	if (!obj)
		return Place::None;
	const Fvector& p = obj->Position();
	float at[3];
	to_array(p, at);

	Fsphere probe;
	probe.set(p, kRestProbe);
	for (const auto& it : g.rest_zones)
	{
		const CSpaceRestrictor* zone = smart_cast<CSpaceRestrictor*>(object_here(it.first));
		if (zone && zone->inside(probe))
		{
			source = it.first;
			return Place::Rest;
		}
	}

	const float tent_r2 = g_set.v.tent_rest_radius * g_set.v.tent_rest_radius;
	float best = tent_r2;
	for (const checkpoint::TentSeen& t : g.tents)
	{
		if (g.book.owner_of(t.id) != character)
			continue;
		const float dx = t.pos[0] - at[0], dy = t.pos[1] - at[1], dz = t.pos[2] - at[2];
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 <= best)
		{
			best = d2;
			source = t.id;
		}
	}
	if (source != kNoSource)
		return Place::Tent;

	const std::vector<checkpoint::SourceSpot> spots = current_spots();
	if (const checkpoint::SourceSpot* s = checkpoint::nearest_spot(spots, at))
	{
		source = s->id;
		return s->kind == Kind::Mod ? Place::Mod : Place::Campfire;
	}
	return Place::None;
}

bool host_register_source(u16 id, float radius)
{
	if (mgr().is_client() || !ai().get_alife())
		return false; // a client plays in the host's world: its camps are the host's to know
	const char* why = nullptr;
	const CSE_ALifeDynamicObject* e = id == kNoId ? nullptr : ai().alife().objects().object(id, true);
	if (!e)
		why = "no such object";
	else if (world::is_player_character(e))
		why = "a player's character";
	else if (!mods().add(id, radius))
		why = checkpoint::radius_ok(radius) ? "too many mod sources" : "not a radius";
	if (why)
	{
		++g_stat.mods_refused;
		if (log_allowed(g_stat.mods_refused))
			::Msg("! [coop] checkpoint sources: register_source(%u, %f) refused: %s", id, radius, why);
		return false;
	}
	++g_stat.mods_added;
	return true;
}

bool host_unregister_source(u16 id) { return !mgr().is_client() && mods().remove(id); }

void host_claim_tent(u16 character, const Fvector& pos)
{
	if (!ensure_world())
		return;
	++g_stat.claims;
	CGameObject* obj = object_here(character);
	const bool in_world = character == 0 || (join::peer_of_proxy(character) != kInvalidPeer &&
		join::host_player_in_world(character));
	if (!pos_ok(pos) || !obj || !in_world)
	{
		++g_stat.not_in_world;
		if (log_allowed(g_stat.not_in_world))
			::Msg("[coop] checkpoint sources: tent claim of character %u ignored (%s)", character,
			    !pos_ok(pos) ? "not a position" : "he is not in the world");
		return;
	}
	if (!alive(character))
		return refuse(character, "dead", g_stat.refused_dead);
	if (obj->Position().distance_to(pos) > g_set.v.tent_claim_radius)
		return refuse(character, "far", g_stat.refused_far);
	if (in_combat(obj))
		return refuse(character, "combat", g_stat.refused_combat);
	if (character != 0)
	{
		// F2 Task 12b (ids leased to clients) lets a client create the tent himself; then this branch goes and his
		// claim waits for his tent like the host's.
		if (g.book.owned_by(character) >= kMaxPlayerTents)
			return refuse(character, "limit", g_stat.refused_limit);
		place_tent(character, obj, pos);
		return;
	}
	float p[3];
	to_array(pos, p);
	if (!g.book.claim(character, p, mgr().now_ms(), g_set.v.tent_claim_ms))
	{
		++g_stat.claims_full;
		::Msg("! [coop] checkpoint sources: %u tent claims wait already, the one of character %u is dropped",
		    u32(checkpoint::kMaxClaims), character);
	}
}

// ---------------------------------------------------------------- any machine

bool claim_tent(const Fvector& pos)
{
	if (mgr().is_host())
	{
		host_claim_tent(0, pos);
		return true;
	}
	if (!mgr().is_live_client() || !pos_ok(pos))
		return false;
	const u64 now = mgr().now_ms();
	if (g_client.sent_any && now - g_client.last_ms < kClaimSpacingMs)
	{
		++g_stat.held;
		return false;
	}
	float p[3];
	to_array(pos, p);
	Writer w;
	checkpoint::write_claim(w, p);
	net::client_send(Msg::CheckpointClaim, w);
	g_client.sent_any = true;
	g_client.last_ms = now;
	++g_stat.sent;
	return true;
}
} // namespace sources
} // namespace checkpoints
} // namespace coop
