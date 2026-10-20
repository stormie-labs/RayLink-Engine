// Anomaly Coop — joining the host's world.
//
// Host: on JoinRequest it makes sure the player has a character (a proxy NPC in the host world,
// found by player key), writes a fresh save of the world and streams it to the client.
// Client: loads that save, so every server entity has the same id as on the host. Right after
// the save is read and before anything goes online, the client swaps roles: its local actor
// (id 0) takes the data of its own proxy, and the proxy takes the host actor's data — the proxy
// becomes the host's avatar on this machine. Then live sync starts.
#pragma once

#include "xrCoop/coop_session.h"

class CALifeObjectRegistry;

namespace coop
{
namespace join
{
// Blob kinds sent with Session::send_blob.
enum BlobKind : u32
{
	BlobWorldSave = 1,
};

void install(Session& s);

// --- host ---
bool host_can_start(); // in a level with ALife and a living actor
void on_host_started();
void on_peer_joined(const PeerInfo& p);
void on_peer_left(PeerId id); // parks the player's proxy

// Proxy (avatar) of a peer in the host world, 0xffff if none. Host only.
u16 proxy_of_peer(PeerId id);
PeerId peer_of_proxy(u16 proxy_id);
bool is_proxy(u16 id);

// --- client ---
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
} // namespace join
} // namespace coop
