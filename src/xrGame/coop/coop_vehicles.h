// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players (docs/VEHICLES.md): the module API. Declarations only.
//
// A player drives, rides and gets in and out of a car; the process that decides for his level is the authority: the co-op
// host's process, or in the Server mode the map server of his level (the world server tunnels the four messages, Scope::Map).
// The same code runs in both modes; what differs is only who receives (the mode's delivery). Pure rules and the four
// message layouts: xrCoop/coop_vehicle_rules.h.
//
// The module is four .cpp files; the section of each is named below:
//   coop_vehicles_seat.cpp    the decider: requests, the seat table, driving without an owner, releases, the save
//   coop_vehicles_client.cpp  a player's machine: request instead of the refusal, sitting in the kinematic copy,
//                             keys -> VehicleInput, exit, console and Lua
//   coop_vehicles_state.cpp   the car's state on every copy (sound, lights, exhaust, damage), the dolls of sitters
//   coop_vehicles_ride.cpp    passengers: seats per model (configs/coop_vehicles.ltx), the ride mode
// The hubs register install_server / install_client, the four frames (server_frame, client_frame, state_frame,
// ride_frame), register_lua and register_console in the hubs (coop_sync.cpp, coop_script.cpp, the console), the same way
// as coop_money.h / coop_levels.h: both modes install them. Every install starts from an empty state (a new session, a new
// world); the decider's end of a session is session_over() (sync::reset).
//
// The messages (ids: coop_protocol.h): VehicleReq and VehicleInput are a player's (ClientOnly), VehicleSeat and
// VehicleState the decider's (HostOnly). Send through the mode's delivery, never to peers picked here (docs/MODES.md §3).
#pragma once

#include "xrCoop/coop_vehicle_rules.h"

struct lua_State;
class CCar;
class CGameObject;

