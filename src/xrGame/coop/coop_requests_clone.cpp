// Anomaly Coop - requests: a clone of a player's magazined weapon (alife():clone_weapon: a scope fitted, an upgrade kit)
// replaces it in one transaction (xrCoop/coop_replacement.h): the client stages the clone's state and its Lua metadata
// (coop_economy_mdata.script) before the register, the host builds the clone from its own copy of the source and
// releases the source with it. The clone takes its old weapon's slot (xrCoop/coop_clone_slot.h).
#include "stdafx.h"
#include "pch_script.h"
#include "coop_requests_impl.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_items.h"

#include <algorithm>

#include "Level.h"
#include "Actor.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "WeaponMagazined.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "ai_space.h"
#include "script_engine.h"

namespace coop
{
namespace requests
{
namespace
{
u64 g_clone_nonce = 0;

lua_State* clone_function(const char* name, int& top)
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return nullptr;
	top = lua_gettop(L);
	lua_getglobal(L, "coop_economy_mdata");
	if (lua_istable(L, -1))
		lua_getfield(L, -1, name);
	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, top);
		return nullptr;
	}
	return L;
}

bool clone_return(lua_State* L, int top, int args, std::string* text = nullptr)
{
	bool ok = lua_pcall(L, args, 1, 0) == 0;
	if (ok && text)
	{
		size_t size = 0;
		const char* bytes = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &size) : nullptr;
		ok = bytes && size && size <= kMaxItemState;
		if (ok)
			text->assign(bytes, size);
	}
	else if (ok)
		ok = lua_toboolean(L, -1) != 0;
	lua_settop(L, top);
	return ok;
}
} // namespace

// `clone` is a variant of `source` (lease::clone_family over their parent_section lines), or the variant stock
// attach_scope makes of it with an addon of its `scopes`
// (lease::addon_variant: wpn_vityaz + vitup = wpn_vityaz_vitup, which names itself in parent_section).
bool clone_variant(LPCSTR source, LPCSTR clone)
{
	if (!source || !clone || !pSettings->section_exist(source) || !pSettings->section_exist(clone))
		return false;
	const auto line = [](LPCSTR sec, LPCSTR name) -> LPCSTR
	{
		if (!pSettings->line_exist(sec, name))
			return "";
		const LPCSTR v = pSettings->r_string(sec, name);
		return v ? v : "";
	};
	const LPCSTR source_parent = line(source, "parent_section");
	return lease::clone_family(source, source_parent, clone, line(clone, "parent_section")) ||
	    lease::addon_variant(source, source_parent, line(source, "scopes"), clone);
}

cloneslot::Book g_clone_slots; // the slots our clones take (their old weapons' ones); under g_out_lock
xr_map<u16, replacement::Snapshot> g_clone_prepared;
xr_map<std::pair<PeerId, u16>, std::vector<u8>> g_clone_committed;
xr_set<u16> g_clone_confirmed, g_clone_canonical;
u32 g_clone_world = 0;

// Stock _g.alife_clone_weapon releases the old weapon, then registers the clone: in single player the slot is free when
// the clone comes. Here the old weapon's release goes with the clone to the host (clone_source), so it would hold the
// slot and put the clone into the backpack. It leaves the slot for the backpack now (it goes with the clone; its hands are hidden if it was in them), and the
// clone takes that slot when it spawns (clone_spawn_slot; the engine activates it as it would in single player). Our own
// placement: nothing of it goes to the host.
void free_source_slot(u16 clone, u16 source)
{
	CActor* actor = smart_cast<CActor*>(Level().Objects.net_Find(kActor));
	CInventoryItem* item = smart_cast<CInventoryItem*>(Level().Objects.net_Find(source));
	if (!actor || !item || item->m_pInventory != &actor->inventory() || item->CurrPlace() != eItemPlaceSlot)
		return;
	const u16 slot = item->CurrSlot();
	if (!actor->inventory().Ruck(item))
		return;
	xrCriticalSectionGuard guard(g_out_lock);
	g_clone_slots.plan(clone, source, slot, mgr().now_ms());
}

// A refused clone of ours is gone here: its old weapon gets its slot back (clone_slots_frame).
void clone_slots_frame(u64 now)
{
	u16 source = kNoId, slot = NO_ACTIVE_SLOT;
	{
		xrCriticalSectionGuard guard(g_out_lock);
		g_clone_slots.expire(now);
		if (!g_clone_slots.restore(source, slot))
			return;
	}
	CActor* actor = smart_cast<CActor*>(Level().Objects.net_Find(kActor));
	CInventoryItem* item = smart_cast<CInventoryItem*>(Level().Objects.net_Find(source));
	if (!actor || !item)
		return; // the source's game object may still be returning online
	if (item->m_pInventory == &actor->inventory())
	{
		CInventory& inv = actor->inventory();
		if (slot > inv.LastSlot() || inv.ItemFromSlot(slot))
			return; // the clone is still there (its destroy runs): later, or it expires
		if (item->CurrPlace() == eItemPlaceRuck)
			inv.Slot(slot, item);
	}
	xrCriticalSectionGuard guard(g_out_lock);
	g_clone_slots.restored(source);
}

