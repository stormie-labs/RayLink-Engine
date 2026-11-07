// Anomaly Coop — a client's character arriving on a new level: where it stands, how healthy it is, and the journal of the
// transition window (live report 2026-11-07: a G.A.M.M.A. client walked through a level changer with the group, lost
// health and broke a leg).
//
// Where. The host puts each moving client's character beside the level changer's point (coop_join.cpp host_place_movers,
// 1.5 m around it) and a late one beside the host (bring_proxy_to_level) at the height of that point: the host still stands
// on the old level and has no ground of the new one. Single player puts the actor on the designer's point itself. On a
// slope, at a wall or at a cliff the point beside it is in the air (a fall: GAMMA's Body Health System gives a fall to a
// leg) or under the ground (the actor's feet in the hill: he drops through the level). The host marks such a character
// (`placed = 1` in the [coop] block of its custom data, mark_placed); the client, once its new level graph is loaded
// (alife_storage_manager.cpp, after the objects are registered), puts its actor on the ground with place(): its own point
// when the AI map has ground under it within [-kMaxDrop, +kMaxRise], else the first of the host's ring around the level
// changer's vertex that has, else that vertex. The host clears the mark when the player is in the world (WorldLoaded).
//
// Health. The role swap gave the client's actor the health of its NPC proxy on the host (an unharmable stalker, 1.0) and
// only after the client went live coop_equip.cpp client_restore_vitals set the player's own health: a drop of up to 0.95
// in one frame, which every script that watches the health (GAMMA's Body Health System: a hit on a random body part, 5
// points break a leg) takes for damage. The actor now spawns with the health of the player's last report (spawn_health).
//
// Journal. One line per player per transition (rate-limited by the caller): the vitals before leaving and kWindowMs after
// arriving, and where the health went in between: hits (what, how much, by whom, which bone; dropped ones counted), co-op
// applies (the swap, the restore, a revive) and the condition's own update (the rest: bleeding, radiation, hunger, regen).
// Pure C++17, header-only, no engine dependencies: used by xrGame and by the unit-test exe.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "coop_checkpoint_rules.h" // checkpoint::ini_get / ini_set / ini_erase

