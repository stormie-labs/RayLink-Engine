// Anomaly Coop — the scope of every message and the route a process gives it (F3a split levels; docs/plans/
// 2026-10-24-f3a-split-levels.md §2.2).
//
// Every player may be on another level. The host's process holds the world and leads its own level; a background map
// server (Mk) leads each other level that has players. A message a player sends goes to whoever has to act on it:
//
//   Core   the session's own (handshake, blobs, roster, chat): handled in the process that receives it, never routed;
//   World  the world server (the host's process): story, tasks, money, time, the emission, the checkpoints, the life states,
//          the join and the level moves; a player on a level of a map server asks the same process as everybody;
//   Map    the authority of the level the SENDER stands on: the stream of his body, the entities, the requests on objects,
//          dialogs, trade, zones, the weather of the level, leased ids, a dead player's marker. The host's process handles it
//          when it leads that level, and tunnels it to the map server that does otherwise (Msg::Tunnel);
//   Trunk  the link between the host's process and a map server (Level* except the three a player sees, Tunnel,
//          MapServer*): taken from the bridge of a map server alone.
//
// route() is the whole decision and a pure function; the game passes what it knows (coop_trunk.cpp, F3a T9). A message that
// the Tunnel carries is taken out by the trunk code and handed to the handler of the process that leads the level: it is not
// routed again, so Map messages cannot go round between the two processes.
//
// Some "world" messages touch the body of a player, and the body is an object of the map of its authority: the list
// also_to_level_authority() names them, with the reason; the world server handles them and the authority of the sender's
// level gets a copy.
//
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstdint>

#include "coop_protocol.h"

