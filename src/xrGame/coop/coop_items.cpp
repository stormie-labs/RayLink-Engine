#include "stdafx.h"
#include "coop_items.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_levels.h"
#include "coop_sync.h"
#include "coop_alife.h"
#include "coop_trade.h"
#include "coop_requests.h"

#include <algorithm>
#include <mutex>

#include "ai_space.h"
#include "Level.h"
#include "Actor.h"
#include "entity_alive.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "Weapon.h"
#include "WeaponMagazined.h"
#include "WeaponMagazinedWGrenade.h"
#include "WeaponAmmo.h"
#include "eatable_item.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "inventory_upgrade_manager.h"
#include "inventory_upgrade_root.h"
#include "inventory_upgrade.h"
#include "inventory_upgrade_group.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrServer_Objects.h"
#include "../xrServerEntities/xrMessages.h"
#include "xrCoop/coop_item_tags.h"
#include "xrCoop/coop_ledger.h"
#include "xrCoop/coop_rules.h"
#include "xrCoop/coop_spawn.h"
#include "xrCoop/coop_upgrades.h"
#include "xrCoop/coop_validate.h"

namespace coop
{
namespace items
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u8 kBlobVersion = 1;
constexpr u32 kScanMs = 500; // periodic owner scan
constexpr u32 kModifyScanMs = 100; // scan after an inventory change, at most this often
constexpr u32 kEnforceMs = 500; // host: re-apply owner state if the host copy drifted
constexpr u32 kGraceMs = 3000; // host: late state of an item that just left the sender's hands
constexpr u32 kPendingMs = 30000; // client: state for an entity that does not exist yet
constexpr u32 kTakeBaseMs = 30000; // client: how long the copy before a take waits for the host's push (as kPendingMs)
constexpr float kForeignRange = 50.f; // a corpse / stash a player unloads must be near that player
constexpr size_t kMaxBlob = kMaxItemState; // shared with ReqSpawn (coop_spawn.h)
constexpr u32 kLauncherCap = 3; // rounds a launcher magazine may report (stock launchers hold 1)

enum class Op : u8
{
	State = 1,
	SpawnAmmo = 2,
};
constexpr u8 kFlagAuthority = 1; // host truth for an item that just entered a player's inventory

enum Tag : u8
{
	TagCondition = 1,
	TagUpgrades = 2,
	TagBox = 3,
	TagAddons = 4,
	TagScope = 5,
	TagMain = 6,
	TagLauncher = 7,
	TagUses = 8,
	TagMode = 9,
	TagFireMode = 10,
	TagCartridges = 11,
};

using Blob = std::vector<u8>;
using UpgradeList = xr_vector<shared_str>;

// Decoded item state. Every field is optional (has_*).
struct ItemData
{
	bool has_cond = false;
	u16 cond = 0;
	bool has_upgrades = false;
	UpgradeList upgrades; // install order (it matters: upgrades depend on each other)
	bool has_box = false;
	u16 box = 0;
	bool has_addons = false;
	u8 addons = 0;
	bool has_scope = false;
	u8 scope = 0;
	bool has_main = false; // gun magazine, whatever the launcher mode is
	u16 main_count = 0;
	u8 main_type = 0;
	bool has_launcher = false; // grenade launcher magazine
	u16 gl_count = 0;
	u8 gl_type = 0;
	bool has_uses = false;
	u8 uses = 0;
	bool has_mode = false;
	bool grenade_mode = false;
	bool has_fire_mode = false;
	u8 fire_mode = 0;
	bool has_cartridges = false;
	std::vector<u8> cartridges;
};

enum class Reason : u8
{
	Malformed,
	NoProxy,
	Missing,
	Section,
	NotOwner,
	Moved,
	Insane,
	Count
};
const char* reason_name(Reason r)
{
	switch (r)
	{
	case Reason::Malformed: return "malformed";
	case Reason::NoProxy: return "no character";
	case Reason::Missing: return "missing";
	case Reason::Section: return "section";
	case Reason::NotOwner: return "not owner";
	case Reason::Moved: return "moved";
	case Reason::Insane: return "insane value";
	default: return "?";
	}
}

struct Stats
{
	u64 sent = 0, spawns_sent = 0, foreign_sent = 0;
	u64 received = 0, applied = 0, relayed = 0, pushed = 0;
	u64 authority_applied = 0, authority_skipped = 0, ignored = 0, pending_dropped = 0;
	u64 enforced = 0, boxes_released = 0, spawns_ok = 0, spawn_rounds = 0, spawns_repeat = 0;
	u64 rejected[u32(Reason::Count)] = {};
	u64 upgrades_dropped = 0; // R3 В6: unknown / out-of-order / clashing upgrades never reach a loading install
	u64 dropped_before_live = 0; // R2 I2: client, ItemState that came before we were live
	u64 upgrades_sold = 0; // F2 T5 fix round 1: host, sold upgrades installed on its copy and sent to the others
	u64 world_states = 0; // parity audit F1: host, states of world items a client asked for or opened
	u64 take_kept = 0; // live report 2026-11-08: client, what its take callbacks changed kept over the host's push
	u64 join_held = 0, join_taken = 0; // host: states of a player still joining, kept and taken when his join ended
};
Stats g_stats;

// A player's game says the state of its own items once when it goes live (frame, g_resync). On a map server that batch came
// before the word that he is in the world: all of it was refused as "no character" - 37 to 57 red lines at every entry of a level,
// and the launcher made an error report of the session (the owner's logs, build 46, 2026-11-13). The states of a peer whose
// character is known and whose join is not over are kept here, in the order they came, and taken when the join ends.
struct HeldState
{
	u16 id = kNoId; // host numbering, as on the wire
	std::string section;
	Blob blob;
};
constexpr size_t kJoinHeldMax = 512; // an inventory is some tens of items; more than this is refused as before
xr_map<PeerId, xr_vector<HeldState>> g_join_held;

// ---------------------------------------------------------------- hook queues (any thread)

struct SpawnReq
{
	u16 weapon = kNoId; // local ids
	u16 parent = kNoId;
	std::string section;
	u16 rounds = 0;
};

std::mutex g_lock;
xr_vector<u16> g_touched; // local ids
xr_vector<SpawnReq> g_spawns;
bool g_scan_now = false;
xr_vector<u16> g_made_here; // client_made_here (a Lua create may run on the game thread): into g_take_base on the main thread
xr_vector<u16> g_forgotten, g_clear_pending;
WeaponFiredFn g_fired = nullptr;

// ---------------------------------------------------------------- owner side (main thread)

xr_map<u16, Blob> g_sent; // own items: last captured/reported state (local id), not an acknowledgement
xr_map<u16, Blob> g_spawn_states; // complete replacement state, consumed after the weapon's native spawn
bool g_was_live = false;
bool g_resync = false;
u64 g_last_scan = 0;
u64 g_last_modify_scan = 0;
u32 g_last_modify = 0;
u32 g_stream = 0; // SpawnAmmo stream id, new per live session
u32 g_seq = 0;

struct Pending
{
	std::string section;
	u8 flags = 0;
	Blob blob;
	u64 time = 0;
};
xr_map<u16, Pending> g_pending; // client: local id

// Client (live report 2026-11-08, GAMMA): our copy of an item right before the host's take put it into our character
// (local id). What our own take callbacks change on it then is ours (GAMMA pins a taken gun at 0.83): it goes to the host
// at once (scan_local) and survives the host's push of its copy (client_apply), which fills in only the rest.
struct TakeBase
{
	ItemData data;
	u64 time = 0;
	bool made_here = false; // client_made_here: our own leased create, every tag is ours (itemtags::push_keeps)
};
xr_map<u16, TakeBase> g_take_base;
constexpr u32 kMadeHereMs = 120000; // a made item's base waits this long for the host's push (its budget queue may hold it)

// ---------------------------------------------------------------- host side (main thread)

struct Owned
{
	PeerId peer = kInvalidPeer;
	ItemData data;
	bool stuck = false; // the host copy does not take this state: do not retry until the next one
};
xr_map<u16, Owned> g_owned; // items of players' characters with the owner's last state
xr_map<u16, PeerId> g_detached_silencers; // fence old owner snapshots until its detach acknowledgement
xr_set<u32> g_logged_tags; // R3 В1: (item id << 8 | itemtags::Bit) already reported as different from the owner's state
struct Left
{
	PeerId peer = kInvalidPeer;
	u64 time = 0;
};
xr_map<u16, Left> g_left; // items that just left a player's character
xr_map<u16, xr_vector<u16>> g_proxy_items; // proxy -> sorted children
xr_map<u16, u32> g_unload_used; // weapon -> rounds spawned from it since its last state
struct Stream
{
	u32 stream = 0;
	u32 seq = 0;
};
xr_map<PeerId, Stream> g_streams;
xr_set<u16> g_releasing; // emptied boxes the host is destroying
u64 g_last_enforce = 0;
bool g_host_precaching = false; // host: a freshly loaded world is being precached

// ---------------------------------------------------------------- helpers

u16 quant(float c) { return u16(iFloor(clampr(c, 0.f, 1.f) * 65535.f + 0.5f)); }
float dequant(u16 q) { return float(q) / 65535.f; }

bool same_str(const shared_str& a, const char* b) { return a.c_str() && b && !xr_strcmp(a.c_str(), b); }

CSE_Abstract* find_entity(u16 id)
{
	if (id == kNoId || !ai().get_alife())
		return nullptr;
	return ai().alife().objects().object(id, true);
}

CInventoryItem* find_item(u16 id)
{
	if (id == kNoId || !g_pGameLevel)
		return nullptr;
	CObject* o = Level().Objects.net_Find(id);
	if (!o || o->getDestroy())
		return nullptr;
	return smart_cast<CInventoryItem*>(o);
}

// Top-level owner of a server entity (itself if it lies in the world).
u16 entity_root(CSE_Abstract* e)
{
	u16 id = e->ID;
	u16 parent = e->ID_Parent;
	for (int depth = 0; parent != kNoId && depth < 8; ++depth)
	{
		id = parent;
		CSE_Abstract* p = find_entity(parent);
		if (!p)
			break;
		parent = p->ID_Parent;
	}
	return id;
}

bool in_list(const char* list, const char* sect)
{
	if (!list || !*list || !sect)
		return false;
	string256 item;
	for (int i = 0, n = _GetItemCount(list); i < n; ++i)
		if (!xr_strcmp(_GetItem(list, i, item), sect))
			return true;
	return false;
}

upgrades::Upgrade describe_upgrade(
    inventory::upgrade::Manager& um, inventory::upgrade::Root* root, const shared_str& id)
{
	upgrades::Upgrade u;
	u.id = id.c_str() ? id.c_str() : "";
	inventory::upgrade::Upgrade* up = id.c_str() ? um.get_upgrade(id) : nullptr;
	u.in_tree = root && up && root->contain_upgrade(id);
	if (!u.in_tree)
		return u;
	u.group = up->parent_group_id().c_str();
	xr_vector<shared_str> needs;
	up->parent_group()->required_groups(needs);
	for (const shared_str& g : needs)
		u.needs_groups.push_back(g.c_str());
	return u;
}

const char* verdict_name(upgrades::Verdict v)
{
	switch (v)
	{
	case upgrades::Verdict::Unknown: return "not in this item's tree";
	case upgrades::Verdict::NoParent: return "its parent group has no upgrade (yet)";
	case upgrades::Verdict::GroupTaken: return "another upgrade of its group is there";
	default: return "?";
	}
}

// The section's installed_upgrades, read like Manager::init_install does (only for an item with an upgrade tree).
UpgradeList section_default_upgrades(inventory::upgrade::Manager& um, const shared_str& section)
{
	UpgradeList out;
	if (!um.get_root(section) || !pSettings->line_exist(section, "installed_upgrades"))
		return out;
	LPCSTR list = pSettings->r_string(section, "installed_upgrades");
	if (!list)
		return out;
	string256 item;
	for (int i = 0, n = _GetItemCount(list); i < n; ++i)
		out.push_back(shared_str(_GetItem(list, i, item)));
	return out;
}

// R3 В6 (review): the upgrades of `wanted` the engine can install on an item of `section` in this order. A loading
// install (net_Spawn of a new entity, ItemState) is FATAL on a missing parent group or a taken group
// (inventory::upgrade::Group::can_install), so those are dropped here, like unknown ones. `report`: count and log
// the first 10 dropped ones.
//   fresh = false: `installed` is what the item has; the result is what is left to install.
//   fresh = true:  the list goes onto a new entity, whose net_Spawn puts the section's installed_upgrades on first
//                  (Manager::init_install): those count, and the ones the list names stay in it (a game object's
//                  list has them, an entity's does not); a repeat is dropped.
UpgradeList checked_upgrades(
    const shared_str& section, const UpgradeList& installed, const UpgradeList& wanted, bool fresh, bool report)
{
	UpgradeList out;
	if (!ai().get_alife())
		return out;
	inventory::upgrade::Manager& um = ai().alife().inventory_upgrade_manager();
	inventory::upgrade::Root* root = um.get_root(section);
	std::vector<upgrades::Upgrade> have, want, defaults;
	for (const shared_str& id : installed)
		have.push_back(describe_upgrade(um, root, id));
	for (const shared_str& id : wanted)
		want.push_back(describe_upgrade(um, root, id));
	if (fresh)
		for (const shared_str& id : section_default_upgrades(um, section))
			defaults.push_back(describe_upgrade(um, root, id));
	const std::vector<upgrades::Verdict> verdicts = upgrades::plan(have, want, defaults);
	for (size_t i = 0; i < verdicts.size(); ++i)
	{
		const upgrades::Verdict v = verdicts[i];
		if (v == upgrades::Verdict::Install || v == upgrades::Verdict::Default)
			out.push_back(wanted[i]);
		else if (v != upgrades::Verdict::Installed && report && ++g_stats.upgrades_dropped <= 10)
			::Msg("! [coop] upgrade [%s] of [%s] dropped: %s",
			    validate::resource_name_ok(want[i].id.c_str(), 64) ? want[i].id.c_str() : "<bad name>",
			    section.c_str(), verdict_name(v));
	}
	return out;
}

// A list for a new entity of `section` (its net_Spawn installs it after the section defaults).
UpgradeList spawn_upgrade_list(const shared_str& section, const UpgradeList& wanted, bool report)
{
	return checked_upgrades(section, {}, wanted, true, report);
}

// ---------------------------------------------------------------- blob

void put(Writer& w, Tag tag, const Writer& payload)
{
	w.u8(tag);
	w.varu(payload.size());
	if (payload.size())
		w.bytes(payload.ptr(), payload.size());
}

void write_data(const ItemData& d, Blob& out)
{
	Writer w(64);
	Writer p(32);
	w.u8(kBlobVersion);
	if (d.has_cond)
	{
		p.clear();
		p.u16(d.cond);
		put(w, TagCondition, p);
	}
	if (d.has_upgrades)
	{
		p.clear();
		const u32 n = _min(u32(d.upgrades.size()), u32(255));
		p.u8(u8(n));
		for (u32 i = 0; i < n; ++i)
		{
			const char* s = d.upgrades[i].c_str();
			p.str(s ? s : "");
		}
		put(w, TagUpgrades, p);
	}
	if (d.has_box)
	{
		p.clear();
		p.u16(d.box);
		put(w, TagBox, p);
	}
	if (d.has_addons)
	{
		p.clear();
		p.u8(d.addons);
		put(w, TagAddons, p);
	}
	if (d.has_scope)
	{
		p.clear();
		p.u8(d.scope);
		put(w, TagScope, p);
	}
	if (d.has_main)
	{
		p.clear();
		p.u16(d.main_count);
		p.u8(d.main_type);
		put(w, TagMain, p);
	}
	if (d.has_launcher)
	{
		p.clear();
		p.u16(d.gl_count);
		p.u8(d.gl_type);
		put(w, TagLauncher, p);
	}
	if (d.has_uses)
	{
		p.clear();
		p.u8(d.uses);
		put(w, TagUses, p);
	}
	if (d.has_mode)
	{
		p.clear();
		p.u8(d.grenade_mode ? 1 : 0);
		put(w, TagMode, p);
	}
	if (d.has_fire_mode)
	{
		p.clear();
		p.u8(d.fire_mode);
		put(w, TagFireMode, p);
	}
	if (d.has_cartridges)
	{
		p.clear();
		for (u8 type : d.cartridges)
			p.u8(type);
		put(w, TagCartridges, p);
	}
	out = w.data();
}

bool read_data(const u8* data, size_t size, ItemData& d)
{
	d = ItemData();
	if (!data || !size)
		return false;
	Reader r(data, size);
	if (r.u8() != kBlobVersion)
		return false;
	while (r.ok() && r.remaining() > 0)
	{
		const u8 tag = r.u8();
		const u64 len = r.varu();
		if (!r.ok() || len > r.remaining())
			return false;
		Reader p(r.cursor(), size_t(len));
		r.skip(size_t(len));
		switch (tag)
		{
		case TagCondition:
			d.cond = p.u16();
			d.has_cond = p.ok();
			break;
		case TagUpgrades:
		{
			const u8 n = p.u8();
			for (u8 i = 0; i < n && p.ok(); ++i)
			{
				const std::string s = p.str();
				if (p.ok() && !s.empty())
					d.upgrades.push_back(shared_str(s.c_str()));
			}
			d.has_upgrades = p.ok();
			break;
		}
		case TagBox:
			d.box = p.u16();
			d.has_box = p.ok();
			break;
		case TagAddons:
			d.addons = p.u8();
			d.has_addons = p.ok();
			break;
		case TagScope:
			d.scope = p.u8();
			d.has_scope = p.ok();
			break;
		case TagMain:
			d.main_count = p.u16();
			d.main_type = p.u8();
			d.has_main = p.ok();
			break;
		case TagLauncher:
			d.gl_count = p.u16();
			d.gl_type = p.u8();
			d.has_launcher = p.ok();
			break;
		case TagUses:
			d.uses = p.u8();
			d.has_uses = p.ok();
			break;
		case TagMode:
		{
			const u8 mode = p.u8();
			if (!p.ok() || mode > 1 || p.remaining())
				return false;
			d.has_mode = true;
			d.grenade_mode = mode != 0;
			break;
		}
		case TagFireMode:
			d.fire_mode = p.u8();
			d.has_fire_mode = p.ok() && !p.remaining();
			if (!d.has_fire_mode)
				return false;
			break;
		case TagCartridges:
			d.has_cartridges = true;
			d.cartridges.assign(p.cursor(), p.cursor() + size_t(len));
			break;
		default:
			break; // newer field: skipped by length
		}
	}
	return r.ok();
}

bool same_upgrades(const UpgradeList& a, const UpgradeList& b)
{
	if (a.size() != b.size())
		return false;
	for (const shared_str& s : a)
		if (std::find(b.begin(), b.end(), s) == b.end())
			return false;
	return true;
}

// R3 В1: tags (itemtags::Bit) of the host copy `cur` that differ from the owner's state. Semantic comparison (upgrade
// order does not matter). Only the tags the owner carries count: the old symmetric comparison made every weapon
// without scopes "stuck" over a scope tag the owner never sent.
u32 diverged_tags(const ItemData& cur, const ItemData& owner)
{
	return itemtags::diverged(cur, owner, same_upgrades);
}

// R3 В1: a tag's value for the log ("none" = the state does not carry it).
std::string describe_tag(itemtags::Bit bit, const ItemData& d)
{
	char buf[96];
	buf[0] = 0;
	switch (bit)
	{
	case itemtags::Cond:
		if (d.has_cond)
			xr_sprintf(buf, "%u/65535", u32(d.cond));
		break;
	case itemtags::Upgrades:
	{
		if (!d.has_upgrades)
			break;
		std::string s = "[";
		for (const shared_str& id : d.upgrades)
		{
			if (s.size() > 1)
				s += ' ';
			s += id.c_str() ? id.c_str() : "?";
			if (s.size() > 200)
			{
				s += "...";
				break;
			}
		}
		return s + "]";
	}
	case itemtags::Box:
		if (d.has_box)
			xr_sprintf(buf, "%u rounds", u32(d.box));
		break;
	case itemtags::Addons:
		if (d.has_addons)
			xr_sprintf(buf, "0x%02x", u32(d.addons));
		break;
	case itemtags::Scope:
		if (d.has_scope)
			xr_sprintf(buf, "scope %u", u32(d.scope));
		break;
	case itemtags::Main:
		if (d.has_main)
			xr_sprintf(buf, "%u rounds, type %u", u32(d.main_count), u32(d.main_type));
		break;
	case itemtags::Launcher:
		if (d.has_launcher)
			xr_sprintf(buf, "%u rounds, type %u", u32(d.gl_count), u32(d.gl_type));
		break;
	case itemtags::Uses:
		if (d.has_uses)
			xr_sprintf(buf, "%u portions", u32(d.uses));
		break;
	default:
		break;
	}
	return buf[0] ? std::string(buf) : std::string("none");
}

// R3 В1: a host copy that does not take the owner's state is reported by tag, once per item and tag (a stuck item
// is retried with every new state of the owner: the log must not repeat). Forgotten when the item leaves g_owned.
void log_diverged(u16 id, const char* section, u32 mask, const ItemData& cur, const ItemData& owner)
{
	bool headed = false;
	for (u32 bit = 1; bit <= itemtags::Uses; bit <<= 1)
	{
		if (!(mask & bit) || !g_logged_tags.insert((u32(id) << 8) | bit).second)
			continue;
		if (!headed) // F0 final review (task 6): the item's line only with a tag not reported yet, not on every retry
		{
			headed = true;
			::Msg("! [coop] item %u [%s]: the host copy does not take its owner's state", u32(id), section);
		}
		::Msg("! [coop] item %u [%s]: the host copy keeps its own %s after the owner's state was applied: host %s, owner %s",
		    u32(id), section, itemtags::name(itemtags::Bit(bit)), describe_tag(itemtags::Bit(bit), cur).c_str(),
		    describe_tag(itemtags::Bit(bit), owner).c_str());
	}
}

// ---------------------------------------------------------------- capture

// `upgrades`: false for a digest, which does not read them (the list is copied for every item of a bucket otherwise).
void capture_object(CInventoryItem* item, ItemData& d, bool upgrades = true)
{
	d = ItemData();
	d.has_cond = true;
	d.cond = quant(item->GetCondition());
	d.has_upgrades = upgrades;
	if (upgrades)
		d.upgrades = item->get_upgrades();

	if (CWeaponAmmo* box = smart_cast<CWeaponAmmo*>(item))
	{
		d.has_box = true;
		d.box = box->m_boxCurr;
	}
	if (CWeapon* w = smart_cast<CWeapon*>(item))
	{
		d.has_addons = true;
		d.addons = w->GetAddonsState();
		// R3 В1: no scope tag for a weapon without scopes (pistols, knife): has_scope stays untouched (false), the
		// same as in the owner's state after host_sanitize.
		if (itemtags::scope_tag_applies(w->m_scopes.size()))
		{
			d.has_scope = true;
			d.scope = w->m_cur_scope;
		}
		CWeaponMagazinedWGrenade* g = smart_cast<CWeaponMagazinedWGrenade*>(w);
		if (g)
		{
			d.has_mode = true;
			d.grenade_mode = g->m_bGrenadeMode;
		}
		const u16 cur_count = u16(_max(0, w->GetAmmoElapsed()));
		d.has_main = true;
		if (g && g->m_bGrenadeMode)
		{
			d.main_count = u16(g->m_magazine2.size());
			d.main_type = g->m_ammoType2;
			d.has_launcher = true;
			d.gl_count = cur_count;
			d.gl_type = w->m_ammoType;
		}
		else
		{
			d.main_count = cur_count;
			d.main_type = w->m_ammoType;
			if (g)
			{
				d.has_launcher = true;
				d.gl_count = u16(g->m_magazine2.size());
				d.gl_type = g->m_ammoType2;
			}
		}
	}
	if (CEatableItem* e = smart_cast<CEatableItem*>(item))
	{
		d.has_uses = true;
		d.uses = e->GetRemainingUses();
	}
}

// Entity state, using the live WGL object when its current mode hides the rifle magazine in the CSE.
// Portions still require a game object.
bool capture_entity(CSE_Abstract* e, ItemData& d, bool upgrades = true)
{
	d = ItemData();
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(e);
	if (!ii)
		return false;
	d.has_cond = true;
	d.cond = quant(ii->m_fCondition);
	d.has_upgrades = upgrades;
	if (upgrades)
		d.upgrades = ii->m_upgrades;
	if (CSE_ALifeItemAmmo* a = smart_cast<CSE_ALifeItemAmmo*>(e))
	{
		d.has_box = true;
		d.box = a->a_elapsed;
	}
	if (CSE_ALifeItemWeapon* w = smart_cast<CSE_ALifeItemWeapon*>(e))
	{
		d.has_addons = true;
		d.addons = w->m_addon_flags.get();
		CSE_ALifeItemWeaponMagazinedWGL* g = smart_cast<CSE_ALifeItemWeaponMagazinedWGL*>(e);
		if (g)
		{
			d.has_mode = true;
			d.grenade_mode = g->m_bGrenadeMode;
			if (CWeaponMagazinedWGrenade* live = smart_cast<CWeaponMagazinedWGrenade*>(find_item(e->ID)))
			{
				capture_object(live, d, upgrades); // normalizes both magazines regardless of the active mode
				if (upgrades)
					d.upgrades = ii->m_upgrades; // the entity retains authority over installed upgrades
				return true;
			}
			d.has_launcher = true;
			d.gl_count = g->m_bGrenadeMode ? w->a_elapsed : w->a_elapsed_grenades.grenades_count;
			d.gl_type = g->m_bGrenadeMode ? w->ammo_type : w->a_elapsed_grenades.grenades_type;
		}
		if (!g || !g->m_bGrenadeMode)
		{
			d.has_main = true;
			d.main_count = w->a_elapsed;
			d.main_type = w->ammo_type;
		}
	}
	return true;
}

void capture_blob(CInventoryItem* item, Blob& out)
{
	ItemData d;
	capture_object(item, d);
	write_data(d, out);
}

// ---------------------------------------------------------------- apply

void install_upgrades(CInventoryItem* item, const UpgradeList& list)
{
	if (!ai().get_alife())
		return;
	const shared_str& section = item->object().cNameSect();
	inventory::upgrade::Manager& um = ai().alife().inventory_upgrade_manager();
	// R3 В6: only what Group::can_install accepts in this order — a loading install is FATAL otherwise
	for (const shared_str& id : checked_upgrades(section, item->get_upgrades(), list, false, true))
		um.upgrade_install(*item, id, true); // "loading": no unload/detach, no event
}

// Magazine of the current launcher mode.
// An ammo type unknown here (different upgrade state) keeps ours: the round count still counts.
void set_current(CWeapon* w, u16 count, u8 type)
{
	if (w->m_ammoTypes.empty())
		return;
	if (type >= w->m_ammoTypes.size())
		type = w->m_ammoType < w->m_ammoTypes.size() ? w->m_ammoType : u8(0);
	const bool retype =
	    w->m_ammoType != type || (!w->m_magazine.empty() && w->m_magazine.back().m_LocalAmmoType != type);
	if (!retype && w->GetAmmoElapsed() == int(count) && w->m_magazine.size() == count)
		return;
	if (retype)
	{
		w->m_ammoType = type;
		w->m_magazine.clear();
	}
	w->SetAmmoElapsed(int(count));
}

// The other magazine of a weapon with a grenade launcher.
void set_other(CWeaponMagazinedWGrenade* g, u16 count, u8 type)
{
	if (g->m_ammoTypes2.empty())
		return;
	if (type >= g->m_ammoTypes2.size())
		type = g->m_ammoType2 < g->m_ammoTypes2.size() ? g->m_ammoType2 : u8(0);
	const bool retype =
	    g->m_ammoType2 != type || (!g->m_magazine2.empty() && g->m_magazine2.back().m_LocalAmmoType != type);
	if (!retype && g->m_magazine2.size() == count)
		return;
	if (retype)
	{
		g->m_ammoType2 = type;
		g->m_magazine2.clear();
	}
	g->SetAmmoElapsed2(int(count));
}

// `ammo_only`: only rounds (weapon magazines, box count) — for world items a player unloaded.
void apply_object(CInventoryItem* item, const ItemData& d, bool ammo_only)
{
	if (!ammo_only)
	{
		if (d.has_cond && quant(item->GetCondition()) != d.cond)
			item->SetCondition(dequant(d.cond));
		if (d.has_upgrades)
			install_upgrades(item, d.upgrades);
		if (d.has_uses)
			if (CEatableItem* e = smart_cast<CEatableItem*>(item))
				e->SetRemainingUses(d.uses);
	}
	if (d.has_box)
		if (CWeaponAmmo* box = smart_cast<CWeaponAmmo*>(item))
		{
			const u16 n = _min(d.box, box->m_boxSize);
			if (box->m_boxCurr != n)
			{
				box->m_boxCurr = n;
				if (box->m_pInventory)
					box->m_pInventory->InvalidateState();
			}
		}

	CWeapon* w = smart_cast<CWeapon*>(item);
	if (!w)
		return;
	if (!ammo_only && (d.has_addons || d.has_scope))
	{
		u8 flags = d.has_addons ? d.addons : w->GetAddonsState();
		if (w->m_scopes.empty() && w->ScopeAttachable())
			flags &= ~u8(CSE_ALifeItemWeapon::eWeaponAddonScope); // InitAddons would index m_scopes
		const u8 scope = (d.has_scope && d.scope < w->m_scopes.size()) ? d.scope : w->m_cur_scope;
		if (flags != w->GetAddonsState() || scope != w->m_cur_scope)
		{
			w->m_cur_scope = scope;
			w->SetAddonsState(flags); // same as GE_ADDON_CHANGE
			w->UpdateAddonsVisibility();
			w->InitAddons();
		}
	}
	CWeaponMagazinedWGrenade* g = smart_cast<CWeaponMagazinedWGrenade*>(w);
	const bool gl_mode = g && g->m_bGrenadeMode;
	if (d.has_main)
	{
		if (gl_mode)
			set_other(g, d.main_count, d.main_type);
		else
			set_current(w, d.main_count, d.main_type);
	}
	if (d.has_launcher && g)
	{
		if (gl_mode)
			set_current(w, d.gl_count, d.gl_type);
		else
			set_other(g, d.gl_count, d.gl_type);
	}
	w->ForceUpdateAmmo();
}

void apply_entity(CSE_Abstract* e, const ItemData& d, bool ammo_only)
{
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(e);
	if (!ii)
		return;
	if (!ammo_only)
	{
		if (d.has_cond)
			ii->m_fCondition = dequant(d.cond);
		// Not on the host: there the owner's GE_INSTALL_UPGRADE (relayed by WP2) adds it to the
		// server entity, and CSE_ALifeInventoryItem::add_upgrade is FATAL on a duplicate.
		if (d.has_upgrades && !is_map_authority())
			ii->m_upgrades = spawn_upgrade_list(e->s_name, d.upgrades, false); // net_Spawn installs them
	}
	if (d.has_box)
		if (CSE_ALifeItemAmmo* a = smart_cast<CSE_ALifeItemAmmo*>(e))
			a->a_elapsed = _min(d.box, a->m_boxSize);

	CSE_ALifeItemWeapon* w = smart_cast<CSE_ALifeItemWeapon*>(e);
	if (!w)
		return;
	if (d.has_fire_mode)
		if (CSE_ALifeItemWeaponMagazined* magazined = smart_cast<CSE_ALifeItemWeaponMagazined*>(e))
			magazined->m_u8CurFireMode = d.fire_mode;
	if (d.has_cartridges)
		if (CSE_ALifeItemWeaponShotGun* shotgun = smart_cast<CSE_ALifeItemWeaponShotGun*>(e))
			shotgun->m_AmmoIDs.assign(d.cartridges.begin(), d.cartridges.end());
	if (!ammo_only && d.has_addons)
		w->m_addon_flags.assign(d.addons);
	// a_elapsed / ammo_type describe the magazine of the current launcher mode (the online
	// object's export keeps m_bGrenadeMode in step with its own mode).
	CSE_ALifeItemWeaponMagazinedWGL* g = smart_cast<CSE_ALifeItemWeaponMagazinedWGL*>(e);
	if (g && g->m_bGrenadeMode)
	{
		if (d.has_launcher)
		{
			w->a_elapsed = d.gl_count;
			w->ammo_type = d.gl_type;
		}
	}
	else
	{
		if (d.has_main)
		{
			w->a_elapsed = d.main_count;
			w->ammo_type = d.main_type;
		}
		if (g && d.has_launcher)
		{
			w->a_elapsed_grenades.grenades_count = u8(_min<u16>(d.gl_count, 63));
			w->a_elapsed_grenades.grenades_type = u8(d.gl_type & 3);
		}
	}
}

// ---------------------------------------------------------------- messages

void write_state(Writer& w, u16 host_id, const char* section, u8 flags, const Blob& blob)
{
	w.u8(u8(Op::State));
	w.u16(host_id);
	w.str(section ? section : "");
	w.u8(flags);
	w.varu(blob.size());
	if (!blob.empty())
		w.bytes(blob.data(), blob.size());
}

// Own item (or a world item this machine changed): client -> host, host -> all.
void send_state(CInventoryItem* item, const Blob& blob)
{
	Writer w(blob.size() + 48);
	write_state(w, join::to_host(item->object_id()), item->object().cNameSect().c_str(), 0, blob);
	if (is_map_authority())
		net::host_broadcast(Msg::ItemState, w);
	else
		net::client_send(Msg::ItemState, w);
}

void reject(Reason why, PeerId from, u16 id, const char* detail)
{
	++g_stats.rejected[u32(why)];
	::Msg("! [coop] item state from peer %u for %u refused: %s%s%s", from, id, reason_name(why), detail ? ", " : "",
	    detail ? detail : "");
}

// ---------------------------------------------------------------- owner side

// Client: the tags of our item `id` that we changed ourselves since the host's take put it into our character (our take
// callbacks), and the copy from before the take. 0: no take base, or nothing changed.
// Client: our own creates (client_made_here) get their take base, on the main thread.
void drain_forgotten()
{
	xr_vector<u16> gone, pending;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		gone.swap(g_forgotten);
		pending.swap(g_clear_pending);
	}
	for (u16 id : pending)
		g_pending.erase(id);
	for (u16 id : gone)
	{
		g_sent.erase(id);
		g_pending.erase(id);
		g_take_base.erase(id);
		g_owned.erase(id);
		g_detached_silencers.erase(id);
		g_left.erase(id);
		g_unload_used.erase(id);
		g_releasing.erase(id);
		for (auto& proxy : g_proxy_items)
			proxy.second.erase(std::remove(proxy.second.begin(), proxy.second.end(), id), proxy.second.end());
		for (auto it = g_logged_tags.begin(); it != g_logged_tags.end();)
			it = u16(*it >> 8) == id ? g_logged_tags.erase(it) : std::next(it);
	}
}

