// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players, a car's state on every copy and the dolls of sitters (docs/VEHICLES.md
// sections 3, 5.4, 5.6 and 11). Declarations: coop_vehicles.h (state_frame, state_on_state) and coop_vehicles_state.h (the stock hooks);
// the pure half: xrCoop/coop_vehicle_state_rules.h.
//
// The process that decides for a level (the co-op host, whose own actor drives by the stock path; in the Server mode the map server of
// the level) reads the stock fields of every car of its level with a sitter or a running engine and sends VehicleState to the players
// there (the mode's delivery): at a change, and every kStateRepeatMs while the engine runs; silent when parked. A player who comes to
// the map gets the last state of every car. A player's machine plays it on its kinematic copy with the stock methods (CoopCarState):
// StartEngine / StopEngine (the engine sounds, the exhaust), the engine sound at the decider's rpm (the CarSound.cpp hook), the head
// lights, the damage particles of the stages (Play1 / Play2, as ApplyDamage), the transmission switch sound. Its physics and health are
// the stock's own (coop_objects.cpp poses the copy).
// The doll of a seated player on another machine (coop_players.cpp drive_avatar -> doll_frame): at his seat of the car's copy, in the
// vehicle idle of its own animations, else hidden; on the decider his proxy stands where coop_vehicles_seat.cpp
// parks it and is posed the same way.
// Nothing here knows the mode: who decides answers mode().leads_level(), a player's machine is_mirror(). Main thread only.
#include "stdafx.h"
#include "coop_vehicles.h"
#include "coop_vehicles_state.h"
#include "coop_vehicles_client.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_players.h"
#include "coop_world.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_vehicle_state_rules.h"
#include "xrCoop/coop_vehicle_drive_rules.h" // Arrivals

#include "Level.h"
#include "Car.h"
#include "ai/stalker/ai_stalker.h"
#include "step_manager.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"

// A friend of CCar (Car.h: `friend struct CoopCarState;`): what the state of a car needs of its private part, read on the decider and
// played on a copy.
struct CoopCarState
{
	// The bones of the head lights as CCarLights::ParseDefinitions parses them ([lights] headlights -> each section's bone): every one
	// is a light of m_lights, so findLight finds it (it reads past the list for a bone it has not).
	static void light_bones(CCar& car, xr_vector<u16>& out)
	{
		out.clear();
		IKinematics* K = smart_cast<IKinematics*>(car.Visual());
		CInifile* ini = K ? K->LL_UserData() : nullptr;
		if (!ini || !ini->section_exist("lights") || !ini->line_exist("lights", "headlights"))
			return;
		LPCSTR S = ini->r_string("lights", "headlights");
		string64 S1;
		const int count = _GetItemCount(S);
		for (int i = 0; i < count && i < 16; ++i)
		{
			_GetItem(S, i, S1);
			if (!ini->section_exist(S1) || !ini->line_exist(S1, "bone"))
				continue;
			out.push_back(K->LL_BoneID(ini->r_string(S1, "bone")));
		}
	}

	static bool lights_on(CCar& car, const xr_vector<u16>& bones)
	{
		for (const u16 bone : bones)
		{
			SCarLight* light = nullptr;
			if (car.m_lights.findLight(bone, light) && light && light->isOn())
				return true;
		}
		return false;
	}

	// The stock fields of a car on the decider. `sitter`: its stock Owner (the decider's own actor) or a player in its seat table.
	static void read(CCar& car, const xr_vector<u16>& bones, bool sitter, coop::vehicles::CarReading& r)
	{
		r.engine = car.b_engine_on;
		r.lights = lights_on(car, bones);
		// brp: the driver holds the hand brake; a car without a driver stands on it (CCar::Init and detach_Actor call HandBreak)
		r.handbrake = car.brp || !sitter;
		r.rpm = car.m_current_rpm;
		r.max_rpm = car.m_max_rpm;
		r.transmission = u32(car.m_current_transmission_num);
		r.neutral = car.e_state_drive == CCar::neutral;
		r.health = car.GetfHealth();
		r.fuel = car.m_fuel;
		r.tank = car.m_fuel_tank;
		r.damage = car.m_level_applied;
		r.sitter = sitter;
	}

