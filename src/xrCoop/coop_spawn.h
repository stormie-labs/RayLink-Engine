// Anomaly Coop — a client's ReqSpawn on the wire, what the host checks before it constructs the entity (R3 В6) and
// what a client may ask for at all: engine item classes, no quest items, a cost budget per character (F2 T2).
// The host never reads a peer's raw M_SPAWN bytes (Spawn_Read trusts every byte): it builds the entity from its own
// section, after checking that the section has the lines the entity's constructor reads unchecked.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include "coop_wire.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
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
constexpr uint8_t kMarkerLeased = 0xC7; // section + item state, created on the client under a leased id (F2 T12b)
constexpr size_t kMaxSection = 255; // string256
constexpr uint16_t kNoId = 0xffff;

// Wire (client -> host, ids in host numbering):
//   State   u32 seq, u8 0xC6, u16 parent, vec3 pos, str section, varu n, n bytes item state (coop_items blob)
//   Leased  u32 seq, u8 0xC7, u16 parent, u16 id, u16 source, vec3 pos, str section, varu n, n bytes item state
//           id: leased to the asker (xrCoop/coop_id_lease.h), the client has the item under it already; source: the
//           weapon a clone replaces (alife():clone_weapon; the host releases it with the clone), 0xffff for a create
//   Legacy  str section, u16 parent, vec3 pos  (no seq)
//   Raw     u32 seq, u8 0xC5, ...              (refused)
enum class Layout : uint8_t
{
	Legacy,
	State,
	Raw,
	Leased, // F2 T12b
};

