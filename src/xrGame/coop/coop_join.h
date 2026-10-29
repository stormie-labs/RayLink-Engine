// Anomaly Coop — joining the host's world.
//
// Host: on JoinRequest it makes sure the player has a character (a proxy NPC in the host world,
// found by player key), writes a fresh save of the world and streams it to the client.
// Protocol 8 (owner, live test 2026-10-23): a key the world has no character for chooses one first. The host answers
// its JoinRequest with Msg::CharOffer; the client shows it in the choice window (scripts\coop_menu_char.script, main
// menu) and sends Msg::CharChoice; the host's script checks it and picks the faction's start on the host's level (else
// a camp of the faction there, else next to the host: every player is on the host's level until split levels, F3a);
// the host makes the character there with the faction's community, the script gives the kit and the money, and the
// world save follows as for a returning player. A refused choice is offered again with the reason; the time to choose
// is limited (xrCoop/coop_newchar.h). F3 Task 10: a player whose hardcore character retired goes the same way in the game
// (host_rejoin_peer); Ruling F3-9: his new character does not count against coop_max_new_characters.
// Client: loads that save, so every server entity has the same id as on the host. Right after
// the save is read and before anything goes online, the client swaps roles: its local actor
// (id 0) takes the data of its own proxy, and the proxy takes the host actor's data — the proxy
// becomes the host's avatar on this machine. Then live sync starts.
#pragma once

#include "xrCoop/coop_session.h"

class CALifeObjectRegistry;
class NET_Packet;
struct lua_State;

// Host setting (console coop_max_new_characters, Ruling F2-10 b, F2 T3 fix round 1): how many new player characters
// (a player key the world has no character for) a session makes; default and range in xrCoop/coop_limits.h. Ruling F3-9:
// only keys that never had a character count; a key that had one in this session, or whose character retired in this
// world (hardcore, F3 Task 10), neither checks nor spends it.
extern int g_coop_max_new_characters;
// 1: print the C++ stacks of coop::diag (dbghelp loads the ~225 MB PDB and stalls the game 1-2 min the first time;
// live 2026-10-23 a client froze after the host crashed). Off by default.
extern int g_coop_diag_stacks;

