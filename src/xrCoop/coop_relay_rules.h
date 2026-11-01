// Anomaly Coop — what a client gets of the host's creatures and machines beyond their bodies, and how (parity audits
// 2026-10-31: sound H2, H3, M5; world W2, W3, W8; objects F6, F7). The host is a client with direct access: a client hears and
// sees what the host does, within what the wire can carry.
//
// Shots. A shot of a host NPC or helicopter is one small event, a firefight is many, and on the receiver each can spawn
// bullets (tracers, impacts, holes, the whine of a bullet going by). Every client has a budget of its own, measured from
// its character: near the muzzle or near the line of fire it gets the bullets while their bucket holds; farther off the
// sound and the flame only, thinned to kFarShotsPerSec; past kNpcShotRange nothing.
// Lanes. A shot that opens a burst and a long voice go on the reliable lane while a small bucket holds: a lost one is a
// whole silence; the rest of a burst, a short sound, is covered by what follows it.
// Lamps. CHangingLamp has no sync of its own: the host keeps its last word on each lamp (LampBook) and sends the changes,
// the whole book at a join (Msg::LampState).
//
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_wire.h"

namespace coop
{
namespace relay
{
struct Vec
{
	float x = 0.f, y = 0.f, z = 0.f;
};

// Times in ms. A bucket that was never used starts full.
struct Bucket
{
	float tokens = -1.f;
	uint64_t last = 0;

	bool take(uint64_t now, float per_sec, float burst, float cost = 1.f)
	{
		if (tokens < 0.f)
			tokens = burst;
		else if (now > last)
			tokens = std::fmin(burst, tokens + float(now - last) * per_sec / 1000.f);
		last = now;
		if (tokens < cost)
			return false;
		tokens -= cost;
		return true;
	}
};

// How close the shot from `from` along the unit `dir`, `range` long, passes `p`.
inline float line_distance(const Vec& from, const Vec& dir, float range, const Vec& p)
{
	const Vec v{p.x - from.x, p.y - from.y, p.z - from.z};
	float along = v.x * dir.x + v.y * dir.y + v.z * dir.z;
	along = along < 0.f ? 0.f : (along > range ? range : along);
	const Vec off{v.x - dir.x * along, v.y - dir.y * along, v.z - dir.z * along};
	return std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
}

constexpr float kNpcShotRange = 500.f; // a shot goes to the players this close to the muzzle
constexpr float kBulletMuzzleRange = 150.f; // a fight this close shows its bullets: tracers, dust, holes
constexpr float kBulletLineRange = 30.f; // a line of fire this close too, wherever the muzzle: the whine, the hits near him
constexpr float kBulletsPerSec = 60.f, kBulletsBurst = 90.f;
constexpr float kFarShotsPerSec = 15.f, kFarShotsBurst = 20.f; // the crackle of a fight beyond kBulletMuzzleRange
constexpr float kFirmPerSec = 4.f, kFirmBurst = 8.f; // shots on the reliable lane
constexpr uint64_t kBurstGapMs = 400; // this long since the shooter's last shot: the next one opens a burst

// `last`: the shooter's last shot (0: none yet); it moves to `now`.
inline bool opens_burst(uint64_t now, uint64_t& last)
{
	const bool opens = last == 0 || now - last >= kBurstGapMs;
	last = now;
	return opens;
}

struct ShotPlan
{
	bool send = false;
	bool bullets = false;
	bool firm = false; // reliable lane
};

class ShotBudget
{
public:
	// `to_muzzle`, `to_line`: from this client's character; `bullets`: as many as the event carries.
	ShotPlan plan(uint64_t now, float to_muzzle, float to_line, uint32_t bullets, bool opens)
	{
		ShotPlan p;
		if (to_muzzle > kNpcShotRange)
			return p;
		const bool close = to_muzzle <= kBulletMuzzleRange || to_line <= kBulletLineRange;
		if (!close && !m_far.take(now, kFarShotsPerSec, kFarShotsBurst))
			return p;
		p.send = true;
		p.bullets = close && m_bullets.take(now, kBulletsPerSec, kBulletsBurst, float(bullets ? bullets : 1));
		p.firm = opens && m_firm.take(now, kFirmPerSec, kFirmBurst);
		return p;
	}

private:
	Bucket m_bullets, m_far, m_firm;
};

// --- creature sounds (EntitySound)
constexpr float kLongSoundSec = 1.f;
constexpr float kFirmSoundsPerSec = 6.f, kFirmSoundsBurst = 12.f;

// A line a player follows, a dying creature's last sound: a lost one has no repeat.
inline bool sound_matters(float length_sec, bool owner_dead) { return owner_dead || length_sec >= kLongSoundSec; }

// --- the rest of what an NPC's weapon does (audit F6). A puppet's weapon has no state machine of its own, so the host names
// the sound; the show and hide sounds stay out: a puppet plays those itself (its object handler takes the host's hold).
constexpr float kWeaponSoundRange = 60.f;
constexpr uint8_t kNoWeaponSound = 0xff;
inline const char* const* weapon_sounds(size_t& count)
{
	static const char* const names[] = {"sndReload", "sndReloadEmpty", "sndReloadMisfire", "sndReloadG", "sndEmptyClick",
	    "sndClickMisfire", "sndOpen", "sndAddCartridge", "sndClose", "sndCloseEmpty", "sndSwitchMode", "sndSwitch",
	    "sndShot" /* a knife's swing: guns play theirs outside CHudItem::PlaySound */, "sndCheckout", "sndThrow"};
	count = sizeof(names) / sizeof(names[0]);
	return names;
}
inline uint8_t weapon_sound_index(const char* alias)
{
	size_t n = 0;
	const char* const* names = weapon_sounds(n);
	for (size_t i = 0; alias && i < n; ++i)
		if (!std::strcmp(alias, names[i]))
			return uint8_t(i);
	return kNoWeaponSound;
}
inline const char* weapon_sound_alias(uint8_t index)
{
	size_t n = 0;
	const char* const* names = weapon_sounds(n);
	return index < n ? names[index] : nullptr;
}

// --- flinches (audit F7): a hit's flinch is additive, so it is not in the mirrored animations; the host names the hit and
// the puppet plays its own. Near players only, a fight is many hits.
constexpr float kFlinchRange = 80.f;
constexpr float kFlinchesPerSec = 12.f, kFlinchesBurst = 16.f;

// --- a helicopter (audits H2, W8). The host's scripts run it: what a client copies goes out on change and every second.
enum HeliFlags : uint8_t
{
	HeliEngine = 1, // TurnEngineSound
	HeliLight = 2, // TurnLighting
	HeliSmoke = 4, // StartFlame
	HeliDead = 8,
	HeliAiming = 16, // it has a target: the report's direction is where the gun looks
};
constexpr uint64_t kHeliStateMs = 1000;
constexpr uint32_t kHeliAimMs = 600; // a client's gun counts as firing this long after the last shot
constexpr uint32_t kHeliTrackMs = 2500; // a client's barrel holds the reported aim this long: a lost report does not drop it

inline uint8_t heli_health_byte(float health)
{
	const float h = health < 0.f ? 0.f : (health > 1.f ? 1.f : health);
	return uint8_t(h * 255.f + 0.5f);
}
inline float heli_health(uint8_t byte) { return float(byte) / 255.f; }

// Host: true when the state of one machine has to go out now.
struct HeliReport
{
	uint8_t flags = 0, health = 0;
	uint64_t next = 0;

