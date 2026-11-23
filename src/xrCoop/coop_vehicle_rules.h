// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players (F5, build 57, docs/VEHICLES.md): the pure half.
//
// A player drives a car, rides in it and gets in and out through the process that decides for his level: the co-op host, or
// in the Server mode the map server of his level (the world server only tunnels the messages, Scope::Map). These rules are
// the same in both modes and do not know which one runs:
//   - the layouts of the four messages, each read with every bound checked:
//       VehicleReq   player -> decider       enter | exit | switch_seat               Reliable
//       VehicleSeat  decider -> players      granted | released | refused             Reliable (refused: to the asker only)
//       VehicleInput driver -> decider       held keys, press counters, weapon aim   Unreliable (sequenced), seq
//       VehicleState decider -> players      engine, lights, hand brake, rpm, gear,  Reliable
//                                            health, fuel, damage stage
//   - SeatTable: who sits where (the decider's truth; every player's machine keeps a mirror from VehicleSeat);
//   - the verdicts enter / exit / switch_to on plain facts, the game rules first and the checks of the client's word
//     (coop_trust, shipped in "log") last;
//   - the input rules: sequence with wrap, press counters, stale input (every key released, the hand brake held), rate cap.
// What the decider calls on the car for a held key is F5-VEH-2's (coop_vehicle_drive_rules.h); the key -> bit mapping of a
// player's machine is F5-VEH-3's (coop_vehicle_input_rules.h); state quantization is F5-VEH-4's.
//
// Pure C++17, header-only, no engine dependencies: used by the engine (xrGame/coop/coop_vehicles_*.cpp) and by the unit-test
// exe (tests/test_vehicle_rules.cpp).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <vector>

#include "coop_wire.h"