namespace coop
{
// F0 final review (task 14 diagnostics): CGameObject::lua_game_object prints a C++ stack for the first
// kDestroyedObjectStacks accesses to a destroyed object after a player left a co-op session, not since the process
// started (single player and loads spent them, and every stack stalls the thread that prints it).
namespace diag
{
constexpr u32 kDestroyedObjectStacks = 3;
// A player left: the next kDestroyedObjectStacks accesses print a stack (armed again by every departure). Host:
// join::on_peer_left; client: another player left (Manager), or it left the host's world itself (on_session_lost).
void arm_destroyed_object_stacks();
// An access to a destroyed object: true if it is to print its C++ stack (takes one of the armed ones). Any thread.
bool take_destroyed_object_stack();
} // namespace diag

namespace join
{
// Blob kinds sent with Session::send_blob.
enum BlobKind : u32
{
	BlobWorldSave = 1,
	// F3a T8: the shared save of a group transition (the same layout as BlobWorldSave), sent with LoadWait Prepared to every
	// moving client before the host's own reload: everybody loads it at once (levels::wire kBlobLevelState is 3).
	BlobPreparedSave = 2,
};

// F3a T4: the world server's handlers (the joins, choices and votes of the players; the session's rate limits) and
// the client's (the world server's answers and its board), installed by role (Manager::install_handlers).
void install_server(Session& s);
void install_client(Endpoint& e);

// --- host ---
bool host_can_start(); // in a level with ALife and a living actor
void on_host_started();
void on_peer_joined(const PeerInfo& p);
void on_peer_left(PeerId id); // parks the player's proxy and takes it out of the world

// Proxy (avatar) of a peer in the host world, 0xffff if none. Host only.
u16 proxy_of_peer(PeerId id);
PeerId peer_of_proxy(u16 proxy_id);
bool is_proxy(u16 id);
// W9, host: the player of proxy `id` is connected and has finished loading the world (WorldLoaded). Only then is
// his character in the world: an absent or still loading player's proxy stays offline (coop::switch_vote).
bool host_player_in_world(u16 proxy_id);

// Host: increments when a load or level change replaces the served world (on_world_created), and so does the
// session's world epoch: the session drops what peers made in an older world (R2 I1). Requests from peers that
// rejoin the current world but have not loaded it yet must be ignored (see sync::host_peer_joining).
u32 host_world_epoch();
// Engine hook (end of the CALifeSimulator constructor, main thread): a world was created
// (save loaded, level changed, new game). A same-level quick load deletes and recreates the
// simulator in one call, often at the same address, so the pointer cannot tell worlds apart.
void on_world_created();
// Number of worlds created in this process (any role).
u32 world_generation();
// Host: move the group at once through the exit somebody waits at (console coop_accept_transition, the chat's /go),
// whoever is missing. The level change is built from the host's own level changer (R2 I3).
bool accept_transition();

// --- the waiting point of a level change (protocol 10; rule and messages in xrCoop/coop_transition.h) ---
// Until the split levels (F3a) the whole group changes level together: a player who stands in the zone of an exit (or in a
// route restrictor that leads into it, coop.transition_route) is ready for it, one who is not at the exit may hold a key to
// come along from where he is, the group goes on the moment every player in the world is ready (no countdown; the screens
// darken, the load follows a second later; owner 2026-10-24). Both roles run
// it from frame(); the placard reads coop.transition() (scripts\coop_hud_transition.script). F3a T8: the host player votes
// and reads the board over the local link like a client (TransitionVote / TransitionState); the group's move is one
// simultaneous load (LoadWait / LoadReady: every client in the world loads the transition's save with the host, the loading
// screens come off together, hold_loading_screen below).
// Hook of CLevelChanger (the stock "go?" dialog and the silent change of its zone): true = the group waits here, the
// stock reaction stays off. False for a disabled exit (its own message stays), outside a session and for a host nobody
// has joined.
bool transition_owns_exit(bool exit_enabled);
// Hook of game_sv_Single::change_level, the one place every level change of the host goes through (the zone, the
// dialog, jump_to_level of a guide): true = held, the group waits (a guide trip, which has no exit, needs everybody's
// yes); false = the stock change goes on (no players besides the host, or the board's own change). `P` is read, not
// consumed.
bool host_gate_level_change(NET_Packet& P);
// Console coop_transition_consent (any player who is NOT at the exit the board waits at, 2026-10-24): the key held: he goes
// with the group from where he is (no distance) until he holds it again. coop_transition_cancel (any player): the key held
// while ready: withdraw (at an exit: no claim until he leaves the zone; coming along: not any more); a guide trip: not
// confirmed. coop_transition_confirm (client): yes to the host's guide trip. The host's "go now" key is gone (the chat's
// /go, accept_transition, stays).
bool transition_consent();
bool transition_cancel();
bool transition_confirm();
// coop.transition() and coop.transition_route() for Lua (coop_script.cpp register_raw).
void register_lua(lua_State* L);
// Host, F3 Task 10: player `peer` goes through the join again in the world the host serves now: SessionReset {why,
// epoch} to him alone, with the epoch he is in already (his session and the host's take it as it is), his entry
// loading, his world traffic held (sync::host_begin_join), his character nobody's. His JoinRequest finds his character
// by key, or with none (a retired hardcore character) offers him a new one (protocol 8). A join of his still in flight
// is void; the 5 s between two admissions hold (a JoinRequest within them waits, JoinGate::Later, and goes after).
// False if he is not a player of this session in the world (no entry, the host itself).
bool host_rejoin_peer(PeerId peer, const char* why);
// Host, Ruling F3-9: the character of player key `key_digest` retired (F3 Task 10). Kept for this session and in the
// world (the host actor's custom data, [coop_retired], death::note_retired_key: the newest 64): his new character, now
// or in a later session, neither checks nor spends coop_max_new_characters, and (Ruling F3-10) starts with nothing -
// no kit, no faction money, the offer has 0 points - unless coop_hardcore_new_kit is on (death::host_new_kit).
void host_key_retired(const std::string& key_digest);

// --- client ---
// Client touched a level exit: the host decides. Only the id of the level changer goes to the host (R2 I3).
void client_request_level_change(NET_Packet& P);
void on_welcome(const WelcomeMsg& w);
// Server entity id that represents this client in the host world (our own proxy there).
// On the client the ids 0 and my_proxy() are swapped against the host numbering.
u16 my_proxy();

// Host id <-> local id. Identity for every id except the swapped pair (0, my_proxy()).
inline u16 to_local(u16 host_id)
{
	const u16 p = my_proxy();
	if (p == 0xffff)
		return host_id;
	return host_id == 0 ? p : (host_id == p ? 0 : host_id);
}
inline u16 to_host(u16 local_id) { return to_local(local_id); } // swap is symmetric

// --- both ---
void on_session_lost(const char* reason);
void on_blob_progress(PeerId from, u32 id, u32 kind, u64 done, u64 total);
void on_blob_complete(PeerId from, u32 id, u32 kind, std::vector<u8>& data);
void frame();
void print_status();

// --- engine hooks ---
// Client: ALife read the objects of the save but has not registered them yet (no children
// lists, nothing online). Applies the role swap by rewriting ID_Parent.
void on_alife_objects_loaded(CALifeObjectRegistry& objects);
// True while a coop client is loading the host's world.
bool client_loading_world();
// Received world snapshots are a private load cache, never a campaign save or quick-load target.
bool is_client_cache_save(LPCSTR name);
void client_cache_path(string_path& out, LPCSTR extension);
void prepare_client_cache(); // once at engine initialization; preserves legacy files in an archive
bool may_save_world(); // blocks client writes, including a mirror while disconnect tears it down
// B2: this machine's world is a mirror of a host's: loaded from the host's save as a co-op client (also while it loads).
// It stays so after the session ends (the client leaves the session before the level is torn down, SLS_Clear runs with
// the phase Off), until another world (a load, a level change, a new game) replaces it or this machine hosts it. Any
// thread.
bool world_is_mirror();
} // namespace join

// F3a T8, engine hook (CGamePersistent::game_loaded): the loading screen of a simultaneous group load stays up. A player
// whose world of the transition is loaded waits on it until the world server says Enter (every mover is ready, or a minute
// after the server's world came up), then all screens come off together; "Waiting for: <nicks>" is its header line. Never
// outside a session; a held screen lets go by itself kLoadWaitMs + 30 s after the server's world was up (xrCoop/coop_transition.h
// LoadScreen). Main thread.
bool hold_loading_screen();
} // namespace coop