// Byte 4 is the marker after u32 seq. The legacy layout starts with a short string (length < 0x80), so its byte 4
// is a letter of the section (or the parent id for a section shorter than 4 characters).
inline Layout detect(const uint8_t* data, size_t size)
{
	if (!data || size < 5)
		return Layout::Legacy;
	if (data[4] == kMarkerState)
		return Layout::State;
	if (data[4] == kMarkerLeased)
		return Layout::Leased;
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
	uint16_t id = kNoId; // Leased: the id the client created it under
	uint16_t source = kNoId; // Leased: the weapon a clone replaces
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
	const uint8_t marker = r.u8();
	out.parent = r.u16();
	if (marker == kMarkerLeased)
	{
		out.id = r.u16();
		out.source = r.u16();
		if (!r.ok() || out.id == 0 || out.id == kNoId)
			return false; // 0 is the actor, 0xffff no id: never leased
	}
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

// F2 T12b: the request of an item the client created already under leased `id` (source: a clone's weapon, or kNoId).
inline void write_leased(Writer& w, uint32_t seq, uint16_t parent, uint16_t id, uint16_t source, const float pos[3],
    std::string_view section, const uint8_t* state, size_t state_size)
{
	w.u32(seq);
	w.u8(kMarkerLeased);
	w.u16(parent);
	w.u16(id);
	w.u16(source);
	w.vec3(pos);
	w.str(section);
	w.varu(state_size);
	if (state && state_size)
		w.bytes(state, state_size);
}

// ---------------------------------------------------------------- what a client may ask for (F2 T2)
// A client may ask the host only for an item of an engine item class: the entity a section's `class` names must be
// an inventory item (CSE_ALifeItem and below) or a launcher's rocket (CSE_Temporary), registered in a game that is
// not a dedicated server. Everything else (stalkers, monsters, zones, physics objects, boxes, squads) has no rule.

// Server entity class of a rule: the C++ class the factory constructs for the clsid (directly, or as the base of the
// Lua server class Anomaly registers for it).
enum class CseClass : uint8_t
{
	Item, // CSE_ALifeItem
	Weapon, // CSE_ALifeItemWeapon
	WeaponMagazined, // CSE_ALifeItemWeaponMagazined
	WeaponMagazinedWGL, // CSE_ALifeItemWeaponMagazinedWGL
	WeaponShotGun, // CSE_ALifeItemWeaponShotGun
	Ammo, // CSE_ALifeItemAmmo
	Grenade, // CSE_ALifeItemGrenade
	Explosive, // CSE_ALifeItemExplosive
	Bolt, // CSE_ALifeItemBolt
	Artefact, // CSE_ALifeItemArtefact
	Detector, // CSE_ALifeItemDetector
	CustomOutfit, // CSE_ALifeItemCustomOutfit
	Torch, // CSE_ALifeItemTorch
	PDA, // CSE_ALifeItemPDA
	Document, // CSE_ALifeItemDocument
	// Classes Anomaly registers too (not in the F2 T2 brief's list):
	WeaponAutoShotGun, // CSE_ALifeItemWeaponAutoShotGun: WP_ASHTG (se_weapon_automatic_shotgun)
	Helmet, // CSE_ALifeItemHelmet: E_HLMET (se_helmet)
	Temporary, // CSE_Temporary: G_RPG7, G_FAKE, a launcher's rocket the client's weapon asks for as its child (W3a)
};
constexpr uint8_t kCseClassCount = uint8_t(CseClass::Temporary) + 1;

// The game object the factory builds for a clsid (its client class), grouped by what its constructor, Load and
// net_Spawn read from the item's own section without line_exist (Ruling F2-17). The host builds that object too
// (Process_spawn), so a missing line of it is as FATAL as one of the server entity. Lines: kGameGroups below.
enum class GameClass : uint8_t
{
	InventoryItem, // CInventoryItemObject: addons, eatables, documents, attachables, the MP bag
	GrenadeLauncher, // CGrenadeLauncher
	Ammo, // CWeaponAmmo
	CustomOutfit, // CCustomOutfit and the outfits below it
	Helmet, // CHelmet
	Backpack, // CBackpack
	Torch, // CTorch
	ExplosiveItem, // CExplosiveItem
	Missile, // CBolt (CMissile)
	Grenade, // CF1, CRGD5 (CGrenade)
	ExplosiveRocket, // CExplosiveRocket
	Artefact, // CArtefact and the artefacts whose own Load reads nothing unchecked
	BastArtefact, // CBastArtefact
	BlackGraviArtefact, // CBlackGraviArtefact
	MercuryBall, // CMercuryBall
	Pda, // CPda
	Flare, // CFlare
	CustomDevice, // CCustomDevice, CFlashlight, CDosimeter
	CustomDetector, // CCustomDetector and the four detectors
	WeaponKnife, // CWeaponKnife
	WeaponMagazined, // CWeaponMagazined and the weapons below it without a grenade or rocket launcher
	WeaponBinoculars, // CWeaponBinoculars
	WeaponMagazinedWGrenade, // CWeaponMagazinedWGrenade, CWeaponAK74, CWeaponGroza
	WeaponRocket, // CWeaponRG6, CWeaponSSRS (CRocketLauncher)
	WeaponRPG7, // CWeaponRPG7
};
constexpr uint8_t kGameClassCount = uint8_t(GameClass::WeaponRPG7) + 1;

struct ClassRule
{
	std::string_view clsid; // the `class` value of a section, as TEXT2CLSID reads it (case-sensitive, <= 8 chars)
	CseClass cse;
	GameClass game; // its game object (Ruling F2-17)
};

// Every item class of the game, one rule per clsid, with its server entity class and the group of its game object (the
// client class of the same registration: every Load above it must be in its group's `loads`). Sources
// (tools/check_spawn_classes.py checks this table against them and fails on any difference, the cited lines included):
//   engine :N  engine/src/xrServerEntities/object_factory_register.cpp line N, an ADD(..., CSE_ALifeItem* or
//              CSE_Temporary, id, ...) before `if (!g_dedicated_server) return;` (the ADDs after it exist only on a
//              dedicated server); id from clsid_game.h or TEXT2CLSID;
//   <script> :N  line N of a class registrator script (configs\script.ltx [common] class_registrators with its DLTX
//              mods): a cs_register whose Lua server class derives from a cse_alife_item* class (se_item.script,
//              se_artefact.script).
inline constexpr ClassRule kClassRules[] = {
	// engine: artefacts
	{"AF_MBALL", CseClass::Artefact, GameClass::MercuryBall}, // engine :287
	{"AF_BDROP", CseClass::Artefact, GameClass::Artefact}, // engine :288
	{"AF_NEEDL", CseClass::Artefact, GameClass::BlackGraviArtefact}, // engine :289
	{"AF_BAST", CseClass::Artefact, GameClass::BastArtefact}, // engine :290
	{"AF_BGRAV", CseClass::Artefact, GameClass::BlackGraviArtefact}, // engine :291
	{"AF_DUMMY", CseClass::Artefact, GameClass::Artefact}, // engine :292
	{"AF_ZUDA", CseClass::Artefact, GameClass::Artefact}, // engine :293
	{"AF_THORN", CseClass::Artefact, GameClass::Artefact}, // engine :294
	{"AF_FBALL", CseClass::Artefact, GameClass::Artefact}, // engine :295
	{"AF_EBALL", CseClass::Artefact, GameClass::Artefact}, // engine :296
	{"AF_RHAIR", CseClass::Artefact, GameClass::Artefact}, // engine :297
	{"AF_GALAN", CseClass::Artefact, GameClass::Artefact}, // engine :298
	{"AF_GRAVI", CseClass::Artefact, GameClass::Artefact}, // engine :299
	{"ARTEFACT", CseClass::Artefact, GameClass::Artefact}, // engine :300
	{"AF_CTA", CseClass::Artefact, GameClass::Artefact}, // engine :301
	// engine: weapons
	{"W_WMAGAZ", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :304
	{"W_WMAGGL", CseClass::WeaponMagazinedWGL, GameClass::WeaponMagazinedWGrenade}, // engine :307
	{"W_FN2000", CseClass::WeaponMagazinedWGL, GameClass::WeaponMagazined}, // engine :309
	{"W_AK74", CseClass::WeaponMagazinedWGL, GameClass::WeaponMagazinedWGrenade}, // engine :310
	{"W_LR300", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :311
	{"W_HPSA", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :312
	{"W_PM", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :313
	{"W_FORT", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :314
	{"W_BINOC", CseClass::WeaponMagazined, GameClass::WeaponBinoculars}, // engine :315
	{"W_SHOTGN", CseClass::WeaponShotGun, GameClass::WeaponMagazined}, // engine :316
	{"W_SVD", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :317
	{"W_SVU", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :318
	{"W_RPG7", CseClass::WeaponMagazined, GameClass::WeaponRPG7}, // engine :319
	{"W_VAL", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :320
	{"W_VINT", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :321
	{"W_WALTHR", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :322
	{"W_USP45", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // engine :323
	{"W_GROZA", CseClass::WeaponMagazinedWGL, GameClass::WeaponMagazinedWGrenade}, // engine :324
	{"W_KNIFE", CseClass::Weapon, GameClass::WeaponKnife}, // engine :325
	{"W_BM16", CseClass::WeaponShotGun, GameClass::WeaponMagazined}, // engine :326
	{"W_RG6", CseClass::WeaponShotGun, GameClass::WeaponRocket}, // engine :327
	{"_W_SSRS", CseClass::WeaponMagazined, GameClass::WeaponRocket}, // engine :431
	// engine: ammo, addons
	{"AMMO", CseClass::Ammo, GameClass::Ammo}, // engine :329
	{"A_VOG25", CseClass::Ammo, GameClass::Ammo}, // engine :330
	{"A_OG7B", CseClass::Ammo, GameClass::Ammo}, // engine :331
	{"A_M209", CseClass::Ammo, GameClass::Ammo}, // engine :332
	{"W_SCOPE", CseClass::Item, GameClass::InventoryItem}, // engine :336
	{"W_SILENC", CseClass::Item, GameClass::InventoryItem}, // engine :337
	{"W_GLAUNC", CseClass::Item, GameClass::GrenadeLauncher}, // engine :338
	// engine: inventory
	{"II_BOLT", CseClass::Bolt, GameClass::Missile}, // engine :342
	{"II_MEDKI", CseClass::Item, GameClass::InventoryItem}, // engine :344
	{"II_BANDG", CseClass::Item, GameClass::InventoryItem}, // engine :345
	{"II_ANTIR", CseClass::Item, GameClass::InventoryItem}, // engine :346
	{"II_FOOD", CseClass::Item, GameClass::InventoryItem}, // engine :347
	{"II_BOTTL", CseClass::Item, GameClass::InventoryItem}, // engine :348
	{"II_EXPLO", CseClass::Explosive, GameClass::ExplosiveItem}, // engine :349
	{"II_DOC", CseClass::Document, GameClass::InventoryItem}, // engine :352
	{"II_ATTCH", CseClass::Item, GameClass::InventoryItem}, // engine :353
	// engine: equipment
	{"EQU_SCIE", CseClass::CustomOutfit, GameClass::CustomOutfit}, // engine :356
	{"EQU_STLK", CseClass::CustomOutfit, GameClass::CustomOutfit}, // engine :357
	{"EQU_MLTR", CseClass::CustomOutfit, GameClass::CustomOutfit}, // engine :358
	{"EQU_EXO", CseClass::CustomOutfit, GameClass::CustomOutfit}, // engine :359
	{"EQ_HLMET", CseClass::Item, GameClass::Helmet}, // engine :360
	{"EQ_BAKPK", CseClass::Item, GameClass::Backpack}, // engine :361
	// engine: grenades, rockets
	{"G_F1", CseClass::Grenade, GameClass::Grenade}, // engine :364
	{"G_RGD5", CseClass::Grenade, GameClass::Grenade}, // engine :365
	{"G_RPG7", CseClass::Temporary, GameClass::ExplosiveRocket}, // engine :368
	{"G_FAKE", CseClass::Temporary, GameClass::ExplosiveRocket}, // engine :369
	{"MP_PLBAG", CseClass::Item, GameClass::InventoryItem}, // engine :372
	// engine: detectors, devices
	{"D_SIMDET", CseClass::Detector, GameClass::CustomDetector}, // engine :397
	{"D_ADVANC", CseClass::Detector, GameClass::CustomDetector}, // engine :398
	{"D_ELITE", CseClass::Detector, GameClass::CustomDetector}, // engine :399
	{"D_SCIENT", CseClass::Detector, GameClass::CustomDetector}, // engine :400
	{"D_TORCH", CseClass::Torch, GameClass::Torch}, // engine :403
	{"D_PDA", CseClass::PDA, GameClass::Pda}, // engine :404
	{"D_FLARE", CseClass::Item, GameClass::Flare}, // engine :405
	{"D_FLALIT", CseClass::Item, GameClass::CustomDevice}, // engine :406
	{"D_DSMETR", CseClass::Item, GameClass::CustomDevice}, // engine :407
	{"D_CUSTOM", CseClass::Item, GameClass::CustomDevice}, // engine :408
	// script (class_registrator.script)
	{"SCRPTART", CseClass::Artefact, GameClass::Artefact}, // class_registrator :36
	{"TORCH_S", CseClass::Torch, GameClass::Torch}, // class_registrator :59
	{"DET_SCIE", CseClass::Detector, GameClass::CustomDetector}, // class_registrator :62
	{"DET_ELIT", CseClass::Detector, GameClass::CustomDetector}, // class_registrator :63
	{"DET_ADVA", CseClass::Detector, GameClass::CustomDetector}, // class_registrator :64
	{"DET_SIMP", CseClass::Detector, GameClass::CustomDetector}, // class_registrator :65
	{"S_PDA", CseClass::PDA, GameClass::Pda}, // class_registrator :66
	{"E_STLK", CseClass::CustomOutfit, GameClass::CustomOutfit}, // class_registrator :72
	{"E_HLMET", CseClass::Helmet, GameClass::Helmet}, // class_registrator :73
	{"WP_BINOC", CseClass::WeaponMagazined, GameClass::WeaponBinoculars}, // class_registrator :76
	{"WP_KNIFE", CseClass::Weapon, GameClass::WeaponKnife}, // class_registrator :77
	{"WP_BM16", CseClass::WeaponShotGun, GameClass::WeaponMagazined}, // class_registrator :78
	{"WP_GROZA", CseClass::WeaponMagazinedWGL, GameClass::WeaponMagazinedWGrenade}, // class_registrator :79
	{"WP_SVD", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :80
	{"WP_AK74", CseClass::WeaponMagazinedWGL, GameClass::WeaponMagazinedWGrenade}, // class_registrator :81
	{"WP_LR300", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :82
	{"WP_HPSA", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :83
	{"WP_PM", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :84
	{"WP_RG6", CseClass::WeaponShotGun, GameClass::WeaponRocket}, // class_registrator :85
	{"WP_RPG7", CseClass::WeaponMagazined, GameClass::WeaponRPG7}, // class_registrator :86
	{"WP_SHOTG", CseClass::WeaponShotGun, GameClass::WeaponMagazined}, // class_registrator :87
	{"WP_ASHTG", CseClass::WeaponAutoShotGun, GameClass::WeaponMagazined}, // class_registrator :88
	{"WP_SVU", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :90
	{"WP_USP45", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :91
	{"WP_VAL", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :92
	{"WP_VINT", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :93
	{"WP_WALTH", CseClass::WeaponMagazined, GameClass::WeaponMagazined}, // class_registrator :94
	{"AMMO_S", CseClass::Ammo, GameClass::Ammo}, // class_registrator :108
	{"S_VOG25", CseClass::Ammo, GameClass::Ammo}, // class_registrator :109
	{"S_OG7B", CseClass::Ammo, GameClass::Ammo}, // class_registrator :110
	{"S_M209", CseClass::Ammo, GameClass::Ammo}, // class_registrator :111
	{"G_F1_S", CseClass::Grenade, GameClass::Grenade}, // class_registrator :114
	{"G_RGD5_S", CseClass::Grenade, GameClass::Grenade}, // class_registrator :115
	{"S_FOOD", CseClass::Item, GameClass::InventoryItem}, // class_registrator :118
	{"WP_SCOPE", CseClass::Item, GameClass::InventoryItem}, // class_registrator :121
	{"WP_SILEN", CseClass::Item, GameClass::InventoryItem}, // class_registrator :122
	{"WP_GLAUN", CseClass::Item, GameClass::GrenadeLauncher}, // class_registrator :123
	{"S_EXPLO", CseClass::Explosive, GameClass::ExplosiveItem}, // class_registrator :129
	// script (engine/gamedata/scripts/class_registrator_modded_exes.script, added by mod_script_dxml.ltx)
	{"_WP_SSRS", CseClass::WeaponMagazined, GameClass::WeaponRocket}, // class_registrator_modded_exes :17
};

// The rule of a section's `class` value (trailing blanks of an ini value ignored), nullptr for anything that is not
// an item class of the game: stalkers, monsters, zones, physics objects, boxes, squads, unknown ids.
inline const ClassRule* class_rule(std::string_view clsid)
{
	while (!clsid.empty() && (clsid.back() == ' ' || clsid.back() == '\t'))
		clsid.remove_suffix(1);
	if (clsid.empty())
		return nullptr;
	for (const ClassRule& rule : kClassRules)
		if (rule.clsid == clsid)
			return &rule;
	return nullptr;
}

// Lines a section must have before the host constructs a client-asked entity of it: the lines the constructor of the
// class's server entity and of its bases read without line_exist (FATAL on a missing line). All in
// engine/src/xrServerEntities/xrServer_Objects_ALife_Items.cpp unless named otherwise; `class` itself
// (CSE_Abstract, xrServer_Object_Base.cpp:102) is checked before (spawnable_section), `visual`
// (CSE_ALifeDynamicObjectVisual, xrServer_Objects_ALife.cpp:722) is read only when it exists.
inline std::vector<const char*> required_lines(CseClass cse)
{
	// CSE_ALifeInventoryItem (:37-38), a base of CSE_ALifeItem (:326). CSE_Temporary reads neither, but the rocket's
	// game object does (CInventoryItem::Load, xrGame/inventory_item.cpp:116, :119) and the cost budget needs `cost`.
	std::vector<const char*> lines = {"inv_weight", "cost"};
	switch (cse)
	{
	case CseClass::Weapon:
	case CseClass::WeaponMagazined:
	case CseClass::WeaponMagazinedWGL:
	case CseClass::WeaponShotGun:
	case CseClass::WeaponAutoShotGun:
		// CSE_ALifeItemWeapon (:504-506, :512-514); its descendants (:769, :813, :682, :733) read nothing more.
		lines.insert(lines.end(), {"hit_power", "hit_type", "ammo_class", "scope_status", "silencer_status",
		                              "grenade_launcher_status"});
		break;
	case CseClass::Ammo: lines.push_back("box_size"); // CSE_ALifeItemAmmo (:856)
		break;
	case CseClass::Detector: lines.push_back("ef_detector_type"); // CSE_ALifeItemDetector (:913)
		break;
	case CseClass::CustomOutfit: lines.push_back("ef_equipment_type"); // CSE_ALifeItemCustomOutfit (:1268)
		break;
	// Nothing unchecked in their own constructors: Torch (:433), Artefact (:956), PDA (:1004), Document (:1075),
	// Grenade (:1126, READ_IF_EXISTS), Explosive (:1171), Bolt (:1209, READ_IF_EXISTS), Helmet (:1317),
	// Temporary (xrServer_Objects.cpp:120).
	case CseClass::Item:
	case CseClass::Grenade:
	case CseClass::Explosive:
	case CseClass::Bolt:
	case CseClass::Artefact:
	case CseClass::Torch:
	case CseClass::PDA:
	case CseClass::Document:
	case CseClass::Helmet:
	case CseClass::Temporary: break;
	}
	return lines;
}

// ---------------------------------------------------------------- the game object's lines (Ruling F2-17)
// When a line of the game object's Load is read at all, decided by other lines of the same section. Ini (missing_line)
// answers has(line), number(line) (atoi, as r_s32 / r_u32; 0 if missing) and flag(line) (r_bool; false if missing).
enum class When : uint8_t
{
	Always,
	InSlotOrBelt, // slot is not -1, or belt (inventory_item.cpp:120-121, :136)
	ShotLight, // light_disabled is not true (ShootingObject.cpp:64-68, LoadLights :180)
	// ShotLight, and a permanent or attachable silencer: InitAddons loads the "silencer_" lights
	// (WeaponMagazined.cpp:1567), at Load for a permanent one, at net_Spawn for an attachable one the state attaches.
	SilencerShotLight,
	ShellParticles, // shell_particles exists (ShootingObject.cpp:289)
	FirstBullet, // use_first_bullet (first_bullet_controller.cpp:16-17)
	ScopeModular, // scope_status 2 (attachable) and modular_attachments (Weapon.cpp:766-770)
	// scope_status 2, not modular, no scopes_sect: the weapon is its own scope (Weapon.cpp:790-792) and
	// LoadScopeKoeffs (WeaponMagazined.cpp:1622) reads its scope_name (Weapon.h:333).
	ScopeOwnName,
	SilencerAttachable, // silencer_status 2 (Weapon.cpp:815)
	LauncherAttachable, // grenade_launcher_status 2 (Weapon.cpp:822)
	LauncherPermanent, // grenade_launcher_status 1 (WeaponMagazinedWGrenade.cpp:45)
	ArtefactLights, // lights_enabled (Artefact.cpp:61)
};

struct GameLine
{
	const char* line;
	When when;
	const char* src; // the read, file:line in engine/src/xrGame (tools/check_spawn_classes.py checks each one)
};

// CInventoryItem::Load, every item.
inline constexpr GameLine kInventoryItemLines[] = {
	{"immunities_sect", When::Always, "inventory_item.cpp:96"},
	{"inv_name", When::Always, "inventory_item.cpp:104"},
	{"inv_name_short", When::Always, "inventory_item.cpp:105"},
	{"inv_weight", When::Always, "inventory_item.cpp:116"},
	{"cost", When::Always, "inventory_item.cpp:119"},
	{"slot", When::Always, "inventory_item.cpp:120"},
	{"description", When::Always, "inventory_item.cpp:123"},
	{"default_to_ruck", When::InSlotOrBelt, "inventory_item.cpp:138"},
	{"sprint_allowed", When::InSlotOrBelt, "inventory_item.cpp:139"},
	{"control_inertion_factor", When::InSlotOrBelt, "inventory_item.cpp:140"},
};
// CHitImmunity::LoadImmunities of the section `immunities_sect` names: it must exist (R_ASSERT2, hit_immunity.cpp:23).
inline constexpr GameLine kImmunityLines[] = {
	{"burn_immunity", When::Always, "hit_immunity.cpp:25"},
	{"strike_immunity", When::Always, "hit_immunity.cpp:26"},
	{"shock_immunity", When::Always, "hit_immunity.cpp:27"},
	{"wound_immunity", When::Always, "hit_immunity.cpp:28"},
	{"radiation_immunity", When::Always, "hit_immunity.cpp:29"},
	{"telepatic_immunity", When::Always, "hit_immunity.cpp:30"},
	{"chemical_burn_immunity", When::Always, "hit_immunity.cpp:31"},
	{"explosion_immunity", When::Always, "hit_immunity.cpp:32"},
	{"fire_wound_immunity", When::Always, "hit_immunity.cpp:33"},
};
// CHudItem::Load: weapons, missiles, artefacts, devices, PDA, flare.
inline constexpr GameLine kHudItemLines[] = {
	{"hud", When::Always, "HudItem.cpp:79"},
	{"animation_slot", When::Always, "HudItem.cpp:80"},
};
// CShootingObject::Load, LoadFireParams, LoadLights, LoadShellParticles (prefix "").
inline constexpr GameLine kShootingLines[] = {
	{"rpm", When::Always, "ShootingObject.cpp:71"},
	{"fire_dispersion_base", When::Always, "ShootingObject.cpp:117"},
	{"hit_power", When::Always, "ShootingObject.cpp:120"},
	{"hit_power_critical", When::Always, "ShootingObject.cpp:121"},
	{"hit_impulse", When::Always, "ShootingObject.cpp:163"},
	{"fire_distance", When::Always, "ShootingObject.cpp:165"},
	{"bullet_speed", When::Always, "ShootingObject.cpp:167"},
	{"light_color", When::ShotLight, "ShootingObject.cpp:181"},
	{"light_range", When::ShotLight, "ShootingObject.cpp:183"},
	{"light_var_color", When::ShotLight, "ShootingObject.cpp:185"},
	{"light_var_range", When::ShotLight, "ShootingObject.cpp:187"},
	{"light_time", When::ShotLight, "ShootingObject.cpp:188"},
	{"shell_point", When::ShellParticles, "ShootingObject.cpp:293"},
};
// CWeapon::Load and the first bullet controller it loads.
inline constexpr GameLine kWeaponLines[] = {
	{"ammo_class", When::Always, "Weapon.cpp:586"},
	{"ammo_elapsed", When::Always, "Weapon.cpp:598"},
	{"ammo_mag_size", When::Always, "Weapon.cpp:599"},
	{"cam_relax_speed", When::Always, "Weapon.cpp:612"},
	{"cam_max_angle", When::Always, "Weapon.cpp:633"},
	{"cam_max_angle_horz", When::Always, "Weapon.cpp:641"},
	{"cam_step_angle_horz", When::Always, "Weapon.cpp:649"},
	{"PDM_disp_base", When::Always, "Weapon.cpp:711"},
	{"PDM_disp_vel_factor", When::Always, "Weapon.cpp:712"},
	{"PDM_disp_accel_factor", When::Always, "Weapon.cpp:713"},
	{"PDM_disp_crouch", When::Always, "Weapon.cpp:714"},
	{"PDM_disp_crouch_no_acc", When::Always, "Weapon.cpp:715"},
	{"use_first_bullet", When::Always, "first_bullet_controller.cpp:16"},
	{"first_bullet_dispertion", When::FirstBullet, "first_bullet_controller.cpp:19"},
	{"first_bullet_timeout", When::FirstBullet, "first_bullet_controller.cpp:20"},
	{"first_bullet_velocity_limit", When::FirstBullet, "first_bullet_controller.cpp:21"},
	{"fire_dispersion_condition_factor", When::Always, "Weapon.cpp:719"},
	{"misfire_start_condition", When::Always, "Weapon.cpp:724"},
	{"misfire_end_prob", When::Always, "Weapon.cpp:727"},
	{"condition_shot_dec", When::Always, "Weapon.cpp:728"},
	{"fire_point", When::Always, "Weapon.cpp:732"},
	{"hand_dependence", When::Always, "Weapon.cpp:745"},
	{"min_radius", When::Always, "Weapon.cpp:750"},
	{"max_radius", When::Always, "Weapon.cpp:751"},
	{"scope_status", When::Always, "Weapon.cpp:754"},
	{"silencer_status", When::Always, "Weapon.cpp:755"},
	{"grenade_launcher_status", When::Always, "Weapon.cpp:756"},
	{"zoom_enabled", When::Always, "Weapon.cpp:760"},
	{"zoom_rotate_time", When::Always, "Weapon.cpp:761"},
	{"modular_scope_group", When::ScopeModular, "Weapon.cpp:770"},
	{"silencer_name", When::SilencerAttachable, "Weapon.cpp:817"},
	{"silencer_x", When::SilencerAttachable, "Weapon.cpp:818"},
	{"silencer_y", When::SilencerAttachable, "Weapon.cpp:819"},
	{"grenade_launcher_name", When::LauncherAttachable, "Weapon.cpp:824"},
	{"grenade_launcher_x", When::LauncherAttachable, "Weapon.cpp:825"},
	{"grenade_launcher_y", When::LauncherAttachable, "Weapon.cpp:826"},
};
// CWeaponMagazined::Load (LoadScopeKoeffs) and InitAddons (the silencer's lights, LoadLights with prefix "silencer_").
inline constexpr GameLine kWeaponMagazinedLines[] = {
	{"scope_name", When::ScopeOwnName, "Weapon.h:333"},
	{"silencer_light_color", When::SilencerShotLight, "ShootingObject.cpp:181"},
	{"silencer_light_range", When::SilencerShotLight, "ShootingObject.cpp:183"},
	{"silencer_light_var_color", When::SilencerShotLight, "ShootingObject.cpp:185"},
	{"silencer_light_var_range", When::SilencerShotLight, "ShootingObject.cpp:187"},
	{"silencer_light_time", When::SilencerShotLight, "ShootingObject.cpp:188"},
};
inline constexpr GameLine kRocketLauncherLines[] = {
	{"launch_speed", When::Always, "RocketLauncher.cpp:25"},
};
inline constexpr GameLine kWeaponGrenadeLines[] = {
	{"grenade_flame_particles", When::Always, "WeaponMagazinedWGrenade.cpp:43"},
	{"grenade_vel", When::LauncherPermanent, "WeaponMagazinedWGrenade.cpp:47"},
	{"grenade_class", When::Always, "WeaponMagazinedWGrenade.cpp:53"},
};
inline constexpr GameLine kRPG7Lines[] = {
	{"max_zoom_factor", When::Always, "WeaponRPG7.cpp:23"},
	{"rocket_class", When::Always, "WeaponRPG7.cpp:25"},
};
inline constexpr GameLine kBinocularsLines[] = {
	{"vision_present", When::Always, "WeaponBinoculars.cpp:35"},
};
inline constexpr GameLine kKnifeLines[] = {
	{"wm_size", When::Always, "WeaponKnife.cpp:53"},
	{"splash1_direction", When::Always, "WeaponKnife.cpp:58"},
	{"splash2_direction", When::Always, "WeaponKnife.cpp:59"},
	{"spash1_dist", When::Always, "WeaponKnife.cpp:61"},
	{"spash2_dist", When::Always, "WeaponKnife.cpp:62"},
	{"spash1_radius", When::Always, "WeaponKnife.cpp:64"},
	{"spash2_radius", When::Always, "WeaponKnife.cpp:65"},
	{"splash1_hits_count", When::Always, "WeaponKnife.cpp:67"},
	{"splash1_pervictim_hcount", When::Always, "WeaponKnife.cpp:68"},
	{"splash2_hits_count", When::Always, "WeaponKnife.cpp:69"},
	{"splash_hit_divide_factor", When::Always, "WeaponKnife.cpp:73"},
};
inline constexpr GameLine kAmmoLines[] = {
	{"k_dist", When::Always, "WeaponAmmo.cpp:115"},
	{"k_disp", When::Always, "WeaponAmmo.cpp:116"},
	{"k_hit", When::Always, "WeaponAmmo.cpp:117"},
	{"k_impulse", When::Always, "WeaponAmmo.cpp:119"},
	{"k_ap", When::Always, "WeaponAmmo.cpp:121"},
	{"tracer", When::Always, "WeaponAmmo.cpp:128"},
	{"buck_shot", When::Always, "WeaponAmmo.cpp:134"},
	{"impair", When::Always, "WeaponAmmo.cpp:135"},
	{"wm_size", When::Always, "WeaponAmmo.cpp:136"},
	{"box_size", When::Always, "WeaponAmmo.cpp:139"},
};
inline constexpr GameLine kGrenadeLauncherLines[] = {
	{"grenade_vel", When::Always, "GrenadeLauncher.cpp:27"},
};
// CExplosive::Load(ini, section): grenades, explosive items, rockets.
inline constexpr GameLine kExplosiveLines[] = {
	{"blast", When::Always, "Explosive.cpp:105"},
	{"blast_r", When::Always, "Explosive.cpp:106"},
	{"blast_impulse", When::Always, "Explosive.cpp:107"},
	{"frags", When::Always, "Explosive.cpp:109"},
	{"frags_r", When::Always, "Explosive.cpp:110"},
	{"frag_hit", When::Always, "Explosive.cpp:111"},
	{"frag_hit_impulse", When::Always, "Explosive.cpp:112"},
	{"hit_type_blast", When::Always, "Explosive.cpp:114"},
	{"hit_type_frag", When::Always, "Explosive.cpp:115"},
	{"up_throw_factor", When::Always, "Explosive.cpp:117"},
	{"wm_size", When::Always, "Explosive.cpp:128"},
	{"explode_particles", When::Always, "Explosive.cpp:131"},
	{"light_color", When::Always, "Explosive.cpp:133"},
	{"light_range", When::Always, "Explosive.cpp:134"},
	{"light_time", When::Always, "Explosive.cpp:135"},
	{"fragment_speed", When::Always, "Explosive.cpp:138"},
	{"explode_duration", When::Always, "Explosive.cpp:142"},
};
inline constexpr GameLine kExplosiveItemLines[] = {
	{"time_to_explode", When::Always, "ExplosiveItem.cpp:31"},
	{"condition_to_explode", When::Always, "ExplosiveItem.cpp:32"},
};
inline constexpr GameLine kMissileLines[] = {
	{"force_min", When::Always, "Missile.cpp:84"},
	{"force_const", When::Always, "Missile.cpp:85"},
	{"force_max", When::Always, "Missile.cpp:86"},
	{"force_grow_speed", When::Always, "Missile.cpp:87"},
	{"destroy_time", When::Always, "Missile.cpp:89"},
	{"throw_point", When::Always, "Missile.cpp:91"},
	{"throw_dir", When::Always, "Missile.cpp:92"},
};
inline constexpr GameLine kOutfitLines[] = {
	{"burn_protection", When::Always, "CustomOutfit.cpp:74"},
	{"strike_protection", When::Always, "CustomOutfit.cpp:75"},
	{"shock_protection", When::Always, "CustomOutfit.cpp:76"},
	{"wound_protection", When::Always, "CustomOutfit.cpp:77"},
	{"radiation_protection", When::Always, "CustomOutfit.cpp:78"},
	{"telepatic_protection", When::Always, "CustomOutfit.cpp:79"},
	{"chemical_burn_protection", When::Always, "CustomOutfit.cpp:80"},
	{"explosion_protection", When::Always, "CustomOutfit.cpp:81"},
	{"hit_fraction_actor", When::Always, "CustomOutfit.cpp:86"},
	{"ef_equipment_type", When::Always, "CustomOutfit.cpp:100"},
	{"additional_inventory_weight", When::Always, "CustomOutfit.cpp:104"},
	{"additional_inventory_weight2", When::Always, "CustomOutfit.cpp:105"},
	{"full_icon_name", When::Always, "CustomOutfit.cpp:114"},
};
inline constexpr GameLine kHelmetLines[] = {
	{"burn_protection", When::Always, "ActorHelmet.cpp:34"},
	{"strike_protection", When::Always, "ActorHelmet.cpp:35"},
	{"shock_protection", When::Always, "ActorHelmet.cpp:36"},
	{"wound_protection", When::Always, "ActorHelmet.cpp:37"},
	{"radiation_protection", When::Always, "ActorHelmet.cpp:38"},
	{"telepatic_protection", When::Always, "ActorHelmet.cpp:39"},
	{"chemical_burn_protection", When::Always, "ActorHelmet.cpp:40"},
	{"explosion_protection", When::Always, "ActorHelmet.cpp:41"},
	{"hit_fraction_actor", When::Always, "ActorHelmet.cpp:46"},
};
inline constexpr GameLine kBackpackLines[] = {
	{"additional_inventory_weight", When::Always, "ActorBackpack.cpp:19"},
	{"additional_inventory_weight2", When::Always, "ActorBackpack.cpp:20"},
};
inline constexpr GameLine kTorchLines[] = {
	{"light_trace_bone", When::Always, "Torch.cpp:107"},
};
// CArtefact::Load, and net_Spawn (can_be_controlled).
inline constexpr GameLine kArtefactLines[] = {
	{"lights_enabled", When::Always, "Artefact.cpp:60"},
	{"trail_light_color", When::ArtefactLights, "Artefact.cpp:63"},
	{"trail_light_range", When::ArtefactLights, "Artefact.cpp:65"},
	{"health_restore_speed", When::Always, "Artefact.cpp:69"},
	{"radiation_restore_speed", When::Always, "Artefact.cpp:70"},
	{"satiety_restore_speed", When::Always, "Artefact.cpp:71"},
	{"power_restore_speed", When::Always, "Artefact.cpp:72"},
	{"bleeding_restore_speed", When::Always, "Artefact.cpp:73"},
	{"hit_absorbation_sect", When::Always, "Artefact.cpp:75"},
	{"af_rank", When::Always, "Artefact.cpp:80"},
	{"additional_inventory_weight", When::Always, "Artefact.cpp:81"},
	{"can_be_controlled", When::Always, "Artefact.cpp:86"},
};
inline constexpr GameLine kBastArtefactLines[] = {
	{"impulse_threshold", When::Always, "BastArtifact.cpp:108"},
	{"radius", When::Always, "BastArtifact.cpp:109"},
	{"strike_impulse", When::Always, "BastArtifact.cpp:110"},
	{"energy_max", When::Always, "BastArtifact.cpp:112"},
	{"energy_decrease_speed", When::Always, "BastArtifact.cpp:113"},
	{"particle", When::Always, "BastArtifact.cpp:115"},
};
inline constexpr GameLine kBlackGraviArtefactLines[] = {
	{"impulse_threshold", When::Always, "BlackGraviArtifact.cpp:37"},
	{"radius", When::Always, "BlackGraviArtifact.cpp:38"},
	{"strike_impulse", When::Always, "BlackGraviArtifact.cpp:39"},
	{"particle", When::Always, "BlackGraviArtifact.cpp:40"},
};
inline constexpr GameLine kMercuryBallLines[] = {
	{"time_to_update", When::Always, "MercuryBall.cpp:29"},
	{"impulse_min", When::Always, "MercuryBall.cpp:30"},
	{"impulse_max", When::Always, "MercuryBall.cpp:31"},
};
inline constexpr GameLine kPdaLines[] = {
	{"radius", When::Always, "PDA.cpp:64"},
};
inline constexpr GameLine kFlareLines[] = {
	{"work_time_sec", When::Always, "flare.cpp:9"},
};
inline constexpr GameLine kCustomDeviceLines[] = {
	{"hud", When::Always, "CustomDevice.cpp:319"},
};
inline constexpr GameLine kCustomDetectorLines[] = {
	{"af_radius", When::Always, "CustomDetector.cpp:39"},
	{"af_vis_radius", When::Always, "CustomDetector.cpp:40"},
};

struct LineBlock
{
	const GameLine* lines = nullptr;
	size_t count = 0;
};
template <size_t N>
constexpr LineBlock block(const GameLine (&lines)[N])
{
	return {lines, N};
}

// The blocks of each group, in Load order. `loads`: every class with its own Load(LPCSTR) above the group's client
// classes (up to CGameObject), so the blocks cover all of them; tools/check_spawn_classes.py walks each client class's
// bases in xrGame and fails on a Load that is not listed. A listed Load with no block reads nothing unchecked.
struct GameGroup
{
	GameClass game;
	const char* loads;
	LineBlock blocks[7];
};
inline constexpr GameGroup kGameGroups[] = {
	{GameClass::InventoryItem,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CSilencer CInfoDocument"
	    " CEatableItemObject CEatableItem CBottleItem",
	    {block(kInventoryItemLines)}},
	{GameClass::GrenadeLauncher,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CGrenadeLauncher",
	    {block(kInventoryItemLines), block(kGrenadeLauncherLines)}},
	{GameClass::Ammo,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CWeaponAmmo",
	    {block(kInventoryItemLines), block(kAmmoLines)}},
	{GameClass::CustomOutfit,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CCustomOutfit",
	    {block(kInventoryItemLines), block(kOutfitLines)}},
	{GameClass::Helmet,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHelmet",
	    {block(kInventoryItemLines), block(kHelmetLines)}},
	{GameClass::Backpack,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CBackpack",
	    {block(kInventoryItemLines), block(kBackpackLines)}},
	{GameClass::Torch,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CTorch",
	    {block(kInventoryItemLines), block(kTorchLines)}},
	{GameClass::ExplosiveItem,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CExplosiveItem CExplosive",
	    {block(kInventoryItemLines), block(kExplosiveLines), block(kExplosiveItemLines)}},
	{GameClass::Missile,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CMissile",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kMissileLines)}},
	{GameClass::Grenade,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CMissile"
	    " CGrenade CExplosive",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kMissileLines), block(kExplosiveLines)}},
	{GameClass::ExplosiveRocket,
	    "CExplosiveRocket CCustomRocket CPhysicItem CGameObject CScriptBinder CInventoryItem CExplosive",
	    {block(kInventoryItemLines), block(kExplosiveLines)}},
	{GameClass::Artefact,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CArtefact"
	    " CBlackDrops CDummyArtefact CElectricBall CFadedBall CGalantineArtefact CRustyHairArtefact CThornArtefact"
	    " CZudaArtefact CGraviArtefact",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kArtefactLines)}},
	{GameClass::BastArtefact,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CArtefact"
	    " CBastArtefact",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kArtefactLines), block(kBastArtefactLines)}},
	{GameClass::BlackGraviArtefact,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CArtefact"
	    " CGraviArtefact CBlackGraviArtefact",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kArtefactLines), block(kBlackGraviArtefactLines)}},
	{GameClass::MercuryBall,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CArtefact"
	    " CMercuryBall",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kArtefactLines), block(kMercuryBallLines)}},
	{GameClass::Pda,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CPda",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kPdaLines)}},
	{GameClass::Flare,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CFlare",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kFlareLines)}},
	{GameClass::CustomDevice,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem"
	    " CCustomDevice CFlashlight",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kCustomDeviceLines)}},
	{GameClass::CustomDetector,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem"
	    " CCustomDevice CCustomDetector CScientificDetector",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kCustomDeviceLines), block(kCustomDetectorLines)}},
	{GameClass::WeaponKnife,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CWeapon"
	    " CShootingObject CWeaponKnife",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kShootingLines), block(kWeaponLines),
	        block(kKnifeLines)}},
	{GameClass::WeaponMagazined,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CWeapon"
	    " CShootingObject CWeaponMagazined CWeaponPistol CWeaponShotgun CWeaponBM16 CWeaponAutomaticShotgun",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kShootingLines), block(kWeaponLines),
	        block(kWeaponMagazinedLines)}},
	{GameClass::WeaponBinoculars,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CWeapon"
	    " CShootingObject CWeaponMagazined CWeaponPistol CWeaponShotgun CWeaponBM16 CWeaponAutomaticShotgun"
	    " CWeaponBinoculars",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kShootingLines), block(kWeaponLines),
	        block(kWeaponMagazinedLines), block(kBinocularsLines)}},
	{GameClass::WeaponMagazinedWGrenade,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CWeapon"
	    " CShootingObject CWeaponMagazined CWeaponPistol CWeaponShotgun CWeaponBM16 CWeaponAutomaticShotgun"
	    " CWeaponMagazinedWGrenade CRocketLauncher",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kShootingLines), block(kWeaponLines),
	        block(kWeaponMagazinedLines), block(kRocketLauncherLines), block(kWeaponGrenadeLines)}},
	{GameClass::WeaponRocket,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CWeapon"
	    " CShootingObject CWeaponMagazined CWeaponPistol CWeaponShotgun CWeaponBM16 CWeaponAutomaticShotgun"
	    " CRocketLauncher CWeaponRG6 CWeaponSSRS",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kShootingLines), block(kWeaponLines),
	        block(kWeaponMagazinedLines), block(kRocketLauncherLines)}},
	{GameClass::WeaponRPG7,
	    "CInventoryItemObject CInventoryItem CPhysicItem CGameObject CScriptBinder CHudItemObject CHudItem CWeapon"
	    " CShootingObject CWeaponMagazined CWeaponPistol CWeaponShotgun CWeaponBM16 CWeaponAutomaticShotgun"
	    " CRocketLauncher CWeaponRPG7",
	    {block(kInventoryItemLines), block(kHudItemLines), block(kShootingLines), block(kWeaponLines),
	        block(kWeaponMagazinedLines), block(kRocketLauncherLines), block(kRPG7Lines)}},
};