void drain_made_here()
{
	drain_forgotten();
	xr_vector<u16> made;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		made.swap(g_made_here);
	}
	for (u16 id : made)
	{
		TakeBase& b = g_take_base[id];
		b.data = ItemData();
		b.made_here = true;
		b.time = mgr().now_ms();
	}
}

u32 take_changes(u16 id, const ItemData& now)
{
	auto it = g_take_base.find(id);
	if (it == g_take_base.end() || it->second.made_here)
		return 0; // a made item: the host does not have it yet, ours goes with the answer to its push (client_apply)
	return itemtags::changed_since(now, it->second.data, same_upgrades) & itemtags::kOwnerTakeTags;
}

// The tags we keep over the host's push of item `id` (2026-11-08): all of an item we made, the take's changes of a taken one.
u32 push_keeps(u16 id, const ItemData& now)
{
	auto it = g_take_base.find(id);
	if (it == g_take_base.end())
		return 0;
	return itemtags::push_keeps(it->second.made_here,
	    it->second.made_here ? 0u : itemtags::changed_since(now, it->second.data, same_upgrades));
}

void scan_local(bool send_all)
{
	CActor* actor = Actor();
	if (!actor || actor->getDestroy())
		return;
	const bool host = is_map_authority();
	xr_set<u16> present;
	Blob blob;
	for (PIItem item : actor->inventory().m_all)
	{
		if (!item || item->object().getDestroy())
			continue;
		const u16 id = item->object_id();
		present.insert(id);
		if (!host && requests::client_lease_waiting(id))
			continue;
		capture_blob(item, blob);
		auto it = g_sent.find(id);
		if (it == g_sent.end())
		{
			// New in our hands. A client got it from the host (same state there); the host
			// refreshes the clients' copies (NPC loot can be stale on clients).
			g_sent.emplace(id, blob);
			if (send_all || host)
			{
				send_state(item, blob);
				++g_stats.sent;
			}
			else
			{
				// Live report 2026-11-08: what our take callbacks changed (GAMMA pins a looted gun at 0.83) is ours,
				// the host never saw it. Only those tags go: the rest of our copy may be stale (an NPC's magazine).
				ItemData now;
				capture_object(item, now);
				if (const u32 mine = take_changes(id, now))
				{
					Blob part;
					write_data(itemtags::only_tags(now, mine), part);
					send_state(item, part);
					++g_stats.sent;
					++g_stats.take_kept;
				}
			}
		}
		else if (it->second != blob)
		{
			it->second = blob;
			send_state(item, blob);
			++g_stats.sent;
		}
	}
	// Items that left our hands: their last state still goes out (the host accepts it for a
	// short while), e.g. a gun unloaded and put into a stash in the same moment.
	for (auto it = g_sent.begin(); it != g_sent.end();)
	{
		if (present.count(it->first))
		{
			++it;
			continue;
		}
		if (CInventoryItem* item = find_item(it->first))
		{
			capture_blob(item, blob);
			if (blob != it->second)
			{
				send_state(item, blob);
				++g_stats.sent;
			}
		}
		g_take_base.erase(it->first);
		it = g_sent.erase(it);
	}
}