namespace coop
{
namespace vehicles
{
// ---------------------------------------------------------------- ids and seats

// No car / no character: the u16 object ids of the protocol (host numbering) never use 0xffff (ALife's invalid id).
constexpr uint16_t kNoCar = 0xffff;
constexpr uint16_t kNoCharacter = 0xffff;
// Seat 0 is the driver's: the one place a stock car has (car_definition driver_place, Car.cpp attach_Actor).
constexpr uint8_t kDriverSeat = 0;
// Seats a car may have in the table. Stock cars have only the driver's place; the passenger places come from
// configs/coop_vehicles.ltx (F5-VEH-5). 8 is a driver and seven passengers: more than any car body of GAMMA seats (not
// counted per model here), and it keeps a broken ltx line from making a car of 255 seats.
constexpr uint8_t kMaxSeats = 8;
// Not a seat (a lookup that found none). Never on the wire.
constexpr uint8_t kNoSeat = 0xfe;
// VehicleReq enter: the driver's seat when it is free, else the lowest free one (F on a car asks for "a place").
constexpr uint8_t kAnySeat = 0xff;

// ---------------------------------------------------------------- times, rates, bounds

// Seat requests of one character (enter, exit, switch): 4 a second. Faster than anybody presses F; a client that sends
// more is spamming. A trust check (veh_request_rate): "log" lets it through and writes the line.
constexpr uint32_t kRequestMinIntervalMs = 250;
// From the requester's body to the car's position (its root, XFORM().c) at an enter. A long car (a BTR, ~7.5 m) puts its
// doors ~4 m from the root, a player reaches ~1.5 m beyond, and the body the decider sees lags up to ~1 m behind a running
// player (snapshots, <= 150 ms at ~7 m/s). A trust check (veh_enter_distance).
constexpr float kEnterMaxDistance = 7.f;
// Input of the seated driver older than this: every key is released and the hand brake held (InputTrack::tick), until his
// input comes again. Two repeats lost (kInputRepeatMs) and half of the third.
constexpr uint32_t kInputStaleMs = 500;
// The driver's machine repeats its input this often while he drives, held keys or not: so a silent driver means a lost
// link, not "nothing pressed" (a car with no keys held rolls, as in stock; a stale one brakes).
constexpr uint32_t kInputRepeatMs = 200;
// The driver's machine sends at most one input per this many ms (~30 a second). A change sooner waits for the next slot:
// the held bits are a state and the presses are counters, so nothing is lost.
constexpr uint32_t kInputMinIntervalMs = 33;
// The decider's cap on one driver's input: 40 a second with a burst of 12. Above the client's own ~30 for packets that
// arrive bunched; the burst covers ~400 ms of them after a hitch. Over it: trust check veh_input_rate.
constexpr uint32_t kInputRatePerSec = 40;
constexpr uint32_t kInputBurst = 12;
// A request with no answer for this long: the player's machine says so on the HUD and forgets the request (F5-VEH-3; in
// the Server mode the level's process may still be starting).
constexpr uint32_t kAnswerTimeoutMs = 3000;
// An exit position farther than this from the origin is garbage: the levels of the game lie within a few km of theirs.
constexpr float kMaxCoord = 100000.f;
// VehicleState gear: -1 reverse (the stock transmission 0), 0 neutral, 1.. forward. The gears come from the model's user
// data (not read here); 15 is above any real gearbox and still refuses garbage.
constexpr int8_t kMaxGear = 15;
// VehicleState damage stage: CDamagableItem levels of a car, Car.cpp Init: CDamagableItem::Init(GetfHealth(), 3).
constexpr uint8_t kMaxDamageStage = 3;

// coop_trust names (sections of mod_system_coop_trust.ltx; the default "log" covers them). Permanent: config keys.
constexpr const char* kTrustRequestRate = "veh_request_rate";
constexpr const char* kTrustEnterDistance = "veh_enter_distance";
constexpr const char* kTrustInputRate = "veh_input_rate";

// ---------------------------------------------------------------- reasons

// VehicleReq op. Values are on the wire.
enum class ReqOp : uint8_t
{
	Enter = 1, // a seat (kAnySeat or a number)
	Exit = 2, // leave the seat he holds (the seat byte is kAnySeat)
	SwitchSeat = 3, // move to another seat of the same car
};

// VehicleSeat op. Values are on the wire.
enum class SeatOp : uint8_t
{
	Granted = 1, // `character` sits in `seat` now (a switch of seat is one granted: his old seat is free)
	Released = 2, // `character` left `seat`; `exit` is where he stands now
	Refused = 3, // his request was refused for `reason` (to the asker only)
};

// Why a request was refused (VehicleSeat refused). Values are on the wire. The order of the checks: enter / exit / switch_to.
enum class Refusal : uint8_t
{
	None = 0, // granted
	NoCar = 1, // the id is not a car on this level (not online here, not a CCar)
	Unsupported = 2, // a car the coop cannot drive for a client in build 57 (remote control, drone: CarDrone)
	Destroyed = 3, // the car is dead or exploded (CPHDestroyable::Destroyed, b_exploded)
	Dead = 4, // the asker is dead
	Wounded = 5, // the asker is wounded (coop_survival)
	AlreadySeated = 6, // he holds a seat already (to move: switch_seat; to the seat he holds: nothing to do)
	Carrying = 7, // he carries a body (stock ActorUse drops it first: his machine sends the request after the drop)
	Locked = 8, // CHolderCustom::EnterLocked (a script locked the car, Lua SetEnterLocked)
	NoSuchSeat = 9, // the car has no such seat
	SeatTaken = 10, // somebody sits there
	Full = 11, // kAnySeat: every seat is taken
	NotSeated = 12, // exit / switch: he sits in no seat of this car
	ExitLocked = 13, // CHolderCustom::ExitLocked (stock detach_Vehicle without bForce refuses too)
	NoCharacter = 14, // the asker has no character on this level (joining, or on another level)
	TooSoon = 15, // trust (veh_request_rate): a request of his came less than kRequestMinIntervalMs ago
	TooFar = 16, // trust (veh_enter_distance): his body is farther than kEnterMaxDistance from the car
	Last = TooFar,
};

// Why a seat was left (VehicleSeat released). Values are on the wire. Every one moves the sitter to the exit position.
enum class Release : uint8_t
{
	Exit = 1, // he asked (VehicleReq exit) and the verdict let him out
	Disconnect = 2, // his player left the session
	Death = 3, // his character died
	Wounded = 4, // his character fell wounded
	LeftLevel = 5, // co-op: before the group moves to another level; Server: his virtual peer left the level's process
	CarDestroyed = 6, // the car exploded or was destroyed (Car.cpp CarExplode: the stock Owner is thrown out too)
	CarGone = 7, // the car's object is gone from the level (destroyed object, went offline)
	Script = 8, // a script of the decider took him out (coop.vehicle.exit, a mod's detach)
	Last = Script,
};

// Edge events: press counters of VehicleInput.edges (2 bits each, mod 4).
enum class Edge : uint8_t
{
	Engine = 0, // kDETECTOR: SwitchEngine()
	Lights = 1, // kTORCH: m_lights.SwitchHeadLights()
	Horn = 2, // no stock key: reserved, the decider calls nothing in build 57
};

inline const char* refusal_name(Refusal r)
{
	switch (r)
	{
	case Refusal::None: return "none";
	case Refusal::NoCar: return "no_car";
	case Refusal::Unsupported: return "unsupported";
	case Refusal::Destroyed: return "destroyed";
	case Refusal::Dead: return "dead";
	case Refusal::Wounded: return "wounded";
	case Refusal::AlreadySeated: return "already_seated";
	case Refusal::Carrying: return "carrying";
	case Refusal::Locked: return "locked";
	case Refusal::NoSuchSeat: return "no_such_seat";
	case Refusal::SeatTaken: return "seat_taken";
	case Refusal::Full: return "full";
	case Refusal::NotSeated: return "not_seated";
	case Refusal::ExitLocked: return "exit_locked";
	case Refusal::NoCharacter: return "no_character";
	case Refusal::TooSoon: return "too_soon";
	case Refusal::TooFar: return "too_far";
	}
	return "?";
}

inline const char* release_name(Release r)
{
	switch (r)
	{
	case Release::Exit: return "exit";
	case Release::Disconnect: return "disconnect";
	case Release::Death: return "death";
	case Release::Wounded: return "wounded";
	case Release::LeftLevel: return "left_level";
	case Release::CarDestroyed: return "car_destroyed";
	case Release::CarGone: return "car_gone";
	case Release::Script: return "script";
	}
	return "?";
}

inline const char* op_name(ReqOp op)
{
	switch (op)
	{
	case ReqOp::Enter: return "enter";
	case ReqOp::Exit: return "exit";
	case ReqOp::SwitchSeat: return "switch_seat";
	}
	return "?";
}

inline const char* op_name(SeatOp op)
{
	switch (op)
	{
	case SeatOp::Granted: return "granted";
	case SeatOp::Released: return "released";
	case SeatOp::Refused: return "refused";
	}
	return "?";
}

// The coop_trust name of a refusal that checks the client's word, else nullptr (a game rule: always refused).
inline const char* trust_name(Refusal r)
{
	switch (r)
	{
	case Refusal::TooSoon: return kTrustRequestRate;
	case Refusal::TooFar: return kTrustEnterDistance;
	default: return nullptr;
	}
}

// A check of the client's word: the caller asks coop_trust; in "log" the request is granted anyway and the line written.
inline bool is_trust(Refusal r) { return trust_name(r) != nullptr; }

namespace detail
{
// no fabs / free here: the engine marks them deprecated (C4995, xrCore/vector.h)
inline bool sane_coord(float v) { return std::isfinite(v) && v <= kMaxCoord && v >= -kMaxCoord; }
inline bool real_seat(uint8_t seat) { return seat < kMaxSeats; }
} // namespace detail

// ---------------------------------------------------------------- VehicleReq (player -> decider, Reliable)

// Wire: u8 op, u16 car, u8 seat. 4 bytes.
// enter: seat < kMaxSeats or kAnySeat; exit: kAnySeat (the decider knows where he sits); switch_seat: seat < kMaxSeats.
// The asker is the sender (his peer -> his character on the decider), never a field.
struct ReqMsg
{
	ReqOp op = ReqOp::Enter;
	uint16_t car = kNoCar;
	uint8_t seat = kAnySeat;
};

inline void write(Writer& w, const ReqMsg& m)
{
	w.u8(uint8_t(m.op));
	w.u16(m.car);
	w.u8(m.seat);
}

// False on an overrun, a byte too many or anything the layout above refuses; `out` is untouched then.
inline bool read(Reader& r, ReqMsg& out)
{
	const uint8_t op = r.u8();
	const uint16_t car = r.u16();
	const uint8_t seat = r.u8();
	if (!r.ok() || r.remaining() != 0 || car == kNoCar)
		return false;
	switch (ReqOp(op))
	{
	case ReqOp::Enter:
		if (!detail::real_seat(seat) && seat != kAnySeat)
			return false;
		break;
	case ReqOp::Exit:
		if (seat != kAnySeat)
			return false;
		break;
	case ReqOp::SwitchSeat:
		if (!detail::real_seat(seat))
			return false;
		break;
	default:
		return false;
	}
	out.op = ReqOp(op);
	out.car = car;
	out.seat = seat;
	return true;
}

// ---------------------------------------------------------------- VehicleSeat (decider -> players of the level, Reliable)

// Wire: u16 car, u8 seat, u16 character, u8 op, u8 reason, then 3 x f32 exit position when op is released. 7 or 19 bytes.
//   granted:  seat < kMaxSeats, a character, reason 0;
//   released: seat < kMaxSeats, a character, reason a Release, the exit position finite and within kMaxCoord;
//   refused:  seat < kMaxSeats or kAnySeat (what he asked), character his or kNoCharacter (he has none here), reason a
//             Refusal other than None.
// granted and released go to every player of the level (and to one who arrives, a granted per taken seat); refused only to
// the asker.
struct SeatMsg
{
	uint16_t car = kNoCar;
	uint8_t seat = kNoSeat;
	uint16_t character = kNoCharacter;
	SeatOp op = SeatOp::Refused;
	uint8_t reason = 0;
	float exit[3] = {0.f, 0.f, 0.f};