// The state a clone is built with - the host's own source, captured as the client's lua_clone
// copies it (condition, upgrades, addons, magazine), checked like a client's blob (host_apply_spawn_state). Empty when
// there is no source.
bool clone_state(u16 source, u16 target, const u8* data, size_t size, std::vector<u8>& out)
{
	out.clear();
	if (source == kNoId)
		return true;
	replacement::Snapshot snapshot;
	CSE_Abstract* original = world::server_entity(source);
	return original && replacement::decode(data, size, snapshot) && snapshot.source == source && snapshot.target == target &&
	    snapshot.source_name == original->name_replace() && !snapshot.native.empty() &&
	    items::replacement_state(original, snapshot.native.data(), snapshot.native.size(), out);
}

Reason host_replace(const Req& q, CSE_Abstract* entity, u16 parent, u16& created, u16 target, u16 source,
    const u8* bytes, size_t size)
{
	if (source == kNoId)
		return host_spawn(entity, parent, created, target);
	replacement::Snapshot snapshot;
	std::vector<u8> native;
	if (!replacement::decode(bytes, size, snapshot) || !clone_state(source, target, bytes, size, native))
	{
		F_entity_Destroy(entity);
		return Reason::Malformed;
	}
	entity->ID = target;
	name_with_id(entity);
	if (!clone_stage(source, target, entity->name_replace(), snapshot.metadata, 1, replacement::token(snapshot)))
	{
		F_entity_Destroy(entity);
		return Reason::NotAllowed;
	}
	items::stage_spawn_state(target, native);
	Writer initial;
	if (!write_clone_initial(initial, entity))
	{
		clone_finish(target, false);
		items::discard_spawn_state(target);
		F_entity_Destroy(entity);
		return Reason::Malformed;
	}
	Reason result = host_spawn(entity, parent, created, target);
	if (result == Reason::Ok && !clone_activate(target))
		result = Reason::Engine;
	Writer final_initial;
	if (result == Reason::Ok && !write_clone_initial(final_initial, world::server_entity(created)))
		result = Reason::Malformed;
	if (result == Reason::Ok)
		result = host_destroy(q, source);
	if (result != Reason::Ok)
	{
		if (created != kNoId)
			host_destroy(q, created);
		clone_finish(target, false);
		items::discard_spawn_state(target);
		items::restore_item_state(world::server_entity(source), native);
		return result;
	}
	clone_finish(target, true);
	g_clone_committed[{q.from, target}] = std::vector<u8>(bytes, bytes + size);
	return Reason::Ok;
}

bool clone_committed(PeerId peer, u16 target, const u8* bytes, size_t size)
{
	const auto it = g_clone_committed.find({peer, target});
	return it != g_clone_committed.end() && it->second.size() == size &&
	    std::equal(it->second.begin(), it->second.end(), bytes);
}

bool clone_export(u16 source, std::string& metadata, const char* stamp)
{
	metadata.clear();
	int top = 0;
	lua_State* L = clone_function("clone_export", top);
	if (!L)
		return false;
	lua_pushnumber(L, source);
	if (stamp)
		lua_pushstring(L, stamp);
	return clone_return(L, top, stamp ? 2 : 1, &metadata);
}

bool clone_stage(u16 source, u16 target, const char* name, const std::string& metadata, int trust, const std::string& token)
{
	int top = 0;
	lua_State* L = clone_function("clone_stage", top);
	if (!L)
		return false;
	lua_pushnumber(L, source);
	lua_pushnumber(L, target);
	lua_pushstring(L, name);
	lua_pushlstring(L, metadata.data(), metadata.size());
	lua_pushnumber(L, trust);
	lua_pushlstring(L, token.data(), token.size());
	return clone_return(L, top, 6);
}

bool clone_activate(u16 target)
{
	int top = 0;
	lua_State* L = clone_function("clone_activate", top);
	if (!L)
		return false;
	lua_pushnumber(L, target);
	return clone_return(L, top, 1);
}

bool clone_finish(u16 target, bool accepted)
{
	int top = 0;
	lua_State* L = clone_function("clone_finish", top);
	if (!L)
		return false;
	lua_pushnumber(L, target);
	lua_pushboolean(L, accepted);
	return clone_return(L, top, 2);
}