// The lines of a group's game object, in Load order.
inline std::vector<GameLine> game_lines(GameClass game)
{
	std::vector<GameLine> out;
	for (const GameGroup& group : kGameGroups)
		if (group.game == game)
			for (const LineBlock& b : group.blocks)
				out.insert(out.end(), b.lines, b.lines + b.count);
	return out;
}

template <class Ini>
bool holds(When when, const Ini& ini)
{
	switch (when)
	{
	case When::Always: return true;
	case When::InSlotOrBelt: return ini.number("slot") != -1 || ini.flag("belt");
	case When::ShotLight: return !ini.flag("light_disabled");
	case When::SilencerShotLight:
	{
		const long long silencer = ini.number("silencer_status");
		return !ini.flag("light_disabled") && (silencer == 1 || silencer == 2);
	}
	case When::ShellParticles: return ini.has("shell_particles");
	case When::FirstBullet: return ini.flag("use_first_bullet");
	case When::ScopeModular: return ini.number("scope_status") == 2 && ini.flag("modular_attachments");
	case When::ScopeOwnName:
		return ini.number("scope_status") == 2 && !ini.flag("modular_attachments") && !ini.has("scopes_sect");
	case When::SilencerAttachable: return ini.number("silencer_status") == 2;
	case When::LauncherAttachable: return ini.number("grenade_launcher_status") == 2;
	case When::LauncherPermanent: return ini.number("grenade_launcher_status") == 1;
	case When::ArtefactLights: return ini.flag("lights_enabled");
	}
	return true;
}

