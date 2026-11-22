// Anomaly Coop — TZ R32 W8.3/W8.4: the initial state of an item that rides its create, and the stock client_data of an eatable.
// An item's portions (uses) live in its game object only (CEatableItem::m_iRemainingUses); the server entity keeps them in its
// client_data, which is what the object's save wrote (CGameObject::net_Save; the world's CSE_Abstract::load takes that chunk) and
// what its next net_Spawn reads back (CGameObject::net_Spawn -> net_Load). A process with no game object of the item (the world
// server, a map server where it is offline) writes the uses into client_data in exactly that stock format, so the object that
// comes online, a save and an older build read them as a stock save would.
// The create of an item (EntitySpawn's tail, a script's coop.items.set_initial) carries uses, condition and ammo count, and the
// decider's field versions of the item (coop_owner_rules.h arrive, §8a A12).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "coop_item_tags.h"
#include "coop_wire.h"

namespace coop
{
namespace initial
{
// ---------------------------------------------------------------- the stock client_data of a held eatable
// CEatableItemObject::save (eatable_item_object.cpp) = CPhysicItem::save then CEatableItem::save:
//   CPhysicsShellHolder::save   u8 enable state (stEnable 0, stDisable 1, stNotDefitnite 2; 2 with no active shell)
//   CInventoryItem::save        u16 m_ItemCurrPlace.value, float m_fCondition, u8 sync items: 0 when the item has a holder
//                               (a held item writes nothing more; one on the ground writes its physics state after a non-zero count)
//   CEatableItem::save          u8 m_iRemainingUses
// then the script binder's bytes (CScriptBinder::save), which a process with no object cannot write: such client_data is made only
// for a section with no script binding, and an existing one is patched in place.
constexpr std::uint8_t kEnableNotDefinite = 2; // PhysicsShellHolder.cpp EEnableState::stNotDefitnite
constexpr std::size_t kPlaceAt = 1;
constexpr std::size_t kConditionAt = 3;
constexpr std::size_t kSyncItemsAt = 7;
constexpr std::size_t kUsesAt = 8;
constexpr std::size_t kHeldEatableSize = 9;

struct HeldEatable
{
	std::uint8_t enable_state = kEnableNotDefinite;
	std::uint16_t place = 0; // eItemPlaceUndefined with no slot: what a fresh object has (CInventoryItem::CInventoryItem)
	float condition = 1.f;
	std::uint8_t uses = 0;
};

// Is `cd` the client_data of a held eatable (its uses byte is where kUsesAt says)?
inline bool is_held_eatable(const std::vector<std::uint8_t>& cd)
{
	return cd.size() >= kHeldEatableSize && cd[kSyncItemsAt] == 0;
}

// A fresh client_data of a held eatable with no script binding.
inline std::vector<std::uint8_t> make_held_eatable(const HeldEatable& h)
{
	std::vector<std::uint8_t> cd(kHeldEatableSize, 0);
	cd[0] = h.enable_state;
	cd[kPlaceAt] = std::uint8_t(h.place & 0xFFu);
	cd[kPlaceAt + 1] = std::uint8_t(h.place >> 8);
	static_assert(sizeof(float) == 4, "CInventoryItem::save writes a 4-byte float");
	std::memcpy(&cd[kConditionAt], &h.condition, sizeof(float)); // NET_Packet::w_float: the raw bytes (x86: little endian)
	cd[kSyncItemsAt] = 0;
	cd[kUsesAt] = h.uses;
	return cd;
}

inline bool read_held_eatable(const std::vector<std::uint8_t>& cd, HeldEatable& h)
{
	if (!is_held_eatable(cd))
		return false;
	h.enable_state = cd[0];
	h.place = std::uint16_t(cd[kPlaceAt] | (std::uint16_t(cd[kPlaceAt + 1]) << 8));
	std::memcpy(&h.condition, &cd[kConditionAt], sizeof(float));
	h.uses = cd[kUsesAt];
	return true;
}

// Write `uses` (and, when `cond` is not null, the condition) into the client_data of an item that has a holder. Empty client_data
// is made fresh only when `may_make` (the section has no script binding: no binder bytes would follow). False: client_data of an
// item on the ground (physics state, not patched) or empty with !may_make; `cd` is unchanged then.
inline bool write_uses(std::vector<std::uint8_t>& cd, std::uint8_t uses, const float* cond, bool may_make)
{
	if (cd.empty())
	{
		if (!may_make)
			return false;
		HeldEatable h;
		h.uses = uses;
		if (cond)
			h.condition = *cond;
		cd = make_held_eatable(h);
		return true;
	}
	if (!is_held_eatable(cd))
		return false;
	cd[kUsesAt] = uses;
	if (cond)
		std::memcpy(&cd[kConditionAt], cond, sizeof(float));
	return true;
}

inline bool read_uses(const std::vector<std::uint8_t>& cd, std::uint8_t& uses)
{
	if (!is_held_eatable(cd))
		return false;
	uses = cd[kUsesAt];
	return true;
}

// ---------------------------------------------------------------- which sections have portions (WP6 fix round, finding 5)
// The layout above says nothing of the kind of item: every held inventory item's client_data starts the same way (CInventoryItem::save
// writes 0 for a held item), and a weapon's continues with its rounds where an eatable has its uses byte. So whether an entity has
// uses is decided by its section's `class`: the eatable classes of the object factory (CEatableItemObject's subclasses: engine
// xrServerEntities/object_factory_register.cpp :345-349 CMedkit x2, CAntirad, CFoodItem, CBottleItem; class_registrator.script :118
// S_FOOD; the same rows in coop_spawn.h kClassRules). Trailing blanks of an ini value are ignored (TEXT2CLSID pads with blanks).
inline bool is_eatable_class(std::string_view clsid)
{
	while (!clsid.empty() && (clsid.back() == ' ' || clsid.back() == '\t'))
		clsid.remove_suffix(1);
	constexpr std::string_view kEatable[] = {"II_MEDKI", "II_BANDG", "II_ANTIR", "II_FOOD", "II_BOTTL", "S_FOOD"};
	for (const std::string_view c : kEatable)
		if (c == clsid)
			return true;
	return false;
}

// ---------------------------------------------------------------- the writer's own bytes (WP6 fix round, finding 1)
// A section with a script binding (G.A.M.M.A.'s whole multi-use family: [booster_multi] has script_binding = bind_item.bind) keeps
// its binder's bytes after the uses, and a process with no game object cannot make them: a fresh client_data of such an item cannot be
// built. The map server has the object: its ItemFacts carry the object's own client_data (coop_item_tags.h kClientDataTag), the
// exact bytes its stock save leaves, and the world stores them.
constexpr std::size_t kMaxClientData = 1024; // an eatable's save and its binder's are tens of bytes; well under the blob's 4096

// What CGameObject::net_Save leaves in a packet of its own: u16 size, then the body the entity keeps as client_data (CSE_Abstract::load
// reads exactly that chunk). False: the packet is not exactly that, or the body is empty or longer than kMaxClientData.
inline bool client_data_of_save(const std::uint8_t* packet, std::size_t count, std::vector<std::uint8_t>& out)
{
	out.clear();
	if (!packet || count < sizeof(std::uint16_t))
		return false;
	const std::size_t said = std::size_t(packet[0]) | (std::size_t(packet[1]) << 8);
	if (said + sizeof(std::uint16_t) != count || said == 0 || said > kMaxClientData)
		return false;
	out.assign(packet + sizeof(std::uint16_t), packet + count);
	return true;
}

// The writer's bytes are taken when they are what the stock save of a held eatable is, with the same uses as the facts they came with
// (captured together), for an entity that has a holder here too (an item on the ground keeps its physics state, not touched).
inline bool stock_bytes_ok(const std::vector<std::uint8_t>& stock, std::uint8_t uses, bool held)
{
	return held && stock.size() <= kMaxClientData && is_held_eatable(stock) && stock[kUsesAt] == uses;
}

// How a process with no game object of an eatable stores its uses in the entity's client_data.
enum class UsesStore : std::uint8_t
{
	Stock, // the writer's own bytes of its object (its binder's included)
	Patch, // the uses byte of the held eatable's client_data here
	Make, // a fresh client_data: there is none yet and the section has no script binding
	Refuse, // none of these: a scripted section with no client_data and no writer's bytes, or client_data of an item on the ground
};

inline UsesStore uses_store(
    const std::vector<std::uint8_t>& cd, const std::vector<std::uint8_t>* stock, std::uint8_t uses, bool held, bool may_make)
{
	if (stock && stock_bytes_ok(*stock, uses, held))
		return UsesStore::Stock;
	if (cd.empty())
		return may_make ? UsesStore::Make : UsesStore::Refuse;
	return is_held_eatable(cd) ? UsesStore::Patch : UsesStore::Refuse;
}

// Store the uses as `how` says. With the writer's bytes the item's place (bytes 1-2) stays what the client_data here has
// (eItemPlaceUndefined when it had none, as Make writes): the writer's object is a copy whose slot is not the player's to set (the
// player's own layout puts his things back, coop_equip.cpp on_item_spawn). False for Refuse; `cd` is unchanged then.
inline bool store_uses(std::vector<std::uint8_t>& cd, UsesStore how, const std::vector<std::uint8_t>* stock, std::uint8_t uses,
    const float* cond, bool may_make)
{
	switch (how)
	{
	case UsesStore::Stock:
	{
		if (!stock || !is_held_eatable(*stock))
			return false;
		std::vector<std::uint8_t> out = *stock;
		const bool had = is_held_eatable(cd);
		out[kPlaceAt] = had ? cd[kPlaceAt] : std::uint8_t(0);
		out[kPlaceAt + 1] = had ? cd[kPlaceAt + 1] : std::uint8_t(0);
		cd.swap(out);
		return true;
	}
	case UsesStore::Patch:
	case UsesStore::Make: return write_uses(cd, uses, cond, may_make);
	case UsesStore::Refuse: break;
	}
	return false;
}

// ---------------------------------------------------------------- the initial state of a create (protocol 34)
// Wire: varu n (0: nothing), then n bytes: u8 flags (kHasUses 1, kHasCond 2, kHasAmmo 4, kHasVersions 8), u8 uses, u16 condition
// (q 1/65535), u16 ammo count, the field versions (coop_item_tags.h write_versions), each only when its flag is set.
enum Flag : std::uint8_t
{
	kHasUses = 1,
	kHasCond = 2,
	kHasAmmo = 4,
	kHasVersions = 8,
};
constexpr std::uint8_t kKnownFlags = kHasUses | kHasCond | kHasAmmo | kHasVersions;
constexpr std::size_t kMaxInitial = 1 + 1 + 2 + 2 + 1 + 2 * itemtags::kTagCount;

struct State
{
	bool has_uses = false;
	std::uint8_t uses = 0;
	bool has_cond = false;
	std::uint16_t cond = 0;
	bool has_ammo = false;
	std::uint16_t ammo = 0;
	itemtags::FieldVersions versions;
	std::uint32_t versions_mask = 0; // the tags whose version rides (0: none)

