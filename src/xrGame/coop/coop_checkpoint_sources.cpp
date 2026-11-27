// Anomaly Coop — where a checkpoint may come from, on the host (F3 Task 7). See coop_checkpoint_sources.h; the pure
// rules (settings bounds, the claim message, the nearest source, mod objects, the sections behind ids, the mark of a
// tent, the item a tent is paid with, the book of tents and claims) are xrCoop/coop_checkpoint_source_rules.h, the
// claim's answer xrCoop/coop_checkpoint_store.h, the dwell timer xrCoop/coop_checkpoint_rules.h.
#include "stdafx.h"
#include "coop_checkpoint_sources.h"
#include "coop_checkpoints.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_levels.h"
#include "coop_world.h"
#include "coop_survival.h"
#include "coop_script.h"
#include "coop_alife.h"
#include "xrCoop/coop_checkpoint_source_rules.h"
#include "xrCoop/coop_ids.h"
#include "xrCoop/coop_lua_errors.h" // lua_errors::line_allowed
#include "xrCoop/coop_checkpoint_base_rules.h" // Build 57: the checkpoint at the bed of a base
#include "xrCoop/coop_ledger.h" // ledger::character_key: the owner key of a tent (R1 I-2, Ruling F2-18)

#include "Level.h"
#include "GameObject.h"
#include "ZoneCampfire.h"
#include "space_restrictor.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "inventory_item.h"
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
#include "xrServer.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h" // Build 57: CSE_ALifeTraderAbstract, a player's community
#include "relation_registry.h" // Build 57: is a base's owner his enemy (as coop_story.cpp reads relations)
#include "character_community.h"
#include "../xrServerEntities/xrMessages.h"

#include <atomic>
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
using checkpoint::ClaimWhat;
using checkpoint::Kind;
using checkpoint::Refusal;
using checkpoint::kNoSource;

constexpr const char* kSection = "coop_checkpoints";
constexpr const char* kRestZonesSection = "coop_rest_zones";
constexpr const char* kBasesSection = "coop_bases"; // Build 57 (owner 2026-11-18): the bases whose bed is the checkpoint
constexpr const char* kCoverListSection = "list"; // of the file named by cover_list
constexpr const char* kTentItemsSection = "tent_i_bag_sections"; // of the file named by item_lists (stock lists)
constexpr const char* kSleepbagItemsSection = "sleeping_bag_sections";
constexpr const char* kPlacedCallback = "coop_tent_placed";
constexpr u64 kTickMs = 500; // players against campfires and mod objects (plan F3 Task 7, step 3)
constexpr u64 kScanMs = 1000; // the level's campfires and tents, claims, lost sources (steps 2, 4, 5)
constexpr float kRestProbe = 0.3f; // the sphere tested against a rest restrictor (step 6)
constexpr size_t kMaxRestNames = 1024; // rest zones + surge covers (stock: 25 + 264; Build 57 (owner 2026-11-18): 264, not 294)
constexpr const char* kStartBasesSection = "coop_start_bases"; // Build 57 round 2: the start locations that are bases (opt-in)
constexpr u32 kFootLogs = 3; // Build 57: footing reports refused that are logged
constexpr size_t kMaxCampfires = 1024; // campfires of one level the scan keeps
constexpr size_t kMaxItemSections = 32; // items a tent / a sleeping bag may be paid with
// A player's tents and sleeping bags in the world (by his key, R1 I-2): the host puts one up for an item of his, and a
// world of permanent objects should not grow without end. Stock has one tent item per player.
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
	std::vector<std::string> tent_items; // [tent_i_bag_sections] of item_lists: what a tent is paid with
	std::vector<std::string> sleepbag_items; // [sleeping_bag_sections]
	// Build 57 (owner 2026-11-18): the bases ([coop_bases]), how long on one makes its bed the checkpoint, how far past
	// its radius he is still on it.
	std::vector<checkpoint::base::Row> bases;
	u32 base_dwell_ms = checkpoint::base::kDefaultDwellMs;
	float base_leave_margin = checkpoint::base::kLeaveMargin;
	// Build 57 (owner 2026-11-18: "for the bases that are not covered, just put the point at the place of the first spawn"):
	// the start locations file base_start_locations (of $game_config$; empty: none), the radius of their ground, and the start
	// bases (round 2: the rows of [coop_start_bases] placed by that file; ltx only: every process reads the same; the level
	// comes from the game graph per world).
	std::string start_file; // an ltx without the key: none
	float base_start_radius = checkpoint::base::kDefaultStartRadius;
	std::vector<checkpoint::base::Start> starts;
	bool unknown_owners_logged = false; // round 2: owners that are no community are logged once per session
};
Settings g_set;

constexpr u64 kLogBurst = 10; // a repeating line of this module: the first ones in full, then one in a hundred

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

u32 read_period(LPCSTR key, u32 def, u32 min_ms)
{
	if (!pSettings->line_exist(kSection, key))
		return def;
	const double v = double(pSettings->r_float(kSection, key));
	if (checkpoint::period_ok(v) && v == std::floor(v) && v >= double(min_ms))
		return u32(v);
	::Msg("! [coop] [%s] %s = %f is not a whole number of ms in %u..%u: %u is used", kSection, key, v, min_ms,
	    checkpoint::kMaxSourcePeriodMs, def);
	return def;
}

// R1 M-6: a section the host may spawn: it exists, names an entity class (F_entity_Create is FATAL without `class`)
// and makes an ALife object (spawn_item takes nothing else); `item`: an inventory item too.
bool spawnable(const std::string& section, bool item)
{
	if (!checkpoint::level_ok(section) || !pSettings->section_exist(section.c_str()) ||
	    !pSettings->line_exist(section.c_str(), "class"))
		return false;
	CSE_Abstract* e = F_entity_Create(section.c_str());
	const bool ok = e && smart_cast<CSE_ALifeDynamicObject*>(e) && (!item || smart_cast<CSE_ALifeInventoryItem*>(e));
	if (e)
		F_entity_Destroy(e);
	return ok;
}

// R1 (Ruling F3-6): what the host puts up for a client; it must spawn and be one of tent_sections (or the scan and
// rest_place would never see it). Empty: that kind is not put up for clients ("failed").
std::string read_put_up(LPCSTR key, const std::string& def)
{
	std::string s = def;
	if (pSettings->line_exist(kSection, key))
	{
		LPCSTR v = pSettings->r_string(kSection, key);
		s = v ? v : "";
	}
	if (spawnable(s, false) && checkpoint::name_listed(s, g_set.v.tent_sections))
		return s;
	::Msg("! [coop] [%s] %s = %s does not spawn or is not in tent_sections: clients get none", kSection, key,
	    s.c_str());
	return std::string();
}

void add_rest_name(LPCSTR name)
{
	if (name && *name && g_set.rest_names.size() < kMaxRestNames)
		g_set.rest_names.insert(name);
}

// A file of $game_config$ named by a setting; nullptr (logged) if it is not there.
CInifile* open_config(const std::string& file, LPCSTR what)
{
	if (file.empty())
		return nullptr;
	string_path path;
	FS.update_path(path, "$game_config$", file.c_str());
	if (!FS.exist(path))
	{
		::Msg("! [coop] checkpoint sources: configs\\%s not found, %s", file.c_str(), what);
		return nullptr;
	}
	return xr_new<CInifile>(path, TRUE, TRUE, FALSE);
}

// The names on the lines of [section] (a list section: names without values).
void read_names(CInifile& ini, LPCSTR section, std::vector<std::string>& out, size_t max)
{
	if (!ini.section_exist(section))
		return;
	const u32 n = ini.line_count(section);
	for (u32 i = 0; i < n && out.size() < max; ++i)
	{
		LPCSTR name = nullptr;
		LPCSTR value = nullptr;
		if (ini.r_line(section, int(i), &name, &value) && name && checkpoint::level_ok(name) &&
		    !checkpoint::name_listed(name, out))
			out.push_back(name);
	}
}

// The stock surge covers: section [list] of the file named by cover_list (misc\surge_manager.ltx).
void read_cover_list()
{
	CInifile* ini = open_config(g_set.v.cover_list, "the surge covers are no rest places");
	if (!ini)
		return;
	std::vector<std::string> names;
	read_names(*ini, kCoverListSection, names, kMaxRestNames);
	for (const std::string& n : names)
		add_rest_name(n.c_str());
	xr_delete(ini);
}

// R1 I-1: the items a tent / a sleeping bag is paid with — the stock TentBagSections / SleepBagSections.
void read_item_lists()
{
	CInifile* ini = open_config(g_set.v.item_lists, "the stock tent and sleeping bag items are used");
	if (ini)
	{
		read_names(*ini, kTentItemsSection, g_set.tent_items, kMaxItemSections);
		read_names(*ini, kSleepbagItemsSection, g_set.sleepbag_items, kMaxItemSections);
		xr_delete(ini);
	}
	if (g_set.tent_items.empty())
		g_set.tent_items = {"itm_tent"};
	if (g_set.sleepbag_items.empty())
		g_set.sleepbag_items = {"itm_sleepbag"};
}

// Build 57 (owner 2026-11-18): [coop_bases], one row per sleep zone that stands in a real base: <zone> = <radius>, <owner
// community>[, <owner>...] (round 2: up to 6) or none. A bad row is logged and skipped (the values go to the log as %s
// arguments, never as a format).
void read_bases()
{
	if (!pSettings->section_exist(kBasesSection))
	{
		::Msg("! [coop] [%s] not found: no base makes its bed the checkpoint", kBasesSection);
		return;
	}
	const u32 n = pSettings->line_count(kBasesSection);
	for (u32 i = 0; i < n; ++i)
	{
		LPCSTR name = nullptr;
		LPCSTR value = nullptr;
		if (!pSettings->r_line(kBasesSection, int(i), &name, &value))
			continue;
		checkpoint::base::Row row;
		if (!name || !checkpoint::base::parse_row(name, value ? value : "", row))
		{
			::Msg("! [coop] [%s] %s = %s is not '<radius>, <owner>[, <owner>...] (up to 6) or none': skipped", kBasesSection, name ? name : "?",
			    value ? value : "");
			continue;
		}
		bool known = false;
		for (const checkpoint::base::Row& r : g_set.bases)
			known = known || r.zone == row.zone;
		if (known)
			continue;
		if (g_set.bases.size() >= checkpoint::base::kMaxBases)
		{
			::Msg("! [coop] [%s] more than %u rows: the rest are not read", kBasesSection, u32(checkpoint::base::kMaxBases));
			break;
		}
		g_set.bases.push_back(row);
	}
}

