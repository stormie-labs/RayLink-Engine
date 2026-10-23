// Anomaly Coop — HUD v2.1: the name tag over another player (drawn by xrGame/coop/coop_presence.cpp render_hud): what
// it shows and how strongly. Pure C++17, header-only, no engine dependencies: used by coop_presence.cpp and by the
// unit-test exe (tests/test_nametag.cpp).
//
// Owner 2026-10-23 19:40 on build 9: the old tag ("player_b  2 m" on one line, small blue Arial, alpha in three steps)
// should be presentable, the distance under the nick, prettier, and its transparency should "play". Look (HUD v2
// language: flat, no glow, a dark 1 px edge; docs/hud/hud_v21_mockup.png), from the head upwards:
//   - an icon: a chevron in the player's colour pointing at him (the compass marks allies the same way); wounded: the
//     red ring with a cross, pulsing at 1 Hz and a bit bigger; dead / spectating / returning: a grey skull;
//   - a line: the distance, smaller and dimmer; wounded: the bleed-out timer and the distance in red ("0:42  34 m");
//   - the nick in white (grey when dead), a 1 px dark outline and a drop shadow: readable on a light sky.
// Alpha: full up to near_m, smoothstep down to far_alpha at far_m; a wounded player behind a wall stays shown (to be
// found and raised) at x wall_alpha (the others are hidden there, coop_presence.cpp); near the centre of the screen
// (the crosshair) the tag steps aside to aim_alpha so it never hides the target (wounded: aim_alpha_down).
// Every number can be overridden by a line tag_* in [coop_hud_layout] (configs/mod_system_coop.ltx), read once per
// session (set_param checks the range).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace coop
{
namespace nametag
{
enum class State : uint8_t
{
	Alive,
	Wounded,
	Dead, // also spectating and returning
};

enum class Icon : uint8_t
{
	Chevron,
	Down,
	Skull,
};

constexpr uint32_t kWhite = 0xF4F5F6; // the white of HUD v2 (#F4F5F6)

struct Params
{
	float near_m = 20.f; // full alpha up to here
	float far_m = 250.f; // far_alpha from here on (tags end at 400 m)
	float far_alpha = 0.4f;
	float wall_alpha = 0.6f; // a wounded player behind a wall (the others are not shown there)
	float aim_inner = 0.05f; // of the screen height from its centre: the tag is at aim_alpha
	float aim_outer = 0.12f; // from here on it is not dimmed
	float aim_alpha = 0.45f;
	float aim_alpha_down = 0.75f; // a wounded one stays easy to find
	float line_alpha = 0.72f; // the distance line, of the tag's alpha
	float outline_alpha = 0.55f; // the 1 px outline of the text, of its alpha
	float shadow_alpha = 0.8f; // the drop shadow (+1, +1), of the text's alpha
	float pulse_low = 0.6f; // wounded: the icon and the line pulse between this and 1 at 1 Hz
	float icon_px = 9.f; // the chevron, px at 768 high
	float icon_state_px = 11.f; // the down ring, the skull
	float gap_px = 2.f; // between the icon and the line (screen px at 768 high)
};

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

inline float smoothstep01(float t)
{
	t = clamp01(t);
	return t * t * (3.f - 2.f * t);
}

// 1 up to near_m, smoothly down to far_alpha at far_m and beyond.
inline float distance_alpha(float dist_m, const Params& p)
{
	if (!(dist_m > p.near_m))
		return 1.f;
	const float t = smoothstep01((dist_m - p.near_m) / (p.far_m - p.near_m));
	return 1.f - (1.f - p.far_alpha) * t;
}

// The step aside at the crosshair: (dx, dy) is the tag's anchor from the centre of the screen, px.
inline float aim_factor(float dx_px, float dy_px, float screen_h_px, State s, const Params& p)
{
	if (!(screen_h_px > 0.f))
		return 1.f;
	const float r = std::sqrt(dx_px * dx_px + dy_px * dy_px) / screen_h_px;
	const float low = s == State::Wounded ? p.aim_alpha_down : p.aim_alpha;
	const float t = smoothstep01((r - p.aim_inner) / (p.aim_outer - p.aim_inner));
	return low + (1.f - low) * t;
}

// 1 Hz: 1 at a whole second, pulse_low half a second later.
inline float pulse(uint64_t now_ms, const Params& p)
{
	const float ph = float(now_ms % 1000u) / 1000.f;
	const float c = 0.5f + 0.5f * std::cos(ph * 6.28318530718f); // 1 -> 0 -> 1
	return p.pulse_low + (1.f - p.pulse_low) * c;
}

struct Look
{
	Icon icon = Icon::Chevron;
	uint32_t nick_rgb = kWhite;
	uint32_t line_rgb = kWhite;
	uint32_t icon_rgb = kWhite;
	float alpha = 1.f; // the nick
	float line_alpha = 1.f;
	float icon_alpha = 1.f;
	float icon_scale = 1.f;
	float icon_px = 9.f;
};

// The tag of a player in state `s`, `dist_m` away, behind a wall or not, at `now_ms`; the colours: the player's own,
// a wounded one's and a dead one's (the palette [coop_player_colors]). The step aside at the crosshair comes on top.
inline Look look(State s, float dist_m, bool behind_wall, uint64_t now_ms, uint32_t player_rgb, uint32_t down_rgb,
    uint32_t dead_rgb, const Params& p)
{
	Look l;
	float a = distance_alpha(dist_m, p);
	if (behind_wall)
		a *= p.wall_alpha;
	l.alpha = a;
	switch (s)
	{
	case State::Wounded:
	{
		const float pl = pulse(now_ms, p);
		l.icon = Icon::Down;
		l.icon_rgb = down_rgb;
		l.line_rgb = down_rgb;
		l.icon_px = p.icon_state_px;
		l.icon_alpha = a * pl;
		l.line_alpha = a * pl;
		l.icon_scale = 1.f + 0.12f * (pl - p.pulse_low) / (1.f - p.pulse_low);
		break;
	}
	case State::Dead:
		l.icon = Icon::Skull;
		l.icon_rgb = dead_rgb;
		l.nick_rgb = dead_rgb;
		l.line_rgb = dead_rgb;
		l.icon_px = p.icon_state_px;
		l.icon_alpha = a;
		l.line_alpha = a * p.line_alpha;
		break;
	default:
		l.icon = Icon::Chevron;
		l.icon_rgb = player_rgb;
		l.icon_px = p.icon_px;
		l.icon_alpha = a;
		l.line_alpha = a * p.line_alpha;
		break;
	}
	return l;
}

inline uint32_t argb(float a01, uint32_t rgb)
{
	const uint32_t a = uint32_t(clamp01(a01) * 255.f + 0.5f);
	return (a << 24) | (rgb & 0xFFFFFFu);
}

// "26 m" under 100 m, "120 m" (10 m steps) under 1 km, "1.2 km" (the compass's coop_allies_map.format_distance).
inline int format_distance(char* out, size_t n, float dist_m, const char* m, const char* km)
{
	if (!(dist_m >= 0.f))
		dist_m = 0.f;
	if (dist_m < 99.5f)
		return std::snprintf(out, n, "%d %s", int(dist_m + 0.5f), m);
	if (dist_m < 995.f)
		return std::snprintf(out, n, "%d %s", int(dist_m / 10.f + 0.5f) * 10, m);
	return std::snprintf(out, n, "%.1f %s", dist_m / 1000.f, km);
}

// The line under the nick: the distance (when shown); a wounded one: the bleed-out timer first ("0:42  34 m"). Two
// spaces between them: no glyph a font may lack. Empty: no line.
inline void format_line(char* out, size_t n, State s, bool with_dist, float dist_m, uint32_t bleed_ms, const char* m,
    const char* km)
{
	if (!out || n == 0)
		return;
	out[0] = 0;
	char dist[32] = {};
	if (with_dist)
		format_distance(dist, sizeof(dist), dist_m, m, km);
	if (s == State::Wounded && bleed_ms > 0)
	{
		const uint32_t sec = (bleed_ms + 999u) / 1000u;
		if (dist[0])
			std::snprintf(out, n, "%u:%02u  %s", sec / 60u, sec % 60u, dist);
		else
			std::snprintf(out, n, "%u:%02u", sec / 60u, sec % 60u);
		return;
	}
	std::snprintf(out, n, "%s", dist);
}

// Screen px, y down. The icon's bottom sits on the anchor (the point over the head); the line over it, the nick over
// the line. Tops of the text boxes and the icon's centre.
struct Stack
{
	float nick_y = 0.f;
	float line_y = 0.f;
	float icon_cy = 0.f;
};

inline Stack stack(float anchor_y, float nick_h, float line_h, float icon_h, float gap, bool has_line)
{
	Stack s;
	s.icon_cy = anchor_y - icon_h / 2.f;
	const float over_icon = anchor_y - icon_h - gap;
	if (has_line)
	{
		s.line_y = over_icon - line_h;
		s.nick_y = s.line_y - 1.f - nick_h;
	}
	else
	{
		s.line_y = over_icon;
		s.nick_y = over_icon - nick_h;
	}
	return s;
}

// [coop_hud_layout] tag_* lines: key, field, accepted range.
struct ParamKey
{
	const char* key;
	float Params::*field;
	float lo;
	float hi;
};

inline const ParamKey* param_keys(size_t& n)
{
	static const ParamKey keys[] = {
		{"tag_near", &Params::near_m, 0.f, 400.f},
		{"tag_far", &Params::far_m, 1.f, 1000.f},
		{"tag_far_alpha", &Params::far_alpha, 0.f, 1.f},
		{"tag_wall_alpha", &Params::wall_alpha, 0.f, 1.f},
		{"tag_aim_inner", &Params::aim_inner, 0.f, 0.5f},
		{"tag_aim_outer", &Params::aim_outer, 0.f, 0.5f},
		{"tag_aim_alpha", &Params::aim_alpha, 0.f, 1.f},
		{"tag_aim_alpha_down", &Params::aim_alpha_down, 0.f, 1.f},
		{"tag_line_alpha", &Params::line_alpha, 0.f, 1.f},
		{"tag_outline_alpha", &Params::outline_alpha, 0.f, 1.f},
		{"tag_shadow_alpha", &Params::shadow_alpha, 0.f, 1.f},
		{"tag_pulse_low", &Params::pulse_low, 0.f, 0.99f},
		{"tag_icon", &Params::icon_px, 2.f, 64.f},
		{"tag_icon_state", &Params::icon_state_px, 2.f, 64.f},
		{"tag_gap", &Params::gap_px, 0.f, 32.f},
	};
	n = sizeof(keys) / sizeof(keys[0]);
	return keys;
}

// Sets the value of a tag_* line; false (nothing changed): an unknown key, a value out of its range or not a number,
// or one that breaks an order (near < far, inner < outer).
inline bool set_param(Params& p, const char* key, float v)
{
	if (!key || !(v == v))
		return false;
	size_t n = 0;
	const ParamKey* keys = param_keys(n);
	for (size_t i = 0; i < n; ++i)
	{
		if (std::strcmp(keys[i].key, key) != 0)
			continue;
		if (v < keys[i].lo || v > keys[i].hi)
			return false;
		Params t = p;
		t.*(keys[i].field) = v;
		if (!(t.near_m < t.far_m) || !(t.aim_inner < t.aim_outer))
			return false;
		p = t;
		return true;
	}
	return false;
}
} // namespace nametag
} // namespace coop