	Refusal refusal() const { return op == SeatOp::Refused ? Refusal(reason) : Refusal::None; }
	Release release() const { return op == SeatOp::Released ? Release(reason) : Release(0); }

	static SeatMsg granted(uint16_t car, uint8_t seat, uint16_t character)
	{
		SeatMsg m;
		m.car = car;
		m.seat = seat;
		m.character = character;
		m.op = SeatOp::Granted;
		return m;
	}
	static SeatMsg released(uint16_t car, uint8_t seat, uint16_t character, Release why, const float exit_pos[3])
	{
		SeatMsg m;
		m.car = car;
		m.seat = seat;
		m.character = character;
		m.op = SeatOp::Released;
		m.reason = uint8_t(why);
		for (int i = 0; i < 3; ++i)
			m.exit[i] = exit_pos[i];
		return m;
	}
	static SeatMsg refused(uint16_t car, uint8_t seat, uint16_t character, Refusal why)
	{
		SeatMsg m;
		m.car = car;
		m.seat = seat;
		m.character = character;
		m.op = SeatOp::Refused;
		m.reason = uint8_t(why);
		return m;
	}
};

inline void write(Writer& w, const SeatMsg& m)
{
	w.u16(m.car);
	w.u8(m.seat);
	w.u16(m.character);
	w.u8(uint8_t(m.op));
	w.u8(m.reason);
	if (m.op == SeatOp::Released)
		for (int i = 0; i < 3; ++i)
			w.f32(m.exit[i]);
}

inline bool read(Reader& r, SeatMsg& out)
{
	SeatMsg m;
	m.car = r.u16();
	m.seat = r.u8();
	m.character = r.u16();
	const uint8_t op = r.u8();
	m.reason = r.u8();
	if (!r.ok() || m.car == kNoCar)
		return false;
	switch (SeatOp(op))
	{
	case SeatOp::Granted:
		if (!detail::real_seat(m.seat) || m.character == kNoCharacter || m.reason != 0)
			return false;
		break;
	case SeatOp::Released:
		if (!detail::real_seat(m.seat) || m.character == kNoCharacter || m.reason == 0 ||
		    m.reason > uint8_t(Release::Last))
			return false;
		for (int i = 0; i < 3; ++i)
			m.exit[i] = r.f32();
		if (!r.ok())
			return false;
		for (int i = 0; i < 3; ++i)
			if (!detail::sane_coord(m.exit[i]))
				return false;
		break;
	case SeatOp::Refused:
		if ((!detail::real_seat(m.seat) && m.seat != kAnySeat) || m.reason == 0 || m.reason > uint8_t(Refusal::Last))
			return false;
		break;
	default:
		return false;
	}
	if (r.remaining() != 0)
		return false;
	m.op = SeatOp(op);
	out = m;
	return true;
}

// ---------------------------------------------------------------- VehicleInput (driver -> decider, Unreliable)

// Held bits: the keys of CarInput.cpp OnKeyboardPress / OnKeyboardRelease the driver holds now. The decider turns a change
// of a bit into the stock call of that key (F5-VEH-2, coop_vehicle_drive_rules.h).
constexpr uint16_t kHeldForward = 1u << 0; // kFWD: PressForward / ReleaseForward
constexpr uint16_t kHeldBack = 1u << 1; // kBACK: PressBack / ReleaseBack
constexpr uint16_t kHeldLeft = 1u << 2; // kL_STRAFE: PressLeft / ReleaseLeft
constexpr uint16_t kHeldRight = 1u << 3; // kR_STRAFE: PressRight / ReleaseRight
constexpr uint16_t kHeldBrake = 1u << 4; // kJUMP: PressBreaks (the hand brake) / ReleaseBreaks
constexpr uint16_t kHeldTransUp = 1u << 5; // kACCEL: TransmissionUp on press, nothing on release
constexpr uint16_t kHeldTransDown = 1u << 6; // kCROUCH: TransmissionDown on press, nothing on release
constexpr uint16_t kHeldFire = 1u << 7; // kWPN_FIRE: the car weapon's Action(eWpnFire, 1 | 0) when HasWeapon
// Not a key but the switch kWPN_FUNC toggles on the driver's machine (Action(eWpnActivate, !IsActive())): the decider sets
// its weapon to it. Without it the weapon never shoots (CCarWeapon::UpdateCL returns while not active).
constexpr uint16_t kHeldWeapon = 1u << 8;
// Layout flag, not a key: two i16 angles of the aim follow (the car has a weapon).
constexpr uint16_t kAimFollows = 1u << 15;
constexpr uint16_t kHeldKeys = 0x00ff;
constexpr uint16_t kHeldState = kHeldKeys | kHeldWeapon; // what InputTrack keeps
constexpr uint16_t kHeldAll = kHeldState | kAimFollows; // every bit a message may have
// Edges: three 2-bit press counters (Edge). Counters, not bits: an Unreliable message may be lost or dropped by the rate
// cap; the next one still carries the count, and an old one never replays a press.
constexpr uint8_t kEdgesUsed = 0x3f;
// The aim pitch on the i16 scale of angle_to_wire (pi = 32768): straight up / down.
constexpr int16_t kMaxPitchWire = 16384;

// Wire: u16 car, u8 seq, u16 held, u8 edges, then i16 yaw, i16 pitch when held has kAimFollows. 6 or 10 bytes.
// seq: 1 for the first message after his granted, +1 for each next one (wraps; seq_newer). held: no bits outside kHeldAll.
// edges: no bits outside kEdgesUsed; the counters start at 0 at his granted. yaw any; |pitch| <= kMaxPitchWire.
// The aim is the direction of the weapon's desired line (heading, pitch): the decider calls
// SetParam(CCarWeapon::eWpnDesiredDir, Fvector2(yaw, pitch)) = m_destEnemyDir.setHP(yaw, pitch).
struct InputMsg
{
	uint16_t car = kNoCar;
	uint8_t seq = 0;
	uint16_t held = 0;
	uint8_t edges = 0;
	int16_t aim_yaw = 0;
	int16_t aim_pitch = 0;