// Build 57 round 2 (lead 2026-11-18, review B1): start locations are bases only by a row of [coop_start_bases] (this mod's
// ltx, in system.ltx): <start location id> = <owner community>[, <owner>...] or none; only real bases without a sleep zone are
// listed. The owners come from that row, never from the factions whose new-game lists name the place. The position comes
// from the start locations file base_start_locations (stock GAMMA: plugins\new_game_start_locations.ltx, not part of
// system.ltx; Lua reads it in coop_menu_char.script): the location's own section with lvid, gvid, x, y, z. The rows are in the
// order CInifile gives (lines sorted by name), the same on every process. A bad row, an id without a section of numbers in
// the file, a missing file: logged once (here, at the session's settings) and skipped, never asserted. The values go to the
// log as %s arguments only. An owner that is no community of the game counts as none (collect_bases logs it once).
void read_starts()
{
	namespace base = checkpoint::base;
	if (!pSettings->section_exist(kStartBasesSection))
	{
		::Msg("! [coop] [%s] not found: no start location makes a base", kStartBasesSection);
		return;
	}
	std::vector<base::StartRow> rows;
	u32 bad = 0;
	const u32 n = pSettings->line_count(kStartBasesSection);
	for (u32 i = 0; i < n; ++i)
	{
		LPCSTR id = nullptr;
		LPCSTR value = nullptr;
		if (!pSettings->r_line(kStartBasesSection, int(i), &id, &value))
			continue;
		base::StartRow row;
		if (!id || !base::parse_start_row(id, value ? value : "", row))
		{
			if (++bad <= 8)
				::Msg("! [coop] [%s] %s = %s is not '<owner>[, <owner>...] (up to 6) or none': skipped", kStartBasesSection,
				    id ? id : "?", value ? value : "");
			continue;
		}
		rows.push_back(row);
	}
	if (rows.empty())
		return;
	if (g_set.start_file.empty())
	{
		::Msg("! [coop] [%s] has %u row(s) but [%s] base_start_locations names no file: no start location makes a base",
		    kStartBasesSection, u32(rows.size()), kSection);
		return;
	}
	CInifile* ini = open_config(g_set.start_file, "no start location makes a base");
	if (!ini)
		return;
	const CInifile& in = *ini;
	std::vector<base::Start> located;
	for (const base::StartRow& row : rows)
	{
		if (located.size() >= base::kMaxStarts)
			break;
		LPCSTR keys[5] = {"lvid", "gvid", "x", "y", "z"};
		std::string v[5];
		bool have = in.section_exist(row.id.c_str()) != FALSE;
		for (int k = 0; have && k < 5; ++k)
		{
			have = in.line_exist(row.id.c_str(), keys[k]) != FALSE;
			if (have)
			{
				LPCSTR text = in.r_string(row.id.c_str(), keys[k]);
				v[k] = text ? text : "";
			}
		}
		base::Start parsed;
		if (have && base::parse_start(row.id, v[0], v[1], v[2], v[3], v[4], parsed))
			located.push_back(parsed);
	}
	xr_delete(ini);
	std::vector<std::string> missing;
	g_set.starts = base::start_bases(rows, located, missing);
	std::string names;
	for (const std::string& m : missing)
	{
		names += names.empty() ? "" : ", ";
		names += m;
	}
	if (!names.empty())
		::Msg("! [coop] [%s] row(s) with no start location of configs\\%s (no [<id>] with lvid, gvid, x, y, z as numbers, the "
		      "position within 100 km): skipped: %s",
		    kStartBasesSection, g_set.start_file.c_str(), names.c_str());
	::Msg("[coop] checkpoint sources: %u start base(s) of [%s] placed by configs\\%s, %u bad row(s) skipped",
	    u32(g_set.starts.size()), kStartBasesSection, g_set.start_file.c_str(), bad + u32(missing.size()));
}

void load_settings()
{
	g_set = Settings();
	g_set.loaded = true;
	checkpoint::SourceSettings& v = g_set.v;
	const checkpoint::SourceSettings def;
	if (!pSettings->section_exist(kSection))
		::Msg("! [coop] [%s] not found (configs\\mod_system_coop_checkpoints.ltx): the defaults apply", kSection);
	else
	{
		v.campfire_radius = read_radius("campfire_radius", v.campfire_radius);
		v.campfire_dwell_ms = read_period("campfire_dwell_ms", v.campfire_dwell_ms, 0);
		v.combat_radius = read_radius("combat_radius", v.combat_radius);
		v.tent_rest_radius = read_radius("tent_rest_radius", v.tent_rest_radius);
		v.tent_claim_radius = read_radius("tent_claim_radius", v.tent_claim_radius);
		g_set.base_dwell_ms = read_period("base_dwell_ms", g_set.base_dwell_ms, 0); // Build 57
		g_set.base_leave_margin = read_radius("base_leave_margin", g_set.base_leave_margin);
		g_set.base_start_radius = read_radius("base_start_radius", g_set.base_start_radius); // Build 57 (owner 2026-11-18)
		if (pSettings->line_exist(kSection, "base_start_locations"))
		{
			LPCSTR file = pSettings->r_string(kSection, "base_start_locations");
			g_set.start_file = file ? file : "";
		}
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
		if (pSettings->line_exist(kSection, "item_lists"))
		{
			LPCSTR file = pSettings->r_string(kSection, "item_lists");
			v.item_lists = file ? file : "";
		}
	}
	v.tent_section = read_put_up("tent_section", def.tent_section);
	v.sleepbag_section = read_put_up("sleepbag_section", def.sleepbag_section);
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
	read_item_lists();
	read_bases(); // Build 57
	read_starts(); // Build 57

	::Msg("[coop] checkpoint sources: campfire %.1f m / %u ms, fight %.1f m, tents %u section(s) (rest %.1f m, claim "
	      "%.1f m), a player gets [%s] / [%s] for %u / %u item kind(s), %u rest zone name(s), %u base(s) (%u ms, leave "
	      "+%.1f m), %u start location(s) (%.1f m)",
	    v.campfire_radius, v.campfire_dwell_ms, v.combat_radius, u32(v.tent_sections.size()), v.tent_rest_radius,
	    v.tent_claim_radius, v.tent_section.c_str(), v.sleepbag_section.c_str(),
	    u32(g_set.tent_items.size()), u32(g_set.sleepbag_items.size()), u32(g_set.rest_names.size()),
	    u32(g_set.bases.size()), g_set.base_dwell_ms, g_set.base_leave_margin, u32(g_set.starts.size()),
	    g_set.base_start_radius);
}

const checkpoint::SourceSettings& settings()
{
	if (!g_set.loaded)
		load_settings();
	return g_set.v;
}

// ---------------------------------------------------------------- state

// Build 57 (owner 2026-11-18): the owner communities of a base the game knows, in row order (empty: nobody's, never hostile).
// Round 3 (lead 2026-11-18, review B1): the first owner of a row is its holder (the faction whose guards hold the place);
// `holder` says that known[0] is that first owner (false: the row's first name is no community the game knows, there is no
// holder and only "every owner is his enemy" makes the base hostile).
struct Owners
{
	std::vector<CHARACTER_COMMUNITY_INDEX> known;
	bool holder = false;
	bool empty() const { return known.empty(); }
	size_t size() const { return known.size(); }
};

// Build 57 (owner 2026-11-18): a row of [coop_bases] found in the world: its sleep zone (the bed's object), the owners, the
// level of the zone's game vertex and its origin (for diagnostics), and, for a zone on this level, the bed point on the AI map.
struct BaseSeen
{
	u16 bed = kNoId;
	std::string name; // the row's zone
	Owners owners;
	levels::LevelId level = levels::wire::kNoLevel;
	Fvector zone_pos = {0.f, 0.f, 0.f};
	bool here = false; // on this level with a bed point: `spot`, `lv` and `gv` hold it
	checkpoint::base::Spot spot; // key, level, bed point, radius (hostile is per player: base_spots)
	u32 lv = u32(-1);
	GameGraph::_GRAPH_ID gv = GameGraph::_GRAPH_ID(-1);
};

// Build 57 (owner 2026-11-18): a start location of this world (g_set.starts with the level of its game vertex, merged as
// merge_starts says: the same on every process), its owners as communities and the name of its level.
struct StartSeen
{
	checkpoint::base::Start s;
	Owners owners;
	std::string level_name;
};

// Build 57 (owner 2026-11-18): a base of this level, as accept() built the list (zones first, then start locations): its spot
// (hostile false here; per player in base_spots), the vertices of its point, and its owners.
struct LevelBase
{
	checkpoint::base::Spot spot;
	u32 lv = u32(-1);
	GameGraph::_GRAPH_ID gv = GameGraph::_GRAPH_ID(-1);
	Owners owners;
};

struct State
{
	bool world = false; // the fields below belong to the world of `epoch`
	u32 epoch = 0;
	u64 next_tick_ms = 0;
	u64 next_scan_ms = 0;
	xr_vector<u16> campfires; // CZoneCampfire objects on this level (last scan)
	std::vector<checkpoint::TentSeen> tents; // tents of tent_sections on this level (last scan)
	checkpoint::TentBook book; // owners by key, filled from the tents' marks when the world is served (R1 I-2)
	checkpoint::SourceSections seen; // R1 M-4: the section behind each source of a record
	xr_map<u16, checkpoint::Dwell> dwell; // by character (host numbering): the players checked at the last tick
	xr_map<u16, shared_str> rest_zones; // restrictor id -> name, from ALife when the world was served
	xr_map<std::string, u16> key_char; // player key -> his character, from ALife when the world was served
	std::vector<BaseSeen> bases; // Build 57: the rows of [coop_bases] found in this world (<= kMaxBases)
	xr_map<u16, checkpoint::base::Visit> base_visit; // Build 57: by character, the players on a base at the last tick
	std::vector<StartSeen> starts; // Build 57: the start locations of this world, by index (the key of a start base)
	std::vector<LevelBase> level_bases; // Build 57: the bases of this level (accept()), zones first
	u32 dropped = 0; // Build 57: [coop_bases] rows of this level dropped as overlapping an earlier base
	u32 starts_on_ground = 0; // Build 57: start locations of this level on the ground of another base (no base of their own)
	xr_map<u16, checkpoint::base::Key> foot_current; // Build 57, a map server: by character, the base whose ground he is on
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
	// F3a T7: the host player's claim waits here for the main thread's frame (Lua may claim from the game thread; the
	// internal channel is the main thread's), then goes over the link like a client's (player_frame). R1: written before
	// g_claim_queued says it is there.
	ClaimWhat queued_what = ClaimWhat::Tent;
	float queued_pos[3] = {0, 0, 0};
};
Client g_client;
std::atomic<bool> g_claim_queued{false}; // F3a T7 R1: set after the claim's data (release), taken by player_frame (acquire)

constexpr size_t kRefusals = size_t(Refusal::Failed) + 1;

struct Stats
{
	u64 camps = 0, camps_refused = 0; // dwell done: host_set took it / refused it
	u64 claims = 0, malformed = 0, not_in_world = 0;
	u64 refused[kRefusals] = {};
	u64 placed = 0, packed = 0;
	u64 lost = 0, reused = 0, tents_over = 0;
	u64 mods_added = 0, mods_refused = 0;
	u64 sent = 0, held = 0; // client
	u64 beds = 0, beds_refused = 0; // Build 57: the bed of a base became a player's checkpoint / host_set refused it
	u64 beds_far = 0, beds_far_refused = 0; // Build 57: the same for a player on a map server's level (the footing)
	u64 feet = 0, feet_refused = 0; // Build 57, the world server: footings of a base heard from map servers / refused
	u64 feet_sent = 0; // Build 57, a map server: body reports that named a base
};
Stats g_stat;

