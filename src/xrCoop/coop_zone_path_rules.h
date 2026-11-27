// Anomaly Coop - the wander path of a moving anomaly zone across machines.
//
// DAO (drx_da_main.script anomalies_spawn_functions.zone_mine_ghost) gives each Ghost a path around the point it spawned at: a
// start angle i of {0, 45, 90, 135, 180}, a step of i of {1, 2, 3}, a step of j that depends on it ({2, 3} / {1, 3} / {2, 4}),
// two amplitudes random() * 2.5 + 0.5 with a random sign. Then its actor_on_update handler moves the zone every frame
// (set_anomaly_position -> CCustomZone::MoveScript) and steps i and j by floor(step * min(50, frame ms) / 22) degrees. Each
// machine rolled its own path and stepped it by its own frame time, while that zone is also the damage: a player's own anomaly
// damage comes from his copy of the zone. Players saw the Ghost in different places, and a client's damage was away from the
// Ghost of the host.
//
// Now the process that leads the map rolls the path (DAO's own choices, scripts\coop_mods_dao_fx.script) and the engine runs it
// on every machine by one rule, with no frame time in it: the path runs on the clock of the machine, one tick is 22 ms (the
// mod's divisor: 1 tick = 1 step at its intended speed). Its viewers get the path and the time it has run (ZoneState kind
// Paths) and put their copy of the zone on it; a player who comes later gets the same, with the time it has run by then.
//
// Rule, per tick n since the path started (it repeats after period_ticks):
//   i = (i0 + i_step * n) mod 360, j = (j_step * n) mod 360                 degrees, the mod's i and j
//   x = origin.x + sin(i) * x_mod, z = origin.z + sin(j) * z_mod            sin as the mod's sin_lut: sin(deg * 0.0174533)
//   y = the mod's height rule over the ticks of the path: the ground under the point (the level graph's vertex under x, the
//       spawn height, z) when it is less than 0.5 m from the height of the step before, else the spawn height. It is run
//       twice round the path and the second round kept: the same on every machine, whenever the path started there.
//
// Wire (Msg::ZoneState kind 2 Paths, reliable, from the process that leads the map to the players of that map):
//   u16 count, count x {u16 zone (host numbering), u32 section hash (FNV-1a of the zone's section), vec3 origin, u16 i0,
//   u8 i_step, u8 j_step, f32 x_mod, f32 z_mod, u32 phase_ms (time the path had run when it left, modulo its period)}
// A viewer puts its copy on it only when the copy has that section and stands within kMaxOriginShift of the origin.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include "coop_hash.h"
#include "coop_ids.h"
#include "coop_wire.h"