// Touched items that are not ours: a corpse's / stash's / ground gun the player unloaded.
void send_foreign(xr_vector<u16>& touched)
{
	if (touched.empty())
		return;
	std::sort(touched.begin(), touched.end());
	touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
	CActor* actor = Actor();
	Blob blob;
	for (u16 id : touched)
	{
		CInventoryItem* item = find_item(id);
		if (!item || (actor && item->object().H_Root() == actor))
			continue;
		// NPCs reloading on the host: their things are pushed when a player gets them.
		const CEntityAlive* holder = smart_cast<const CEntityAlive*>(item->object().H_Root());
		if (holder && holder->g_Alive())
			continue;
		capture_blob(item, blob);
		send_state(item, blob);
		++g_stats.foreign_sent;
	}
}

void send_spawns(xr_vector<SpawnReq>& spawns)
{
	for (const SpawnReq& s : spawns)
	{
		Writer w(64);
		w.u8(u8(Op::SpawnAmmo));
		w.u32(g_stream);
		w.u32(++g_seq);
		w.u16(join::to_host(s.weapon));
		w.u16(join::to_host(s.parent));
		w.str(s.section);
		w.u16(s.rounds);
		net::client_send(Msg::ItemState, w);
		++g_stats.spawns_sent;
	}
}

