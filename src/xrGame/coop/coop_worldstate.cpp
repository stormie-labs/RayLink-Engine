// Anomaly Coop — world state: host-authoritative game clock and weather. See coop_worldstate.h.
#include "stdafx.h"
#include "coop_worldstate.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_sync.h"
#include "coop_alife.h"

#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "date_time.h"
#include "../xrEngine/IGame_Persistent.h"
#include "../xrEngine/Environment.h"

namespace coop
{
namespace worldstate
{
namespace
{
// --- tuning ---
constexpr u64 kTimePeriodMs = 1000; // host: TimeSync refresh
constexpr u64 kWeatherPeriodMs = 5000; // host: WeatherSync refresh
constexpr float kSnapRealMs = 250.f; // client: follow the host when off by more than this much real time...
constexpr s64 kSnapMinGameMs = 1000; // ...and never for less than one game second (network jitter)
constexpr s64 kJumpGameMs = 1000; // host: clock moved more than its rate explains -> send at once
constexpr s64 kLogJumpGameMs = 60 * 1000; // client: log corrections bigger than one game minute
constexpr float kWfxTolSec = 30.f; // client: weather fx position tolerance, game seconds
constexpr float kExecTolSec = 1.f; // client: env section time tolerance, game seconds
constexpr u64 kRequestDebounceMs = 3000; // sleep requests: one per 3 s per player
constexpr u64 kRefuseLogMs = 10000; // rate limit of "refused" log lines
constexpr s64 kDayMs = 24 * 60 * 60 * 1000;

// TimeSync payload, host -> clients.
//   u64 game_time   ALife game time, ms
//   f32 factor      ALife time factor (game ms per real ms)
//   u64 host_clock  host send clock (Manager::now_ms), orders samples across lanes
//   u8  flags       bit 0: host game paused (its clock stands still)
struct TimeState
{
	u64 game_time = 0;
	float factor = 1.f;
	u64 host_clock = 0;
	bool paused = false;

	void write(Writer& w) const
	{
		w.u64(game_time);
		w.f32(factor);
		w.u64(host_clock);
		w.u8(paused ? 1 : 0);
	}
	bool read(Reader& r)
	{
		game_time = r.u64();
		factor = r.f32();
		host_clock = r.u64();
		paused = (r.u8() & 1) != 0;
		return r.ok() && _valid(factor) && factor >= 0.f;
	}
};

// One env section (CEnvDescriptor) of a weather cycle: cycle name + section id ("HH:MM:SS") + its
// current exec time (Anomaly's level.set_weather_weight moves exec times).
struct EnvRef
{
	std::string cycle;
	std::string id;
	float exec_time = 0.f;
};

// WeatherSync payload, host -> clients.
//   str cycle, str fx, f32 fx_time, 2 x {str cycle, str id, f32 exec_time}, f32 blend, u64 host_clock
struct WeatherState
{
	std::string cycle; // weather cycle (CEnvironment::CurrentCycleName)
	std::string fx; // active weather fx, empty if none
	float fx_time = 0.f; // game seconds left of the fx
	EnvRef env[2]; // current and next env section; empty while an fx plays
	float blend = 0.f; // weight between them (diagnostics: it follows from time + sections)
	u64 host_clock = 0;

	void write(Writer& w) const
	{
		w.str(cycle);
		w.str(fx);
		w.f32(fx_time);
		for (const EnvRef& e : env)
		{
			w.str(e.cycle);
			w.str(e.id);
			w.f32(e.exec_time);
		}
		w.f32(blend);
		w.u64(host_clock);
	}
	bool read(Reader& r)
	{
		cycle = r.str();
		fx = r.str();
		fx_time = r.f32();
		for (EnvRef& e : env)
		{
			e.cycle = r.str();
			e.id = r.str();
			e.exec_time = r.f32();
		}
		blend = r.f32();
		host_clock = r.u64();
		return r.ok() && _valid(fx_time) && _valid(blend) && _valid(env[0].exec_time) && _valid(env[1].exec_time);
	}
};

// Host: cheap per-frame fingerprint of the weather, to send on change.
struct RawWeather
{
	shared_str cycle;
	shared_str fx;
	bool wfx = false;
	const CEnvDescriptor* cur[2] = {nullptr, nullptr};
	float exec[2] = {0.f, 0.f};

