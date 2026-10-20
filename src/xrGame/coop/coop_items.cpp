#include "stdafx.h"
#include "coop_items.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_sync.h"
#include "coop_alife.h"

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
#include "xrServer.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrServer_Objects.h"
#include "../xrServerEntities/xrMessages.h"

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
constexpr float kForeignRange = 50.f; // a corpse / stash a player unloads must be near that player
constexpr size_t kMaxBlob = 4096;
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
};
Stats g_stats;

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
WeaponFiredFn g_fired = nullptr;

// ---------------------------------------------------------------- owner side (main thread)

xr_map<u16, Blob> g_sent; // own items: the state the host has (local id)
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

// ---------------------------------------------------------------- host side (main thread)

struct Owned
{
	PeerId peer = kInvalidPeer;
	ItemData data;
	bool stuck = false; // the host copy does not take this state: do not retry until the next one
};
xr_map<u16, Owned> g_owned; // items of players' characters with the owner's last state
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

bool upgrade_valid(const shared_str& item_section, const shared_str& upgrade)
{
	if (!ai().get_alife() || !upgrade.c_str())
		return false;
	inventory::upgrade::Manager& um = ai().alife().inventory_upgrade_manager();
	inventory::upgrade::Root* root = um.get_root(item_section);
	return root && um.get_upgrade(upgrade) && root->contain_upgrade(upgrade);
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

// Semantic comparison (upgrade order does not matter).
bool same(const ItemData& a, const ItemData& b)
{
	if (a.has_cond != b.has_cond || (a.has_cond && a.cond != b.cond))
		return false;
	if (a.has_upgrades != b.has_upgrades || (a.has_upgrades && !same_upgrades(a.upgrades, b.upgrades)))
		return false;
	if (a.has_box != b.has_box || (a.has_box && a.box != b.box))
		return false;
	if (a.has_addons != b.has_addons || (a.has_addons && a.addons != b.addons))
		return false;
	if (a.has_scope != b.has_scope || (a.has_scope && a.scope != b.scope))
		return false;
	if (a.has_main != b.has_main || (a.has_main && (a.main_count != b.main_count || a.main_type != b.main_type)))
		return false;
	if (a.has_launcher != b.has_launcher ||
	    (a.has_launcher && (a.gl_count != b.gl_count || a.gl_type != b.gl_type)))
		return false;
	if (a.has_uses != b.has_uses || (a.has_uses && a.uses != b.uses))
		return false;
	return true;
}

// ---------------------------------------------------------------- capture

void capture_object(CInventoryItem* item, ItemData& d)
{
	d = ItemData();
	d.has_cond = true;
	d.cond = quant(item->GetCondition());
	d.has_upgrades = true;
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
		d.has_scope = true;
		d.scope = w->m_cur_scope;
		CWeaponMagazinedWGrenade* g = smart_cast<CWeaponMagazinedWGrenade*>(w);
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

// Fallback when the item has no game object here. The launcher magazine and portions live only
// in the game object's save data, so they are left out.
bool capture_entity(CSE_Abstract* e, ItemData& d)
{
	d = ItemData();
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(e);
	if (!ii)
		return false;
	d.has_cond = true;
	d.cond = quant(ii->m_fCondition);
	d.has_upgrades = true;
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
	for (const shared_str& id : list)
		if (!item->has_upgrade(id) && upgrade_valid(section, id))
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
		if (d.has_upgrades && !mgr().is_host())
		{
			UpgradeList valid;
			for (const shared_str& id : d.upgrades)
				if (upgrade_valid(e->s_name, id))
					valid.push_back(id);
			ii->m_upgrades = valid;
		}
	}
	if (d.has_box)
		if (CSE_ALifeItemAmmo* a = smart_cast<CSE_ALifeItemAmmo*>(e))
			a->a_elapsed = _min(d.box, a->m_boxSize);

	CSE_ALifeItemWeapon* w = smart_cast<CSE_ALifeItemWeapon*>(e);
	if (!w)
		return;
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
	if (mgr().is_host())
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

void scan_local(bool send_all)
{
	CActor* actor = Actor();
	if (!actor || actor->getDestroy())
		return;
	const bool host = mgr().is_host();
	xr_set<u16> present;
	Blob blob;
	for (PIItem item : actor->inventory().m_all)
	{
		if (!item || item->object().getDestroy())
			continue;
		const u16 id = item->object_id();
		present.insert(id);
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
		if (item)
		{
			Blob cur;
			capture_blob(item, cur);
			auto it = g_sent.find(id);
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
	const bool weapon_fields = d.has_main || d.has_launcher || d.has_addons || d.has_scope;
	if (weapon_fields && !smart_cast<CSE_ALifeItemWeapon*>(e))
	{
		why = "weapon state for a non-weapon";
		return false;
	}
	return true;
}

// Refused: counts no item of this kind can have (rounds or portions out of nothing).
// Checked after the upgrades are installed: they may enlarge the magazine.
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
	return true;
}

// Descriptive fields (upgrade list, ammo type index, scope index) are cleaned, not refused: a
// refused state would leave the host with stale round counts.
void host_sanitize(CSE_Abstract* e, CInventoryItem* item, ItemData& d)
{
	if (d.has_upgrades)
	{
		UpgradeList valid;
		for (const shared_str& id : d.upgrades)
			if (upgrade_valid(e->s_name, id))
				valid.push_back(id);
		d.upgrades.swap(valid);
	}
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
	if (d.has_scope && d.scope >= c.scopes)
		d.has_scope = false;
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

void host_on_state(PeerId from, Reader& r)
{
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
	if (item && !ammo_only && d.has_upgrades)
		install_upgrades(item, d.upgrades); // unknown ones are skipped
	if (!host_validate_counts(e, item, d, why))
	{
		reject(Reason::Insane, from, id, why);
		return;
	}
	host_sanitize(e, item, d);
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
	if (!rounds || !is_ammo_section(sect.c_str()) || !weapon_takes(we, item, sect.c_str()))
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
	o.stuck = false;
	g_left.erase(id);
	host_send_view(id, e, item, nullptr, kFlagAuthority, kInvalidPeer);
	++g_stats.pushed;
}

void host_track_proxies(u64 now)
{
	xr_set<u16> seen;
	for (PeerId peer = 1; peer < kMaxPlayers; ++peer)
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
			if (!same(cur, it->second.data))
			{
				apply_object(item, it->second.data, false);
				if (CSE_Abstract* e = find_entity(it->first))
					apply_entity(e, it->second.data, false);
				++g_stats.enforced;
				capture_object(item, cur);
				if (!same(cur, it->second.data))
				{
					it->second.stuck = true;
					::Msg("! [coop] item %u [%s]: the host copy does not take its owner's state", it->first,
					    item->object().cNameSect().c_str());
				}
			}
		}
		++it;
	}
	for (auto it = g_left.begin(); it != g_left.end();)
		it = now - it->second.time > kGraceMs ? g_left.erase(it) : std::next(it);
	for (auto it = g_releasing.begin(); it != g_releasing.end();)
		it = find_entity(*it) ? std::next(it) : g_releasing.erase(it);
}
} // namespace

// ---------------------------------------------------------------- API

void install(Session& s)
{
	s.on(Msg::ItemState, [](PeerId from, Reader& r)
	{
		const Op op = Op(r.u8());
		if (!r.ok())
			return;
		if (mgr().is_host())
		{
			// Precaching a freshly loaded world: states and unloads made in the old one are dropped
			// (the pump runs before join::frame() marks the peers as joining).
			if (!ai().get_alife() || Device.dwPrecacheFrame)
				return;
			if (op == Op::State)
				host_on_state(from, r);
			else if (op == Op::SpawnAmmo)
				host_on_spawn_ammo(from, r);
		}
		else if (mgr().is_client() && op == Op::State)
			client_on_state(r);
	});
}

void reset()
{
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_touched.clear();
		g_spawns.clear();
		g_scan_now = false;
	}
	g_sent.clear();
	g_pending.clear();
	g_owned.clear();
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
	const bool host = mgr().is_host();
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
	if (mgr().is_host())
	{
		u32 stuck = 0;
		for (const auto& it : g_owned)
			stuck += it.second.stuck ? 1 : 0;
		::Msg("[coop]   items host: %u tracked (%u stuck), %llu relayed, %llu pushed, %llu enforced, "
		      "%llu boxes released, ammo spawns %llu ok (%llu rounds, %llu repeats)",
		    (u32)g_owned.size(), stuck, s.relayed, s.pushed, s.enforced, s.boxes_released, s.spawns_ok,
		    s.spawn_rounds, s.spawns_repeat);
		::Msg("[coop]   items refused: malformed %llu, no character %llu, missing %llu, section %llu, "
		      "not owner %llu, moved %llu, insane %llu",
		    s.rejected[0], s.rejected[1], s.rejected[2], s.rejected[3], s.rejected[4], s.rejected[5],
		    s.rejected[6]);
	}
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
	if (!weapon || !mgr().is_live_client() || weapon->m_ammoTypes.empty())
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
	if (!box || !mgr().is_live_client())
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
} // namespace items
} // namespace coop
