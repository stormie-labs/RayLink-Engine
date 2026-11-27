// Anomaly Coop — the health ledger of this machine's own actor (a diagnostic: it only logs). A player's health can fall
// steadily with no hit from the host and nothing in the log saying who lowers it: in a modded game many scripts write the
// actor's health themselves (game_object:set_health_ex / change_health / health = v), and the
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

// A client lost 0.017-0.018 every 15 s for minutes and no line came (each window under kReportDrop). A window
// with a drop of kSlowFloor or more but under kReportDrop is part of a slow drain; kSlowRun such windows in a row write one
// "slow drain:" line with the breakdown summed over the run, then again after twice as many windows, four times as many
// ... while it goes on (windows 4, 12, 28, 60, ...: hunger alone is a drain for hours). A rise, a window under kSlowFloor
// or one of kReportDrop or more ends the run (and the backoff starts over). The slow drain lines have their own budget,
// kMaxSlowLines, so that they never silence the big drops the ledger exists for.
constexpr float kSlowFloor = 0.005f;
constexpr unsigned kSlowRun = 4; // one minute
constexpr unsigned kMaxSlowLines = 100; // per process, apart from kMaxLines

// What the condition update applied, as the engine queued it (CEntityCondition::UpdateHealth / UpdateRadiation,
// CActorCondition::UpdateSatiety); worn items (CActor::UpdateArtefactsOnBeltAndOutfit) go by their section.
enum class Part : uint8_t
{
	Radiation,
	Bleeding,
	Satiety,
	Restore, // m_fV_HealthRestore and the health_restore booster
	Count
};

class Ledger
{
public:
	// A script's write of the health (`health = v`, set_health_ex): what it changed.
	void script(const std::string& caller, float delta) { add_named(w_.callers, w_.others, caller, delta, 1u, &w_.scripts); }
	// A script's change_health: what it queued. The next condition update that runs applies it.
	void script_queued(const std::string& caller, float amount)
	{
		add_named(w_.callers, w_.others, caller, amount, 1u, &w_.scripts);
		queued_ += amount;
	}
	// A part of the next condition update's change, as the engine queued it (see Part).
	void condition_part(Part part, float amount)
	{
		if (part < Part::Count)
			pending_.parts[size_t(part)] += amount;
	}
	// A worn item's health restore (an artefact on the belt, the outfit, the helmet), queued with ChangeHealth.
	void worn(const std::string& section, float amount) { add_named(pending_.worn, pending_.worn_others, section, amount, 1u, nullptr); }
	// CActor::Hit took a hit of this power (as received); hit_end when it is done. Hits nest (a script's hit inside one).
	void hit_begin(float power)
	{
		++w_.hits;
		w_.hit_power += power;
		++depth_;
	}
	void hit_end()
	{
		if (depth_)
			--depth_;
	}
	bool in_hit() const { return depth_ != 0; }
	// A co-op write of the health: what it changed.
	void coop(float delta) { w_.coop += delta; }
	// One condition update: what it changed. `ran`: it got past its health <= 0 return, so it applied and cleared the queue
	// (what change_health queued is the scripts', not the condition's) and the parts queued for it. Inside a hit it is the
	// hit's damage, its parts with it. `at_full`: the health is at its maximum after the update (SetHealth clamped what it
	// would have added); an update that ran with named parts and changed nothing at all did not apply (the invulnerable
	// time after the last-chance health): the line says so at "unnamed".
	void condition(float delta, bool ran, bool at_full = false)
	{
		if (ran)
		{
			if (!depth_ && (at_full || delta == 0.f) && named_sum(pending_) != 0.f)
				++(at_full ? w_.clamped : w_.not_applied);
			delta -= queued_;
			queued_ = 0.f;
			if (!depth_)
				w_.add_parts(pending_);
			pending_ = Parts();
		}
		(depth_ ? w_.hit_health : w_.condition) += delta;
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
			{
				queued_ = 0.f; // what the old actor queued went with it
				pending_ = Parts();
			}
			actor_ = actor;
			end_run();
			begin(now, health);
			return false;
		}
		if (now < start_)
		{
			end_run();
			begin(now, health);
			return false;
		}
		if (now - start_ < kWindowMs)
			return false;
		const float drop = start_health_ - health;
		bool report = false;
		if (drop >= kReportDrop)
		{
			end_run();
			if (lines_ < kMaxLines)
			{
				report = true;
				line = format("", w_, start_health_, health, now - start_, 0u, vitals());
			}
		}
		else if (drop >= kSlowFloor)
		{
			if (!run_windows_)
			{
				run_ = Totals();
				run_start_ = start_;
				run_start_health_ = start_health_;
			}
			run_.add(w_);
			++run_windows_;
			if (run_windows_ == run_next_)
			{
				run_gap_ *= 2; // the backoff: 4, then 8 more, 16 more ...
				run_next_ += run_gap_;
				if (slow_lines_ < kMaxSlowLines)
				{
					line = format("slow drain: ", run_, run_start_health_, health, now - run_start_, run_windows_, vitals());
					if (++slow_lines_ == kMaxSlowLines)
						line += "; the last slow drain line of this process";
					begin(now, health);
					return true;
				}
			}
		}
		else
			end_run(); // a rise or a still window
		if (report && ++lines_ == kMaxLines)
			line += "; the last health line of this process";
		begin(now, health);
		return report;
	}
	// No world or no actor: the window ends without a line.
	void stop()
	{
		open_ = false;
		actor_ = 0;
		queued_ = 0.f;
		pending_ = Parts();
		end_run();
	}
	unsigned lines() const { return lines_; } // the ordinary lines (budget kMaxLines)
	unsigned slow_lines() const { return slow_lines_; } // the slow drain lines (budget kMaxSlowLines)