	bool has_aim() const { return (held & kAimFollows) != 0; }
	inline void set_aim(float yaw, float pitch);
};

// An angle in radians on the i16 scale: wrapped into [-pi, pi), pi = 32768. A step is ~0.0055 degrees. NaN / inf -> 0.
inline int16_t angle_to_wire(float radians)
{
	if (!std::isfinite(radians))
		return 0;
	const double pi = 3.14159265358979323846;
	double a = std::remainder(double(radians), 2.0 * pi); // [-pi, pi]
	long v = std::lround(a * 32768.0 / pi);
	if (v >= 32768)
		v -= 65536; // +pi is -pi
	if (v < -32768)
		v = -32768;
	return int16_t(v);
}

inline float angle_from_wire(int16_t v)
{
	const double pi = 3.14159265358979323846;
	return float(double(v) * pi / 32768.0);
}

// A pitch clamped to [-pi/2, pi/2] on the same scale (+-kMaxPitchWire). NaN / inf -> 0.
inline int16_t pitch_to_wire(float radians)
{
	if (!std::isfinite(radians))
		return 0;
	const double pi = 3.14159265358979323846;
	double p = double(radians);
	if (p > pi / 2.0)
		p = pi / 2.0;
	if (p < -pi / 2.0)
		p = -pi / 2.0;
	return int16_t(std::lround(p * 32768.0 / pi));
}

inline void InputMsg::set_aim(float yaw, float pitch)
{
	held = uint16_t(held | kAimFollows);
	aim_yaw = angle_to_wire(yaw);
	aim_pitch = pitch_to_wire(pitch);
}

inline uint8_t edge_count(uint8_t edges, Edge e) { return uint8_t((edges >> (2 * uint8_t(e))) & 3u); }

// One more press of `e` (the driver's machine, at the press of its key).
inline uint8_t bump_edge(uint8_t edges, Edge e)
{
	const uint8_t shift = uint8_t(2 * uint8_t(e));
	const uint8_t count = uint8_t((edge_count(edges, e) + 1u) & 3u);
	return uint8_t((edges & ~(3u << shift)) | (count << shift));
}

// Presses of `e` between two messages (at most 3: four lost presses in a row wrap to none).
inline uint8_t edge_delta(uint8_t now, uint8_t before, Edge e) { return uint8_t((edge_count(now, e) - edge_count(before, e)) & 3u); }

inline void write(Writer& w, const InputMsg& m)
{
	w.u16(m.car);
	w.u8(m.seq);
	w.u16(m.held);
	w.u8(m.edges);
	if (m.has_aim())
	{
		w.u16(uint16_t(m.aim_yaw));
		w.u16(uint16_t(m.aim_pitch));
	}
}

inline bool read(Reader& r, InputMsg& out)
{
	InputMsg m;
	m.car = r.u16();
	m.seq = r.u8();
	m.held = r.u16();
	m.edges = r.u8();
	if (!r.ok() || m.car == kNoCar || (m.held & ~kHeldAll) != 0 || (m.edges & ~kEdgesUsed) != 0)
		return false;
	if (m.has_aim())
	{
		m.aim_yaw = int16_t(r.u16());
		m.aim_pitch = int16_t(r.u16());
		if (!r.ok() || m.aim_pitch > kMaxPitchWire || m.aim_pitch < -kMaxPitchWire)
			return false;
	}
	if (r.remaining() != 0)
		return false;
	out = m;
	return true;
}

// ---------------------------------------------------------------- VehicleState (decider -> players of the level, Reliable)

constexpr uint8_t kStateEngine = 1; // b_engine_on (isActiveEngine)
constexpr uint8_t kStateLights = 2; // the head lights shine (m_lights)
constexpr uint8_t kStateHandbrake = 4; // the hand brake holds the wheels (brp, or parked by detach_Actor's HandBreak)
constexpr uint8_t kStateFlagsAll = kStateEngine | kStateLights | kStateHandbrake;

// Wire: u16 car, u8 flags, u8 rpm, i8 gear, u8 health, u8 fuel, u8 damage. 8 bytes.
// rpm: m_current_rpm of m_max_rpm, 0..255; gear: -1 reverse, 0 neutral, 1..kMaxGear; health: of 1.0, 0..255; fuel: of the
// tank, 0..255; damage: 0..kMaxDamageStage. The quantization and the cadence are F5-VEH-4's (coop_vehicle_state_rules.h).
struct StateMsg
{
	uint16_t car = kNoCar;
	uint8_t flags = 0;
	uint8_t rpm = 0;
	int8_t gear = 0;
	uint8_t health = 0;
	uint8_t fuel = 0;
	uint8_t damage = 0;
};

inline void write(Writer& w, const StateMsg& m)
{
	w.u16(m.car);
	w.u8(m.flags);
	w.u8(m.rpm);
	w.u8(uint8_t(m.gear));
	w.u8(m.health);
	w.u8(m.fuel);
	w.u8(m.damage);
}

inline bool read(Reader& r, StateMsg& out)
{
	StateMsg m;
	m.car = r.u16();
	m.flags = r.u8();
	m.rpm = r.u8();
	m.gear = int8_t(r.u8());
	m.health = r.u8();
	m.fuel = r.u8();
	m.damage = r.u8();
	if (!r.ok() || r.remaining() != 0 || m.car == kNoCar || (m.flags & ~kStateFlagsAll) != 0 || m.gear < -1 ||
	    m.gear > kMaxGear || m.damage > kMaxDamageStage)
		return false;
	out = m;
	return true;
}

// ---------------------------------------------------------------- the seat table

struct Seated
{
	uint16_t car = kNoCar;
	uint8_t seat = kNoSeat;
	uint16_t character = kNoCharacter;
};

struct TakeResult
{
	Refusal reason = Refusal::None;
	uint8_t seat = kNoSeat; // the seat taken; kNoSeat when refused
};

// Who sits where. The decider's table is the truth: take / switch_seat / free_seat / release_* after its verdicts (the co-op
// host's own actor is in it too, at his stock attach). A player's machine keeps a mirror fed by apply(VehicleSeat).
// A character holds at most one seat. A car leaves the table when its last sitter goes, so the table never holds more rows
// than seated characters; its seat count is the one given at its first seating while anybody sits in it.
class SeatTable
{
public:
	// Seat `seat` (kAnySeat: the driver's when free, else the lowest free one) of `car`, which has `seats` seats, for
	// `character`. Refused: NoCar, NoCharacter, NoSuchSeat, AlreadySeated, SeatTaken, Full.
	TakeResult take(uint16_t car, uint8_t seats, uint8_t seat, uint16_t character)
	{
		if (car == kNoCar)
			return {Refusal::NoCar, kNoSeat};
		if (character == kNoCharacter)
			return {Refusal::NoCharacter, kNoSeat};
		const uint8_t count = seats_of(car, seats);
		if (count == 0 || count > kMaxSeats || (seat != kAnySeat && seat >= count))
			return {Refusal::NoSuchSeat, kNoSeat};
		if (seat_of(character).car != kNoCar)
			return {Refusal::AlreadySeated, kNoSeat};
		const uint8_t s = pick(car, count, seat);
		if (s == kNoSeat)
			return {seat == kAnySeat ? Refusal::Full : Refusal::SeatTaken, kNoSeat};
		row(car, count).at[s] = character;
		return {Refusal::None, s};
	}