namespace coop
{
namespace zonepath
{
constexpr uint32_t kTickMs = 22; // drx_da_main: delta = min(50, time_delta) / 22 steps per frame
constexpr uint32_t kCircle = 360; // full_circle
constexpr double kDegToRad = 0.0174533; // anomalies_vars.sin_lut: sin(i * 0.0174533)
constexpr float kMaxStepY = 0.5f; // max_offset_y: the ground is followed while it moves less than this per step
constexpr float kMinAmplitude = 0.5f; // generate_distance: random() * 2.5 + 0.5
constexpr float kMaxAmplitude = 3.0f;
constexpr float kAmplitudeSlack = 0.001f; // a float that went through Lua and the wire
constexpr uint8_t kMaxIStep = 3; // random_choice(1, 2, 3)
constexpr uint8_t kMaxJStep = 4; // random_choice(2, 4) at most
constexpr float kMaxCoord = 100000.f; // a level is a few km wide
// The farthest a Ghost gets from its origin is |x_mod| and |z_mod| at most 3 m each plus its height rule: a copy farther from
// the origin than this is not the zone the path was rolled for.
constexpr float kMaxOriginShift = 10.f;
constexpr uint16_t kMaxPathsPerMessage = 4000; // the u16 count of a Batcher with count16
constexpr uint64_t kPendingTtlMs = 300000; // a viewer keeps a path for a copy that is not here yet this long

struct Path
{
	uint16_t id = kNoId; // the zone: host numbering on the wire, local after the viewer maps it
	uint32_t section_hash = 0;
	float origin[3] = {0.f, 0.f, 0.f}; // the point it spawned at (DAO: the binder's spawn_position)
	uint16_t i0 = 0; // start angle of i, degrees
	uint8_t i_step = 1;
	uint8_t j_step = 1;
	float x_mod = kMinAmplitude; // signed amplitude along x, metres
	float z_mod = kMinAmplitude; // along z
};

struct Point
{
	float x = 0.f, y = 0.f, z = 0.f;
};

// |v| without std::fabs (the engine marks it deprecated: C4995 in every file that includes this one).
inline float magnitude(float v) { return v < 0.f ? -v : v; }

inline bool coord_ok(float v) { return std::isfinite(v) && magnitude(v) < kMaxCoord; }

inline bool amplitude_ok(float a)
{
	if (!std::isfinite(a))
		return false;
	const float m = magnitude(a);
	return m >= kMinAmplitude - kAmplitudeSlack && m <= kMaxAmplitude + kAmplitudeSlack;
}

// Every value DAO can roll, and nothing else.
inline bool valid(const Path& p)
{
	return p.i0 < kCircle && p.i_step >= 1 && p.i_step <= kMaxIStep && p.j_step >= 1 && p.j_step <= kMaxJStep &&
		amplitude_ok(p.x_mod) && amplitude_ok(p.z_mod) && coord_ok(p.origin[0]) && coord_ok(p.origin[1]) &&
		coord_ok(p.origin[2]);
}

// Ticks until an angle that moves `step` degrees a tick is back where it started.
inline uint32_t cycle_of(uint32_t step) { return step ? kCircle / std::gcd(kCircle, step) : 1; }

// Ticks until the path is back where it started (both angles): 90..360.
inline uint32_t period_ticks(const Path& p)
{
	const uint32_t a = cycle_of(p.i_step);
	const uint32_t b = cycle_of(p.j_step);
	return a / std::gcd(a, b) * b;
}

inline uint32_t period_ms(const Path& p) { return period_ticks(p) * kTickMs; }

// The tick of the path after it has run `run_ms`, within its period.
inline uint32_t tick_at(const Path& p, uint64_t run_ms) { return uint32_t((run_ms / kTickMs) % period_ticks(p)); }

// The time it has run, as it goes on the wire: within its period, the same tick.
inline uint32_t phase_ms(const Path& p, uint64_t run_ms) { return uint32_t(run_ms % period_ms(p)); }

// How long a path has run at `now_ms` on this machine: it had run `base_run_ms` at `base_ms` (the process that rolled it: its
// start and 0; a viewer: when the word came and the phase in it). Never goes back.
inline uint64_t run_at(uint64_t base_ms, uint32_t base_run_ms, uint64_t now_ms)
{
	return uint64_t(base_run_ms) + (now_ms > base_ms ? now_ms - base_ms : 0);
}

inline void angles(const Path& p, uint32_t tick, uint32_t& i, uint32_t& j)
{
	i = uint32_t((uint64_t(p.i0) + uint64_t(p.i_step) * tick) % kCircle);
	j = uint32_t((uint64_t(p.j_step) * tick) % kCircle);
}

inline float sin_deg(uint32_t deg) { return float(std::sin(double(deg) * kDegToRad)); }

inline void xz_at(const Path& p, uint32_t tick, float& x, float& z)
{
	uint32_t i = 0, j = 0;
	angles(p, tick, i, j);
	x = p.origin[0] + sin_deg(i) * p.x_mod;
	z = p.origin[2] + sin_deg(j) * p.z_mod;
}

// The height of each tick of one period. `ground(x, y, z, out_y) -> bool` answers the level graph under the point (DAO:
// level.vertex_id(vector(x, spawn y, z)), then level.vertex_position(id).y); false when there is no vertex. It is asked once per
// tick of the period.
template <class Ground>
inline std::vector<float> heights(const Path& p, Ground&& ground)
{
	const uint32_t n = period_ticks(p);
	const float spawn_y = p.origin[1];
	std::vector<float> under(n, 0.f);
	std::vector<uint8_t> has(n, 0);
	for (uint32_t k = 0; k < n; ++k)
	{
		float x = 0.f, z = 0.f, g = 0.f;
		xz_at(p, k, x, z);
		if (ground(x, spawn_y, z, g) && std::isfinite(g))
		{
			under[k] = g;
			has[k] = 1;
		}
	}
	std::vector<float> out(n, spawn_y);
	float prev = spawn_y;
	for (int round = 0; round < 2; ++round)
	{
		for (uint32_t k = 0; k < n; ++k)
		{
			float y = spawn_y;
			if (has[k] && magnitude(under[k] - prev) < kMaxStepY)
				y = under[k];
			prev = y;
			if (round == 1)
				out[k] = y;
		}
	}
	return out;
}

// Where the zone is after the path has run `run_ms`; `ys` from heights() (the spawn height when it is not there).
inline Point point_at(const Path& p, const std::vector<float>& ys, uint64_t run_ms)
{
	const uint32_t tick = tick_at(p, run_ms);
	Point out;
	xz_at(p, tick, out.x, out.z);
	out.y = tick < ys.size() ? ys[tick] : p.origin[1];
	return out;
}

// A viewer's copy is the zone the path was rolled for: the same section, and near its origin.
inline bool same_zone(uint32_t sent_hash, uint32_t copy_hash, const float origin[3], const float copy_pos[3])
{
	if (sent_hash != copy_hash)
		return false;
	const float dx = copy_pos[0] - origin[0], dy = copy_pos[1] - origin[1], dz = copy_pos[2] - origin[2];
	const float d2 = dx * dx + dy * dy + dz * dz;
	return std::isfinite(d2) && d2 <= kMaxOriginShift * kMaxOriginShift;
}

// The section hash of a path. Transitional: coop_objects.cpp still calls it; it goes when that file calls fnv1a32.
inline uint32_t name_hash(const char* s) { return fnv1a32(s); }

inline void write_path(Writer& w, const Path& p, uint32_t phase)
{
	w.u16(p.id);
	w.u32(p.section_hash);
	w.vec3(p.origin);
	w.u16(p.i0);
	w.u8(p.i_step);
	w.u8(p.j_step);
	w.f32(p.x_mod);
	w.f32(p.z_mod);
	w.u32(phase);
}

// False on an overrun, a value DAO cannot roll, or a phase past the period.
inline bool read_path(Reader& r, Path& p, uint32_t& phase)
{
	p.id = r.u16();
	p.section_hash = r.u32();
	r.vec3(p.origin);
	p.i0 = r.u16();
	p.i_step = r.u8();
	p.j_step = r.u8();
	p.x_mod = r.f32();
	p.z_mod = r.f32();
	phase = r.u32();
	return r.ok() && valid(p) && phase < period_ms(p);
}
} // namespace zonepath
} // namespace coop
