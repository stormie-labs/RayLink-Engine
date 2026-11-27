// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players, passengers (docs/VEHICLES.md sections 4, 5 and 12): beyond the module API of
// coop_vehicles.h, the hooks of the stock actor and what the decider's half (coop_vehicles_seat.cpp) asks of the places. All of it is
// implemented in coop_vehicles_ride.cpp; the pure half is xrCoop/coop_vehicle_seats.h.
#pragma once

#include "coop_vehicles.h" // riding(), ride_frame(), ride_begin()

class CActor;
class CCar;

namespace coop
{
namespace vehicles
{
// ---------------------------------------------------------------- the places of a car (configs/coop_vehicles.ltx)

// coop_vehicles_seat.cpp seat counts: the places of `car` with the driver's, 1 when its section has none (or none of its bones is in
// the model: the places end before the first seat whose bone is missing).
u8 car_seat_count(CCar* car);
// coop_vehicles_seat.cpp seat_transform for a seat >= 1: its world transform (the car's XFORM times the seat in the car's space).
// False: no such seat here.
bool passenger_seat(CCar* car, u8 seat, Fmatrix& out);
// coop_vehicles_seat.cpp exit_position, the stock point of a seat >= 1: the door of the seat (exit_<i>) beside the car, world space.
// False: no such seat here (`out` untouched).
bool passenger_exit(CCar* car, u8 seat, Fvector& out);

// ---------------------------------------------------------------- the stock actor's hooks (a player's machine)

// Actor.cpp CActor::UpdateCL, in place of cam_Update while he rides (cam_Update returns for a holder too): the first-person camera at
// his seat's eye with his free look within the stock car camera's limits. False: no ride here (the stock cam_Update runs).
bool ride_camera(CActor* actor, float fov);
// Actor.cpp CActor::Die, after the stock driver's use_HolderEx(NULL, true): a passenger leaves his seat here the same way.
void ride_actor_died();
// ActorInput.cpp CActor::IR_OnKeyboardPress (after the alive test): true swallows the key while he rides. The use key asks out
// (VehicleReq exit; the stock refusal of a locked exit stays); nothing else moves him, as the stock driver's keys go to his car only.
bool ride_key(int cmd);
// ActorInput.cpp CActor::IR_OnKeyboardHold: true swallows the key while he rides; the look keys (kUP, kDOWN, kLEFT, kRIGHT) turn
// his camera as for anybody.
bool ride_hold(int cmd);
// coop_status on a player's machine: his seat and the counters of the ride.
void ride_print_status();
} // namespace vehicles
} // namespace coop
