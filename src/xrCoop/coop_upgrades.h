// Anomaly Coop — which upgrades of a list the engine can install, in that order, without a FATAL (R3 В6 review).
// CInventoryItem::net_Spawn and our ItemState apply install with loading = true, and there
// inventory::upgrade::Group::can_install is FATAL instead of a refusal:
//   result_e_parents  a non-root parent upgrade of the upgrade's group has no installed upgrade in its own group;
//   result_e_group    another upgrade of the same group is installed.
// This replays those rules over a list from a peer, without an item. The engine builds the descriptors from its
// upgrade tree (coop_items.cpp).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop
{
namespace upgrades
{
struct Upgrade
{
	std::string id;
	bool in_tree = false; // a known upgrade of this item's tree (Manager::get_upgrade + Root::contain_upgrade)
	std::string group; // its own group (Upgrade::parent_group_id)
	// Groups that need an installed upgrade first: the own groups of the non-root parent upgrades of `group`.
	std::vector<std::string> needs_groups;
};

enum class Verdict : uint8_t
{
	Install,
	Default, // put on by the section's defaults: nothing to install, but the list names it (keep it there)
	Installed, // already on the item, or a repeat in the list: skipped, not an error
	Unknown, // not in this item's tree
	NoParent, // result_e_parents
	GroupTaken, // result_e_group
};

// `installed`: what the item already has (unknown ones are ignored). `wanted`: in install order.
// `defaults`: for a new entity, the section's installed_upgrades — CInventoryItem::net_Spawn_install_upgrades puts
// them on first (Manager::init_install, the same rules) and only then the list. A list taken from a server entity
// lacks them (loading installs send no GE_INSTALL_UPGRADE), a list from a game object has them.
// `refused_defaults` (optional): how many defaults the engine itself would refuse (not in the tree, parent group
// missing, group taken) — init_install is FATAL on those (task 10b). A repeated default is skipped by the engine
// (result_e_installed) and is not counted.
inline std::vector<Verdict> plan(const std::vector<Upgrade>& installed, const std::vector<Upgrade>& wanted,
    const std::vector<Upgrade>& defaults = {}, size_t* refused_defaults = nullptr)
{
	std::vector<std::string> ids, groups, unclaimed_defaults;
	size_t refused = 0;
	const auto has = [](const std::vector<std::string>& v, const std::string& s)
	{ return std::find(v.begin(), v.end(), s) != v.end(); };
	const auto parents_ok = [&](const Upgrade& u)
	{
		for (const std::string& g : u.needs_groups)
			if (!has(groups, g))
				return false;
		return true;
	};
	for (const Upgrade& u : installed)
	{
		if (!u.in_tree)
			continue;
		ids.push_back(u.id);
		groups.push_back(u.group);
	}
	for (const Upgrade& u : defaults)
	{
		if (has(ids, u.id))
			continue;
		if (!u.in_tree || !parents_ok(u) || has(groups, u.group))
		{
			++refused; // FATAL in init_install itself: the section's data is broken
			continue;
		}
		ids.push_back(u.id);
		groups.push_back(u.group);
		unclaimed_defaults.push_back(u.id);
	}
	std::vector<Verdict> out;
	out.reserve(wanted.size());
	for (const Upgrade& u : wanted)
	{
		const auto d = std::find(unclaimed_defaults.begin(), unclaimed_defaults.end(), u.id);
		if (d != unclaimed_defaults.end())
		{
			unclaimed_defaults.erase(d); // a second mention is a repeat
			out.push_back(Verdict::Default);
		}
		else if (has(ids, u.id))
			out.push_back(Verdict::Installed);
		else if (!u.in_tree)
			out.push_back(Verdict::Unknown);
		else if (!parents_ok(u))
			out.push_back(Verdict::NoParent);
		else if (has(groups, u.group))
			out.push_back(Verdict::GroupTaken);
		else
		{
			out.push_back(Verdict::Install);
			ids.push_back(u.id);
			groups.push_back(u.group);
		}
	}
	if (refused_defaults)
		*refused_defaults = refused;
	return out;
}

// The section's installed_upgrades break the tree rules: any new entity of it is FATAL in init_install.
inline bool defaults_ok(const std::vector<Upgrade>& defaults)
{
	size_t refused = 0;
	plan({}, {}, defaults, &refused);
	return refused == 0;
}

// A client installed `added` on its item (GE_INSTALL_UPGRADE, task 10b). `entity`: the item's server entity list
// (no section defaults). The verdict of `added` the way the next net_Spawn from scratch installs them: defaults,
// that list, then it. Only Install may reach the entity (CSE add_upgrade); Installed / Default = already there.
inline Verdict verdict_of_added(
    const std::vector<Upgrade>& entity, const Upgrade& added, const std::vector<Upgrade>& defaults)
{
	std::vector<Upgrade> list = entity;
	list.push_back(added);
	return plan({}, list, defaults).back();
}
} // namespace upgrades
} // namespace coop
