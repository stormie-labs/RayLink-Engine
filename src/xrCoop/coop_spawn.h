// Anomaly Coop — a client's ReqSpawn on the wire and what the host checks before it constructs the entity (R3 В6).
// The host never reads a peer's raw M_SPAWN bytes (Spawn_Read trusts every byte): it builds the entity from its own
// section, after checking that the section has the lines the entity's constructor reads unchecked.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include "coop_wire.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coop
{
// Largest item state blob: coop_items ItemState and the item state of a ReqSpawn.
constexpr size_t kMaxItemState = 4096;

namespace spawn
{
constexpr uint8_t kMarkerRaw = 0xC5; // raw M_SPAWN bytes (older clients): refused
constexpr uint8_t kMarkerState = 0xC6; // section + item state
constexpr size_t kMaxSection = 255; // string256

// Wire (client -> host, ids in host numbering):
//   State   u32 seq, u8 0xC6, u16 parent, vec3 pos, str section, varu n, n bytes item state (coop_items blob)
//   Legacy  str section, u16 parent, vec3 pos  (no seq)
//   Raw     u32 seq, u8 0xC5, ...              (refused)
enum class Layout : uint8_t
{
	Legacy,
	State,
	Raw,
};

// Byte 4 is the marker after u32 seq. The legacy layout starts with a short string (length < 0x80), so its byte 4
// is a letter of the section (or the parent id for a section shorter than 4 characters).
inline Layout detect(const uint8_t* data, size_t size)
{
	if (!data || size < 5)
		return Layout::Legacy;
	if (data[4] == kMarkerState)
		return Layout::State;
	if (data[4] == kMarkerRaw)
		return Layout::Raw;
	return Layout::Legacy;
}

inline bool has_seq(Layout layout) { return layout != Layout::Legacy; }

struct Request
{
	uint16_t parent = 0xffff;
	float pos[3] = {0.f, 0.f, 0.f};
	std::string section;
	const uint8_t* state = nullptr; // points into the message
	size_t state_size = 0;
};

// Reads what follows u32 seq (State) or the whole message (Legacy). False = malformed, or the Raw layout.
inline bool read_body(Reader& r, Layout layout, Request& out)
{
	out = Request();
	if (layout == Layout::Raw)
		return false;
	if (layout == Layout::Legacy)
	{
		out.section = r.str();
		out.parent = r.u16();
		r.vec3(out.pos);
		return r.ok();
	}
	r.u8(); // marker
	out.parent = r.u16();
	r.vec3(out.pos);
	out.section = r.str();
	const uint64_t n = r.varu();
	if (!r.ok() || n > kMaxItemState || n > r.remaining())
		return false;
	out.state = r.cursor();
	out.state_size = size_t(n);
	return r.skip(out.state_size);
}

inline void write_state(Writer& w, uint32_t seq, uint16_t parent, const float pos[3], std::string_view section,
    const uint8_t* state, size_t state_size)
{
	w.u32(seq);
	w.u8(kMarkerState);
	w.u16(parent);
	w.vec3(pos);
	w.str(section);
	w.varu(state_size);
	if (state && state_size)
		w.bytes(state, state_size);
}

// Classes whose server entity constructors read their own lines unchecked (FATAL on a missing line,
// xrServer_Objects_ALife_Items.cpp). By the `class` id as written in the section: engine ids
// (object_factory_register.cpp) and Anomaly's script ids (class_registrator.script). Everything else is Other.
// Partial on purpose (R3 В6 minimum): the full "class id -> required lines" table is phase 2.
enum class ItemKind : uint8_t
{
	Other,
	Weapon, // CSE_ALifeItemWeapon and descendants
	Ammo, // CSE_ALifeItemAmmo
	Detector, // CSE_ALifeItemDetector
	Outfit, // CSE_ALifeItemCustomOutfit
};

inline ItemKind item_kind(std::string_view clsid)
{
	while (!clsid.empty() && (clsid.back() == ' ' || clsid.back() == '\t'))
		clsid.remove_suffix(1);
	constexpr std::string_view kWeapons[] = {"W_KNIFE", "W_BINOC", "W_FORT", "W_HPSA", "W_LR300", "W_WMAGAZ",
	    "W_PM", "W_RPG7", "_W_SSRS", "W_SVD", "W_SVU", "W_USP45", "W_VAL", "W_VINT", "W_WALTHR", "W_AK74", "W_FN2000",
	    "W_GROZA", "W_WMAGGL", "W_BM16", "W_RG6", "W_SHOTGN", "WP_BINOC", "WP_KNIFE", "WP_BM16", "WP_GROZA", "WP_SVD",
	    "WP_AK74", "WP_LR300", "WP_HPSA", "WP_PM", "WP_RG6", "WP_RPG7", "WP_SHOTG", "WP_ASHTG", "WP_SVU", "WP_USP45",
	    "WP_VAL", "WP_VINT", "WP_WALTH", "_WP_SSRS"};
	constexpr std::string_view kAmmo[] = {"AMMO", "A_VOG25", "A_OG7B", "A_M209", "AMMO_S", "S_VOG25", "S_OG7B",
	    "S_M209"};
	constexpr std::string_view kDetectors[] = {"D_SIMDET", "D_ADVANC", "D_ELITE", "D_SCIENT", "DET_SIMP", "DET_ADVA",
	    "DET_ELIT", "DET_SCIE"};
	constexpr std::string_view kOutfits[] = {"EQU_SCIE", "EQU_STLK", "EQU_MLTR", "EQU_EXO", "E_STLK"};
	for (const std::string_view id : kWeapons)
		if (clsid == id)
			return ItemKind::Weapon;
	for (const std::string_view id : kAmmo)
		if (clsid == id)
			return ItemKind::Ammo;
	for (const std::string_view id : kDetectors)
		if (clsid == id)
			return ItemKind::Detector;
	for (const std::string_view id : kOutfits)
		if (clsid == id)
			return ItemKind::Outfit;
	return ItemKind::Other;
}

// Lines a section must have before a client-asked entity of it is constructed: inv_weight and cost, which
// CSE_ALifeInventoryItem reads unconditionally (this also keeps every non-item out), plus the class's own.
inline std::vector<const char*> required_lines(ItemKind kind)
{
	std::vector<const char*> lines = {"inv_weight", "cost"};
	switch (kind)
	{
	case ItemKind::Weapon:
		lines.insert(lines.end(), {"hit_power", "hit_type", "ammo_class", "scope_status", "silencer_status",
		                              "grenade_launcher_status"});
		break;
	case ItemKind::Ammo: lines.push_back("box_size");
		break;
	case ItemKind::Detector: lines.push_back("ef_detector_type");
		break;
	case ItemKind::Outfit: lines.push_back("ef_equipment_type");
		break;
	case ItemKind::Other: break;
	}
	return lines;
}
} // namespace spawn
} // namespace coop
