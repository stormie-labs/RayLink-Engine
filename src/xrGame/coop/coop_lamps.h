// Anomaly Coop — hanging lamps across machines (parity audit W2, 2026-10-31).
//
// CHangingLamp has no sync of its own (net_Export is empty): a lamp shot out or switched by a host-only script went dark on the
// host and stayed lit on the clients, and a client who shot a lamp saw nothing, his hit is a claim the host applies. The host
// is the lamp's owner: what happens to a lamp there (CHangingLamp::TurnOn / TurnOff / Hit) goes out as Msg::LampState, its
// last word on every lamp that changed in this world to a client that enters (the save carries the state before that).
// A client's script does not switch its copy (the gate of hanging_lamp:turn_on / turn_off). A flicker stays each
// machine's own (bind_dynamic_light: a psi monster near, the emission): a client holds a lamp to the host's word once it ends.
//
// Rules and wire: xrCoop/coop_relay_rules.h. Wiring rides on presence (no own line in coop_sync).
// Threading: on_changed may run on the game thread (Lua) and only records; frame() (main thread) sends and applies.
#pragma once

#include "xrCoop/coop_session.h"

class CHangingLamp;

namespace coop
{
namespace lamps
{
void install_server(Session& s);
void install_client(Endpoint& e);
void reset();
void frame(); // the world is up
void world_down(); // the world is gone or in the making: the ids of its lamps mean nothing now
void print_status();

// CHangingLamp, host: the lamp went on or off, or died. `soft`: it lit itself at the end of a flicker.
void on_changed(CHangingLamp* lamp, bool soft = false);
} // namespace lamps
} // namespace coop
