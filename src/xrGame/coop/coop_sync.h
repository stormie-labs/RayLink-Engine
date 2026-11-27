// Anomaly Coop — live world synchronization after join.
// Players: each machine owns its local actor and streams snapshots; the others drive avatars.
// World: the host streams online objects (NPCs, monsters) and entity lifecycle; clients only mirror.
#pragma once

#include "xrCoop/coop_session.h"

class CGameObject;

namespace coop
{
namespace sync
{
// The passes of the module table (coop_sync.cpp), each in its own order.
// What the modules set up once per process (Manager's constructor): settings, the route and call-gate tables. The handler
// tables come with the session's mode when a session starts (Manager::enter_mode).
void init();
// The world server's handlers of every module, on the hosting session.
void install_server(Session& s);
// The client's handlers of every module: on a remote client's session, or on the host player's LocalLink in the
// host's process.
void install_client(Endpoint& e);
void on_session_started();
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
} // namespace sync
} // namespace coop
