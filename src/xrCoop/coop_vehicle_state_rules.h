// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players, a car's state on every copy (F5-VEH-4, build 57, docs/VEHICLES.md sections 3, 5.6, 11): the
// pure half.
//
// The process that decides for a level (the co-op host; in the Server mode the map server of the level) sends VehicleState for every
// car with a sitter or a running engine: at a change, and every kStateRepeatMs while the engine runs (the rpm). Every player's machine
// plays it on its kinematic copy of the car with the stock methods: the engine (CCar::StartEngine / StopEngine: the start and stop
// sounds, the exhaust), the engine sound at the decider's rpm (CCarSound::UpdateDrive), the head lights (TurnOn / TurnOffHeadLights),
// the damage particles (Play1 / Play2, as CCar::ApplyDamage plays them), the transmission switch sound. Nothing changes the copy's
// physics or health: the health is carried for coop_status only. The same rules in both modes: they do not know who decides.
//   - quantization of the stock fields into the 8 bytes of VehicleState (coop_vehicle_rules.h StateMsg) and back;
//   - StateCadence: when the decider sends a car's state (a change, the 2 Hz repeat, silent when parked);
//   - CopyTrack: what a copy does with a state (the plan of stock calls), the rpm its engine sound uses between two states;
//   - doll_pose: a sitter's doll on another machine plays the vehicle idle of its model, or is hidden;
//   - TapLatch (rv_F5-VEH-3, the driver's machine): a held key tapped between two VehicleInput messages is not lost.
//
// Pure C++17, header-only, no engine dependencies: used by the engine (xrGame/coop/coop_vehicles_state.cpp, coop_vehicles_client.cpp)
// and by the unit-test exe (tests/test_vehicle_state.cpp).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "coop_vehicle_rules.h"