	bool empty() const { return !has_uses && !has_cond && !has_ammo && versions_mask == 0; }
};

inline std::uint16_t quant(float c)
{
	const float x = c < 0.f ? 0.f : (c > 1.f ? 1.f : c);
	return std::uint16_t(x * 65535.f + 0.5f);
}
inline float dequant(std::uint16_t q) { return float(q) / 65535.f; }

inline void write(Writer& w, const State& s)
{
	if (s.empty())
	{
		w.varu(0);
		return;
	}
	Writer b(kMaxInitial);
	std::uint8_t flags = 0;
	flags |= s.has_uses ? kHasUses : 0;
	flags |= s.has_cond ? kHasCond : 0;
	flags |= s.has_ammo ? kHasAmmo : 0;
	flags |= (s.versions_mask & itemtags::kAll) ? kHasVersions : 0;
	b.u8(flags);
	if (s.has_uses)
		b.u8(s.uses);
	if (s.has_cond)
		b.u16(s.cond);
	if (s.has_ammo)
		b.u16(s.ammo);
	if (flags & kHasVersions)
		itemtags::write_versions(b, s.versions, s.versions_mask);
	w.varu(b.size());
	w.bytes(b.ptr(), b.size());
}

// False for a cut or malformed body (unknown flags, bytes left over); `s` is cleared then. The reader moves past the body either way
// when its length fits.
inline bool read(Reader& r, State& s)
{
	s = State();
	const std::uint64_t n = r.varu();
	if (!r.ok() || n > r.remaining())
		return false;
	if (n == 0)
		return true;
	if (n > kMaxInitial)
	{
		r.skip(std::size_t(n));
		return false;
	}
	Reader b(r.cursor(), std::size_t(n));
	r.skip(std::size_t(n));
	State t;
	const std::uint8_t flags = b.u8();
	if (!b.ok() || (flags & ~kKnownFlags) != 0)
		return false;
	if (flags & kHasUses)
	{
		t.uses = b.u8();
		t.has_uses = true;
	}
	if (flags & kHasCond)
	{
		t.cond = b.u16();
		t.has_cond = true;
	}
	if (flags & kHasAmmo)
	{
		t.ammo = b.u16();
		t.has_ammo = true;
	}
	if (flags & kHasVersions)
	{
		if (!itemtags::read_versions(b, t.versions, t.versions_mask) || t.versions_mask == 0)
			return false;
	}
	if (!b.ok() || b.remaining() != 0)
		return false;
	s = t;
	return true;
}

// The uses a script may set: 0 .. max_uses of the section (CEatableItem::Load reads max_uses, a u8, 1 when missing).
inline bool uses_ok(std::uint32_t uses, std::uint32_t max_uses)
{
	return uses <= max_uses && uses <= 255u;
}
} // namespace initial
} // namespace coop
