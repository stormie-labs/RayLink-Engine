// Anomaly Coop - call router table (MOD_LAYER.md §2.3, F2 T12). Every engine function a Lua script can call has a class
// that decides what a call does on a client:
//   world - changes the world everybody shares (spawn, release, NPCs and their AI, story, relations, NPC money, weather,
//           time). The host runs it; a client sends it to the host where the engine has a request path (ReqSpawn,
//           story, ReqTransfer, the money journal, TimeRequest) and refuses it otherwise.
//   owner - changes what one player owns: the own actor (health, needs, boosters, slots, movement) and own items. Runs
//           on the owner's machine; on another's object it is a world call.
//   view  - picture, sound and UI of this machine (HUD, effectors, particles, map spots, script plumbing). Runs locally.
//   read  - reads (positions, alife():object, time, info portions). Runs locally: on a client against the mirror.
// A name is "<namespace>:<function>": alife:<f> (methods of alife()), game_object:<f>, and <module>:<f> for the named
// modules of level_script.cpp (level, actor_stats, debug_render, game, hud_adjust, relation_registry, weather). A name
// the table does not know is read. Letter case does not matter (an ltx reader may lowercase keys).
//
// kCalls lists every export of those sources, one row each (overloads share a row). tools/list_exports.py writes the
// export list (tests/lua_exports.txt) and --check finds a row for no export (a typo) and an export without a row;
// tests/test_calls.cpp checks the list against kCalls. A function whose class is a guess is world (the safe side: a
// client asks the host or does nothing) and its row says `// unsure:` with the reason.
//
// Overrides: [call_classes] `namespace:function = world|owner|view|read` in configs\coop_route*.ltx and
// coop_compat_*.ltx (parse_overrides, Classes). An override can move a function to any class; the request paths stay
// the engine's (kRequestPaths).
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
	{"alife:add_in_restriction", CallClass::World},
	{"alife:add_out_restriction", CallClass::World},
	{"alife:clone_weapon", CallClass::World}, // ReqSpawn into our own inventory (lua_clone)
	{"alife:create", CallClass::World}, // ReqSpawn into our own inventory (coop_requests.cpp lua_create)
	{"alife:create_ammo", CallClass::World}, // ReqSpawn into our own inventory (lua_create)
	{"alife:disable_info", CallClass::World},
	{"alife:dont_has_info", CallClass::Read},
	{"alife:force_update", CallClass::World}, // unsure: tunes the ALife scheduler, which only the host runs
	{"alife:get_children", CallClass::Read},
	{"alife:give_info", CallClass::World},
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
	{"alife:remove_all_restrictions", CallClass::World},
	{"alife:remove_in_restriction", CallClass::World},
	{"alife:remove_out_restriction", CallClass::World},
	{"alife:set_interactive", CallClass::World},
	{"alife:set_objects_per_update", CallClass::World}, // unsure: tunes the ALife scheduler, which only the host runs
	{"alife:set_process_time", CallClass::World}, // unsure: tunes the ALife scheduler, which only the host runs
	{"alife:set_switch_distance", CallClass::World},
	{"alife:set_switch_offline", CallClass::World}, // the host's ALife; no request path
	{"alife:set_switch_online", CallClass::World}, // the host's ALife; no request path
	{"alife:spawn_id", CallClass::Read},
	{"alife:story_object", CallClass::Read},
	{"alife:switch_distance", CallClass::Read},
	{"alife:teleport_object", CallClass::World}, // the host's ALife; no request path
	{"alife:valid_object_id", CallClass::Read},
	// --- game objects: class game_object (script_game_object_script*.cpp): 646
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
	{"game_object:add_combat_sound", CallClass::World}, // unsure: an NPC's sound set and speech; the host's AI plays them
	{"game_object:add_restrictions", CallClass::World},
	{"game_object:add_sound", CallClass::World}, // unsure: an NPC's sound set and speech; the host's AI plays them
	{"game_object:affect_cover", CallClass::World},
	{"game_object:aim_bone_id", CallClass::World},
	{"game_object:aim_time", CallClass::World},
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
	{"game_object:buy_condition", CallClass::World}, // unsure: trader prices; a client prices its own trade window with them
	{"game_object:buy_item_condition_factor", CallClass::World}, // unsure: trader prices; a client prices its own trade window with them
	{"game_object:buy_item_exponent", CallClass::World}, // unsure: trader prices; a client prices its own trade window with them
	{"game_object:buy_supplies", CallClass::World}, // fills a trader's stock: creates items
	{"game_object:can_be_harmed", CallClass::Read},
	{"game_object:can_kill_enemy", CallClass::Read},
	{"game_object:can_kill_member", CallClass::Read},
	{"game_object:can_script_capture", CallClass::Read},
	{"game_object:can_select_weapon", CallClass::World},
	{"game_object:can_throw_grenades", CallClass::World},
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
	{"game_object:change_health", CallClass::Owner},
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
	{"game_object:death_sound_enabled", CallClass::World},
	{"game_object:death_time", CallClass::Read},
	{"game_object:destroy_object", CallClass::World},
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
	{"game_object:enable_level_changer", CallClass::World}, // unsure: level changers are shared by the group
	{"game_object:enable_memory_object", CallClass::World},
	{"game_object:enable_night_vision", CallClass::Owner},
	{"game_object:enable_talk", CallClass::World},
	{"game_object:enable_torch", CallClass::Owner},
	{"game_object:enable_trade", CallClass::World},
	{"game_object:enable_vision", CallClass::World},
	{"game_object:explode", CallClass::World},
	{"game_object:external_sound_start", CallClass::View},
	{"game_object:external_sound_stop", CallClass::View},
	{"game_object:extrapolate_length", CallClass::World},
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
	{"game_object:give_task", CallClass::World},
	{"game_object:goodwill", CallClass::Read},
	{"game_object:group", CallClass::Read},
	{"game_object:group_throw_time_interval", CallClass::World},
	{"game_object:has_ammo_type", CallClass::Read},
	{"game_object:has_info", CallClass::Read},
	{"game_object:has_upgrade", CallClass::Read},
	{"game_object:head_orientation", CallClass::Read},
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
	{"game_object:idle_max_time", CallClass::World},
	{"game_object:idle_min_time", CallClass::World},
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
	{"game_object:invulnerable", CallClass::Owner},
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
	{"game_object:lookout_max_time", CallClass::World},
	{"game_object:lookout_min_time", CallClass::World},
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
	{"game_object:motivation_action_manager", CallClass::Read},
	{"game_object:move_to_belt", CallClass::Owner},
	{"game_object:move_to_ruck", CallClass::Owner},
	{"game_object:move_to_slot", CallClass::Owner},
	{"game_object:movement_enabled", CallClass::World},
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
	{"game_object:play_cycle", CallClass::World}, // unsure: a script animation of a host-driven object
	{"game_object:play_hud_motion", CallClass::View},
	{"game_object:play_sound", CallClass::World}, // unsure: an NPC's sound set and speech; the host's AI plays them
	{"game_object:poltergeist_get_actor_ignore", CallClass::Read},
	{"game_object:poltergeist_set_actor_ignore", CallClass::World},
	{"game_object:position", CallClass::Read},
	{"game_object:power_critical", CallClass::Read},
	{"game_object:profile_name", CallClass::Read},
	{"game_object:psy_factor", CallClass::Read},
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
	{"game_object:remove_sound", CallClass::World}, // unsure: an NPC's sound set and speech; the host's AI plays them
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
	{"game_object:script", CallClass::World},
	{"game_object:section", CallClass::Read},
	{"game_object:see", CallClass::Read},
	{"game_object:sell_condition", CallClass::World}, // unsure: trader prices; a client prices its own trade window with them
	{"game_object:sell_item_exponent", CallClass::World}, // unsure: trader prices; a client prices its own trade window with them
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
	{"game_object:set_const_force", CallClass::World}, // unsure: a physics push of a host-simulated object
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
	{"game_object:set_enemy_callback", CallClass::World}, // unsure: steers the host's AI, harmless on a client
	{"game_object:set_fastcall", CallClass::View},
	{"game_object:set_fire_queue_scale", CallClass::World},
	{"game_object:set_force_anti_aim", CallClass::World},
	{"game_object:set_fov", CallClass::World},
	{"game_object:set_goodwill", CallClass::World}, // story Relation path (relation_registry.cpp)
	{"game_object:set_health_ex", CallClass::Owner},
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
	{"game_object:set_idle_particles", CallClass::World}, // unsure: an anomaly's look, maybe a local effect
	{"game_object:set_invisible", CallClass::World},
	{"game_object:set_item", CallClass::World},
	{"game_object:set_level_changer_invitation", CallClass::World}, // unsure: level changers are shared by the group
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
	{"game_object:set_patrol_extrapolate_callback", CallClass::World}, // unsure: steers the host's AI, harmless on a client
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
	{"game_object:set_sound_mask", CallClass::World}, // unsure: an NPC's sound set and speech; the host's AI plays them
	{"game_object:set_sound_threshold", CallClass::World},
	{"game_object:set_spatial_type", CallClass::World}, // unsure: changes how the world collides with the object
	{"game_object:set_start_dialog", CallClass::World},
	{"game_object:set_start_point", CallClass::World},
	{"game_object:set_sympathy", CallClass::World},
	{"game_object:set_task_state", CallClass::World},
	{"game_object:set_tip_text", CallClass::View},
	{"game_object:set_tip_text_default", CallClass::View},
	{"game_object:set_trader_global_anim", CallClass::World}, // unsure: a trader's animation and voice; the host drives the trader
	{"game_object:set_trader_head_anim", CallClass::World}, // unsure: a trader's animation and voice; the host drives the trader
	{"game_object:set_trader_sound", CallClass::World}, // unsure: a trader's animation and voice; the host drives the trader
	{"game_object:set_view_distance_factor", CallClass::World},
	{"game_object:set_vis_state", CallClass::World},
	{"game_object:set_visible_enemy_bias", CallClass::World},
	{"game_object:set_vision_speed", CallClass::World},
	{"game_object:set_visual_memory_enabled", CallClass::World},
	{"game_object:set_visual_name", CallClass::World},
	{"game_object:set_weapon_type", CallClass::Owner},
	{"game_object:set_weight", CallClass::Owner},
	{"game_object:SetBulletCheckVisual", CallClass::World}, // unsure: changes how the world collides with the object
	{"game_object:show_condition", CallClass::World}, // unsure: trader prices; a client prices its own trade window with them
	{"game_object:show_detector", CallClass::Owner},
	{"game_object:sight_params", CallClass::Read},
	{"game_object:skip_transfer_enemy", CallClass::World},
	{"game_object:sniper_fire_mode", CallClass::World},
	{"game_object:sniper_update_rate", CallClass::World},
	{"game_object:sound_prefix", CallClass::World}, // unsure: an NPC's sound set and speech; the host's AI plays them
	{"game_object:sound_voice_prefix", CallClass::Read},
	{"game_object:spawn_ini", CallClass::Read},
	{"game_object:special_danger_move", CallClass::World},
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
	{"game_object:take_item", CallClass::Owner},
	{"game_object:take_items_enabled", CallClass::World},
	{"game_object:target_body_state", CallClass::Read},
	{"game_object:target_mental_state", CallClass::Read},
	{"game_object:target_movement_type", CallClass::Read},
	{"game_object:team", CallClass::Read},
	{"game_object:torch_enabled", CallClass::Read},
	{"game_object:transfer_item", CallClass::World}, // ReqTransfer / ReqTake / ReqDrop when our actor is on one side, else refused
	{"game_object:transfer_money", CallClass::World},
	{"game_object:unload_magazine", CallClass::Owner},
	{"game_object:unlock_door_for_npc", CallClass::World},
	{"game_object:unregister_door_for_npc", CallClass::World},
	{"game_object:unregister_in_combat", CallClass::World},
	{"game_object:update_torch", CallClass::Owner},
	{"game_object:update_weight", CallClass::Read},
	{"game_object:use", CallClass::World},
	{"game_object:use_smart_covers_only", CallClass::World},
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
	{"game_object:wounded", CallClass::World}, // unsure: getter and setter share the name; the setter changes an NPC
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
	{"level:patrol_path_add", CallClass::World}, // unsure: patrol paths steer the host's AI
	{"level:patrol_path_exists", CallClass::Read},
	{"level:patrol_path_remove", CallClass::World}, // unsure: patrol paths steer the host's AI
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
	{"level:set_game_difficulty", CallClass::World},
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
	{"level:spawn_item", CallClass::World},
	{"level:spawn_phantom", CallClass::World},
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
	{"weather:boost_reset", CallClass::World}, // unsure: edits the current weather the host sends; may be a local look
	{"weather:boost_value", CallClass::World}, // unsure: edits the current weather the host sends; may be a local look
	{"weather:get_value_numric", CallClass::Read},
	{"weather:get_value_string", CallClass::Read},
	{"weather:get_value_vector", CallClass::Read},
	{"weather:is_paused", CallClass::Read},
	{"weather:pause", CallClass::World}, // unsure: edits the current weather the host sends; may be a local look
	{"weather:reload", CallClass::World}, // unsure: edits the current weather the host sends; may be a local look
	{"weather:set_value_numric", CallClass::World}, // unsure: edits the current weather the host sends; may be a local look
	{"weather:set_value_string", CallClass::World}, // unsure: edits the current weather the host sends; may be a local look
	{"weather:set_value_vector", CallClass::World}, // unsure: edits the current weather the host sends; may be a local look
	{"weather:sun_time", CallClass::Read},
};

