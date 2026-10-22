// Anomaly Coop — the pure part of the checkpoint sources on the host (F3 Task 7, CHECKPOINTS_DEATH.md §2.2): the
// settings of mod_system_coop_checkpoints.ltx and their bounds, the CheckpointClaim message, the nearest source a
// player stands at, the objects mods declare as camps and the book of tents (who owns which one, and the claims that
// wait for a tent). The engine side is xrGame/coop/coop_checkpoint_sources.cpp.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop_checkpoint_rules.h"
#include "coop_wire.h"

namespace coop
{
namespace checkpoint
{
// ---------------------------------------------------------------- settings ([coop_checkpoints])

// A radius of a source, of the fight check or of a tent: a number in (0, kMaxSourceRadius] metres.
constexpr float kMaxSourceRadius = 500.0f;
// A period of a source: 0..kMaxSourcePeriodMs (10 minutes).
constexpr uint32_t kMaxSourcePeriodMs = 600000;
// tent_sections keeps at most this many names; a name is 1..64 characters of [A-Za-z0-9_].
constexpr size_t kMaxTentSections = 16;

struct SourceSettings
{
	float campfire_radius = 3.0f; // a lit campfire this close counts
	uint32_t campfire_dwell_ms = 5000; // stood by it this long, not in a fight
	float combat_radius = 50.0f; // an enemy that picked the player this close is a fight
	std::vector<std::string> tent_sections{"ph_tent", "ph_sleepbag"}; // the first one is what the host puts up
	float tent_rest_radius = 2.0f; // the own tent this close is a rest place
	float tent_claim_radius = 4.0f; // a claimed spot: this close to the player, the tent this close to the spot
	uint32_t tent_claim_ms = 15000; // a claim waits this long; the tent appeared at most this long before it
	std::string cover_list = "misc\\surge_manager.ltx"; // its [list]: the stock surge covers (rest places too)
};

inline bool radius_ok(float r) { return std::isfinite(r) && r > 0.0f && r <= kMaxSourceRadius; }
inline bool period_ok(double ms) { return std::isfinite(ms) && ms >= 0.0 && ms <= double(kMaxSourcePeriodMs); }

// "ph_tent, ph_sleepbag" -> {"ph_tent", "ph_sleepbag"}: split at commas, blanks trimmed; an empty, overlong or
// malformed name and a repeated one are skipped; at most `max_names` are kept, in order.
std::vector<std::string> split_names(const std::string& list, size_t max_names = kMaxTentSections);

// ---------------------------------------------------------------- Msg::CheckpointClaim

// `what` of CheckpointClaim (coop_protocol.h): the player put up a tent at the position (a client: the host puts it
// up for him, he cannot create a world object).
constexpr uint8_t kClaimTent = 1;

void write_claim(Writer& w, const float pos[3]);
// Strict: what = kClaimTent, a position within +-kMaxCoord (NaN and infinity fail), nothing after it. On false `out`
// is left as it was.
bool read_claim(Reader& rd, float out[3]);

// ---------------------------------------------------------------- the source a player stands at

struct SourceSpot
{
	uint16_t id = kNoSource;
	Kind kind = Kind::None; // Campfire or Mod
	float pos[3] = {0, 0, 0};
	float radius = 0.0f; // its own reach
};

// The spot nearest to `at` among those whose own radius reaches it (a tie goes to the lower id); nullptr if none.
const SourceSpot* nearest_spot(const std::vector<SourceSpot>& spots, const float at[3]);

// ---------------------------------------------------------------- objects mods declare as camps

constexpr size_t kMaxModSources = 256;

// coop.checkpoint.register_source(id, radius): an object a mod declares as a camp ("as a campfire": dwell, no fight).
class ModSources
{
public:
	// False for "no object" (0xffff), a radius that is not radius_ok, or a new id over kMaxModSources; a known id only
	// takes the new radius.
	bool add(uint16_t id, float radius);
	bool remove(uint16_t id);
	bool has(uint16_t id) const;
	const std::map<uint16_t, float>& all() const { return m_radius; }
	void clear() { m_radius.clear(); }

private:
	std::map<uint16_t, float> m_radius;
};

// ---------------------------------------------------------------- tents

constexpr size_t kMaxTents = 1024; // tents (and their owners) the host keeps track of; more are not kept
constexpr size_t kMaxClaims = 64; // claims waiting for a tent, one per character

struct TentSeen
{
	uint16_t id = kNoSource;
	float pos[3] = {0, 0, 0};
};

struct TentMatch
{
	uint16_t character = 0xffff;
	uint16_t tent = kNoSource;
};

// What the host knows of the tents of a world: when each one appeared, whose it is, and the claims ("I put up a tent
// here") that wait for one. A claim is closed by the nearest tent within reach of the claimed spot that nobody owns
// and that appeared at most `fresh_ms` before the claim (or after it); tents that stood when the world was served are
// never new.
class TentBook
{
public:
	// A newly served world: owners and claims are forgotten, and every tent the next observe() sees is old.
	void new_world();
	// The tents of tent_sections now on the level. A tent not known yet appeared at `now_ms` (old on the first call
	// after new_world()). False if some were not kept (kMaxTents).
	bool observe(const std::vector<TentSeen>& tents, uint64_t now_ms);
	// The tent is gone from the world: forgotten with its owner.
	void forget(uint16_t tent);
	// False if the owner is not kept (kMaxTents owners).
	bool set_owner(uint16_t tent, uint16_t character);
	uint16_t owner_of(uint16_t tent) const; // 0xffff if nobody's
	size_t owned_by(uint16_t character) const;
	// A claim of `character` at `pos`, open for `ttl_ms`; a newer one of the same character replaces his older one.
	// False if kMaxClaims other characters have one.
	bool claim(uint16_t character, const float pos[3], uint64_t now_ms, uint32_t ttl_ms);
	bool has_claim(uint16_t character) const;
	// Closes the claims these tents answer, oldest claim first (see the class); the tent becomes the claimant's.
	std::vector<TentMatch> match(const std::vector<TentSeen>& tents, float radius, uint32_t fresh_ms);
	// Drops the claims whose time is up; their characters.
	std::vector<uint16_t> expire(uint64_t now_ms);
	// Drops the claims of characters not in `present` (their players left the world); their characters.
	std::vector<uint16_t> keep_claims_of(const std::set<uint16_t>& present);
	// Every tent known (for the check that it still exists).
	std::vector<uint16_t> known() const;
	size_t tents() const { return m_tents.size(); }
	size_t owners() const { return m_owner.size(); }
	size_t claims() const { return m_claims.size(); }

private:
	struct Tent
	{
		uint64_t first_seen = 0;
		bool old = false; // stood when the world was served
	};
	struct Claim
	{
		uint16_t character = 0xffff;
		float pos[3] = {0, 0, 0};
		uint64_t at = 0;
		uint64_t deadline = 0;
	};
	std::map<uint16_t, Tent> m_tents;
	std::map<uint16_t, uint16_t> m_owner; // tent -> character
	std::vector<Claim> m_claims; // in the order they came
	bool m_first = true; // the next observe() is the first of this world
};

// ---------------------------------------------------------------- implementation

namespace detail
{
inline bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Squared distance in double: no overflow for coordinates within kMaxCoord; NaN stays NaN (every comparison fails).
inline double dist2(const float a[3], const float b[3])
{
	const double dx = double(a[0]) - double(b[0]);
	const double dy = double(a[1]) - double(b[1]);
	const double dz = double(a[2]) - double(b[2]);
	return dx * dx + dy * dy + dz * dz;
}

inline bool pos_ok(const float p[3])
{
	for (int i = 0; i < 3; ++i)
		if (!std::isfinite(p[i]) || std::fabs(p[i]) > kMaxCoord)
			return false;
	return true;
}
} // namespace detail

inline std::vector<std::string> split_names(const std::string& list, size_t max_names)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (out.size() < max_names)
	{
		const size_t comma = list.find(',', pos);
		size_t b = pos;
		size_t e = comma == std::string::npos ? list.size() : comma;
		while (b < e && detail::blank(list[b]))
			++b;
		while (e > b && detail::blank(list[e - 1]))
			--e;
		const std::string name = list.substr(b, e - b);
		// a section name has the alphabet of a level name: 1..64 characters of [A-Za-z0-9_]
		if (level_ok(name) && std::find(out.begin(), out.end(), name) == out.end())
			out.push_back(name);
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
	return out;
}

inline void write_claim(Writer& w, const float pos[3])
{
	w.u8(kClaimTent);
	w.vec3(pos);
}

inline bool read_claim(Reader& rd, float out[3])
{
	const uint8_t what = rd.u8();
	float p[3] = {0, 0, 0};
	rd.vec3(p);
	if (!rd.ok() || rd.remaining() != 0 || what != kClaimTent || !detail::pos_ok(p))
		return false;
	for (int i = 0; i < 3; ++i)
		out[i] = p[i];
	return true;
}

inline const SourceSpot* nearest_spot(const std::vector<SourceSpot>& spots, const float at[3])
{
	const SourceSpot* best = nullptr;
	double best_d2 = 0.0;
	for (const SourceSpot& s : spots)
	{
		if (s.id == kNoSource || !radius_ok(s.radius))
			continue;
		const double d2 = detail::dist2(s.pos, at);
		if (!(d2 <= double(s.radius) * double(s.radius))) // NaN fails too
			continue;
		if (!best || d2 < best_d2 || (d2 == best_d2 && s.id < best->id))
		{
			best = &s;
			best_d2 = d2;
		}
	}
	return best;
}

inline bool ModSources::add(uint16_t id, float radius)
{
	if (id == kNoSource || !radius_ok(radius))
		return false;
	const auto it = m_radius.find(id);
	if (it != m_radius.end())
	{
		it->second = radius;
		return true;
	}
	if (m_radius.size() >= kMaxModSources)
		return false;
	m_radius.emplace(id, radius);
	return true;
}

inline bool ModSources::remove(uint16_t id) { return m_radius.erase(id) != 0; }

inline bool ModSources::has(uint16_t id) const { return m_radius.count(id) != 0; }

inline void TentBook::new_world()
{
	m_tents.clear();
	m_owner.clear();
	m_claims.clear();
	m_first = true;
}

inline bool TentBook::observe(const std::vector<TentSeen>& tents, uint64_t now_ms)
{
	bool all = true;
	for (const TentSeen& t : tents)
	{
		if (t.id == kNoSource || m_tents.count(t.id))
			continue;
		if (m_tents.size() >= kMaxTents)
		{
			all = false;
			continue;
		}
		Tent n;
		n.first_seen = now_ms;
		n.old = m_first;
		m_tents.emplace(t.id, n);
	}
	m_first = false;
	return all;
}

inline void TentBook::forget(uint16_t tent)
{
	m_tents.erase(tent);
	m_owner.erase(tent);
}

inline bool TentBook::set_owner(uint16_t tent, uint16_t character)
{
	if (tent == kNoSource)
		return false;
	const auto it = m_owner.find(tent);
	if (it != m_owner.end())
	{
		it->second = character;
		return true;
	}
	if (m_owner.size() >= kMaxTents)
		return false;
	m_owner.emplace(tent, character);
	return true;
}

inline uint16_t TentBook::owner_of(uint16_t tent) const
{
	const auto it = m_owner.find(tent);
	return it == m_owner.end() ? uint16_t(0xffff) : it->second;
}

inline size_t TentBook::owned_by(uint16_t character) const
{
	size_t n = 0;
	for (const auto& it : m_owner)
		if (it.second == character)
			++n;
	return n;
}

inline bool TentBook::claim(uint16_t character, const float pos[3], uint64_t now_ms, uint32_t ttl_ms)
{
	if (!detail::pos_ok(pos))
		return false;
	bool had = false;
	for (auto it = m_claims.begin(); it != m_claims.end(); ++it)
		if (it->character == character)
		{
			m_claims.erase(it); // the newer claim goes to the end: claims stay in the order they came
			had = true;
			break;
		}
	if (!had && m_claims.size() >= kMaxClaims)
		return false;
	Claim c;
	c.character = character;
	for (int i = 0; i < 3; ++i)
		c.pos[i] = pos[i];
	c.at = now_ms;
	c.deadline = now_ms + ttl_ms;
	m_claims.push_back(c);
	return true;
}

inline bool TentBook::has_claim(uint16_t character) const
{
	for (const Claim& c : m_claims)
		if (c.character == character)
			return true;
	return false;
}

inline std::vector<TentMatch> TentBook::match(const std::vector<TentSeen>& tents, float radius, uint32_t fresh_ms)
{
	std::vector<TentMatch> out;
	if (!radius_ok(radius))
		return out;
	const double r2 = double(radius) * double(radius);
	for (auto c = m_claims.begin(); c != m_claims.end();)
	{
		const TentSeen* best = nullptr;
		double best_d2 = 0.0;
		for (const TentSeen& t : tents)
		{
			const auto it = m_tents.find(t.id);
			if (it == m_tents.end() || it->second.old || m_owner.count(t.id))
				continue; // not observed, stood when the world was served, or somebody's
			if (it->second.first_seen + fresh_ms < c->at)
				continue; // put up too long before the claim
			const double d2 = detail::dist2(t.pos, c->pos);
			if (!(d2 <= r2))
				continue;
			if (!best || d2 < best_d2 || (d2 == best_d2 && t.id < best->id))
			{
				best = &t;
				best_d2 = d2;
			}
		}
		if (best && set_owner(best->id, c->character))
		{
			TentMatch m;
			m.character = c->character;
			m.tent = best->id;
			out.push_back(m);
			c = m_claims.erase(c);
		}
		else
			++c;
	}
	return out;
}

inline std::vector<uint16_t> TentBook::expire(uint64_t now_ms)
{
	std::vector<uint16_t> out;
	for (auto c = m_claims.begin(); c != m_claims.end();)
		if (now_ms >= c->deadline)
		{
			out.push_back(c->character);
			c = m_claims.erase(c);
		}
		else
			++c;
	return out;
}

inline std::vector<uint16_t> TentBook::keep_claims_of(const std::set<uint16_t>& present)
{
	std::vector<uint16_t> out;
	for (auto c = m_claims.begin(); c != m_claims.end();)
		if (!present.count(c->character))
		{
			out.push_back(c->character);
			c = m_claims.erase(c);
		}
		else
			++c;
	return out;
}

inline std::vector<uint16_t> TentBook::known() const
{
	std::set<uint16_t> ids;
	for (const auto& it : m_tents)
		ids.insert(it.first);
	for (const auto& it : m_owner)
		ids.insert(it.first);
	return std::vector<uint16_t>(ids.begin(), ids.end());
}
} // namespace checkpoint
} // namespace coop