bool prepare_local_clone(CSE_Abstract* source, CSE_Abstract* target)
{
	xrCriticalSectionGuard guard(g_out_lock);
	if (!source || !target || target->ID == kNoId)
		return false;
	replacement::Snapshot snapshot;
	snapshot.source = join::to_host(source->ID);
	snapshot.target = join::to_host(target->ID);
	snapshot.source_name = source->name_replace() ? source->name_replace() : "";
	snapshot.nonce = (mgr().now_ms() << 16) ^ ++g_clone_nonce;
	std::vector<u8> encoded;
	if (!items::capture_spawn_state(source, snapshot.native))
		return false;
	if (mgr().active() && (!clone_export(source->ID, snapshot.metadata) || !replacement::encode(snapshot, encoded)))
		return false;
	const char* why = nullptr;
	if (!items::host_apply_spawn_state(target, snapshot.native.data(), snapshot.native.size(), why) ||
	    (!snapshot.metadata.empty() &&
	        !clone_stage(source->ID, target->ID, target->name_replace(), snapshot.metadata, 0, replacement::token(snapshot))))
		return false;
	items::prepare_clone_state(source->ID, target->ID);
	items::stage_spawn_state(target->ID, snapshot.native);
	Writer initial;
	if (!snapshot.metadata.empty() && !write_clone_initial(initial, target))
	{
		clone_finish(target->ID, false);
		items::discard_spawn_state(target->ID);
		return false;
	}
	g_clone_prepared[target->ID] = std::move(snapshot);
	if (CWeaponMagazined* weapon = smart_cast<CWeaponMagazined*>(world::object_here(source->ID)))
		weapon->FireEnd();
	return true;
}

bool before_clone_register(CSE_Abstract* target)
{
	xrCriticalSectionGuard guard(g_out_lock);
	const auto it = target ? g_clone_prepared.find(target->ID) : g_clone_prepared.end();
	if (it == g_clone_prepared.end())
		return true; // an ordinary create
	const auto& snapshot = it->second;
	if (!snapshot.metadata.empty() && !clone_stage(join::to_local(snapshot.source), target->ID, target->name_replace(), snapshot.metadata, 0,
	        replacement::token(snapshot)))
		return false;
	items::stage_spawn_state(target->ID, snapshot.native);
	return true;
}

bool after_clone_register(u16 target, bool accepted)
{
	xrCriticalSectionGuard guard(g_out_lock);
	const auto it = g_clone_prepared.find(target);
	if (it == g_clone_prepared.end())
		return accepted;
	if (accepted && is_live_mirror() && !it->second.metadata.empty() && !g_clone_canonical.count(target))
	{
		g_clone_confirmed.insert(target);
		return true; // EntitySpawn is queued by registration and can follow ReqResult. Keep the item fenced until both.
	}
	accepted = accepted && (it->second.metadata.empty() || clone_activate(target));
	if (!accepted)
		items::restore_item_state(world::server_entity(join::to_local(it->second.source)), it->second.native);
	if (!it->second.metadata.empty())
		clone_finish(target, accepted);
	if (!accepted)
		items::forget_item(target);
	g_clone_prepared.erase(it);
	g_clone_confirmed.erase(target);
	g_clone_canonical.erase(target);
	return accepted;
}

bool cancel_unregistered_clone(CSE_Abstract* object)
{
	xrCriticalSectionGuard guard(g_out_lock);
	if (!object || !g_clone_prepared.count(object->ID) || world::server_entity(object->ID))
		return false;
	after_clone_register(object->ID, false);
	return true;
}

bool write_clone_initial(Writer& w, CSE_Abstract* entity)
{
	xrCriticalSectionGuard guard(g_out_lock);
	if (!smart_cast<CSE_ALifeItemWeaponMagazined*>(entity))
	{
		w.varu(0);
		return true;
	}
	replacement::Snapshot snapshot;
	snapshot.target = join::to_host(entity->ID);
	snapshot.nonce = (mgr().now_ms() << 16) ^ ++g_clone_nonce;
	snapshot.source_name = entity->name_replace() ? entity->name_replace() : "";
	const std::string stamp = replacement::token(snapshot);
	if (!clone_export(entity->ID, snapshot.metadata, stamp.c_str()))
		return false;
	items::capture_spawn_state(entity, snapshot.native); // an offline GL may have only its native save payload
	std::vector<u8> bytes;
	if (!replacement::encode(snapshot, bytes))
		return false;
	w.varu(bytes.size());
	w.bytes(bytes.data(), bytes.size());
	return true;
}

bool read_clone_initial(Reader& r, u16 target, std::vector<u8>& initial)
{
	initial.clear();
	const u64 size = r.varu();
	if (!r.ok() || size > kMaxItemState || size > r.remaining())
		return false;
	if (!size)
		return true;
	replacement::Snapshot snapshot;
	if (!replacement::decode(r.cursor(), size_t(size), snapshot) || snapshot.target != join::to_host(target))
		return false;
	initial.assign(r.cursor(), r.cursor() + size_t(size));
	return r.skip(size_t(size));
}