namespace coop
{
namespace vehicles
{
// ---------------------------------------------------------------- cadence and bounds

// While a car's engine runs its state goes again this often: the rpm (2 Hz, docs/VEHICLES.md section 3).
constexpr uint32_t kStateRepeatMs = 500;
// A car's state goes at most once per this many ms (10 a second): a change sooner waits for its slot (a car under fire changes its
// health with every bullet; VehicleState is Reliable).
constexpr uint32_t kStateMinIntervalMs = 100;
// The decider looks for the cars of its level this often (a car spawned meanwhile is seen within a second).
constexpr uint32_t kStateScanMs = 1000;
// The decider's frames stopped for longer than this (no session, no world): what it sent before is forgotten, the players of the
// next frames get every state again.
constexpr uint32_t kStateGapMs = 2000;
// Cars one machine keeps a state of (a level of GAMMA has a few dozen; a decider's word beyond this is dropped and counted).
constexpr size_t kMaxStateCars = 512;

// ---------------------------------------------------------------- quantization

namespace detail
{
inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
} // namespace detail

// `value` of `of` as 0..255, rounded (0 when `of` is not a positive number or `value` is not a number).
inline uint8_t fraction_byte(float value, float of)
{
	if (!std::isfinite(value) || !std::isfinite(of) || !(of > 0.f))
		return 0;
	return uint8_t(std::lround(detail::clamp01(value / of) * 255.f));
}

inline float fraction_of(uint8_t q) { return float(q) / 255.f; }

// rpm: CCar::m_current_rpm of m_max_rpm. Back: the copy's m_max_rpm (the same model) times the fraction.
inline uint8_t rpm_byte(float rpm, float max_rpm) { return fraction_byte(rpm, max_rpm); }
inline float rpm_of(uint8_t q, float max_rpm) { return fraction_of(q) * max_rpm; }

// health: CEntity::GetfHealth, 1.0 a whole car.
inline uint8_t health_byte(float health) { return fraction_byte(health, 1.f); }

// fuel: m_fuel of m_fuel_tank, rounded UP: a tank with any fuel is never 0 here, because the copy's CCar::StartEngine refuses an
// empty tank (Car.cpp:1214 `if (m_fuel < EPS ...) return`) and its UpdateFuel stops the engine at 0 (:1897).
inline uint8_t fuel_byte(float fuel, float tank)
{
	if (!std::isfinite(fuel) || !std::isfinite(tank) || !(fuel > 0.f) || !(tank > 0.f))
		return 0;
	const long q = long(std::ceil(detail::clamp01(fuel / tank) * 255.f));
	return uint8_t(q < 1 ? 1 : (q > 255 ? 255 : q));
}
inline float fuel_of(uint8_t q, float tank) { return fraction_of(q) * tank; }

// gear: -1 reverse (the stock transmission 0: CCar::DriveBack), 0 neutral (e_state_drive == neutral: no gear drives the wheels),
// 1..kMaxGear the forward gear (m_current_transmission_num).
inline int8_t gear_of(uint32_t transmission, bool neutral)
{
	if (neutral)
		return 0;
	if (transmission == 0)
		return -1;
	return int8_t(transmission > uint32_t(kMaxGear) ? kMaxGear : transmission);
}

// damage: CDamagableItem::m_level_applied of the car, the stages whose effects ApplyDamage played (0..3; u16(-1) before Init: 0).
inline uint8_t damage_stage(uint16_t applied)
{
	if (applied == uint16_t(-1))
		return 0;
	return uint8_t(applied > kMaxDamageStage ? kMaxDamageStage : applied);
}

// What the decider reads of a car (the stock fields, raw).
struct CarReading
{
	bool engine = false; // isActiveEngine (b_engine_on)
	bool lights = false; // a head light of m_lights shines (SCarLight::isOn)
	bool handbrake = false; // brp, or the car has no driver (CCar::Init and detach_Actor hold it, attach_Actor lets it go)
	float rpm = 0.f; // m_current_rpm
	float max_rpm = 0.f; // m_max_rpm
	uint32_t transmission = 1; // m_current_transmission_num
	bool neutral = true; // e_state_drive == neutral
	float health = 1.f; // GetfHealth
	float fuel = 0.f; // m_fuel
	float tank = 0.f; // m_fuel_tank
	uint16_t damage = 0; // m_level_applied
	bool sitter = false; // the stock Owner (the decider's own actor), or a player in the decider's seat table
};

inline StateMsg state_of(uint16_t car, const CarReading& r)
{
	StateMsg m;
	m.car = car;
	m.flags = uint8_t((r.engine ? kStateEngine : 0) | (r.lights ? kStateLights : 0) | (r.handbrake ? kStateHandbrake : 0));
	m.rpm = rpm_byte(r.rpm, r.max_rpm);
	m.gear = gear_of(r.transmission, r.neutral);
	m.health = health_byte(r.health);
	m.fuel = fuel_byte(r.fuel, r.tank);
	m.damage = damage_stage(r.damage);
	return m;
}

// A car is "live" while somebody sits in it or its engine runs: its state goes out (docs/VEHICLES.md section 3).
inline bool live(const CarReading& r) { return r.engine || r.sitter; }

// Two states differ in something but the rpm (the rpm alone goes at the repeat, else a running engine would send every frame).
inline bool same_but_rpm(const StateMsg& a, const StateMsg& b)
{
	return a.car == b.car && a.flags == b.flags && a.gear == b.gear && a.health == b.health && a.fuel == b.fuel &&
	       a.damage == b.damage;
}

// ---------------------------------------------------------------- the decider: when a car's state goes

enum class StateSend : uint8_t
{
	None, // nothing now
	Change, // something but the rpm changed (or the first state of a car that became live)
	Repeat, // the engine runs: the 2 Hz repeat, the rpm
};

inline const char* send_name(StateSend s)
{
	switch (s)
	{
	case StateSend::None: return "none";
	case StateSend::Change: return "change";
	case StateSend::Repeat: return "repeat";
	}
	return "?";
}

// One car on the decider. A car that was never live is silent (a parked car nobody used: its copies are as the stock spawned them).
// Once its state went out, every change goes (its copies show the last state they were told: lights left on, an engine that
// stopped, damage under fire), no sooner than kStateMinIntervalMs after the last; while the engine runs the state goes again every
// kStateRepeatMs; parked (engine off) and unchanged: nothing.
class StateCadence
{
public:
	StateSend step(const StateMsg& now, bool is_live, uint32_t now_ms) const
	{
		if (!m_known)
			return is_live ? StateSend::Change : StateSend::None;
		const uint32_t since = uint32_t(now_ms - m_sent_ms);
		if (!same_but_rpm(now, m_last))
			return since >= kStateMinIntervalMs ? StateSend::Change : StateSend::None;
		if ((now.flags & kStateEngine) != 0 && since >= kStateRepeatMs)
			return StateSend::Repeat;
		return StateSend::None;
	}