	static bool wrecked(CCar& car) { return car.b_exploded || !car.g_Alive() || car.CPHDestroyable::Destroyed(); }

	static coop::vehicles::CopyFacts facts(CCar& car)
	{
		coop::vehicles::CopyFacts f;
		f.engine = car.b_engine_on;
		f.damage = car.m_level_applied;
		f.wrecked = wrecked(car);
		return f;
	}

	// One VehicleState on a copy, the stock calls in the plan's order. Not ApplyDamage itself: its stage 2 arms the explosion fuse by the
	// copy's own health (CDelayedActionFuse::CheckCondition), whose Update changes that health; the stage's bookkeeping
	// (CDamagableItem::ApplyDamage: m_level_applied) and its particles are what the decider's ApplyDamage shows.
	static void apply(CCar& car, const coop::vehicles::StateMsg& m, const coop::vehicles::CopyPlan& p)
	{
		car.SetfFuel(coop::vehicles::fuel_of(m.fuel, car.m_fuel_tank)); // StartEngine refuses an empty tank (Car.cpp:1214)
		if (p.start)
			car.StartEngine();
		if (p.stop)
			car.StopEngine();
		if (p.lights > 0)
			car.m_lights.TurnOnHeadLights();
		else if (p.lights < 0)
			car.m_lights.TurnOffHeadLights();
		for (u16 level = u16(p.damage_from + 1); level <= p.damage_to; ++level)
		{
			car.m_level_applied = level;
			if (level == 1)
				car.m_damage_particles.Play1(&car);
			else if (level == 2)
				car.m_damage_particles.Play2(&car);
		}
		if (p.gear_sound)
			car.AscCall(CCar::ascSndTransmission);
	}

	// CCarSound::UpdateDrive's pitch for `rpm_fraction` of the copy's m_max_rpm (the same model: the decider's m_max_rpm).
	static float sound_scale(CCar& car, float rpm_fraction)
	{
		return coop::vehicles::sound_scale(rpm_fraction * car.m_max_rpm, car.m_torque_rpm);
	}
};