	bool same(const RawWeather& o) const
	{
		return cycle == o.cycle && wfx == o.wfx && fx == o.fx && cur[0] == o.cur[0] && cur[1] == o.cur[1] &&
		    fsimilar(exec[0], o.exec[0], kExecTolSec) && fsimilar(exec[1], o.exec[1], kExecTolSec);
	}
};

struct State
{
	// host
	bool time_sent = false;
	bool time_dirty = false;
	u64 next_time_send = 0;
	u64 sent_game_time = 0;
	u32 sent_dw = 0;
	float sent_factor = 0.f;
	bool sent_paused = false;
	bool weather_dirty = false;
	u64 next_weather_send = 0;
	bool raw_valid = false;
	RawWeather raw;
	xr_set<PeerId> joining; // peers seen loading the world: they get the full state when they finish
	xr_map<PeerId, u64> last_request; // sleep request debounce per player
	u32 time_sent_count = 0;
	u32 weather_sent_count = 0;

	// client
	TimeState rx_time;
	u64 rx_time_at = 0; // local clock when rx_time arrived
	bool have_time = false;
	bool time_pending = false;
	bool time_applied = false; // a sample was applied since going live
	WeatherState rx_weather;
	u64 rx_weather_at = 0;
	bool have_weather = false;
	bool weather_pending = false;
	bool was_live = false;
	u64 last_request_sent = 0;
	bool request_pending = false; // recorded by the hook (any thread), sent from frame()
	u32 request_minutes = 0;