	void sent(const StateMsg& m, uint32_t now_ms)
	{
		m_known = true;
		m_last = m;
		m_sent_ms = now_ms;
	}

	bool known() const { return m_known; }
	const StateMsg& last() const { return m_last; }

private:
	bool m_known = false;
	StateMsg m_last;
	uint32_t m_sent_ms = 0;
};

// ---------------------------------------------------------------- a player's machine: the copy

// What the copy has now (read before a state is applied).
struct CopyFacts
{
	bool engine = false; // the copy's b_engine_on
	uint16_t damage = 0; // the copy's m_level_applied (what its own spawn played: CDamagableItem::RestoreEffect)
	bool wrecked = false; // exploded, dead or destroyed: nothing is switched on (CCar::CarExplode turns the lights off)
};

// The stock calls for one state, in this order: the fuel (SetfFuel: StartEngine needs it), the engine (StartEngine / StopEngine), the
// head lights, the damage stages, the transmission switch sound.
struct CopyPlan
{
	bool start = false; // CCar::StartEngine: exhaust, CCarSound::Start, b_engine_on
	bool stop = false; // CCar::StopEngine: CCarSound::Stop and the exhaust stop (AscCall), NeutralDrive
	int8_t lights = 0; // +1 TurnOnHeadLights, -1 TurnOffHeadLights (both idempotent per light), 0 nothing
	uint8_t damage_from = 0; // the stages damage_from+1 .. damage_to are played (1: Play1, 2: Play2, as CCar::ApplyDamage)
	uint8_t damage_to = 0;
	bool gear_sound = false; // AscCall(ascSndTransmission), as CCar::Transmission asks it on a change of gear
};

// One car on a player's machine: the plans of its states, the gear it was last told and the rpm its engine sound uses.
class CopyTrack
{
public:
	// The plan of the stock calls for state `m` on a copy that has `f`; `now_ms` this machine's time.
	CopyPlan apply(const StateMsg& m, const CopyFacts& f, uint32_t now_ms)
	{
		CopyPlan p;
		const bool engine = (m.flags & kStateEngine) != 0;
		p.start = engine && !f.engine && !f.wrecked;
		p.stop = !engine && f.engine;
		if (f.wrecked)
			p.lights = -1;
		else
			p.lights = (m.flags & kStateLights) != 0 ? int8_t(1) : int8_t(-1);
		p.damage_from = damage_stage(f.damage);
		p.damage_to = m.damage > p.damage_from ? m.damage : p.damage_from; // stages never go back in the stock (no repair effect)
		// the stock plays the switch sound when the gear number changes (Transmission), not at a neutral (NeutralDrive): the copy
		// remembers the last gear it was told and sounds when another one comes, while the engine runs
		if (m.gear != 0)
		{
			p.gear_sound = m_gear != 0 && m.gear != m_gear && engine && !f.wrecked;
			m_gear = m.gear;
		}
		// the engine sound: from what it plays now to the decider's rpm over one repeat (a start begins at 0, as StartEngine sets it)
		const float shown = p.start ? 0.f : rpm_fraction(now_ms);
		m_rpm_from = shown;
		m_rpm_to = engine ? fraction_of(m.rpm) : 0.f;
		m_rpm_at = now_ms;
		m_known = true;
		m_last = m;
		return p;
	}

	// The rpm of the copy's engine sound now, as a fraction of m_max_rpm: from the last shown value to the decider's over
	// kStateRepeatMs (the decider's own rpm moves smoothly, its states come twice a second).
	float rpm_fraction(uint32_t now_ms) const
	{
		if (!m_known)
			return 0.f;
		const uint32_t since = uint32_t(now_ms - m_rpm_at);
		if (since >= kStateRepeatMs)
			return m_rpm_to;
		const float k = float(since) / float(kStateRepeatMs);
		return m_rpm_from + (m_rpm_to - m_rpm_from) * k;
	}

