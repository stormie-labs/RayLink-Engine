// Anomaly Coop — vehicles for players (docs/VEHICLES.md sections 5 and 7): a player's machine, the pure half.
//
// The player sits in the kinematic copy of the car on his own machine; the process that decides for his level (the co-op host,
// in the Server mode the map server of his level) drives the real car. These rules are the same in both modes:
//   - which keys of the stock car input (CarInput.cpp, CCar::OnKeyboardPress / OnKeyboardRelease / OnKeyboardHold) are the
//     decider's: they go into VehicleInput and the copy never runs them; the camera keys stay on this machine;
//   - InputSender: the held keys, the press counters and the weapon's aim of the car the local actor drives -> the next
//     VehicleInput, at a change (never sooner than kInputMinIntervalMs after the last one) and every kInputRepeatMs while he
//     drives, keys held or not (a silent driver means a lost link on the decider: InputTrack brakes after kInputStaleMs);
//   - RequestTrack: one seat request at a time, never sooner than kRequestMinIntervalMs after the last one (the decider's trust
//     check veh_request_rate), forgotten after kAnswerTimeoutMs without an answer (the HUD says so);
//   - seat_step: what a VehicleSeat of the decider means for this machine's own actor.
// Message layouts, the seat table and every constant named here: coop_vehicle_rules.h.
//
// Pure C++17, header-only, no engine dependencies: used by xrGame/coop/coop_vehicles_client.cpp and tests/test_vehicle_input.cpp.
#pragma once

#include <cstdint>

#include "coop_vehicle_rules.h"

