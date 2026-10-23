// Anomaly Coop - call router table (MOD_LAYER.md §2.3, F2 T12). Every engine export a Lua script can reach in the
// sources below has a class that decides what a call does on a client:
//   world - changes the world everybody shares (spawn, release, NPCs and their AI, story, relations, NPC money, server
//           objects, cars and helicopters, time). The host runs it; a client sends it to the host where the engine has
//           a request path (ReqSpawn, story, ReqTransfer, the money journal, TimeRequest) and refuses it otherwise.
//   owner - changes what one player owns: the own actor (health, needs, boosters, slots, movement) and own items. Runs
//           on the owner's machine; on another's object it is a world call.
//   view  - picture, sound and UI of this machine (HUD, effectors, particles, map spots, the look of the current
//           weather, script plumbing). Runs locally.
//   read  - reads (positions, alife():object, time, info portions) and script-local values. Runs locally: on a client
//           against the mirror.
// A name is "<namespace>:<name>": the Lua class (alife = alife(), game_object, CCar, CHelicopter, CPhysicObject,
// CConsole, hanging_lamp, CZoneCampfire, cse_* server objects, CTime, ray_pick ...) or module (level, actor_stats,
// debug_render, game, hud_adjust, relation_registry, weather; _G for globals), then the method, field or property. A
// property or a getter + setter pair has one row with the setter's class (reading is always read). A name the table
// does not know is read. Letter case does not matter (an ltx reader may lowercase keys).
//
// The sources: tools/list_exports.py (its docstring lists them: alife_simulator_script.cpp, script_game_object_script*,
// level_script.cpp, MosquitoBald_script.cpp, HangingLamp.cpp, helicopter_script.cpp, CarScript.cpp,
// PhysicObject_script.cpp, console_registrator_script.cpp, xrServerEntities/xrServer_Objects*_script*.cpp). Every
// .def, def, .def_readwrite, .def_readonly and .property there has a row in kCalls; the virtual methods a server
// object class gets from its luabind_class_* macro (STATE_Read, on_register, switch_online ...) are classified once,
// for every cse_* class, by kMacroVirtuals. Other engine exports (vectors, ini files, net packets, UI, sounds, the
// physics shell, particles, planners ...) are outside the table and read (MOD_LAYER §6). tools/list_exports.py --check
// (run by tests/test_calls.cpp on every unit-test run) fails on a row for no export, an unclassified export and a
// read-only export (def_readonly, a property without a setter) that is not read.
//
// Overrides: [call_classes] `namespace:name = world|owner|view|read` in configs\coop_route*.ltx and coop_compat_*.ltx
// (parse_overrides, Classes). An override can move a function to any class; the request paths stay the engine's
// (kRequestPaths).
//
// Only the engine functions with a gate (xrGame/coop/coop_callgate.h client_gate) consult the class at run time;
// the others run as in single player whatever their row says. The gate never decides how often: the host limits what
// a request path sends (coop_requests.cpp buckets, the session limits, coop.sync.limit).
//
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop
{
namespace calls
{
enum class CallClass : uint8_t
{
	World,
	Owner,
	View,
	Read
};

// What the engine does with one call on this machine.
enum class Gate : uint8_t
{
	Run,     // the stock engine code
	Request, // the request path to the host (the gated function knows it)
	Refuse   // nothing; the function returns what the stock code returns for "nothing" (nil, false)
};

inline const char* class_name(CallClass c)
{
	switch (c)
	{
	case CallClass::World: return "world";
	case CallClass::Owner: return "owner";
	case CallClass::View: return "view";
	case CallClass::Read: return "read";
	}
	return "read";
}

inline const char* gate_name(Gate g)
{
	switch (g)
	{
	case Gate::Run: return "run";
	case Gate::Request: return "request";
	case Gate::Refuse: return "refuse";
	}
	return "run";
}

namespace detail
{
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

inline bool iequals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (lower(a[i]) != lower(b[i]))
			return false;
	return true;
}

inline bool istarts_with(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

inline std::string lowered(std::string_view s)
{
	std::string r(s);
	for (char& c : r)
		c = lower(c);
	return r;
}

inline std::string_view trimmed(std::string_view s)
{
	const std::string_view ws = " \t\r\n";
	const size_t b = s.find_first_not_of(ws);
	if (b == std::string_view::npos)
		return {};
	const size_t e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

inline bool name_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// "namespace:function": two non-empty runs of letters, digits and '_' around one ':'.
inline bool valid_name(std::string_view n)
{
	const size_t colon = n.find(':');
	if (colon == std::string_view::npos || colon == 0 || colon + 1 == n.size())
		return false;
	for (size_t i = 0; i < n.size(); ++i)
		if (i != colon && !name_char(n[i]))
			return false;
	return true;
}
} // namespace detail

// Accepts world, owner, view, read; surrounding blanks and letter case are ignored.
inline bool parse_class(std::string_view text, CallClass& out)
{
	const std::string_view t = detail::trimmed(text);
	for (const CallClass c : {CallClass::World, CallClass::Owner, CallClass::View, CallClass::Read})
		if (detail::iequals(t, class_name(c)))
		{
			out = c;
			return true;
		}
	return false;
}

struct Call
{
	std::string_view name;
	CallClass cls;
};

// Every engine function Lua can call (tools/list_exports.py), by namespace, then by name.
inline constexpr Call kCalls[] = {
	// --- alife(): class alife_simulator (alife_simulator_script.cpp): 39
	{"alife:actor", CallClass::Read},
	{"alife:add_in_restriction", CallClass::World}, // the host's ALife; no request path
	{"alife:add_out_restriction", CallClass::World}, // the host's ALife; no request path
	{"alife:clone_weapon", CallClass::World}, // ReqSpawn into our own inventory (lua_clone)
	{"alife:create", CallClass::World}, // ReqSpawn into our own inventory (coop_requests.cpp lua_create)
	{"alife:create_ammo", CallClass::World}, // ReqSpawn into our own inventory (lua_create)
	{"alife:disable_info", CallClass::World}, // our actor (id 0): the story path; another's: refused
	{"alife:dont_has_info", CallClass::Read},
	{"alife:force_update", CallClass::World}, // the host's ALife scheduler; a client's ALife is frozen (update_scheduled returns at once)
	{"alife:get_children", CallClass::Read},
	{"alife:give_info", CallClass::World}, // our actor (id 0): the story path; another's: refused
	{"alife:has_info", CallClass::Read},
	{"alife:iterate_info", CallClass::Read},
	{"alife:iterate_level_objects_of_clsid", CallClass::Read},
	{"alife:iterate_objects", CallClass::Read},
	{"alife:kill_entity", CallClass::World}, // the host's ALife; no request path
	{"alife:level_id", CallClass::Read},
	{"alife:level_name", CallClass::Read},
	{"alife:max_id", CallClass::Read},
	{"alife:object", CallClass::Read},
	{"alife:object_count", CallClass::Read},
	{"alife:object_ids", CallClass::Read},
	{"alife:objects", CallClass::Read},
	{"alife:register", CallClass::World}, // ReqSpawn of a temporary object (lua_register_spawn)
	{"alife:release", CallClass::World}, // client_destroy, own items only
	{"alife:remove_all_restrictions", CallClass::World}, // the host's ALife; no request path
	{"alife:remove_in_restriction", CallClass::World}, // the host's ALife; no request path
	{"alife:remove_out_restriction", CallClass::World}, // the host's ALife; no request path
	{"alife:set_interactive", CallClass::World}, // the host's ALife; no request path
	{"alife:set_objects_per_update", CallClass::World}, // the host's ALife scheduler; a client's ALife is frozen (update_scheduled returns at once)
	{"alife:set_process_time", CallClass::World}, // the host's ALife scheduler; a client's ALife is frozen (update_scheduled returns at once)
	{"alife:set_switch_distance", CallClass::World},
	{"alife:set_switch_offline", CallClass::World}, // the host's ALife; no request path
	{"alife:set_switch_online", CallClass::World}, // the host's ALife; no request path
	{"alife:spawn_id", CallClass::Read},
	{"alife:story_object", CallClass::Read},
	{"alife:switch_distance", CallClass::Read},
	{"alife:teleport_object", CallClass::World}, // the host's ALife; no request path
	{"alife:valid_object_id", CallClass::Read},
	// --- game objects: class game_object (script_game_object_script*.cpp): 653
	{"game_object:accessible", CallClass::Read},
	{"game_object:accessible_nearest", CallClass::Read},
	{"game_object:accuracy", CallClass::Read},
	{"game_object:action", CallClass::Read},
	{"game_object:action_by_index", CallClass::Read},
	{"game_object:action_count", CallClass::Read},
	{"game_object:activate_slot", CallClass::Owner},
	{"game_object:active_detector", CallClass::Read},
	{"game_object:active_item", CallClass::Read},
	{"game_object:active_slot", CallClass::Read},
	{"game_object:active_sound_count", CallClass::Read},
	{"game_object:active_zone_contact", CallClass::Read},
	{"game_object:actor_look_at_point", CallClass::Owner},
	{"game_object:actor_stop_look_at_point", CallClass::Owner},
	{"game_object:add_animation", CallClass::World},
	{"game_object:add_attachment", CallClass::View},
	{"game_object:add_combat_sound", CallClass::World}, // an NPC's sound set and speech; the host's AI plays them
	{"game_object:add_restrictions", CallClass::World},
	{"game_object:add_sound", CallClass::World}, // an NPC's sound set and speech; the host's AI plays them
	{"game_object:affect_cover", CallClass::Read},
	{"game_object:aim_bone_id", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:aim_time", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:alive", CallClass::Read},
	{"game_object:allow_break_talk_dialog", CallClass::Owner},
	{"game_object:allow_sprint", CallClass::Owner},
	{"game_object:ammo_box_size", CallClass::Read},
	{"game_object:ammo_get_count", CallClass::Read},
	{"game_object:ammo_set_count", CallClass::Owner},
	{"game_object:angle", CallClass::Read},
	{"game_object:animation_count", CallClass::Read},
	{"game_object:animation_slot", CallClass::Read},
	{"game_object:apply_loophole_direction_distance", CallClass::World},
	{"game_object:attach_vehicle", CallClass::World}, // a car: the host drives cars (no request path)
	{"game_object:attachable_item_enabled", CallClass::Read},
	{"game_object:attachable_item_load_attach", CallClass::Owner},
	{"game_object:base_in_restrictions", CallClass::Read},
	{"game_object:base_out_restrictions", CallClass::Read},
	{"game_object:belt_count", CallClass::Read},
	{"game_object:berserk", CallClass::World},
	{"game_object:best_cover", CallClass::Read},
	{"game_object:best_cover_invalidate", CallClass::World},
	{"game_object:best_danger", CallClass::Read},
	{"game_object:best_enemy", CallClass::Read},
	{"game_object:best_item", CallClass::Read},
	{"game_object:best_weapon", CallClass::Read},
	{"game_object:bind_object", CallClass::View},
	{"game_object:binded_object", CallClass::Read},
	{"game_object:bleeding", CallClass::Owner}, // a property; another's creature: refused (F2 T12 round 1)
	{"game_object:body_state", CallClass::Read},
	{"game_object:bone_direction", CallClass::Read},
	{"game_object:bone_id", CallClass::Read},
	{"game_object:bone_name", CallClass::Read},
	{"game_object:bone_parent", CallClass::Read},
	{"game_object:bone_position", CallClass::Read},
	{"game_object:bone_transform", CallClass::Read},
	{"game_object:bone_visible", CallClass::Read},
	{"game_object:bounding_box", CallClass::Read},
	{"game_object:burer_get_force_gravi_attack", CallClass::Read},
	{"game_object:burer_set_force_gravi_attack", CallClass::World},
	{"game_object:buy_condition", CallClass::View}, // a trader's prices; a client's trade window prices with them (the host checks each deal)
	{"game_object:buy_item_condition_factor", CallClass::View}, // a trader's prices; a client's trade window prices with them (the host checks each deal)
	{"game_object:buy_item_exponent", CallClass::View}, // a trader's prices; a client's trade window prices with them (the host checks each deal)
	{"game_object:buy_supplies", CallClass::World}, // fills a trader's stock: creates items (a world spawn)
	{"game_object:can_be_harmed", CallClass::Read},
	{"game_object:can_kill_enemy", CallClass::Read},
	{"game_object:can_kill_member", CallClass::Read},
	{"game_object:can_script_capture", CallClass::Read},
	{"game_object:can_select_weapon", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:can_throw_grenades", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:cast_Actor", CallClass::Read},
	{"game_object:cast_Ammo", CallClass::Read},
	{"game_object:cast_Antirad", CallClass::Read},
	{"game_object:cast_Artefact", CallClass::Read},
	{"game_object:cast_BottleItem", CallClass::Read},
	{"game_object:cast_Car", CallClass::Read},
	{"game_object:cast_CustomOutfit", CallClass::Read},
	{"game_object:cast_CustomZone", CallClass::Read},
	{"game_object:cast_EatableItem", CallClass::Read},
	{"game_object:cast_Explosive", CallClass::Read},
	{"game_object:cast_FoodItem", CallClass::Read},
	{"game_object:cast_Heli", CallClass::Read},
	{"game_object:cast_Helmet", CallClass::Read},
	{"game_object:cast_InventoryBox", CallClass::Read},
	{"game_object:cast_InventoryItem", CallClass::Read},
	{"game_object:cast_InventoryOwner", CallClass::Read},
	{"game_object:cast_Knife", CallClass::Read},
	{"game_object:cast_Medkit", CallClass::Read},
	{"game_object:cast_Missile", CallClass::Read},
	{"game_object:cast_MosquitoBald", CallClass::Read},
	{"game_object:cast_TorridZone", CallClass::Read},
	{"game_object:cast_Weapon", CallClass::Read},
	{"game_object:cast_WeaponMagazined", CallClass::Read},
	{"game_object:cast_WeaponMagazinedWGrenade", CallClass::Read},
	{"game_object:cast_ZoneCampfire", CallClass::Read},
	{"game_object:center", CallClass::Read},
	{"game_object:change_character_rank", CallClass::Owner},
	{"game_object:change_character_reputation", CallClass::Owner},
	{"game_object:change_goodwill", CallClass::World}, // story Relation path (relation_registry.cpp)
	{"game_object:change_health", CallClass::Owner}, // another's creature: refused (F2 T12 round 1)
	{"game_object:change_morale", CallClass::Owner},
	{"game_object:change_power", CallClass::Owner},
	{"game_object:change_psy_health", CallClass::Owner},
	{"game_object:change_radiation", CallClass::Owner},
	{"game_object:change_satiety", CallClass::Owner},
	{"game_object:change_team", CallClass::World},
	{"game_object:character_community", CallClass::Read},
	{"game_object:character_dialogs", CallClass::Read},
	{"game_object:character_icon", CallClass::Read},
	{"game_object:character_name", CallClass::Read},
	{"game_object:character_rank", CallClass::Read},
	{"game_object:character_reputation", CallClass::Read},
	{"game_object:clear_animations", CallClass::World},
	{"game_object:clear_override_animation", CallClass::World},
	{"game_object:clsid", CallClass::Read},
	{"game_object:command", CallClass::World},
	{"game_object:community_goodwill", CallClass::Read},
	{"game_object:condition", CallClass::Read},
	{"game_object:cost", CallClass::Read},
	{"game_object:critically_wounded", CallClass::Read},
	{"game_object:deadbody_can_take", CallClass::World},
	{"game_object:deadbody_can_take_status", CallClass::Read},
	{"game_object:deadbody_closed", CallClass::World},
	{"game_object:deadbody_closed_status", CallClass::Read},
	{"game_object:death_sound_enabled", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:death_time", CallClass::Read},
	{"game_object:destroy_object", CallClass::World}, // client_destroy of an own or free item (the event router), else refused
	{"game_object:detach_vehicle", CallClass::Owner},
	{"game_object:detail_path_type", CallClass::Read},
	{"game_object:direction", CallClass::Read},
	{"game_object:disable_anomaly", CallClass::World},
	{"game_object:disable_hit_marks", CallClass::View},
	{"game_object:disable_info_portion", CallClass::World}, // story path (inventory_owner_info.cpp -> coop_story)
	{"game_object:disable_inv_upgrade", CallClass::World},
	{"game_object:disable_show_hide_sounds", CallClass::View},
	{"game_object:disable_talk", CallClass::World},
	{"game_object:disable_trade", CallClass::World},
	{"game_object:dont_has_info", CallClass::Read},
	{"game_object:drop_item", CallClass::Owner},
	{"game_object:drop_item_and_teleport", CallClass::Owner},
	{"game_object:eat", CallClass::Owner},
	{"game_object:enable_anomaly", CallClass::World},
	{"game_object:enable_attachable_item", CallClass::Owner},
	{"game_object:enable_inv_upgrade", CallClass::World},
	{"game_object:enable_level_changer", CallClass::World}, // level changers are the group's: refused on a client (F2 T12 round 1)
	{"game_object:enable_memory_object", CallClass::World},
	{"game_object:enable_night_vision", CallClass::Owner},
	{"game_object:enable_talk", CallClass::World},
	{"game_object:enable_torch", CallClass::Owner},
	{"game_object:enable_trade", CallClass::World},
	{"game_object:enable_vision", CallClass::World},
	{"game_object:explode", CallClass::World},
	{"game_object:external_sound_start", CallClass::View},
	{"game_object:external_sound_stop", CallClass::View},
	{"game_object:extrapolate_length", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:fake_death_fall_down", CallClass::World},
	{"game_object:fake_death_stand_up", CallClass::World},
	{"game_object:find_best_cover", CallClass::Read},
	{"game_object:fire_make_sense", CallClass::Read},
	{"game_object:force_hide_detector", CallClass::Owner},
	{"game_object:force_set_angle", CallClass::Owner},
	{"game_object:force_set_goodwill", CallClass::World}, // story Relation path (relation_registry.cpp)
	{"game_object:force_set_position", CallClass::Owner},
	{"game_object:force_set_restrictor_type", CallClass::World},
	{"game_object:force_set_rotation", CallClass::Owner},
	{"game_object:force_stand_sleep_animation", CallClass::World},
	{"game_object:force_unload_magazine", CallClass::Owner},
	{"game_object:force_visibility_state", CallClass::World},
	{"game_object:fov", CallClass::Read},
	{"game_object:g_fireParams", CallClass::Read},
	{"game_object:game_vertex_id", CallClass::Read},
	{"game_object:general_goodwill", CallClass::Read},
	{"game_object:get_actor_climb_coef", CallClass::Read},
	{"game_object:get_actor_crouch_coef", CallClass::Read},
	{"game_object:get_actor_damage_stagger_time_factor", CallClass::Read},
	{"game_object:get_actor_default_action_for_object", CallClass::Read},
	{"game_object:get_actor_jump_speed", CallClass::Read},
	{"game_object:get_actor_lookout_coef", CallClass::Read},
	{"game_object:get_actor_max_walk_weight", CallClass::Read},
	{"game_object:get_actor_max_weight", CallClass::Read},
	{"game_object:get_actor_object_looking_at", CallClass::Read},
	{"game_object:get_actor_person_looking_at", CallClass::Read},
	{"game_object:get_actor_relation_flags", CallClass::Read},
	{"game_object:get_actor_run_coef", CallClass::Read},
	{"game_object:get_actor_run_strafe_coef", CallClass::Read},
	{"game_object:get_actor_runback_coef", CallClass::Read},
	{"game_object:get_actor_sprint_koef", CallClass::Read},
	{"game_object:get_actor_sprint_strafe_coef", CallClass::Read},
	{"game_object:get_actor_ui_luminosity", CallClass::Read},
	{"game_object:get_actor_visibility", CallClass::Read},
	{"game_object:get_actor_walk_accel", CallClass::Read},
	{"game_object:get_actor_walk_back_coef", CallClass::Read},
	{"game_object:get_actor_walk_strafe_coef", CallClass::Read},
	{"game_object:get_additional_max_walk_weight", CallClass::Read},
	{"game_object:get_additional_max_weight", CallClass::Read},
	{"game_object:get_ammo_count_for_type", CallClass::Read},
	{"game_object:get_ammo_in_magazine", CallClass::Read},
	{"game_object:get_ammo_total", CallClass::Read},
	{"game_object:get_ammo_type", CallClass::Read},
	{"game_object:get_anomaly_power", CallClass::Read},
	{"game_object:get_anomaly_radius", CallClass::Read},
	{"game_object:get_artefact", CallClass::Read},
	{"game_object:get_artefact_additional_inventory_weight", CallClass::Read},
	{"game_object:get_artefact_bleeding", CallClass::Read},
	{"game_object:get_artefact_health", CallClass::Read},
	{"game_object:get_artefact_immunity", CallClass::Read},
	{"game_object:get_artefact_power", CallClass::Read},
	{"game_object:get_artefact_radiation", CallClass::Read},
	{"game_object:get_artefact_satiety", CallClass::Read},
	{"game_object:get_attached_vehicle", CallClass::Read},
	{"game_object:get_attachment", CallClass::Read},
	{"game_object:get_bone_id", CallClass::Read},
	{"game_object:get_campfire", CallClass::Read},
	{"game_object:get_car", CallClass::Read},
	{"game_object:get_corpse", CallClass::Read},
	{"game_object:get_current_direction", CallClass::Read},
	{"game_object:get_current_holder", CallClass::Read},
	{"game_object:get_current_loophole_id", CallClass::Read},
	{"game_object:get_current_outfit", CallClass::Read},
	{"game_object:get_current_outfit_protection", CallClass::Read},
	{"game_object:get_current_point_index", CallClass::Read},
	{"game_object:get_current_smart_cover_name", CallClass::Read},
	{"game_object:get_default_shaders", CallClass::Read},
	{"game_object:get_dest_smart_cover", CallClass::Read},
	{"game_object:get_dest_smart_cover_name", CallClass::Read},
	{"game_object:get_enable_anomalies_damage", CallClass::Read},
	{"game_object:get_enable_anomalies_pathfinding", CallClass::Read},
	{"game_object:get_enemy", CallClass::Read},
	{"game_object:get_enemy_strength", CallClass::Read},
	{"game_object:get_force_anti_aim", CallClass::Read},
	{"game_object:get_hanging_lamp", CallClass::Read},
	{"game_object:get_helicopter", CallClass::Read},
	{"game_object:get_holder_class", CallClass::Read},
	{"game_object:get_inv_grid_rect", CallClass::Read},
	{"game_object:get_inv_max_weight", CallClass::Read},
	{"game_object:get_inv_weight", CallClass::Read},
	{"game_object:get_luminocity", CallClass::Read},
	{"game_object:get_luminocity_hemi", CallClass::Read},
	{"game_object:get_main_weapon_type", CallClass::Read},
	{"game_object:get_max_uses", CallClass::Read},
	{"game_object:get_monster_hit_info", CallClass::Read},
	{"game_object:get_movement_speed", CallClass::Read},
	{"game_object:get_object_luminocity", CallClass::Read},
	{"game_object:get_object_visible_distance", CallClass::Read},
	{"game_object:get_physics_object", CallClass::Read},
	{"game_object:get_physics_shell", CallClass::Read},
	{"game_object:get_projector", CallClass::Read},
	{"game_object:get_remaining_uses", CallClass::Read},
	{"game_object:get_restrictor_type", CallClass::Read},
	{"game_object:get_scope_ui", CallClass::Read},
	{"game_object:get_script", CallClass::Read},
	{"game_object:get_script_name", CallClass::Read},
	{"game_object:get_shaders", CallClass::Read},
	{"game_object:get_smart_cover_description", CallClass::Read},
	{"game_object:get_sound_info", CallClass::Read},
	{"game_object:get_spatial_type", CallClass::Read},
	{"game_object:get_start_dialog", CallClass::Read},
	{"game_object:get_state", CallClass::Read},
	{"game_object:get_stmgun", CallClass::Read},
	{"game_object:get_talking_npc", CallClass::Read},
	{"game_object:get_task", CallClass::Read},
	{"game_object:get_task_state", CallClass::Read},
	{"game_object:get_total_weight", CallClass::Read},
	{"game_object:get_total_weight_force_update", CallClass::Read},
	{"game_object:get_upgr_icon_rect", CallClass::Read},
	{"game_object:get_visibility_state", CallClass::Read},
	{"game_object:get_visual_name", CallClass::Read},
	{"game_object:get_weapon_substate", CallClass::Read},
	{"game_object:get_weapon_type", CallClass::Read},
	{"game_object:GetBulletCheckVisual", CallClass::Read},
	{"game_object:give_game_news", CallClass::View},
	{"game_object:give_info_portion", CallClass::World}, // story path (inventory_owner_info.cpp -> coop_story)
	{"game_object:give_money", CallClass::World}, // our actor: the money journal (F2 T1); an NPC: refused
	{"game_object:give_talk_message", CallClass::View},
	{"game_object:give_talk_message2", CallClass::View},
	{"game_object:give_task", CallClass::World}, // story task path (GameTaskManager / GameTask hooks)
	{"game_object:goodwill", CallClass::Read},
	{"game_object:group", CallClass::Read},
	{"game_object:group_throw_time_interval", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:has_ammo_type", CallClass::Read},
	{"game_object:has_info", CallClass::Read},
	{"game_object:has_upgrade", CallClass::Read},
	{"game_object:head_orientation", CallClass::Read},
	{"game_object:health", CallClass::Owner}, // a property; another's creature: refused (F2 T12 round 1)
	{"game_object:hide_detector", CallClass::Owner},
	{"game_object:hide_weapon", CallClass::Owner},
	{"game_object:hit", CallClass::Owner},
	{"game_object:hud_fire_bone2_id", CallClass::Read},
	{"game_object:hud_fire_bone2_name", CallClass::Read},
	{"game_object:hud_fire_bone_id", CallClass::Read},
	{"game_object:hud_fire_bone_name", CallClass::Read},
	{"game_object:hud_fire_bone_silencer_id", CallClass::Read},
	{"game_object:hud_fire_bone_silencer_name", CallClass::Read},
	{"game_object:hud_fire_point", CallClass::Read},
	{"game_object:hud_fire_point2", CallClass::Read},
	{"game_object:hud_fire_point_silencer", CallClass::Read},
	{"game_object:hud_inertion_enabled", CallClass::Read},
	{"game_object:id", CallClass::Read},
	{"game_object:idle_max_time", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:idle_min_time", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:ignore_monster_threshold", CallClass::World},
	{"game_object:in_current_loophole_fov", CallClass::Read},
	{"game_object:in_current_loophole_range", CallClass::Read},
	{"game_object:in_loophole_fov", CallClass::Read},
	{"game_object:in_loophole_range", CallClass::Read},
	{"game_object:in_restrictions", CallClass::Read},
	{"game_object:in_smart_cover", CallClass::Read},
	{"game_object:inactualize_patrol_path", CallClass::World},
	{"game_object:info_add", CallClass::View},
	{"game_object:info_clear", CallClass::View},
	{"game_object:inside", CallClass::Read},
	{"game_object:inside_anomaly", CallClass::Read},
	{"game_object:install_upgrade", CallClass::Owner},
	{"game_object:inv_box_can_take", CallClass::World},
	{"game_object:inv_box_can_take_status", CallClass::Read},
	{"game_object:inv_box_closed", CallClass::World},
	{"game_object:inv_box_closed_status", CallClass::Read},
	{"game_object:invalidate_restrictions", CallClass::World},
	{"game_object:inventory_for_each", CallClass::Read},
	{"game_object:invulnerable", CallClass::Owner}, // getter and setter share the name; the setter changes the object
	{"game_object:is_active_task", CallClass::Read},
	{"game_object:is_actor", CallClass::Read},
	{"game_object:is_ammo", CallClass::Read},
	{"game_object:is_anomaly", CallClass::Read},
	{"game_object:is_artefact", CallClass::Read},
	{"game_object:is_body_turning", CallClass::Read},
	{"game_object:is_custom_monster", CallClass::Read},
	{"game_object:is_device_enabled", CallClass::Read},
	{"game_object:is_door_blocked_by_npc", CallClass::Read},
	{"game_object:is_door_locked_for_npc", CallClass::Read},
	{"game_object:is_enabled_anomaly", CallClass::Read},
	{"game_object:is_entity_alive", CallClass::Read},
	{"game_object:is_grenade_launcher", CallClass::Read},
	{"game_object:is_helmet", CallClass::Read},
	{"game_object:is_hit_anim_playing", CallClass::Read},
	{"game_object:is_hud_item", CallClass::Read},
	{"game_object:is_inv_box_empty", CallClass::Read},
	{"game_object:is_inv_upgrade_enabled", CallClass::Read},
	{"game_object:is_inventory_box", CallClass::Read},
	{"game_object:is_inventory_item", CallClass::Read},
	{"game_object:is_inventory_owner", CallClass::Read},
	{"game_object:is_level_changer_enabled", CallClass::Read},
	{"game_object:is_monster", CallClass::Read},
	{"game_object:is_on_belt", CallClass::Read},
	{"game_object:is_outfit", CallClass::Read},
	{"game_object:is_scope", CallClass::Read},
	{"game_object:is_silencer", CallClass::Read},
	{"game_object:is_space_restrictor", CallClass::Read},
	{"game_object:is_stalker", CallClass::Read},
	{"game_object:is_talk_enabled", CallClass::Read},
	{"game_object:is_talking", CallClass::Read},
	{"game_object:is_there_items_to_pickup", CallClass::Read},
	{"game_object:is_trade_enabled", CallClass::Read},
	{"game_object:is_trader", CallClass::Read},
	{"game_object:is_weapon", CallClass::Read},
	{"game_object:is_weapon_gl", CallClass::Read},
	{"game_object:is_weapon_going_to_be_strapped", CallClass::Read},
	{"game_object:is_weapon_magazined", CallClass::Read},
	{"game_object:item_allow_trade", CallClass::Owner},
	{"game_object:item_deny_trade", CallClass::Owner},
	{"game_object:item_in_slot", CallClass::Read},
	{"game_object:item_on_belt", CallClass::Read},
	{"game_object:iterate_attachments", CallClass::Read},
	{"game_object:iterate_belt", CallClass::Read},
	{"game_object:iterate_feel_touch", CallClass::Read},
	{"game_object:iterate_installed_upgrades", CallClass::Read},
	{"game_object:iterate_inventory", CallClass::Read},
	{"game_object:iterate_inventory_box", CallClass::Read},
	{"game_object:iterate_ruck", CallClass::Read},
	{"game_object:jump", CallClass::World},
	{"game_object:kill", CallClass::Owner}, // another's object: refused (MOD_LAYER §2.3 minimal coverage)
	{"game_object:level_vertex_id", CallClass::Read},
	{"game_object:list_bones", CallClass::Read},
	{"game_object:location_on_path", CallClass::Read},
	{"game_object:lock_door_for_npc", CallClass::World},
	{"game_object:lookout_max_time", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:lookout_min_time", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:make_enemy_visible", CallClass::World},
	{"game_object:make_item_active", CallClass::Owner},
	{"game_object:make_object_visible_somewhen", CallClass::World},
	{"game_object:mark_item_dropped", CallClass::Owner},
	{"game_object:marked_dropped", CallClass::Read},
	{"game_object:mass", CallClass::Read},
	{"game_object:max_health", CallClass::Read},
	{"game_object:max_ignore_monster_distance", CallClass::World},
	{"game_object:memory_hit_objects", CallClass::Read},
	{"game_object:memory_position", CallClass::Read},
	{"game_object:memory_remove_links", CallClass::World},
	{"game_object:memory_sound_objects", CallClass::Read},
	{"game_object:memory_time", CallClass::Read},
	{"game_object:memory_visible_objects", CallClass::Read},
	{"game_object:mental_state", CallClass::Read},
	{"game_object:money", CallClass::Read},
	{"game_object:morale", CallClass::Owner}, // a property; another's creature: refused (F2 T12 round 1)
	{"game_object:motivation_action_manager", CallClass::Read},
	{"game_object:move_to_belt", CallClass::Owner},
	{"game_object:move_to_ruck", CallClass::Owner},
	{"game_object:move_to_slot", CallClass::Owner},
	{"game_object:movement_enabled", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:movement_target_reached", CallClass::Read},
	{"game_object:movement_type", CallClass::Read},
	{"game_object:name", CallClass::Read},
	{"game_object:night_vision_allowed", CallClass::Owner},
	{"game_object:night_vision_enabled", CallClass::Read},
	{"game_object:not_yet_visible_objects", CallClass::Read},
	{"game_object:object", CallClass::Read},
	{"game_object:object_count", CallClass::Read},
	{"game_object:object_id", CallClass::Read},
	{"game_object:on_door_is_closed", CallClass::World},
	{"game_object:on_door_is_open", CallClass::World},
	{"game_object:out_restrictions", CallClass::Read},
	{"game_object:parent", CallClass::Read},
	{"game_object:path_completed", CallClass::Read},
	{"game_object:path_type", CallClass::Read},
	{"game_object:patrol", CallClass::Read},
	{"game_object:patrol_path_make_inactual", CallClass::World},
	{"game_object:phantom_set_enemy", CallClass::World},
	{"game_object:play_cycle", CallClass::View}, // an animation or particles of this machine's copy
	{"game_object:play_hud_motion", CallClass::View},
	{"game_object:play_sound", CallClass::World}, // an NPC's sound set and speech; the host's AI plays them
	{"game_object:poltergeist_get_actor_ignore", CallClass::Read},
	{"game_object:poltergeist_set_actor_ignore", CallClass::World},
	{"game_object:position", CallClass::Read},
	{"game_object:power", CallClass::Owner}, // a property; another's creature: refused (F2 T12 round 1)
	{"game_object:power_critical", CallClass::Read},
	{"game_object:profile_name", CallClass::Read},
	{"game_object:psy_factor", CallClass::Read},
	{"game_object:psy_health", CallClass::Owner}, // a property; another's creature: refused (F2 T12 round 1)
	{"game_object:radiation", CallClass::Owner}, // a property; another's creature: refused (F2 T12 round 1)
	{"game_object:range", CallClass::Read},
	{"game_object:rank", CallClass::Read},
	{"game_object:rank_name", CallClass::Read},
	{"game_object:register_door_for_npc", CallClass::World},
	{"game_object:register_in_combat", CallClass::World},
	{"game_object:relation", CallClass::Read},
	{"game_object:release_stand_sleep_animation", CallClass::World},
	{"game_object:reload_weapon", CallClass::Owner},
	{"game_object:remove_all_restrictions", CallClass::World},
	{"game_object:remove_attachment", CallClass::View},
	{"game_object:remove_home", CallClass::World},
	{"game_object:remove_restrictions", CallClass::World},
	{"game_object:remove_sound", CallClass::World}, // an NPC's sound set and speech; the host's AI plays them
	{"game_object:reset_action_queue", CallClass::World},
	{"game_object:reset_bone_protections", CallClass::Owner},
	{"game_object:reset_color_animator", CallClass::View},
	{"game_object:reset_shader", CallClass::View},
	{"game_object:restore_default_start_dialog", CallClass::World},
	{"game_object:restore_ignore_monster_threshold", CallClass::World},
	{"game_object:restore_max_ignore_monster_distance", CallClass::World},
	{"game_object:restore_sound_threshold", CallClass::World},
	{"game_object:restore_weapon", CallClass::Owner},
	{"game_object:run_talk_dialog", CallClass::Owner},
	{"game_object:safe_cover", CallClass::Read},
	{"game_object:satiety", CallClass::Owner}, // a property; another's creature: refused (F2 T12 round 1)
	{"game_object:script", CallClass::World},
	{"game_object:section", CallClass::Read},
	{"game_object:see", CallClass::Read},
	{"game_object:sell_condition", CallClass::View}, // a trader's prices; a client's trade window prices with them (the host checks each deal)
	{"game_object:sell_item_exponent", CallClass::View}, // a trader's prices; a client's trade window prices with them (the host checks each deal)
	{"game_object:set_active_task", CallClass::Owner},
	{"game_object:set_actor_box_y_offset", CallClass::Owner},
	{"game_object:set_actor_climb_coef", CallClass::Owner},
	{"game_object:set_actor_crouch_coef", CallClass::Owner},
	{"game_object:set_actor_damage_stagger_time_factor", CallClass::Owner},
	{"game_object:set_actor_direction", CallClass::Owner},
	{"game_object:set_actor_jump_speed", CallClass::Owner},
	{"game_object:set_actor_lookout_coef", CallClass::Owner},
	{"game_object:set_actor_max_walk_weight", CallClass::Owner},
	{"game_object:set_actor_max_weight", CallClass::Owner},
	{"game_object:set_actor_position", CallClass::Owner},
	{"game_object:set_actor_relation_flags", CallClass::World},
	{"game_object:set_actor_run_coef", CallClass::Owner},
	{"game_object:set_actor_run_strafe_coef", CallClass::Owner},
	{"game_object:set_actor_runback_coef", CallClass::Owner},
	{"game_object:set_actor_sprint_koef", CallClass::Owner},
	{"game_object:set_actor_sprint_strafe_coef", CallClass::Owner},
	{"game_object:set_actor_walk_accel", CallClass::Owner},
	{"game_object:set_actor_walk_back_coef", CallClass::Owner},
	{"game_object:set_actor_walk_strafe_coef", CallClass::Owner},
	{"game_object:set_additional_max_walk_weight", CallClass::Owner},
	{"game_object:set_additional_max_weight", CallClass::Owner},
	{"game_object:set_aim_params", CallClass::World},
	{"game_object:set_alien_control", CallClass::World},
	{"game_object:set_ammo_elapsed", CallClass::Owner},
	{"game_object:set_ammo_type", CallClass::Owner},
	{"game_object:set_anomaly_position", CallClass::World},
	{"game_object:set_anomaly_power", CallClass::World},
	{"game_object:set_anomaly_radius", CallClass::World},
	{"game_object:set_artefact_additional_inventory_weight", CallClass::Owner},
	{"game_object:set_artefact_bleeding", CallClass::Owner},
	{"game_object:set_artefact_health", CallClass::Owner},
	{"game_object:set_artefact_immunity", CallClass::Owner},
	{"game_object:set_artefact_power", CallClass::Owner},
	{"game_object:set_artefact_radiation", CallClass::Owner},
	{"game_object:set_artefact_satiety", CallClass::Owner},
	{"game_object:set_body_state", CallClass::World},
	{"game_object:set_bone_visible", CallClass::View},
	{"game_object:set_callback", CallClass::View},
	{"game_object:set_can_be_harmed", CallClass::Owner},
	{"game_object:set_capture_anim", CallClass::World},
	{"game_object:set_character_community", CallClass::Owner},
	{"game_object:set_character_icon", CallClass::Owner},
	{"game_object:set_character_rank", CallClass::Owner},
	{"game_object:set_character_reputation", CallClass::Owner},
	{"game_object:set_collision_off", CallClass::World},
	{"game_object:set_color_animator", CallClass::View},
	{"game_object:set_community_goodwill", CallClass::World}, // story Relation path (relation_registry.cpp)
	{"game_object:set_condition", CallClass::Owner},
	{"game_object:set_const_force", CallClass::World}, // a physics push of a host-simulated object
	{"game_object:set_custom_panic_threshold", CallClass::World},
	{"game_object:set_default_panic_threshold", CallClass::World},
	{"game_object:set_desired_direction", CallClass::World},
	{"game_object:set_desired_position", CallClass::World},
	{"game_object:set_dest_game_vertex_id", CallClass::World},
	{"game_object:set_dest_level_vertex_id", CallClass::World},
	{"game_object:set_dest_loophole", CallClass::World},
	{"game_object:set_dest_smart_cover", CallClass::World},
	{"game_object:set_detail_path_type", CallClass::World},
	{"game_object:set_device_enabled", CallClass::Owner},
	{"game_object:set_enable_anomalies_damage", CallClass::World},
	{"game_object:set_enable_anomalies_pathfinding", CallClass::World},
	{"game_object:set_enable_movement_collision", CallClass::World},
	{"game_object:set_enemy", CallClass::World},
	{"game_object:set_enemy_callback", CallClass::World}, // steers the host's AI
	{"game_object:set_fastcall", CallClass::View},
	{"game_object:set_fire_queue_scale", CallClass::World},
	{"game_object:set_force_anti_aim", CallClass::World},
	{"game_object:set_fov", CallClass::World},
	{"game_object:set_goodwill", CallClass::World}, // story Relation path (relation_registry.cpp)
	{"game_object:set_health_ex", CallClass::Owner}, // another's creature: refused (F2 T12 round 1)
	{"game_object:set_health_restore_boost", CallClass::Owner},
	{"game_object:set_hit_redirect", CallClass::World},
	{"game_object:set_home", CallClass::World},
	{"game_object:set_hud_fire_bone", CallClass::View},
	{"game_object:set_hud_fire_bone2", CallClass::View},
	{"game_object:set_hud_fire_bone_silencer", CallClass::View},
	{"game_object:set_hud_fire_point", CallClass::View},
	{"game_object:set_hud_fire_point2", CallClass::View},
	{"game_object:set_hud_fire_point_silencer", CallClass::View},
	{"game_object:set_hud_inertion_enabled", CallClass::View},
	{"game_object:set_idle_particles", CallClass::View}, // an animation or particles of this machine's copy
	{"game_object:set_invisible", CallClass::World},
	{"game_object:set_item", CallClass::World},
	{"game_object:set_level_changer_invitation", CallClass::View}, // a trader's animation and voice, a level changer's text
	{"game_object:set_main_weapon_type", CallClass::Owner},
	{"game_object:set_manual_invisibility", CallClass::World},
	{"game_object:set_mental_state", CallClass::World},
	{"game_object:set_movement_hold", CallClass::World},
	{"game_object:set_movement_selection_type", CallClass::World},
	{"game_object:set_movement_speed", CallClass::Owner},
	{"game_object:set_movement_type", CallClass::World},
	{"game_object:set_nonscript_usable", CallClass::World},
	{"game_object:set_npc_position", CallClass::Owner}, // another's object: refused (MOD_LAYER §2.3 minimal coverage)
	{"game_object:set_override_animation", CallClass::World},
	{"game_object:set_path_type", CallClass::World},
	{"game_object:set_patrol_extrapolate_callback", CallClass::World}, // steers the host's AI
	{"game_object:set_patrol_path", CallClass::World},
	{"game_object:set_previous_point", CallClass::World},
	{"game_object:set_psy_factor", CallClass::Owner},
	{"game_object:set_queue_size", CallClass::World},
	{"game_object:set_range", CallClass::World},
	{"game_object:set_relation", CallClass::World}, // story Relation path (relation_registry.cpp)
	{"game_object:set_remaining_uses", CallClass::Owner},
	{"game_object:set_restrictor_type", CallClass::World},
	{"game_object:set_scope_ui", CallClass::View},
	{"game_object:set_shader", CallClass::View},
	{"game_object:set_sight", CallClass::World},
	{"game_object:set_smart_cover_target", CallClass::World},
	{"game_object:set_smart_cover_target_default", CallClass::World},
	{"game_object:set_smart_cover_target_fire", CallClass::World},
	{"game_object:set_smart_cover_target_fire_no_lookout", CallClass::World},
	{"game_object:set_smart_cover_target_idle", CallClass::World},
	{"game_object:set_smart_cover_target_lookout", CallClass::World},
	{"game_object:set_smart_cover_target_selector", CallClass::World},
	{"game_object:set_sound_mask", CallClass::World}, // an NPC's sound set and speech; the host's AI plays them
	{"game_object:set_sound_threshold", CallClass::World},
	{"game_object:set_spatial_type", CallClass::World}, // how the world collides with the object
	{"game_object:set_start_dialog", CallClass::World},
	{"game_object:set_start_point", CallClass::World},
	{"game_object:set_sympathy", CallClass::World},
	{"game_object:set_task_state", CallClass::World}, // story task path (GameTaskManager / GameTask hooks)
	{"game_object:set_tip_text", CallClass::View},
	{"game_object:set_tip_text_default", CallClass::View},
	{"game_object:set_trader_global_anim", CallClass::View}, // a trader's animation and voice, a level changer's text
	{"game_object:set_trader_head_anim", CallClass::View}, // a trader's animation and voice, a level changer's text
	{"game_object:set_trader_sound", CallClass::View}, // a trader's animation and voice, a level changer's text
	{"game_object:set_view_distance_factor", CallClass::World},
	{"game_object:set_vis_state", CallClass::World},
	{"game_object:set_visible_enemy_bias", CallClass::World},
	{"game_object:set_vision_speed", CallClass::World},
	{"game_object:set_visual_memory_enabled", CallClass::World},
	{"game_object:set_visual_name", CallClass::World},
	{"game_object:set_weapon_type", CallClass::Owner},
	{"game_object:set_weight", CallClass::Owner},
	{"game_object:SetBulletCheckVisual", CallClass::World}, // how the world collides with the object
	{"game_object:show_condition", CallClass::View}, // a trader's prices; a client's trade window prices with them (the host checks each deal)
	{"game_object:show_detector", CallClass::Owner},
	{"game_object:sight_params", CallClass::Read},
	{"game_object:skip_transfer_enemy", CallClass::World},
	{"game_object:sniper_fire_mode", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:sniper_update_rate", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:sound_prefix", CallClass::World}, // getter and setter share the name; an NPC's sound set (the host's AI plays it)
	{"game_object:sound_voice_prefix", CallClass::Read},
	{"game_object:spawn_ini", CallClass::Read},
	{"game_object:special_danger_move", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:squad", CallClass::Read},
	{"game_object:start_particles", CallClass::View},
	{"game_object:start_trade", CallClass::Owner},
	{"game_object:start_upgrade", CallClass::Owner},
	{"game_object:stop_particles", CallClass::View},
	{"game_object:stop_talk", CallClass::Owner},
	{"game_object:story_id", CallClass::Read},
	{"game_object:suitable_smart_cover", CallClass::Read},
	{"game_object:switch_state", CallClass::Owner},
	{"game_object:switch_to_talk", CallClass::Owner},
	{"game_object:switch_to_trade", CallClass::Owner},
	{"game_object:switch_to_upgrade", CallClass::Owner},
	{"game_object:sympathy", CallClass::Read},
	{"game_object:take_item", CallClass::World}, // ReqTake / ReqTransfer when our actor takes or the item is ours, else refused
	{"game_object:take_items_enabled", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:target_body_state", CallClass::Read},
	{"game_object:target_mental_state", CallClass::Read},
	{"game_object:target_movement_type", CallClass::Read},
	{"game_object:team", CallClass::Read},
	{"game_object:torch_enabled", CallClass::Read},
	{"game_object:transfer_item", CallClass::World}, // ReqTransfer / ReqTake / ReqDrop when our actor is on one side, else refused
	{"game_object:transfer_money", CallClass::World}, // our actor pays: its side goes to the money journal; else refused
	{"game_object:unload_magazine", CallClass::Owner},
	{"game_object:unlock_door_for_npc", CallClass::World},
	{"game_object:unregister_door_for_npc", CallClass::World},
	{"game_object:unregister_in_combat", CallClass::World},
	{"game_object:update_torch", CallClass::Owner},
	{"game_object:update_weight", CallClass::Read},
	{"game_object:use", CallClass::View}, // the player's interaction: the object's use callback, a search window; a physics object's use goes to the host in the engine (client_intercept_use)
	{"game_object:use_smart_covers_only", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:vertex_in_direction", CallClass::Read},
	{"game_object:visibility_threshold", CallClass::Read},
	{"game_object:vision_enabled", CallClass::Read},
	{"game_object:weapon_addon_attach", CallClass::Owner},
	{"game_object:weapon_addon_detach", CallClass::Owner},
	{"game_object:weapon_grenadelauncher_status", CallClass::Read},
	{"game_object:weapon_in_grenade_mode", CallClass::Read},
	{"game_object:weapon_is_grenadelauncher", CallClass::Read},
	{"game_object:weapon_is_scope", CallClass::Read},
	{"game_object:weapon_is_silencer", CallClass::Read},
	{"game_object:weapon_scope_status", CallClass::Read},
	{"game_object:weapon_silencer_status", CallClass::Read},
	{"game_object:weapon_strapped", CallClass::Read},
	{"game_object:weapon_unstrapped", CallClass::Read},
	{"game_object:weight", CallClass::Read},
	{"game_object:who_hit_name", CallClass::Read},
	{"game_object:who_hit_section_name", CallClass::Read},
	{"game_object:wounded", CallClass::World}, // getter and setter share the name; the setter changes an NPC
	{"game_object:xform", CallClass::Read},
	// --- module level (level_script.cpp): 119
	{"level:actor_moving_state", CallClass::Read},
	{"level:add_attachment", CallClass::View},
	{"level:add_bullet", CallClass::World},
	{"level:add_call", CallClass::View},
	{"level:add_cam_effector", CallClass::View},
	{"level:add_complex_effector", CallClass::View},
	{"level:add_dialog_to_render", CallClass::View},
	{"level:add_pp_effector", CallClass::View},
	{"level:change_game_time", CallClass::World}, // TimeRequest to the host
	{"level:check_cam_effector", CallClass::Read},
	{"level:check_pp_effector", CallClass::Read},
	{"level:client_spawn_manager", CallClass::Read},
	{"level:disable_input", CallClass::View},
	{"level:enable_input", CallClass::View},
	{"level:environment", CallClass::Read},
	{"level:game_id", CallClass::Read},
	{"level:get_active_cam", CallClass::Read},
	{"level:get_attachment", CallClass::Read},
	{"level:get_bounding_volume", CallClass::Read},
	{"level:get_cam_effector_factor", CallClass::Read},
	{"level:get_cam_effector_length", CallClass::Read},
	{"level:get_env_rads", CallClass::Read},
	{"level:get_game_difficulty", CallClass::Read},
	{"level:get_music_volume", CallClass::Read},
	{"level:get_nearby_covers", CallClass::Read},
	{"level:get_nearby_vertices", CallClass::Read},
	{"level:get_pp_effector_factor", CallClass::Read},
	{"level:get_pp_effector_length", CallClass::Read},
	{"level:get_rain_volume", CallClass::Read},
	{"level:get_snd_volume", CallClass::Read},
	{"level:get_start_time", CallClass::Read},
	{"level:get_sun_color", CallClass::Read},
	{"level:get_sun_intensity", CallClass::Read},
	{"level:get_sun_pos", CallClass::Read},
	{"level:get_target_dist", CallClass::Read},
	{"level:get_target_element", CallClass::Read},
	{"level:get_target_obj", CallClass::Read},
	{"level:get_target_pos", CallClass::Read},
	{"level:get_target_result", CallClass::Read},
	{"level:get_time_days", CallClass::Read},
	{"level:get_time_factor", CallClass::Read},
	{"level:get_time_hours", CallClass::Read},
	{"level:get_time_minutes", CallClass::Read},
	{"level:get_view_entity", CallClass::Read},
	{"level:get_weather", CallClass::Read},
	{"level:get_weather_weight", CallClass::Read},
	{"level:get_wfx_time", CallClass::Read},
	{"level:hide_indicators", CallClass::View},
	{"level:hide_indicators_safe", CallClass::View},
	{"level:high_cover_in_direction", CallClass::Read},
	{"level:hold_action", CallClass::Owner}, // input of this machine's actor
	{"level:is_sun_visible", CallClass::Read},
	{"level:is_wfx_playing", CallClass::Read},
	{"level:iterate_attachments", CallClass::Read},
	{"level:iterate_nearest", CallClass::Read},
	{"level:iterate_sounds", CallClass::Read},
	{"level:low_cover_in_direction", CallClass::Read},
	{"level:map_add_object_spot", CallClass::View},
	{"level:map_add_object_spot_ser", CallClass::View},
	{"level:map_change_spot_hint", CallClass::View},
	{"level:map_get_object_minimap_spot_static", CallClass::Read},
	{"level:map_get_object_spot_static", CallClass::Read},
	{"level:map_get_object_spots_by_id", CallClass::Read},
	{"level:map_has_object_spot", CallClass::Read},
	{"level:map_manager", CallClass::Read},
	{"level:map_pan_to", CallClass::View},
	{"level:map_pan_to_level", CallClass::View},
	{"level:map_remove_all_object_spots", CallClass::View},
	{"level:map_remove_object_spot", CallClass::View},
	{"level:name", CallClass::Read},
	{"level:object_by_id", CallClass::Read},
	{"level:patrol_path_add", CallClass::View}, // this machine's path registry: patrol(name) here needs it
	{"level:patrol_path_exists", CallClass::Read},
	{"level:patrol_path_remove", CallClass::View}, // this machine's path registry: patrol(name) here needs it
	{"level:physics_world", CallClass::Read},
	{"level:pick_material", CallClass::Read},
	{"level:prefetch_sound", CallClass::View},
	{"level:present", CallClass::Read},
	{"level:press_action", CallClass::Owner}, // input of this machine's actor
	{"level:rain_factor", CallClass::Read},
	{"level:rain_hemi", CallClass::Read},
	{"level:rain_wetness", CallClass::Read},
	{"level:ray_pick", CallClass::Read},
	{"level:release_action", CallClass::Owner}, // input of this machine's actor
	{"level:remove_attachment", CallClass::View},
	{"level:remove_call", CallClass::View},
	{"level:remove_calls_for_object", CallClass::View},
	{"level:remove_cam_custom_position_direction", CallClass::View},
	{"level:remove_cam_effector", CallClass::View},
	{"level:remove_complex_effector", CallClass::View},
	{"level:remove_dialog_to_render", CallClass::View},
	{"level:remove_hud_motion_cam_effectors", CallClass::View},
	{"level:remove_pp_effector", CallClass::View},
	{"level:scheduler_flush", CallClass::View},
	{"level:send", CallClass::World},
	{"level:set_active_cam", CallClass::View},
	{"level:set_cam_custom_position_direction", CallClass::View},
	{"level:set_cam_effector_factor", CallClass::View},
	{"level:set_game_difficulty", CallClass::World}, // the host's setting (coop_settings); no request path
	{"level:set_music_volume", CallClass::View},
	{"level:set_pp_effector_factor", CallClass::View},
	{"level:set_snd_volume", CallClass::View},
	{"level:set_time_factor", CallClass::World}, // the host owns the clock (worldstate::allow_time_factor_change)
	{"level:set_view_entity", CallClass::View},
	{"level:set_weather", CallClass::World}, // the host decides the weather (WeatherSync)
	{"level:set_weather_fx", CallClass::World}, // the host decides the weather (WeatherSync)
	{"level:set_weather_smooth", CallClass::World}, // the host decides the weather (WeatherSync)
	{"level:set_weather_weight", CallClass::World}, // the host decides the weather (WeatherSync)
	{"level:show_indicators", CallClass::View},
	{"level:show_weapon", CallClass::View},
	{"level:spawn_item", CallClass::World}, // ReqSpawn into our own inventory (the event router's filter_spawn)
	{"level:spawn_phantom", CallClass::World}, // a world spawn: the host's
	{"level:start_weather_fx_from_time", CallClass::World}, // the host decides the weather (WeatherSync)
	{"level:stop_weather_fx", CallClass::World}, // the host decides the weather (WeatherSync)
	{"level:take_screenshot", CallClass::View},
	{"level:vertex_id", CallClass::Read},
	{"level:vertex_in_direction", CallClass::Read},
	{"level:vertex_link", CallClass::Read},
	{"level:vertex_position", CallClass::Read},
	// --- module actor_stats (level_script.cpp): 3
	{"actor_stats:add_points", CallClass::Owner},
	{"actor_stats:add_points_str", CallClass::Owner},
	{"actor_stats:get_points", CallClass::Read},
	// --- module debug_render (level_script.cpp): 5
	{"debug_render:add_object", CallClass::View},
	{"debug_render:get_flags", CallClass::Read},
	{"debug_render:get_object", CallClass::Read},
	{"debug_render:remove_object", CallClass::View},
	{"debug_render:set_flags", CallClass::View},
	// --- module game (level_script.cpp): 36
	{"game:active_tutorial_name", CallClass::Read},
	{"game:actor_lower_weapon", CallClass::Owner},
	{"game:actor_weapon_lowered", CallClass::Read},
	{"game:change_game_news_show_time", CallClass::View},
	{"game:get_actor_alcohol", CallClass::Read},
	{"game:get_game_time", CallClass::Read},
	{"game:get_motion_length", CallClass::Read},
	{"game:get_resolutions", CallClass::Read},
	{"game:get_visual_userdata", CallClass::Read},
	{"game:has_active_tutorial", CallClass::Read},
	{"game:hud_motion_allowed", CallClass::Read},
	{"game:motion_exists", CallClass::Read},
	{"game:only_allow_movekeys", CallClass::Owner},
	{"game:only_movekeys_allowed", CallClass::Read},
	{"game:open_originals_link", CallClass::View},
	{"game:play_hud_anm", CallClass::View},
	{"game:play_hud_motion", CallClass::View},
	{"game:prefetch_model", CallClass::View},
	{"game:prefetch_texture", CallClass::View},
	{"game:reload_language", CallClass::View},
	{"game:reload_ui_xml", CallClass::View},
	{"game:set_actor_allow_ladder", CallClass::Owner},
	{"game:set_hud_anm_time", CallClass::View},
	{"game:set_nv_lumfactor", CallClass::View},
	{"game:start_tutorial", CallClass::View},
	{"game:stop_all_hud_anms", CallClass::View},
	{"game:stop_hud_anm", CallClass::View},
	{"game:stop_hud_motion", CallClass::View},
	{"game:stop_tutorial", CallClass::View},
	{"game:time", CallClass::Read},
	{"game:translate_string", CallClass::Read},
	{"game:ui2world", CallClass::Read},
	{"game:ui2world_offscreen", CallClass::Read},
	{"game:update_pda_news_from_uiwindow", CallClass::View},
	{"game:world2ui", CallClass::Read},
	{"game:world2ui_with_depth", CallClass::Read},
	// --- module hud_adjust (level_script.cpp): 4
	{"hud_adjust:enabled", CallClass::Read},
	{"hud_adjust:remove_hud_model", CallClass::View},
	{"hud_adjust:set_value", CallClass::View},
	{"hud_adjust:set_vector", CallClass::View},
	// --- module relation_registry (level_script.cpp): 6
	{"relation_registry:change_community_goodwill", CallClass::World}, // story Relation path (relation_registry.cpp)
	{"relation_registry:community_goodwill", CallClass::Read},
	{"relation_registry:community_relation", CallClass::Read},
	{"relation_registry:get_general_goodwill_between", CallClass::Read},
	{"relation_registry:set_community_goodwill", CallClass::World}, // story Relation path (relation_registry.cpp)
	{"relation_registry:set_community_relation", CallClass::World}, // story Relation path (relation_registry.cpp)
	// --- module weather (level_script.cpp): 12
	{"weather:boost_reset", CallClass::View}, // the look of this machine's current weather (perk_based_artefacts)
	{"weather:boost_value", CallClass::View}, // the look of this machine's current weather (perk_based_artefacts)
	{"weather:get_value_numric", CallClass::Read},
	{"weather:get_value_string", CallClass::Read},
	{"weather:get_value_vector", CallClass::Read},
	{"weather:is_paused", CallClass::Read},
	{"weather:pause", CallClass::View}, // the look of this machine's current weather (perk_based_artefacts)
	{"weather:reload", CallClass::View}, // the look of this machine's current weather (perk_based_artefacts)
	{"weather:set_value_numric", CallClass::View}, // the look of this machine's current weather (perk_based_artefacts)
	{"weather:set_value_string", CallClass::View}, // the look of this machine's current weather (perk_based_artefacts)
	{"weather:set_value_vector", CallClass::View}, // the look of this machine's current weather (perk_based_artefacts)
	{"weather:sun_time", CallClass::Read},
	// --- global functions: 10
	{"_G:alife", CallClass::Read},
	{"_G:buy_condition", CallClass::View}, // the trade window's prices on this machine
	{"_G:command_line", CallClass::Read},
	{"_G:get_console", CallClass::Read},
	{"_G:IsDynamicMusic", CallClass::Read},
	{"_G:IsGameTypeSingle", CallClass::Read},
	{"_G:IsImportantSave", CallClass::Read},
	{"_G:render_get_dx_level", CallClass::Read},
	{"_G:sell_condition", CallClass::View}, // the trade window's prices on this machine
	{"_G:show_condition", CallClass::View}, // the trade window's prices on this machine
	// --- class alife_object_iterator: 2
	{"alife_object_iterator:__call", CallClass::Read},
	{"alife_object_iterator:next", CallClass::Read},
	// --- class alife_object_without_actor_iterator: 2
	{"alife_object_without_actor_iterator:__call", CallClass::Read},
	{"alife_object_without_actor_iterator:next", CallClass::Read},
	// --- class CCar (CarScript.cpp): 61
	{"CCar:Action", CallClass::World}, // a car: the host drives cars
	{"CCar:CanHit", CallClass::Read},
	{"CCar:CarExplode", CallClass::World}, // a car: the host drives cars
	{"CCar:CCarDrone_GetPowerEfficiency", CallClass::Read},
	{"CCar:CCarDrone_SetPowerEfficiency", CallClass::World}, // a car: the host drives cars
	{"CCar:ChangefFuel", CallClass::World}, // a car: the host drives cars
	{"CCar:ChangefHealth", CallClass::World}, // a car: the host drives cars
	{"CCar:ClearTargetSpeed", CallClass::World}, // a car: the host drives cars
	{"CCar:CurrentTransmission", CallClass::Read},
	{"CCar:CurrentVel", CallClass::Read},
	{"CCar:DoorClose", CallClass::World}, // a car: the host drives cars
	{"CCar:DoorOpen", CallClass::World}, // a car: the host drives cars
	{"CCar:DoorSwitch", CallClass::World}, // a car: the host drives cars
	{"CCar:DoorUse", CallClass::World}, // a car: the host drives cars
	{"CCar:EngineCurTorque", CallClass::Read},
	{"CCar:EnginePower", CallClass::Read},
	{"CCar:ExplodeTime", CallClass::Read},
	{"CCar:FireDirDiff", CallClass::Read},
	{"CCar:GetfFuel", CallClass::Read},
	{"CCar:GetfFuelConsumption", CallClass::Read},
	{"CCar:GetfFuelTank", CallClass::Read},
	{"CCar:GetfHealth", CallClass::Read},
	{"CCar:GetRPM", CallClass::Read},
	{"CCar:GetScopeActive", CallClass::Read},
	{"CCar:GetScopeSize", CallClass::Read},
	{"CCar:GetSpeed", CallClass::Read},
	{"CCar:GetViewportNear", CallClass::Read},
	{"CCar:GetWheelFriction", CallClass::Read},
	{"CCar:GetZoomFactor", CallClass::Read},
	{"CCar:HandBreak", CallClass::World}, // a car: the host drives cars
	{"CCar:HasWeapon", CallClass::Read},
	{"CCar:IsActiveEngine", CallClass::Read},
	{"CCar:IsDoor", CallClass::Read},
	{"CCar:IsObjectVisible", CallClass::Read},
	{"CCar:IsScopeEnable", CallClass::Read},
	{"CCar:PlayDamageParticles", CallClass::View},
	{"CCar:RefWheelMaxSpeed", CallClass::Read},
	{"CCar:ReleaseHandBreak", CallClass::World}, // a car: the host drives cars
	{"CCar:SetExplodeTime", CallClass::World}, // a car: the host drives cars
	{"CCar:SetfFuel", CallClass::World}, // a car: the host drives cars
	{"CCar:SetfFuelConsumption", CallClass::World}, // a car: the host drives cars
	{"CCar:SetfFuelTank", CallClass::World}, // a car: the host drives cars
	{"CCar:SetfHealth", CallClass::World}, // a car: the host drives cars
	{"CCar:SetParam", CallClass::World}, // a car: the host drives cars
	{"CCar:SetRPM", CallClass::World}, // a car: the host drives cars
	{"CCar:SetScopeActive", CallClass::View},
	{"CCar:SetSteer", CallClass::World}, // a car: the host drives cars
	{"CCar:SetTargetSpeed", CallClass::World}, // a car: the host drives cars
	{"CCar:SetThrottle", CallClass::World}, // a car: the host drives cars
	{"CCar:SetUseAction", CallClass::World}, // a car: the host drives cars
	{"CCar:SetViewportNear", CallClass::View},
	{"CCar:SetWheelFriction", CallClass::World}, // a car: the host drives cars
	{"CCar:StartEngine", CallClass::World}, // a car: the host drives cars
	{"CCar:StopDamageParticles", CallClass::View},
	{"CCar:StopEngine", CallClass::World}, // a car: the host drives cars
	{"CCar:Transmission", CallClass::World}, // a car: the host drives cars
	{"CCar:TransmissionDown", CallClass::World}, // a car: the host drives cars
	{"CCar:TransmissionUp", CallClass::World}, // a car: the host drives cars
	{"CCar:VisualCamera_GetDesireAngle", CallClass::Read},
	{"CCar:VisualCamera_GetRotXCur", CallClass::Read},
	{"CCar:VisualCamera_GetRotYCur", CallClass::Read},
	// --- class CConsole (console_registrator_script.cpp): 12
	{"CConsole:execute", CallClass::World}, // a console command: mostly this machine's settings, some are the world's (g_game_difficulty ...)
	{"CConsole:execute_deferred", CallClass::World}, // a console command: mostly this machine's settings, some are the world's (g_game_difficulty ...)
	{"CConsole:execute_script", CallClass::World}, // a console command: mostly this machine's settings, some are the world's (g_game_difficulty ...)
	{"CConsole:get_bool", CallClass::Read},
	{"CConsole:get_float", CallClass::Read},
	{"CConsole:get_integer", CallClass::Read},
	{"CConsole:get_string", CallClass::Read},
	{"CConsole:get_token", CallClass::Read},
	{"CConsole:get_token_list", CallClass::Read},
	{"CConsole:get_variable_bounds", CallClass::Read},
	{"CConsole:hide", CallClass::View},
	{"CConsole:show", CallClass::View},
	// --- class CEnvDescriptor: 2
	{"CEnvDescriptor:far_plane", CallClass::Read},
	{"CEnvDescriptor:fog_density", CallClass::Read},
	// --- class CEnvironment: 1
	{"CEnvironment:current", CallClass::Read},
	// --- class CHelicopter (helicopter_script.cpp): 45
	{"CHelicopter:ClearEnemy", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:Die", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:Explode", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:GetBodyState", CallClass::Read},
	{"CHelicopter:GetCurrVelocity", CallClass::Read},
	{"CHelicopter:GetCurrVelocityVec", CallClass::Read},
	{"CHelicopter:GetDistanceToDestPosition", CallClass::Read},
	{"CHelicopter:GetfHealth", CallClass::Read},
	{"CHelicopter:GetHuntState", CallClass::Read},
	{"CHelicopter:GetMaxVelocity", CallClass::Read},
	{"CHelicopter:GetMovementState", CallClass::Read},
	{"CHelicopter:GetOnPointRangeDist", CallClass::Read},
	{"CHelicopter:GetRealAltitude", CallClass::Read},
	{"CHelicopter:GetSafeAltitude", CallClass::Read},
	{"CHelicopter:GetSpeedInDestPoint", CallClass::Read},
	{"CHelicopter:GetState", CallClass::Read},
	{"CHelicopter:GoPatrolByPatrolPath", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:GoPatrolByRoundPath", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:isVisible", CallClass::Read},
	{"CHelicopter:LookAtPoint", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:m_dead", CallClass::Read},
	{"CHelicopter:m_exploded", CallClass::Read},
	{"CHelicopter:m_flame_started", CallClass::Read},
	{"CHelicopter:m_light_started", CallClass::Read},
	{"CHelicopter:m_max_mgun_dist", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:m_max_rocket_dist", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:m_min_mgun_dist", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:m_min_rocket_dist", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:m_syncronize_rocket", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:m_time_between_rocket_attack", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:m_use_mgun_on_attack", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:m_use_rocket_on_attack", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetBarrelDirTolerance", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetDestPosition", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetEnemy", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetfHealth", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetFireTrailLength", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetLinearAcc", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetMaxVelocity", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetOnPointRangeDist", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:SetSpeedInDestPoint", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:StartFlame", CallClass::World}, // a helicopter: the host's AI flies it
	{"CHelicopter:TurnEngineSound", CallClass::View},
	{"CHelicopter:TurnLighting", CallClass::View},
	{"CHelicopter:UseFireTrail", CallClass::World}, // a helicopter: the host's AI flies it
	// --- class CPhysicObject (PhysicObject_script.cpp): 9
	{"CPhysicObject:anim_time_get", CallClass::Read},
	{"CPhysicObject:anim_time_set", CallClass::World}, // a door or an animated object of the world
	{"CPhysicObject:play_bones_sound", CallClass::View},
	{"CPhysicObject:run_anim_back", CallClass::World}, // a door or an animated object of the world
	{"CPhysicObject:run_anim_forward", CallClass::World}, // a door or an animated object of the world
	{"CPhysicObject:set_door_ignore_dynamics", CallClass::World}, // a door or an animated object of the world
	{"CPhysicObject:stop_anim", CallClass::World}, // a door or an animated object of the world
	{"CPhysicObject:stop_bones_sound", CallClass::View},
	{"CPhysicObject:unset_door_ignore_dynamics", CallClass::World}, // a door or an animated object of the world
	// --- server object cse_abstract (xrServerEntities): 13
	{"cse_abstract:angle", CallClass::World}, // a server object's state
	{"cse_abstract:clsid", CallClass::Read},
	{"cse_abstract:id", CallClass::Read},
	{"cse_abstract:name", CallClass::Read},
	{"cse_abstract:parent_id", CallClass::Read},
	{"cse_abstract:position", CallClass::World}, // a server object's state
	{"cse_abstract:script_version", CallClass::Read},
	{"cse_abstract:section_name", CallClass::Read},
	{"cse_abstract:spawn_ini", CallClass::Read},
	{"cse_abstract:STATE_Read", CallClass::World}, // a server object's state
	{"cse_abstract:STATE_Write", CallClass::Read},
	{"cse_abstract:UPDATE_Read", CallClass::World}, // a server object's state
	{"cse_abstract:UPDATE_Write", CallClass::Read},
	// --- server object CSE_AbstractVisual (xrServerEntities): 1
	{"CSE_AbstractVisual:getStartupAnimation", CallClass::Read},
	// --- server object cse_alife_creature_abstract (xrServerEntities): 6
	{"cse_alife_creature_abstract:alive", CallClass::Read},
	{"cse_alife_creature_abstract:group", CallClass::World}, // a server object's state
	{"cse_alife_creature_abstract:health", CallClass::Read},
	{"cse_alife_creature_abstract:o_torso", CallClass::Read},
	{"cse_alife_creature_abstract:squad", CallClass::World}, // a server object's state
	{"cse_alife_creature_abstract:team", CallClass::World}, // a server object's state
	// --- server object cse_alife_human_abstract (xrServerEntities): 3
	{"cse_alife_human_abstract:brain", CallClass::Read},
	{"cse_alife_human_abstract:rank", CallClass::Read},
	{"cse_alife_human_abstract:set_rank", CallClass::World}, // a server object's state
	// --- server object cse_alife_inventory_item (xrServerEntities): 2
	{"cse_alife_inventory_item:add_upgrade", CallClass::World}, // a server object's state
	{"cse_alife_inventory_item:has_upgrade", CallClass::Read},
	// --- server object cse_alife_item_weapon (xrServerEntities): 5
	{"cse_alife_item_weapon:clone_addons", CallClass::World}, // a server object's state
	{"cse_alife_item_weapon:clone_upgrades", CallClass::World}, // a server object's state
	{"cse_alife_item_weapon:get_ammo_elapsed", CallClass::Read},
	{"cse_alife_item_weapon:get_ammo_magsize", CallClass::Read},
	{"cse_alife_item_weapon:set_ammo_elapsed", CallClass::World}, // a server object's state
	// --- server object cse_alife_level_changer (xrServerEntities): 3
	{"cse_alife_level_changer:dest_game_vertex_id", CallClass::Read},
	{"cse_alife_level_changer:dest_level_vertex_id", CallClass::Read},
	{"cse_alife_level_changer:dest_position", CallClass::Read},
	// --- server object cse_alife_monster_abstract (xrServerEntities): 13
	{"cse_alife_monster_abstract:brain", CallClass::Read},
	{"cse_alife_monster_abstract:clear_smart_terrain", CallClass::World}, // a server object's state
	{"cse_alife_monster_abstract:current_level_travel_speed", CallClass::World}, // getter and setter share the name; the setter changes a server object
	{"cse_alife_monster_abstract:force_set_goodwill", CallClass::World}, // a server object's state
	{"cse_alife_monster_abstract:group_id", CallClass::Read},
	{"cse_alife_monster_abstract:has_detector", CallClass::Read},
	{"cse_alife_monster_abstract:kill", CallClass::World}, // a server object's state
	{"cse_alife_monster_abstract:m_smart_terrain_id", CallClass::World}, // a server object's state
	{"cse_alife_monster_abstract:rank", CallClass::Read},
	{"cse_alife_monster_abstract:smart_terrain_id", CallClass::Read},
	{"cse_alife_monster_abstract:smart_terrain_task_activate", CallClass::World}, // a server object's state
	{"cse_alife_monster_abstract:smart_terrain_task_deactivate", CallClass::World}, // a server object's state
	{"cse_alife_monster_abstract:travel_speed", CallClass::World}, // getter and setter share the name; the setter changes a server object
	// --- server object cse_alife_object (xrServerEntities): 9
	{"cse_alife_object:can_switch_offline", CallClass::World}, // getter and setter share the name; the setter changes a server object
	{"cse_alife_object:can_switch_online", CallClass::World}, // getter and setter share the name; the setter changes a server object
	{"cse_alife_object:m_game_vertex_id", CallClass::Read},
	{"cse_alife_object:m_level_vertex_id", CallClass::Read},
	{"cse_alife_object:m_story_id", CallClass::Read},
	{"cse_alife_object:move_offline", CallClass::World}, // getter and setter share the name; the setter changes a server object
	{"cse_alife_object:online", CallClass::Read},
	{"cse_alife_object:use_ai_locations", CallClass::World}, // a server object's state
	{"cse_alife_object:visible_for_map", CallClass::World}, // getter and setter share the name; the setter changes a server object
	// --- server object cse_alife_object_physic (xrServerEntities): 1
	{"cse_alife_object_physic:set_yaw", CallClass::World}, // a server object's state
	// --- server object cse_alife_online_offline_group (xrServerEntities): 8
	{"cse_alife_online_offline_group:add_location_type", CallClass::World}, // a squad of the host's ALife
	{"cse_alife_online_offline_group:clear_location_types", CallClass::World}, // a squad of the host's ALife
	{"cse_alife_online_offline_group:commander_id", CallClass::Read},
	{"cse_alife_online_offline_group:force_change_position", CallClass::World}, // a squad of the host's ALife
	{"cse_alife_online_offline_group:npc_count", CallClass::Read},
	{"cse_alife_online_offline_group:register_member", CallClass::World}, // a squad of the host's ALife
	{"cse_alife_online_offline_group:squad_members", CallClass::Read},
	{"cse_alife_online_offline_group:unregister_member", CallClass::World}, // a squad of the host's ALife
	// --- server object cse_alife_stmgun (xrServerEntities): 5
	{"cse_alife_stmgun:get_ammo_elapsed", CallClass::Read},
	{"cse_alife_stmgun:get_ammo_magsize", CallClass::Read},
	{"cse_alife_stmgun:get_ammo_type", CallClass::Read},
	{"cse_alife_stmgun:set_ammo_elapsed", CallClass::World}, // a server object's state
	{"cse_alife_stmgun:set_ammo_type", CallClass::World}, // a server object's state
	// --- server object cse_alife_trader_abstract (xrServerEntities): 10
	{"cse_alife_trader_abstract:character_icon", CallClass::Read},
	{"cse_alife_trader_abstract:character_name", CallClass::Read},
	{"cse_alife_trader_abstract:character_name_str", CallClass::Read},
	{"cse_alife_trader_abstract:community", CallClass::Read},
	{"cse_alife_trader_abstract:profile_name", CallClass::Read},
	{"cse_alife_trader_abstract:rank", CallClass::Read},
	{"cse_alife_trader_abstract:reputation", CallClass::Read},
	{"cse_alife_trader_abstract:set_character_name", CallClass::World}, // a server object's state
	{"cse_alife_trader_abstract:set_profile_name", CallClass::World}, // a server object's state
	{"cse_alife_trader_abstract:set_rank", CallClass::World}, // a server object's state
	// --- server object cse_smart_cover (xrServerEntities): 2
	{"cse_smart_cover:description", CallClass::Read},
	{"cse_smart_cover:set_available_loopholes", CallClass::World}, // a server object's state
	// --- class CSightParams: 3
	{"CSightParams:m_object", CallClass::Read},
	{"CSightParams:m_sight_type", CallClass::Read},
	{"CSightParams:m_vector", CallClass::Read},
	// --- class CTime: 9
	{"CTime:add", CallClass::Read}, // a time value of the script
	{"CTime:dateToString", CallClass::Read}, // a time value of the script
	{"CTime:diffSec", CallClass::Read}, // a time value of the script
	{"CTime:get", CallClass::Read}, // a time value of the script
	{"CTime:set", CallClass::Read}, // a time value of the script
	{"CTime:setHMS", CallClass::Read}, // a time value of the script
	{"CTime:setHMSms", CallClass::Read}, // a time value of the script
	{"CTime:sub", CallClass::Read}, // a time value of the script
	{"CTime:timeToString", CallClass::Read}, // a time value of the script
	// --- class CZoneCampfire (MosquitoBald_script.cpp): 3
	{"CZoneCampfire:is_on", CallClass::Read},
	{"CZoneCampfire:turn_off", CallClass::World}, // a campfire of the world
	{"CZoneCampfire:turn_on", CallClass::World}, // a campfire of the world
	// --- class DBG_ScriptBox: 2
	{"DBG_ScriptBox:matrix", CallClass::View}, // a debug shape of this machine
	{"DBG_ScriptBox:size", CallClass::View}, // a debug shape of this machine
	// --- class DBG_ScriptLine: 2
	{"DBG_ScriptLine:point_a", CallClass::View}, // a debug shape of this machine
	{"DBG_ScriptLine:point_b", CallClass::View}, // a debug shape of this machine
	// --- class DBG_ScriptObject: 6
	{"DBG_ScriptObject:cast_dbg_box", CallClass::Read},
	{"DBG_ScriptObject:cast_dbg_line", CallClass::Read},
	{"DBG_ScriptObject:cast_dbg_sphere", CallClass::Read},
	{"DBG_ScriptObject:color", CallClass::View}, // a debug shape of this machine
	{"DBG_ScriptObject:hud", CallClass::View}, // a debug shape of this machine
	{"DBG_ScriptObject:visible", CallClass::View}, // a debug shape of this machine
	// --- class DBG_ScriptSphere: 1
	{"DBG_ScriptSphere:matrix", CallClass::View}, // a debug shape of this machine
	// --- class hanging_lamp (HangingLamp.cpp): 6
	{"hanging_lamp:is_flickering", CallClass::Read},
	{"hanging_lamp:is_on", CallClass::Read},
	{"hanging_lamp:reset_color_animator", CallClass::View},
	{"hanging_lamp:set_color_animator", CallClass::View},
	{"hanging_lamp:turn_off", CallClass::World}, // a lamp of the world
	{"hanging_lamp:turn_on", CallClass::World}, // a lamp of the world
	// --- class MEMBERS__value_type: 2
	{"MEMBERS__value_type:id", CallClass::Read},
	{"MEMBERS__value_type:object", CallClass::Read},
	// --- class ray_pick: 11
	{"ray_pick:get_distance", CallClass::Read}, // a ray query of the script
	{"ray_pick:get_element", CallClass::Read}, // a ray query of the script
	{"ray_pick:get_normal", CallClass::Read}, // a ray query of the script
	{"ray_pick:get_object", CallClass::Read}, // a ray query of the script
	{"ray_pick:get_result", CallClass::Read}, // a ray query of the script
	{"ray_pick:query", CallClass::Read}, // a ray query of the script
	{"ray_pick:set_direction", CallClass::Read}, // a ray query of the script
	{"ray_pick:set_flags", CallClass::Read}, // a ray query of the script
	{"ray_pick:set_ignore_object", CallClass::Read}, // a ray query of the script
	{"ray_pick:set_position", CallClass::Read}, // a ray query of the script
	{"ray_pick:set_range", CallClass::Read}, // a ray query of the script
	// --- class rq_result: 18
	{"rq_result:element", CallClass::Read},
	{"rq_result:material_bounce_damage_factor", CallClass::Read},
	{"rq_result:material_density_factor", CallClass::Read},
	{"rq_result:material_flags", CallClass::Read},
	{"rq_result:material_flotation_factor", CallClass::Read},
	{"rq_result:material_injurious_speed", CallClass::Read},
	{"rq_result:material_name", CallClass::Read},
	{"rq_result:material_phbounce_start_velocity", CallClass::Read},
	{"rq_result:material_phbouncing", CallClass::Read},
	{"rq_result:material_phdamping", CallClass::Read},
	{"rq_result:material_phfriction", CallClass::Read},
	{"rq_result:material_phspring", CallClass::Read},
	{"rq_result:material_shoot_factor", CallClass::Read},
	{"rq_result:material_shoot_factor_mp", CallClass::Read},
	{"rq_result:material_snd_occlusion_factor", CallClass::Read},
	{"rq_result:material_vis_transparency_factor", CallClass::Read},
	{"rq_result:object", CallClass::Read},
	{"rq_result:range", CallClass::Read},
};

inline size_t call_count() { return sizeof(kCalls) / sizeof(kCalls[0]); }

// The virtual methods every server object class gets from its luabind_class_* macro (xrServer_script_macroses.h,
// XRGAME_EXPORTS): a row for each would repeat ~860 times. They apply to cse_* namespaces only (list_exports.py checks
// that every luabind_class_* class is one); an exact kCalls row of the same name wins (cse_alife_object's
// can_switch_online has a setter overload). Engine hooks a script server object overrides; called from Lua they are
// its base class's code.
inline constexpr std::string_view kMacroPrefix = "cse_";
inline constexpr Call kMacroVirtuals[] = {
	{"bfUseful", CallClass::Read},
	{"can_save", CallClass::Read},
	{"can_switch_offline", CallClass::Read},
	{"can_switch_online", CallClass::Read},
	{"detect_probability", CallClass::Read},
	{"enabled", CallClass::Read},
	{"g_group", CallClass::Read},
	{"g_squad", CallClass::Read},
	{"g_team", CallClass::Read},
	{"get_current_task", CallClass::Read},
	{"init", CallClass::World}, // initializes the server object
	{"interactive", CallClass::Read},
	{"keep_saved_data_anyway", CallClass::Read},
	{"on_before_register", CallClass::World}, // ALife registration hooks of the host
	{"on_death", CallClass::World},
	{"on_register", CallClass::World},
	{"on_spawn", CallClass::World},
	{"on_unregister", CallClass::World},
	{"register_npc", CallClass::World}, // a smart terrain's population
	{"smart_touch", CallClass::World},
	{"STATE_Read", CallClass::World}, // reads a packet into the server object
	{"STATE_Write", CallClass::Read},  // writes the server object into a packet
	{"suitable", CallClass::Read},
	{"switch_offline", CallClass::World},
	{"switch_online", CallClass::World},
	{"task", CallClass::Read},
	{"unregister_npc", CallClass::World},
	{"update", CallClass::World},
	{"used_ai_locations", CallClass::Read},
};

// Client calls the engine sends to the host instead of running them (MOD_LAYER §2.3). own_only: only for a target
// the client owns (its actor, an item under it); another's target has no path.
struct RequestPath
{
	std::string_view name;
	bool own_only;
};

inline constexpr RequestPath kRequestPaths[] = {
	// ReqSpawn (coop_requests.cpp lua_create / lua_clone / lua_register_spawn, the event router's filter_spawn): an item
	// into our own inventory only (client_forward_spawn); own = the parent is our actor or under it (own_spawn).
	{"alife:create", true},
	{"alife:create_ammo", true},
	{"alife:clone_weapon", true},
	{"alife:register", true}, // a temporary object of create(..., false) / clone_weapon(..., false)
	{"level:spawn_item", true},
	{"alife:release", true},               // client_destroy of an own item (lua_release, own_release)
	{"game_object:destroy_object", true}, // the event router's on_destroy (own_destroy)
	// story (inventory_owner_info.cpp -> coop_story on_info): our actor's infos only (owner id 0); an NPC's info stays
	// in this machine's mirror, counted as requested
	{"game_object:give_info_portion", false},
	{"game_object:disable_info_portion", false},
	{"alife:give_info", true}, // id 0 = our actor: story::on_info; another's registry is the host's
	{"alife:disable_info", true},
	// story Relation (relation_registry.cpp -> coop_story): a relation with a player; between NPCs it stays in this
	// machine's mirror, counted as requested
	{"game_object:set_relation", false},
	{"game_object:set_goodwill", false},
	{"game_object:force_set_goodwill", false},
	{"game_object:change_goodwill", false},
	{"game_object:set_community_goodwill", false},
	{"relation_registry:set_community_goodwill", false},
	{"relation_registry:change_community_goodwill", false},
	{"relation_registry:set_community_relation", false},
	// story tasks (GameTaskManager / GameTask hooks: on_give_task, on_task_state; personal tasks stay local)
	{"game_object:give_task", false},
	{"game_object:set_task_state", false},
	{"game_object:transfer_item", true}, // ReqTransfer / ReqTake / ReqDrop (coop_events.cpp); own: own_move
	{"game_object:take_item", true},     // the same
	{"game_object:give_money", true},    // our actor: the money journal (GE_MONEY -> money::client_on_actor_money)
	{"game_object:transfer_money", true}, // our actor pays: its side goes to the journal, the receiver's is the host's
	{"level:change_game_time", false},    // TimeRequest (coop_worldstate.cpp intercept_time_jump)
};

namespace detail
{
// kCalls by lowercase name, built once.
inline const std::unordered_map<std::string, CallClass>& table()
{
	static const std::unordered_map<std::string, CallClass> t = []
	{
		std::unordered_map<std::string, CallClass> m;
		m.reserve(call_count() * 2);
		for (const Call& c : kCalls)
			m.emplace(lowered(c.name), c.cls);
		return m;
	}();
	return t;
}

// The class kMacroVirtuals gives `name` (a cse_* namespace and a macro virtual), false if none.
inline bool macro_virtual(std::string_view name, CallClass& out)
{
	const size_t colon = name.find(':');
	if (colon == std::string_view::npos || !istarts_with(name.substr(0, colon), kMacroPrefix))
		return false;
	const std::string_view fn = name.substr(colon + 1);
	for (const Call& v : kMacroVirtuals)
		if (iequals(v.name, fn))
		{
			out = v.cls;
			return true;
		}
	return false;
}
} // namespace detail

// Whether the table classifies `name`: a kCalls row, or a server object's macro virtual.
inline bool has_row(std::string_view name)
{
	if (detail::table().count(detail::lowered(name)) != 0)
		return true;
	CallClass unused = CallClass::Read;
	return detail::macro_virtual(name, unused);
}

// The built-in class of a function: its row, else its macro virtual, else read.
inline CallClass class_of(std::string_view name)
{
	const auto& t = detail::table();
	const auto it = t.find(detail::lowered(name));
	if (it != t.end())
		return it->second;
	CallClass c = CallClass::Read;
	detail::macro_virtual(name, c);
	return c;
}

// Whether the engine has a request path for this call on a client, for a target it owns or not.
inline bool has_request_path(std::string_view name, bool own_target)
{
	for (const RequestPath& p : kRequestPaths)
		if (detail::iequals(p.name, name))
			return own_target || !p.own_only;
	return false;
}

// The class matrix (MOD_LAYER §2.3):
//   host (or no session)  run everything;
//   client  read, view    run;
//           owner         run on an own target; on another's target as world;
//           world         a request where the engine has a path, else refused.
inline Gate gate(CallClass cls, bool is_client, bool own_target, bool has_request_path)
{
	if (!is_client)
		return Gate::Run;
	switch (cls)
	{
	case CallClass::Read:
	case CallClass::View: return Gate::Run;
	case CallClass::Owner:
		if (own_target)
			return Gate::Run;
		break;
	case CallClass::World: break;
	}
	return has_request_path ? Gate::Request : Gate::Refuse;
}

// The whole decision of the engine's gate (coop_callgate.cpp):
//   not a client in the host's world   run (the host and single player run everything, bad arguments included);
//   bad arguments (NaN, a missing id)   refused, even while applying: a bad number neither runs nor goes to the host;
//   applying a host command             run: the engine applies the host's change, our Lua mirrors call world functions
//                                       to show it (the event router still drops echoes, R4 4);
//   else                                the class matrix (gate).
inline Gate decide(CallClass cls, bool is_client, bool applying, bool own_target, bool has_request_path, bool args_ok)
{
	if (!is_client)
		return Gate::Run;
	if (!args_ok)
		return Gate::Refuse;
	if (applying)
		return Gate::Run;
	return gate(cls, true, own_target, has_request_path);
}

// The "own target" of the gated call sites, the same as the engine paths behind them:
// transfer_item / take_item: our actor is a side of the move, the item is ours or it comes to us (Ruling F2-30; the
// host checks owner, reach and the sides' locks in host_move).
inline bool own_move(bool item_ours, bool other_side_ours) { return item_ours || other_side_ours; }
// alife:create / create_ammo / clone_weapon / register, level:spawn_item: registered now only into our own inventory
// (client_forward_spawn); a temporary object (register = false) passes, its alife():register is gated, so Lua keeps
// getting the temporary object it got before.
inline bool own_spawn(bool registers_now, bool parent_ours) { return !registers_now || parent_ours; }
// alife:release: an item under our actor, never our actor itself.
inline bool own_release(bool is_our_actor, bool ours) { return ours && !is_our_actor; }
// destroy_object: as the event router's on_destroy: ours and free items (no parent, not a creature) go to the host
// (client_destroy), an object outside the host's world is local; anything else is the host's.
inline bool own_destroy(bool in_host_world, bool ours, bool free_item) { return !in_host_world || ours || free_item; }

// One [call_classes] line.
struct Override
{
	std::string name;
	CallClass cls = CallClass::Read;
};

// Parses the `name = class` lines of a [call_classes] section. A bad line is skipped and reported in `errors` (may be
// null) as "call_classes: <name> = <value>: <why>"; the other lines still parse. A name the table does not know is
// accepted (a modded exe's export).
inline std::vector<Override> parse_overrides(const std::vector<std::pair<std::string, std::string>>& lines,
	std::vector<std::string>* errors)
{
	std::vector<Override> out;
	for (const std::pair<std::string, std::string>& line : lines)
	{
		const std::string_view key = detail::trimmed(line.first);
		const std::string_view value = detail::trimmed(line.second);
		Override o;
		const char* why = nullptr;
		if (!detail::valid_name(key))
			why = "expected namespace:function (letters, digits, _)";
		else if (!parse_class(value, o.cls))
			why = "unknown class (world, owner, view, read)";
		if (why)
		{
			if (errors)
				errors->push_back("call_classes: " + std::string(key) + " = " + std::string(value) + ": " + why);
			continue;
		}
		o.name = std::string(key);
		out.push_back(std::move(o));
	}
	return out;
}

// kCalls with the ltx overrides on top; a later override of a name replaces the earlier one.
class Classes
{
public:
	void add(const Override& o)
	{
		if (detail::valid_name(o.name))
			m_overrides[detail::lowered(o.name)] = o.cls;
	}
	void clear() { m_overrides.clear(); }
	size_t size() const { return m_overrides.size(); }
	bool overridden(std::string_view name) const { return m_overrides.count(detail::lowered(name)) != 0; }

	CallClass class_of(std::string_view name) const
	{
		if (!m_overrides.empty())
		{
			const auto it = m_overrides.find(detail::lowered(name));
			if (it != m_overrides.end())
				return it->second;
		}
		return calls::class_of(name);
	}

private:
	std::map<std::string, CallClass> m_overrides; // lowercase name
};

// Gated calls on this machine for coop_status: totals, refusals per name for "the most refused" list, and the log
// budget: the first kLoggedPerName refusals of each name get a log line (review M7: one noisy name, the weather of
// level_weathers, must not hide the others), the rest are only counted. The per-name table is bounded: a name past
// kMaxNames counts in the totals only and is not logged.
class Counters
{
public:
	static constexpr size_t kMaxNames = 512;
	static constexpr size_t kLoggedPerName = 10;

	struct Totals
	{
		uint64_t ran = 0, requested = 0, refused = 0;
	};

	void note(std::string_view name, Gate g)
	{
		switch (g)
		{
		case Gate::Run: ++m_totals.ran; break;
		case Gate::Request: ++m_totals.requested; break;
		case Gate::Refuse:
			++m_totals.refused;
			if (const auto it = m_refused.find(name); it != m_refused.end())
				++it->second.refused;
			else if (m_refused.size() < kMaxNames)
				m_refused.emplace(std::string(name), Name{1, 0});
			else
				++m_dropped;
			break;
		}
	}

	const Totals& totals() const { return m_totals; }

	// The names with the most refusals, most first (a tie by name), at most n.
	std::vector<std::pair<std::string, uint64_t>> top_refused(size_t n) const
	{
		std::vector<std::pair<std::string, uint64_t>> all;
		all.reserve(m_refused.size());
		for (const auto& [name, counts] : m_refused)
			all.emplace_back(name, counts.refused);
		const auto more = [](const std::pair<std::string, uint64_t>& a, const std::pair<std::string, uint64_t>& b)
		{ return a.second != b.second ? a.second > b.second : a.first < b.first; };
		const size_t k = n < all.size() ? n : all.size();
		std::partial_sort(all.begin(), all.begin() + std::ptrdiff_t(k), all.end(), more);
		all.resize(k);
		return all;
	}

	// After note(name, Refuse): true for the first kLoggedPerName refusals of `name` since clear(), that refusal gets
	// its own log line. False for a name the table does not track (no refusal noted, or past kMaxNames).
	bool take_log_line(std::string_view name)
	{
		const auto it = m_refused.find(name);
		if (it == m_refused.end() || it->second.logged >= kLoggedPerName)
			return false;
		++it->second.logged;
		return true;
	}

	// Log lines taken for `name` since clear().
	size_t logged(std::string_view name) const
	{
		const auto it = m_refused.find(name);
		return it == m_refused.end() ? 0 : it->second.logged;
	}

	size_t names_tracked() const { return m_refused.size(); }
	uint64_t names_dropped() const { return m_dropped; }

	void clear()
	{
		m_totals = Totals();
		m_refused.clear();
		m_dropped = 0;
	}

private:
	struct Name
	{
		uint64_t refused = 0;
		size_t logged = 0;
	};

	Totals m_totals;
	std::map<std::string, Name, std::less<>> m_refused; // std::less<>: find() by string_view
	uint64_t m_dropped = 0;
};
} // namespace calls
} // namespace coop