	bool known() const { return m_known; }
	const StateMsg& last() const { return m_last; }
	int8_t gear() const { return m_gear; }

private:
	bool m_known = false;
	StateMsg m_last;
	int8_t m_gear = 0;
	float m_rpm_from = 0.f;
	float m_rpm_to = 0.f;
	uint32_t m_rpm_at = 0;
};

// The engine sound's pitch as CCar::SCarSound::UpdateDrive computes it before its clamp (CarSound.cpp: 0.5 + 0.5 * rpm / torque rpm).
inline float sound_scale(float rpm, float torque_rpm)
{
	if (!(torque_rpm > 0.f) || !std::isfinite(rpm))
		return 0.5f;
	return 0.5f + 0.5f * rpm / torque_rpm;
}

// ---------------------------------------------------------------- the doll of a sitter on another machine

enum class DollPose : uint8_t
{
	Idle, // the vehicle idle of the doll's own animations, as CActor::attach_Vehicle plays idles[0] for its Owner
	Hidden, // hidden while he sits
};

// Seat 0 of a model without car_definition driver_place: the stock hides its Owner (Car.cpp attach_Actor `Owner()->setVisible(0)`),
// so the doll hides too. Else the doll plays the idle when its animations have one (the stalker set has steering_idle_0_0 and
// steering_idle_1_0, stalker_animation.omf), else it hides (a doll standing in a car is worse than none).
inline DollPose doll_pose(uint8_t seat, bool driver_place, bool has_idle)
{
	if (seat == kDriverSeat && !driver_place)
		return DollPose::Hidden;
	return has_idle ? DollPose::Idle : DollPose::Hidden;
}

// The idle the actor plays when he sits: SVehicleAnimCollection::Create (ActorAnimation.cpp) "steering_idle_" + the car's
// DriverAnimationType + "_" + 0, idles[0] (Actor.cpp attach_Vehicle).
inline std::string driver_idle_name(uint16_t anim_type) { return "steering_idle_" + std::to_string(anim_type) + "_0"; }

// ---------------------------------------------------------------- the driver's machine: a tap is not lost (rv_F5-VEH-3)

// InputSender (coop_vehicle_input_rules.h) sends the HELD bits at most once per kInputMinIntervalMs: a held key pressed and released
// between two messages (a quick tap of a gear at a low frame rate, a single shot) never reached the decider. The latch keeps such a
// key held until the next message is taken, then releases it, so the decider sees the press in one message and the release in the
// next. A press of the same key while its release waits keeps it held (two taps inside one message are one press).
class TapLatch
{
public:
	void reset() { *this = TapLatch(); }

	// `key` was pressed; `added` are the held bits the press set in the sender (0: none, or it was held already).
	void pressed(int key, uint16_t added)
	{
		m_unsent = uint16_t(m_unsent | added);
		for (size_t i = 0; i < m_waiting.size(); ++i)
			if (m_waiting[i] == key)
			{
				m_waiting.erase(m_waiting.begin() + std::ptrdiff_t(i));
				break;
			}
	}

	// `key` (held bit `bit`) was released. True: its press went out already, the release goes as it is. False: its press has not
	// gone out yet: the caller holds it again until the next message is taken (taken() gives it back).
	bool release_now(int key, uint16_t bit)
	{
		if (bit == 0 || (m_unsent & bit) == 0)
			return true;
		for (int k : m_waiting)
			if (k == key)
				return false;
		if (m_waiting.size() >= kMaxWaiting)
			return true;
		m_waiting.push_back(key);
		return false;
	}

	// A message was taken: every held bit went out with it. The keys whose release waited, to release now.
	std::vector<int> taken()
	{
		m_unsent = 0;
		std::vector<int> out;
		out.swap(m_waiting);
		return out;
	}

	uint16_t unsent() const { return m_unsent; }
	size_t waiting() const { return m_waiting.size(); }

private:
	static constexpr size_t kMaxWaiting = 16; // the held keys of kKeyRules are 8
	uint16_t m_unsent = 0;
	std::vector<int> m_waiting;
};
} // namespace vehicles
} // namespace coop