bool stage_clone_initial(CSE_Abstract* entity, const std::vector<u8>& initial, bool existing)
{
	xrCriticalSectionGuard guard(g_out_lock);
	if (!entity)
		return false;
	if (initial.empty())
		return !smart_cast<CSE_ALifeItemWeaponMagazined*>(entity);
	replacement::Snapshot snapshot;
	if (!replacement::decode(initial.data(), initial.size(), snapshot) || snapshot.source_name != entity->name_replace())
		return false;
	if (existing && !g_clone_prepared.count(entity->ID))
	{
		int top = 0;
		lua_State* L = clone_function("clone_adopt", top);
		if (!L)
			return false;
		lua_pushnumber(L, entity->ID);
		lua_pushlstring(L, snapshot.metadata.data(), snapshot.metadata.size());
		return clone_return(L, top, 2);
	}
	if (!clone_stage(kNoId, entity->ID, entity->name_replace(), snapshot.metadata, 2, replacement::token(snapshot)))
		return false;
	if (!snapshot.native.empty())
	{
		if (existing)
			items::restore_item_state(entity, snapshot.native);
		else
			items::stage_spawn_state(entity->ID, snapshot.native);
	}
	if (existing && g_clone_prepared.count(entity->ID))
	{
		g_clone_canonical.insert(entity->ID);
		if (g_clone_confirmed.count(entity->ID))
			return after_clone_register(entity->ID, true);
	}
	return true;
}

SpawnStamp adopt_spawn_stamp(const u8* message, size_t size, u16& id_host)
{
	id_host = kNoId;
	Reader r(message, size);
	if (r.u16() != u16(Msg::EntitySpawn))
		return SpawnStamp::NoWeapon;
	// the layout of write_spawn (coop_entities.cpp), read as client_on_spawn reads it: u16 id, u16 parent, u8 online; the placement
	// (write_placement: two vec3, u32 node, u16 graph vertex); the spawn and the update packet (write_packet: u16 length, the bytes);
	// then the clone initial (write_clone_initial)
	id_host = r.u16();
	r.u16();
	r.u8();
	float skipped[3];
	r.vec3(skipped);
	r.vec3(skipped);
	r.u32();
	r.u16();
	for (int packet = 0; packet < 2; ++packet)
		r.skip(r.u16());
	const u16 id = join::to_local(id_host);
	std::vector<u8> initial;
	if (!r.ok() || !read_clone_initial(r, id, initial))
		return SpawnStamp::Malformed;
	if (initial.empty())
		return SpawnStamp::NoWeapon;
	xrCriticalSectionGuard guard(g_out_lock);
	CSE_Abstract* entity = world::server_entity(id);
	// a clone transaction of this process's own under the id stages its own metadata (g_clone_prepared): never another's stamp
	if (!entity || g_clone_prepared.count(id))
		return SpawnStamp::NoEntity;
	// the stamp alone (coop_economy_mdata.script clone_take_stamp), not the whole adoption stage_clone_initial(existing)
	// does through clone_adopt (its SE_HOST_ONLY keys, parts seed, purge of the item's held and pending changes, resend to everyone)
	replacement::Snapshot snapshot;
	if (!replacement::decode(initial.data(), initial.size(), snapshot) ||
	    snapshot.source_name != (entity->name_replace() ? entity->name_replace() : ""))
		return SpawnStamp::Refused;
	int top = 0;
	lua_State* L = clone_function("clone_take_stamp", top);
	if (!L)
		return SpawnStamp::Refused;
	lua_pushnumber(L, entity->ID);
	lua_pushlstring(L, snapshot.metadata.data(), snapshot.metadata.size());
	return clone_return(L, top, 2) ? SpawnStamp::Taken : SpawnStamp::Refused;
}

bool clone_spawn_slot(u16 id, u16& slot)
{
	slot = NO_ACTIVE_SLOT;
	CActor* actor = smart_cast<CActor*>(Level().Objects.net_Find(kActor));
	if (!actor)
		return false;
	CInventory& inv = actor->inventory();
	xrCriticalSectionGuard guard(g_out_lock);
	const u16 planned = g_clone_slots.peek(id);
	if (planned == cloneslot::kNone)
		return false;
	// a slot is never overwritten (CInventory::Take trusts a strict place): free only when nothing holds it
	const bool free = planned <= inv.LastSlot() && !inv.ItemFromSlot(planned);
	slot = g_clone_slots.spawn_slot(id, free);
	return slot != cloneslot::kNone;
}
} // namespace requests
} // namespace coop