// The first line a section lacks for a client-asked entity of `rule`'s class.
struct Missing
{
	const char* line = nullptr; // nullptr: nothing is missing
	const char* in = nullptr; // `line` is missing in the section this line of the item names (immunities_sect)
	bool no_section = false; // `line` names a section that does not exist
};

// Ini: has(line), number(line), flag(line) of the item's section (see When), ref_exists(line) (the line names an
// existing section) and ref_has(line, other) (that section has `other`). The server entity's lines first, then the
// game object's unconditional ones (the conditions read some of them), its conditional ones, its immunities section.
template <class Ini>
Missing missing_line(const ClassRule& rule, const Ini& ini)
{
	Missing m;
	for (const char* line : required_lines(rule.cse))
		if (!ini.has(line))
		{
			m.line = line;
			return m;
		}
	const std::vector<GameLine> lines = game_lines(rule.game);
	for (const bool always : {true, false})
		for (const GameLine& g : lines)
			if ((g.when == When::Always) == always && !ini.has(g.line) && holds(g.when, ini))
			{
				m.line = g.line;
				return m;
			}
	if (!ini.ref_exists("immunities_sect"))
	{
		m.line = "immunities_sect";
		m.no_section = true;
		return m;
	}
	for (const GameLine& g : kImmunityLines)
		if (!ini.ref_has("immunities_sect", g.line))
		{
			m.line = g.line;
			m.in = "immunities_sect";
			return m;
		}
	return m;
}

