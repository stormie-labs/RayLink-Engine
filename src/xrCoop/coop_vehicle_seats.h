// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players, passengers (F5-VEH-5, build 58, docs/VEHICLES.md sections 4, 5 and 12): the pure half.
//
// A stock car has one place, the driver's (car_definition driver_place of the model's user data, else the root: CCar::attach_Actor).
// The passenger places come from configs/coop_vehicles.ltx, a section per car section:
//
//   [veh_niva]
//   seats  = 4                      ; places with the driver's (seat 0), 1..8 (kMaxSeats). No section, no line: the driver alone
//   seat_1 = driver, 0.8, 0, 0      ; <bone>[, <dx, dy, dz>][, <yaw>]  or  <x, y, z>, <yaw>
//   exit_1 = right                  ; left | right | back; no line: the side of the seat (x < 0 left, else right)
//   eye    = 0, 0.75, 0.1           ; optional: the eye in a seat's own space; no line: the driver's (camera_pos in his seat's space)
//
// seat_<i>:
//   <bone>                 the seat is that bone of the model, as driver_place is the driver's; `driver` is the driver's place
//                          itself (car_definition driver_place, else the root, as CCar::attach_Actor picks it);
//   <bone>, <yaw>          the same, turned by <yaw> degrees about the seat's up axis (positive: to the right);
//   <bone>, <dx, dy, dz>[, <yaw>]  moved by (dx, dy, dz) in the car's space (x right, y up, z forward), then turned;
//   <x, y, z>, <yaw>       a point in the car's space, the car's axes turned by <yaw>.
// The seat's world transform is the car's XFORM times this local one (as CCar::VisualUpdate puts the Owner: XFORM * the bone).
//
// Here: the parser of a section (every line checked: a missing or malformed seat ends the list there, a malformed exit or eye is
// written and left out), the composition of the transforms, the door point of a seat, the passenger's eye and the free look of
// a passenger's camera within the limits of the stock car camera (car_firsteye_cam). The engine half: xrGame/coop/
// coop_vehicles_ride.cpp (the ride on a player's machine, the ltx) and coop_vehicles_seat.cpp (seat_transform, seat counts).
//
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe (tests/test_vehicle_seats.cpp).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "coop_vehicle_rules.h"