namespace coop
{
namespace vehicles
{
namespace
{
// ---------------------------------------------------------------- the decider

struct Watched
{
	xr_vector<u16> bones; // the head lights (CoopCarState::light_bones)
	StateCadence cadence;
};

struct Counters
{
	u64 changes = 0, repeats = 0, to_arrivals = 0, scans = 0, over_cap = 0;
	u64 states = 0, bad = 0, no_car_yet = 0, applied = 0, starts = 0, stops = 0, stages = 0, gear_sounds = 0, copies_over_cap = 0;
	u64 dolls_idle = 0, dolls_hidden = 0, dolls_released = 0;
};

struct Decider
{
	u32 world = 0;
	u64 last_frame = 0;
	u64 scan_at = 0;
	xr_map<u16, Watched> cars; // car id (this process's numbering = the host numbering of the messages) -> its cadence
	Arrivals arrivals;
};

// ---------------------------------------------------------------- a player's machine

struct Copy
{
	StateMsg pending; // the last state of the decider
	bool due = false; // not played yet (the copy was not here when it came)
	CopyTrack track;
};

struct Copies
{
	u32 world = 0;
	xr_map<u16, Copy> cars; // host numbering
};

// A doll this machine posed in a car (its local id).
struct Doll
{
	u16 car = kNoCar; // the car's local id
	u8 seat = kNoSeat;
	IKinematicsAnimated* visual = nullptr; // the visual the pose was played on (a changed visual is posed again)
	bool idle = false; // plays the vehicle idle
	bool hidden = false; // hidden by this module (shown again on release)
	// Its character controller is off on a player's machine.
	// On the decider, coop_vehicles_seat.cpp parks the proxy.
	bool collision_off = false;
	bool placed = false; // put at its seat by this module
};

Decider g_dec;
Copies g_copies;
xr_map<u16, Doll> g_dolls;
u32 g_dolls_world = 0; // join::world_generation() of the world the dolls' ids belong to
Counters g_c;

CCar* car_by_id(u16 id) { return smart_cast<CCar*>(world::object_here(id)); }

bool has_driver_place(CCar* car)
{
	IKinematics* K = car ? smart_cast<IKinematics*>(car->Visual()) : nullptr;
	CInifile* ini = K ? K->LL_UserData() : nullptr;
	return ini && ini->line_exist("car_definition", "driver_place");
}

void send_all(const StateMsg& m)
{
	Writer w(16);
	write(w, m);
	net::host_broadcast(Msg::VehicleState, w, Lane::Reliable);
}

void send_to(PeerId peer, const StateMsg& m)
{
	Writer w(16);
	write(w, m);
	net::host_send(peer, Msg::VehicleState, w, Lane::Reliable);
}

// The cars of the decider's level (a car spawned since is found by the next scan).
void scan_cars()
{
	++g_c.scans;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CCar* car = smart_cast<CCar*>(Level().Objects.o_get_by_iterator(i));
		if (!car || car->getDestroy() || g_dec.cars.count(car->ID()))
			continue;
		if (g_dec.cars.size() >= kMaxStateCars)
		{
			++g_c.over_cap;
			break;
		}
		CoopCarState::light_bones(*car, g_dec.cars[car->ID()].bones);
	}
}

// A sitter: the stock Owner (the decider's own actor: the co-op host's) or a client in seat 0 of the seat table
// (coop_vehicles_seat.cpp).
bool has_sitter(CCar* car) { return car->Engaged() || driver_of(car->ID()) != kNoCharacter; }

void decider_frame(u64 now)
{
	const u32 world = join::world_generation();
	if (world != g_dec.world || now - g_dec.last_frame > kStateGapMs)
	{
		// a new world (its cars are others), or the frames stopped (no session, no world): what was sent is nobody's now
		g_dec = Decider();
		g_dec.world = world;
	}
	g_dec.last_frame = now;
	if (!g_dec.scan_at || now - g_dec.scan_at >= kStateScanMs)
	{
		g_dec.scan_at = now;
		scan_cars();
	}
	for (auto it = g_dec.cars.begin(); it != g_dec.cars.end();)
	{
		CCar* car = car_by_id(it->first);
		if (!car)
		{
			it = g_dec.cars.erase(it); // gone (destroyed, offline): its copies go with it
			continue;
		}
		Watched& w = it->second;
		CarReading r;
		CoopCarState::read(*car, w.bones, has_sitter(car), r);
		const StateMsg m = state_of(car->ID(), r);
		const StateSend s = w.cadence.step(m, live(r), u32(now));
		if (s != StateSend::None)
		{
			send_all(m);
			w.cadence.sent(m, u32(now));
			++(s == StateSend::Change ? g_c.changes : g_c.repeats);
		}
		++it;
	}
	// a player who came to the map or finished loading it: the last state of every car that has one, to him
	static std::vector<std::pair<PeerId, bool>> peers; // scratch of this frame: cleared, not freed
	peers.clear();
	mgr().session().for_each_joined(
	    [](const PeerInfo& p) { peers.push_back({p.id, p.state == PeerState::Joined && mode().where(p.id) == Where::Here}); });
	for (const PeerId peer : g_dec.arrivals.update(peers))
		for (const auto& it : g_dec.cars)
			if (it.second.cadence.known())
			{
				send_to(peer, it.second.cadence.last());
				++g_c.to_arrivals;
			}
}

// ---------------------------------------------------------------- a player's machine

void copies_world()
{
	const u32 world = join::world_generation();
	if (world == g_copies.world)
		return;
	g_copies = Copies(); // the cars of another world
	g_copies.world = world;
}

// The pending state of `host_id` on its copy, if the copy is here.
void play(u16 host_id, Copy& c)
{
	CCar* car = car_by_id(join::to_local(host_id));
	if (!car)
		return; // not spawned here yet (or on another level): played when it comes (copies_frame)
	const CopyPlan p = c.track.apply(c.pending, CoopCarState::facts(*car), u32(mgr().now_ms()));
	CoopCarState::apply(*car, c.pending, p);
	c.due = false;
	++g_c.applied;
	g_c.starts += p.start ? 1 : 0;
	g_c.stops += p.stop ? 1 : 0;
	g_c.stages += p.damage_to > p.damage_from ? u64(p.damage_to - p.damage_from) : 0;
	g_c.gear_sounds += p.gear_sound ? 1 : 0;
}

void copies_frame()
{
	copies_world();
	for (auto& it : g_copies.cars)
		if (it.second.due)
			play(it.first, it.second);
}

// ---------------------------------------------------------------- dolls

// Where `doll` sits now on this machine: true and the car's local id, the seat and the car's anim type. The decider: its seat table
// (seated(); the car by driver_of, a passenger's car is not told). A player's machine: the mirror of the decider's table.
struct DollSeat
{
	CCar* car = nullptr;
	u8 seat = kNoSeat;
	// This module puts it at its seat on a player's machine. On the decider, coop_vehicles_seat.cpp parks it.
	bool placed_here = false;
};

bool doll_seat(CAI_Stalker* doll, DollSeat& out)
{
	const u16 id = doll->ID();
	if (seated(id))
	{
		for (const auto& it : g_dec.cars)
			if (driver_of(it.first) == id)
			{
				out.car = car_by_id(it.first);
				out.seat = kDriverSeat;
				break;
			}
		out.placed_here = false;
		return true;
	}
	if (!is_mirror())
		return false;
	Seated where;
	if (!mirror_seat_of(join::to_host(id), where))
		return false;
	out.car = car_by_id(join::to_local(where.car));
	out.seat = where.seat;
	out.placed_here = true;
	return out.car != nullptr; // the copy is not here: he is drawn where his snapshots say
}

// The doll back to normal: shown if this module hid it, its character controller back (alive), his own motions again.
void release_doll(CAI_Stalker* doll, const Doll& d)
{
	if (!doll)
		return;
	if (d.hidden)
		doll->setVisible(TRUE);
	if (d.collision_off && doll->g_Alive())
		doll->set_enable_movement_collision(true);
	players::on_avatar_visual_changed(doll->ID()); // drive_avatar plays the owner's current motions again
	++g_c.dolls_released;
}

// The vehicle idle (as CActor::attach_Vehicle plays idles[0]) or hidden, once per seat and visual. `placed`: a player's machine put the
// doll at its seat (false there: no transform for that seat, it hides).
void pose_doll(CAI_Stalker* doll, Doll& d, const DollSeat& where, bool placed)
{
	IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(doll->Visual());
	const u16 car_id = where.car ? u16(where.car->ID()) : kNoCar;
	if (d.visual == ka && d.car == car_id && d.seat == where.seat && d.placed == placed)
		return;
	d.visual = ka;
	d.car = car_id;
	d.seat = where.seat;
	d.placed = placed;
	MotionID idle;
	if (ka)
		idle = ka->ID_Cycle_Safe(driver_idle_name(where.car ? where.car->DriverAnimationType() : 0).c_str());
	const bool place = where.car ? has_driver_place(where.car) : true;
	DollPose pose = doll_pose(where.seat, place, idle.valid());
	if (where.placed_here && !placed)
		pose = DollPose::Hidden;
	// On the decider, coop_vehicles_seat.cpp hides the proxy without driver_place, as it does for the stock Owner.
	// Only that code controls its visibility.
	if (!where.placed_here && !place && where.seat == kDriverSeat)
		pose = DollPose::Idle;
	if (pose == DollPose::Idle && idle.valid())
	{
		ka->PlayCycle(idle, FALSE);
		doll->CStepManager::on_animation_start(MotionID(), nullptr);
		d.idle = true;
		++g_c.dolls_idle;
		if (d.hidden)
		{
			doll->setVisible(TRUE);
			d.hidden = false;
		}
		return;
	}
	d.idle = false;
	if (pose == DollPose::Hidden && !d.hidden && doll->getVisible())
	{
		doll->setVisible(FALSE);
		d.hidden = true;
		++g_c.dolls_hidden;
	}
}

// The dolls of another world (a load, a level change) are forgotten without touching any
// object: an id of the old world can name another stalker here, which release_doll would show and give its collision back.
void dolls_world()
{
	const u32 world = join::world_generation();
	if (world == g_dolls_world)
		return;
	g_dolls.clear();
	g_dolls_world = world;
}

// Every frame: a posed doll whose player sits no more (or that is not driven any more: dead, gone) is let go.
void dolls_sweep()
{
	dolls_world();
	for (auto it = g_dolls.begin(); it != g_dolls.end();)
	{
		CAI_Stalker* doll = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(it->first));
		DollSeat where;
		if (doll && !doll->getDestroy() && doll_seat(doll, where))
		{
			++it;
			continue;
		}
		if (doll && !doll->getDestroy())
			release_doll(doll, it->second);
		it = g_dolls.erase(it);
	}
}
} // namespace