// What the host knows of the asked section: `quest_item` (as CInventoryItem::Load reads it), `cost`, and whether the
// host's configs\coop_spawn.ltx lists it in [allow_sections] / [deny_sections]; and of the request: a quest gift
// (quest_gift) lets a quest item through, a parent item that does not take the item (child_allowed) refuses it, and
// requests of the character waiting in the queue put this one behind them.
struct SectionFacts
{
	bool quest_item = false;
	uint32_t cost = 0;
	bool allow_listed = false;
	bool deny_listed = false;
	bool in_dialog_to_own_actor = false; // Ruling F2-16
	bool parent_ok = true; // review round 2, Important 1
	bool queue_waiting = false; // Ruling F2-21
	bool launch_child = false; // Ruling F2-22: a child its parent item takes (child_allowed): a shot
};

// What the budget does not charge (and the queue does not hold): an allow-listed section, and a child its parent item
// takes (Ruling F2-22: a rocket in its launcher, a fake missile in its grenade or bolt - a shot, the charge already
// left the client's inventory; waiting would refuse the shot; task 3's rate limits bound how often).
inline bool budget_free(const SectionFacts& facts)
{
	return facts.allow_listed || facts.launch_child;
}

// coop_dialog kTalkReach: a client talks only to an NPC his character stands by on the host (host_try_lock).
constexpr float kTalkReach = 6.f;

