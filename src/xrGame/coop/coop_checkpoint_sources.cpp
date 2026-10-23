// Anomaly Coop — where a checkpoint may come from, on the host (F3 Task 7). See coop_checkpoint_sources.h; the pure
// rules (settings bounds, the claim message, the nearest source, mod objects, the sections behind ids, the mark of a
// tent, the item a tent is paid with, the book of tents and claims) are xrCoop/coop_checkpoint_source_rules.h, the
// claim's answer xrCoop/coop_checkpoint_store.h, the dwell timer xrCoop/coop_checkpoint_rules.h.
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
#include "../xrServerEntities/xrMessages.h"

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

constexpr u16 kNoId = 0xffff;
constexpr const char* kSection = "coop_checkpoints";
constexpr const char* kRestZonesSection = "coop_rest_zones";
constexpr const char* kCoverListSection = "list"; // of the file named by cover_list
constexpr const char* kTentItemsSection = "tent_i_bag_sections"; // of the file named by item_lists (stock lists)
constexpr const char* kSleepbagItemsSection = "sleeping_bag_sections";
constexpr const char* kRefusedCallback = "coop_checkpoint_refused";
constexpr const char* kPlacedCallback = "coop_tent_placed";
constexpr u64 kTickMs = 500; // players against campfires and mod objects (plan F3 Task 7, step 3)
constexpr u64 kScanMs = 1000; // the level's campfires and tents, claims, lost sources (steps 2, 4, 5)
constexpr float kRestProbe = 0.3f; // the sphere tested against a rest restrictor (step 6)
constexpr size_t kMaxRestNames = 1024; // rest zones + surge covers (stock: 25 + 294)
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
		v.tent_claim_ms = read_period("tent_claim_ms", v.tent_claim_ms, checkpoint::kMinClaimMs); // R1 M-11
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

	::Msg("[coop] checkpoint sources: campfire %.1f m / %u ms, fight %.1f m, tents %u section(s) (rest %.1f m, claim "
	      "%.1f m / %u ms), a client gets [%s] / [%s] for %u / %u item kind(s), %u rest zone name(s)",
	    v.campfire_radius, v.campfire_dwell_ms, v.combat_radius, u32(v.tent_sections.size()), v.tent_rest_radius,
	    v.tent_claim_radius, v.tent_claim_ms, v.tent_section.c_str(), v.sleepbag_section.c_str(),
	    u32(g_set.tent_items.size()), u32(g_set.sleepbag_items.size()), u32(g_set.rest_names.size()));
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
	checkpoint::TentBook book; // owners by key, filled from the tents' marks when the world is served (R1 I-2)
	checkpoint::SourceSections seen; // R1 M-4: the section behind each source of a record
	xr_map<u16, checkpoint::Dwell> dwell; // by character (host numbering): the players checked at the last tick
	xr_map<u16, shared_str> rest_zones; // restrictor id -> name, from ALife when the world was served
	xr_map<std::string, u16> key_char; // player key -> his character, from ALife when the world was served
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

constexpr size_t kRefusals = size_t(Refusal::Failed) + 1;