	// diagnostics
	s64 last_error_ms = 0; // client: local minus host game time at the last sample
	u32 time_corrections = 0;
	u32 weather_applies = 0;
	u32 refused_time = 0;
	u32 refused_weather = 0;
	u32 requests_seen = 0;
	u64 last_refuse_log = 0;
	bool warned_unknown_weather = false;
};
State g;

const char* str_of(const shared_str& s) { return s.c_str() ? s.c_str() : ""; }

bool refuse_log_allowed()
{
	const u64 now = mgr().now_ms();
	if (g.last_refuse_log && now - g.last_refuse_log < kRefuseLogMs)
		return false;
	g.last_refuse_log = now;
	return true;
}

bool alife_ready() { return g_pGameLevel && ai().get_alife() && ai().alife().initialized(); }

// The weather is usable once a level runs and the first lerp selected both env sections.
CEnvironment* env_ready()
{
	if (!g_pGameLevel || !g_pGamePersistent)
		return nullptr;
	CEnvironment& e = g_pGamePersistent->Environment();
	if (!e.CurrentWeather || !e.Current[0] || !e.Current[1] || !e.CurrentEnv)
		return nullptr;
	return &e;
}

float normalize_day(float t)
{
	while (t < 0.f)
		t += DAY_LENGTH;
	while (t > DAY_LENGTH)
		t -= DAY_LENGTH;
	return t;
}

void format_time(u64 t, char* out, u32 size)
{
	u32 y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
	split_time(t, y, mo, d, h, mi, s, ms);
	xr_sprintf(out, size, "%02u.%02u.%04u %02u:%02u:%02u", d, mo, y, h, mi, s);
}

// Weather cycle that owns env section `d` (Current[] point into cycle vectors), nullptr if none.
const char* owner_cycle(CEnvironment& e, const CEnvDescriptor* d)
{
	for (const auto& c : e.WeatherCycles)
		for (const CEnvDescriptor* x : c.second)
			if (x == d)
				return c.first.c_str();
	return nullptr;
}

CEnvDescriptor* find_env(CEnvironment& e, const EnvRef& r)
{
	if (r.cycle.empty() || r.id.empty())
		return nullptr;
	const auto it = e.WeatherCycles.find(shared_str(r.cycle.c_str()));
	if (it == e.WeatherCycles.end())
		return nullptr;
	for (CEnvDescriptor* x : it->second)
		if (!xr_strcmp(str_of(x->m_identifier), r.id.c_str()))
			return x;
	return nullptr;
}

bool has_cycle(CEnvironment& e, const std::string& name)
{
	return e.WeatherCycles.find(shared_str(name.c_str())) != e.WeatherCycles.end();
}

bool has_fx(CEnvironment& e, const std::string& name)
{
	return e.WeatherFXs.find(shared_str(name.c_str())) != e.WeatherFXs.end();
}

void warn_unknown_weather(const char* what, const char* name)
{
	if (g.warned_unknown_weather)
		return;
	g.warned_unknown_weather = true;
	::Msg("! [coop] weather: host uses %s '%s' that this game does not have (different weather mods?)", what, name);
}

// ---------------------------------------------------------------- host

void host_send_time(PeerId to)
{
	const CALifeTimeManager& tm = ai().alife().time_manager();
	TimeState t;
	t.game_time = tm.game_time();
	t.factor = tm.time_factor();
	t.host_clock = mgr().now_ms();
	t.paused = Device.Paused();
	Writer w(32);
	t.write(w);
	++g.time_sent_count;
	if (to != kInvalidPeer)
	{
		// Initial state for a player who just finished loading: reliable, it must not be lost.
		net::host_send(to, Msg::TimeSync, w, Lane::Reliable);
		return;
	}
	// Refresh and jumps: unreliable (dropped for loading players, the next one is a second away).
	net::host_broadcast(Msg::TimeSync, w, Lane::Unreliable);
	g.time_sent = true;
	g.time_dirty = false;
	g.sent_game_time = t.game_time;
	g.sent_dw = Device.dwTimeGlobal;
	g.sent_factor = t.factor;
	g.sent_paused = t.paused;
	g.next_time_send = t.host_clock + kTimePeriodMs;
}

void host_time_frame(u64 now)
{
	bool send = g.time_dirty || !g.time_sent || now >= g.next_time_send;
	if (!send)
	{
		// Catches every jump without a hook: loads, console commands, scripts.
		const CALifeTimeManager& tm = ai().alife().time_manager();
		const u64 expected = g.sent_game_time + u64(g.sent_factor * float(Device.dwTimeGlobal - g.sent_dw));
		const s64 drift = s64(tm.game_time() - expected);
		send = drift > kJumpGameMs || drift < -kJumpGameMs || !fsimilar(tm.time_factor(), g.sent_factor, 1e-3f) ||
		    Device.Paused() != g.sent_paused;
	}
	if (send)
		host_send_time(kInvalidPeer);
}

RawWeather snapshot(CEnvironment& e)
{
	RawWeather r;
	r.cycle = e.CurrentCycleName;
	r.wfx = e.bWFX;
	if (e.bWFX)
		r.fx = e.CurrentWeatherName;
	for (int i = 0; i < 2; ++i)
	{
		r.cur[i] = e.Current[i];
		r.exec[i] = e.Current[i]->exec_time;
	}
	return r;
}

void capture(CEnvironment& e, WeatherState& w)
{
	w.cycle = str_of(e.CurrentCycleName);
	w.fx = e.bWFX ? str_of(e.CurrentWeatherName) : "";
	w.fx_time = e.bWFX ? e.wfx_time : 0.f;
	for (int i = 0; i < 2; ++i)
	{
		EnvRef& r = w.env[i];
		r = EnvRef();
		if (e.bWFX)
			continue; // fx sections are rebuilt on the client by StartWeatherFXFromTime
		const char* c = owner_cycle(e, e.Current[i]);
		if (!c)
			continue;
		r.cycle = c;
		r.id = str_of(e.Current[i]->m_identifier);
		r.exec_time = e.Current[i]->exec_time;
	}
	w.blend = e.CurrentEnv->weight;
	w.host_clock = mgr().now_ms();
}

void host_send_weather(CEnvironment& e, PeerId to, Lane lane)
{
	WeatherState w;
	capture(e, w);
	Writer wr(160);
	w.write(wr);
	++g.weather_sent_count;
	if (to != kInvalidPeer)
	{
		net::host_send(to, Msg::WeatherSync, wr, lane);
		return;
	}
	net::host_broadcast(Msg::WeatherSync, wr, lane);
	g.weather_dirty = false;
	g.next_weather_send = w.host_clock + kWeatherPeriodMs;
}

void host_weather_frame(u64 now)
{
	CEnvironment* e = env_ready();
	if (!e)
		return;
	const RawWeather r = snapshot(*e);
	if (!g.raw_valid || !r.same(g.raw))
	{
		g.raw = r;
		g.raw_valid = true;
		g.weather_dirty = true;
	}
	if (!g.weather_dirty && now < g.next_weather_send)
		return;
	// A change goes reliable (state, queued for loading players); the refresh goes unreliable.
	host_send_weather(*e, kInvalidPeer, g.weather_dirty ? Lane::Reliable : Lane::Unreliable);
}

// Full state to every player who just finished loading the world (right before JoinComplete,
// which coop_join sends after sync::host_end_join; the periodic refresh covers the rest).
void host_join_frame()
{
	const std::vector<PeerInfo> peers = mgr().session().peers();
	for (auto it = g.joining.begin(); it != g.joining.end();)
	{
		bool present = false;
		for (const PeerInfo& p : peers)
			present = present || (p.id == *it && p.state == PeerState::Joined);
		it = present ? std::next(it) : g.joining.erase(it);
	}
	for (const PeerInfo& p : peers)
	{
		if (p.state != PeerState::Joined)
			continue;
		if (sync::host_peer_joining(p.id))
		{
			g.joining.insert(p.id);
			continue;
		}
		if (!g.joining.erase(p.id))
			continue;
		host_send_time(p.id);
		if (CEnvironment* e = env_ready())
			host_send_weather(*e, p.id, Lane::Reliable);
	}
}

void host_frame(u64 now)
{
	if (!alife_ready())
		return;
	host_join_frame();
	host_time_frame(now);
	host_weather_frame(now);
}

void host_on_time_request(PeerId from, Reader& r)
{
	if (!mgr().is_host())
		return;
	const u32 minutes = r.u32();
	if (!r.ok())
		return;
	const u64 now = mgr().now_ms();
	u64& last = g.last_request[from];
	if (last && now - last < kRequestDebounceMs)
		return;
	last = now;
	++g.requests_seen;
	const PeerInfo* p = mgr().session().peer(from);
	::Msg("[coop] <%s> wants to sleep (%u h %02u min)", p ? p->nick.c_str() : "?", minutes / 60, minutes % 60);
}

// ---------------------------------------------------------------- client

// Host game time now, estimated from the last sample: + its age + half the round trip.
u64 host_time_now(u64 now)
{
	const TimeState& t = g.rx_time;
	if (t.paused)
		return t.game_time;
	const PeerInfo* host = mgr().session().peer(kHostPeer);
	const u64 age = (now - g.rx_time_at) + (host ? host->rtt_ms / 2 : 0);
	return t.game_time + u64(t.factor * float(age));
}

// Game seconds that passed on the host since the last WeatherSync was captured.
float weather_age_game_sec(u64 now)
{
	const PeerInfo* host = mgr().session().peer(kHostPeer);
	const u64 age = (now - g.rx_weather_at) + (host ? host->rtt_ms / 2 : 0);
	const float factor = g.have_time ? (g.rx_time.paused ? 0.f : g.rx_time.factor) : 1.f;
	return factor * float(age) / 1000.f;
}

void client_apply_time(u64 now)
{
	CALifeTimeManager& tm = alife_rw().time_manager();
	const TimeState& t = g.rx_time;
	const u64 target = host_time_now(now);
	const s64 err = s64(tm.game_time() - target);
	g.last_error_ms = err;
	const bool factor_differs = !fsimilar(tm.time_factor(), t.factor, 1e-3f);
	const s64 tol = _max(kSnapMinGameMs, s64(t.factor * kSnapRealMs));
	const bool off = err > tol || err < -tol;
	// Host paused (menu): hold, one correction when it resumes instead of a step back every second.
	if (t.paused && g.time_applied && !factor_differs)
		return;
	g.time_applied = true;
	if (!off && !factor_differs)
		return;
	const s64 delta = -err;
	// Step the sky clock by the same amount first, as level.change_game_time does: otherwise
	// CEnvironment::SetGameTime counts a step back as a whole day and ends a running weather fx.
	g_pGamePersistent->Environment().ChangeGameTime(float(delta % kDayMs) / 1000.f);
	tm.coop_set_game_time(target, t.factor);
	++g.time_corrections;
	if (delta > kLogJumpGameMs || delta < -kLogJumpGameMs || factor_differs)
		::Msg("[coop] time: following the host clock (%+.1f game min, factor %.2f)", float(delta) / 60000.f, t.factor);
}

void client_apply_weather(CEnvironment& e, u64 now)
{
	const WeatherState& w = g.rx_weather;
	bool changed = false;

	if (w.fx.empty())
	{
		if (e.bWFX)
		{
			// Let an fx that is about to end finish on its own (no visible pop); the next sync aligns.
			if (e.wfx_time <= kWfxTolSec)
				return;
			e.StopWFX();
			changed = true;
		}
		if (!w.cycle.empty() && xr_strcmp(str_of(e.CurrentCycleName), w.cycle.c_str()))
		{
			if (has_cycle(e, w.cycle))
			{
				e.SetWeather(shared_str(w.cycle.c_str()), false);
				changed = true;
			}
			else
				warn_unknown_weather("weather cycle", w.cycle.c_str());
		}
		// Same sections and section times + same game time = same blend and same transitions.
		for (int i = 0; i < 2; ++i)
		{
			const EnvRef& r = w.env[i];
			if (r.id.empty())
				continue;
			CEnvDescriptor* d = find_env(e, r);
			if (!d)
			{
				warn_unknown_weather("env section", r.cycle.c_str());
				continue;
			}
			if (e.Current[i] != d)
			{
				e.Current[i] = d;
				changed = true;
			}
			if (_abs(d->exec_time - r.exec_time) > kExecTolSec)
			{
				d->exec_time = r.exec_time;
				changed = true;
			}
		}
	}
	else
	{
		if (e.bWFX && xr_strcmp(str_of(e.CurrentWeatherName), w.fx.c_str()))
		{
			e.StopWFX();
			changed = true;
		}
		// The cycle the fx returns to (while an fx plays SetWeather only records the cycle name).
		if (!w.cycle.empty() && xr_strcmp(str_of(e.CurrentCycleName), w.cycle.c_str()))
		{
			if (has_cycle(e, w.cycle))
			{
				e.SetWeather(shared_str(w.cycle.c_str()), false);
				changed = true;
			}
			else
				warn_unknown_weather("weather cycle", w.cycle.c_str());
		}
		const float left = w.fx_time - weather_age_game_sec(now);
		if (!e.bWFX)
		{
			if (!has_fx(e, w.fx))
				warn_unknown_weather("weather fx", w.fx.c_str());
			else if (left > kWfxTolSec) // an fx about to end is not worth starting
			{
				e.StartWeatherFXFromTime(shared_str(w.fx.c_str()), left);
				changed = true;
			}
		}
		else if (left > 0.f && _abs(e.wfx_time - left) > kWfxTolSec)
		{
			// Same fx at another position: shift its timeline the way StartWeatherFXFromTime does.
			for (CEnvDescriptor* d : *e.CurrentWeather)
				d->exec_time = normalize_day(d->exec_time - e.wfx_time + left);
			e.wfx_time = left;
			changed = true;
		}
	}

	if (changed)
	{
		++g.weather_applies;
		::Msg("[coop] weather: following the host (%s%s%s)", w.cycle.c_str(), w.fx.empty() ? "" : ", fx ",
		    w.fx.c_str());
	}
}

void client_frame(u64 now)
{
	const bool live = mgr().is_live_client();
	if (live && !g.was_live)
	{
		// (Re)entered the world: apply what we hold (P6: kept while loading, applied now).
		g.time_applied = false;
		g.time_pending = g.have_time;
		g.weather_pending = g.have_weather;
	}
	g.was_live = live;
	if (!live)
		return;
	if (g.request_pending)
	{
		g.request_pending = false;
		Writer w(8);
		w.u32(g.request_minutes);
		net::client_send(Msg::TimeRequest, w);
	}
	if (g.time_pending && alife_ready() && g_pGamePersistent)
	{
		client_apply_time(now);
		g.time_pending = false;
	}
	if (g.weather_pending)
	{
		if (CEnvironment* e = env_ready())
		{
			client_apply_weather(*e, now);
			g.weather_pending = false;
		}
	}
}

void client_on_time_sync(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_client())
		return;
	TimeState t;
	if (!t.read(r))
	{
		::Msg("! [coop] time: malformed TimeSync");
		return;
	}
	if (g.have_time && t.host_clock < g.rx_time.host_clock)
		return; // older than the sample we hold (lanes are not ordered between each other)
	g.rx_time = t;
	g.rx_time_at = mgr().now_ms();
	g.have_time = true;
	g.time_pending = true;
}

void client_on_weather_sync(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_client())
		return;
	WeatherState w;
	if (!w.read(r))
	{
		::Msg("! [coop] weather: malformed WeatherSync");
		return;
	}
	if (g.have_weather && w.host_clock < g.rx_weather.host_clock)
		return;
	g.rx_weather = std::move(w);
	g.rx_weather_at = mgr().now_ms();
	g.have_weather = true;
	g.weather_pending = true;
}

// ---------------------------------------------------------------- slow motion

void enforce_device_time_factor()
{
	if (fsimilar(Device.time_factor(), 1.f))
		return;
	::Msg("[coop] time_factor %.3f reset to 1: slow motion is off in a co-op session", Device.time_factor());
	Device.time_factor(1.f);
	if (!Core.ParamsData.test(ECoreParams::sound_constant_speed))
		psSpeedOfSound = 1.f;
}
} // namespace