// Ruling F2-16: the stock dialogs create a quest reward on the client (alife_create_item(section, db.actor), the talk
// runs there). A quest item is a gift only into the asking player's own character (`parent` is his proxy on the host)
// while he holds a talk lock with an NPC (coop_dialog host_lock_of) and his character stands within talk reach of that
// NPC (round 2: as host_try_lock; `npc_distance` 0 for an NPC that talks from afar, NeedOsoznanieMode).
inline bool quest_gift(bool holds_talk_lock, float npc_distance, uint16_t parent, uint16_t own_character)
{
	return holds_talk_lock && npc_distance <= kTalkReach && own_character != 0xffff && parent == own_character;
}

// Review round 2, Important 1: what may be created inside one of the player's items (the parent is not his character).
// The host gives the parent the new entity with GE_OWNERSHIP_TAKE, and the items whose handler takes a child cast it
// unchecked: CRocketLauncher::AttachRocket (RocketLauncher.cpp:56-57) to CCustomRocket, CMissile::OnEvent
// (Missile.cpp:606-608) to CMissile. So only a rocket into a weapon with a rocket launcher; nothing else into an item.
// Round 5 (Ruling F2-23):
// - a launcher takes its rocket whatever it holds: the host itself loads a rocket into its copy of a loaded launcher
//   (CWeaponRPG7::net_Spawn, CWeaponRG6::net_Spawn a rocket per round, the grenade launcher), so round 4's "room"
//   refused every honest shot after a join or a load. A rocket is a CSE_Temporary (not saved, cheap), and the
//   ReqSpawn rate limit bounds a flood;
// - a grenade or bolt takes no child from a client: a fair client never asks for a fake missile (coop_objects.cpp
//   skip_fake_missile = is_client()), and a fake missile is a whole ALife grenade that ReqDrop takes out (free grenades).
inline bool child_allowed(GameClass parent, std::string_view /*parent_section*/, GameClass child,
    std::string_view /*child_section*/, size_t /*children*/, long long /*mag_size*/)
{
	switch (parent)
	{
	case GameClass::WeaponMagazinedWGrenade:
	case GameClass::WeaponRocket:
	case GameClass::WeaponRPG7: return child == GameClass::ExplosiveRocket;
	default: return false;
	}
}