// ---------------------------------------------------------------- API (coop_vehicles.h)

void state_frame()
{
	const u64 now = mgr().now_ms();
	if (mode().leads_level())
		decider_frame(now);
	if (is_mirror())
		copies_frame();
	dolls_sweep();
}

void state_on_state(Reader& payload)
{
	StateMsg m;
	if (!read(payload, m))
	{
		if (++g_c.bad <= 5)
			::Msg("! [coop] vehicle: a VehicleState that does not read was dropped");
		return;
	}
	if (!is_mirror() || !world_up())
		return; // the decider's own cars are the real ones; a world that is not up has none
	copies_world();
	++g_c.states;
	auto it = g_copies.cars.find(m.car);
	if (it == g_copies.cars.end())
	{
		if (g_copies.cars.size() >= kMaxStateCars)
		{
			++g_c.copies_over_cap;
			return;
		}
		it = g_copies.cars.emplace(m.car, Copy()).first;
	}
	it->second.pending = m;
	it->second.due = true;
	play(m.car, it->second);
	if (it->second.due)
		++g_c.no_car_yet;
}

// ---------------------------------------------------------------- API (coop_vehicles_state.h)

void copy_sound_scale(CCar* car, float& scale)
{
	if (!car || !is_mirror())
		return;
	auto it = g_copies.cars.find(join::to_host(car->ID()));
	if (it == g_copies.cars.end() || !it->second.track.known() || g_copies.world != join::world_generation())
		return;
	scale = CoopCarState::sound_scale(*car, it->second.track.rpm_fraction(u32(mgr().now_ms())));
}