namespace coop
{
namespace vehicles
{
// ---------------------------------------------------------------- the keys of the stock car input

// The EGameActions values (xrGame/xr_level_controller.h, an enum without explicit values) of the keys the car's switches name.
// tests/test_vehicle_input.cpp reads the enum from the source and compares; coop_vehicles_client.cpp static_asserts each one
// against the engine's enum. Inside namespace keys the stock names are the stock names.
namespace keys
{
constexpr int kLEFT = 0; // OnKeyboardHold: active_camera->Move
constexpr int kRIGHT = 1;
constexpr int kUP = 2;
constexpr int kDOWN = 3;
constexpr int kJUMP = 4; // the hand brake
constexpr int kCROUCH = 5; // a gear down
constexpr int kACCEL = 6; // a gear up
constexpr int kFWD = 8;
constexpr int kBACK = 9;
constexpr int kL_STRAFE = 10;
constexpr int kR_STRAFE = 11;
constexpr int kCAM_1 = 14;
constexpr int kCAM_2 = 15;
constexpr int kCAM_3 = 16;
constexpr int kCAM_ZOOM_IN = 17; // OnKeyboardHold
constexpr int kCAM_ZOOM_OUT = 18;
constexpr int kTORCH = 19; // the head lights
constexpr int kDETECTOR = 21; // the engine
constexpr int kWPN_FIRE = 30;
constexpr int kWPN_ZOOM = 31; // CAR_NEW: m_zoom_status, the camera's zoom
constexpr int kWPN_FUNC = 35; // the car weapon's switch
constexpr int kUSE = 40; // the car does nothing with it; the actor's ActorUse asks out (GEG_PLAYER_DETACH_HOLDER)
} // namespace keys

enum class KeyUse : uint8_t
{
	Local, // the copy runs the stock code: the camera, kUSE, every key the car's switches ignore
	Held, // a held bit of VehicleInput: the decider makes the stock press / release calls of the key
	Edge, // a press counter of VehicleInput (Edge): the decider makes the stock call once per press
	WeaponSwitch, // kWPN_FUNC: toggles kHeldWeapon (with a weapon), as the stock Action(eWpnActivate, !IsActive())
};

struct KeyRule
{
	int key;
	const char* name; // the EGameActions name
	KeyUse use;
	uint16_t held; // Held, WeaponSwitch: the bit of VehicleInput.held
	Edge edge; // Edge: the counter of VehicleInput.edges
	const char* press; // what the case of the key in CCar::OnKeyboardPress calls ("" nothing)
	const char* release; // what its case in CCar::OnKeyboardRelease calls ("" nothing, or no case)
	bool needs_weapon; // the stock call is under `if (HasWeapon())`
};

// The case of every key in CarInput.cpp's OnKeyboardPress / OnKeyboardRelease (the CAR_NEW zoom switch included) and the look
// keys of OnKeyboardHold. A key that is in none of them is Local too (key_use): the car's switches do nothing with it.
constexpr KeyRule kKeyRules[] = {
	// the camera: stays on this machine (the copy runs it)
	{keys::kCAM_1, "kCAM_1", KeyUse::Local, 0, Edge::Engine, "OnCameraChange(ectFirst)", "", false},
	{keys::kCAM_2, "kCAM_2", KeyUse::Local, 0, Edge::Engine, "OnCameraChange(ectChase)", "", false},
	{keys::kCAM_3, "kCAM_3", KeyUse::Local, 0, Edge::Engine, "OnCameraChange(ectFree)", "", false},
	{keys::kWPN_ZOOM, "kWPN_ZOOM", KeyUse::Local, 0, Edge::Engine, "m_zoom_status", "m_zoom_status", false},
	{keys::kCAM_ZOOM_IN, "kCAM_ZOOM_IN", KeyUse::Local, 0, Edge::Engine, "", "", false},
	{keys::kCAM_ZOOM_OUT, "kCAM_ZOOM_OUT", KeyUse::Local, 0, Edge::Engine, "", "", false},
	{keys::kUP, "kUP", KeyUse::Local, 0, Edge::Engine, "", "", false},
	{keys::kDOWN, "kDOWN", KeyUse::Local, 0, Edge::Engine, "", "", false},
	{keys::kLEFT, "kLEFT", KeyUse::Local, 0, Edge::Engine, "", "", false},
	{keys::kRIGHT, "kRIGHT", KeyUse::Local, 0, Edge::Engine, "", "", false},
	// the car does nothing with it (`case kUSE: break;`): the actor's own ActorUse asks out
	{keys::kUSE, "kUSE", KeyUse::Local, 0, Edge::Engine, "", "", false},
	// the decider's
	{keys::kACCEL, "kACCEL", KeyUse::Held, kHeldTransUp, Edge::Engine, "TransmissionUp()", "", false},
	{keys::kCROUCH, "kCROUCH", KeyUse::Held, kHeldTransDown, Edge::Engine, "TransmissionDown()", "", false},
	{keys::kFWD, "kFWD", KeyUse::Held, kHeldForward, Edge::Engine, "PressForward()", "ReleaseForward()", false},
	{keys::kBACK, "kBACK", KeyUse::Held, kHeldBack, Edge::Engine, "PressBack()", "ReleaseBack()", false},
	{keys::kR_STRAFE, "kR_STRAFE", KeyUse::Held, kHeldRight, Edge::Engine, "PressRight()", "ReleaseRight()", false},
	{keys::kL_STRAFE, "kL_STRAFE", KeyUse::Held, kHeldLeft, Edge::Engine, "PressLeft()", "ReleaseLeft()", false},
	{keys::kJUMP, "kJUMP", KeyUse::Held, kHeldBrake, Edge::Engine, "PressBreaks()", "ReleaseBreaks()", false},
	{keys::kDETECTOR, "kDETECTOR", KeyUse::Edge, 0, Edge::Engine, "SwitchEngine()", "", false},
	{keys::kWPN_FUNC, "kWPN_FUNC", KeyUse::WeaponSwitch, kHeldWeapon, Edge::Engine,
	    "m_car_weapon->Action(CCarWeapon::eWpnActivate, !m_car_weapon->IsActive())", "", true},
	{keys::kTORCH, "kTORCH", KeyUse::Edge, 0, Edge::Lights, "m_lights.SwitchHeadLights()", "", false},
	{keys::kWPN_FIRE, "kWPN_FIRE", KeyUse::Held, kHeldFire, Edge::Engine, "m_car_weapon->Action(CCarWeapon::eWpnFire, 1)",
	    "m_car_weapon->Action(CCarWeapon::eWpnFire, 0)", true},
};

inline const KeyRule* key_rule(int key)
{
	for (const KeyRule& r : kKeyRules)
		if (r.key == key)
			return &r;
	return nullptr;
}

// Local for a key no rule names: the car's switches ignore it, the stock code may run (it does nothing).
inline KeyUse key_use(int key)
{
	const KeyRule* r = key_rule(key);
	return r ? r->use : KeyUse::Local;
}

// ---------------------------------------------------------------- the driver's input (this machine)

// The input of the car the local actor drives, from his granted of seat 0 to his release. press / release take the keys of
// CCar::OnKeyboardPress / OnKeyboardRelease of the copy; due / take make the VehicleInput stream of docs/VEHICLES.md section 7.
class InputSender
{
public:
	// His granted of seat 0 of `car` (host numbering): seq starts again (the first message is 1), the counters at 0, no key
	// held, no aim. The first message is due at once.
	void start(uint16_t car, bool has_weapon)
	{
		*this = InputSender();
		m_car = car;
		m_weapon = has_weapon;
		m_driving = true;
	}