CSE_ALifeDynamicObject* entity(u16 id)
{
	return id != kNoId && ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
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

bool is_tent_section(const shared_str& sect)
{
	for (const shared_str& t : g_set.tent_sections)
		if (sect == t)
			return true;
	return false;
}

// The ClaimWhat a tent of this section answers (0: either).
u8 what_of(const shared_str& sect)
{
	if (!g_set.v.tent_section.empty() && !xr_strcmp(sect, g_set.v.tent_section.c_str()))
		return u8(ClaimWhat::Tent);
	if (!g_set.v.sleepbag_section.empty() && !xr_strcmp(sect, g_set.v.sleepbag_section.c_str()))
		return u8(ClaimWhat::Sleepbag);
	return 0;
}

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

// R1 I-2: the key a character owns tents by — the player key digest of his custom data (ledger::character_key, as the
// money book and the spawn budget key him); the host's actor has none and is kHostOwner; a character without one is
// keyed by its id (no player of coop_join's).
std::string key_of(u16 character)
{
	if (character == 0)
		return checkpoint::kHostOwner;
	const CSE_ALifeDynamicObject* e = entity(character);
	std::string key = e && e->m_ini_string.size() ? ledger::character_key(e->m_ini_string.c_str()) : std::string();
	if (key.empty())
	{
		char id[16];
		xr_sprintf(id, "#%u", u32(character));
		key = id;
	}
	return key;
}

// The character who owns tents by `key` now: the host, a player in the session, or one whose character this world
// keeps; kNoId if none.
u16 character_of_key(const std::string& key)
{
	if (key == checkpoint::kHostOwner)
		return 0;
	for (const PeerInfo& p : mgr().session().peers())
	{
		const u16 proxy = join::proxy_of_peer(p.id);
		if (proxy != kNoId && proxy != 0 && key_of(proxy) == key)
			return proxy;
	}
	const auto it = g.key_char.find(key);
	if (it != g.key_char.end())
	{
		const CSE_ALifeDynamicObject* e = entity(it->second);
		if (e && world::is_player_character(e) && key_of(it->second) == key)
			return it->second;
	}
	return kNoId;
}

// The answer "refused" to a claim (Ruling F3-5): CheckpointState flag 4 to the claimant alone, and his engine calls the
// callback there (F3a T7: the host player's over the internal channel, coop_checkpoints client_on_state).
void refuse(u16 character, ClaimWhat what, Refusal reason)
{
	++g_stat.refused[size_t(reason)];
	if (lua_errors::line_allowed(g_stat.refused[size_t(reason)], kLogBurst))
		::Msg("[coop] checkpoint sources: the %s claim of character %u is refused (%s)",
		    checkpoint::claim_what_name(what), character, checkpoint::refusal_name(reason));
	const PeerId peer = survival::peer_of_character(character);
	if (peer == kInvalidPeer)
		return; // he left meanwhile
	Writer w;
	checkpoint::write_refusal(w, character, what, reason);
	net::host_send(peer, Msg::CheckpointState, w);
}

// The mark (owner, item) on a tent's custom data.
bool read_mark(u16 tent, checkpoint::TentMark& out)
{
	const CSE_ALifeDynamicObject* e = entity(tent);
	return e && e->m_ini_string.size() && checkpoint::tent_mark_from_ini(e->m_ini_string.c_str(), out);
}

void write_mark(CSE_Abstract* e, const checkpoint::TentMark& m)
{
	std::string text = e->m_ini_string.size() ? e->m_ini_string.c_str() : "";
	checkpoint::tent_mark_to_ini(text, m);
	e->m_ini_string = text.c_str();
}

// ---------------------------------------------------------------- the world

// The level this machine has loaded (as checkpoints::current_level); empty outside a level.
std::string level_here()
{
	if (!ai().get_game_graph() || !ai().get_level_graph())
		return std::string();
	const char* name = ai().game_graph().header().level(ai().level_graph().level_id()).name().c_str();
	return name ? std::string(name) : std::string();
}

// Build 57 (owner 2026-11-18): the level of a game vertex by name; empty if the game graph has no such vertex.
std::string level_name_of(GameGraph::_GRAPH_ID gv)
{
	if (!ai().get_game_graph() || !ai().game_graph().valid_vertex_id(gv))
		return std::string();
	const char* name = ai().game_graph().header().level(ai().game_graph().vertex(gv)->level_id()).name().c_str();
	return name ? std::string(name) : std::string();
}

// Build 57 (owner 2026-11-18): the bed point of a base's sleep zone on this level's AI map: the zone's own level vertex
// (the one the spawn compiler gave it); without one, the centre of the zone's shapes in the world (each shape's offset put
// through the zone's transform, as coop_join.cpp does with a restrictor's shapes) put on the AI grid as fix_spot puts an
// arrival spot (the vertex under it, the height of its plane). False: no vertex there (the base has no bed).
bool bed_point(CSE_ALifeDynamicObject* e, BaseSeen& b)
{
	const CLevelGraph& lg = ai().level_graph();
	u32 v = e->m_tNodeID;
	Fvector p;
	if (lg.valid_vertex_id(v))
		p = lg.vertex_position(v);
	else
	{
		Fvector c = e->o_Position;
		if (const CSE_ALifeSpaceRestrictor* zone = smart_cast<CSE_ALifeSpaceRestrictor*>(e))
		{
			// Build 57 (owner 2026-11-18, review S1): the shapes' offsets are local to the zone; a rotated zone turned them
			Fmatrix xf;
			xf.setXYZ(e->o_Angle);
			xf.c.set(e->o_Position);
			Fvector sum;
			sum.set(0.f, 0.f, 0.f);
			u32 n = 0;
			for (const CShapeData::shape_def& sh : zone->shapes)
			{
				Fvector w;
				xf.transform_tiny(w, sh.type == CShapeData::cfSphere ? sh.data.sphere.P : sh.data.box.c);
				sum.add(w);
				++n;
			}
			if (n)
			{
				sum.div(float(n));
				c = sum; // already in the world
			}
		}
		v = lg.valid_vertex_position(c) ? lg.vertex_id(c) : u32(-1);
		if (!lg.valid_vertex_id(v))
			return false;
		p.set(c.x, lg.vertex_plane_y(v, c.x, c.z), c.z);
	}
	GameGraph::_GRAPH_ID gv = e->m_tGraphID;
	if (ai().get_cross_table())
		gv = ai().cross_table().vertex(v).game_vertex_id();
	if (!pos_ok(p) || !ai().game_graph().valid_vertex_id(gv))
		return false;
	to_array(p, b.spot.pos);
	b.lv = v;
	b.gv = gv;
	return true;
}

// Build 57 (owner 2026-11-18): the point of a start location on this level's AI map: its (x, y, z) on its lvid when that is a
// vertex here, else the vertex under the position and the height of its plane (as bed_point's fallback); the game vertex of
// that vertex (the cross table), else its gvid. False: no vertex there (the start location makes no base here).
bool start_point(const checkpoint::base::Start& s, Fvector& p, u32& lv, GameGraph::_GRAPH_ID& gv)
{
	const CLevelGraph& lg = ai().level_graph();
	p.set(s.pos[0], s.pos[1], s.pos[2]);
	u32 v = s.lvid;
	if (!lg.valid_vertex_id(v))
	{
		v = lg.valid_vertex_position(p) ? lg.vertex_id(p) : u32(-1);
		if (!lg.valid_vertex_id(v))
			return false;
		p.y = lg.vertex_plane_y(v, p.x, p.z);
	}
	gv = GameGraph::_GRAPH_ID(s.gvid);
	if (ai().get_cross_table())
		gv = ai().cross_table().vertex(v).game_vertex_id();
	if (!pos_ok(p) || !ai().game_graph().valid_vertex_id(gv))
		return false;
	lv = v;
	return true;
}

// Build 57 (owner 2026-11-18): the communities of `names` the game knows (once each, in row order); the unknown ones are named
// in `unknown`. Round 3 (review B1): the holder is the row's first name when the game knows it.
Owners owners_of_names(const std::vector<std::string>& names, std::string& unknown)
{
	Owners out;
	for (size_t i = 0; i < names.size(); ++i)
	{
		const std::string& n = names[i];
		const CHARACTER_COMMUNITY_INDEX idx = CHARACTER_COMMUNITY::IdToIndex(shared_str(n.c_str()), NO_COMMUNITY_INDEX, true);
		if (idx == NO_COMMUNITY_INDEX)
		{
			unknown += unknown.empty() ? "" : ", ";
			unknown += n;
			continue;
		}
		if (i == 0)
			out.holder = true;
		bool known = false;
		for (const CHARACTER_COMMUNITY_INDEX o : out.known)
			known = known || o == idx;
		if (!known)
			out.known.push_back(idx);
	}
	return out;
}

const BaseSeen* base_of_bed(u16 bed)
{
	for (const BaseSeen& b : g.bases)
		if (b.bed == bed)
			return &b;
	return nullptr;
}

// Build 57: the start location of a start key that is a base of its own in this world (not merged); nullptr otherwise.
const StartSeen* start_of_key(checkpoint::base::Key k)
{
	const size_t i = checkpoint::base::start_of(k);
	return i < g.starts.size() && !g.starts[i].s.merged ? &g.starts[i] : nullptr;
}

// Build 57: what the log calls a base: its zone's or its start location's name.
const char* base_name(checkpoint::base::Key k)
{
	if (checkpoint::base::is_zone_key(k))
	{
		const BaseSeen* b = base_of_bed(checkpoint::base::zone_of(k));
		return b ? b->name.c_str() : "?";
	}
	const size_t i = checkpoint::base::start_of(k);
	return i < g.starts.size() ? g.starts[i].s.id.c_str() : "?";
}

// Build 57 (owner 2026-11-18): the bases of this world, read by read_world's pass (`found`: zone name -> its object). Every
// process that serves a world builds them the same way: the host and the world server decide with them; a map server builds
// the list of its level only to report on which base's ground a player stands (it decides nothing).
//   - [coop_bases] rows found in the world: their owners, the level of the zone and its position; a zone on this level gets
//     its bed point. The log names every row not in the world and every one here without a bed point.
//   - the start locations (g_set.starts) with the level of their game vertex, merged (merge_starts: the ltx and the game graph
//     only, so the keys agree on every process).
//   - the list of this level, through accept(): the zones in table order, then the start locations in list order; a later
//     zone that overlaps an earlier base is dropped (the log names both), a start location on the ground of another base makes
//     no base of its own (counted; the names of the start bases that were added are logged once per world).
void collect_bases(const xr_map<std::string, CSE_ALifeDynamicObject*>& found)
{
	namespace base = checkpoint::base;
	g.bases.clear();
	g.starts.clear();
	g.level_bases.clear();
	g.foot_current.clear();
	g.dropped = 0;
	g.starts_on_ground = 0;
	const std::string here = level_here();
	const levels::LevelId here_id =
	    ai().get_level_graph() ? levels::LevelId(ai().level_graph().level_id()) : levels::LevelId(levels::wire::kNoLevel);
	std::string missing, no_bed, unknown;
	u32 on_level = 0;
	for (const base::Row& row : g_set.bases)
	{
		const auto it = found.find(row.zone);
		if (it == found.end())
		{
			missing += missing.empty() ? "" : ", ";
			missing += row.zone;
			continue;
		}
		CSE_ALifeDynamicObject* e = it->second;
		BaseSeen b;
		b.bed = e->ID;
		b.name = row.zone;
		b.spot.id = base::zone_key(e->ID);
		b.spot.bed = e->ID;
		b.spot.radius = row.radius;
		b.zone_pos = e->o_Position;
		b.level = levels::level_of_vertex(e->m_tGraphID);
		b.owners = owners_of_names(row.owners, unknown); // round 2: up to 6; none: empty
		if (b.level != levels::wire::kNoLevel && b.level == here_id)
		{
			++on_level;
			b.spot.level = here;
			b.here = bed_point(e, b);
			if (!b.here)
			{
				no_bed += no_bed.empty() ? "" : ", ";
				no_bed += row.zone;
			}
		}
		g.bases.push_back(b);
	}

	std::vector<base::Start> list = g_set.starts;
	std::string no_level;
	for (base::Start& s : list)
	{
		const GameGraph::_GRAPH_ID gv = GameGraph::_GRAPH_ID(s.gvid);
		s.level = ai().game_graph().valid_vertex_id(gv) ? u16(ai().game_graph().vertex(gv)->level_id()) : base::kNoLevel;
		if (s.level == base::kNoLevel)
		{
			no_level += no_level.empty() ? "" : ", ";
			no_level += s.id;
		}
	}
	base::merge_starts(list, g_set.base_start_radius, g_set.base_leave_margin);
	u32 merged = 0;
	for (const base::Start& s : list)
	{
		StartSeen st;
		st.s = s;
		st.owners = owners_of_names(s.owners, unknown);
		st.level_name = s.level == base::kNoLevel ? std::string() : level_name_of(GameGraph::_GRAPH_ID(s.gvid));
		merged += s.merged ? 1 : 0;
		g.starts.push_back(st);
	}

	// the list of this level
	std::vector<base::Spot> accepted;
	for (const BaseSeen& b : g.bases)
	{
		if (!b.here)
			continue;
		const size_t r = base::accept(accepted, b.spot, g_set.base_leave_margin);
		if (r == base::kAdded)
		{
			LevelBase lb;
			lb.spot = b.spot;
			lb.lv = b.lv;
			lb.gv = b.gv;
			lb.owners = b.owners;
			g.level_bases.push_back(lb);
		}
		else if (r < accepted.size())
		{
			++g.dropped;
			::Msg("[coop] checkpoint sources: base zone %s of [%s] is on the ground of base %s: dropped (one base, one record)",
			    b.name.c_str(), kBasesSection, base_name(accepted[r].id));
		}
	}
	std::string added, no_point, on_ground;
	for (size_t i = 0; i < g.starts.size(); ++i)
	{
		const StartSeen& st = g.starts[i];
		if (st.s.merged || st.s.level == base::kNoLevel || levels::LevelId(st.s.level) != here_id)
			continue;
		Fvector p;
		u32 lv = u32(-1);
		GameGraph::_GRAPH_ID gv = GameGraph::_GRAPH_ID(-1);
		if (!start_point(st.s, p, lv, gv))
		{
			no_point += no_point.empty() ? "" : ", ";
			no_point += st.s.id;
			continue;
		}
		base::Spot sp;
		sp.id = base::start_key(i);
		sp.bed = kNoSource;
		sp.level = here;
		to_array(p, sp.pos);
		sp.radius = g_set.base_start_radius;
		const size_t r = base::accept(accepted, sp, g_set.base_leave_margin);
		if (r == base::kAdded)
		{
			LevelBase lb;
			lb.spot = sp;
			lb.lv = lv;
			lb.gv = gv;
			lb.owners = st.owners;
			g.level_bases.push_back(lb);
			added += added.empty() ? "" : ", ";
			added += st.s.id;
		}
		else if (r < accepted.size())
		{
			++g.starts_on_ground;
			// round 2: the log names them (the lead's list of [coop_start_bases] is checked against it on the stand)
			on_ground += on_ground.empty() ? "" : ", ";
			on_ground += st.s.id;
			on_ground += " (";
			on_ground += base_name(accepted[r].id);
			on_ground += ")";
		}
	}

	::Msg("[coop] checkpoint sources: %u of %u base(s) of [%s] in this world, %u on this level (%u dropped as overlapping); %u "
	      "start location(s), %u merged into another; %u base(s) on this level, %u start location(s) here on another base's "
	      "ground",
	    u32(g.bases.size()), u32(g_set.bases.size()), kBasesSection, on_level, g.dropped, u32(g.starts.size()), merged,
	    u32(g.level_bases.size()), g.starts_on_ground);
	if (!added.empty())
		::Msg("[coop] checkpoint sources: start location(s) that are bases of their own on this level: %s", added.c_str());
	if (!on_ground.empty())
		::Msg("[coop] checkpoint sources: start location(s) of [%s] on the ground of another base of this level (no base of "
		      "their own): %s",
		    kStartBasesSection, on_ground.c_str());
	if (!missing.empty())
		::Msg("! [coop] checkpoint sources: base zone(s) of [%s] not in this world (no bed there): %s", kBasesSection,
		    missing.c_str());
	if (!no_bed.empty())
		::Msg("! [coop] checkpoint sources: base zone(s) on this level without an AI map vertex (no bed there): %s",
		    no_bed.c_str());
	if (!no_point.empty())
		::Msg("! [coop] checkpoint sources: start location(s) on this level without an AI map vertex (no base there): %s",
		    no_point.c_str());
	if (!no_level.empty())
		::Msg("! [coop] checkpoint sources: start location(s) whose gvid is no game vertex (no base anywhere): %s",
		    no_level.c_str());
	if (!unknown.empty() && !g_set.unknown_owners_logged)
	{
		g_set.unknown_owners_logged = true; // round 2: once per session
		::Msg("! [coop] checkpoint sources: owner(s) of bases that are no community of this game (not counted; a base of "
		      "no known owner is never hostile): %s",
		    unknown.c_str());
	}
}


// One pass over ALife for a newly served world: the rest zones and surge covers (a release build cannot find an object
// by name), every tent with its owner from its mark (R1 I-2: owners outlast loads and level changes; every tent that
// stands now is old, R1 M-5), the player characters by key (tent_owner) and (Build 57) the sleep zones of the bases.
void read_world(u64 now_ms)
{
	g.rest_zones.clear();
	g.key_char.clear();
	std::vector<checkpoint::TentSeen> standing;
	xr_map<std::string, CSE_ALifeDynamicObject*> base_zones; // Build 57: [coop_bases] zone name -> its object
	std::set<std::string> base_names;
	for (const checkpoint::base::Row& row : g_set.bases)
		base_names.insert(row.zone);
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		if (!e)
			continue;
		LPCSTR name = e->name_replace();
		if (name && *name && g_set.rest_names.count(name))
			g.rest_zones.emplace(it.first, shared_str(name));
		if (name && *name && base_names.count(name) && !base_zones.count(name))
			base_zones.emplace(std::string(name), it.second);
		if (is_tent_section(e->s_name))
		{
			checkpoint::TentSeen t;
			t.id = it.first;
			to_array(e->o_Position, t.pos);
			t.what = what_of(e->s_name);
			standing.push_back(t);
			checkpoint::TentMark m;
			if (e->m_ini_string.size() && checkpoint::tent_mark_from_ini(e->m_ini_string.c_str(), m))
				g.book.set_owner(it.first, m.owner);
		}
		else if (it.first != 0 && world::is_player_character(e))
			g.key_char[key_of(it.first)] = it.first;
	}
	if (!g.book.observe(standing, now_ms)) // the first observe of the world: all of them old
		::Msg("! [coop] checkpoint sources: more than %u tents in the world, the rest are not kept",
		    u32(checkpoint::kMaxTents));
	collect_bases(base_zones); // Build 57
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
		if (is_tent_section(go->cNameSect()) && g.tents.size() < checkpoint::kMaxTents)
		{
			checkpoint::TentSeen seen;
			seen.id = go->ID();
			to_array(go->Position(), seen.pos);
			seen.what = what_of(go->cNameSect());
			g.tents.push_back(seen);
		}
	}
	if (!g.book.observe(g.tents, now_ms))
	{
		++g_stat.tents_over;
		if (lua_errors::line_allowed(g_stat.tents_over, kLogBurst))
			::Msg("! [coop] checkpoint sources: more than %u tents are known, the newest are not kept",
			    u32(checkpoint::kMaxTents));
	}
}

