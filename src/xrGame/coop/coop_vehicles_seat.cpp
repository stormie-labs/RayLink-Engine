// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players (F5-VEH-2, docs/VEHICLES.md): the decider. Declarations: coop_vehicles.h; pure rules:
// xrCoop/coop_vehicle_rules.h (F5-VEH-1) and xrCoop/coop_vehicle_drive_rules.h (this unit).
//
// The process that decides for a player's level (the co-op host; in the Server mode the map server of his level, whose session
// peers are the players there) seats players in cars, drives a client's car without an Owner and lets him out. The seat table
// is the truth. The same code in both modes: it asks mode() only about a player (where, decider_of, departure) and about the map it
// leads (leads_level), and sends through the mode's delivery (net::host_broadcast; net::host_send to the asker or to a
// player who comes to the map).
//   VehicleReq   (a player's)   enter / exit / switch_seat: the facts from his character here and the car, the verdict
//                               (the game rules, then the checks of his word through coop_trust), VehicleSeat granted to the
//                               players of the level or refused to him alone.
//   VehicleInput (the driver's) his held keys and press counters -> the stock CCar::OnKeyboardPress / OnKeyboardRelease calls
//                               (plan_drive), the weapon's aim; stale input -> every key released and the hand brake.
//   server_frame                the releases the decider makes itself (disconnect, leaving the map, death, wound, the car
//                               destroyed or gone), the stale inputs, every seated proxy parked at its seat, the seat table
//                               for a player who comes to the map.
// A client's car has no Owner here: seat 0 does to the car what CCar::attach_Actor / detach_Actor do minus the Owner and the
// camera (kAttachLines / kDetachLines, run by CoopCarDriver, a friend of CCar). The decider's own actor (the co-op host's)
// keeps the stock path and is written in the table as seat 0 by the hooks in CActor::attach_Vehicle / detach_Vehicle.
#include "stdafx.h"
#include "coop_vehicles.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_players.h"
#include "coop_survival.h"
#include "coop_vehicles_ride.h" // F5-VEH-5: the passenger places (seat counts, seat_transform, the door of a seat)
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_vehicle_drive_rules.h"

#include "Level.h"
#include "Actor.h"
#include "Car.h"
#include "CarWeapon.h"
#include "ai/stalker/ai_stalker.h"
#include "xr_level_controller.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xrphysics/PhysicsShell.h"

// A friend of CCar (Car.h: `friend struct CoopCarDriver;`): what a driver who is not the car's Owner needs of its private part.
struct CoopCarDriver
{
	// One kept line of CCar::attach_Actor / detach_Actor (coop_vehicle_drive_rules.h kAttachLines / kDetachLines).
	static void step(CCar& car, coop::vehicles::CarStep s)
	{
		using coop::vehicles::CarStep;
		CPhysicsShell* shell = car.PPhysicsShell();
		switch (s)
		{
		case CarStep::EnablePhysics:
			if (shell)
				shell->Enable();
			break;
		case CarStep::AddObstacleCallback:
			if (shell)
				shell->add_ObjectContactCallback(CCar::ActorObstacleCallback);
			break;
		case CarStep::ProcessingActivate: car.processing_activate(); break;
		case CarStep::ReleaseHandBreak: car.ReleaseHandBreak(); break;
		case CarStep::RemoveObstacleCallback:
			if (shell)
				shell->remove_ObjectContactCallback(CCar::ActorObstacleCallback);
			break;
		case CarStep::NeutralDrive: car.NeutralDrive(); break;
		case CarStep::Unclutch: car.Unclutch(); break;
		case CarStep::ResetKeys: car.ResetKeys(); break;
		case CarStep::RpmToMin: car.m_current_rpm = car.m_min_rpm; break;
		case CarStep::HandBreak: car.HandBreak(); break;
		case CarStep::ProcessingDeactivate: car.processing_deactivate(); break;
		case CarStep::None: break;
		}
	}

	// CCarWeapon::IsActive of the car's weapon (CarInput.cpp:208 reads it for kWPN_FUNC).
	static bool weapon_active(CCar& car) { return car.m_car_weapon && car.m_car_weapon->IsActive(); }

	// The door test of CCar::Enter (Car.cpp:861-875: enter_pos = (pos + foot_pos) / 2, any door's CanEnter) over the doors whose
	// state the decider knows: no joint, opened or broken. A jointed closed door opens only on the player's copy (docs/VEHICLES.md
	// §18) and its IsInArea is never true then (CarDoors.cpp:483-489). 1 passed, 0 failed, -1 unknown (no such door).
	static int door_test(CCar& car, const Fvector& head, const Fvector& dir, const Fvector& centre)
	{
		Fvector enter_pos;
		enter_pos.add(head, centre);
		enter_pos.mul(0.5f);
		int known = -1;
		for (auto& it : car.m_doors)
		{
			CCar::SDoor& d = it.second;
			if (d.joint && d.state != CCar::SDoor::opened && d.state != CCar::SDoor::broken)
				continue;
			known = 0;
			if (d.CanEnter(head, dir, enter_pos))
				return 1;
		}
		return known;
	}
};