	void stop() { *this = InputSender(); }

	// A key of the copy pressed. True: the decider's key (the caller does not run the stock code on the copy, whether or not he
	// drives now); false: Local, the stock code of the copy runs it.
	bool press(int key)
	{
		const KeyRule* r = key_rule(key);
		if (!r || r->use == KeyUse::Local)
			return false;
		if (!m_driving || (r->needs_weapon && !m_weapon))
			return true; // the stock call would be under `if (HasWeapon())`: nothing
		switch (r->use)
		{
		case KeyUse::Held: m_held = uint16_t(m_held | r->held); break;
		case KeyUse::Edge: m_edges = bump_edge(m_edges, r->edge); break;
		case KeyUse::WeaponSwitch: m_held = uint16_t(m_held ^ kHeldWeapon); break;
		default: break;
		}
		return true;
	}

	// A key of the copy released. The stock release does nothing for a counter or the weapon switch, and nothing for a gear
	// (`case kACCEL: break;`); the bit of a gear key still goes, so that the next press is a press again.
	bool release(int key)
	{
		const KeyRule* r = key_rule(key);
		if (!r || r->use == KeyUse::Local)
			return false;
		if (m_driving && r->use == KeyUse::Held)
			m_held = uint16_t(m_held & ~r->held);
		return true;
	}

	// The aim of the car's weapon (heading, pitch of the line from the weapon's fire point to the point the camera looks at,
	// radians), as the stock CCar::OnMouseMove takes it while the weapon is on. Nothing without a weapon.
	void set_aim(float yaw, float pitch)
	{
		if (!m_driving || !m_weapon)
			return;
		m_has_aim = true;
		m_yaw = angle_to_wire(yaw);
		m_pitch = pitch_to_wire(pitch);
	}

	// The stand's console (coop_vehicle_drive): these held bits instead of the keys until `until_ms`, then the keys again.
	void force(uint16_t held, uint32_t now_ms, uint32_t for_ms)
	{
		if (!m_driving)
			return;
		m_forced = true;
		m_force_held = uint16_t(held & (m_weapon ? kHeldState : uint16_t(kHeldState & ~(kHeldFire | kHeldWeapon))));
		m_force_until = now_ms + for_ms;
	}

	// The held bits of a message made at `now_ms` (kHeldState only).
	uint16_t held_at(uint32_t now_ms) const
	{
		if (m_forced && int32_t(m_force_until - now_ms) > 0)
			return m_force_held;
		return m_held;
	}

	// A message is due now: the first one at once; a change no sooner than kInputMinIntervalMs after the last one; anything
	// every kInputRepeatMs.
	bool due(uint32_t now_ms) const
	{
		if (!m_driving)
			return false;
		if (!m_sent_any)
			return true;
		const uint32_t since = uint32_t(now_ms - m_sent_ms);
		if (since >= kInputRepeatMs)
			return true;
		return since >= kInputMinIntervalMs && changed(now_ms);
	}

	// The message to send now; it counts as sent (seq + 1, wraps 255 -> 0).
	InputMsg take(uint32_t now_ms)
	{
		InputMsg m;
		m.car = m_car;
		m.seq = ++m_seq;
		m.held = held_at(now_ms);
		m.edges = m_edges;
		if (m_weapon && m_has_aim)
		{
			m.held = uint16_t(m.held | kAimFollows);
			m.aim_yaw = m_yaw;
			m.aim_pitch = m_pitch;
		}
		m_sent_any = true;
		m_sent_ms = now_ms;
		m_sent_held = held_at(now_ms);
		m_sent_edges = m_edges;
		m_sent_aim = m_has_aim;
		m_sent_yaw = m_yaw;
		m_sent_pitch = m_pitch;
		if (m_forced && int32_t(m_force_until - now_ms) <= 0)
			m_forced = false;
		return m;
	}

	bool driving() const { return m_driving; }
	uint16_t car() const { return m_car; }
	bool has_weapon() const { return m_weapon; }
	uint16_t held() const { return m_held; }
	uint8_t edges() const { return m_edges; }
	uint8_t seq() const { return m_seq; }

private:
	bool changed(uint32_t now_ms) const
	{
		return held_at(now_ms) != m_sent_held || m_edges != m_sent_edges || m_has_aim != m_sent_aim ||
		       (m_has_aim && (m_yaw != m_sent_yaw || m_pitch != m_sent_pitch));
	}