// A newly served world (a load or a level change): what belonged to the old one goes, its tents and owners are read.
void new_world(u32 epoch, u64 now_ms)
{
	g.world = true;
	g.epoch = epoch;
	g.next_tick_ms = 0;
	g.next_scan_ms = 0;
	g.campfires.clear();
	g.tents.clear();
	g.book.new_world();
	g.seen.clear();
	g.dwell.clear();
	g.base_visit.clear(); // Build 57
	read_world(now_ms);
	::Msg("[coop] checkpoint sources: world epoch %u, %u rest zone(s) and surge cover(s) of %u name(s), %u tent(s), %u "
	      "of them owned",
	    epoch, u32(g.rest_zones.size()), u32(g_set.rest_names.size()), u32(g.book.tents()), u32(g.book.owners()));
}

// False when there is no host world to look at.
bool ensure_world()
{
	if ((!mode().decides_world() && !mode().leads_level()) || !g_pGameLevel || !ai().get_alife() ||
	    !ai().get_level_graph() || !ai().get_game_graph())
		return false;
	settings();
	const u32 epoch = join::host_world_epoch();
	if (!g.world || g.epoch != epoch)
		new_world(epoch, mgr().now_ms());
	return true;
}

// Lit campfires and online mod objects on this level, with their reach.
std::vector<checkpoint::SourceSpot> current_spots()
{
	std::vector<checkpoint::SourceSpot> spots;
	for (u16 id : g.campfires)
	{
		CZoneCampfire* c = smart_cast<CZoneCampfire*>(world::object_here(id));
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
		const CGameObject* o = world::object_here(it.first);
		if (!o)
			continue;
		checkpoint::SourceSpot s;
		s.id = it.first;
		s.kind = Kind::Mod;
		to_array(o->Position(), s.pos);
		s.radius = it.second.radius;
		spots.push_back(s);
	}
	return spots;
}

// ---------------------------------------------------------------- bases (Build 57, owner 2026-11-18)

// The community `character` is judged by: his server entity's (under a G.A.M.M.A. disguise that is the disguise's: the base is
// judged by it). Round 3 (lead 2026-11-18, review S4): as it is, the actor_<f> index, the column the guards judge an actor by
// (game_relations.ltx has its own actor_* columns, and dynamic relations may change them alone); the NPC community <f> only when
// the relation table has no row for that index (checkpoint::base::judged_community); NO_COMMUNITY_INDEX when it cannot be told.
CHARACTER_COMMUNITY_INDEX community_of(u16 character)
{
	const CSE_ALifeTraderAbstract* t = smart_cast<CSE_ALifeTraderAbstract*>(entity(character));
	if (!t || t->m_community_index == NO_COMMUNITY_INDEX)
		return NO_COMMUNITY_INDEX;
	const int rows = int(CHARACTER_COMMUNITY::GetMaxIndex()) + 1; // the relation table is square over the communities
	const CHARACTER_COMMUNITY_INDEX own = t->m_community_index;
	CHARACTER_COMMUNITY_INDEX npc = NO_COMMUNITY_INDEX;
	if (!(own >= 0 && own < rows))
	{
		const shared_str id = CHARACTER_COMMUNITY::IndexToId(own, shared_str(), true);
		const std::string faction = checkpoint::base::true_faction(id.size() ? id.c_str() : "");
		if (!faction.empty())
			npc = CHARACTER_COMMUNITY::IdToIndex(shared_str(faction.c_str()), NO_COMMUNITY_INDEX, true);
	}
	const int judged = checkpoint::base::judged_community(int(own), int(npc), rows);
	return judged < 0 ? NO_COMMUNITY_INDEX : CHARACTER_COMMUNITY_INDEX(judged);
}

// A base of `owners` is hostile to `character` now (checkpoint::base::base_hostile, round 3, review B1): its holder (the first
// owner of its row) is his enemy, or every owner is; enemy by the game's own sum (checkpoint::base::hostile: the owner
// community's relation to his community plus its goodwill to him, what relation_registry.community_relation +
// community_goodwill give the Lua, coop_task_audience.script hostile()). A base of nobody, or a character whose community
// cannot be told, is never hostile. The world server is the authority of both the relations (RELATION_REGISTRY) and his server
// entity, so this holds for a player on a map server's level as well. Round 2 (compile review S3): `mine` is
// community_of(character), worked out once per player per tick by the caller, not once per base.
bool hostile_to(CHARACTER_COMMUNITY_INDEX mine, u16 character, const Owners& owners)
{
	if (owners.empty() || mine == NO_COMMUNITY_INDEX)
		return false;
	size_t hostile = 0;
	bool holder_hostile = false;
	for (size_t i = 0; i < owners.known.size(); ++i)
	{
		const CHARACTER_COMMUNITY_INDEX owner = owners.known[i];
		const bool h = checkpoint::base::hostile(true, int(RELATION_REGISTRY().GetCommunityRelation(owner, mine)),
		    int(RELATION_REGISTRY().GetCommunityGoodwill(owner, character)));
		hostile += h ? 1 : 0;
		holder_hostile = holder_hostile || (i == 0 && owners.holder && h);
	}
	return checkpoint::base::base_hostile(holder_hostile, owners.size(), hostile);
}

