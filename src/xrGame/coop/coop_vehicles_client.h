// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players, a player's machine (docs/VEHICLES.md sections 5, 7 and 14): the hooks of the stock car
// input and of the script's attach beyond the module API of coop_vehicles.h. All of it is implemented in coop_vehicles_client.cpp; the
// pure half is xrCoop/coop_vehicle_input_rules.h.
#pragma once

class CCar;
class CGameObject;

namespace coop
{
namespace vehicles
{
struct Seated;

// CarInput.cpp, CCar::OnKeyboardPress / OnKeyboardRelease (one line each, after the stock Remote() test). True: the decider's key of a
// player's copy (drive, brake, gears, engine, lights, the weapon's switch and fire): the car he drives takes it into the next VehicleInput
// and the stock code below never runs on the copy; false: the stock code runs (the camera keys, kUSE, keys the car ignores, any car
// that is no copy).
bool client_key(CCar* car, int key, bool pressed);
// CarInput.cpp, CCar::OnMouseMove (one line, before the stock aim of the weapon): on the copy he drives, with the car weapon's switch on,
// the aim as the stock takes it at a mouse move (the point the camera looks at, seen from the weapon's fire point) for the next
// VehicleInput. The stock block after it never aims a copy: the copy's weapon is never switched on here.
void client_mouse_aim(CCar* car);
// coop_events.cpp client_refuse_vehicle (a script's attach_vehicle that the call gate lets run) and the stand's coop_vehicle_enter: a
// VehicleReq enter for any seat of `car` without the stock door test of F (the script or the stand chose the car; the decider checks
// the distance). True: a request went out or one waits; false: no player's copy of a car here, nothing sent.
bool client_request_enter(CGameObject* car);
// coop_vehicle_status full: the counters of this machine's half (requests, answers, inputs, releases) and its seat.
void client_print_status();
// For the dolls of sitters (coop_vehicles_state.cpp): where `character` (host numbering) sits by this machine's mirror of the
// decider's seat table (VehicleSeat); false: nowhere, or this is no player's machine (the mirror is empty there).
bool mirror_seat_of(u16 character, Seated& out);
} // namespace vehicles
} // namespace coop