	uint16_t m_car = kNoCar;
	bool m_driving = false;
	bool m_weapon = false;
	uint16_t m_held = 0;
	uint8_t m_edges = 0;
	bool m_has_aim = false;
	int16_t m_yaw = 0;
	int16_t m_pitch = 0;
	bool m_forced = false;
	uint16_t m_force_held = 0;
	uint32_t m_force_until = 0;
	uint8_t m_seq = 0;
	bool m_sent_any = false;
	uint32_t m_sent_ms = 0;
	uint16_t m_sent_held = 0;
	uint8_t m_sent_edges = 0;
	bool m_sent_aim = false;
	int16_t m_sent_yaw = 0;
	int16_t m_sent_pitch = 0;
};

// ---------------------------------------------------------------- seat requests (this machine)

// One VehicleReq at a time. A new one goes out when none waits and kRequestMinIntervalMs passed since the last (a quicker one
// would only be written by the decider's trust check veh_request_rate). A request with no answer for kAnswerTimeoutMs is
// forgotten (Server: the map server of his level may be starting, or not connected: the world server drops a map stream then).
class RequestTrack
{
public:
	bool may_send(uint32_t now_ms) const
	{
		return !m_pending && (!m_sent_any || uint32_t(now_ms - m_sent_ms) >= kRequestMinIntervalMs);
	}

	void sent(ReqOp op, uint16_t car, uint32_t now_ms)
	{
		m_pending = true;
		m_op = op;
		m_car = car;
		m_sent_any = true;
		m_sent_ms = now_ms;
	}

	// A VehicleSeat here. True when it answers the waiting request, which is forgotten then: a refused about its car (a
	// refused goes to the asker only), a granted or released of `me` (my character, host numbering) in its car.
	bool answer(const SeatMsg& m, uint16_t me)
	{
		if (!m_pending || m.car != m_car)
			return false;
		const bool mine = m.op == SeatOp::Refused || (me != kNoCharacter && m.character == me);
		if (mine)
			m_pending = false;
		return mine;
	}

	// Every frame: true once, when the waiting request had no answer for kAnswerTimeoutMs (it is forgotten).
	bool timed_out(uint32_t now_ms)
	{
		if (!m_pending || uint32_t(now_ms - m_sent_ms) < kAnswerTimeoutMs)
			return false;
		m_pending = false;
		return true;
	}

	bool pending() const { return m_pending; }
	ReqOp op() const { return m_op; }
	uint16_t car() const { return m_car; }
	void reset() { *this = RequestTrack(); }

private:
	bool m_pending = false;
	ReqOp m_op = ReqOp::Enter;
	uint16_t m_car = kNoCar;
	bool m_sent_any = false;
	uint32_t m_sent_ms = 0;
};

// ---------------------------------------------------------------- what a VehicleSeat means for this machine's actor

enum class SeatStep : uint8_t
{
	None, // not about me (the mirror table takes it), or my seat said again
	Attach, // granted to me, seat 0: sit in the copy (the stock CActor::attach_Vehicle(car, true))
	Ride, // granted to me, a seat >= 1: the passenger's ride (coop_vehicle_seats.h)
	Leave, // released for me: out, at the decider's exit position
	Lost, // granted to another character in the seat I hold: the decider has me out of it
	Refused, // refused: RequestTrack says whether it was my request
};

// `me`: my character (host numbering); `mine`: the seat this machine has me in (.car kNoCar: none).
inline SeatStep seat_step(const SeatMsg& m, uint16_t me, const Seated& mine)
{
	switch (m.op)
	{
	case SeatOp::Refused: return SeatStep::Refused;
	case SeatOp::Granted:
		if (me != kNoCharacter && m.character == me)
		{
			if (mine.car == m.car && mine.seat == m.seat)
				return SeatStep::None; // said again (a player who comes to the map gets every taken seat)
			return m.seat == kDriverSeat ? SeatStep::Attach : SeatStep::Ride;
		}
		if (mine.car != kNoCar && mine.car == m.car && mine.seat == m.seat)
			return SeatStep::Lost;
		return SeatStep::None;
	case SeatOp::Released: return me != kNoCharacter && m.character == me ? SeatStep::Leave : SeatStep::None;
	}
	return SeatStep::None;
}
} // namespace vehicles
} // namespace coop