// The bases of this level as `character` sees them now (the hostile ones marked).
std::vector<checkpoint::base::Spot> base_spots(u16 character)
{
	std::vector<checkpoint::base::Spot> out;
	out.reserve(g.level_bases.size());
	const CHARACTER_COMMUNITY_INDEX mine = community_of(character); // round 2: once, not per base
	for (const LevelBase& lb : g.level_bases)
	{
		checkpoint::base::Spot s = lb.spot;
		s.hostile = hostile_to(mine, character, lb.owners);
		out.push_back(s);
	}
	return out;
}

const LevelBase* level_base(checkpoint::base::Key k)
{
	for (const LevelBase& lb : g.level_bases)
		if (lb.spot.id == k)
			return &lb;
	return nullptr;
}

// The record of base `lb` of this level for the online character `obj`: kind Rest, the sleep zone as its object (none for a
// start base), the base's point and vertices (the level and the game time as record_here gives them).
bool bed_record(const CGameObject* obj, const LevelBase& lb, checkpoint::Record& out)
{
	checkpoint::Record r = record_here(obj, Kind::Rest, lb.spot.bed);
	if (!r.valid())
		return false;
	r.level = lb.spot.level;
	for (int i = 0; i < 3; ++i)
		r.pos[i] = lb.spot.pos[i];
	r.level_vertex = lb.lv;
	r.game_vertex = lb.gv;
	if (!r.valid())
		return false;
	out = r;
	return true;
}

// `cur` is his own tent standing on the base of `spot` (he chose it: the base does not take it over). The tents scanned on this
// level first; Build 57 (review S1): a tent just put up is not online or scanned yet (place_for spawns it offline and sets the
// camp at once), or it stands on another level (a player on a map server's level): its ALife entity says where it stands. The
// level is the record's own.
bool own_tent_on(const checkpoint::Record& cur, const checkpoint::base::Spot& spot)
{
	if (!cur.valid() || cur.kind != Kind::Tent || cur.source == kNoSource)
		return false;
	for (const checkpoint::TentSeen& t : g.tents)
		if (t.id == cur.source)
			return checkpoint::base::stands_on(spot, cur.level, t.pos);
	if (const CSE_ALifeDynamicObject* e = entity(cur.source))
		if (is_tent_section(e->s_name))
		{
			float p[3];
			to_array(e->o_Position, p);
			return checkpoint::base::stands_on(spot, cur.level, p);
		}
	return false;
}

// One tick of the base rule for the online character `c` at `at` (the co-op path: his character is an object of this process,
// which has his level's AI map): true while he is on a friendly base (its campfires and mod objects are no sources for him
// then), and its record becomes his checkpoint after base_dwell_ms (checkpoint::base::step).
bool base_tick(u16 c, const CGameObject* obj, const std::string& here, const float at[3], checkpoint::base::Visit& v,
    u64 now_ms)
{
	namespace base = checkpoint::base;
	const base::Key k =
	    g.level_bases.empty() ? base::kNoBase : base::base_at(base_spots(c), here, at, v.base, g_set.base_leave_margin);
	const LevelBase* lb = base::is_base_key(k) ? level_base(k) : nullptr;
	bool settled = false;
	if (lb)
	{
		const checkpoint::Slot* slot = host_slot(c);
		const checkpoint::Record cur = slot ? slot->cur : checkpoint::Record();
		settled = base::is_base_record(cur, lb->spot) || own_tent_on(cur, lb->spot);
	}
	const bool fight = lb && !settled && in_combat(obj);
	const base::Step st = base::step(v, lb ? k : base::kNoBase, settled, fight, now_ms, g_set.base_dwell_ms);
	if (st == base::Step::Outside)
		return false;
	if (st == base::Step::SetBed)
	{
		checkpoint::Record r;
		if (!bed_record(obj, *lb, r))
		{
			// Log failures here because host_set only runs when bed_record succeeds.
			++g_stat.beds_refused;
			if (lua_errors::line_allowed(g_stat.beds_refused, kLogBurst))
				::Msg("! [coop] checkpoint sources: character %u on base %s: no record of it (he stands where no record of "
				      "this level can be made)",
				    c, base_name(k));
			base::retry_visit(v, now_ms); // round 2 (review S4): tried again after another dwell
		}
		else if (host_set(c, r, false))
		{
			++g_stat.beds;
			if (lua_errors::line_allowed(g_stat.beds, kLogBurst))
				::Msg("[coop] checkpoint sources: character %u is on base %s: its %s is his checkpoint", c, base_name(k),
				    lb->spot.bed != kNoSource ? "bed" : "point");
		}
		else
		{
			++g_stat.beds_refused; // host_set logs why
			base::retry_visit(v, now_ms); // round 2 (review S4)
		}
	}
	return true;
}

// Build 57 (owner 2026-11-18), the server mode (a player whose character is an object of a map server): a base of this world
// that the map server of his level reported (the footing, checked by host_footing when it came): the spot from this
// process's own tables (the key, the object of its record, the level, the radius, the owners) with the reported point and
// vertices. Round 2 (review S1): a footing whose game vertex is not on the level of his entity now is no base
// (checkpoint::base::footing_on_level).
struct FarBase
{
	checkpoint::base::Spot spot;
	u32 lv = u32(-1);
	u16 gv = 0xffff;
	const Owners* owners = nullptr;
};

bool far_base(u16 character, const checkpoint::base::Footing& f, FarBase& out)
{
	namespace base = checkpoint::base;
	const base::Key k = base::key_of(f);
	if (!base::is_base_key(k))
		return false;
	const CSE_ALifeDynamicObject* me = entity(character);
	if (!me || !base::footing_on_level(levels::level_of_vertex(f.gv), levels::level_of_vertex(me->m_tGraphID)))
		return false;
	FarBase fb;
	fb.spot.id = k;
	if (base::is_zone_key(k))
	{
		const BaseSeen* b = base_of_bed(base::zone_of(k));
		const char* name = b ? levels::level_name(b->level) : nullptr;
		if (!b || !name)
			return false;
		fb.spot.bed = b->bed;
		fb.spot.radius = b->spot.radius;
		fb.spot.level = name;
		fb.owners = &b->owners;
	}
	else
	{
		const StartSeen* s = start_of_key(k);
		if (!s || s->level_name.empty())
			return false;
		fb.spot.bed = kNoSource;
		fb.spot.radius = g_set.base_start_radius;
		fb.spot.level = s->level_name;
		fb.owners = &s->owners;
	}
	for (int i = 0; i < 3; ++i)
		fb.spot.pos[i] = f.pos[i];
	fb.lv = f.lv;
	fb.gv = f.gv;
	out = fb;
	return true;
}

// The record of a far base for `character`: record_far's kind Rest with the base's object (none for a start base), its point,
// its vertices and its level's name (the game time as record_far gives it).
bool far_record(u16 character, const FarBase& fb, checkpoint::Record& out)
{
	checkpoint::Record r = record_far(character, Kind::Rest, fb.spot.bed);
	if (!r.valid())
		return false;
	r.level = fb.spot.level;
	for (int i = 0; i < 3; ++i)
		r.pos[i] = fb.spot.pos[i];
	r.level_vertex = fb.lv;
	r.game_vertex = fb.gv;
	if (!r.valid())
		return false;
	out = r;
	return true;
}

// The far player's half of the base rule (the world server, a player whose character is no object here): the SAME rule as
// base_tick (checkpoint::base::step, is_base_record, own_tent_on, hostility by base_hostile: the holder or every owner), fed with
// what the map server of his level saw: the footing and the fight. True while he is on a friendly base: the campfire he is
// reported at is no source then, and his campfire visit ends. A hostile or absent base: false, the campfires as before.
bool far_base_tick(u16 c, const levels::BodyFacts& there, checkpoint::base::Visit& v, u64 now_ms)
{
	namespace base = checkpoint::base;
	FarBase fb;
	const bool usable = far_base(c, there.footing, fb);
	const bool hostile = usable && hostile_to(community_of(c), c, *fb.owners);
	bool settled = false;
	if (usable && !hostile)
	{
		const checkpoint::Slot* slot = host_slot(c);
		const checkpoint::Record cur = slot ? slot->cur : checkpoint::Record();
		settled = base::is_base_record(cur, fb.spot) || own_tent_on(cur, fb.spot);
	}
	// round 2 (review S5): the decision is the pure far_step (tested with a sequence of footings)
	const base::Step st =
	    base::far_step(v, there.footing, usable, hostile, settled, there.fight, now_ms, g_set.base_dwell_ms);
	if (st == base::Step::Outside)
		return false;
	if (st == base::Step::SetBed)
	{
		checkpoint::Record r;
		if (!far_record(c, fb, r))
		{
			++g_stat.beds_far_refused;
			if (lua_errors::line_allowed(g_stat.beds_far_refused, kLogBurst))
				::Msg("! [coop] checkpoint sources: character %u on base %s of a map server's level: no record of it (his "
				      "entity has no game vertex)",
				    c, base_name(fb.spot.id));
			base::retry_visit(v, now_ms); // round 2 (review S4): tried again after another dwell
		}
		else if (host_set(c, r, true))
		{
			++g_stat.beds_far;
			if (lua_errors::line_allowed(g_stat.beds_far, kLogBurst))
				::Msg("[coop] checkpoint sources: character %u is on base %s of a map server's level: its %s is his checkpoint",
				    c, base_name(fb.spot.id), fb.spot.bed != kNoSource ? "bed" : "point");
		}
		else
		{
			++g_stat.beds_far_refused; // host_set logs why
			base::retry_visit(v, now_ms); // round 2 (review S4)
		}
	}
	return true;
}

// ---------------------------------------------------------------- campfires and mod objects (every kTickMs)

void tick_players(u64 now_ms)
{
	const std::vector<checkpoint::SourceSpot> spots = current_spots();
	const std::string here = level_here(); // Build 57
	xr_map<u16, checkpoint::Dwell> next;
	xr_map<u16, checkpoint::base::Visit> next_base; // Build 57: the players on a base
	for (u16 c : characters_in_world())
	{
		if (!alive(c) || (c != 0 && world::is_parked(c)))
			continue; // a visit ends with death or leaving
		// Keep each player's base visit alongside the campfire dwell.
		checkpoint::base::Visit visit;
		const auto visit_was = g.base_visit.find(c);
		if (visit_was != g.base_visit.end())
			visit = visit_was->second;
		const CGameObject* obj = world::object_here(c);
		if (!obj)
		{
			// a player on a map server's level: its authority says what he stands at, the camp is written here all the same
			levels::BodyFacts there;
			const bool told = mode().facts_of(c, there);
			// far_base_tick checks the base before the campfire, as on this level. The map server reports which base's
			// ground the player stands on.
			if (told && far_base_tick(c, there, visit, now_ms))
			{
				next_base.emplace(c, visit);
				continue;
			}
			const bool at_fire = told && (Place(there.place) == Place::Campfire || Place(there.place) == Place::Mod);
			if (!at_fire)
				continue;
			checkpoint::Dwell far_visit;
			const auto far_was = g.dwell.find(c);
			if (far_was != g.dwell.end())
				far_visit = far_was->second;
			if (checkpoint::dwell_update(far_visit, there.source, there.fight, now_ms, g_set.v.campfire_dwell_ms))
			{
				const Kind kind = Place(there.place) == Place::Mod ? Kind::Mod : Kind::Campfire;
				if (host_set(c, record_far(c, kind, there.source), true))
					++g_stat.camps;
				else
					++g_stat.camps_refused; // host_set logs why
			}
			next.emplace(c, far_visit);
			continue;
		}
		float at[3];
		to_array(obj->Position(), at);
		// Build 57 (owner 2026-11-18): the base before the campfire. On a friendly base its record is the checkpoint and its
		// campfires are no sources ("no new checkpoint at a campfire on a base", no "checkpoint updated" every N-th time
		// there); the campfire visit ends. A hostile base is no base for him: its campfires work as before.
		if (base_tick(c, obj, here, at, visit, now_ms))
		{
			next_base.emplace(c, visit);
			continue;
		}
		const checkpoint::SourceSpot* s = checkpoint::nearest_spot(spots, at);
		checkpoint::Dwell d;
		const auto was = g.dwell.find(c);
		if (was != g.dwell.end())
			d = was->second;
		const bool fight = s && in_combat(obj); // only matters next to a source
		if (checkpoint::dwell_update(d, s ? s->id : kNoSource, fight, now_ms, g_set.v.campfire_dwell_ms))
		{
			if (host_set(c, record_here(obj, s->kind, s->id), false))
				++g_stat.camps;
			else
				++g_stat.camps_refused; // host_set logs why
		}
		next.emplace(c, d);
	}
	g.dwell.swap(next);
	g.base_visit.swap(next_base);
}