// Client: a state from the host (relayed from another player, or host authority).
// Returns false if the entity does not exist yet.
bool client_apply(u16 id, const char* section, u8 flags, const Blob& blob)
{
	drain_made_here();
	if (requests::client_lease_waiting(id))
		return true; // the initial replacement envelope is the only state allowed across this fence
	CSE_Abstract* e = find_entity(id);
	if (!e)
		return false;
	ItemData d;
	if (!same_str(e->s_name, section) || !read_data(blob.data(), blob.size(), d))
	{
		++g_stats.ignored;
		return true;
	}
	CInventoryItem* item = find_item(id);
	if (entity_root(e) == 0)
	{
		// Our own item: we are the owner. Only the host's state for an item that just came into
		// our hands is taken, and only if we have not changed it since (else ours goes out).
		if (!(flags & kFlagAuthority))
		{
			++g_stats.ignored;
			return true;
		}
		auto base = g_take_base.find(id);
		if (!item && base != g_take_base.end() && base->second.made_here)
		{
			++g_stats.authority_skipped; // our made item is not online here yet: its entity keeps our state
			return true;
		}
		if (item && base != g_take_base.end())
		{
			// Live report 2026-11-08 (GAMMA): the push of an item the host's take just gave us. What we changed
			// ourselves since that take (GAMMA pinned the looted gun at 0.83) stays ours; the rest is the host's copy,
			// fresher than ours (an NPC's magazine). Before, the whole push won: the gun went red in our hands.
			// The same day: an item we made ourselves (a scope's clone) keeps every tag but the upgrade list - the host
			// built it from its own view of the source (7 % instead of our 83 %).
			ItemData now;
			capture_object(item, now);
			const u32 mine = push_keeps(id, now);
			g_take_base.erase(base);
			const ItemData merged = itemtags::take_push(d, now, mine);
			apply_object(item, merged, false);
			apply_entity(e, merged, false);
			Blob cur;
			capture_blob(item, cur);
			if (mine)
			{
				send_state(item, cur); // the host's copy takes ours (it may have pushed before our first scan's part came)
				++g_stats.sent;
				++g_stats.take_kept;
			}
			g_sent[id] = cur;
			++g_stats.authority_applied;
			return true;
		}
		// g_sent is the last report, not an unchanged-since-take baseline. Once we track
		// this ownership, a delayed/repeated host push must not refund spent ammunition.
		// A fresh take's baseline is handled above, including its locally changed tags.
		// Protect the entity too when its online object is temporarily absent.
		auto it = g_sent.find(id);
		if (it != g_sent.end())
			d = itemtags::only_tags(d, itemtags::kAll &
			    ~u32(itemtags::Box | itemtags::Main | itemtags::Launcher));
		if (item)
		{
			Blob cur;
			capture_blob(item, cur);
			if (it != g_sent.end() && it->second != cur)
			{
				++g_stats.authority_skipped;
				return true;
			}
			apply_object(item, d, false);
			capture_blob(item, cur);
			g_sent[id] = cur;
		}
		apply_entity(e, d, false);
		++g_stats.authority_applied;
		return true;
	}
	if (item)
		apply_object(item, d, false);
	apply_entity(e, d, false);
	++g_stats.applied;
	trade::client_on_item_state(entity_root(e)); // F1: the bars and prices of an open window
	return true;
}

void client_on_state(Reader& r)
{
	const u16 id = join::to_local(r.u16());
	const std::string section = r.str();
	const u8 flags = r.u8();
	const u64 n = r.varu();
	if (!r.ok() || n > kMaxBlob || n > r.remaining())
		return;
	Blob blob(r.cursor(), r.cursor() + size_t(n));
	++g_stats.received;
	if (mgr().is_live_client() && client_apply(id, section.c_str(), flags, blob))
		return;
	Pending& p = g_pending[id]; // P6: deferred until the entity exists / we are live
	p.section = section;
	p.flags = flags;
	p.blob.swap(blob);
	p.time = mgr().now_ms();
}

void process_pending(u64 now)
{
	drain_made_here();
	for (auto it = g_pending.begin(); it != g_pending.end();)
	{
		if (client_apply(it->first, it->second.section.c_str(), it->second.flags, it->second.blob))
			it = g_pending.erase(it);
		else if (now - it->second.time > kPendingMs)
		{
			++g_stats.pending_dropped;
			it = g_pending.erase(it);
		}
		else
			++it;
	}
	// A take whose push came before the take itself (applied to a world item then) waits for none.
	for (auto it = g_take_base.begin(); it != g_take_base.end();)
		it = now - it->second.time > (it->second.made_here ? kMadeHereMs : kTakeBaseMs) ? g_take_base.erase(it)
		                                                                                  : std::next(it);
}

// ---------------------------------------------------------------- host side

enum class Access : u8
{
	Denied,
	Owned, // in the sender's character
	Late, // left the sender's character a moment ago, not taken by another player
	Foreign, // a world item near the sender (corpse, stash, ground): rounds only
};

Access host_access(PeerId from, u16 proxy, CSE_Abstract* e)
{
	const u16 root = entity_root(e);
	if (root == proxy)
		return Access::Owned;
	if (root == 0 || join::is_proxy(root))
		return Access::Denied; // the host's or another player's
	auto it = g_left.find(e->ID);
	if (it != g_left.end() && it->second.peer == from && mgr().now_ms() - it->second.time <= kGraceMs)
		return Access::Late;
	// Left in this very frame (WP2 applied the move before host_track_proxies noticed it).
	auto pit = g_proxy_items.find(proxy);
	if (pit != g_proxy_items.end() && std::binary_search(pit->second.begin(), pit->second.end(), e->ID))
		return Access::Late;
	CSE_Abstract* r = find_entity(root);
	if (CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(r))
		if (c->g_Alive())
			return Access::Denied; // a living creature's things are not for players to change
	CSE_Abstract* p = find_entity(proxy);
	if (!p || !r || p->o_Position.distance_to(r->o_Position) > kForeignRange)
		return Access::Denied;
	return Access::Foreign;
}

struct Caps
{
	u32 main_cap = 0, gl_cap = 0, main_types = 0, gl_types = 0, scopes = 0;
};

void weapon_caps(CSE_ALifeItemWeapon* we, CInventoryItem* item, Caps& c)
{
	const u32 cfg_mag = we->get_ammo_magsize();
	c.main_cap = cfg_mag * 2 + 1; // upgrades may enlarge the magazine
	c.gl_cap = kLauncherCap;
	c.main_types = we->m_caAmmoSections ? u32(_GetItemCount(we->m_caAmmoSections)) : 0;
	c.gl_types = u32(_GetItemCount(READ_IF_EXISTS(pSettings, r_string, we->s_name, "grenade_class", "")));
	c.scopes = 255;
	CWeapon* w = item ? smart_cast<CWeapon*>(item) : nullptr;
	if (!w)
		return;
	CWeaponMagazinedWGrenade* g = smart_cast<CWeaponMagazinedWGrenade*>(w);
	const bool gl_mode = g && g->m_bGrenadeMode;
	u32 live = u32(_max(0, w->GetAmmoMagSize()));
	if (g)
		live = _max(live, u32(_max(0, g->GetAmmoMagSize2())));
	c.main_cap = _max(c.main_cap, live);
	c.main_types = u32(gl_mode ? g->m_ammoTypes2.size() : w->m_ammoTypes.size());
	c.gl_types = g ? u32(gl_mode ? w->m_ammoTypes.size() : g->m_ammoTypes2.size()) : 0;
	c.scopes = u32(w->m_scopes.size());
}