	// Moves a seated character to `seat` of the car he sits in. Refused: NotSeated, NoSuchSeat, AlreadySeated (that is his
	// seat), SeatTaken.
	TakeResult switch_seat(uint16_t character, uint8_t seat)
	{
		const Seated where = seat_of(character);
		if (where.car == kNoCar)
			return {Refusal::NotSeated, kNoSeat};
		Row& c = m_cars[where.car];
		if (seat >= c.seats)
			return {Refusal::NoSuchSeat, kNoSeat};
		if (seat == where.seat)
			return {Refusal::AlreadySeated, kNoSeat};
		if (c.at[seat] != kNoCharacter)
			return {Refusal::SeatTaken, kNoSeat};
		c.at[where.seat] = kNoCharacter;
		c.at[seat] = character;
		return {Refusal::None, seat};
	}

	// Frees `seat` of `car`; the character who sat there, kNoCharacter when it was free. (Not "free": the engine marks that
	// name deprecated, C4995.)
	uint16_t free_seat(uint16_t car, uint8_t seat)
	{
		auto it = m_cars.find(car);
		if (it == m_cars.end() || seat >= it->second.seats)
			return kNoCharacter;
		const uint16_t was = it->second.at[seat];
		it->second.at[seat] = kNoCharacter;
		drop_if_empty(it);
		return was;
	}