// ---------------------------------------------------------------- tents, claims, lost sources (every kScanMs)

// Every source of a record, every tent known and every mod object is looked up in ALife; one that is gone, or whose id
// names an object of another section now (R1 M-4: ALife hands a freed id to the next object), rolls its records back.
void sweep_lost(const xr_vector<SourceRef>& refs)
{
	std::set<u16> in_records;
	for (const SourceRef& r : refs)
		in_records.insert(r.source);
	g.seen.retain(in_records);
	const std::vector<u16> known = g.book.known();
	const std::set<u16> tents(known.begin(), known.end());
	std::set<u16> ids(in_records);
	for (const auto& it : mods().all())
		ids.insert(it.first);
	ids.insert(tents.begin(), tents.end());
	for (u16 id : ids)
	{
		const CSE_ALifeDynamicObject* e = entity(id);
		bool reused = false;
		if (e)
		{
			const std::string sect = e->s_name.size() ? e->s_name.c_str() : "";
			const auto mod = mods().all().find(id);
			reused = (in_records.count(id) && !g.seen.same(id, sect)) ||
				(mod != mods().all().end() && mod->second.section != sect) ||
				(tents.count(id) && !is_tent_section(e->s_name));
			if (!reused)
				continue;
			++g_stat.reused;
		}
		++g_stat.lost;
		if (lua_errors::line_allowed(g_stat.lost, kLogBurst))
			::Msg("[coop] checkpoint sources: object %u is %s (%s%s%s)", id, e ? "another object now" : "gone",
			    in_records.count(id) ? "a camp" : "", mods().has(id) ? " a mod source" : "",
			    tents.count(id) ? " a tent" : "");
		if (in_records.count(id))
			host_source_lost(id);
		mods().remove(id);
		g.book.forget(id);
		g.seen.forget(id);
	}
}

// The level and game vertex for a new object at `pos` next to `obj`: the vertex under the spot, else under him.
bool vertices_at(const CGameObject* obj, const Fvector& pos, u32& lv, GameGraph::_GRAPH_ID& gv)
{
	const CLevelGraph& lg = ai().level_graph();
	lv = obj->ai_location().level_vertex_id();
	if (lg.valid_vertex_position(pos))
	{
		const u32 v = lg.vertex_id(pos);
		if (lg.valid_vertex_id(v))
			lv = v;
	}
	gv = obj->ai_location().game_vertex_id();
	if (lg.valid_vertex_id(lv) && ai().get_cross_table())
		gv = ai().cross_table().vertex(lv).game_vertex_id();
	return lg.valid_vertex_id(lv) && ai().game_graph().valid_vertex_id(gv);
}

// R1 I-1: the item of `sections` the character carries (in his inventory on the host, his own by the server state),
// the lowest id; kNoId if none.
u16 carried_item(CGameObject* obj, u16 character, const std::vector<std::string>& sections)
{
	CInventoryOwner* io = smart_cast<CInventoryOwner*>(obj);
	if (!io)
		return kNoId;
	std::vector<checkpoint::ItemSeen> items;
	for (const PIItem item : io->inventory().m_all)
	{
		if (!item)
			continue;
		const CSE_ALifeDynamicObject* e = entity(item->object().ID());
		if (!e || e->ID_Parent != character)
			continue; // being handed over right now
		checkpoint::ItemSeen s;
		s.id = item->object().ID();
		s.section = item->object().cNameSect().c_str();
		items.push_back(s);
	}
	const u16 id = checkpoint::pick_item(items, sections);
	return id == kNoSource ? kNoId : id;
}

// An item into the online character `holder`, as alife():create does it for an online parent
// (alife_simulator_script.cpp CALifeSimulator__spawn_item2): built unregistered, its spawn goes through the server's
// Process_spawn, and the player's machine gets it like any item given to him.
bool give_item(const std::string& section, u16 holder)
{
	const CSE_ALifeDynamicObject* h = entity(holder);
	if (!h || !h->m_bOnline || !Level().Server || !spawnable(section, true))
		return false;
	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(section.c_str());
	CSE_Abstract* item =
	    alife_rw().spawn_item(section.c_str(), h->o_Position, h->m_tNodeID, h->m_tGraphID, holder, false);
	item->Spawn_Write(packet, FALSE);
	Level().Server->FreeID(item->ID, 0);
	F_entity_Destroy(item);
	ClientID client;
	client.set(0xffff);
	u16 dummy;
	packet.r_begin(dummy);
	return Level().Server->Process_spawn(packet, client) != nullptr;
}

// Releases an object the normal way: online through its game object, offline through ALife.
void release_object(u16 id)
{
	if (CGameObject* go = world::object_here(id))
	{
		go->DestroyObject();
		return;
	}
	if (CSE_ALifeDynamicObject* e = entity(id))
		alife_rw().release(e, true);
}

// A map server's book knows the tents of its save and of its level. His tents on other levels since then are in its copy of the world.
size_t world_tents_of(const std::string& key)
{
	size_t n = 0;
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		checkpoint::TentMark m;
		if (e && is_tent_section(e->s_name) && e->m_ini_string.size() && checkpoint::tent_mark_from_ini(e->m_ini_string.c_str(), m) &&
		    m.owner == key)
			++n;
	}
	return n;
}

// A player's tent or sleeping bag (Rulings F3-6, I-1): paid with an item of his, put up by the authority of his level (he cannot create
// a world object until F2 Task 12b), marked his and made his camp — or nothing at all. F3a T7: the host player's too. On a map server
// the camp and the book of the world are the world server's: it hears of the tent right after the tent's spawn (host_tent_up).
void place_for(u16 character, ClaimWhat what, CGameObject* obj, const Fvector& pos)
{
	const std::string key = key_of(character);
	LevelSide* const level = mode().level();
	if (level ? level->level_tent_limit(world_tents_of(key), kMaxPlayerTents) : !g.book.may_place(key, kMaxPlayerTents))
		return refuse(character, what, Refusal::Limit);
	const bool tent = what == ClaimWhat::Tent;
	const u16 paid = carried_item(obj, character, tent ? g_set.tent_items : g_set.sleepbag_items);
	if (paid == kNoId)
		return refuse(character, what, Refusal::NoItem);
	CGameObject* paid_obj = world::object_here(paid);
	const std::string paid_section = paid_obj ? paid_obj->cNameSect().c_str() : "";
	const std::string& section = tent ? g_set.v.tent_section : g_set.v.sleepbag_section;
	// Check everything the checkpoint needs before creating the tent or sleeping bag.
	checkpoint::Record r = record_here(obj, Kind::Tent, kNoSource);
	u32 lv = u32(-1);
	GameGraph::_GRAPH_ID gv = GameGraph::_GRAPH_ID(-1);
	const char* why = nullptr;
	if (section.empty())
		why = "no section to put up (tent_section / sleepbag_section)";
	else if (!paid_obj)
		why = "the item is not online";
	else if (!r.valid() || !ai().level_graph().valid_vertex_id(r.level_vertex) ||
	    !ai().game_graph().valid_vertex_id(r.game_vertex))
		why = "he stands where no checkpoint can be";
	else if (!vertices_at(obj, pos, lv, gv))
		why = "no level or game vertex at the spot";
	CSE_Abstract* e = nullptr;
	if (!why)
	{
		e = alife_rw().spawn_item(section.c_str(), pos, lv, gv, kNoId); // registered, still offline
		if (!e)
			why = "spawn failed";
	}
	if (!why)
	{
		checkpoint::TentMark mark;
		mark.owner = key;
		mark.item = paid_section;
		write_mark(e, mark);
		r.source = e->ID;
		if (level)
			level->level_tent_placed(e->ID, character);
		else if (!host_set(character, r, false))
		{
			alife_rw().release(smart_cast<CSE_ALifeDynamicObject*>(e), true); // just made, not online yet
			why = "the checkpoint was refused";
		}
	}
	if (why)
	{
		::Msg("! [coop] checkpoint sources: no %s for character %u at (%.1f, %.1f, %.1f): %s",
		    checkpoint::claim_what_name(what), character, pos.x, pos.y, pos.z, why);
		return refuse(character, what, Refusal::Failed);
	}
	paid_obj->DestroyObject(); // the item goes only now: a refusal above kept it (one operation)
	g.book.set_owner(e->ID, key);
	++g_stat.placed;
	::Msg("[coop] checkpoint sources: %s %u (%s) put up for character %u at (%.1f, %.1f, %.1f), paid with %s",
	    checkpoint::claim_what_name(what), e->ID, section.c_str(), character, pos.x, pos.y, pos.z,
	    paid_section.c_str());
	if (!level)
		script::queue_callback(kPlacedCallback, {script::cb_int(e->ID), script::cb_int(character)}); // T13: spawned_tents
}

// Ruling F3-8: a player packs up his own tent or sleeping bag at the spot: the host releases it and gives back the very
// item it took for it; the checkpoint on it rolls back as with stock. F3a T7: the host player's too. R1: which tents are
// his is checkpoint::may_pack - for the host player also every tent nobody owns (the stock put those up for him, in
// single player or before a claim named them: no mark, or a mark without an item); such a tent gives back the stock
// item of its kind, as the stock tent bag and compression bag do.
void pack_for(u16 character, const Fvector& pos)
{
	const std::string key = key_of(character);
	float at[3];
	to_array(pos, at);
	u16 tent = kNoId;
	double best = double(g_set.v.tent_claim_radius) * double(g_set.v.tent_claim_radius);
	for (u16 id : g.book.known())
	{
		if (!checkpoint::may_pack(key, g.book.owner_of(id)))
			continue;
		const CGameObject* t = world::object_here(id);
		if (!t)
			continue;
		float p[3];
		to_array(t->Position(), p);
		const double d2 = checkpoint::detail::dist2(p, at);
		if (d2 <= best)
		{
			best = d2;
			tent = id;
		}
	}
	if (tent == kNoId)
		return refuse(character, ClaimWhat::Pack, Refusal::Far); // none of his within reach of the spot
	checkpoint::TentMark mark;
	const bool marked = read_mark(tent, mark);
	if (key == checkpoint::kHostOwner && (!marked || mark.item.empty()))
	{
		const CGameObject* t = world::object_here(tent);
		const bool bag = t && what_of(t->cNameSect()) == u8(ClaimWhat::Sleepbag);
		mark.item = bag ? g_set.sleepbag_items.front() : g_set.tent_items.front(); // read_item_lists: never empty
	}
	if (mark.item.empty() || !give_item(mark.item, character))
	{
		::Msg("! [coop] checkpoint sources: tent %u of character %u not packed up: %s", tent, character,
		    mark.item.empty() ? "nothing to give back for it" : "the item could not be given");
		return refuse(character, ClaimWhat::Pack, Refusal::Failed);
	}
	release_object(tent);
	g.book.forget(tent);
	LevelSide* const level = mode().level();
	if (!level || !level->level_tent_packed(tent))
		host_source_lost(tent); // his camp on it rolls back now, not at the next sweep; the world server's sweep does on a map server's word
	++g_stat.packed;
	::Msg("[coop] checkpoint sources: tent %u of character %u packed up, %s given back", tent, character,
	    mark.item.c_str());
}