namespace coop
{
namespace vehicles
{
// ---------------------------------------------------------------- vectors and the X-Ray matrix

struct SeatVec
{
	float x = 0.f;
	float y = 0.f;
	float z = 0.f;
};

inline SeatVec seat_vec(float x, float y, float z)
{
	SeatVec v;
	v.x = x;
	v.y = y;
	v.z = z;
	return v;
}

inline SeatVec operator+(const SeatVec& a, const SeatVec& b) { return seat_vec(a.x + b.x, a.y + b.y, a.z + b.z); }
inline SeatVec operator-(const SeatVec& a, const SeatVec& b) { return seat_vec(a.x - b.x, a.y - b.y, a.z - b.z); }
inline SeatVec operator*(const SeatVec& a, float s) { return seat_vec(a.x * s, a.y * s, a.z * s); }
inline float dot(const SeatVec& a, const SeatVec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// The rows of an X-Ray Fmatrix (i right, j up, k forward, c the position): transform_tiny(v) = v.x * i + v.y * j + v.z * k + c.
struct SeatXform
{
	SeatVec i = seat_vec(1.f, 0.f, 0.f);
	SeatVec j = seat_vec(0.f, 1.f, 0.f);
	SeatVec k = seat_vec(0.f, 0.f, 1.f);
	SeatVec c;
};

inline SeatVec apply_dir(const SeatXform& m, const SeatVec& v) { return m.i * v.x + m.j * v.y + m.k * v.z; }
inline SeatVec apply(const SeatXform& m, const SeatVec& v) { return apply_dir(m, v) + m.c; }

// `a` after `b`: Fmatrix::mul_43(a, b) (the Owner's XFORM is mul_43(car XFORM, seat): the seat first, then the car).
inline SeatXform compose(const SeatXform& a, const SeatXform& b)
{
	SeatXform r;
	r.i = apply_dir(a, b.i);
	r.j = apply_dir(a, b.j);
	r.k = apply_dir(a, b.k);
	r.c = apply(a, b.c);
	return r;
}

// A point of the world in the space of `m` (its axes orthogonal, as bone and object transforms are; a scale is divided out).
inline SeatVec unapply(const SeatXform& m, const SeatVec& p)
{
	const SeatVec d = p - m.c;
	auto along = [&d](const SeatVec& axis)
	{
		const float n = dot(axis, axis);
		return n > 1e-12f ? dot(d, axis) / n : 0.f;
	};
	return seat_vec(along(m.i), along(m.j), along(m.k));
}

constexpr float kSeatPi = 3.14159265358979323846f;

inline float seat_rad(float degrees) { return degrees * (kSeatPi / 180.f); }

// [-pi, pi)
inline float seat_wrap(float a)
{
	if (!std::isfinite(a))
		return 0.f;
	a = std::fmod(a + kSeatPi, 2.f * kSeatPi);
	if (a < 0.f)
		a += 2.f * kSeatPi;
	return a - kSeatPi;
}

// Turned about the up axis by `yaw` radians, positive to the right: a seat facing +z turned by pi/2 faces +x.
inline SeatXform turn_y(float yaw)
{
	const float s = std::sin(yaw), co = std::cos(yaw);
	SeatXform r;
	r.i = seat_vec(co, 0.f, -s);
	r.k = seat_vec(s, 0.f, co);
	return r;
}

// ---------------------------------------------------------------- one section of configs/coop_vehicles.ltx

enum class ExitSide : uint8_t
{
	Left,
	Right,
	Back,
};

inline const char* exit_side_name(ExitSide s)
{
	switch (s)
	{
	case ExitSide::Left: return "left";
	case ExitSide::Right: return "right";
	case ExitSide::Back: return "back";
	}
	return "?";
}

// The name of seat_<i> that means the driver's place (car_definition driver_place, else the root).
constexpr const char* kDriverPlaceBone = "driver";
// An offset or a point farther than this from the car's origin is a typo: the longest car of the game is ~12 m.
constexpr float kSeatMaxOffset = 20.f;
// An explicit eye farther than this from its seat is a typo.
constexpr float kSeatMaxEye = 2.f;

struct SeatPlace
{
	std::string bone; // empty: a point in the car's space (`offset` is the point)
	SeatVec offset; // the bone form: added in the car's space; the point form: the point
	float yaw_deg = 0.f;
	ExitSide exit = ExitSide::Right;
	bool exit_given = false; // exit_<i> said it; else the side of the seat
};

struct CarSeats
{
	uint8_t count = 1; // places with the driver's
	SeatPlace seat[kMaxSeats]; // [0] is the driver's (the stock place): never read
	bool eye_given = false;
	SeatVec eye;
	std::vector<std::string> errors; // "<key>: <what>", for the log
};

namespace detail
{
inline std::string trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
		++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
		--b;
	return s.substr(a, b - a);
}

inline std::vector<std::string> split_commas(const std::string& s)
{
	std::vector<std::string> out;
	size_t at = 0;
	for (;;)
	{
		const size_t comma = s.find(',', at);
		out.push_back(trim(s.substr(at, comma == std::string::npos ? std::string::npos : comma - at)));
		if (comma == std::string::npos)
			return out;
		at = comma + 1;
	}
}

// The whole token is one finite number.
inline bool number(const std::string& t, float& out)
{
	if (t.empty())
		return false;
	char* end = nullptr;
	const float v = std::strtof(t.c_str(), &end);
	if (end != t.c_str() + t.size() || !std::isfinite(v))
		return false;
	out = v;
	return true;
}

inline bool bone_name(const std::string& t)
{
	if (t.empty() || t.size() > 64)
		return false;
	for (char ch : t)
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'))
			return false;
	return !(t[0] >= '0' && t[0] <= '9');
}

inline bool near_car(const SeatVec& v)
{
	return std::fabs(v.x) <= kSeatMaxOffset && std::fabs(v.y) <= kSeatMaxOffset && std::fabs(v.z) <= kSeatMaxOffset;
}
} // namespace detail

// seat_<i>: true and `out` (bone, offset, yaw) when the value is one of the four forms within the bounds.
inline bool parse_seat_line(const std::string& value, SeatPlace& out)
{
	const std::vector<std::string> t = detail::split_commas(value);
	SeatPlace p;
	float n[4] = {0.f, 0.f, 0.f, 0.f};
	float first = 0.f;
	if (detail::number(t[0], first))
	{
		// <x, y, z>, <yaw>
		if (t.size() != 4)
			return false;
		for (size_t i = 0; i < 4; ++i)
			if (!detail::number(t[i], n[i]))
				return false;
		p.offset = seat_vec(n[0], n[1], n[2]);
		p.yaw_deg = n[3];
	}
	else
	{
		if (!detail::bone_name(t[0]))
			return false;
		p.bone = t[0];
		const size_t count = t.size() - 1;
		if (count != 0 && count != 1 && count != 3 && count != 4)
			return false;
		for (size_t i = 0; i < count; ++i)
			if (!detail::number(t[i + 1], n[i]))
				return false;
		if (count == 1)
			p.yaw_deg = n[0];
		else if (count >= 3)
		{
			p.offset = seat_vec(n[0], n[1], n[2]);
			if (count == 4)
				p.yaw_deg = n[3];
		}
	}
	if (!detail::near_car(p.offset) || std::fabs(p.yaw_deg) > 360.f)
		return false;
	out.bone = p.bone;
	out.offset = p.offset;
	out.yaw_deg = p.yaw_deg;
	return true;
}

// exit_<i>: left | right | back.
inline bool parse_exit_line(const std::string& value, ExitSide& out)
{
	const std::string v = detail::trim(value);
	if (v == "left")
		out = ExitSide::Left;
	else if (v == "right")
		out = ExitSide::Right;
	else if (v == "back")
		out = ExitSide::Back;
	else
		return false;
	return true;
}

// eye: x, y, z in a seat's own space, each within kSeatMaxEye.
inline bool parse_eye_line(const std::string& value, SeatVec& out)
{
	const std::vector<std::string> t = detail::split_commas(value);
	float n[3];
	if (t.size() != 3)
		return false;
	for (size_t i = 0; i < 3; ++i)
		if (!detail::number(t[i], n[i]) || std::fabs(n[i]) > kSeatMaxEye)
			return false;
	out = seat_vec(n[0], n[1], n[2]);
	return true;
}

// The value of `key` in the car's section: true and `value` when the line exists.
using SeatLine = std::function<bool(const char* key, std::string& value)>;

// One section. No `seats` line: the driver alone (no error). A malformed `seats`: the driver alone. seat_<i> missing or malformed:
// the places end before it. A malformed exit_<i>: the side of the seat. A malformed eye: the driver's.
inline CarSeats parse_car_seats(const SeatLine& line)
{
	CarSeats out;
	std::string v;
	if (!line("seats", v))
		return out;
	const std::string count_text = detail::trim(v);
	char* end = nullptr;
	const long want = std::strtol(count_text.c_str(), &end, 10);
	if (count_text.empty() || end != count_text.c_str() + count_text.size() || want < 1 || want > long(kMaxSeats))
	{
		out.errors.push_back("seats: not a whole number 1.." + std::to_string(int(kMaxSeats)) + ": the driver alone");
		return out;
	}
	for (long i = 1; i < want; ++i)
	{
		const std::string key = "seat_" + std::to_string(i);
		if (!line(key.c_str(), v))
		{
			out.errors.push_back(key + ": missing: " + std::to_string(i) + " place(s)");
			break;
		}
		SeatPlace p;
		if (!parse_seat_line(v, p))
		{
			out.errors.push_back(key + ": malformed: " + std::to_string(i) + " place(s)");
			break;
		}
		const std::string exit_key = "exit_" + std::to_string(i);
		if (line(exit_key.c_str(), v))
		{
			if (parse_exit_line(v, p.exit))
				p.exit_given = true;
			else
				out.errors.push_back(exit_key + ": not left, right or back: the side of the seat");
		}
		out.seat[i] = p;
		out.count = uint8_t(i + 1);
	}
	if (line("eye", v))
	{
		if (parse_eye_line(v, out.eye))
			out.eye_given = true;
		else
			out.errors.push_back("eye: not x, y, z within " + std::to_string(int(kSeatMaxEye)) + " m: the driver's");
	}
	return out;
}

// ---------------------------------------------------------------- where a passenger sits

// The seat in the car's space: `base` is the bone's model transform for the bone form (the driver's place for `driver`), the
// identity for the point form. Moved by the offset in the car's space, then turned about its own up axis by the yaw.
inline SeatXform seat_local(const SeatXform& base, const SeatPlace& p)
{
	SeatXform b = base;
	b.c = b.c + p.offset;
	return compose(b, turn_y(seat_rad(p.yaw_deg)));
}

// exit_<i> when given, else the side the seat is on (x < 0: left).
inline ExitSide exit_side_of(const SeatPlace& p, const SeatVec& seat_pos)
{
	if (p.exit_given)
		return p.exit;
	return seat_pos.x < 0.f ? ExitSide::Left : ExitSide::Right;
}

// Beyond the car's box: a standing body (~0.35 m) and a margin.
constexpr float kSeatExitClearance = 0.7f;

// The door point of a seat in the car's space (its box `lo` .. `hi`): beside the box on the exit side, level with the seat along
// the car (left / right) or across it (back), at the box's middle height (the decider finds the ground under it).
inline SeatVec seat_exit_local(const SeatVec& lo, const SeatVec& hi, const SeatVec& seat_pos, ExitSide side)
{
	auto clampf = [](float v, float a, float b) { return v < a ? a : (v > b ? b : v); };
	SeatVec p;
	p.y = (lo.y + hi.y) * 0.5f;
	switch (side)
	{
	case ExitSide::Left:
		p.x = lo.x - kSeatExitClearance;
		p.z = clampf(seat_pos.z, lo.z, hi.z);
		break;
	case ExitSide::Right:
		p.x = hi.x + kSeatExitClearance;
		p.z = clampf(seat_pos.z, lo.z, hi.z);
		break;
	case ExitSide::Back:
		p.x = clampf(seat_pos.x, lo.x, hi.x);
		p.z = lo.z - kSeatExitClearance;
		break;
	}
	return p;
}

// ---------------------------------------------------------------- the passenger's eye and look

// A seated body's eye above its seat when the model tells nothing sane (a stock driver's camera_pos sits ~0.7-0.8 m above his
// driver_place).
inline SeatVec default_seat_eye() { return seat_vec(0.f, 0.75f, 0.1f); }

// An eye a seated body can have: over the seat, not beside it.
inline bool seat_eye_sane(const SeatVec& e)
{
	return std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z) && std::fabs(e.x) <= 0.4f && e.y >= 0.3f &&
	    e.y <= 1.3f && e.z >= -0.4f && e.z <= 0.6f;
}

// The passenger's eye in a seat's own space: the section's `eye`, else the driver's camera (car_definition camera_pos, the car's
// space) in his seat's space (`driver_seat`: the driver's place in the car's space) when sane, else default_seat_eye().
inline SeatVec seat_eye(const CarSeats* seats, bool has_camera, const SeatVec& camera_pos, const SeatXform& driver_seat)
{
	if (seats && seats->eye_given)
		return seats->eye;
	if (has_camera)
	{
		const SeatVec e = unapply(driver_seat, camera_pos);
		if (seat_eye_sane(e))
			return e;
	}
	return default_seat_eye();
}

// The stock car camera's limits (system.ltx [car_firsteye_cam], radians, relative to the car): the passenger looks as far.
// Camera pitch: positive is down (the world direction is setHP(-yaw, -pitch)).
struct LookLimits
{
	float yaw_lo = -2.f;
	float yaw_hi = 2.f;
	float pitch_lo = -0.5f;
	float pitch_hi = 0.3f;
};

// The passenger's look relative to his seat. The actor's camera keeps world angles (the mouse moves them); each frame the change
// since the angles were last set here is his free look, the seat's turn is not.
struct RideLook
{
	float rel_yaw = 0.f;
	float rel_pitch = 0.f;
	float set_yaw = 0.f; // the camera's angles as last set here
	float set_pitch = 0.f;
	bool started = false;
};

// The start of a ride: straight ahead, the pitch in the middle of the limits (as CCameraBase::Load starts a clamped camera).
inline RideLook ride_look_start(const LookLimits& lim)
{
	RideLook l;
	l.rel_pitch = (lim.pitch_lo + lim.pitch_hi) * 0.5f;
	return l;
}

// Takes the mouse's change of the camera since the last set, within the limits.
inline void look_follow(RideLook& l, float cam_yaw, float cam_pitch, const LookLimits& lim)
{
	if (l.started)
	{
		l.rel_yaw += seat_wrap(cam_yaw - l.set_yaw);
		l.rel_pitch += seat_wrap(cam_pitch - l.set_pitch);
	}
	auto clampf = [](float v, float a, float b) { return v < a ? a : (v > b ? b : v); };
	l.rel_yaw = clampf(l.rel_yaw, lim.yaw_lo, lim.yaw_hi);
	l.rel_pitch = clampf(l.rel_pitch, lim.pitch_lo, lim.pitch_hi);
}

// The camera's world angles for the look relative to the seat (`seat`: its world transform), remembered as set. X-Ray: a camera's
// direction is setHP(-yaw, -pitch); Fvector::getH() = atan2(-x, z), getP() = atan2(y, |xz|).
inline void look_world(const SeatXform& seat, RideLook& l, float& cam_yaw, float& cam_pitch)
{
	const float cy = std::cos(l.rel_yaw), sy = std::sin(l.rel_yaw);
	const float cp = std::cos(l.rel_pitch), sp = std::sin(l.rel_pitch);
	const SeatVec local = seat_vec(cp * sy, -sp, cp * cy); // setHP(-rel_yaw, -rel_pitch)
	SeatVec d = apply_dir(seat, local);
	const float n = std::sqrt(dot(d, d));
	if (n > 1e-6f)
		d = d * (1.f / n);
	const float flat = std::sqrt(d.x * d.x + d.z * d.z);
	const float h = flat > 1e-6f ? std::atan2(-d.x, d.z) : 0.f;
	const float p = std::atan2(d.y, flat);
	cam_yaw = seat_wrap(-h);
	cam_pitch = -p;
	l.set_yaw = cam_yaw;
	l.set_pitch = cam_pitch;
	l.started = true;
}
} // namespace vehicles
} // namespace coop