// Refused: the fields do not fit the kind of item.
bool host_validate_kind(CSE_Abstract* e, const ItemData& d, const char*& why)
{
	if (d.has_box)
	{
		CSE_ALifeItemAmmo* a = smart_cast<CSE_ALifeItemAmmo*>(e);
		if (!a)
		{
			why = "box count for a non-ammo item";
			return false;
		}
		if (d.box > a->m_boxSize)
		{
			why = "more rounds than the box holds";
			return false;
		}
	}
	if (d.has_mode && !smart_cast<CSE_ALifeItemWeaponMagazinedWGL*>(e))
	{
		why = "launcher mode for a non-launcher";
		return false;
	}
	if (d.has_fire_mode && !smart_cast<CSE_ALifeItemWeaponMagazined*>(e))
	{
		why = "fire mode for a non-magazined weapon";
		return false;
	}
	const bool weapon_fields = d.has_main || d.has_launcher || d.has_addons || d.has_scope || d.has_cartridges;
	if (weapon_fields && !smart_cast<CSE_ALifeItemWeapon*>(e))
	{
		why = "weapon state for a non-weapon";
		return false;
	}
	return true;
}

// Refused: counts no item of this kind can have (rounds or portions out of nothing). The host copy has the upgrades the
// host sold (F2 T5): a magazine an upgrade enlarged fits the slack of the cap.
bool host_validate_counts(CSE_Abstract* e, CInventoryItem* item, const ItemData& d, const char*& why)
{
	if (d.has_uses && item)
		if (CEatableItem* ei = smart_cast<CEatableItem*>(item))
			if (d.uses > ei->GetMaxUses())
			{
				why = "more portions than the item has";
				return false;
			}
	CSE_ALifeItemWeapon* we = smart_cast<CSE_ALifeItemWeapon*>(e);
	if (!we)
		return true;
	Caps c;
	weapon_caps(we, item, c);
	if (d.has_main && d.main_count > c.main_cap)
	{
		why = "more rounds than the magazine holds";
		return false;
	}
	if (d.has_launcher && d.gl_count > c.gl_cap)
	{
		why = "more rounds than the launcher holds";
		return false;
	}
	if (d.has_cartridges)
	{
		if (!d.has_main || d.cartridges.size() != d.main_count)
		{
			why = "cartridge order does not match the magazine";
			return false;
		}
		for (u8 type : d.cartridges)
			if (type >= c.main_types)
			{
				why = "unknown cartridge type";
				return false;
			}
	}
	return true;
}

// Descriptive fields (upgrade list, ammo type index, scope index) are cleaned, not refused: a
// refused state would leave the host with stale round counts.
void host_sanitize(CSE_Abstract* e, CInventoryItem* item, ItemData& d)
{
	// The owner's list is its item's, from the spawn on: section defaults first (R3 В6: an order the engine takes).
	if (d.has_upgrades)
		d.upgrades = spawn_upgrade_list(e->s_name, d.upgrades, false);
	CSE_ALifeItemWeapon* we = smart_cast<CSE_ALifeItemWeapon*>(e);
	if (!we)
		return;
	Caps c;
	weapon_caps(we, item, c);
	ItemData cur;
	if (item)
		capture_object(item, cur);
	else
		capture_entity(e, cur);
	if (d.has_main && d.main_type >= c.main_types)
		d.main_type = cur.has_main && cur.main_type < c.main_types ? cur.main_type : u8(0);
	if (d.has_launcher && d.gl_type >= c.gl_types)
		d.gl_type = cur.has_launcher && cur.gl_type < c.gl_types ? cur.gl_type : u8(0);
	if (d.has_scope && !itemtags::scope_index_ok(d.scope, c.scopes))
		d.has_scope = false; // R3 В1: also every scope tag for a weapon without scopes (c.scopes == 0)
}

// Broadcast the host's view of an item (after applying a player's state, or authority push).
void host_send_view(u16 id, CSE_Abstract* e, CInventoryItem* item, const Blob* fallback, u8 flags, PeerId except)
{
	Blob blob;
	ItemData d;
	if (item)
	{
		capture_object(item, d);
		write_data(d, blob);
	}
	else if (fallback)
		blob = *fallback;
	else if (capture_entity(e, d))
		write_data(d, blob);
	else
		return;
	Writer w(blob.size() + 48);
	write_state(w, id, e->s_name.c_str(), flags, blob);
	net::host_broadcast(Msg::ItemState, w, Lane::Reliable, except);
}

// The owner emptied this box (reload). The standard destroy chain removes it everywhere.
void host_release_box(u16 id, CSE_Abstract* e)
{
	if (g_releasing.count(id))
		return;
	g_owned.erase(id);
	CGameObject* go = g_pGameLevel ? smart_cast<CGameObject*>(Level().Objects.net_Find(id)) : nullptr;
	if (go)
	{
		if (go->getDestroy())
			return;
		g_releasing.insert(id);
		go->DestroyObject();
		++g_stats.boxes_released;
		return;
	}
	CSE_ALifeDynamicObject* dyn = smart_cast<CSE_ALifeDynamicObject*>(e);
	if (!dyn || dyn->m_bOnline)
		return; // online without a game object: being spawned or destroyed right now
	g_releasing.insert(id);
	alife_rw().release(e, true);
	++g_stats.boxes_released;
}

void filter_detached_silencer(u16 id, PeerId from, ItemData& data)
{
	const auto detached = g_detached_silencers.find(id);
	if (detached == g_detached_silencers.end() || detached->second != from || !data.has_addons)
		return;
	if (!(data.addons & CSE_ALifeItemWeapon::eWeaponAddonSilencer))
		g_detached_silencers.erase(detached);
	else
		data.addons &= ~CSE_ALifeItemWeapon::eWeaponAddonSilencer;
}

void host_on_state(PeerId from, Reader& r)
{
	drain_forgotten();
	const u16 id = r.u16();
	const std::string section = r.str();
	r.u8(); // flags: only the host sets them
	const u64 n = r.varu();
	if (!r.ok() || n > kMaxBlob || n > r.remaining())
	{
		reject(Reason::Malformed, from, id, nullptr);
		return;
	}
	const Blob blob(r.cursor(), r.cursor() + size_t(n));
	++g_stats.received;
	ItemData d;
	if (!read_data(blob.data(), blob.size(), d))
	{
		reject(Reason::Malformed, from, id, "blob");
		return;
	}
	const u16 proxy = join::proxy_of_peer(from);
	if (proxy == kNoId || sync::host_peer_joining(from))
	{
		reject(Reason::NoProxy, from, id, nullptr);
		return;
	}
	CSE_Abstract* e = find_entity(id);
	if (!e)
	{
		++g_stats.rejected[u32(Reason::Missing)]; // destroyed meanwhile (eaten, emptied): normal
		return;
	}
	if (!same_str(e->s_name, section.c_str()))
	{
		reject(Reason::Section, from, id, section.c_str());
		return;
	}
	const Access access = host_access(from, proxy, e);
	if (access == Access::Denied)
	{
		reject(Reason::NotOwner, from, id, e->s_name.c_str());
		return;
	}
	const bool ammo_only = access == Access::Foreign;
	const char* why = nullptr;
	if (!host_validate_kind(e, d, why))
	{
		reject(Reason::Insane, from, id, why);
		return;
	}
	CInventoryItem* item = find_item(id);
	// F2 T5 fix round 1 (review 1): the owner's upgrade list never reaches the host copy. An upgrade is bought from a
	// mechanic (ReqItemEvent: checked and charged, then host_upgrade_sold installs it here); one taken from this state
	// would be unpaid and still count in the copy's price (CInventoryItem::Cost), which a trade on the host pays. The
	// copy and the relay below keep the host's list.
	ledger::host_keeps_own_upgrades(d);
	if (!host_validate_counts(e, item, d, why))
	{
		reject(Reason::Insane, from, id, why);
		return;
	}
	host_sanitize(e, item, d);
	filter_detached_silencer(id, from, d);
	Blob clean;
	write_data(d, clean);
	if (item)
		apply_object(item, d, ammo_only);
	apply_entity(e, d, ammo_only);
	++g_stats.applied;
	g_unload_used.erase(id);

	if (access == Access::Owned)
	{
		Owned& o = g_owned[id];
		o.peer = from;
		o.data = d;
		o.stuck = false;
	}
	if (d.has_box && d.box == 0 && !ammo_only && smart_cast<CSE_ALifeItemAmmo*>(e))
	{
		host_release_box(id, e); // its destroy reaches everybody, no state relay needed
		return;
	}
	host_send_view(id, e, item, ammo_only ? nullptr : &clean, 0, from);
	++g_stats.relayed;
}

bool is_ammo_section(const char* sect)
{
	if (!sect || !*sect || !pSettings->section_exist(sect) || !pSettings->line_exist(sect, "box_size") ||
	    !pSettings->line_exist(sect, "class"))
		return false;
	CSE_Abstract* probe = F_entity_Create(sect);
	const bool ok = smart_cast<CSE_ALifeItemAmmo*>(probe) != nullptr;
	if (probe)
		F_entity_Destroy(probe);
	return ok;
}

bool weapon_takes(CSE_ALifeItemWeapon* we, CInventoryItem* item, const char* sect)
{
	if (CWeapon* w = item ? smart_cast<CWeapon*>(item) : nullptr)
	{
		for (const shared_str& s : w->m_ammoTypes)
			if (same_str(s, sect))
				return true;
		if (CWeaponMagazinedWGrenade* g = smart_cast<CWeaponMagazinedWGrenade*>(w))
			for (const shared_str& s : g->m_ammoTypes2)
				if (same_str(s, sect))
					return true;
		return false;
	}
	return in_list(we->m_caAmmoSections, sect) ||
	    in_list(READ_IF_EXISTS(pSettings, r_string, we->s_name, "grenade_class", ""), sect);
}

// Rounds the host knows to be in a weapon (both magazines): an owned weapon's last owner state,
// else the host copy (game object, or the entity when offline).
u32 host_rounds(u16 id, CSE_Abstract* e, CInventoryItem* item)
{
	ItemData d;
	auto it = g_owned.find(id);
	if (it != g_owned.end())
		d = it->second.data;
	else if (item)
		capture_object(item, d);
	else
	{
		CSE_ALifeItemWeapon* w = smart_cast<CSE_ALifeItemWeapon*>(e);
		if (!w)
			return 0;
		u32 n = w->a_elapsed;
		if (smart_cast<CSE_ALifeItemWeaponMagazinedWGL*>(e))
			n += w->a_elapsed_grenades.grenades_count;
		return n;
	}
	return (d.has_main ? u32(d.main_count) : 0) + (d.has_launcher ? u32(d.gl_count) : 0);
}

// Same steps as alife():create_ammo (alife_simulator_script.cpp): the count is in the entity
// before anybody sees it.
bool spawn_box(const char* section, CSE_ALifeDynamicObject* parent, u16 rounds)
{
	CALifeSimulator& sim = alife_rw();
	if (!parent->m_bOnline)
	{
		CSE_Abstract* item =
		    sim.spawn_item(section, parent->o_Position, parent->m_tNodeID, parent->m_tGraphID, parent->ID);
		CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(item);
		if (ammo)
			ammo->a_elapsed = _min(rounds, ammo->m_boxSize);
		return ammo != nullptr;
	}
	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(section);
	CSE_Abstract* item =
	    sim.spawn_item(section, parent->o_Position, parent->m_tNodeID, parent->m_tGraphID, parent->ID, false);
	CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(item);
	if (ammo)
	{
		ammo->a_elapsed = _min(rounds, ammo->m_boxSize);
		item->Spawn_Write(packet, FALSE);
	}
	sim.server().FreeID(item->ID, 0);
	F_entity_Destroy(item);
	if (!ammo)
		return false;
	ClientID client;
	client.set(0xffff);
	u16 dummy;
	packet.r_begin(dummy);
	return sim.server().Process_spawn(packet, client) != nullptr;
}