// ---------------------------------------------------------------- messages

void host_on_claim(PeerId from, Reader& rd)
{
	if (!mode().decides_world() && !mode().leads_level())
		return; // ClientOnly: the host takes it, and the map server of the sender's level (coop_scope.h also_to_level_authority)
	ClaimWhat what = ClaimWhat::Tent;
	float p[3];
	if (!checkpoint::read_claim(rd, what, p))
	{
		++g_stat.malformed;
		if (lua_errors::line_allowed(g_stat.malformed, kLogBurst))
			::Msg("! [coop] checkpoint sources: malformed CheckpointClaim from #%u ignored", from);
		return;
	}
	const u16 character = survival::character_of_peer(from); // never the packet's word (F3a T7: peer 0 is the host player)
	if (character == kNoId)
	{
		++g_stat.not_in_world;
		if (lua_errors::line_allowed(g_stat.not_in_world, kLogBurst))
			::Msg("[coop] checkpoint sources: CheckpointClaim from #%u without a character ignored", from);
		return;
	}
	Fvector pos;
	pos.set(p[0], p[1], p[2]);
	host_claim_tent(character, what, pos);
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

void install_server(Session& s) { s.on(Msg::CheckpointClaim, &host_on_claim); }

void install_client(Endpoint&) {} // nothing the world server sends here

void map_server_scan(u64 now_ms)
{
	const LevelSide* level = mode().level();
	if (!level)
		return;
	// Build 57 round 2 (compile review S4): the base a player stood on is forgotten when he is no longer on this level
	for (auto it = g.foot_current.begin(); it != g.foot_current.end();)
		it = level->level_peer_of(it->first) != kInvalidPeer ? std::next(it) : g.foot_current.erase(it);
	if (!ensure_world() || now_ms < g.next_scan_ms)
		return;
	g.next_scan_ms = now_ms + kScanMs;
	scan_level(now_ms);
}

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
		sweep_lost(refs);
	}
	if (now_ms >= g.next_tick_ms)
	{
		g.next_tick_ms = now_ms + kTickMs;
		tick_players(now_ms);
	}
}

void player_frame()
{
	if (!g_claim_queued.exchange(false, std::memory_order_acquire))
		return;
	if (!mode().decides_world())
		return; // the session ended or turned: the claim has nobody to go to
	Writer w;
	checkpoint::write_claim(w, g_client.queued_what, g_client.queued_pos);
	net::client_send(Msg::CheckpointClaim, w); // F3a T7: to the world server over the internal channel
}

void reset()
{
	g = State();
	g_set = Settings(); // read again at the next session (a mod pack may have changed the ltx)
	g_claim_queued.store(false, std::memory_order_relaxed);
	g_client = Client();
	g_stat = Stats();
}

void print_level_status()
{
	::Msg("[coop]   checkpoint sources: this level: %u base(s) (%u row(s) dropped as overlapping, %u start "
	      "location(s) on another base's ground); body reports naming a base %llu",
	    u32(g.level_bases.size()), g.dropped, g.starts_on_ground, g_stat.feet_sent);
}

void print_status()
{
	if (mode().decides_world())
	{
		const checkpoint::SourceSettings& v = settings();
		::Msg("[coop]   checkpoint sources: campfire %.1f m / %u ms, fight %.1f m, tent rest %.1f m, claim %.1f m, %u "
		      "tent section(s), players get [%s] / [%s], %u rest name(s)",
		    v.campfire_radius, v.campfire_dwell_ms, v.combat_radius, v.tent_rest_radius, v.tent_claim_radius,
		    u32(v.tent_sections.size()), v.tent_section.c_str(), v.sleepbag_section.c_str(), u32(g_set.rest_names.size()));
		::Msg("[coop]   checkpoint sources: world %s (epoch %u), %u campfire(s), %u tent(s) here, %u known, %u "
		      "owned, %u mod source(s), %u rest zone(s), %u player(s) timed",
		    g.world ? "served" : "none", g.epoch, u32(g.campfires.size()), u32(g.tents.size()), u32(g.book.tents()),
		    u32(g.book.owners()), u32(mods().all().size()), u32(g.rest_zones.size()), u32(g.dwell.size()));
		const u64* r = g_stat.refused;
		::Msg("[coop]   checkpoint sources: camps %llu (refused %llu); claims %llu: put up %llu, packed %llu, malformed "
		      "%llu, no character %llu; refused dead %llu, far %llu, combat %llu, limit %llu, no item %llu, failed "
		      "%llu; sources gone %llu (id reused %llu); mods %llu (refused %llu); tents over the bound %llu; the host "
		      "player's claims sent %llu, held back %llu",
		    g_stat.camps, g_stat.camps_refused, g_stat.claims, g_stat.placed, g_stat.packed, g_stat.malformed,
		    g_stat.not_in_world,
		    r[size_t(Refusal::Dead)], r[size_t(Refusal::Far)], r[size_t(Refusal::Combat)], r[size_t(Refusal::Limit)],
		    r[size_t(Refusal::NoItem)], r[size_t(Refusal::Failed)], g_stat.lost, g_stat.reused, g_stat.mods_added,
		    g_stat.mods_refused, g_stat.tents_over, g_stat.sent, g_stat.held);
		// Build 57 (owner 2026-11-18): the bases, both paths: on this level (co-op) and on map servers' levels (the footing)
		u32 here = 0, start_bases = 0;
		for (const LevelBase& lb : g.level_bases)
			(lb.spot.bed != kNoSource ? here : start_bases) += 1;
		u32 merged = 0;
		for (const StartSeen& s : g.starts)
			merged += s.s.merged ? 1 : 0;
		::Msg("[coop]   checkpoint sources: bases %u row(s), %u in this world, %u start location(s) (%u merged), %u ms on one, "
		      "leave +%.1f m, start ground %.1f m; this level: %u zone base(s), %u start base(s), %u row(s) dropped as "
		      "overlapping, %u start location(s) on another base's ground; %u player(s) on a base",
		    u32(g_set.bases.size()), u32(g.bases.size()), u32(g.starts.size()), merged, g_set.base_dwell_ms,
		    g_set.base_leave_margin, g_set.base_start_radius, here, start_bases, g.dropped, g.starts_on_ground,
		    u32(g.base_visit.size()));
		::Msg("[coop]   checkpoint sources: base records set here %llu (refused %llu), on map servers' levels %llu (refused "
		      "%llu); footings heard %llu, refused %llu",
		    g_stat.beds, g_stat.beds_refused, g_stat.beds_far, g_stat.beds_far_refused, g_stat.feet, g_stat.feet_refused);
	}
	else if (mgr().active() && !mode().decides_world())
	{
		::Msg("[coop]   checkpoint sources: tent claims sent %llu, held back %llu", g_stat.sent, g_stat.held);
		if (mode().level())
			print_level_status();
	}
}

// ---------------------------------------------------------------- host API