bool doll_frame(CAI_Stalker* doll)
{
	if (!doll || !mgr().active() || !world_up())
		return false;
	dolls_world(); // no doll of another world is let go here
	DollSeat where;
	const bool sits = doll_seat(doll, where);
	auto it = g_dolls.find(doll->ID());
	if (!sits)
	{
		if (it != g_dolls.end())
		{
			release_doll(doll, it->second);
			g_dolls.erase(it);
		}
		return false;
	}
	Doll& d = g_dolls[doll->ID()];
	bool placed = false;
	if (where.placed_here)
	{
		if (!d.collision_off)
		{
			// A car would push its own sitter.
			// coop_vehicles_seat.cpp parks the decider's proxy for the same reason.
			doll->set_enable_movement_collision(false);
			d.collision_off = true;
		}
		Fmatrix seat;
		placed = seat_transform(where.car, where.seat, seat);
		if (placed)
			doll->XFORM().set(seat); // as CCar::VisualUpdate puts its Owner: XFORM * the seat bone
		// no transform for that seat yet (passengers: coop_vehicles_ride.cpp): hidden where his snapshots say, not standing in the car
	}
	pose_doll(doll, d, where, placed);
	return true;
}

void state_print_status()
{
	u32 live_cars = 0;
	for (const auto& it : g_dec.cars)
		live_cars += it.second.cadence.known() ? 1 : 0;
	::Msg("[coop]   vehicle states: %u car(s) watched here, %u told; sent %llu change(s), %llu repeat(s), %llu to arrivals (%llu scans, %llu "
	      "over the cap)",
	    u32(g_dec.cars.size()), live_cars, g_c.changes, g_c.repeats, g_c.to_arrivals, g_c.scans, g_c.over_cap);
	::Msg("[coop]   vehicle states here: %llu got (%llu bad, %llu before the car, %llu over the cap), %llu played: %llu engine "
	      "start(s), %llu stop(s), %llu damage stage(s), %llu gear sound(s); %u car(s) known",
	    g_c.states, g_c.bad, g_c.no_car_yet, g_c.copies_over_cap, g_c.applied, g_c.starts, g_c.stops, g_c.stages, g_c.gear_sounds,
	    u32(g_copies.cars.size()));
	::Msg("[coop]   vehicle dolls: %u seated now; %llu idle, %llu hidden, %llu released", u32(g_dolls.size()), g_c.dolls_idle,
	    g_c.dolls_hidden, g_c.dolls_released);
}
} // namespace vehicles
} // namespace coop