namespace coop
{
namespace arrival
{
constexpr float kLift = 0.2f; // above the AI map's plane: a drop of 0.2 m (2 m/s), far below GAMMA's crash speed of 15 m/s
constexpr float kMaxRise = 1.5f; // ground this much above the point still counts (the point is inside the slope)
constexpr float kMaxDrop = 3.f; // ... and this much below it (the point is above the slope); further: another floor or a cliff
constexpr float kRing = 1.5f; // the host's ring around the level changer's point (coop_join.cpp host_place_movers)
constexpr float kMinSpawnHealth = 0.05f; // as client_restore_vitals: a player is never brought into a world dying
constexpr uint64_t kWindowMs = 10000; // the journal's window after arriving
constexpr int kVitalsCount = 6; // health, satiety, radiation, psy, power, bleeding (coop_equip.cpp Vitals)
constexpr size_t kMaxNotes = 8;

struct Point
{
	float x = 0.f, y = 0.f, z = 0.f;
};

// The ground under (x, z) at or below probe_y: the AI vertex there (CLevelGraph::vertex_id) and its plane's height. false:
// the AI map has nothing there.
using Ground = std::function<bool(float x, float probe_y, float z, float& ground_y, uint32_t& vertex)>;

enum class How
{
	Snapped, // his own point, on the ground under it
	Moved, // another place around the level changer's point, on its ground
	NoGround, // no ground anywhere near: the point stays as the host wrote it
};

struct Placed
{
	Point at;
	How how = How::NoGround;
	float ground_y = 0.f;
	float drop = 0.f; // his own point's height over the ground under it (+ a fall, - under the ground), when it had ground
	bool had_ground = false;
	uint32_t vertex = 0xffffffffu;
};

inline const char* how_name(How h)
{
	switch (h)
	{
	case How::Snapped: return "snapped";
	case How::Moved: return "moved";
	case How::NoGround: return "no ground";
	}
	return "?";
}

// One candidate: the ground under it within the band, and the candidate put on it.
inline bool on_ground(const Point& c, const Ground& ground, Point& out, float& ground_y, uint32_t& vertex)
{
	float gy = 0.f;
	uint32_t v = 0xffffffffu;
	if (!ground || !ground(c.x, c.y + kMaxRise, c.z, gy, v) || !std::isfinite(gy))
		return false;
	if (gy > c.y + kMaxRise || gy < c.y - kMaxDrop)
		return false;
	out = c;
	out.y = gy + kLift;
	ground_y = gy;
	vertex = v;
	return true;
}

// own: where the host put him; anchor: the level changer's point (the vertex the host gave him). See the header.
inline Placed place(const Point& own, const Point& anchor, const Ground& ground)
{
	Placed p;
	p.at = own;
	float gy = 0.f;
	uint32_t v = 0xffffffffu;
	Point put;
	{
		// how far his own point was from the ground, for the log (the column's ground, whatever the band)
		float any = 0.f;
		uint32_t vv = 0;
		if (ground && ground(own.x, own.y + kMaxRise, own.z, any, vv) && std::isfinite(any))
		{
			p.had_ground = true;
			p.drop = own.y - any;
		}
	}
	if (on_ground(own, ground, put, gy, v))
	{
		p.at = put;
		p.how = How::Snapped;
		p.ground_y = gy;
		p.vertex = v;
		return p;
	}
	static const float kAround[][2] = {
	    {kRing, 0.f}, {0.f, kRing}, {-kRing, 0.f}, {0.f, -kRing}, {kRing, kRing}, {-kRing, -kRing}, {kRing, -kRing}, {0.f, 0.f}};
	for (const auto& d : kAround)
	{
		Point c = anchor;
		c.x += d[0];
		c.z += d[1];
		if (on_ground(c, ground, put, gy, v))
		{
			p.at = put;
			p.how = How::Moved;
			p.ground_y = gy;
			p.vertex = v;
			return p;
		}
	}
	return p;
}

// --- health

// The "vitals = h,s,r,p,w,b" line the host keeps in a proxy's custom data (coop_equip.cpp store_vitals).
inline bool parse_vitals(const char* ini, float (&v)[kVitalsCount])
{
	const char* at = ini ? std::strstr(ini, "vitals =") : nullptr;
	if (!at)
		return false;
	float t[kVitalsCount] = {};
	if (std::sscanf(at, "vitals = %f,%f,%f,%f,%f,%f", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5]) != kVitalsCount)
		return false;
	for (int i = 0; i < kVitalsCount; ++i)
		v[i] = t[i];
	return true;
}

// The health the client's actor spawns with in the role swap: his own last report, else (a new character) the proxy's.
inline float spawn_health(const char* proxy_ini, float proxy_health)
{
	float v[kVitalsCount] = {};
	if (!parse_vitals(proxy_ini, v) || !std::isfinite(v[0]))
		return proxy_health;
	return std::fmin(std::fmax(v[0], kMinSpawnHealth), 1.f);
}

// --- wounds (live 2026-11-07, round 2)

constexpr float kWoundMax = 10.f; // CWound::AddHit clamps a wound's size there (Wound.cpp WOUND_MAX)

// coop_equip.cpp client_restore_vitals: the wounds of the actor's client data are the host's (the save is his) and always go;
// the player's own bleeding (his last report: CEntityCondition::BleedingSpeed = the wounds' sizes * bleed_speed_k) comes back
// as one wound of the size returned here, 0 = none. bleed_speed_k is measured on the actor (a unit wound), not assumed.
inline float restore_wounds(float own_bleeding, float bleed_speed_k)
{
	if (!std::isfinite(own_bleeding) || own_bleeding <= 0.f || !std::isfinite(bleed_speed_k) || bleed_speed_k <= 0.f)
		return 0.f;
	return std::fmin(own_bleeding / bleed_speed_k, kWoundMax);
}

// --- the host's mark: "this character was put on the new level blind"

inline bool placed_by_host(const std::string& ini)
{
	std::string v;
	return checkpoint::ini_get(ini, "coop", "placed", v) && v == "1";
}
inline void mark_placed(std::string& ini) { checkpoint::ini_set(ini, "coop", "placed", "1"); }
inline void clear_placed(std::string& ini) { checkpoint::ini_erase(ini, "coop", "placed"); }

// --- the journal of one transition

struct Vitals4
{
	float health = -1.f, bleeding = 0.f, radiation = 0.f, satiety = 0.f;
};

class Journal
{
public:
	void begin(const char* kind, const char* to, const Vitals4& before, uint64_t now)
	{
		*this = Journal();
		active_ = true;
		kind_ = kind ? kind : "?";
		to_ = to ? to : "?";
		before_ = before;
		started_ = now;
	}
	bool active() const { return active_; }
	bool arrived_yet() const { return arrived_ != 0; }
	void set_to(const char* to)
	{
		if (to && *to)
			to_ = to;
	}