void install(Session& s)
{
	s.on(Msg::TimeSync, &client_on_time_sync);
	s.on(Msg::WeatherSync, &client_on_weather_sync);
	s.on(Msg::TimeRequest, &host_on_time_request);
}

void reset() { g = State(); }

void frame()
{
	if (!mgr().active())
		return;
	enforce_device_time_factor();
	const u64 now = mgr().now_ms();
	if (mgr().is_host())
		host_frame(now);
	else
		client_frame(now);
}

void print_status()
{
	if (!mgr().active())
		return;
	string64 local_s = "-";
	if (alife_ready())
		format_time(ai().alife().time_manager().game_time(), local_s, sizeof(local_s));
	if (mgr().is_host())
	{
		::Msg("[coop]   time: host %s (this machine), factor %.2f%s, %u syncs sent, %u sleep requests", local_s,
		    alife_ready() ? ai().alife().time_manager().time_factor() : 0.f, Device.Paused() ? ", paused" : "",
		    g.time_sent_count, g.requests_seen);
	}
	else
	{
		string64 host_s = "-";
		if (g.have_time)
			format_time(host_time_now(mgr().now_ms()), host_s, sizeof(host_s));
		::Msg("[coop]   time: host %s / local %s, last error %lld ms, factor %.2f%s, %u corrections, %u local "
		      "changes refused",
		    host_s, local_s, (long long)g.last_error_ms, g.have_time ? g.rx_time.factor : 0.f,
		    g.have_time && g.rx_time.paused ? ", host paused" : "", g.time_corrections, g.refused_time);
	}
	CEnvironment* e = env_ready();
	if (!e)
	{
		::Msg("[coop]   weather: not in a level");
		return;
	}
	const bool host = mgr().is_host();
	string32 host_blend = "";
	if (!host && g.have_weather)
		xr_sprintf(host_blend, " (host %.2f)", g.rx_weather.blend);
	::Msg("[coop]   weather: %s%s%s, env %s -> %s, blend %.2f%s, %u %s, %u script changes refused",
	    str_of(e->CurrentCycleName), e->bWFX ? ", fx " : "", e->bWFX ? str_of(e->CurrentWeatherName) : "",
	    str_of(e->Current[0]->m_identifier), str_of(e->Current[1]->m_identifier), e->CurrentEnv->weight, host_blend,
	    host ? g.weather_sent_count : g.weather_applies, host ? "syncs sent" : "applied", g.refused_weather);
}

