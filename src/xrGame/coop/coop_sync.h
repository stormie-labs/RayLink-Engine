// Anomaly Coop — live world synchronization after join.
// Players: each machine owns its local actor and streams snapshots; the others drive avatars.
// World: the host streams online objects (NPCs, monsters) and entity lifecycle; clients only mirror.
#pragma once

#include "xrCoop/coop_session.h"

class CGameObject;
class CSE_Abstract;

namespace coop
{
namespace sync
{
// F3a T4: what the modules set up once per process (Manager's constructor): settings, console commands, the
// route and call-gate tables. The handler tables come by role when a session starts (Manager::install_handlers).
void init();
// The world server's handlers of every module, on the hosting session.
void install_server(Session& s);
// The client's handlers of every module: on a remote client's session, or on the host player's LocalLink in the
// host's process.
void install_client(Endpoint& e);
void on_session_started();
void on_peer_joined(const PeerInfo& p);
void on_peer_left(PeerId id);
void reset();
void frame();
void print_status();
void dump_state(LPCSTR args);

// Client: entering live mode after the world loaded.
void on_client_live();

// Host: the world save was sent to `id`; world changes for it are queued until it loaded.
void host_begin_join(PeerId id);
void host_end_join(PeerId id);
bool host_peer_joining(PeerId id);

// --- engine hooks ---
// True if this game object must not think on this machine (remote NPC on a client,
// player avatar anywhere). Its movement/animation come from snapshots.
bool is_remote_driven(const CGameObject* obj);
// True on a coop client: ALife simulation (switching, scheduled updates, spawns) is frozen.
bool alife_frozen();
// Client: the local game asked its own server to create an entity. Returns true if the
// request was forwarded to the host (caller must drop it locally).
bool intercept_local_spawn(CSE_Abstract* entity);
} // namespace sync
} // namespace coop