	// A health the co-op code set (the swap, the restore, a revive): its jump is not damage.
	void apply(const char* what, float from, float to)
	{
		if (!active_)
			return;
		pending_apply_ += to - from;
		char b[96];
		std::snprintf(b, sizeof(b), "%s %.3f->%.3f", what ? what : "?", from, to);
		note(b);
	}
	// A hit on the actor (blocked: dropped by co-op, e.g. behind the held loading screen).
	void hit(const char* type, float power, const char* who, int bone, bool blocked)
	{
		if (!active_)
			return;
		if (blocked)
		{
			++dropped_;
			return;
		}
		++hits_;
		pending_hit_ = true;
		char b[128];
		std::snprintf(b, sizeof(b), "%s %.3f by %s bone %d", type ? type : "?", power, who ? who : "?", bone);
		note(b);
	}
	void place(const char* text) { place_ = text ? text : ""; }

	// Every frame while the actor exists: the health change since the last frame goes to a hit (one came since), to the
	// co-op applies, or to the condition's own update.
	void frame(float health, uint64_t /*now*/)
	{
		if (!active_ || !std::isfinite(health))
			return;
		if (last_ < 0.f)
		{
			last_ = health; // the first frame of the new actor: what it spawned with
			pending_apply_ = 0.f;
			pending_hit_ = false;
			return;
		}
		float d = health - last_ - pending_apply_;
		coop_ += pending_apply_;
		if (pending_hit_)
			hits_delta_ += d;
		else
			own_ += d;
		last_ = health;
		pending_apply_ = 0.f;
		pending_hit_ = false;
	}
	void arrived(uint64_t now)
	{
		if (active_ && !arrived_)
			arrived_ = now ? now : 1;
	}
	bool due(uint64_t now) const { return active_ && arrived_ && now >= arrived_ + kWindowMs; }
	// Gives up a journal whose world never came (a stuck load): the line says so.
	bool stale(uint64_t now, uint64_t limit_ms) const { return active_ && !arrived_ && now >= started_ + limit_ms; }

	std::string finish(const Vitals4& after)
	{
		char b[512];
		std::snprintf(b, sizeof(b),
		    "%s to '%s': before hp %.3f bleed %.3f rad %.3f sat %.3f -> after hp %.3f bleed %.3f rad %.3f sat %.3f; "
		    "hp change: hits %.3f, own %.3f, coop %.3f; %u hit(s), %u dropped",
		    kind_.c_str(), to_.c_str(), before_.health, before_.bleeding, before_.radiation, before_.satiety, after.health,
		    after.bleeding, after.radiation, after.satiety, hits_delta_, own_, coop_, hits_, dropped_);
		std::string line = b;
		if (!place_.empty())
			line += "; place " + place_;
		if (!notes_.empty())
		{
			line += "; events:";
			for (size_t i = 0; i < notes_.size(); ++i)
				line += (i ? ", " : " ") + notes_[i];
			if (notes_dropped_)
				line += ", +" + std::to_string(notes_dropped_) + " more";
		}
		*this = Journal();
		return line;
	}

private:
	void note(const char* text)
	{
		if (notes_.size() < kMaxNotes)
			notes_.push_back(text);
		else
			++notes_dropped_;
	}

	bool active_ = false;
	std::string kind_, to_, place_;
	Vitals4 before_;
	uint64_t started_ = 0, arrived_ = 0;
	float last_ = -1.f, pending_apply_ = 0.f, hits_delta_ = 0.f, own_ = 0.f, coop_ = 0.f;
	bool pending_hit_ = false;
	unsigned hits_ = 0, dropped_ = 0, notes_dropped_ = 0;
	std::vector<std::string> notes_;
};
} // namespace arrival
} // namespace coop
