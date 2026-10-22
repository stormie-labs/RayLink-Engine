// Anomaly Coop — a client's ReqSpawn on the wire, what the host checks before it constructs the entity (R3 В6) and
// what a client may ask for at all: engine item classes, no quest items, a cost budget per character (F2 T2).
// The host never reads a peer's raw M_SPAWN bytes (Spawn_Read trusts every byte): it builds the entity from its own
// section, after checking that the section has the lines the entity's constructor reads unchecked.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include "coop_wire.h"

#include <cstddef>
#include <cstdint>
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

struct ClassRule
{
	std::string_view clsid; // the `class` value of a section, as TEXT2CLSID reads it (case-sensitive, <= 8 chars)
	CseClass cse;
};

// Every item class of the game, one rule per clsid. Sources (tools/check_spawn_classes.py checks this table against
// them and fails on any difference, the cited lines included):
//   engine :N  engine/src/xrServerEntities/object_factory_register.cpp line N, an ADD(..., CSE_ALifeItem* or
//              CSE_Temporary, id, ...) before `if (!g_dedicated_server) return;` (the ADDs after it exist only on a
//              dedicated server); id from clsid_game.h or TEXT2CLSID;
//   <script> :N  line N of a class registrator script (configs\script.ltx [common] class_registrators with its DLTX
//              mods): a cs_register whose Lua server class derives from a cse_alife_item* class (se_item.script,
//              se_artefact.script).
inline constexpr ClassRule kClassRules[] = {
	// engine: artefacts
	{"AF_MBALL", CseClass::Artefact}, // engine :287
	{"AF_BDROP", CseClass::Artefact}, // engine :288
	{"AF_NEEDL", CseClass::Artefact}, // engine :289
	{"AF_BAST", CseClass::Artefact}, // engine :290
	{"AF_BGRAV", CseClass::Artefact}, // engine :291
	{"AF_DUMMY", CseClass::Artefact}, // engine :292
	{"AF_ZUDA", CseClass::Artefact}, // engine :293
	{"AF_THORN", CseClass::Artefact}, // engine :294
	{"AF_FBALL", CseClass::Artefact}, // engine :295
	{"AF_EBALL", CseClass::Artefact}, // engine :296
	{"AF_RHAIR", CseClass::Artefact}, // engine :297
	{"AF_GALAN", CseClass::Artefact}, // engine :298
	{"AF_GRAVI", CseClass::Artefact}, // engine :299
	{"ARTEFACT", CseClass::Artefact}, // engine :300
	{"AF_CTA", CseClass::Artefact}, // engine :301
	// engine: weapons
	{"W_WMAGAZ", CseClass::WeaponMagazined}, // engine :304
	{"W_WMAGGL", CseClass::WeaponMagazinedWGL}, // engine :307
	{"W_FN2000", CseClass::WeaponMagazinedWGL}, // engine :309
	{"W_AK74", CseClass::WeaponMagazinedWGL}, // engine :310
	{"W_LR300", CseClass::WeaponMagazined}, // engine :311
	{"W_HPSA", CseClass::WeaponMagazined}, // engine :312
	{"W_PM", CseClass::WeaponMagazined}, // engine :313
	{"W_FORT", CseClass::WeaponMagazined}, // engine :314
	{"W_BINOC", CseClass::WeaponMagazined}, // engine :315
	{"W_SHOTGN", CseClass::WeaponShotGun}, // engine :316
	{"W_SVD", CseClass::WeaponMagazined}, // engine :317
	{"W_SVU", CseClass::WeaponMagazined}, // engine :318
	{"W_RPG7", CseClass::WeaponMagazined}, // engine :319
	{"W_VAL", CseClass::WeaponMagazined}, // engine :320
	{"W_VINT", CseClass::WeaponMagazined}, // engine :321
	{"W_WALTHR", CseClass::WeaponMagazined}, // engine :322
	{"W_USP45", CseClass::WeaponMagazined}, // engine :323
	{"W_GROZA", CseClass::WeaponMagazinedWGL}, // engine :324
	{"W_KNIFE", CseClass::Weapon}, // engine :325
	{"W_BM16", CseClass::WeaponShotGun}, // engine :326
	{"W_RG6", CseClass::WeaponShotGun}, // engine :327
	{"_W_SSRS", CseClass::WeaponMagazined}, // engine :431
	// engine: ammo, addons
	{"AMMO", CseClass::Ammo}, // engine :329
	{"A_VOG25", CseClass::Ammo}, // engine :330
	{"A_OG7B", CseClass::Ammo}, // engine :331
	{"A_M209", CseClass::Ammo}, // engine :332
	{"W_SCOPE", CseClass::Item}, // engine :336
	{"W_SILENC", CseClass::Item}, // engine :337
	{"W_GLAUNC", CseClass::Item}, // engine :338
	// engine: inventory
	{"II_BOLT", CseClass::Bolt}, // engine :342
	{"II_MEDKI", CseClass::Item}, // engine :344
	{"II_BANDG", CseClass::Item}, // engine :345
	{"II_ANTIR", CseClass::Item}, // engine :346
	{"II_FOOD", CseClass::Item}, // engine :347
	{"II_BOTTL", CseClass::Item}, // engine :348
	{"II_EXPLO", CseClass::Explosive}, // engine :349
	{"II_DOC", CseClass::Document}, // engine :352
	{"II_ATTCH", CseClass::Item}, // engine :353
	// engine: equipment
	{"EQU_SCIE", CseClass::CustomOutfit}, // engine :356
	{"EQU_STLK", CseClass::CustomOutfit}, // engine :357
	{"EQU_MLTR", CseClass::CustomOutfit}, // engine :358
	{"EQU_EXO", CseClass::CustomOutfit}, // engine :359
	{"EQ_HLMET", CseClass::Item}, // engine :360
	{"EQ_BAKPK", CseClass::Item}, // engine :361
	// engine: grenades, rockets
	{"G_F1", CseClass::Grenade}, // engine :364
	{"G_RGD5", CseClass::Grenade}, // engine :365
	{"G_RPG7", CseClass::Temporary}, // engine :368
	{"G_FAKE", CseClass::Temporary}, // engine :369
	{"MP_PLBAG", CseClass::Item}, // engine :372
	// engine: detectors, devices
	{"D_SIMDET", CseClass::Detector}, // engine :397
	{"D_ADVANC", CseClass::Detector}, // engine :398
	{"D_ELITE", CseClass::Detector}, // engine :399
	{"D_SCIENT", CseClass::Detector}, // engine :400
	{"D_TORCH", CseClass::Torch}, // engine :403
	{"D_PDA", CseClass::PDA}, // engine :404
	{"D_FLARE", CseClass::Item}, // engine :405
	{"D_FLALIT", CseClass::Item}, // engine :406
	{"D_DSMETR", CseClass::Item}, // engine :407
	{"D_CUSTOM", CseClass::Item}, // engine :408
	// script (class_registrator.script)
	{"SCRPTART", CseClass::Artefact}, // class_registrator :36
	{"TORCH_S", CseClass::Torch}, // class_registrator :59
	{"DET_SCIE", CseClass::Detector}, // class_registrator :62
	{"DET_ELIT", CseClass::Detector}, // class_registrator :63
	{"DET_ADVA", CseClass::Detector}, // class_registrator :64
	{"DET_SIMP", CseClass::Detector}, // class_registrator :65
	{"S_PDA", CseClass::PDA}, // class_registrator :66
	{"E_STLK", CseClass::CustomOutfit}, // class_registrator :72
	{"E_HLMET", CseClass::Helmet}, // class_registrator :73
	{"WP_BINOC", CseClass::WeaponMagazined}, // class_registrator :76
	{"WP_KNIFE", CseClass::Weapon}, // class_registrator :77
	{"WP_BM16", CseClass::WeaponShotGun}, // class_registrator :78
	{"WP_GROZA", CseClass::WeaponMagazinedWGL}, // class_registrator :79
	{"WP_SVD", CseClass::WeaponMagazined}, // class_registrator :80
	{"WP_AK74", CseClass::WeaponMagazinedWGL}, // class_registrator :81
	{"WP_LR300", CseClass::WeaponMagazined}, // class_registrator :82
	{"WP_HPSA", CseClass::WeaponMagazined}, // class_registrator :83
	{"WP_PM", CseClass::WeaponMagazined}, // class_registrator :84
	{"WP_RG6", CseClass::WeaponShotGun}, // class_registrator :85
	{"WP_RPG7", CseClass::WeaponMagazined}, // class_registrator :86
	{"WP_SHOTG", CseClass::WeaponShotGun}, // class_registrator :87
	{"WP_ASHTG", CseClass::WeaponAutoShotGun}, // class_registrator :88
	{"WP_SVU", CseClass::WeaponMagazined}, // class_registrator :90
	{"WP_USP45", CseClass::WeaponMagazined}, // class_registrator :91
	{"WP_VAL", CseClass::WeaponMagazined}, // class_registrator :92
	{"WP_VINT", CseClass::WeaponMagazined}, // class_registrator :93
	{"WP_WALTH", CseClass::WeaponMagazined}, // class_registrator :94
	{"AMMO_S", CseClass::Ammo}, // class_registrator :108
	{"S_VOG25", CseClass::Ammo}, // class_registrator :109
	{"S_OG7B", CseClass::Ammo}, // class_registrator :110
	{"S_M209", CseClass::Ammo}, // class_registrator :111
	{"G_F1_S", CseClass::Grenade}, // class_registrator :114
	{"G_RGD5_S", CseClass::Grenade}, // class_registrator :115
	{"S_FOOD", CseClass::Item}, // class_registrator :118
	{"WP_SCOPE", CseClass::Item}, // class_registrator :121
	{"WP_SILEN", CseClass::Item}, // class_registrator :122
	{"WP_GLAUN", CseClass::Item}, // class_registrator :123
	{"S_EXPLO", CseClass::Explosive}, // class_registrator :129
	// script (engine/gamedata/scripts/class_registrator_modded_exes.script, added by mod_script_dxml.ltx)
	{"_WP_SSRS", CseClass::WeaponMagazined}, // class_registrator_modded_exes :17
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

// What the host knows of the asked section: `quest_item` (as CInventoryItem::Load reads it), `cost`, and whether the
// host's configs\coop_spawn.ltx lists it in [allow_sections] / [deny_sections].
struct SectionFacts
{
	bool quest_item = false;
	uint32_t cost = 0;
	bool allow_listed = false;
	bool deny_listed = false;
};

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

enum class SpawnVerdict : uint8_t
{
	Allow,
	UnknownClass, // the section's class has no rule
	MissingLine, // a line the class's constructor reads unchecked is missing
	Denied, // [deny_sections]
	QuestItem, // quest_item = true
	OverBudget,
};

// The verdict on a client's ReqSpawn, in this order:
//   1. no rule (not an item class)            -> UnknownClass
//   2. a required line is missing (`missing`) -> MissingLine
//   3. deny-listed                            -> Denied
//   4. allow-listed                           -> Allow, the budget untouched
//   5. a quest item                           -> QuestItem
//   6. `cost` over what the budget holds      -> OverBudget (nothing taken); else Allow and the cost is taken.
// Only step 6 reads or changes `budget`.
inline SpawnVerdict judge(const ClassRule* rule, const char* missing_line, const SectionFacts& facts,
    CostBudget& budget, const SpawnPolicy& policy, uint64_t now_ms)
{
	if (!rule)
		return SpawnVerdict::UnknownClass;
	if (missing_line)
		return SpawnVerdict::MissingLine;
	if (facts.deny_listed)
		return SpawnVerdict::Denied;
	if (facts.allow_listed)
		return SpawnVerdict::Allow;
	if (facts.quest_item)
		return SpawnVerdict::QuestItem;
	const double burst = double(policy.cost_burst);
	if (!budget.started)
	{
		budget.started = true;
		budget.tokens = burst;
		budget.at = now_ms;
	}
	else if (now_ms > budget.at) // a clock that went back refills nothing
	{
		budget.tokens += double(now_ms - budget.at) * double(policy.cost_per_sec) / 1000.0;
		budget.at = now_ms;
	}
	if (budget.tokens > burst)
		budget.tokens = burst;
	if (double(facts.cost) > budget.tokens)
		return SpawnVerdict::OverBudget;
	budget.tokens -= double(facts.cost);
	return SpawnVerdict::Allow;
}

// Cost budgets per character (the player's proxy id in the host world) for the whole session (Ruling F2-11): a
// player who leaves and joins again finds his budget as he left it, whatever his new connection is; only a session
// start or end clears the book. A character the book has not seen yet starts with the full burst (the number of new
// characters per session is a host setting of its own).
class BudgetBook
{
public:
	CostBudget& of(uint16_t character) { return m_budgets[character]; }
	void clear() { m_budgets.clear(); }
	size_t size() const { return m_budgets.size(); }

private:
	std::map<uint16_t, CostBudget> m_budgets;
};
} // namespace spawn
} // namespace coop