inline size_t call_count() { return sizeof(kCalls) / sizeof(kCalls[0]); }

// Client calls the engine sends to the host instead of running them (MOD_LAYER §2.3). own_only: only for a target
// the client owns (its actor, an item under it); another's target has no path.
struct RequestPath
{
	std::string_view name;
	bool own_only;
};

inline constexpr RequestPath kRequestPaths[] = {
	// ReqSpawn (coop_requests.cpp lua_create / lua_clone / lua_register_spawn): an item into our own inventory only
	// (client_forward_spawn); own = the parent is our actor or under it. A world spawn is the host's.
	{"alife:create", true},
	{"alife:create_ammo", true},
	{"alife:clone_weapon", true},
	{"alife:register", true}, // a temporary object of create(..., false) / clone_weapon(..., false)
	{"alife:release", true},  // client_destroy of an own item (lua_release)
	{"game_object:give_info_portion", false},    // story (inventory_owner_info.cpp -> coop_story on_info)
	{"game_object:disable_info_portion", false}, // the same
	{"game_object:set_relation", false},         // story Relation (relation_registry.cpp -> coop_story)
	{"game_object:set_goodwill", false},
	{"game_object:force_set_goodwill", false},
	{"game_object:change_goodwill", false},
	{"game_object:set_community_goodwill", false},
	{"relation_registry:set_community_goodwill", false},
	{"relation_registry:change_community_goodwill", false},
	{"relation_registry:set_community_relation", false},
	{"game_object:transfer_item", true}, // ReqTransfer / ReqTake / ReqDrop (coop_events.cpp); own: our actor is a side
	{"game_object:give_money", true},    // our actor: the money journal (GE_MONEY -> money::client_on_actor_money)
	{"level:change_game_time", false},   // TimeRequest (coop_worldstate.cpp intercept_time_jump)
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

inline const Call* find_row(std::string_view name)
{
	const auto& t = table();
	const auto it = t.find(lowered(name));
	if (it == t.end())
		return nullptr;
	for (const Call& c : kCalls)
		if (iequals(c.name, name))
			return &c;
	return nullptr;
}
} // namespace detail