namespace coop
{
namespace vehicles
{
namespace
{
// The exit point a seated client would get is kept fresh this often (save_position reads it every frame).
constexpr u64 kExitEveryMs = 250;

enum TrustCheck : u8
{
	TrustRequestRate,
	TrustEnterDistance,
	TrustEnterDoor,
	TrustInputRate,
	TrustChecks,
};
const char* const kTrustNames[TrustChecks] = {kTrustRequestRate, kTrustEnterDistance, kTrustEnterDoor, kTrustInputRate};

// A seated character, as the table has him, and what the decider did to his proxy.
struct Sitter
{
	u16 car = kNoCar;
	u8 seat = kNoSeat;
	PeerId peer = kInvalidPeer; // his player; kInvalidPeer for the decider's own actor
	bool own = false; // the decider's own actor (the co-op host's): the stock path, his row only
	bool parked = false; // his proxy's character controller is off (set_enable_movement_collision(false))
	bool hidden = false; // his proxy is hidden (the car's model has no driver_place, as the stock Owner)
	Fvector car_pos = {0.f, 0.f, 0.f}; // the car's last position (CarGone: he stands there)
	Fvector exit = {0.f, 0.f, 0.f}; // where a release would put him now (save_position)
	u64 exit_at = 0; // when `exit` was found; 0: not yet
};

struct Driver
{
	InputTrack track;
	RateGate gate;
};

struct State
{
	bool installed = false; // install_server ran in this session
	u32 world = 0; // join::world_generation() of the world the table is about
	SeatTable table;
	xr_map<u16, Sitter> sitters; // character -> his seat
	xr_map<u16, Driver> drivers; // a client in seat 0 -> his input
	xr_map<u16, u64> last_request; // character -> his last VehicleReq (veh_request_rate)
	Arrivals arrivals;
	TrustMode trust_mode[TrustChecks] = {TrustMode::Log, TrustMode::Log, TrustMode::Log, TrustMode::Log};
	TrustCount trust_count[TrustChecks];
	SeatStats stats;
	PerSecond inputs;
};

State g;

u64 now_ms() { return mgr().now_ms(); }

TrustMode read_trust_mode(const char* name)
{
	// gamedata/configs/mod_system_coop_trust.ltx [coop_trust] (system.ltx includes mod_system_*.ltx): the line of the check,
	// else "default", else log (coop_trust.script mode_of)
	TrustMode m = TrustMode::Log;
	if (!pSettings || !pSettings->section_exist(kTrustSection))
		return m;
	if (pSettings->line_exist(kTrustSection, kTrustDefaultKey))
		parse_trust_mode(pSettings->r_string(kTrustSection, kTrustDefaultKey), m);
	if (pSettings->line_exist(kTrustSection, name))
		parse_trust_mode(pSettings->r_string(kTrustSection, name), m);
	return m;
}

// coop_trust.pass for the engine: true lets the request through. `text` is built here from numbers, never from the network.
bool trust(TrustCheck c, bool ok, const char* text)
{
	const TrustResult r = trust_pass(g.trust_mode[c], ok, g.trust_count[c]);
	if (r.log)
		::Msg("[coop-trust] ! %s: %s (%s, %u of %u failed)", kTrustNames[c], text, r.pass ? "would be refused: let through" : "refused",
		    g.trust_count[c].failed, g.trust_count[c].checked);
	return r.pass;
}

CCar* car_here(u16 id)
{
	if (id == kNoCar)
		return nullptr;
	CObject* o = Level().Objects.net_Find(id);
	return o && !o->getDestroy() ? smart_cast<CCar*>(o) : nullptr;
}

CAI_Stalker* body_here(u16 character)
{
	if (character == kNoCharacter)
		return nullptr;
	CObject* o = Level().Objects.net_Find(character);
	return o && !o->getDestroy() ? smart_cast<CAI_Stalker*>(o) : nullptr;
}

u16 own_character()
{
	CActor* a = Actor();
	return a ? a->ID() : kNoCharacter;
}

// The decider decides for its own actor: the co-op host (a map server's and a world server's actors are ghosts, a player's
// machine has a decider elsewhere, single player has none).
bool decides_own_actor()
{
	const u16 own = own_character();
	return g.installed && own != kNoCharacter && mode().decider_of(own) == Decider::This;
}

// Seats of a car: the driver's and the passenger places of configs/coop_vehicles.ltx for its section (F5-VEH-5); the driver's
// alone without them (a stock car).
u8 seat_count(CCar* car) { return car_seat_count(car); }

bool has_driver_place(CCar* car)
{
	IKinematics* K = car ? smart_cast<IKinematics*>(car->Visual()) : nullptr;
	CInifile* ini = K ? K->LL_UserData() : nullptr;
	return ini && ini->line_exist("car_definition", "driver_place");
}

bool car_destroyed(CCar* car) { return !car->g_Alive() || car->b_exploded || car->CPHDestroyable::Destroyed(); }

bool car_unsupported(CCar* car) { return car->IsRemoteControl() || car->CarDrone() != nullptr; }

Point3 p3(const Fvector& v)
{
	Point3 p;
	p.x = v.x;
	p.y = v.y;
	p.z = v.z;
	return p;
}

// A point where a body stands: ground under it within 4 m, room for a standing body above it, and no wall between it and the
// car. On success `p` is moved onto the ground.
bool stand_point(CCar* car, Fvector& p)
{
	collide::rq_result R;
	Fvector top = p;
	top.y += 1.5f;
	if (!Level().ObjectSpace.RayPick(top, Fvector().set(0.f, -1.f, 0.f), 4.f, collide::rqtStatic, R, nullptr))
		return false;
	Fvector ground = top;
	ground.y -= R.range;
	Fvector feet = ground;
	feet.y += 0.3f;
	if (Level().ObjectSpace.RayTest(feet, Fvector().set(0.f, 1.f, 0.f), 1.5f, collide::rqtBoth, nullptr, car))
		return false;
	Fvector from;
	car->Center(from);
	Fvector chest = ground;
	chest.y += 1.f;
	Fvector dir;
	dir.sub(chest, from);
	const float len = dir.magnitude();
	if (len > EPS_L)
	{
		dir.div(len);
		if (Level().ObjectSpace.RayTest(from, dir, len, collide::rqtStatic, nullptr, car))
			return false;
	}
	p = ground;
	return true;
}

// Where a released sitter of `car` stands: the stock exit point (CCar::ExitPosition, as CActor::detach_Vehicle takes it), else
// the nearest free point beside the car (exit_candidates); the stock point when none is free. The car gone: where it was.
Fvector exit_position(CCar* car, const Sitter& s)
{
	if (!car)
		return s.car_pos;
	Fvector stock = car->ExitPosition();
	if (s.seat != kDriverSeat)
		passenger_exit(car, s.seat, stock); // F5-VEH-5: a passenger's door (exit_<i>); the stock point when the seat has none here
	const Fbox& box = car->BoundingBox();
	Fvector half, local_centre, centre;
	box.getradius(half);
	box.getcenter(local_centre);
	car->XFORM().transform_tiny(centre, local_centre);
	const std::vector<Point3> points = exit_candidates(p3(stock), p3(centre), p3(car->XFORM().i), p3(car->XFORM().k), half.x, half.z);
	for (const Point3& c : points)
	{
		Fvector p = Fvector().set(c.x, c.y, c.z);
		if (stand_point(car, p))
			return p;
	}
	return stock;
}

// The proxy of a seated client: its character controller off (a car would push its own driver, as CActor::attach_Vehicle
// destroys the actor's), hidden when the model has no driver_place (as the stock Owner, Car.cpp:797-803), and put at its seat
// every frame (as CCar::VisualUpdate puts the Owner: XFORM * the seat bone).
void park(u16 character, Sitter& s, CCar* car)
{
	CAI_Stalker* body = body_here(character);
	if (!body || !car)
		return;
	if (!s.parked)
	{
		body->set_enable_movement_collision(false);
		s.parked = true;
		if (!has_driver_place(car))
		{
			body->setVisible(FALSE);
			s.hidden = true;
		}
	}
	Fmatrix seat;
	if (seat_transform(car, s.seat, seat))
		body->XFORM().set(seat);
}

// The proxy leaves its seat: upright at `at`, facing as it sat, its character controller back (where it fits: CreateCharacterSafe
// moves it out of what it stands in), shown.
void unpark(u16 character, Sitter& s, const Fvector& at)
{
	CAI_Stalker* body = body_here(character);
	if (!body || !s.parked)
		return;
	Fvector dir = body->XFORM().k;
	dir.y = 0.f;
	if (dir.square_magnitude() < EPS_S)
		dir.set(0.f, 0.f, 1.f);
	dir.normalize();
	Fmatrix x;
	x.setHPB(dir.getH(), 0.f, 0.f);
	x.c.set(at);
	body->XFORM().set(x);
	if (body->g_Alive())
		body->set_enable_movement_collision(true);
	if (s.hidden)
		body->setVisible(TRUE);
	s.parked = false;
	s.hidden = false;
}

void run_lines(CCar& car, const StockLine* lines, size_t n)
{
	for (size_t i = 0; i < n; ++i)
		if (lines[i].step != CarStep::None)
			CoopCarDriver::step(car, lines[i].step);
}

// A client takes seat 0: what CCar::attach_Actor does to the car minus the Owner and the camera.
void drive_on(CCar& car) { run_lines(car, kAttachLines, sizeof(kAttachLines) / sizeof(kAttachLines[0])); }

// He leaves it: what CCar::detach_Actor does to the car minus the Owner.
void drive_off(CCar& car) { run_lines(car, kDetachLines, sizeof(kDetachLines) / sizeof(kDetachLines[0])); }

// The stock calls of one input step on the car (plan_drive), then the aim.
void apply_step(CCar& car, const InputStep& st)
{
	for (const DriveCall& c : plan_drive(st, car.HasWeapon(), CoopCarDriver::weapon_active(car)))
	{
		int cmd = -1;
		switch (c.key)
		{
		case DriveKey::Forward: cmd = kFWD; break;
		case DriveKey::Back: cmd = kBACK; break;
		case DriveKey::Left: cmd = kL_STRAFE; break;
		case DriveKey::Right: cmd = kR_STRAFE; break;
		case DriveKey::Brake: cmd = kJUMP; break;
		case DriveKey::TransUp: cmd = kACCEL; break;
		case DriveKey::TransDown: cmd = kCROUCH; break;
		case DriveKey::Fire: cmd = kWPN_FIRE; break;
		case DriveKey::WeaponSwitch: cmd = kWPN_FUNC; break;
		case DriveKey::Engine: cmd = kDETECTOR; break;
		case DriveKey::Lights: cmd = kTORCH; break;
		case DriveKey::Aim:
			// CarWeapon.cpp:294-301: m_destEnemyDir.setHP(yaw, pitch)
			car.SetParam(CCarWeapon::eWpnDesiredDir, Fvector2().set(angle_from_wire(st.aim_yaw), angle_from_wire(st.aim_pitch)));
			break;
		}
		if (cmd < 0)
			continue;
		if (c.press)
			car.OnKeyboardPress(cmd);
		else
			car.OnKeyboardRelease(cmd);
	}
}

void send_all(const SeatMsg& m)
{
	Writer w(24);
	write(w, m);
	net::host_broadcast(Msg::VehicleSeat, w, Lane::Reliable);
}

void send_to(PeerId peer, const SeatMsg& m)
{
	Writer w(24);
	write(w, m);
	net::host_send(peer, Msg::VehicleSeat, w, Lane::Reliable);
}

void refuse(PeerId to, u16 car, u8 seat, u16 character, Refusal why)
{
	g.stats.refuse(why);
	send_to(to, SeatMsg::refused(car, seat, character, why));
}

// A character takes a seat the table gave him: his row, the car driven for a client in seat 0, his proxy parked, granted to all.
void seated_now(u16 character, PeerId peer, CCar* car, u8 seat, bool own)
{
	Sitter& s = g.sitters[character];
	s = Sitter();
	s.car = car->ID();
	s.seat = seat;
	s.peer = peer;
	s.own = own;
	s.car_pos = car->Position();
	if (!own && seat == kDriverSeat)
	{
		drive_on(*car);
		Driver& d = g.drivers[character];
		d = Driver();
		d.track.start(car->ID(), u32(now_ms()));
	}
	if (!own)
	{
		park(character, s, car);
		s.exit = exit_position(car, s); // save_position from the first frame
		s.exit_at = now_ms();
	}
	++g.stats.granted;
	send_all(SeatMsg::granted(car->ID(), seat, character));
}

// The stock Owner took seat 0 of a client's car (own_actor_attached after CCar::attach_Actor): only the pairs of drive_on
// are undone (the obstacle callback, processing); the rest of detach_Actor (neutral, keys, hand brake) would stop the Owner's
// drive that attach_Actor has just started.
void drive_pairs_off(CCar& car)
{
	CoopCarDriver::step(car, CarStep::RemoveObstacleCallback);
	CoopCarDriver::step(car, CarStep::ProcessingDeactivate);
}

// Frees the seat of `character` and tells everybody where he stands now. For a client: the car without its driver (seat 0),
// his proxy back on its feet at the exit point. The decider's own actor: only the row (its stock detach did the rest).
// `owner_took_car`: the decider's own actor took the car by the stock path (a script past the gate of Actor_Events.cpp).
void release(u16 character, Release why, bool owner_took_car = false)
{
	auto it = g.sitters.find(character);
	if (it == g.sitters.end())
	{
		g.table.release_all(character);
		return;
	}
	Sitter s = it->second;
	g.sitters.erase(it);
	CCar* car = car_here(s.car);
	const Fvector at = s.own ? (car ? car->ExitPosition() : s.car_pos) : exit_position(car, s);
	g.table.release_all(character);
	if (!s.own)
	{
		if (s.seat == kDriverSeat)
		{
			g.drivers.erase(character);
			if (car && owner_took_car)
				drive_pairs_off(*car);
			else if (car)
				drive_off(*car);
		}
		unpark(character, s, at);
	}
	g.stats.release(why);
	const float pos[3] = {at.x, at.y, at.z};
	send_all(SeatMsg::released(s.car, s.seat, character, why, pos));
}

// Everything forgotten without a message: the session is over (`objects_live`: its objects are still here, the proxies stand
// up and the cars are left as detach_Actor leaves them) or the world was replaced (its objects are gone; a new world's objects
// with the same ids are not ours to touch).
void drop_quietly(bool objects_live)
{
	if (objects_live)
		for (auto& it : g.sitters)
		{
			Sitter& s = it.second;
			if (s.own)
				continue;
			CCar* car = car_here(s.car);
			if (s.seat == kDriverSeat && car)
				drive_off(*car);
			unpark(it.first, s, exit_position(car, s));
		}
	g.sitters.clear();
	g.drivers.clear();
	g.table.clear();
	g.last_request.clear();
	g.arrivals.clear();
}

// The stock Owner of `car` (the decider's own actor, the only one a car has on the decider) is in the table: a seat taken by
// the stock path before this session's table existed counts.
void note_stock_owner(CCar* car)
{
	if (!car->Engaged() || g.table.driver_of(car->ID()) != kNoCharacter)
		return;
	CActor* a = Actor();
	if (a && a->Holder() == car)
		own_actor_attached(car);
}

// The character of the peer that sent a request: his proxy here, online and in the world; kNoCharacter when he has none on
// this map (joining, on another map).
u16 asker(PeerId from)
{
	const u16 character = join::proxy_of_peer(from);
	if (character == kNoCharacter || mode().where(from) != Where::Here || !join::host_player_in_world(character) ||
	    !body_here(character))
		return kNoCharacter;
	return character;
}

u32 since_last(u16 character, u64 now)
{
	auto it = g.last_request.find(character);
	const u64 since = it == g.last_request.end() ? u64(UINT32_MAX) : now - it->second;
	g.last_request[character] = now;
	return since > UINT32_MAX ? UINT32_MAX : u32(since);
}

bool rate_ok(u16 character, u32 since)
{
	string256 text;
	xr_sprintf(text, "character %u: a seat request %u ms after the last (min %u)", character, since, kRequestMinIntervalMs);
	return trust(TrustRequestRate, since >= kRequestMinIntervalMs, text);
}

// ---------------------------------------------------------------- VehicleReq

void on_enter(PeerId from, const ReqMsg& m, u16 character, u64 now)
{
	CCar* car = car_here(m.car);
	CAI_Stalker* body = body_here(character);
	if (car)
		note_stock_owner(car);
	EnterFacts f;
	f.car_known = car != nullptr;
	f.supported = car && !car_unsupported(car);
	f.car_destroyed = car && car_destroyed(car);
	f.enter_locked = car && car->EnterLocked();
	const survival::Life life = survival::life_of(character);
	f.alive = body->g_Alive() && (life == survival::Life::Alive || life == survival::Life::Wounded);
	f.wounded = life == survival::Life::Wounded;
	// Carrying a body: the decider has no such fact of a player's character (his drag is his own actor's, on his machine, and
	// stock ActorUse drops it with the same F before the request goes out, ActorInput.cpp:549-550): "not carrying".
	f.carrying = false;
	f.seated = g.table.seat_of(character).car != kNoCar;
	f.seats = car ? seat_count(car) : 0;
	f.seat = m.seat;
	const u8 pick = car ? g.table.pick(m.car, f.seats, m.seat) : kNoSeat;
	// a stock Owner the table does not know (a script, a session started in a car) holds seat 0 too
	f.seat_free = pick != kNoSeat && !(pick == kDriverSeat && car->Engaged() && g.table.driver_of(m.car) == kNoCharacter);
	f.distance = car ? body->Position().distance_to(car->Position()) : 0.f;
	f.since_last_ms = since_last(character, now);
	const Refusal rule = enter_rules(f);
	if (rule != Refusal::None)
		return refuse(from, m.car, m.seat, character, rule);
	// the checks of his word, each with its own line (TZ 6.4): the request rate, the distance, the door
	if (!rate_ok(character, f.since_last_ms))
		return refuse(from, m.car, m.seat, character, Refusal::TooSoon);
	string256 text;
	xr_sprintf(text, "character %u: %.1f m from car %u (max %.1f)", character, f.distance, m.car, kEnterMaxDistance);
	if (!trust(TrustEnterDistance, f.distance <= kEnterMaxDistance, text))
		return refuse(from, m.car, m.seat, character, Refusal::TooFar);
	float yaw = 0.f, pitch = 0.f;
	if (players::avatar_view(character, yaw, pitch)) // his look, as his last PlayerState said
	{
		Fvector dir = Fvector().setHP(yaw, -pitch);
		Fvector centre;
		body->Center(centre);
		Fvector head = centre; // CActor::attach_Vehicle passes the camera: the head of his body here
		IKinematics* K = smart_cast<IKinematics*>(body->Visual());
		const u16 bone = K ? K->LL_BoneID("bip01_head") : BI_NONE;
		if (bone != BI_NONE)
		{
			Fmatrix head_xform;
			head_xform.mul_43(body->XFORM(), K->LL_GetTransform(bone));
			head = head_xform.c;
		}
		const int door = CoopCarDriver::door_test(*car, head, dir, centre);
		if (door >= 0)
		{
			xr_sprintf(text, "character %u: at no door of car %u by his look", character, m.car);
			if (!trust(TrustEnterDoor, door == 1, text))
				return refuse(from, m.car, m.seat, character, Refusal::TooFar);
		}
	}
	const TakeResult t = g.table.take(m.car, f.seats, m.seat, character);
	if (t.reason != Refusal::None)
		return refuse(from, m.car, m.seat, character, t.reason);
	seated_now(character, from, car, t.seat, false);
}

void on_exit(PeerId from, const ReqMsg& m, u16 character, u64 now)
{
	CCar* car = car_here(m.car);
	ExitFacts f;
	f.seated = g.table.seat_of(character).car == m.car;
	f.exit_locked = car && car->ExitLocked();
	f.since_last_ms = since_last(character, now);
	const Refusal rule = exit_rules(f);
	if (rule != Refusal::None)
		return refuse(from, m.car, m.seat, character, rule);
	if (!rate_ok(character, f.since_last_ms))
		return refuse(from, m.car, m.seat, character, Refusal::TooSoon);
	release(character, Release::Exit);
}

void on_switch(PeerId from, const ReqMsg& m, u16 character, u64 now)
{
	CCar* car = car_here(m.car);
	const Seated where = g.table.seat_of(character);
	SwitchFacts f;
	f.seated = where.car == m.car && car;
	const survival::Life life = survival::life_of(character);
	CAI_Stalker* body = body_here(character);
	f.alive = body && body->g_Alive() && (life == survival::Life::Alive || life == survival::Life::Wounded);
	f.wounded = life == survival::Life::Wounded;
	f.seats = car ? seat_count(car) : 0;
	f.from = where.seat;
	f.seat = m.seat;
	f.seat_free = m.seat < f.seats && g.table.at(m.car, m.seat) == kNoCharacter;
	f.since_last_ms = since_last(character, now);
	const Refusal rule = switch_rules(f);
	if (rule != Refusal::None)
		return refuse(from, m.car, m.seat, character, rule);
	if (!rate_ok(character, f.since_last_ms))
		return refuse(from, m.car, m.seat, character, Refusal::TooSoon);
	const TakeResult t = g.table.switch_seat(character, m.seat);
	if (t.reason != Refusal::None)
		return refuse(from, m.car, m.seat, character, t.reason);
	Sitter& s = g.sitters[character];
	if (where.seat == kDriverSeat)
	{
		g.drivers.erase(character);
		drive_off(*car);
	}
	s.seat = t.seat;
	if (t.seat == kDriverSeat)
	{
		drive_on(*car);
		Driver& d = g.drivers[character];
		d = Driver();
		d.track.start(car->ID(), u32(now));
	}
	park(character, s, car);
	++g.stats.granted;
	send_all(SeatMsg::granted(m.car, t.seat, character)); // a switch of seat is one granted: his old seat is free
}

void on_request(PeerId from, Reader& r)
{
	ReqMsg m;
	if (!read(r, m))
	{
		++g.stats.requests_bad;
		return;
	}
	if (!g.installed || !world_up())
		return;
	const u16 character = asker(from);
	if (character == kNoCharacter)
		return refuse(from, m.car, m.seat, kNoCharacter, Refusal::NoCharacter);
	const u64 now = now_ms();
	switch (m.op)
	{
	case ReqOp::Enter: on_enter(from, m, character, now); break;
	case ReqOp::Exit: on_exit(from, m, character, now); break;
	case ReqOp::SwitchSeat: on_switch(from, m, character, now); break;
	}
}

// ---------------------------------------------------------------- VehicleInput

void on_input(PeerId from, Reader& r)
{
	InputMsg m;
	if (!read(r, m))
	{
		++g.stats.inputs_bad;
		return;
	}
	if (!g.installed || !world_up())
		return;
	const u16 character = join::proxy_of_peer(from);
	auto it = character == kNoCharacter ? g.drivers.end() : g.drivers.find(character);
	if (it == g.drivers.end())
	{
		++g.stats.inputs_not_driving; // not a verdict: dropped, always (docs/VEHICLES.md §6)
		return;
	}
	const u64 now = now_ms();
	const bool admitted = it->second.gate.admit(u32(now));
	string256 text = "";
	if (!admitted)
	{
		++g.stats.inputs_over_rate;
		xr_sprintf(text, "character %u: driver input over %u a second (burst %u)", character, kInputRatePerSec, kInputBurst);
	}
	if (!trust(TrustInputRate, admitted, text))
		return; // "refuse": dropped; "log": applied, the line written
	const InputStep st = it->second.track.accept(m, u32(now));
	switch (st.drop)
	{
	case InputDrop::None: break;
	case InputDrop::Old: ++g.stats.inputs_old; return;
	case InputDrop::WrongCar: ++g.stats.inputs_wrong_car; return;
	case InputDrop::NotDriving:
	case InputDrop::Quiet: ++g.stats.inputs_not_driving; return;
	}
	CCar* car = car_here(it->second.track.car());
	if (!car)
		return; // gone: server_frame releases him
	apply_step(*car, st);
	++g.stats.inputs_applied;
	g.inputs.add(now);
}

// ---------------------------------------------------------------- the frame

// The decider's own actor's row: its car still has him (the hooks keep it; a path past them is caught here).
void check_own_rows()
{
	for (auto it = g.sitters.begin(); it != g.sitters.end();)
	{
		const u16 character = it->first;
		const Sitter s = it->second;
		++it;
		if (!s.own)
			continue;
		CCar* car = car_here(s.car);
		CActor* a = Actor();
		if (car && a && a->ID() == character && a->Holder() == car)
		{
			g.sitters[character].car_pos = car->Position();
			continue;
		}
		release(character, car ? Release::Exit : Release::CarGone);
	}
}

void release_by_facts()
{
	xr_vector<std::pair<u16, Release>> out;
	for (auto& it : g.sitters)
	{
		Sitter& s = it.second;
		if (s.own)
			continue;
		SitterFacts f;
		const PeerInfo* p = mgr().session().peer(s.peer);
		// VEH-DISC: the session's facts and the mode's answers about him (where he is; once his peer is gone, how it left)
		session_facts(f, mode(), s.peer, it.first, p && p->state == PeerState::Joined, join::proxy_of_peer(s.peer));
		CAI_Stalker* body = body_here(it.first);
		f.body_online = body != nullptr;
		const survival::Life life = survival::life_of(it.first);
		f.alive = body && body->g_Alive() && (life == survival::Life::Alive || life == survival::Life::Wounded);
		f.wounded = life == survival::Life::Wounded;
		CCar* car = car_here(s.car);
		f.car_online = car != nullptr;
		f.car_destroyed = car && car_destroyed(car);
		if (car)
			s.car_pos = car->Position();
		const Release why = release_reason(f);
		if (why != Release(0))
			out.push_back({it.first, why});
	}
	for (const auto& r : out)
		release(r.first, r.second);
}

void stale_inputs(u64 now)
{
	for (auto& it : g.drivers)
	{
		const InputStep st = it.second.track.tick(u32(now));
		if (!st.stale)
			continue;
		++g.stats.stale;
		if (CCar* car = car_here(it.second.track.car()))
			apply_step(*car, st);
	}
}

void park_all(u64 now)
{
	for (auto& it : g.sitters)
	{
		Sitter& s = it.second;
		if (s.own)
			continue;
		CCar* car = car_here(s.car);
		if (!car)
			continue;
		park(it.first, s, car);
		if (!s.exit_at || now - s.exit_at >= kExitEveryMs)
		{
			s.exit = exit_position(car, s);
			s.exit_at = now;
		}
	}
}

// A player who came to the map or finished loading it: a granted for every held seat, to him.
void arrivals()
{
	std::vector<std::pair<PeerId, bool>> peers;
	for (const PeerInfo& p : mgr().session().peers())
		peers.push_back({p.id, p.state == PeerState::Joined && mode().where(p.id) == Where::Here});
	for (const PeerId peer : g.arrivals.update(peers))
		for (const auto& it : g.sitters)
			send_to(peer, SeatMsg::granted(it.second.car, it.second.seat, it.first));
}
} // namespace

void install_server(Session& s)
{
	if (g.installed)
		drop_quietly(world_up() && join::world_generation() == g.world);
	g = State();
	for (u8 i = 0; i < TrustChecks; ++i)
		g.trust_mode[i] = read_trust_mode(kTrustNames[i]);
	g.world = join::world_generation();
	g.installed = true;
	s.on(Msg::VehicleReq, &on_request);
	s.on(Msg::VehicleInput, &on_input);
}

void server_frame()
{
	if (!g.installed)
		return;
	if (!mode().leads_level())
	{
		// this process leads no map (the world server: the map servers decide its players' cars): nothing is decided here. The
		// session's end never comes here (sync::frame stops with it): sync::reset() calls session_over()
		drop_quietly(true);
		g.installed = false;
		return;
	}
	const u32 world = join::world_generation();
	if (world != g.world)
	{
		// a load or a level change replaced the world: its players load the new one, where nobody sits
		drop_quietly(false);
		g.world = world;
		return;
	}
	const u64 now = now_ms();
	// a load gives the host's own actor his car back through the stock: his row comes back here (rv_F5-VEH-2 should-fix)
	if (CActor* a = Actor())
		if (CCar* held = smart_cast<CCar*>(a->Holder()))
			note_stock_owner(held);
	check_own_rows();
	release_by_facts();
	stale_inputs(now);
	park_all(now);
	arrivals();
}

bool seated(u16 character) { return g.installed && g.sitters.count(character) != 0; }

u16 driver_of(u16 car) { return g.installed ? g.table.driver_of(car) : kNoCharacter; }

bool seat_transform(CCar* car, u8 seat, Fmatrix& out)
{
	if (!car)
		return false;
	if (seat != kDriverSeat)
		return passenger_seat(car, seat, out); // F5-VEH-5: the passenger places of configs/coop_vehicles.ltx (coop_vehicles_ride.cpp)
	IKinematics* K = smart_cast<IKinematics*>(car->Visual());
	if (!K)
		return false;
	// as CCar::attach_Actor picks it (Car.cpp:794-804): car_definition driver_place of the model's user data, else the root
	CInifile* ini = K->LL_UserData();
	u16 bone = BI_NONE;
	if (ini && ini->line_exist("car_definition", "driver_place"))
		bone = K->LL_BoneID(ini->r_string("car_definition", "driver_place"));
	if (bone == BI_NONE)
		bone = K->LL_GetBoneRoot();
	// as CCar::VisualUpdate puts the Owner (Car.cpp:593): XFORM * the seat bone
	out.mul_43(car->XFORM(), K->LL_GetBoneInstance(bone).mTransform);
	return true;
}

bool save_position(u16 character, Fvector& out)
{
	if (!g.installed)
		return false;
	auto it = g.sitters.find(character);
	if (it == g.sitters.end() || it->second.own || !it->second.exit_at)
		return false; // the own actor: stock (m_holderID is saved, he is in his car after the load)
	out = it->second.exit;
	return true;
}

void own_actor_attached(CCar* car)
{
	if (!car || !decides_own_actor())
		return;
	const u16 own = own_character();
	const u16 driver = g.table.driver_of(car->ID());
	if (driver == own)
		return;
	if (driver != kNoCharacter)
		release(driver, Release::Script, true); // a script seated the own actor past the gate of Actor_Events.cpp: the client goes out
	if (g.table.seat_of(own).car != kNoCar)
		release(own, Release::Exit);
	const TakeResult t = g.table.take(car->ID(), seat_count(car), kDriverSeat, own);
	if (t.reason != Refusal::None)
	{
		::Msg("! [coop] vehicles: the own actor's seat in car %u not taken (%s)", car->ID(), refusal_name(t.reason));
		return;
	}
	seated_now(own, kInvalidPeer, car, t.seat, true);
}

void own_actor_detached(CCar* car)
{
	if (!car || !decides_own_actor())
		return;
	const u16 own = own_character();
	auto it = g.sitters.find(own);
	if (it == g.sitters.end() || !it->second.own || it->second.car != car->ID())
		return;
	release(own, car->b_exploded ? Release::CarDestroyed : (car->getDestroy() ? Release::CarGone : Release::Exit));
}

bool own_actor_may_attach(CCar* car)
{
	if (!car || !g.installed)
		return true;
	const u16 driver = g.table.driver_of(car->ID());
	if (driver == kNoCharacter || driver == own_character())
		return true;
	++g.stats.own_refused;
	return false;
}

// coop_vehicles.h: the co-op group's transition starts - every client's seat is released with `why` (Release::LeftLevel) before
// the movers load the next level; the own actor keeps the stock path. The caller in coop_join.cpp is the lead's to add.
void release_all_seats(Release why)
{
	if (!g.installed)
		return;
	xr_vector<u16> clients;
	for (const auto& it : g.sitters)
		if (!it.second.own)
			clients.push_back(it.first);
	for (const u16 character : clients)
		release(character, why);
}

// coop_sync.cpp reset() (Manager::stop, a lost session): server_frame runs no more once the phase is Off. The cars driven by
// clients are parked as detach_Actor parks them, the proxies stand up, the table is forgotten; nothing is sent.
void session_over()
{
	if (g.installed)
		drop_quietly(world_up() && join::world_generation() == g.world);
	g = State();
}

void print_status()
{
	if (!g.installed)
	{
		::Msg("[coop]   vehicles: no seat is decided here");
		return;
	}
	std::string rows;
	for (const auto& it : g.sitters)
		rows += " " + std::to_string(it.second.car) + "/" + std::to_string(it.second.seat) + "=" + std::to_string(it.first) +
		    (it.second.own ? "(own)" : "");
	std::string trust_text;
	for (u8 i = 0; i < TrustChecks; ++i)
		trust_text += std::string(i ? ", " : "") + kTrustNames[i] + " [" + trust_mode_name(g.trust_mode[i]) + "] " +
		    std::to_string(g.trust_count[i].failed) + "/" + std::to_string(g.trust_count[i].checked);
	const std::string counters = stats_text(g.stats, g.inputs.rate(now_ms()));
	::Msg("[coop]   vehicles: %u seated in %u car(s) [car/seat=character:%s], %u driven by clients; %s; trust: %s", u32(g.table.seated()),
	    u32(g.table.cars()), rows.empty() ? " none" : rows.c_str(), u32(g.drivers.size()), counters.c_str(), trust_text.c_str());
}
} // namespace vehicles
} // namespace coop