bool allow_time_factor_change(float factor)
{
	if (mgr().is_host())
	{
		g.time_dirty = true;
		return true;
	}
	if (!mgr().is_client())
		return true;
	++g.refused_time;
	if (refuse_log_allowed())
		::Msg("[coop] time: time factor change to %.2f refused on a client (the host owns the clock)", factor);
	return false;
}

bool intercept_time_jump(u32 days, u32 hours, u32 mins)
{
	if (mgr().is_host())
	{
		g.time_dirty = true;
		return false;
	}
	if (!mgr().is_client())
		return false;
	++g.refused_time;
	const u32 minutes = days * 24 * 60 + hours * 60 + mins;
	const u64 now = mgr().now_ms();
	if (mgr().is_live_client() && (!g.last_request_sent || now - g.last_request_sent >= kRequestDebounceMs))
	{
		// May run on the game worker thread (Lua): only record it, frame() sends it.
		g.last_request_sent = now;
		g.request_minutes = minutes;
		g.request_pending = true;
		::Msg("[coop] time: asked the host to skip %u h %02u min; only the host moves time", minutes / 60,
		    minutes % 60);
	}
	return true;
}

bool refuse_script_weather(const char* what)
{
	if (mgr().is_host())
	{
		g.weather_dirty = true;
		return false;
	}
	if (!mgr().is_client())
		return false;
	++g.refused_weather;
	if (refuse_log_allowed())
		::Msg("[coop] weather: level.%s ignored on a client (the host decides the weather)", what);
	return true;
}

bool refuse_engine_time_factor(float factor)
{
	if (!mgr().active() || fsimilar(factor, 1.f))
		return false;
	::Msg("! [coop] time_factor %.3f refused: slow motion is off in a co-op session", factor);
	return true;
}
} // namespace worldstate
} // namespace coop