// Round 4 minor: what may be created straight into the player's character: any item but a launcher's rocket
// (CSE_Temporary): a fair client asks for one only as its launcher's child.
inline bool character_takes(GameClass child)
{
	return child != GameClass::ExplosiveRocket;
}

// Cost budget (host settings, configs\coop_spawn.ltx [coop_spawn]): the sum of `cost` a character may have created
// in a row, and how much comes back per second.
struct SpawnPolicy
{
	uint32_t cost_burst = 200000;
	uint32_t cost_per_sec = 2000;
};

// One character's budget. It starts full at the character's first counted request.
struct CostBudget
{
	double tokens = 0;
	uint64_t at = 0; // ms of the last refill
	bool started = false;
};

// Ruling F2-21: what an item takes from the budget: its cost, at most the whole burst (an item dearer than the burst
// waits for a full budget instead of being refused for ever).
inline uint32_t charge_of(uint32_t cost, const SpawnPolicy& policy)
{
	return cost < policy.cost_burst ? cost : policy.cost_burst;
}

// Refills `budget` up to now (full at its first use; a clock that went back refills nothing), then takes the item's
// charge if it holds it. False: not enough, nothing taken.
inline bool charge(CostBudget& budget, uint32_t cost, const SpawnPolicy& policy, uint64_t now_ms)
{
	const double burst = double(policy.cost_burst);
	if (!budget.started)
	{
		budget.started = true;
		budget.tokens = burst;
		budget.at = now_ms;
	}
	else if (now_ms > budget.at)
	{
		budget.tokens += double(now_ms - budget.at) * double(policy.cost_per_sec) / 1000.0;
		budget.at = now_ms;
	}
	if (budget.tokens > burst)
		budget.tokens = burst;
	const double amount = double(charge_of(cost, policy));
	if (amount > budget.tokens)
		return false;
	budget.tokens -= amount;
	return true;
}

// Ruling F2-21 (3): the charge of an item the engine then refused goes back (up to the burst).
inline void refund(CostBudget& budget, uint32_t cost, const SpawnPolicy& policy)
{
	budget.tokens += double(charge_of(cost, policy));
	if (budget.tokens > double(policy.cost_burst))
		budget.tokens = double(policy.cost_burst);
}

enum class SpawnVerdict : uint8_t
{
	Allow,
	UnknownClass, // the section's class has no rule
	MissingLine, // a line the class's constructor reads unchecked is missing
	Denied, // [deny_sections]
	QuestItem, // quest_item = true
	OverBudget, // the host queues it (SpawnQueue); refused only when the queue is full
	WrongParent, // an item into one of his items that does not take it (child_allowed)
};