	bool due(uint64_t now, uint8_t new_flags, uint8_t new_health)
	{
		if (new_flags == flags && new_health == health && now < next)
			return false;
		flags = new_flags;
		health = new_health;
		next = now + kHeliStateMs;
		return true;
	}
};

// --- lamps (audit W2)
enum LampBits : uint8_t
{
	LampOn = 1,
	LampDead = 2, // shot out: no script lights it again
	LampKnown = LampOn | LampDead,
};
constexpr uint64_t kLampSendMs = 100; // host: the changes of this long go out as one message
constexpr uint64_t kLampCheckMs = 500; // client: how often its copies are held to the host's word
constexpr uint64_t kLampPendingMs = 300000; // client: the word on a lamp that is not here yet
constexpr size_t kMaxLampsPerMsg = 128;

struct LampEntry
{
	uint16_t id = 0xFFFF;
	uint8_t state = 0;
};

// Msg::LampState (host -> clients): varu n, n x {u16 lamp (host numbering), u8 LampBits}.
inline void write_lamps(Writer& w, const LampEntry* entries, size_t count)
{
	w.varu(count);
	for (size_t i = 0; i < count; ++i)
	{
		w.u16(entries[i].id);
		w.u8(entries[i].state);
	}
}
// False (out cleared) on overrun, an oversized list or unknown bits.
inline bool read_lamps(Reader& r, std::vector<LampEntry>& out)
{
	out.clear();
	const uint64_t n = r.varu();
	if (!r.ok() || n > kMaxLampsPerMsg)
		return false;
	for (uint64_t i = 0; i < n; ++i)
	{
		LampEntry e;
		e.id = r.u16();
		e.state = r.u8();
		if (!r.ok() || (e.state & ~uint8_t(LampKnown)))
		{
			out.clear();
			return false;
		}
		out.push_back(e);
	}
	return true;
}

// Msg::Thunderbolt (host -> clients, parity audit W5): str collection, f32 period, f32 duration, u32 seed. The seed is the
// bolt: the same picture, the same direction from each machine's camera, the same clap (CEffect_Thunderbolt::Bolt).
struct BoltMsg
{
	std::string id; // thunderbolt collection of the weather
	float period = 0.f;
	float duration = 0.f;
	uint32_t seed = 0;
};

inline void write_bolt(Writer& w, const BoltMsg& b)
{
	w.str(b.id);
	w.f32(b.period);
	w.f32(b.duration);
	w.u32(b.seed);
}
// False on overrun, no collection, or numbers the effect could not run on.
inline bool read_bolt(Reader& r, BoltMsg& b)
{
	b.id = r.str();
	b.period = r.f32();
	b.duration = r.f32();
	b.seed = r.u32();
	return r.ok() && !b.id.empty() && std::isfinite(b.period) && std::isfinite(b.duration) && b.period >= 0.f && b.duration >= 0.f;
}

// Host: the last word on every lamp that changed in this world. The save carries the rest.
class LampBook
{
public:
	// True when the clients have to hear it. `soft`: the end of a flicker lights the lamp by itself on every machine, so
	// it is news only against an older word.
	bool note(uint16_t id, uint8_t state, bool soft = false)
	{
		auto it = m_lamps.find(id);
		if (it == m_lamps.end())
		{
			if (soft)
				return false;
			m_lamps[id] = state;
			return true;
		}
		if (it->second == state)
			return false;
		it->second = state;
		return true;
	}
	void clear() { m_lamps.clear(); }
	size_t size() const { return m_lamps.size(); }
	const std::map<uint16_t, uint8_t>& lamps() const { return m_lamps; }

private:
	std::map<uint16_t, uint8_t> m_lamps;
};
} // namespace relay
} // namespace coop
