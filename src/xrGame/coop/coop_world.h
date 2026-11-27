// Anomaly Coop — registry of player avatars (proxy NPCs) in the local world, and the lookups of this machine's objects by id.
// Host: every remote player has a proxy stalker entity (section coop_player_avatar) that
// carries the player's character in the host save. Client: the same entities are avatars of
// the other players, and our own proxy id stands for the host's avatar (role swap).
#pragma once

#include "xrCoop/coop_protocol.h"

class CSE_Abstract;
class CGameObject;
class CInventoryOwner;

namespace coop
{
namespace world
{
// Host: collect existing proxies from ALife (after hosting starts or after a load).
void host_scan_proxies();
// Host: proxy `id` now belongs to connected `peer`.
void host_mark_proxy(u16 id, PeerId peer);
// Host: the player left; the character stays where it is, inert and unkillable.
void host_park_proxy(u16 id);
// Host, F3 Task 10: the character left the world for good (a retired hardcore character is released). Its id is no
// player's character any more: ALife may give it to anything spawned later.
void host_forget_proxy(u16 id);
// Client: our host-numbered proxy id is the host's avatar here.
void client_mark_host_avatar(u16 id);
// Client: another player's avatar (id in local numbering).
void client_mark_avatar(u16 id, PeerId peer);

// R3 В3, client: the host's table (Msg::AvatarList, see join) replaces what this machine knows about who plays which
// character. Every character listed gets its player; every other one known here gets none (its player left or is
// loading). Ids in local numbering, our own character (0) not among them.
void client_set_owners(const xr_vector<AvatarOwner>& local_owners);
// F0 final review (task 8): this machine knows who plays each character. Host: always. Client: from the host's first
// AvatarList since its join (until then only the host's avatar is known); reset() forgets it.
bool owners_known();

// Any player avatar / proxy known on this machine.
bool is_player_proxy(u16 id);
// The section of every player character, docked once: compare a section with it as shared_str (a pointer compare).
const shared_str& avatar_section();
// W9: the entity / object is a player character (section coop_player_avatar): a proxy on the host, an avatar on
// clients. Known in and out of a session and before any registry here has its id. Any thread.
bool is_player_character(const CSE_Abstract* e);
bool is_player_character(const CGameObject* obj);
// Peer that drives avatar `id` (host: kHostPeer for the host avatar on clients), kInvalidPeer if none.
PeerId avatar_peer(u16 id);
// Avatar id of a peer in local numbering, 0xffff if unknown.
u16 avatar_of(PeerId peer);
// Proxies whose player is offline (host): they must stay put and take no damage.
bool is_parked(u16 id);

// ---------------------------------------------------------------- objects of this machine's world (ids in local numbering)

// The game object `id` on the loaded level and not being destroyed; nullptr for none, no level, or kNoId.
CGameObject* object_here(u16 id);
// The ALife record of `id` (online or offline); nullptr for none, no ALife, or kNoId.
CSE_Abstract* alife_entity(u16 id);
// The server's entity of `id`: the ALife record, else the level server's entity (an object ALife does not keep); nullptr for none.
CSE_Abstract* server_entity(u16 id);
// The inventory owner (the actor, a stalker, a trader, a corpse) `id` on the loaded level, one being destroyed included; nullptr for
// none, no level, or kNoId.
CInventoryOwner* find_owner(u16 id);
// `io` is a living creature.
bool alive(CInventoryOwner* io);
// The id of `io`'s game object; kNoId for none.
u16 id_of(CInventoryOwner* io);

void reset();
void print_status();
} // namespace world
} // namespace coop