private:
	struct Caller
	{
		std::string key;
		float sum = 0.f;
		unsigned count = 0;
	};

	// A named amount (a script caller, a worn item's section) kept apart up to kMaxCallers; any further one is "others".
	static void add_named(std::vector<Caller>& list, Caller& others, const std::string& key, float delta, unsigned count,
	                      float* total)
	{
		if (total)
			*total += delta;
		for (Caller& c : list)
			if (c.key == key)
			{
				c.sum += delta;
				c.count += count;
				return;
			}
		if (list.size() < kMaxCallers)
		{
			list.push_back({key, delta, count});
			return;
		}
		others.sum += delta;
		others.count += count;
	}

	static void add_list(std::vector<Caller>& list, Caller& others, const std::vector<Caller>& from, const Caller& from_others)
	{
		for (const Caller& c : from)
			add_named(list, others, c.key, c.sum, c.count, nullptr);
		others.sum += from_others.sum;
		others.count += from_others.count;
	}

	// What a condition update queued: the parts and the worn items.
	struct Parts
	{
		float parts[size_t(Part::Count)] = {};
		std::vector<Caller> worn;
		Caller worn_others;
	};

	// The buckets of a window, or of a slow drain's run of windows.
	struct Totals
	{
		std::vector<Caller> callers;
		Caller others;
		float scripts = 0.f;
		unsigned hits = 0;
		float hit_power = 0.f, hit_health = 0.f, coop = 0.f, condition = 0.f;
		Parts named; // the condition's parts
		unsigned clamped = 0, not_applied = 0; // updates that ended at full health / applied nothing (see condition)

		void add_parts(const Parts& p)
		{
			for (size_t i = 0; i < size_t(Part::Count); ++i)
				named.parts[i] += p.parts[i];
			add_list(named.worn, named.worn_others, p.worn, p.worn_others);
		}
		void add(const Totals& t)
		{
			add_list(callers, others, t.callers, t.others);
			scripts += t.scripts;
			hits += t.hits;
			hit_power += t.hit_power;
			hit_health += t.hit_health;
			coop += t.coop;
			condition += t.condition;
			add_parts(t.named);
			clamped += t.clamped;
			not_applied += t.not_applied;
		}
	};

	// The sum of the named parts of a condition update (the parts and the worn items).
	static float named_sum(const Parts& p)
	{
		float named = p.worn_others.sum;
		for (const Caller& c : p.worn)
			named += c.sum;
		for (size_t i = 0; i < size_t(Part::Count); ++i)
			named += p.parts[i];
		return named;
	}

	// The kTopCallers largest by size, the rest summed as "others"; `counts`: with " xN".
	static std::string top(const std::vector<Caller>& list, const Caller& others, bool counts)
	{
		std::vector<Caller> by_size = list;
		std::sort(by_size.begin(), by_size.end(), [](const Caller& a, const Caller& b) {
			const float sa = a.sum < 0.f ? -a.sum : a.sum, sb = b.sum < 0.f ? -b.sum : b.sum; // no fabs: xrCore deprecates it
			if (sa != sb)
				return sa > sb;
			if (a.count != b.count)
				return a.count > b.count;
			return a.key < b.key;
		});
		float rest = others.sum;
		unsigned rest_count = others.count;
		std::string s;
		for (size_t i = 0; i < by_size.size(); ++i)
		{
			const Caller& c = by_size[i];
			if (i < kTopCallers)
				s += (i ? ", " : "") + c.key + " " + num(c.sum) + (counts ? " x" + std::to_string(c.count) : std::string());
			else
			{
				rest += c.sum;
				rest_count += c.count;
			}
		}
		if (rest_count)
			s += ", others " + num(rest) + (counts ? " x" + std::to_string(rest_count) : std::string());
		return s;
	}

	void begin(uint64_t now, float health)
	{
		open_ = true;
		start_ = now;
		start_health_ = health;
		w_ = Totals();
	}

	void end_run()
	{
		run_windows_ = 0;
		run_ = Totals();
		run_next_ = kSlowRun;
		run_gap_ = kSlowRun;
	}

	// windows: 0 for one window, else the slow drain's count
	static std::string format(const char* prefix, const Totals& t, float from, float health, uint64_t ms, unsigned windows,
	                          const Vitals& v)
	{
		std::string s = std::string(prefix) + "health " + num(from) + " -> " + num(health) + " in " +
		    std::to_string(unsigned((ms + 500) / 1000)) + " s" +
		    (windows ? " (" + std::to_string(windows) + " windows)" : std::string()) + ": scripts " + num(t.scripts);
		if (!t.callers.empty())
			s += " (" + top(t.callers, t.others, true) + ")";
		s += ", hits " + std::to_string(t.hits) + " applied (power " + num(t.hit_power) + ", health " + num(t.hit_health) + ")";
		s += ", coop applies " + num(t.coop);
		const Parts& p = t.named;
		const std::string unnamed = num(t.condition - named_sum(p));
		const bool unexplained = unnamed != "0.000";
		s += ", condition " + num(t.condition) + " (worn: " + (p.worn.empty() && !p.worn_others.count ? "none" : top(p.worn, p.worn_others, false)) +
		    "; radiation " + num(p.parts[size_t(Part::Radiation)]) + "; bleeding " + num(p.parts[size_t(Part::Bleeding)]) +
		    "; satiety " + num(p.parts[size_t(Part::Satiety)]) + "; restore " + num(p.parts[size_t(Part::Restore)]) +
		    "; unnamed " + unnamed + (unexplained && t.clamped ? " (clamped at full health)" : "") +
		    (unexplained && t.not_applied ? " (an update did not apply)" : "") + ")";
		s += " (bleeding " + num(v.bleeding, 4) + ", radiation " + num(v.radiation) + ", satiety " + num(v.satiety) +
		    ", psy " + num(v.psy) + ", boosters: ";
		if (v.boosters.empty())
			s += "none";
		for (size_t i = 0; i < v.boosters.size(); ++i)
			s += (i ? ", " : "") + v.boosters[i].name + " " + num(v.boosters[i].value, 4);
		const float other = (health - from) - (t.scripts + t.hit_health + t.coop + t.condition);
		s += "), other " + num(other);
		return s;
	}

	bool open_ = false;
	uintptr_t actor_ = 0;
	uint64_t start_ = 0;
	float start_health_ = 0.f;
	Totals w_; // this window
	Parts pending_; // queued for the next condition update that runs (outlives a window, as queued_)
	Totals run_; // a slow drain: its windows so far
	unsigned run_windows_ = 0;
	unsigned run_next_ = kSlowRun; // the run's window that writes its next line (the backoff)
	unsigned run_gap_ = kSlowRun; // windows from one line of the run to the next, doubled after each
	uint64_t run_start_ = 0;
	float run_start_health_ = 0.f;
	float queued_ = 0.f; // what scripts' change_health queued and no condition update applied yet (outlives a window)
	unsigned depth_ = 0; // CActor::Hit calls in progress
	unsigned lines_ = 0;
	unsigned slow_lines_ = 0;
};
} // namespace health_ledger
} // namespace coop