namespace coop
{
class Session;
class Endpoint;

namespace vehicles
{
// ---------------------------------------------------------------- the decider: coop_vehicles_seat.cpp

// The handlers of VehicleReq and VehicleInput on the deciding session (co-op: the host's; Server: the map server's
// session of its level, whose peers are the players there). Starts with an empty SeatTable.
void install_server(Session& s);
// Main thread, every frame of the decider: releases (death, wounded, disconnect, leaving the level, the car
// destroyed or gone) with a VehicleSeat released each, stale input (InputTrack::tick -> the stock calls), each seated proxy
// parked at its seat_transform.
void server_frame();
// `character` holds a seat here. coop_players.cpp applies no position snapshot of his player to his proxy while
// true (one query line).
bool seated(u16 character);
// The character in seat 0 of `car`, kNoCharacter when free. For CCar::Initiator (a client driver's kills are
// his, as the stock Owner's) and the own actor's gate below.
u16 driver_of(u16 car);
// The world transform (seats >= 1: coop_vehicles_ride.cpp, in the same function) of `seat` of `car`. Seat 0: the bone
// car_definition driver_place of the model's user data, else the root, as CCar::attach_Actor picks it. False: no such seat.
bool seat_transform(CCar* car, u8 seat, Fmatrix& out);
// A seated character is saved standing at his exit position (where a release would put him): true and `out` when
// he sits, false otherwise. Called where a player character's position goes into the save (co-op: the host's save of his
// proxy; Server: the body facts the map server reports for the world server's save).
bool save_position(u16 character, Fvector& out);
// CActor::attach_Vehicle / detach_Vehicle hooks (Actor.cpp, one line each). The stock path of the actor of the
// process that decides for it (the co-op host's own actor) is recorded in the SeatTable as seat 0; nothing on a player's
// machine (its own actor sits in a copy after a granted: coop_vehicles_client.cpp) and nothing for a server's ghost
// (it never drives).
void own_actor_attached(CCar* car);
void own_actor_detached(CCar* car);
// Actor_Events.cpp GEG_PLAYER_ATTACH_HOLDER gate (one line): false when a client holds seat 0 of `car`. On the
// decider a client's car has no Owner, so the stock Engaged() test alone would let the own actor in.
bool own_actor_may_attach(CCar* car);
// The coop_status section: seated characters, cars driven, requests granted / refused by reason, stale inputs,
// inputs over the rate, trust lines.
void print_status();
// The co-op group's transition starts (coop_join.cpp, before the movers load the next level): every client's seat is
// released with `why` (Release::LeftLevel), a VehicleSeat released each; the own actor keeps the stock path.
void release_all_seats(Release why);
// sync::reset() (Manager::stop, a lost session; main thread): server_frame runs no more once the session is over. The
// cars driven by clients are parked as detach_Actor parks them, the proxies stand up, the table is forgotten; nothing is sent.
void session_over();

// ---------------------------------------------------------------- a player's machine: coop_vehicles_client.cpp

// The handlers on a player's machine: VehicleSeat here (the mirror SeatTable, my own seat), VehicleState to
// state_on_state (coop_vehicles_state.cpp).
void install_client(Endpoint& e);
// Main thread, every frame: VehicleInput of the car I drive (at a change, every kInputRepeatMs, at most one per
// kInputMinIntervalMs), the answer timeout (kAnswerTimeoutMs).
void client_frame();
// The local player mirror still drives or rides in a managed seat, including after release until physical detach.
bool local_seat_attached();
// coop.vehicle.status() -> seat or nil, { requests, refused }; attached() -> local_seat_attached(); exit() asks out.
void register_lua(lua_State* L);
// The stand's console commands coop_vehicle_enter <id|nearest> [seat], coop_vehicle_drive <bits> <ms>,
// coop_vehicle_exit, coop_vehicle_status.
void register_console();
// `car` is a kinematic copy here (this machine's objects are copies) in which the local actor sits: the hooks in
// CCar::attach_Actor / detach_Actor skip the physics parts (Enable, processing, brakes) and CarInput.cpp sends the drive keys
// as VehicleInput instead of applying them.
bool mirror_copy(const CCar* car);
// GEG_PLAYER_ATTACH_HOLDER of the own actor to a car on a player's machine (coop_events.cpp). True: a VehicleReq
// went out (or one is pending), swallow the stock event; false: the stock refusal path stays.
bool client_on_use(CGameObject* car);
// The car the local actor drives (seat 0), kNoCar when none. coop_objects.cpp extrapolates that copy's latest
// pose instead of the 100 ms buffer.
u16 driving_car();

// ---------------------------------------------------------------- the car's state: coop_vehicles_state.cpp

// Main thread, every frame of the decider: VehicleState of every car with a sitter or a running engine (the
// cadence and quantization: coop_vehicle_state_rules.h), and the dolls of sitters on this machine.
void state_frame();
// A VehicleState on a player's machine (from install_client): sound, lights, exhaust and damage of the copy with
// the stock methods; never its physics or health.
void state_on_state(Reader& payload);

// ---------------------------------------------------------------- passengers: coop_vehicles_ride.cpp

// Main thread, every frame on a player's machine: the actor riding in a seat >= 1 follows its seat_transform.
void ride_frame();
// The local actor rides as a passenger (seat >= 1).
bool riding();
// What the ride needs of the seat record of a player's machine (coop_vehicles_client.cpp keeps it): whether it still holds
// `seat` of `car` (a release, a loss or a new world ends it), and the request out of it (VehicleReq exit, at its next free slot).
struct RideLink
{
	bool (*still_mine)(u16 car, u8 seat) = nullptr;
	void (*ask_out)() = nullptr;
};
// VehicleSeat granted to this machine's actor for a seat >= 1 (coop_vehicles_client.cpp, the Ride step): he sits there at
// the next ride_frame (coop_vehicles_ride.cpp; the hooks of the stock actor: coop_vehicles_ride.h).
void ride_begin(u16 car, u8 seat, const RideLink& link);
// VehicleSeat released for this machine's actor in a seat >= 1 (coop_vehicles_client.cpp, the Leave step):
// the point the decider stood his character at; the ride stands him there (docs/VEHICLES.md 5.8). Not the seat he rides in here:
// nothing.
void ride_exit_point(u16 car, u8 seat, const Fvector& at);
// sync::reset(), next to session_over(): ride_frame runs no more once the session is over. A live
// passenger still seated in this world stands up (the stock detach's lines, no box test); the ride is forgotten either way.
void ride_session_over();
} // namespace vehicles
} // namespace coop