bool in_combat(const CGameObject* character)
{
	if (!character || !g_pGameLevel || (!mode().decides_world() && !mode().leads_level()))
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
	const CGameObject* obj = world::object_here(character);
	if (!obj)
		return Place::None;
	const Fvector& p = obj->Position();
	float at[3];
	to_array(p, at);

	Fsphere probe;
	probe.set(p, kRestProbe);
	for (const auto& it : g.rest_zones)
	{
		const CSpaceRestrictor* zone = smart_cast<CSpaceRestrictor*>(world::object_here(it.first));
		if (zone && zone->inside(probe))
		{
			source = it.first;
			return Place::Rest;
		}
	}

	const std::string key = key_of(character);
	double best = double(g_set.v.tent_rest_radius) * double(g_set.v.tent_rest_radius);
	for (const checkpoint::TentSeen& t : g.tents)
	{
		if (g.book.owner_of(t.id) != key)
			continue; // R1 I-2: his by key, also after a load
		const double d2 = checkpoint::detail::dist2(t.pos, at);
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
	if ((mgr().active() && !mode().decides_world()) || !ai().get_alife())
		return false; // a client plays in the host's world: its camps are the host's to know
	const char* why = nullptr;
	const CSE_ALifeDynamicObject* e = entity(id);
	if (!e)
		why = "no such object";
	else if (world::is_player_character(e))
		why = "a player's character";
	else if (!mods().add(id, radius, e->s_name.size() ? e->s_name.c_str() : "")) // R1 M-4: its section kept
		why = checkpoint::radius_ok(radius) ? "too many mod sources" : "not a radius";
	if (why)
	{
		++g_stat.mods_refused;
		if (lua_errors::line_allowed(g_stat.mods_refused, kLogBurst))
			::Msg("! [coop] checkpoint sources: register_source(%u, %f) refused: %s", id, radius, why);
		return false;
	}
	++g_stat.mods_added;
	return true;
}

bool host_unregister_source(u16 id) { return (!mgr().active() || mode().decides_world()) && mods().remove(id); }

void host_claim_tent(u16 character, ClaimWhat what, const Fvector& pos)
{
	if (!ensure_world())
		return;
	++g_stat.claims;
	CGameObject* obj = world::object_here(character);
	LevelSide* const level = mode().level();
	const bool in_world = level ? level->level_in_world(character) : survival::player_in_world(character);
	const PeerId peer = survival::peer_of_character(character);
	if (!obj && mode().decides_world() && peer != kInvalidPeer && mode().where(peer) == Where::Away)
	{
		// The claim's copy goes to his level's authority. If nobody leads it, the world must answer the player.
		if (!mode().led_elsewhere(peer))
			return refuse(character, what, Refusal::Failed);
		return;
	}
	if (!pos_ok(pos) || !obj || !in_world)
	{
		++g_stat.not_in_world;
		if (lua_errors::line_allowed(g_stat.not_in_world, kLogBurst))
			::Msg("[coop] checkpoint sources: %s claim of character %u ignored (%s)", checkpoint::claim_what_name(what),
			    character, !pos_ok(pos) ? "not a position" : "he is not in the world");
		return;
	}
	if (!alive(character))
		return refuse(character, what, Refusal::Dead);
	if (obj->Position().distance_to(pos) > g_set.v.tent_claim_radius)
		return refuse(character, what, Refusal::Far);
	if (what == ClaimWhat::Pack)
		return pack_for(character, pos); // F3a T7: the host player's Fold too
	if (in_combat(obj))
		return refuse(character, what, Refusal::Combat);
	// F3a T7: every player's tent is put up here, the host player's too (the stock item_tent no longer puts his up:
	// coop_tent_guard.script sends him the client's way). F2 Task 12b (ids leased to clients) may let a player create
	// the tent himself.
	place_for(character, what, obj, pos);
}

void host_tent_up(u16 tent, u16 character)
{
	if (!ensure_world())
		return;
	const CSE_ALifeDynamicObject* e = entity(tent);
	checkpoint::TentMark mark;
	if (!e || !is_tent_section(e->s_name) || !read_mark(tent, mark))
	{
		::Msg("! [coop] checkpoint sources: a map server speaks of tent %u, this world has no such tent", u32(tent));
		return;
	}
	checkpoint::TentSeen seen;
	seen.id = tent;
	to_array(e->o_Position, seen.pos);
	seen.what = what_of(e->s_name);
	g.book.observe(std::vector<checkpoint::TentSeen>(1, seen), mgr().now_ms());
	g.book.set_owner(tent, mark.owner);
	++g_stat.placed;
	script::queue_callback(kPlacedCallback, {script::cb_int(tent), script::cb_int(character)}); // T13: spawned_tents
	// His tent stands and he paid for it; a camp that is refused leaves him a tent he can pack up.
	if (!host_set(character, record_far(character, Kind::Tent, tent), true))
		refuse(character, seen.what == u8(ClaimWhat::Sleepbag) ? ClaimWhat::Sleepbag : ClaimWhat::Tent, Refusal::Failed);
}

u16 host_tent_owner(u16 tent)
{
	if (!ensure_world())
		return kNoId;
	const std::string& key = g.book.owner_of(tent);
	return key.empty() ? kNoId : character_of_key(key);
}

// ---------------------------------------------------------------- any machine

bool is_sleeping_bag(const shared_str& section)
{
	settings(); // a client reads the section names too
	return what_of(section) == u8(ClaimWhat::Sleepbag);
}

bool is_tent(const shared_str& section)
{
	settings();
	return what_of(section) == u8(ClaimWhat::Tent);
}

bool claim_tent(ClaimWhat what, const Fvector& pos)
{
	if (!(mode().decides_world() || mgr().is_live_client()) || !pos_ok(pos))
		return false;
	const u64 now = mgr().now_ms();
	if (g_client.sent_any && now - g_client.last_ms < kClaimSpacingMs)
	{
		++g_stat.held;
		return false;
	}
	float p[3];
	to_array(pos, p);
	if (mode().decides_world())
	{
		// F3a T7: the host player claims like a client, over the internal channel from the next frame (player_frame).
		// R1: the data first, then the flag.
		g_client.queued_what = what;
		for (int i = 0; i < 3; ++i)
			g_client.queued_pos[i] = p[i];
		g_claim_queued.store(true, std::memory_order_release);
	}
	else
	{
		Writer w;
		checkpoint::write_claim(w, what, p);
		net::client_send(Msg::CheckpointClaim, w);
	}
	g_client.sent_any = true;
	g_client.last_ms = now;
	++g_stat.sent;
	return true;
}

// ---------------------------------------------------------------- bases (Build 57, owner 2026-11-18)

bool base_bed_record(u16 character, checkpoint::Record& out)
{
	namespace base = checkpoint::base;
	if (!mode().decides_world() || !ensure_world())
		return false;
	const CGameObject* obj = world::object_here(character);
	if (obj)
	{
		// the co-op path: his character is an object here, this process has his level's AI map
		if (g.level_bases.empty())
			return false;
		float at[3];
		to_array(obj->Position(), at);
		const auto was = g.base_visit.find(character);
		const base::Key current = was != g.base_visit.end() ? was->second.base : base::kNoBase;
		const base::Key k = base::base_at(base_spots(character), level_here(), at, current, g_set.base_leave_margin);
		const LevelBase* lb = base::is_base_key(k) ? level_base(k) : nullptr;
		return lb && bed_record(obj, *lb, out);
	}
	// the server path: a player on a map server's level, by the footing its map server reported (checked when it came)
	levels::BodyFacts there;
	if (!mode().facts_of(character, there))
		return false;
	FarBase fb;
	return far_base(character, there.footing, fb) && !hostile_to(community_of(character), character, *fb.owners) &&
	    far_record(character, fb, out);
}

namespace
{
// The start base whose record `r` is (kind Rest, no object, within kSamePlaceM of the ltx point of a start base of its level
// that was not merged), its key in `key`; nullptr when it is none. The host's (world server's) tables.
const StartSeen* start_of_record(const checkpoint::Record& r, checkpoint::base::Key& key)
{
	namespace base = checkpoint::base;
	key = base::kNoBase;
	if (!r.valid() || r.kind != Kind::Rest || r.source != kNoSource)
		return nullptr;
	for (size_t i = 0; i < g.starts.size(); ++i)
	{
		const StartSeen& st = g.starts[i];
		if (st.s.merged || st.level_name.empty() || st.level_name != r.level)
			continue;
		base::Spot s;
		s.id = base::start_key(i);
		s.level = st.level_name;
		for (int k = 0; k < 3; ++k)
			s.pos[k] = st.s.pos[k];
		if (base::is_base_record(r, s))
		{
			key = s.id;
			return &st;
		}
	}
	return nullptr;
}

// A process that holds the world's tables of bases: the host (co-op) or the world server, not a map server.
bool tables_here() { return mode().decides_world() && ensure_world(); }

// Build 57 (lead 2026-11-18, round 3 review S3): the same, but never BUILDS the tables. For a caller in a message handler
// (record_base from the CheckpointState callback), which may run while the world is being replaced: the tables are used
// only when they already belong to this world.
bool tables_built()
{
	return mode().decides_world() && g_pGameLevel && ai().get_alife() && ai().get_level_graph() &&
		ai().get_game_graph() && g.world && g.epoch == join::host_world_epoch();
}
} // namespace

bool bed_hostile(u16 character, const checkpoint::Record& r)
{
	namespace base = checkpoint::base;
	if (!r.valid() || r.kind != Kind::Rest)
		return false;
	if (!tables_here())
		return false;
	if (r.source != kNoSource)
	{
		const BaseSeen* b = base_of_bed(r.source);
		return b && hostile_to(community_of(character), character, b->owners);
	}
	// a start base's record: no object, within kSamePlaceM of the point of a start base of its level (the ltx position)
	base::Key key = base::kNoBase;
	const StartSeen* st = start_of_record(r, key);
	return st && hostile_to(community_of(character), character, st->owners);
}

bool base_ground(const checkpoint::Record& r, checkpoint::base::Spot& out)
{
	namespace base = checkpoint::base;
	if (!r.valid() || r.kind != Kind::Rest || !tables_here())
		return false;
	base::Spot s;
	s.level = r.level;
	if (r.source != kNoSource)
	{
		// a sleep zone base: its record lies at its bed point; the ground is the row's radius around it
		const BaseSeen* b = base_of_bed(r.source);
		if (!b)
			return false;
		s.id = base::zone_key(r.source);
		s.bed = r.source;
		for (int i = 0; i < 3; ++i)
			s.pos[i] = r.pos[i];
		s.radius = b->spot.radius;
	}
	else
	{
		base::Key key = base::kNoBase;
		const StartSeen* st = start_of_record(r, key);
		if (!st)
			return false;
		s.id = key;
		s.bed = kNoSource;
		for (int i = 0; i < 3; ++i)
			s.pos[i] = st->s.pos[i];
		s.radius = g_set.base_start_radius;
	}
	out = s;
	return true;
}

const char* record_base(const checkpoint::Record& r, u16 source_local)
{
	namespace base = checkpoint::base;
	if (!r.valid() || r.kind != Kind::Rest)
		return base::mark_name(base::Mark::None);
	if (mode().decides_world() && !tables_built())
		return nullptr; // the host between two worlds: no tables now (the script's own lookup)
	if (tables_built())
	{
		// the host's machine (the host player hears his own records through the same callback): its own tables, exact
		base::Key key = base::kNoBase;
		const int zone = r.source != kNoSource ? (base_of_bed(r.source) ? 1 : 0) : -1;
		return base::mark_name(base::mark_of(r, zone, r.source == kNoSource && start_of_record(r, key) != nullptr));
	}
	// a client: no ALife, no world tables. A record without an object is a start base's when it lies within kSamePlaceM of a
	// start base's point of [coop_start_bases] (the ltx, read by every machine; the level is not compared here: a client has
	// no game graph to name a start's level). A record with an object: that object on this machine, a sleep zone named in
	// [coop_bases]; an object this machine does not hold cannot be told (nil: the script's own lookup).
	settings();
	if (r.source == kNoSource)
	{
		bool near_start = false;
		for (const base::Start& st : g_set.starts)
		{
			double d2 = 0;
			for (int i = 0; i < 3; ++i)
			{
				const double d = double(r.pos[i]) - double(st.pos[i]);
				d2 += d * d;
			}
			near_start = near_start || d2 <= double(checkpoint::kSamePlaceM) * double(checkpoint::kSamePlaceM);
		}
		return base::mark_name(base::mark_of(r, -1, near_start));
	}
	int zone = -1;
	if (g_pGameLevel && source_local != kNoSource)
		if (const CObject* o = Level().Objects.net_Find(source_local))
		{
			const char* name = o->cName().c_str();
			zone = 0;
			for (const base::Row& row : g_set.bases)
				if (name && row.zone == name)
					zone = 1;
		}
	return base::mark_name(base::mark_of(r, zone, false));
}

void map_server_footing(u16 character, const Fvector& pos, checkpoint::base::Footing& out)
{
	namespace base = checkpoint::base;
	out = base::Footing();
	if (!mode().level() || !ensure_world() || g.level_bases.empty())
	{
		g.foot_current.erase(character);
		return;
	}
	std::vector<base::Spot> spots; // geometry only: nothing is hostile here, the world server judges that
	spots.reserve(g.level_bases.size());
	for (const LevelBase& lb : g.level_bases)
		spots.push_back(lb.spot);
	float at[3];
	to_array(pos, at);
	const auto was = g.foot_current.find(character);
	const base::Key current = was != g.foot_current.end() ? was->second : base::kNoBase;
	const base::Key k = base::base_at(spots, level_here(), at, current, g_set.base_leave_margin);
	const LevelBase* lb = base::is_base_key(k) ? level_base(k) : nullptr;
	if (!lb || !base::footing_of(k, out))
	{
		out = base::Footing();
		g.foot_current.erase(character);
		return;
	}
	g.foot_current[character] = k;
	for (int i = 0; i < 3; ++i)
		out.pos[i] = lb->spot.pos[i];
	out.lv = lb->lv;
	out.gv = u16(lb->gv);
	++g_stat.feet_sent;
}

bool host_footing(const checkpoint::base::Footing& f, u16 level, const float body[3])
{
	namespace base = checkpoint::base;
	if (f.kind == base::kFootNone && base::footing_ok(f))
		return false; // he stands on no base: nothing to keep, nothing refused
	if (!mode().decides_world())
		return false;
	++g_stat.feet;
	// The world supplies the base's identity, level and radius. For a zone the map server supplies the bed point;
	// for a start base the world can also check the point against the config.
	const char* why = nullptr;
	base::FootBase fbase;
	if (!ensure_world())
		why = "no world here";
	else
	{
		const base::Key k = base::key_of(f);
		if (base::is_zone_key(k))
		{
			if (const BaseSeen* b = base_of_bed(base::zone_of(k)))
			{
				fbase.known = true;
				fbase.level = b->level;
				to_array(b->zone_pos, fbase.pos);
				fbase.radius = b->spot.radius;
			}
		}
		else if (const StartSeen* s = start_of_key(k))
		{
			fbase.known = true;
			fbase.level = s->s.level;
			for (int i = 0; i < 3; ++i)
				fbase.pos[i] = s->s.pos[i];
			fbase.radius = g_set.base_start_radius;
		}
		// round 3 (review S5): the body position of the same report is checked against the base's ground too
		const base::FootCheck c =
		    base::footing_check(f, level, levels::level_of_vertex(f.gv), fbase, g_set.base_leave_margin, body);
		if (c != base::FootCheck::Ok)
			why = base::foot_check_name(c);
	}
	if (!why)
		return true;
	++g_stat.feet_refused;
	if (g_stat.feet_refused <= kFootLogs)
		::Msg("! [coop] checkpoint sources: a footing (kind %u, ref %u) from the map server of level %u refused: %s "
		      "(reported point %.2f, %.2f, %.2f; authored point %.2f, %.2f, %.2f; known base %u)",
		    u32(f.kind), u32(f.ref), u32(level), why, f.pos[0], f.pos[1], f.pos[2], fbase.pos[0], fbase.pos[1],
		    fbase.pos[2], u32(fbase.known));
	return false;
}
} // namespace sources
} // namespace checkpoints
} // namespace coop
