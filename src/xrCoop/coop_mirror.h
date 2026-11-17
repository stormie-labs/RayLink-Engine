// Anomaly Coop - a co-op client's mirror of the host's world: where an entity from the host goes on this machine, and
// what a holder does when it leaves the server listing a child that has no server entity here (B2, live 2026-10-23:
// FATAL "child registered but not found" in xrServer::Perform_destroy on a client leaving the game).
// The server keeps online entities only; ALife and the server share one `children` list per holder. A holder on the
// server may list only children that are on the server too: Perform_destroy (the holder switches offline, the level is
// torn down in SLS_Clear) asserts on any other, Process_event_destroy loops on it for ever.
// Pure C++17, header-only, no engine dependencies: used by the engine (coop_entities.cpp) and the unit-test exe.
#pragma once

#include <cstdint>

namespace coop
{
namespace mirror
{
// Where an entity from the host's EntitySpawn goes on this machine.
enum class Placement : uint8_t
{
	Offline,      // stays offline: free and offline on the host, or its holder is offline here (it comes online with it)
	SwitchVote,   // free and online on the host: the host's online set takes it in, the local switch manager does the rest
	WithHolder,   // its holder is online here: online now, as ALife brings every child of an online holder online
	HolderBroken, // its holder is online here but has no server entity: left offline, logged (a desync of the holder)
	OnlineNow     // free and online on the host, in no level registry here, switches by itself and is still offline:
	              // the switch manager never visits it, online now
};

// has_holder: it came with a parent; host_online: the host's flag; holder_online / holder_on_server: the holder here.
// A child follows its holder HERE. The host's flag of a child is its holder's state THERE, which may differ: the host
// keeps a player's character offline until he has loaded the world while our actor is online here from the start, and
// an NPC that switched offline in the same host frame is still online here until its EntitySwitch.
// in_level_registry: a free entity is in this level's ALife registry (CALifeGraphRegistry::add puts it there only with a
// valid game vertex), the only one CALifeUpdateManager::update_switch walks. Build 51 live: a psy phantom spawned by
// level.spawn_item has no game vertex; it stayed offline here for ever, invisible, while the host's copy hit the player.
// flag_of_this_level: the host's online flag is a word about the level this machine has loaded (false on a map server,
// and for a player on another level than the host's under split levels: the flag is about the host's level there).
// switches_alone: the entity goes online by its own switch_online and is still offline after register_object. False
// for a squad (CSE_ALifeOnlineOfflineGroup): it comes online with its members, and here it has no game vertex as a
// rule. Builds 52-54 live (issue 1293): OnlineNow took squads, their second switch_online failed
// R_ASSERT(!m_bOnline) and every client of an ordinary session crashed. A squad stays on the vote, as in build 51.
inline Placement placement(bool has_holder, bool host_online, bool holder_online, bool holder_on_server,
                           bool in_level_registry = true, bool flag_of_this_level = true, bool switches_alone = true)
{
	if (!has_holder)
	{
		if (!host_online)
			return Placement::Offline;
		return !in_level_registry && flag_of_this_level && switches_alone ? Placement::OnlineNow : Placement::SwitchVote;
	}
	if (!holder_online)
		return Placement::Offline;
	return holder_on_server ? Placement::WithHolder : Placement::HolderBroken;
}

// What Perform_destroy / Process_event_destroy do with a child id of a holder leaving the server when that child has no
// server entity here.
enum class LostChild : uint8_t
{
	Fatal,  // not a mirror (single player, the host): the stock assert, the world itself is corrupt
	Detach, // an offline item of this very holder in ALife: taken out of it the ALife way (free, offline, where the
	        // holder is), so a later release or move of it finds ALife's books whole
	Drop    // anything else (gone, held by another, online without a server entity): the id leaves the list
};

// mirror: this machine's world is a mirror of a host's (a co-op client, also while it leaves the session);
// in_alife / child_online / held_by_holder: what ALife here knows of the child (its ID_Parent is this holder);
// detachable: the holder is an ALife object and the child an inventory item (graph().detach takes them).
inline LostChild lost_child(bool mirror, bool in_alife, bool child_online, bool held_by_holder, bool detachable)
{
	if (!mirror)
		return LostChild::Fatal;
	return in_alife && !child_online && held_by_holder && detachable ? LostChild::Detach : LostChild::Drop;
}
} // namespace mirror
} // namespace coop