	// Every seat `character` holds (one at most): a death, a disconnect, a wound, leaving the level. The caller sends a
	// released for each.
	std::vector<Seated> release_all(uint16_t character)
	{
		std::vector<Seated> out;
		if (character == kNoCharacter)
			return out;
		for (auto it = m_cars.begin(); it != m_cars.end();)
		{
			auto next = std::next(it);
			for (uint8_t s = 0; s < it->second.seats; ++s)
				if (it->second.at[s] == character)
				{
					out.push_back({it->first, s, character});
					it->second.at[s] = kNoCharacter;
				}
			drop_if_empty(it);
			it = next;
		}
		return out;
	}

	// Every sitter of `car`: it exploded or is gone.
	std::vector<Seated> release_car(uint16_t car)
	{
		std::vector<Seated> out = sitters(car);
		m_cars.erase(car);
		return out;
	}

	// A player's machine: the decider's VehicleSeat. Never refuses: a granted moves its character from any seat he held
	// (a switch of seat is one granted) and takes the seat whoever the mirror thought sat there; a released frees the seat
	// only if that character sits there; a refused changes nothing.
	void apply(const SeatMsg& m)
	{
		if (m.car == kNoCar || m.character == kNoCharacter || !detail::real_seat(m.seat))
			return;
		if (m.op == SeatOp::Granted)
		{
			release_all(m.character);
			Row& c = row(m.car, kMaxSeats);
			if (m.seat >= c.seats)
				c.seats = uint8_t(m.seat + 1);
			c.at[m.seat] = m.character;
		}
		else if (m.op == SeatOp::Released)
		{
			if (at(m.car, m.seat) == m.character)
				free_seat(m.car, m.seat);
		}
	}

	// Where `character` sits; .car == kNoCar when nowhere.
	Seated seat_of(uint16_t character) const
	{
		if (character == kNoCharacter)
			return {};
		for (const auto& kv : m_cars)
			for (uint8_t s = 0; s < kv.second.seats; ++s)
				if (kv.second.at[s] == character)
					return {kv.first, s, character};
		return {};
	}

	// The sitters of `car` by seat.
	std::vector<Seated> sitters(uint16_t car) const
	{
		std::vector<Seated> out;
		auto it = m_cars.find(car);
		if (it == m_cars.end())
			return out;
		for (uint8_t s = 0; s < it->second.seats; ++s)
			if (it->second.at[s] != kNoCharacter)
				out.push_back({car, s, it->second.at[s]});
		return out;
	}

	uint16_t at(uint16_t car, uint8_t seat) const
	{
		auto it = m_cars.find(car);
		if (it == m_cars.end() || seat >= it->second.seats)
			return kNoCharacter;
		return it->second.at[seat];
	}

	uint16_t driver_of(uint16_t car) const { return at(car, kDriverSeat); }

	// The seat a request gets: `wanted` when it exists and is free, for kAnySeat the driver's when free, else the lowest free
	// one; kNoSeat when none.
	uint8_t pick(uint16_t car, uint8_t seats, uint8_t wanted) const
	{
		const uint8_t count = seats_of(car, seats);
		if (count == 0 || count > kMaxSeats)
			return kNoSeat;
		if (wanted != kAnySeat)
			return wanted < count && at(car, wanted) == kNoCharacter ? wanted : kNoSeat;
		for (uint8_t s = 0; s < count; ++s)
			if (at(car, s) == kNoCharacter)
				return s;
		return kNoSeat;
	}

	size_t seated() const
	{
		size_t n = 0;
		for (const auto& kv : m_cars)
			for (uint8_t s = 0; s < kv.second.seats; ++s)
				n += kv.second.at[s] != kNoCharacter ? 1 : 0;
		return n;
	}

