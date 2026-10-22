// Anomaly Coop — coop.sync for Lua: shared variables, actions through the host, effects for everybody (SYNC_SPEC
// §6.1-6.3, for modders docs/COOP_API.md; F2 T14). The store is xrCoop/coop_syncstore.h (F2 T13), the messages and the
// rules of who hears what are xrCoop/coop_syncnet.h; this is the glue to the engine, Lua and the host save.
//
// Roles. The host holds the store and stamps every change; so does a machine outside a session (single player: a mod
// that uses coop.sync works there, and its world / player / object data go into the save the same way). A client holds
// what the host sends it (world, session, every object, his own player scope) and shows his own changes at once until
// the host answers them.
//
// Lua (coop.sync.*; a refusal returns false and a reason, it never raises):
//   set(scope, key, value) -> true | false, why       nil removes; a value of at most 16 KiB encoded
//   get(scope, key) -> value | nil
//   keys(scope, prefix) -> {key = value}
//   on_change(scope, prefix, fn(key, value, old, by_peer[, id]) | nil) -> true | false, why
//   handler(name, fn(peer, args) -> ok, result | nil) -> true | false, why                               host
//   request(name, args, fn(ok, result) | nil) -> true | false, why
//   broadcast(name, args, {to = peer} | {pos = vector, radius = m} | nil) -> true, machines | false, why    host
//   on(name, fn(args, from) | nil) -> true | false, why
//   near(peer, obj_id, dist), owns(peer, item_id), in_container(peer, item_id, box_id) -> bool
//   limit(name, burst, per_sec) -> true | false, why                                                    host
//   export() -> table | nil, import(table | nil) -> true | false, why      host (scripts\coop_sync.script, the save)
// Scopes: "world" and "session" (the host writes them), "player" (this machine's player), "player:<peer>" (a connected
// player, on the host), "object:<id>" (local numbering; the host or the player who holds it writes it), "object" (every
// object: on_change only, its handler gets the object id as a fifth argument). Values: nil, number, string, boolean,
// vector, a flat table (string keys and array indices; values number / string / boolean / vector). Ids inside values
// are not translated.
//
// Threading: Lua may call coop.sync.* on the game thread, which never runs during FrameMove; those calls change the
// store and queue messages, frame() (main thread) sends them. Lua functions are kept as references in the Lua registry
// and called from frame() under pcall with a traceback: an error is logged and counted, the session goes on (R-11.4).
// A new Lua state (a load) forgets every registration of the old one (register_lua).
#pragma once

#include "xrCoop/coop_session.h"

#include <optional>
#include <string>
#include <string_view>

struct lua_State;

namespace coop
{
namespace syncapi
{
// Registers the five sync messages. Called once with the manager's session.
void install(Session& s);
// The session started or ended, or a client's world is replaced: a client forgets everything of the host's, the host
// keeps world / player / object (the game's data) and drops the session scope. Requests in flight are answered
// false, "session ended".
void reset();
// Main thread, every frame of a session (sync::frame), and through its own frame pump outside one: sends what Lua
// queued (a client paced by kClientRateRules), runs the host's request handlers, delivers on_change, answers and
// effects to Lua.
void frame();
// coop_status: keys and bytes by scope, refusals, requests, effects, Lua errors.
void print_status();

// Host: the full copy for player `peer`, sent at once on the Reliable lane (coop_join host_send_save, before the
// world blob): anything changed before it goes out before it; what changes after it is queued with the rest of his
// world changes until his WorldLoaded (sync::host_begin_join) and so arrives after it.
void host_send_full(PeerId peer);
// Host and single player: object `id` was released from ALife (any game thread): its scope goes, and on every client.
// A client does nothing (the host's drop reaches it).
void on_entity_destroyed(u16 id);
// Host: player `id` left: his request limits go (his player scope stays, by his key).
void on_peer_left(PeerId id);
// A new Lua state: puts the coop.sync table into `coop` and forgets the registrations of the old state.
void register_lua(lua_State* L);

// --- C++ for the engine's own player data (F3 checkpoints) ---
// The player scope of `character` (an entity id, host numbering; 0 = the host's own actor), the scope coop.sync knows
// as "player" on that player's machine: by the player key on the character (ledger::character_key, as money and the
// spawn budget find it). Strings only. Host (and single player); nullopt / false on a client, for a character without
// a player key, a missing key or a value that is not a string.
std::optional<std::string> player_get(u16 character, std::string_view key);
// Sets it (nil is not taken: use an empty string or another key); the player gets it if he is connected. true if the
// store took it (changed or the same).
bool player_set(u16 character, std::string_view key, std::string_view value);
} // namespace syncapi
} // namespace coop