inline bool has_row(std::string_view name) { return detail::table().count(detail::lowered(name)) != 0; }

// The built-in class of a function; read when the table does not know it.
inline CallClass class_of(std::string_view name)
{
	const auto& t = detail::table();
	const auto it = t.find(detail::lowered(name));
	return it == t.end() ? CallClass::Read : it->second;
}

// Whether the engine has a request path for this call on a client, for a target it owns or not.
inline bool has_request_path(std::string_view name, bool own_target)
{
	for (const RequestPath& p : kRequestPaths)
		if (detail::iequals(p.name, name))
			return own_target || !p.own_only;
	return false;
}

// The decision (MOD_LAYER §2.3):
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

// Gated calls on this machine for coop_status: totals, refusals per name for "the most refused" list, and the budget
// of refusals written to the log one by one (the first kLoggedRefusals since clear(); the rest are only counted).
// The per-name table is bounded: a name past kMaxNames counts in the totals only.
class Counters
{
public:
	static constexpr size_t kMaxNames = 512;
	static constexpr size_t kLoggedRefusals = 10;

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
				++it->second;
			else if (m_refused.size() < kMaxNames)
				m_refused.emplace(std::string(name), uint64_t(1));
			else
				++m_dropped;
			break;
		}
	}

	const Totals& totals() const { return m_totals; }

	// The names with the most refusals, most first (a tie by name), at most n.
	std::vector<std::pair<std::string, uint64_t>> top_refused(size_t n) const
	{
		std::vector<std::pair<std::string, uint64_t>> all(m_refused.begin(), m_refused.end());
		const auto more = [](const std::pair<std::string, uint64_t>& a, const std::pair<std::string, uint64_t>& b)
		{ return a.second != b.second ? a.second > b.second : a.first < b.first; };
		const size_t k = n < all.size() ? n : all.size();
		std::partial_sort(all.begin(), all.begin() + std::ptrdiff_t(k), all.end(), more);
		all.resize(k);
		return all;
	}

	// True for the first kLoggedRefusals calls since clear(): that refusal gets its own log line.
	bool take_log_line()
	{
		if (m_logged >= kLoggedRefusals)
			return false;
		++m_logged;
		return true;
	}

	size_t names_tracked() const { return m_refused.size(); }
	uint64_t names_dropped() const { return m_dropped; }

	void clear()
	{
		m_totals = Totals();
		m_refused.clear();
		m_dropped = 0;
		m_logged = 0;
	}

private:
	Totals m_totals;
	std::map<std::string, uint64_t, std::less<>> m_refused; // std::less<>: find() by string_view
	uint64_t m_dropped = 0;
	size_t m_logged = 0;
};
} // namespace calls
} // namespace coop