void host_on_spawn_ammo(PeerId from, Reader& r)
{
	const u32 stream = r.u32();
	const u32 seq = r.u32();
	const u16 weapon_id = r.u16();
	const u16 parent_id = r.u16();
	const std::string sect = r.str();
	const u16 rounds = r.u16();
	if (!r.ok())
	{
		reject(Reason::Malformed, from, weapon_id, "ammo spawn");
		return;
	}
	Stream& st = g_streams[from];
	if (st.stream == stream && seq <= st.seq)
	{
		++g_stats.spawns_repeat; // a repeat changes nothing
		return;
	}
	st.stream = stream;
	st.seq = seq;

	const u16 proxy = join::proxy_of_peer(from);
	if (proxy == kNoId || sync::host_peer_joining(from))
	{
		reject(Reason::NoProxy, from, weapon_id, "ammo spawn");
		return;
	}
	CSE_Abstract* we_abstract = find_entity(weapon_id);
	CSE_ALifeItemWeapon* we = smart_cast<CSE_ALifeItemWeapon*>(we_abstract);
	if (!we)
	{
		reject(Reason::Missing, from, weapon_id, "ammo spawn: weapon");
		return;
	}
	if (host_access(from, proxy, we_abstract) == Access::Denied)
	{
		reject(Reason::NotOwner, from, weapon_id, "ammo spawn");
		return;
	}
	CSE_ALifeDynamicObject* parent = ai().alife().objects().object(parent_id, true);
	if (parent_id == kNoId || we_abstract->ID_Parent != parent_id || !parent)
	{
		reject(Reason::Moved, from, weapon_id, "ammo spawn: the box must go where the weapon is");
		return;
	}
	CInventoryItem* item = find_item(weapon_id);
	// R3 В6 (review): the weapon's own ammo_class / grenade_class first — only one of those is ever constructed
	// as a probe (F_entity_Create is FATAL on lines a peer's section may lack).
	if (!rounds || !weapon_takes(we, item, sect.c_str()) || !is_ammo_section(sect.c_str()))
	{
		reject(Reason::Section, from, weapon_id, sect.c_str());
		return;
	}
	Caps c;
	weapon_caps(we, item, c);
	u32& used = g_unload_used[weapon_id];
	if (used + rounds > c.main_cap + c.gl_cap)
	{
		reject(Reason::Insane, from, weapon_id, "more rounds unloaded than the weapon holds");
		return;
	}
	// Never more than the host knows to be in the weapon: two players unloading one corpse's gun
	// (or a stale copy of it) must not both get its rounds. The weapon's emptied state follows.
	const u32 have = host_rounds(weapon_id, we_abstract, item);
	const u32 avail = have > used ? have - used : 0;
	if (!avail)
	{
		reject(Reason::Moved, from, weapon_id, "ammo spawn: the weapon is already empty here");
		return;
	}
	const u16 rounds_ok = u16(_min(u32(rounds), avail));
	used += rounds_ok;
	const u16 box_size = u16(_max(1, pSettings->r_s32(sect.c_str(), "box_size")));
	u16 left = rounds_ok;
	while (left)
	{
		const u16 now = _min(left, box_size);
		if (!spawn_box(sect.c_str(), parent, now))
		{
			::Msg("! [coop] ammo spawn [%s] x%u for peer %u failed in the engine", sect.c_str(), now, from);
			break;
		}
		left = u16(left - now);
		g_stats.spawn_rounds += now;
	}
	++g_stats.spawns_ok;
}

// Push the host's state of an item that just came into a player's hands (stale there?).
void host_push(u16 id, PeerId peer)
{
	CSE_Abstract* e = find_entity(id);
	if (!e || !smart_cast<CSE_ALifeInventoryItem*>(e))
		return;
	CInventoryItem* item = find_item(id);
	ItemData d;
	if (item)
		capture_object(item, d);
	else if (!capture_entity(e, d))
		return;
	Owned& o = g_owned[id];
	o.peer = peer;
	o.data = d;
	ledger::host_keeps_own_upgrades(o.data); // F2 T5 fix round 1: the list is the host's, never enforced from here
	o.stuck = false;
	g_left.erase(id);
	host_send_view(id, e, item, nullptr, kFlagAuthority, kInvalidPeer);
	++g_stats.pushed;
}

void host_track_proxies(u64 now)
{
	xr_set<u16> seen;
	for (PeerId peer = 1; peer < kMaxPeerSlots; ++peer)
	{
		const u16 proxy = join::proxy_of_peer(peer);
		if (proxy == kNoId)
		{
			g_streams.erase(peer);
			continue;
		}
		if (sync::host_peer_joining(peer))
		{
			seen.insert(proxy); // keep the old list: changes during the join are pushed after it
			continue;
		}
		CSE_ALifeDynamicObject* pe = ai().alife().objects().object(proxy, true);
		if (!pe)
			continue;
		seen.insert(proxy);
		xr_vector<u16> cur = pe->children;
		std::sort(cur.begin(), cur.end());
		auto it = g_proxy_items.find(proxy);
		if (it == g_proxy_items.end())
		{
			// First sight (session start, new player): the host copy is the baseline.
			for (u16 id : cur)
			{
				CInventoryItem* item = find_item(id);
				if (!item)
					continue;
				Owned& o = g_owned[id];
				o.peer = peer;
				capture_object(item, o.data);
				ledger::host_keeps_own_upgrades(o.data); // F2 T5 fix round 1: the host's list, not enforced
				o.stuck = false;
			}
			g_proxy_items.emplace(proxy, cur);
			continue;
		}
		if (it->second == cur)
			continue;
		for (u16 id : it->second)
			if (!std::binary_search(cur.begin(), cur.end(), id))
			{
				Left& l = g_left[id];
				l.peer = peer;
				l.time = now;
				// A gift to a player handled earlier in this loop is already that player's.
				auto ow = g_owned.find(id);
				if (ow != g_owned.end() && ow->second.peer == peer)
					g_owned.erase(ow);
			}
		for (u16 id : cur)
			if (!std::binary_search(it->second.begin(), it->second.end(), id))
				host_push(id, peer);
		it->second.swap(cur);
	}
	for (auto it = g_proxy_items.begin(); it != g_proxy_items.end();)
		it = seen.count(it->first) ? std::next(it) : g_proxy_items.erase(it);
}

// The host copy of a player's item must stay what the player said: anything on the host that
// touched it (an avatar firing for show, scripts, a battery draining) is undone here.
void host_enforce(u64 now)
{
	for (auto it = g_owned.begin(); it != g_owned.end();)
	{
		// A (re)joining player gets the host's world as it is now (after a load: the save's state);
		// his state from before must not be forced onto it. His resync after the join sets it again.
		if (!find_entity(it->first) || sync::host_peer_joining(it->second.peer))
		{
			it = g_owned.erase(it);
			continue;
		}
		CInventoryItem* item = it->second.stuck ? nullptr : find_item(it->first);
		if (item)
		{
			ItemData cur;
			capture_object(item, cur);
			if (diverged_tags(cur, it->second.data))
			{
				apply_object(item, it->second.data, false);
				if (CSE_Abstract* e = find_entity(it->first))
					apply_entity(e, it->second.data, false);
				++g_stats.enforced;
				capture_object(item, cur);
				if (const u32 left = diverged_tags(cur, it->second.data))
				{
					it->second.stuck = true;
					log_diverged(it->first, item->object().cNameSect().c_str(), left, cur, it->second.data); // R3 В1
				}
			}
		}
		++it;
	}
	for (auto it = g_logged_tags.begin(); it != g_logged_tags.end();) // R3 В1: forget items that are gone
		it = g_owned.count(u16(*it >> 8)) ? std::next(it) : g_logged_tags.erase(it);
	for (auto it = g_left.begin(); it != g_left.end();)
		it = now - it->second.time > kGraceMs ? g_left.erase(it) : std::next(it);
	for (auto it = g_releasing.begin(); it != g_releasing.end();)
		it = find_entity(*it) ? std::next(it) : g_releasing.erase(it);
}
} // namespace

// ---------------------------------------------------------------- API

namespace
{
// A state of a peer whose character is known and whose join is not over: kept for host_on_peer_in_world (g_join_held). `at` is not
// moved: a message that is not kept goes to host_on_state as it came, and that one says what is wrong with it.
bool hold_state_of_joining(PeerId from, const Reader& at)
{
	if (join::proxy_of_peer(from) == kNoId || !sync::host_peer_joining(from))
		return false;
	Reader r = at;
	HeldState h;
	h.id = r.u16();
	h.section = r.str();
	r.u8(); // flags: only the host sets them
	const u64 n = r.varu();
	if (!r.ok() || n > kMaxBlob || n > r.remaining())
		return false;
	xr_vector<HeldState>& held = g_join_held[from];
	if (held.size() >= kJoinHeldMax)
		return false;
	h.blob.assign(r.cursor(), r.cursor() + size_t(n));
	held.push_back(std::move(h));
	++g_stats.join_held;
	return true;
}

// ItemState, in both tables (F3a T4: the lambda of the old install, unchanged)
void on_item_state(PeerId from, Reader& r)
{
	const Op op = Op(r.u8());
	if (!r.ok())
		return;
	if (is_map_authority())
	{
		// Precaching a freshly loaded world: states and unloads made in the old one are dropped
		// (the pump runs before join::frame() marks the peers as joining).
		if (!ai().get_alife() || Device.dwPrecacheFrame)
			return;
		if (op == Op::State)
		{
			if (!hold_state_of_joining(from, r))
				host_on_state(from, r);
		}
		else if (op == Op::SpawnAmmo && levels::on_host_level(from))
			host_on_spawn_ammo(from, r); // this message reaches the world server and the map server of his level: the boxes are made at his body
	}
	else if (mgr().is_client() && op == Op::State)
	{
		// R2 I2 (rules::live_only): a state from before we are live is of the window between Welcome and
		// JoinRequest or of a world we left; the save we load is newer. Kept pending, it would be applied over it.
		if (rules::live_only(Msg::ItemState) && !mgr().is_live_client())
			++g_stats.dropped_before_live;
		else
			client_on_state(r);
	}
}
} // namespace

void install_server(Session& s) { s.on(Msg::ItemState, &on_item_state); }

void host_on_peer_in_world(PeerId peer)
{
	const auto it = g_join_held.find(peer);
	if (it == g_join_held.end())
		return;
	xr_vector<HeldState> held;
	held.swap(it->second);
	g_join_held.erase(it);
	if (!is_map_authority() || !ai().get_alife() || Device.dwPrecacheFrame)
		return; // another world meanwhile: what he said belongs to the one before (on_item_state drops the same)
	for (const HeldState& h : held)
	{
		Writer w(h.blob.size() + h.section.size() + 16);
		w.u16(h.id);
		w.str(h.section.c_str());
		w.u8(0);
		w.varu(h.blob.size());
		if (!h.blob.empty())
			w.bytes(h.blob.data(), h.blob.size());
		Reader r(w.ptr(), w.size());
		host_on_state(peer, r);
		++g_stats.join_taken;
	}
}

void install_client(Endpoint& e) { e.on(Msg::ItemState, &on_item_state); }

void reset()
{
	g_join_held.clear(); // a session that ended: nobody's join is waited for
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_spawn_states.clear();
		g_touched.clear();
		g_spawns.clear();
		g_scan_now = false;
		g_made_here.clear();
		g_forgotten.clear();
		g_clear_pending.clear();
	}
	g_sent.clear();
	g_pending.clear();
	g_take_base.clear();
	g_owned.clear();
	g_detached_silencers.clear();
	g_logged_tags.clear();
	g_left.clear();
	g_proxy_items.clear();
	g_unload_used.clear();
	g_streams.clear();
	g_releasing.clear();
	g_was_live = false;
	g_resync = false;
	g_host_precaching = false;
	g_last_scan = g_last_modify_scan = g_last_enforce = 0;
	g_last_modify = 0;
	g_seq = 0;
}

