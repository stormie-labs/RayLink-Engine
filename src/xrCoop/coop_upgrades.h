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
	Installed, // already on the item or earlier in the list: skipped, not an error
	Unknown, // not in this item's tree
	NoParent, // result_e_parents
	GroupTaken, // result_e_group
};

// `installed`: what the item already has (unknown ones are ignored). `wanted`: in install order.
inline std::vector<Verdict> plan(const std::vector<Upgrade>& installed, const std::vector<Upgrade>& wanted)
{
	std::vector<std::string> ids, groups;
	for (const Upgrade& u : installed)
	{
		if (!u.in_tree)
			continue;
		ids.push_back(u.id);
		groups.push_back(u.group);
	}
	const auto has = [](const std::vector<std::string>& v, const std::string& s)
	{ return std::find(v.begin(), v.end(), s) != v.end(); };
	std::vector<Verdict> out;
	out.reserve(wanted.size());
	for (const Upgrade& u : wanted)
	{
		if (has(ids, u.id))
			out.push_back(Verdict::Installed);
		else if (!u.in_tree)
			out.push_back(Verdict::Unknown);
		else if (!std::all_of(u.needs_groups.begin(), u.needs_groups.end(),
		             [&](const std::string& g) { return has(groups, g); }))
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
	return out;
}
} // namespace upgrades
} // namespace coop