namespace coop
{
enum class Scope : uint8_t
{
	Core,
	World,
	Map,
	Trunk,
};

inline const char* scope_name(Scope s)
{
	switch (s)
	{
	case Scope::Core: return "Core";
	case Scope::World: return "World";
	case Scope::Map: return "Map";
	case Scope::Trunk: return "Trunk";
	}
	return "?";
}

// One row per message (coop_protocol.h Msg). False for a value that is no message of the protocol. A new message needs its
// row here: test_scope.cpp scope_every_named_game_message_has_a_scope fails without it, and (MSVC) an enumerator that the
// switch misses is error C4062 (off by default), as in msg_sender.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(error : 4062)
#endif
inline bool scope_row(Msg type, Scope& out)
{
	switch (type)
	{
	// --- the session's own
	case Msg::Hello:
	case Msg::Welcome:
	case Msg::Reject:
	case Msg::Bye:
	case Msg::BlobBegin:
	case Msg::BlobChunk:
	case Msg::BlobEnd:
	case Msg::PeerList:
	case Msg::Chat:
	case Msg::WorldEpoch: out = Scope::Core; return true;

	// --- the world server: the join and the world's lifecycle (Join*, Transition*), the level moves of F3a
	case Msg::JoinRequest: // Msg::GameBase has the same value
	case Msg::WorldBlob:
	case Msg::WorldLoaded:
	case Msg::JoinComplete:
	case Msg::SessionReset:
	case Msg::LevelChange:
	case Msg::TransitionVote:
	case Msg::AvatarList:
	case Msg::CharOffer:
	case Msg::CharChoice:
	case Msg::TransitionState:
	case Msg::LevelMap:
	case Msg::LevelMove:
	case Msg::LevelMoveResult:
	case Msg::LoadWait:
	case Msg::LoadReady:
	// the life states and the rules of death (they also go to the authority of the level, also_to_level_authority)
	case Msg::PlayerWounded:
	case Msg::PlayerRevive:
	case Msg::PlayerDied:
	case Msg::PlayerRules:
	// money (the world server's book), time, the emission
	case Msg::ReqMoney:
	case Msg::MoneyBalance:
	case Msg::TimeSync:
	case Msg::TimeRequest:
	case Msg::Emission:
	// quests, story, PDA
	case Msg::InfoPortion:
	case Msg::TaskList:
	case Msg::TaskUpdate:
	case Msg::MapSpot:
	case Msg::Relation:
	case Msg::Progress:
	case Msg::StoryHash:
	case Msg::StoryResync:
	// coop.sync lives with the world server's Lua
	case Msg::SyncSet:
	case Msg::SyncFull:
	case Msg::SyncRequest:
	case Msg::SyncResult:
	case Msg::SyncCast:
	// F3: checkpoints, the shared sleep, hardcore
	case Msg::CheckpointState:
	case Msg::CheckpointClaim:
	case Msg::RestRequest:
	case Msg::RestState:
	case Msg::RestSleep:
	case Msg::NewCharacter:
	// the Lua RPC of mods runs in the world server's Lua
	case Msg::ScriptEvent: out = Scope::World; return true;

	// --- the authority of the sender's level: the stream of the body, the entities, the requests on them, dialogs, trade
	case Msg::PlayerState:
	case Msg::PlayerVitals:
	case Msg::PlayerEquip:
	case Msg::PlayerAnim:
	case Msg::PlayerShot:
	case Msg::PlayerGrenade:
	case Msg::PlayerHit:
	case Msg::EntitySpawn:
	case Msg::EntityDestroy:
	case Msg::EntitySwitch:
	case Msg::EntitySnapshot:
	case Msg::EntityAnim:
	case Msg::EntityEvent:
	case Msg::EntityHealth:
	case Msg::EntityDied:
	case Msg::EntityParent:
	case Msg::EntityState:
	case Msg::EntityLocal:
	case Msg::EntitySound:
	case Msg::ItemState:
	case Msg::ReqSpawn:
	case Msg::ReqDestroy:
	case Msg::ReqTake:
	case Msg::ReqDrop:
	case Msg::ReqTransfer:
	case Msg::ReqUse:
	case Msg::ReqHit:
	case Msg::ReqResult:
	case Msg::ReqContainer:
	case Msg::ReqItemEvent:
	case Msg::ReqLaunch:
	case Msg::DialogLock:
	case Msg::DialogLockResult:
	case Msg::DialogUnlock:
	case Msg::DialogPhrases:
	case Msg::DialogSelect:
	case Msg::TradeOp:
	case Msg::ZoneState:
	case Msg::WeatherSync:
	case Msg::IdLease:
	case Msg::IdLeaseAck:
	case Msg::DeathMarker:
	case Msg::MonsterEffect: // audit A5: the authority of the monster's level decides
	// a client asks for an entity it misses: the authority of its level answers with the spawn
	case Msg::StateDumpRequest:
	case Msg::StateDump: out = Scope::Map; return true;

	// --- the trunk: the host's process <-> a map server
	case Msg::LevelFlush:
	case Msg::LevelSnapshot:
	case Msg::LevelEntity:
	case Msg::LevelXferUp:
	case Msg::LevelXferDown:
	case Msg::LevelBrain:
	case Msg::LevelHash:
	case Msg::LevelResync:
	case Msg::Tunnel:
	case Msg::MapServerState:
	case Msg::MapServerOrder: out = Scope::Trunk; return true;
	}
	return false;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// True if `type` has a row in scope_row (the tests ask this to find a message without a decision).
inline bool scope_listed(Msg type)
{
	Scope s = Scope::Core;
	return scope_row(type, s);
}

// The scope of a message. A value that is no message of the protocol (no handler takes it) is the world server's: the
// side that neither tunnels nor refuses.
inline Scope scope_of(Msg type)
{
	Scope s = Scope::World;
	scope_row(type, s);
	return s;
}

// What a process does with a message of scope `s` that it has received.
enum class Route : uint8_t
{
	HandleHere, // run the handler in this process
	TunnelToServer, // wrap it in Msg::Tunnel for the map server that leads the sender's level
	RefuseNotTrunk, // a trunk message from somebody that is not the bridge of a map server: kick him as malformed
};

// `from_trunk`: the message came over the bridge of a map server (or was taken out of a Tunnel by the trunk code on the map
// server's side); `host_leads_senders_level`: this process is the authority of the level the sender stands on (on the host's
// process: its own level is his; on a map server: its level is his, a virtual player is always on it).
//
//   Core   always here;
//   World  always here: the process that receives a world message is the world server, wherever the sender stands;
//   Map    here if this process leads the sender's level, else through the tunnel; `from_trunk` does not change it (the trunk
//          code unwraps a Tunnel and calls the handler itself, the unwrapped message is not routed again);
//   Trunk  here from the bridge, refused from anybody else.
inline Route route(Scope s, bool from_trunk, bool host_leads_senders_level)
{
	switch (s)
	{
	case Scope::Core:
	case Scope::World: return Route::HandleHere;
	case Scope::Map: return host_leads_senders_level ? Route::HandleHere : Route::TunnelToServer;
	case Scope::Trunk: return from_trunk ? Route::HandleHere : Route::RefuseNotTrunk;
	}
	return Route::RefuseNotTrunk; // not a scope
}

// World messages that touch the body of a player also go to the authority of the sender's level: the body is an object of
// that map, and the world server cannot change it. The world server handles them as every world message; the copy is for
// the objects.
//   PlayerWounded, PlayerRevive, PlayerDied  the life state of the body: the authority sets the health, drops and holds it
//                                            (the bleed-out clock, the revive hold, the death drop run at the body);
//   CheckpointClaim                          the tent a player placed is an object of his map: the authority finds it and
//                                            vouches for the spot;
//   RestRequest                              the place to lie down is checked by the authority of the player's level
//                                            (plan §1.9, sleep).
inline bool also_to_level_authority(Msg type)
{
	switch (type)
	{
	case Msg::PlayerWounded:
	case Msg::PlayerRevive:
	case Msg::PlayerDied:
	case Msg::CheckpointClaim:
	case Msg::RestRequest: return true;
	default: break;
	}
	return false;
}
} // namespace coop