void frame()
{
	drain_forgotten();
	xr_vector<u16> touched;
	xr_vector<SpawnReq> spawns;
	bool scan_now = false;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		touched.swap(g_touched);
		spawns.swap(g_spawns);
		scan_now = g_scan_now;
		g_scan_now = false;
	}
	const bool host = is_map_authority();
	const bool live_client = mgr().is_live_client();
	if ((!host && !live_client) || !g_pGameLevel || !ai().get_alife())
	{
		g_was_live = false;
		return;
	}
	// 0. Host: a freshly loaded world (precache after a load, also a same-level quick load that
	//    keeps the level). What players said about their items belongs to the old world: the save
	//    is the truth now (R-6.3); tracking starts again from the host copies after the precache.
	if (host && Device.dwPrecacheFrame)
	{
		if (!g_host_precaching)
		{
			g_owned.clear();
			g_detached_silencers.clear();
			g_left.clear();
			g_proxy_items.clear();
			g_unload_used.clear();
			g_releasing.clear();
		}
		g_host_precaching = true;
		return;
	}
	g_host_precaching = false;
	const u64 now = mgr().now_ms();
	if (live_client && !g_was_live)
	{
		// Entered the host's world: start a new spawn stream and tell the host our state once.
		g_sent.clear();
		g_resync = true;
		g_stream = u32(now) ^ (u32(mgr().self()) << 24) ^ 0x5eed0001u;
		g_seq = 0;
	}
	g_was_live = live_client;

	// 1. Ammo spawns of an unload go first: the host checks them against the weapon state it
	//    has, the weapon's emptied state follows in step 3.
	if (live_client)
		send_spawns(spawns);
	// 2. States the host sent while we could not apply them (P6), before our own scan so that
	//    an authority state is not overwritten by our stale copy.
	if (live_client)
		process_pending(now);
	// 3. Own items.
	CActor* actor = Actor();
	const u32 modify = actor ? actor->inventory().ModifyFrame() : 0;
	const bool modified = modify != g_last_modify && now - g_last_modify_scan >= kModifyScanMs;
	if (scan_now || !touched.empty() || modified || g_resync || now - g_last_scan >= kScanMs)
	{
		scan_local(g_resync);
		g_resync = false;
		g_last_scan = g_last_modify_scan = now;
		g_last_modify = modify;
	}
	// 4. World items this player changed (unloading a corpse's gun).
	send_foreign(touched);
	// 5. Host: who holds what, and keep players' items as their owners said.
	if (host)
	{
		host_track_proxies(now);
		if (now - g_last_enforce >= kEnforceMs)
		{
			g_last_enforce = now;
			host_enforce(now);
		}
	}
}

void print_status()
{
	const Stats& s = g_stats;
	::Msg("[coop]   items out: %llu states, %llu foreign, %llu ammo spawns; in: %llu states, %llu applied, "
	      "%llu authority (%llu kept ours), %llu ignored, %u pending (%llu dropped)",
	    s.sent, s.foreign_sent, s.spawns_sent, s.received, s.applied, s.authority_applied, s.authority_skipped,
	    s.ignored, (u32)g_pending.size(), s.pending_dropped);
	if (is_map_authority())
	{
		u32 stuck = 0;
		for (const auto& it : g_owned)
			stuck += it.second.stuck ? 1 : 0;
		::Msg("[coop]   items host: %u tracked (%u stuck), %llu relayed, %llu pushed, %llu enforced, "
		      "%llu boxes released, ammo spawns %llu ok (%llu rounds, %llu repeats), %llu sold upgrades installed "
		      "(owners' upgrade lists are never taken, F2 T5)",
		    (u32)g_owned.size(), stuck, s.relayed, s.pushed, s.enforced, s.boxes_released, s.spawns_ok,
		    s.spawn_rounds, s.spawns_repeat, s.upgrades_sold);
		::Msg("[coop]   items host: %llu states of world items sent to a client that differed or opened them (F1)",
		    s.world_states);
		::Msg("[coop]   items refused: malformed %llu, no character %llu, missing %llu, section %llu, "
		      "not owner %llu, moved %llu, insane %llu",
		    s.rejected[0], s.rejected[1], s.rejected[2], s.rejected[3], s.rejected[4], s.rejected[5],
		    s.rejected[6]);
	}
	::Msg("[coop]   items: %llu upgrades dropped before a loading install (unknown, out of order, group taken)",
	    s.upgrades_dropped);
	if (is_map_authority())
		::Msg("[coop]   items of joining players: %llu state(s) kept, %llu taken when the join ended (%u peer(s) waiting)",
		    s.join_held, s.join_taken, (u32)g_join_held.size());
	if (mgr().is_client())
		::Msg("[coop]   items in: %llu states dropped before live (R2 I2), %llu take changes kept over the host's push "
		      "(%u waiting)",
		    s.dropped_before_live, s.take_kept, (u32)g_take_base.size());
}

void print_inventory()
{
	print_status();
	CActor* actor = g_pGameLevel ? Actor() : nullptr;
	if (!actor)
		return;
	::Msg("[coop-inventory] actor=%u unlimited_ammo=%u", actor->ID(), actor->unlimited_ammo() ? 1u : 0u);
	PIItem active = actor->inventory().ActiveItem();
	::Msg("[coop-inventory] active=%u remote=%u alive=%u", active ? active->object_id() : kNoId,
	    actor->Remote() ? 1u : 0u, actor->g_Alive() ? 1u : 0u);
	for (PIItem item : actor->inventory().m_all)
	{
		if (!item || item->object().getDestroy()) continue;
		ItemData d;
		capture_object(item, d);
		if (!d.has_main && !d.has_box) continue;
		const u16 id = item->object_id();
		CSE_Abstract* e = find_entity(id);
		::Msg("[coop-inventory] id=%u host=%u section=%s parent=%u root=%u cond=%u mag=%u box=%u pending=%u",
		    id, join::to_host(id), item->object().cNameSect().c_str(), e ? e->ID_Parent : kNoId,
		    e ? entity_root(e) : kNoId, d.cond, d.main_count, d.box, requests::client_lease_waiting(id) ? 1u : 0u);
		if (CWeapon* gun = smart_cast<CWeapon*>(item))
			::Msg("[coop-inventory] weapon=%u state=%u next=%u pending=%u misfire=%u working=%u unlimited=%u cartridges=%u",
			    id, gun->GetState(), gun->GetNextState(), gun->IsPending() ? 1u : 0u,
			    gun->IsMisfire() ? 1u : 0u, gun->IsWorking() ? 1u : 0u, gun->unlimited_ammo() ? 1u : 0u,
			    u32(gun->m_magazine.size()));
		std::string metadata;
		if (d.has_main && requests::clone_export(id, metadata))
		{
			// Hex chunks preserve the bounded serializer's exact bytes without control characters in the log.
			static const char hex[] = "0123456789abcdef";
			for (size_t offset = 0; offset < metadata.size(); offset += 128)
			{
				std::string encoded;
				for (size_t i = offset; i < metadata.size() && i < offset + 128; ++i)
				{
					const u8 byte = u8(metadata[i]);
					encoded += hex[byte >> 4]; encoded += hex[byte & 15];
				}
				::Msg("[coop-inventory] metadata %u %u %s", id, u32(offset), encoded.c_str());
			}
		}
	}
}

void client_before_take(u16 local_item)
{
	if (!is_live_mirror() || local_item == kNoId)
		return;
	CInventoryItem* item = find_item(local_item);
	if (!item)
		return;
	TakeBase& b = g_take_base[local_item];
	capture_object(item, b.data);
	b.time = mgr().now_ms();
	b.made_here = false;
}

void client_made_here(u16 local_id)
{
	if (!is_live_mirror() || local_id == kNoId)
		return;
	std::lock_guard<std::mutex> lock(g_lock);
	g_made_here.push_back(local_id);
}

void client_resend_state(u16 local_id)
{
	if (!is_live_mirror() || local_id == kNoId)
		return;
	g_sent[local_id] = Blob(); // differs from any captured state: the next scan sends ours
	std::lock_guard<std::mutex> lock(g_lock);
	g_scan_now = true;
}

void client_send_state_now(u16 local_id)
{
	CInventoryItem* item = is_live_mirror() ? find_item(local_id) : nullptr;
	if (!item || requests::client_lease_waiting(local_id))
		return;
	Blob blob;
	capture_blob(item, blob);
	g_sent[local_id] = blob;
	send_state(item, blob);
	++g_stats.sent;
}

void host_silencer_detached(u16 id, PeerId owner)
{
	CSE_Abstract* entity = find_entity(id);
	CInventoryItem* item = find_item(id);
	if (!entity || !item)
		return;
	ItemData state;
	capture_object(item, state);
	apply_entity(entity, state, false);
	auto owned = g_owned.find(id);
	if (owned != g_owned.end())
		owned->second.data = state;
	g_detached_silencers[id] = owner;
	host_send_view(id, entity, item, nullptr, 0, owner);
}

void touch(CInventoryItem* item)
{
	if (!item || !mgr().active())
		return;
	const u16 id = item->object_id();
	std::lock_guard<std::mutex> lock(g_lock);
	g_touched.push_back(id);
	g_scan_now = true;
}

bool client_spawn_ammo(CWeapon* weapon, u32 box_curr, const char* ammo_sect, u32 parent_id)
{
	if (!weapon || !is_live_mirror() || weapon->m_ammoTypes.empty())
		return false;
	const char* sect = ammo_sect ? ammo_sect : weapon->m_ammoTypes[0].c_str();
	u16 parent = kNoId;
	if (parent_id != 0xffffffff)
		parent = u16(parent_id);
	else if (weapon->H_Parent())
		parent = weapon->H_Parent()->ID();
	if (!sect || !*sect || parent == kNoId)
		return true; // nowhere to put it; never create it locally
	u32 rounds = box_curr;
	if (rounds == 0xffffffff)
		rounds = u32(_max(0, READ_IF_EXISTS(pSettings, r_s32, sect, "box_size", 0)));
	if (!rounds)
		return true;
	SpawnReq s;
	s.weapon = weapon->ID();
	s.parent = parent;
	s.section = sect;
	s.rounds = u16(_min(rounds, u32(0xffff)));
	std::lock_guard<std::mutex> lock(g_lock);
	g_spawns.push_back(s);
	g_touched.push_back(weapon->ID());
	g_scan_now = true;
	return true;
}

bool client_keep_empty_box(CWeaponAmmo* box)
{
	if (!box || !is_live_mirror())
		return false;
	std::lock_guard<std::mutex> lock(g_lock);
	g_touched.push_back(box->ID());
	g_scan_now = true;
	return true;
}

void on_weapon_fired(CWeapon* weapon)
{
	if (g_fired && weapon)
		g_fired(weapon);
}

void set_weapon_fired_handler(WeaponFiredFn fn) { g_fired = fn; }