// The verdict on a client's ReqSpawn, in this order:
//   1. no rule (not an item class)            -> UnknownClass
//   2. a required line is missing (`missing`) -> MissingLine
//   3. into one of his items that cannot take it (parent_ok false) -> WrongParent
//   4. deny-listed                            -> Denied
//   5. allow-listed                           -> Allow, the budget untouched
//   6. a quest item, unless a quest gift      -> QuestItem
//   7. a child its parent item takes (launch_child, Ruling F2-22) -> Allow, the budget untouched, no queue
//   8. requests of the character wait in the queue, or the budget does not hold charge_of(cost)
//                                             -> OverBudget (nothing taken); else Allow and the charge is taken.
// Only step 8 reads or changes `budget` (budget_free: steps 5 and 7 do not); a quest gift goes on to it like any item.
inline SpawnVerdict judge(const ClassRule* rule, const char* missing_line, const SectionFacts& facts,
    CostBudget& budget, const SpawnPolicy& policy, uint64_t now_ms)
{
	if (!rule)
		return SpawnVerdict::UnknownClass;
	if (missing_line)
		return SpawnVerdict::MissingLine;
	if (!facts.parent_ok)
		return SpawnVerdict::WrongParent;
	if (facts.deny_listed)
		return SpawnVerdict::Denied;
	if (facts.allow_listed)
		return SpawnVerdict::Allow;
	if (facts.quest_item && !facts.in_dialog_to_own_actor)
		return SpawnVerdict::QuestItem;
	if (facts.launch_child)
		return SpawnVerdict::Allow;
	if (facts.queue_waiting || !charge(budget, facts.cost, policy, now_ms))
		return SpawnVerdict::OverBudget;
	return SpawnVerdict::Allow;
}

// Cost budgets per character for the whole session (Ruling F2-11): a player who leaves and joins again finds his
// budget as he left it, whatever his new connection is; only a session start or end clears the book. The key is the
// character's player key, as the money book's (ledger::character_key, Ruling F2-18): loading an older save gives the
// same player's character another proxy id. A key the book has not seen yet starts with the full burst (the number of
// new characters per session is a host setting of its own).
class BudgetBook
{
public:
	CostBudget& of(const std::string& character_key) { return m_budgets[character_key]; }
	void clear() { m_budgets.clear(); }
	size_t size() const { return m_budgets.size(); }

private:
	std::map<std::string, CostBudget> m_budgets;
};

// Ruling F2-21 (2): a request over its character's budget is not refused: it waits on the host, first in first out
// per character (budget key), and is created when the budget has refilled for it. Otherwise a fair mass disassembly
// or craft would lose items: the inputs are already spent, the outputs refused. At most kMaxQueued wait per
// character; more is refused.
constexpr size_t kMaxQueued = 256;

// What the queue keeps of a request: data only, never a built entity (review round 4, Important 2: the server entity of
// a script class holds Lua references, and a save load closes the Lua state under it). The host builds and checks the
// entity again when its turn comes (judge_queued, child_allowed, the world epoch).
struct QueuedRequest
{
	uint32_t seq = 0;
	uint32_t peer = 0; // PeerId of the asker
	uint16_t character = 0xffff; // his proxy id in the host world
	uint16_t parent = 0xffff;
	float pos[3] = {0.f, 0.f, 0.f};
	std::string section;
	std::vector<uint8_t> state; // its own copy of the item state blob (coop_items), checked again at its turn
	uint32_t epoch = 0; // the host world it was asked in
	bool quest_gift = false; // Ruling F2-16, decided when asked: the talk may be over by its turn
	uint16_t id = kNoId; // F2 T12b: leased id the client created it under (it stays leased while it waits)
	uint16_t source = kNoId; // F2 T12b: the weapon a clone replaces
};

inline QueuedRequest queued_request(uint32_t seq, uint32_t peer, uint16_t character, uint16_t parent,
    const float pos[3], std::string_view section, const uint8_t* state, size_t state_size, uint32_t epoch,
    bool quest_gift, uint16_t id = kNoId, uint16_t source = kNoId)
{
	QueuedRequest r;
	r.seq = seq;
	r.peer = peer;
	r.character = character;
	r.parent = parent;
	for (int i = 0; i < 3; ++i)
		r.pos[i] = pos[i];
	r.section.assign(section.data(), section.size());
	if (state && state_size)
		r.state.assign(state, state + state_size);
	r.epoch = epoch;
	r.quest_gift = quest_gift;
	r.id = id;
	r.source = source;
	return r;
}

// The verdict at a waiting request's turn: judge with what the host knows now (`facts` from its configs, the parent
// checked again), the quest gift decided when it was asked, and no queue ahead of it (it is the front).
inline SpawnVerdict judge_queued(const ClassRule* rule, const char* missing_line, SectionFacts facts,
    const QueuedRequest& request, CostBudget& budget, const SpawnPolicy& policy, uint64_t now_ms)
{
	facts.in_dialog_to_own_actor = request.quest_gift;
	facts.queue_waiting = false;
	return judge(rule, missing_line, facts, budget, policy, now_ms);
}

// What became of the front request at its turn.
enum class Turn : uint8_t
{
	Created,
	Refused, // answered with the refusal; whatever was charged went back
	Wait, // the budget does not hold it yet: it stays in front
};

template <class Item>
class SpawnQueue
{
public:
	bool waiting(const std::string& key) const
	{
		const auto it = m_queues.find(key);
		return it != m_queues.end() && !it->second.empty();
	}
	size_t size(const std::string& key) const
	{
		const auto it = m_queues.find(key);
		return it == m_queues.end() ? 0 : it->second.size();
	}
	size_t total() const
	{
		size_t n = 0;
		for (const auto& it : m_queues)
			n += it.second.size();
		return n;
	}
	std::vector<std::string> keys() const
	{
		std::vector<std::string> out;
		for (const auto& it : m_queues)
			if (!it.second.empty())
				out.push_back(it.first);
		return out;
	}
	// False: the character's queue is full (the item stays the caller's).
	bool push(const std::string& key, Item item)
	{
		std::deque<Item>& q = m_queues[key];
		if (q.size() >= kMaxQueued)
			return false;
		q.push_back(std::move(item));
		return true;
	}
	// Gives the requests their turns in order (`turn(item)`: judged again, charged, created, see Turn) until one has to
	// wait. Returns the number created.
	template <class TurnOf>
	size_t pump(const std::string& key, TurnOf&& turn)
	{
		const auto it = m_queues.find(key);
		if (it == m_queues.end())
			return 0;
		size_t created = 0;
		std::deque<Item>& q = it->second;
		while (!q.empty())
		{
			const Turn t = turn(q.front());
			if (t == Turn::Wait)
				break;
			created += t == Turn::Created ? 1 : 0;
			q.pop_front();
		}
		if (q.empty())
			m_queues.erase(it);
		return created;
	}
	// The character left (or the world was replaced): `drop(item)` for each waiting one, nothing of it is created.
	template <class Drop>
	size_t drop(const std::string& key, Drop&& drop)
	{
		return drop_if(key, [](const Item&) { return true; }, drop);
	}
	template <class Pred, class Drop>
	size_t drop_if(const std::string& key, Pred&& pred, Drop&& drop)
	{
		const auto it = m_queues.find(key);
		if (it == m_queues.end())
			return 0;
		size_t n = 0;
		std::deque<Item> keep;
		for (Item& item : it->second)
		{
			if (pred(static_cast<const Item&>(item)))
			{
				drop(item);
				++n;
			}
			else
				keep.push_back(std::move(item));
		}
		it->second.swap(keep);
		if (it->second.empty())
			m_queues.erase(it);
		return n;
	}
	template <class Drop>
	void clear(Drop&& drop)
	{
		for (auto& it : m_queues)
			for (Item& item : it.second)
				drop(item);
		m_queues.clear();
	}

private:
	std::map<std::string, std::deque<Item>> m_queues;
};
} // namespace spawn
} // namespace coop