struct Stats
{
	u64 camps = 0, camps_refused = 0; // dwell done: host_set took it / refused it
	u64 claims = 0, malformed = 0, not_in_world = 0, claims_full = 0;
	u64 refused[kRefusals] = {};
	u64 matched = 0, placed = 0, packed = 0, expired = 0, dropped_left = 0;
	u64 lost = 0, reused = 0, tents_over = 0;
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

// The answer "refused" to a claim (Ruling F3-5): the host's own as a callback here; a client's as CheckpointState flag
// 4 to him alone, and his engine calls the callback there.
void refuse(u16 character, ClaimWhat what, Refusal reason)
{
	++g_stat.refused[size_t(reason)];
	if (log_allowed(g_stat.refused[size_t(reason)]))
		::Msg("[coop] checkpoint sources: the %s claim of character %u is refused (%s)",
		    checkpoint::claim_what_name(what), character, checkpoint::refusal_name(reason));
	if (character == 0)
	{
		script::queue_callback(kRefusedCallback,
		    {script::cb_str(checkpoint::refusal_name(reason)), script::cb_str(checkpoint::claim_what_name(what))});
		return;
	}
	const PeerId peer = join::peer_of_proxy(character);
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

// One pass over ALife for a newly served world: the rest zones and surge covers (a release build cannot find an object
// by name), every tent with its owner from its mark (R1 I-2: owners outlast loads and level changes; every tent that
// stands now is old, R1 M-5), and the player characters by key (tent_owner).
void read_world(u64 now_ms)
{
	g.rest_zones.clear();
	g.key_char.clear();
	std::vector<checkpoint::TentSeen> standing;
	for (const auto& it : ai().alife().objects().objects())
	{
		const CSE_ALifeDynamicObject* e = it.second;
		if (!e)
			continue;
		LPCSTR name = e->name_replace();
		if (name && *name && g_set.rest_names.count(name))
			g.rest_zones.emplace(it.first, shared_str(name));
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
		if (log_allowed(g_stat.tents_over))
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
	read_world(now_ms);
	::Msg("[coop] checkpoint sources: world epoch %u, %u rest zone(s) and surge cover(s) of %u name(s), %u tent(s), %u "
	      "of them owned",
	    epoch, u32(g.rest_zones.size()), u32(g_set.rest_names.size()), u32(g.book.tents()), u32(g.book.owners()));
}

// False when there is no host world to look at.
bool ensure_world()
{
	if (!mgr().is_host() || !g_pGameLevel || !ai().get_alife() || !ai().get_level_graph() || !ai().get_game_graph())
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
		s.radius = it.second.radius;
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
		if (log_allowed(g_stat.lost))
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

// The host's own tent turned up for his claim: marked his, his camp.
void on_tent_matched(const checkpoint::TentMatch& m)
{
	++g_stat.matched;
	if (CSE_ALifeDynamicObject* e = entity(m.tent))
	{
		checkpoint::TentMark mark;
		mark.owner = key_of(m.character);
		write_mark(e, mark); // no item: the stock item_tent took it and packs the tent up
	}
	const CGameObject* obj = object_here(m.character);
	if (!obj || !alive(m.character) || !host_set(m.character, record_here(obj, Kind::Tent, m.tent)))
		return refuse(m.character, m.what, obj && !alive(m.character) ? Refusal::Dead : Refusal::Failed);
	::Msg("[coop] checkpoint sources: tent %u is character %u's camp", m.tent, m.character);
}

void settle_claims(u64 now_ms)
{
	const xr_vector<u16> in_world = characters_in_world();
	// Polled, not told: sync::on_peer_left does not reach the F3 modules, and a player who only left the world (a
	// reload) must lose his claim too. Nobody to answer.
	for (const checkpoint::ClaimEnd& c : g.book.keep_claims_of(std::set<u16>(in_world.begin(), in_world.end())))
	{
		++g_stat.dropped_left;
		::Msg("[coop] checkpoint sources: the tent claim of character %u dropped, he left the world", c.character);
	}
	for (const checkpoint::ClaimEnd& c : g.book.expire(now_ms))
	{
		++g_stat.expired;
		refuse(c.character, c.what, Refusal::Failed); // one answer per claim: no tent turned up
	}
	for (const checkpoint::TentMatch& m : g.book.match(g.tents, g_set.v.tent_claim_radius, g_set.v.tent_claim_ms))
		on_tent_matched(m);
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
	if (CGameObject* go = object_here(id))
	{
		go->DestroyObject();
		return;
	}
	if (CSE_ALifeDynamicObject* e = entity(id))
		alife_rw().release(e, true);
}

// A client's tent or sleeping bag (Rulings F3-6, I-1): paid with an item of his, put up by the host (he cannot create a
// world object until F2 Task 12b), marked his and made his camp — or nothing at all.
void place_for(u16 character, ClaimWhat what, CGameObject* obj, const Fvector& pos)
{
	const std::string key = key_of(character);
	if (!g.book.may_place(key, kMaxPlayerTents))
		return refuse(character, what, Refusal::Limit);
	const bool tent = what == ClaimWhat::Tent;
	const u16 paid = carried_item(obj, character, tent ? g_set.tent_items : g_set.sleepbag_items);
	if (paid == kNoId)
		return refuse(character, what, Refusal::NoItem);
	CGameObject* paid_obj = object_here(paid);
	const std::string paid_section = paid_obj ? paid_obj->cNameSect().c_str() : "";
	const std::string& section = tent ? g_set.v.tent_section : g_set.v.sleepbag_section;
	// Everything a checkpoint needs is checked before anything is made (R1 M-2).
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
		if (!host_set(character, r))
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
	script::queue_callback(kPlacedCallback, {script::cb_int(e->ID), script::cb_int(character)}); // T13: spawned_tents
}

// Ruling F3-8: a client packs up his own tent or sleeping bag at the spot: the host releases it and gives back the very
// item it took for it; the checkpoint on it rolls back as with stock.
void pack_for(u16 character, const Fvector& pos)
{
	const std::string key = key_of(character);
	float at[3];
	to_array(pos, at);
	u16 tent = kNoId;
	double best = double(g_set.v.tent_claim_radius) * double(g_set.v.tent_claim_radius);
	for (u16 id : g.book.known())
	{
		if (g.book.owner_of(id) != key)
			continue;
		const CGameObject* t = object_here(id);
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
	if (!read_mark(tent, mark) || mark.item.empty() || !give_item(mark.item, character))
	{
		::Msg("! [coop] checkpoint sources: tent %u of character %u not packed up: %s", tent, character,
		    mark.item.empty() ? "nothing to give back for it" : "the item could not be given");
		return refuse(character, ClaimWhat::Pack, Refusal::Failed);
	}
	release_object(tent);
	g.book.forget(tent);
	host_source_lost(tent); // his camp on it rolls back now, not at the next sweep
	++g_stat.packed;
	::Msg("[coop] checkpoint sources: tent %u of character %u packed up, %s given back", tent, character,
	    mark.item.c_str());
}

// ---------------------------------------------------------------- messages

void host_on_claim(PeerId from, Reader& rd)
{
	if (!mgr().is_host())
		return; // ClientOnly: only the host takes it
	ClaimWhat what = ClaimWhat::Tent;
	float p[3];
	if (!checkpoint::read_claim(rd, what, p))
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
	host_claim_tent(proxy, what, pos);
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
		      "ms, %u tent section(s), clients get [%s] / [%s], %u rest name(s)",
		    v.campfire_radius, v.campfire_dwell_ms, v.combat_radius, v.tent_rest_radius, v.tent_claim_radius,
		    v.tent_claim_ms, u32(v.tent_sections.size()), v.tent_section.c_str(), v.sleepbag_section.c_str(),
		    u32(g_set.rest_names.size()));
		::Msg("[coop]   checkpoint sources: world %s (epoch %u), %u campfire(s), %u tent(s) here, %u known, %u "
		      "owned, %u claim(s) waiting, %u mod source(s), %u rest zone(s), %u player(s) timed",
		    g.world ? "served" : "none", g.epoch, u32(g.campfires.size()), u32(g.tents.size()), u32(g.book.tents()),
		    u32(g.book.owners()), u32(g.book.claims()), u32(mods().all().size()), u32(g.rest_zones.size()),
		    u32(g.dwell.size()));
		const u64* r = g_stat.refused;
		::Msg("[coop]   checkpoint sources: camps %llu (refused %llu); claims %llu: matched %llu, put up %llu, packed "
		      "%llu, expired %llu, left %llu, full %llu, malformed %llu, no character %llu; refused dead %llu, far "
		      "%llu, combat %llu, limit %llu, no item %llu, failed %llu; sources gone %llu (id reused %llu); mods "
		      "%llu (refused %llu); tents over the bound %llu",
		    g_stat.camps, g_stat.camps_refused, g_stat.claims, g_stat.matched, g_stat.placed, g_stat.packed,
		    g_stat.expired, g_stat.dropped_left, g_stat.claims_full, g_stat.malformed, g_stat.not_in_world,
		    r[size_t(Refusal::Dead)], r[size_t(Refusal::Far)], r[size_t(Refusal::Combat)], r[size_t(Refusal::Limit)],
		    r[size_t(Refusal::NoItem)], r[size_t(Refusal::Failed)], g_stat.lost, g_stat.reused, g_stat.mods_added,
		    g_stat.mods_refused, g_stat.tents_over);
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
	if (mgr().is_client() || !ai().get_alife())
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
		if (log_allowed(g_stat.mods_refused))
			::Msg("! [coop] checkpoint sources: register_source(%u, %f) refused: %s", id, radius, why);
		return false;
	}
	++g_stat.mods_added;
	return true;
}

bool host_unregister_source(u16 id) { return !mgr().is_client() && mods().remove(id); }

void host_claim_tent(u16 character, ClaimWhat what, const Fvector& pos)
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
			::Msg("[coop] checkpoint sources: %s claim of character %u ignored (%s)", checkpoint::claim_what_name(what),
			    character, !pos_ok(pos) ? "not a position" : "he is not in the world");
		return;
	}
	if (!alive(character))
		return refuse(character, what, Refusal::Dead);
	if (obj->Position().distance_to(pos) > g_set.v.tent_claim_radius)
		return refuse(character, what, Refusal::Far);
	if (what == ClaimWhat::Pack)
	{
		if (character == 0)
		{
			::Msg("[coop] checkpoint sources: the host packs up his tents with the stock tent bag");
			return refuse(character, what, Refusal::Failed);
		}
		return pack_for(character, pos);
	}
	if (in_combat(obj))
		return refuse(character, what, Refusal::Combat);
	if (character != 0)
	{
		// F2 Task 12b (ids leased to clients) lets a client create the tent himself; then this branch goes and his
		// claim waits for his tent like the host's.
		return place_for(character, what, obj, pos);
	}
	float p[3];
	to_array(pos, p);
	if (!g.book.claim(character, key_of(character), what, p, mgr().now_ms(), g_set.v.tent_claim_ms))
	{
		++g_stat.claims_full;
		::Msg("! [coop] checkpoint sources: %u tent claims wait already, the one of character %u is dropped",
		    u32(checkpoint::kMaxClaims), character);
		refuse(character, what, Refusal::Failed);
	}
}

u16 host_tent_owner(u16 tent)
{
	if (!ensure_world())
		return kNoId;
	const std::string& key = g.book.owner_of(tent);
	return key.empty() ? kNoId : character_of_key(key);
}

// ---------------------------------------------------------------- any machine

bool claim_tent(ClaimWhat what, const Fvector& pos)
{
	if (mgr().is_host())
	{
		host_claim_tent(0, what, pos);
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
	checkpoint::write_claim(w, what, p);
	net::client_send(Msg::CheckpointClaim, w);
	g_client.sent_any = true;
	g_client.last_ms = now;
	++g_stat.sent;
	return true;
}
} // namespace sources
} // namespace checkpoints
} // namespace coop
