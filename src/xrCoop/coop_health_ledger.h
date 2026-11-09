// Anomaly Coop — the health ledger of this machine's own actor (a diagnostic: it only logs). Live 2026-11-09 (G.A.M.M.A., 6
// players): after a reconnect a client's health fell at a steady 0.018/s for 2.5 min, through two revives, until he died;
// the host sent him no hit, bleeding, radiation, psy and thirst were normal, and no log said who lowered it. In G.A.M.M.A.
// some 19 scripts write the actor's health themselves (game_object:set_health_ex / change_health / health = v), and the
// engine's own condition update changes it too.
//
// Each machine (host, client, any role with a real actor) keeps a ledger of its own actor over kWindowMs windows. A window
// in which the health fell by kReportDrop or more gets one line, at most kMaxLines per process:
//   health 0.709 -> 0.439 in 15 s: scripts -0.270 (perk_based_artefacts.script:1087 -0.250 x14,
//   grok_actor_damage_balancer.script:751 -0.020 x1), hits 0 applied (power 0.000, health 0.000), coop applies 0.000,
//   condition 0.000 (bleeding 0.0000, radiation 0.000, satiety 0.611, psy 1.000, boosters: none), other 0.000
// Where a change goes:
//   scripts       a Lua call on the own actor, by its caller file:line (the kTopCallers largest by size, the rest summed as
//                 "others"): `health = v` and set_health_ex(v) by what they changed; change_health(v) by what it queued,
//                 which the next condition update applies (condition() takes that part out of the update's change)
//   hits          the hits CActor::Hit took (after co-op's own drop), their power as received (before scripts), and the
//                 health the condition updates inside them applied (CEntityAlive::Hit -> CalcCondition -> UpdateCondition)
//   coop applies  the co-op's own writes: downed (kDownHealth), revive and respawn, the restore after a join
//   condition     CEntityCondition::UpdateCondition of the actor outside a hit: bleeding, radiation, satiety, boosters,
//                 regeneration, and what other C++ code queued with ChangeHealth (a medkit, ...)
//   other         what none of them explains: the window's change minus all of the above
// The vitals in the condition part are the ones at the end of the window.
// Pure C++17, header-only, no engine dependencies: xrGame/coop/coop_health_ledger.cpp feeds it, tests/test_health_ledger.cpp
// tests it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop
{
namespace health_ledger
{
constexpr uint64_t kWindowMs = 15000;
constexpr float kReportDrop = 0.02f; // a window with a smaller drop, or a rise, writes nothing
constexpr unsigned kMaxLines = 300; // per process
constexpr size_t kTopCallers = 3; // script callers named in a line
constexpr size_t kMaxCallers = 32; // callers kept apart within a window; any further one goes straight to "others"

// The script of a Lua frame and its line: "...\\gamedata\\scripts\\grok_actor_damage_balancer.script", 751 ->
// "grok_actor_damage_balancer.script:751"; "?" without a source.
inline std::string caller_key(const char* src, int line)
{
	if (!src)
		return "?";
	const char* name = src;
	for (const char* p = src; *p; ++p)
		if (*p == '\\' || *p == '/')
			name = p + 1;
	if (!*name)
		return "?";
	return std::string(name) + ":" + std::to_string(line);
}

struct Booster
{
	std::string name; // "health_restore" (the section name without "boost_")
	float value = 0.f;
};

struct Vitals
{
	float bleeding = 0.f, radiation = 0.f, satiety = 0.f, psy = 0.f;
	std::vector<Booster> boosters;
};

// "%.<decimals>f", never "-0.000".
inline std::string num(float v, int decimals = 3)
{
	char b[48];
	std::snprintf(b, sizeof(b), "%.*f", decimals, double(v));
	std::string s = b;
	if (!s.empty() && s[0] == '-' && s.find_first_not_of("-0.") == std::string::npos)
		s.erase(0, 1);
	return s;
}

class Ledger
{
public:
	// A script's write of the health (`health = v`, set_health_ex): what it changed.
	void script(const std::string& caller, float delta) { add_caller(caller, delta); }
	// A script's change_health: what it queued. The next condition update that runs applies it.
	void script_queued(const std::string& caller, float amount)
	{
		add_caller(caller, amount);
		queued_ += amount;
	}
	// CActor::Hit took a hit of this power (as received); hit_end when it is done. Hits nest (a script's hit inside one).
	void hit_begin(float power)
	{
		++hits_;
		hit_power_ += power;
		++depth_;
	}
	void hit_end()
	{
		if (depth_)
			--depth_;
	}
	bool in_hit() const { return depth_ != 0; }
	// A co-op write of the health: what it changed.
	void coop(float delta) { coop_ += delta; }
	// One condition update: what it changed. `ran`: it got past its health <= 0 return, so it applied and cleared the queue
	// (what change_health queued is the scripts', not the condition's). Inside a hit it is the hit's damage.
	void condition(float delta, bool ran)
	{
		if (ran)
		{
			delta -= queued_;
			queued_ = 0.f;
		}
		(depth_ ? hit_health_ : condition_) += delta;
	}

	// Every frame with the own actor (`actor`: who it is; a new one starts over). True when a window closed with a drop of
	// kReportDrop or more: `line` is set (vitals() is asked only then). The first frame only opens a window.
	template <class VitalsFn>
	bool frame(uint64_t now, uintptr_t actor, float health, std::string& line, VitalsFn&& vitals)
	{
		if (!std::isfinite(health))
		{
			stop();
			return false;
		}
		if (!open_ || actor != actor_)
		{
			if (actor != actor_)
				queued_ = 0.f; // what the old actor queued went with it
			actor_ = actor;
			begin(now, health);
			return false;
		}
		if (now < start_)
		{
			begin(now, health);
			return false;
		}
		if (now - start_ < kWindowMs)
			return false;
		const bool report = start_health_ - health >= kReportDrop && lines_ < kMaxLines;
		if (report)
		{
			++lines_;
			line = format(now, health, vitals());
			if (lines_ == kMaxLines)
				line += "; the last health line of this process";
		}
		begin(now, health);
		return report;
	}
	// No world or no actor: the window ends without a line.
	void stop()
	{
		open_ = false;
		actor_ = 0;
		queued_ = 0.f;
	}
	unsigned lines() const { return lines_; }

private:
	struct Caller
	{
		std::string key;
		float sum = 0.f;
		unsigned count = 0;
	};

	void add_caller(const std::string& key, float delta)
	{
		scripts_ += delta;
		for (Caller& c : callers_)
			if (c.key == key)
			{
				c.sum += delta;
				++c.count;
				return;
			}
		if (callers_.size() < kMaxCallers)
		{
			callers_.push_back({key, delta, 1u});
			return;
		}
		others_sum_ += delta;
		++others_count_;
	}

	void begin(uint64_t now, float health)
	{
		open_ = true;
		start_ = now;
		start_health_ = health;
		callers_.clear();
		scripts_ = others_sum_ = 0.f;
		others_count_ = 0;
		hits_ = 0;
		hit_power_ = hit_health_ = coop_ = condition_ = 0.f;
	}

	std::string format(uint64_t now, float health, const Vitals& v) const
	{
		std::string s = "health " + num(start_health_) + " -> " + num(health) + " in " +
		    std::to_string(unsigned((now - start_ + 500) / 1000)) + " s: scripts " + num(scripts_);
		if (!callers_.empty())
		{
			std::vector<Caller> by_size = callers_;
			std::sort(by_size.begin(), by_size.end(), [](const Caller& a, const Caller& b) {
				const float sa = a.sum < 0.f ? -a.sum : a.sum, sb = b.sum < 0.f ? -b.sum : b.sum; // no fabs: xrCore deprecates it
				if (sa != sb)
					return sa > sb;
				if (a.count != b.count)
					return a.count > b.count;
				return a.key < b.key;
			});
			float rest = others_sum_;
			unsigned rest_count = others_count_;
			s += " (";
			for (size_t i = 0; i < by_size.size(); ++i)
			{
				const Caller& c = by_size[i];
				if (i < kTopCallers)
					s += (i ? ", " : "") + c.key + " " + num(c.sum) + " x" + std::to_string(c.count);
				else
				{
					rest += c.sum;
					rest_count += c.count;
				}
			}
			if (rest_count)
				s += ", others " + num(rest) + " x" + std::to_string(rest_count);
			s += ")";
		}
		s += ", hits " + std::to_string(hits_) + " applied (power " + num(hit_power_) + ", health " + num(hit_health_) + ")";
		s += ", coop applies " + num(coop_);
		s += ", condition " + num(condition_) + " (bleeding " + num(v.bleeding, 4) + ", radiation " + num(v.radiation) +
		    ", satiety " + num(v.satiety) + ", psy " + num(v.psy) + ", boosters: ";
		if (v.boosters.empty())
			s += "none";
		for (size_t i = 0; i < v.boosters.size(); ++i)
			s += (i ? ", " : "") + v.boosters[i].name + " " + num(v.boosters[i].value, 4);
		const float other = (health - start_health_) - (scripts_ + hit_health_ + coop_ + condition_);
		s += "), other " + num(other);
		return s;
	}

	bool open_ = false;
	uintptr_t actor_ = 0;
	uint64_t start_ = 0;
	float start_health_ = 0.f;
	std::vector<Caller> callers_;
	float scripts_ = 0.f, others_sum_ = 0.f;
	unsigned others_count_ = 0;
	unsigned hits_ = 0;
	float hit_power_ = 0.f, hit_health_ = 0.f, coop_ = 0.f, condition_ = 0.f;
	float queued_ = 0.f; // what scripts' change_health queued and no condition update applied yet (outlives a window)
	unsigned depth_ = 0; // CActor::Hit calls in progress
	unsigned lines_ = 0;
};
} // namespace health_ledger
} // namespace coop