	size_t cars() const { return m_cars.size(); }
	void clear() { m_cars.clear(); }

private:
	struct Row
	{
		uint8_t seats = 0;
		uint16_t at[kMaxSeats] = {kNoCharacter, kNoCharacter, kNoCharacter, kNoCharacter,
		                          kNoCharacter, kNoCharacter, kNoCharacter, kNoCharacter};
	};
	static_assert(kMaxSeats == 8, "Row::at lists kMaxSeats initializers");

	// The count of a car that has a row (its first seating's) or the one given.
	uint8_t seats_of(uint16_t car, uint8_t given) const
	{
		auto it = m_cars.find(car);
		return it == m_cars.end() ? given : it->second.seats;
	}

	Row& row(uint16_t car, uint8_t seats)
	{
		Row& c = m_cars[car];
		if (c.seats == 0)
			c.seats = seats > kMaxSeats ? kMaxSeats : seats;
		return c;
	}

	void drop_if_empty(std::map<uint16_t, Row>::iterator it)
	{
		for (uint8_t s = 0; s < it->second.seats; ++s)
			if (it->second.at[s] != kNoCharacter)
				return;
		m_cars.erase(it);
	}

	std::map<uint16_t, Row> m_cars;
};

// ---------------------------------------------------------------- verdicts (the decider)

// The order of every verdict: the game rules first, the checks of the client's word (is_trust) last. A trust refusal ships
// in "log" and is then granted, so it may only be reached when no rule refuses.

// Facts of an enter request, read by the decider from its own objects (F5-VEH-2): the car, the asker's body, the table.
struct EnterFacts
{
	bool car_known = false; // the id is a CCar online on this level
	bool supported = false; // not a remote-control or drone car (CAR_NEW m_remote_control, CarDrone)
	bool car_destroyed = false; // dead, exploded or CPHDestroyable::Destroyed
	bool enter_locked = false; // CHolderCustom::EnterLocked
	bool alive = false; // the asker's character
	bool wounded = false; // the asker's character is wounded (coop_survival)
	bool carrying = false; // the asker carries a body
	bool seated = false; // the asker holds a seat already
	uint8_t seats = 0; // seats of the car: 1 = driver only (stock), more from coop_vehicles.ltx
	uint8_t seat = kAnySeat; // asked
	bool seat_free = false; // SeatTable::pick found the asked seat (or, kAnySeat, some seat) free
	float distance = 0.f; // asker's body -> the car's position, m
	uint32_t since_last_ms = 0; // since the asker's previous seat request (a large number when none)
};

inline Refusal enter(const EnterFacts& f)
{
	if (!f.car_known)
		return Refusal::NoCar;
	if (!f.supported)
		return Refusal::Unsupported;
	if (f.car_destroyed)
		return Refusal::Destroyed;
	if (!f.alive)
		return Refusal::Dead;
	if (f.wounded)
		return Refusal::Wounded;
	if (f.seated)
		return Refusal::AlreadySeated;
	if (f.carrying)
		return Refusal::Carrying;
	if (f.enter_locked)
		return Refusal::Locked;
	if (f.seats == 0 || f.seats > kMaxSeats || (f.seat != kAnySeat && f.seat >= f.seats))
		return Refusal::NoSuchSeat;
	if (!f.seat_free)
		return f.seat == kAnySeat ? Refusal::Full : Refusal::SeatTaken;
	// trust, last
	if (f.since_last_ms < kRequestMinIntervalMs)
		return Refusal::TooSoon;
	if (!(f.distance <= kEnterMaxDistance)) // NaN too
		return Refusal::TooFar;
	return Refusal::None;
}

// Facts of an exit request. Releases the decider makes itself (death, disconnect, ...) do not ask: they ignore the lock as
// the stock detach_Vehicle(bForce) does.
struct ExitFacts
{
	bool seated = false; // the asker sits in this car
	bool exit_locked = false; // CHolderCustom::ExitLocked
	uint32_t since_last_ms = 0;
};

inline Refusal exit(const ExitFacts& f)
{
	if (!f.seated)
		return Refusal::NotSeated;
	if (f.exit_locked)
		return Refusal::ExitLocked;
	if (f.since_last_ms < kRequestMinIntervalMs)
		return Refusal::TooSoon;
	return Refusal::None;
}

// Facts of a switch_seat request.
struct SwitchFacts
{
	bool seated = false; // the asker sits in this car
	bool alive = false;
	bool wounded = false;
	uint8_t seats = 0; // seats of the car
	uint8_t from = kNoSeat; // the seat he holds
	uint8_t seat = kNoSeat; // asked
	bool seat_free = false;
	uint32_t since_last_ms = 0;
};

inline Refusal switch_to(const SwitchFacts& f)
{
	if (!f.seated)
		return Refusal::NotSeated;
	if (!f.alive)
		return Refusal::Dead;
	if (f.wounded)
		return Refusal::Wounded;
	if (f.seats == 0 || f.seats > kMaxSeats || f.seat >= f.seats)
		return Refusal::NoSuchSeat;
	if (f.seat == f.from)
		return Refusal::AlreadySeated;
	if (!f.seat_free)
		return Refusal::SeatTaken;
	if (f.since_last_ms < kRequestMinIntervalMs)
		return Refusal::TooSoon;
	return Refusal::None;
}

// ---------------------------------------------------------------- input rules (the decider, per driving seat)

// Is `seq` after `last` on the u8 ring? Up to 127 ahead is newer; 0 (a duplicate) and 128.. (older, or too far to tell) are
// not.
inline bool seq_newer(uint8_t seq, uint8_t last)
{
	const uint8_t d = uint8_t(seq - last);
	return d != 0 && d < 128;
}

enum class InputDrop : uint8_t
{
	None, // applied
	NotDriving, // no driver seated (start() was not called, or after stop())
	WrongCar, // input for a car he does not drive
	Old, // a duplicate or an older seq
	Quiet, // tick(): nothing to do
};

// What one input (or the stale tick) changes. The caller turns it into the stock calls in plan_drive's order
// (coop_vehicle_drive_rules.h, F5-VEH-2): the releases of `before` -> `held`, then the engine and lights toggles, then the
// presses, then the aim.
struct InputStep
{
	InputDrop drop = InputDrop::Quiet;
	uint16_t before = 0; // the held keys and weapon switch until now
	uint16_t held = 0; // from now (kHeldState bits only)
	uint8_t engine_toggles = 0; // SwitchEngine() this many times
	uint8_t lights_toggles = 0; // m_lights.SwitchHeadLights() this many times
	uint8_t horns = 0; // reserved: no stock horn
	bool aim = false; // the aim below is new
	int16_t aim_yaw = 0;
	int16_t aim_pitch = 0;
	bool resync = false; // taken after a stale gap whatever its seq
	bool stale = false; // tick(): the input went stale now