// R3 В6: what a ReqSpawn carries instead of raw M_SPAWN bytes (the host never Spawn_Read's a peer's packet).
bool capture_spawn_state(CSE_Abstract* entity, std::vector<u8>& blob)
{
	blob.clear();
	if (entity)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (std::any_of(g_spawns.begin(), g_spawns.end(), [entity](const SpawnReq& spawn) { return spawn.weapon == entity->ID; }))
			return false; // an earlier unload must reach the host under this source instance first
		const auto pending = g_spawn_states.find(entity->ID);
		if (pending != g_spawn_states.end())
		{
			blob = pending->second;
			return true;
		}
	}
	ItemData d;
	if (!entity || !capture_entity(entity, d))
		return false;
	// In grenade mode an offline CSE has no rifle count/type. Refuse an incomplete replacement snapshot.
	if (smart_cast<CSE_ALifeItemWeaponMagazinedWGL*>(entity) && !d.has_main)
		return false;
	CWeapon* weapon = smart_cast<CWeapon*>(find_item(entity->ID));
	if (weapon)
	{
		capture_object(weapon, d);
		if (CSE_ALifeInventoryItem* server = smart_cast<CSE_ALifeInventoryItem*>(entity))
			d.upgrades = server->m_upgrades;
		if (CWeaponMagazined* magazined = smart_cast<CWeaponMagazined*>(weapon))
		{
			d.has_fire_mode = true;
			d.fire_mode = u8(magazined->GetFireModeIndex());
		}
		CWeaponMagazinedWGrenade* launcher = smart_cast<CWeaponMagazinedWGrenade*>(weapon);
		const auto& magazine = launcher && launcher->m_bGrenadeMode ? launcher->m_magazine2 : weapon->m_magazine;
		d.has_cartridges = true;
		for (const CCartridge& cartridge : magazine)
			d.cartridges.push_back(cartridge.m_LocalAmmoType);
	}
	else
	{
		if (CSE_ALifeItemWeaponMagazined* magazined = smart_cast<CSE_ALifeItemWeaponMagazined*>(entity))
		{
			d.has_fire_mode = true;
			d.fire_mode = magazined->m_u8CurFireMode;
		}
		if (CSE_ALifeItemWeaponShotGun* shotgun = smart_cast<CSE_ALifeItemWeaponShotGun*>(entity))
			if (shotgun->m_AmmoIDs.size() == d.main_count)
			{
				d.has_cartridges = true;
				d.cartridges.assign(shotgun->m_AmmoIDs.begin(), shotgun->m_AmmoIDs.end());
			}
	}
	write_data(d, blob);
	return true;
}

bool replacement_state(CSE_Abstract* source, const u8* data, size_t size, std::vector<u8>& out)
{
	ItemData d;
	CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(source);
	if (!item || !read_data(data, size, d))
		return false;
	d.has_upgrades = true;
	d.upgrades = item->m_upgrades;
	write_data(d, out);
	return true;
}

void stage_spawn_state(u16 id, const std::vector<u8>& blob)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_spawn_states[id] = blob;
}

void discard_spawn_state(u16 id)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_spawn_states.erase(id);
}

void forget_item(u16 id)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_spawn_states.erase(id);
	g_touched.erase(std::remove(g_touched.begin(), g_touched.end(), id), g_touched.end());
	g_made_here.erase(std::remove(g_made_here.begin(), g_made_here.end(), id), g_made_here.end());
	g_spawns.erase(std::remove_if(g_spawns.begin(), g_spawns.end(),
	    [id](const SpawnReq& spawn) { return spawn.weapon == id || spawn.parent == id; }), g_spawns.end());
	g_forgotten.push_back(id); // owner/host tracking belongs to the main thread
}

void prepare_clone_state(u16 source, u16 target)
{
	forget_item(target);
	std::lock_guard<std::mutex> lock(g_lock);
	g_clear_pending.push_back(source);
}

void restore_item_state(CSE_Abstract* entity, const std::vector<u8>& blob, CInventoryItem* object)
{
	ItemData d;
	if (!entity || !read_data(blob.data(), blob.size(), d))
		return;
	CInventoryItem* item = object ? object : find_item(entity->ID);
	if (!item)
		stage_spawn_state(entity->ID, blob); // an offline mirror consumes the complete state when it comes online
	if (item)
	{
		apply_object(item, d, false);
		CWeaponMagazinedWGrenade* weapon = smart_cast<CWeaponMagazinedWGrenade*>(item);
		CWeapon* gun = smart_cast<CWeapon*>(item);
		if (gun && d.has_cartridges)
		{
			auto& magazine = weapon && weapon->m_bGrenadeMode ? weapon->m_magazine2 : gun->m_magazine;
			const auto& types = weapon && weapon->m_bGrenadeMode ? weapon->m_ammoTypes2 : gun->m_ammoTypes;
			if (magazine.size() == d.cartridges.size())
				for (size_t i = 0; i < magazine.size(); ++i)
					if (d.cartridges[i] < types.size())
						magazine[i].Load(types[d.cartridges[i]].c_str(), d.cartridges[i], gun->m_APk);
		}
		if (CWeaponMagazined* magazined = smart_cast<CWeaponMagazined*>(item))
			if (d.has_fire_mode)
				magazined->SetFireMode(d.fire_mode);
		if (weapon && d.has_mode && weapon->m_bGrenadeMode != d.grenade_mode && weapon->IsGrenadeLauncherAttached())
			weapon->PerformSwitchGL();
		if (CSE_ALifeItemWeaponMagazinedWGL* server = smart_cast<CSE_ALifeItemWeaponMagazinedWGL*>(entity))
			if (weapon)
				server->m_bGrenadeMode = weapon->m_bGrenadeMode;
	}
	apply_entity(entity, d, false);
}

void restore_spawn_state(CWeapon* weapon, CSE_Abstract* entity, bool launcher_ready)
{
	if (!weapon || (!launcher_ready && smart_cast<CWeaponMagazinedWGrenade*>(weapon)))
		return;
	Blob blob;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		const auto it = g_spawn_states.find(entity->ID);
		if (it == g_spawn_states.end())
			return;
		blob = it->second;
		g_spawn_states.erase(it);
	}
	restore_item_state(entity, blob, weapon);
}

bool client_pending(CWeapon* weapon)
{
	return weapon && is_live_mirror() && requests::client_lease_waiting(weapon->ID());
}

// R3 В6: the checks of host_on_state for an entity the host is about to create from its own section.
bool host_apply_spawn_state(CSE_Abstract* entity, const u8* data, size_t size, const char*& why)
{
	why = nullptr;
	ItemData d;
	if (!entity || size > kMaxBlob || !read_data(data, size, d))
	{
		why = "malformed state";
		return false;
	}
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(entity);
	if (!ii)
	{
		why = "item state for a non-item";
		return false;
	}
	if (!host_validate_kind(entity, d, why) || !host_validate_counts(entity, nullptr, d, why))
		return false;
	// The new object installs these on its spawn with loading = true after the section defaults (FATAL on a missing
	// parent group, a taken group): only an order the engine takes, each once. Dropped ones are counted and logged.
	if (d.has_upgrades)
		d.upgrades = spawn_upgrade_list(entity->s_name, d.upgrades, true);
	host_sanitize(entity, nullptr, d); // ammo types in range
	apply_entity(entity, d, false);
	// apply_entity leaves upgrades to GE_INSTALL_UPGRADE on the host; a fresh entity has none yet.
	if (d.has_upgrades)
		ii->m_upgrades = d.upgrades;
	return true;
}

bool section_defaults_ok(const shared_str& section)
{
	if (!ai().get_alife())
		return true;
	inventory::upgrade::Manager& um = ai().alife().inventory_upgrade_manager();
	inventory::upgrade::Root* root = um.get_root(section);
	std::vector<upgrades::Upgrade> defaults;
	for (const shared_str& id : section_default_upgrades(um, section))
		defaults.push_back(describe_upgrade(um, root, id));
	return upgrades::defaults_ok(defaults);
}

AddedUpgrade host_check_added_upgrade(CSE_Abstract* entity, const shared_str& upgrade, const char*& why)
{
	why = nullptr;
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(entity);
	if (!ii || !ai().get_alife())
	{
		why = "not an inventory item";
		return AddedUpgrade::Refused;
	}
	// F0 final review (task 10b): the FATAL's own condition first (CSE add_upgrade on an id the entity has), before
	// any replay of the rules.
	if (ii->has_upgrade(upgrade))
		return AddedUpgrade::AlreadyThere;
	inventory::upgrade::Manager& um = ai().alife().inventory_upgrade_manager();
	inventory::upgrade::Root* root = um.get_root(entity->s_name);
	std::vector<upgrades::Upgrade> list, defaults;
	for (const shared_str& id : ii->m_upgrades)
		list.push_back(describe_upgrade(um, root, id));
	for (const shared_str& id : section_default_upgrades(um, entity->s_name))
		defaults.push_back(describe_upgrade(um, root, id));
	const upgrades::Verdict v = upgrades::verdict_of_added(list, describe_upgrade(um, root, upgrade), defaults);
	switch (v)
	{
	case upgrades::Verdict::Install: return AddedUpgrade::Relay;
	case upgrades::Verdict::Installed:
	case upgrades::Verdict::Default: return AddedUpgrade::AlreadyThere;
	default:
		why = verdict_name(v);
		return AddedUpgrade::Refused;
	}
}

u32 state_digest(CSE_Abstract* e, bool with_uses)
{
	ItemData d;
	if (CInventoryItem* item = find_item(e->ID))
		capture_object(item, d, false);
	else if (!capture_entity(e, d, false))
		return 0;
	return itemtags::state_digest(d, with_uses);
}

void host_send_state(PeerId to, u16 id)
{
	CSE_Abstract* e = find_entity(id);
	if (!e || !smart_cast<CSE_ALifeInventoryItem*>(e))
		return;
	CInventoryItem* item = find_item(id);
	ItemData d;
	if (item)
		capture_object(item, d);
	else if (!capture_entity(e, d))
		return;
	Blob blob;
	write_data(d, blob);
	Writer w(blob.size() + 48);
	write_state(w, id, e->s_name.c_str(), 0, blob);
	net::host_send(to, Msg::ItemState, w);
	++g_stats.world_states;
}

void host_send_contents(PeerId to, u16 holder)
{
	CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(holder, true) : nullptr;
	if (!e)
		return;
	for (u16 id : e->children)
		host_send_state(to, id);
}

void host_upgrade_sold(u16 id, PeerId owner)
{
	CSE_Abstract* e = find_entity(id);
	CSE_ALifeInventoryItem* ii = e ? smart_cast<CSE_ALifeInventoryItem*>(e) : nullptr;
	if (!ii)
		return;
	CInventoryItem* item = find_item(id);
	if (item)
		install_upgrades(item, ii->m_upgrades); // a loading install: no event, no Lua price
	host_send_view(id, e, item, nullptr, 0, owner); // the owner has it already
	++g_stats.upgrades_sold;
}

// ---------------------------------------------------------------- F2 T5: an upgrade bought at a mechanic (client)
namespace
{
UpgradePurchase* g_purchase = nullptr; // the innermost alive purchase (main thread)
} // namespace

UpgradePurchase::UpgradePurchase(CInventoryItem* item, u16 mechanic)
	: m_outer(g_purchase), m_item(item ? item->object_id() : kNoId), m_mechanic(mechanic)
{
	g_purchase = this;
}

UpgradePurchase::~UpgradePurchase()
{
	g_purchase = m_outer;
	// Spent here and taken by no upgrade event (the install failed, the event did not leave): an ordinary change, so
	// the host's balance follows what the actor paid. m_scope is still alive: this is not summed again.
	const u32 spent = m_scope.spent();
	if (spent > m_carried)
		money::client_spend(spent - m_carried);
}

bool claim_upgrade_payment(u16 local_item, u16& mechanic, u32& paid, u32& hold)
{
	mechanic = kNoId;
	paid = 0;
	hold = 0;
	UpgradePurchase* p = g_purchase;
	if (!p || p->m_item != local_item)
		return false; // a script's upgrade, or another item's: no mechanic, nothing paid
	const u32 spent = p->m_scope.spent();
	mechanic = p->m_mechanic;
	paid = spent > p->m_carried ? spent - p->m_carried : 0;
	p->m_carried = spent;
	hold = money::client_hold(paid);
	return true;
}

u16 actor_talk_partner()
{
	CActor* a = Actor();
	CInventoryOwner* partner = a && a->IsTalking() ? a->GetTalkPartner() : nullptr;
	CGameObject* go = partner ? smart_cast<CGameObject*>(partner) : nullptr;
	return go ? go->ID() : kNoId;
}
} // namespace items
} // namespace coop
