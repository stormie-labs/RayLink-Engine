// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players, a car's state on every copy and the dolls of sitters (docs/VEHICLES.md sections 3,
// 5.4, 5.6 and 11): the hooks of the stock code beyond the module API of coop_vehicles.h (state_frame, state_on_state). All of it is
// implemented in coop_vehicles_state.cpp; the pure half is xrCoop/coop_vehicle_state_rules.h.
#pragma once

class CCar;
class CAI_Stalker;

namespace coop
{
namespace vehicles
{
// CarSound.cpp, CCar::SCarSound::UpdateDrive (one line, after the stock pitch and before its clamp): on a player's copy that has a
// VehicleState, `scale` becomes the pitch of the decider's rpm (the stock formula, 0.5 + 0.5 * rpm / torque rpm). The copy's own
// m_current_rpm is its physics step's and is left alone. Nothing anywhere else (the decider, single player, a car with no state).
void copy_sound_scale(CCar* car, float& scale);
// coop_players.cpp drive_avatar (one line, before the decider's seated() test and the snapshots): `doll`, a player's character on
// this machine, sits in a car: on a player's machine it is put at seat_transform(car, seat) of the copy (the mirror of the decider's
// seat table says which), on the decider its proxy is where coop_vehicles_seat.cpp parks it. Either way it plays the
// vehicle idle of its own animations, else it is hidden. True: placed, his snapshots are not applied. Back to normal
// on release (his motions again, shown).
bool doll_frame(CAI_Stalker* doll);
// coop_status: what this machine sent, played and posed (the cars watched, the states, the dolls).
void state_print_status();
} // namespace vehicles
} // namespace coop