	bool applied() const { return drop == InputDrop::None; }
};

// The input state of the driver of one car on the decider: start() at his granted of seat 0, stop() at his release, accept()
// for each VehicleInput that passed the RateGate (or coop_trust), tick() every frame.
class InputTrack
{
public:
	void start(uint16_t car, uint32_t now_ms)
	{
		*this = InputTrack();
		m_car = car;
		m_driving = true;
		m_last_ms = now_ms;
	}

	void stop() { *this = InputTrack(); }

	InputStep accept(const InputMsg& m, uint32_t now_ms)
	{
		InputStep s;
		if (!m_driving)
		{
			s.drop = InputDrop::NotDriving;
			return s;
		}
		if (m.car != m_car)
		{
			s.drop = InputDrop::WrongCar;
			return s;
		}
		// After a stale gap the driver's seq ran on while it was lost: the first message back is taken as it is.
		if (!m_stale && !seq_newer(m.seq, m_seq))
		{
			s.drop = InputDrop::Old;
			return s;
		}
		s.drop = InputDrop::None;
		s.resync = m_stale;
		s.before = m_held;
		s.held = uint16_t(m.held & kHeldState);
		s.engine_toggles = edge_delta(m.edges, m_edges, Edge::Engine);
		s.lights_toggles = edge_delta(m.edges, m_edges, Edge::Lights);
		s.horns = edge_delta(m.edges, m_edges, Edge::Horn);
		s.aim = m.has_aim();
		s.aim_yaw = m.aim_yaw;
		s.aim_pitch = m.aim_pitch;
		m_seq = m.seq;
		m_edges = uint8_t(m.edges & kEdgesUsed);
		m_held = s.held;
		m_last_ms = now_ms;
		m_stale = false;
		return s;
	}

	// Every frame. No input for kInputStaleMs: every key released, the hand brake held, the weapon switch kept (kWPN_FUNC is
	// a switch, its release does nothing in stock). Once per gap.
	InputStep tick(uint32_t now_ms)
	{
		InputStep s;
		if (!m_driving || m_stale || uint32_t(now_ms - m_last_ms) < kInputStaleMs)
			return s;
		m_stale = true;
		s.drop = InputDrop::None;
		s.stale = true;
		s.before = m_held;
		s.held = uint16_t(kHeldBrake | (m_held & kHeldWeapon));
		m_held = s.held;
		return s;
	}

	uint16_t car() const { return m_car; }
	uint16_t held() const { return m_held; }
	bool driving() const { return m_driving; }
	bool is_stale() const { return m_stale; }

private:
	uint16_t m_car = kNoCar;
	bool m_driving = false;
	bool m_stale = false;
	uint8_t m_seq = 0;
	uint8_t m_edges = 0;
	uint16_t m_held = 0;
	uint32_t m_last_ms = 0;
};

// The decider's cap on one driver's VehicleInput: a token bucket of `burst` messages refilled at `per_sec`. A refused
// message is a trust check (kTrustInputRate): in "log" the caller applies it anyway and writes the line.
class RateGate
{
public:
	explicit RateGate(uint32_t per_sec = kInputRatePerSec, uint32_t burst = kInputBurst) : m_per_sec(per_sec), m_burst(burst) {}

	bool admit(uint32_t now_ms)
	{
		const uint64_t cap = uint64_t(m_burst) * kUnit;
		if (!m_started)
		{
			m_started = true;
			m_tokens = cap;
		}
		else
		{
			// per_sec messages in 1000 ms = per_sec units of 1/1000 message a ms
			const uint64_t refill = uint64_t(uint32_t(now_ms - m_last_ms)) * m_per_sec;
			m_tokens = m_tokens + refill > cap ? cap : m_tokens + refill;
		}
		m_last_ms = now_ms;
		if (m_tokens < kUnit)
			return false;
		m_tokens -= kUnit;
		return true;
	}

	void reset() { m_started = false; }

private:
	static constexpr uint64_t kUnit = 1000; // one message in thousandths
	uint32_t m_per_sec;
	uint32_t m_burst;
	bool m_started = false;
	uint64_t m_tokens = 0;
	uint32_t m_last_ms = 0;
};
} // namespace vehicles
} // namespace coop
