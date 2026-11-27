// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players, passengers (docs/VEHICLES.md sections 4, 5 and 12).
//
// A stock car has the driver's place alone; the passenger places come from configs\coop_vehicles.ltx (a section per car section,
// the pure parser and transforms: xrCoop/coop_vehicle_seats.h). Two halves live here:
//   - the places (both the decider and a player's machine): the seat count of a car, the world transform of a seat >= 1, its door
//     point. The decider's seat table, proxies and releases are coop_vehicles_seat.cpp's (seat_count, seat_transform and
//     exit_position call these);
//   - the ride of a player's machine: VehicleSeat granted to me for a seat >= 1 (coop_vehicles_client.cpp, the Ride step, which keeps
//     the seat in its own record) starts it. My actor is not the copy's Owner: the stock CActor::attach_Vehicle lines for the actor
//     run (weapons hidden, the character controller destroyed, the driver's idle, actor_on_attach_vehicle for the mods), his XFORM
//     follows the copy's seat every frame, the first-person camera sits at the seat's eye with a free look within the stock car
//     camera's limits, the movement keys do nothing and the use key asks out (VehicleReq exit, as F in his car does for a driver).
//     The record of the seat gone (released, lost, the session's world gone), he stands up as CActor::detach_Vehicle stands the
//     driver up: at the decider's point when VehicleSeat released carried one (ride_exit_point), else at the door of
//     his seat, or the nearest free point beside the car, found as the decider finds it for his proxy (coop_vehicles_seat.cpp
//     exit_position), on this machine's copy. A ride keeps the world it sat in: of another world or of a
//     session that is over it answers no hook and is forgotten; sync::reset stands a seated one up (ride_session_over).
// Nothing here knows the mode: the requests go up through the client's record (RideLink, coop_vehicles.h) and the mode's delivery.
// No shooting from a passenger's seat. Main thread only.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_vehicles.h"
#include "coop_vehicles_ride.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_hud_text.h"
#include "coop_script.h" // actor_on_detach_vehicle when the copy is gone
#include "xrCoop/coop_vehicle_seats.h"
#include "xrCoop/coop_vehicle_drive_rules.h"

#include <map>
#include <string>

#include "../xrEngine/CameraBase.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../xrphysics/PhysicsShell.h"
#include "../xrphysics/ActorCameraCollision.h"
#include "Level.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "actor_anim_defs.h"
#include "Car.h"
#include "Torch.h"
#include "HudItem.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "script_callback_ex.h"
#include "Game_Object_Space.h"
#include "Script_Game_Object.h"
#include "xr_level_controller.h"
#include "clsid_game.h"

namespace coop
{
namespace vehicles
{
namespace
{
// The copy of the car granted is not here this long: he asks out (the request's answer timeout).
constexpr u32 kRideNoCarMs = kAnswerTimeoutMs;
// The stock box test refuses his standing up (ActivateBoxDynamic): again every frame, the log says so after this long ...
constexpr u32 kRideStuckSayMs = 1000;
// ... and after this long he stands up without it: his seat is no longer his, a passenger kept in it for good would be stranded.
constexpr u32 kRideForceOutMs = 3000;
// His standing point is kept fresh this often while he rides (the decider's kExitEveryMs).
constexpr u32 kRideExitEveryMs = 250;

// The seat this machine's actor rides in.
struct Ride
{
	// ride_begin: the seat granted, taken at the next frame
	bool due = false;
	u16 due_car = kNoCar; // host numbering
	u8 due_seat = kNoSeat;
	u32 due_since = 0;
	u32 due_world = 0; // join::world_generation() of the world the seat was granted in
	RideLink link;
	// seated
	bool on = false;
	u16 car = kNoCar; // host numbering
	u8 seat = kNoSeat;
	CActor* actor = nullptr; // the actor that sits (a new one after a load is another)
	u32 world = 0; // join::world_generation() of the world he sat in
	LookLimits limits;
	RideLook look;
	Fvector eye{}; // the eye in the seat's own space
	Fvector exit{}; // where he stands up
	u32 exit_at = 0;
	bool has_exit = false;
	bool decider_exit = false; // VehicleSeat released carried the decider's point (ride_exit_point): he stands there
	Fvector decider_at{};
	// standing up
	bool leaving = false;
	u32 leave_since = 0;
	bool leave_said = false;
};

struct Counters
{
	u64 rides = 0, seat_changes = 0, ends = 0, forced_out = 0, box_retries = 0, deaths = 0, no_car = 0, no_seat = 0, car_lost = 0;
	u64 asked_out = 0, exit_locked = 0, worlds_gone = 0, another_holder = 0;
	u64 decider_exits = 0, sessions_over = 0;
};

Ride g;
Counters g_c;

// configs\coop_vehicles.ltx, read once per process: the places of each car section, and the places that are left for a model once
// its bones are looked up ("<section>|<visual>").
bool g_places_read = false;
std::map<std::string, CarSeats> g_places;
std::map<std::string, u8> g_counted;

u32 now32() { return u32(mgr().now_ms()); }

const char* text_of(const shared_str& s) { return s.c_str() ? s.c_str() : ""; }

void read_places()
{
	if (g_places_read)
		return;
	g_places_read = true;
	string_path path;
	FS.update_path(path, "$game_config$", "coop_vehicles.ltx");
	if (!FS.exist(path))
	{
		::Msg("! [coop] configs\\coop_vehicles.ltx not found: every car has the driver's place alone");
		return;
	}
	const CInifile ini(path, TRUE, TRUE, FALSE);
	for (const CInifile::Sect& s : ini.sections())
	{
		const shared_str name = s.Name;
		const CarSeats seats = parse_car_seats([&ini, &name](const char* key, std::string& value) {
			if (!ini.line_exist(name.c_str(), key))
				return false;
			LPCSTR text = ini.r_string(name.c_str(), key);
			value = text ? text : "";
			return true;
		});
		for (const std::string& e : seats.errors)
			::Msg("! [coop] configs\\coop_vehicles.ltx [%s] %s", name.c_str(), e.c_str());
		g_places[name.c_str()] = seats;
	}
	::Msg("[coop] vehicles: the passenger places of %u car section(s) read", u32(g_places.size()));
}

const CarSeats* places_of(CCar* car)
{
	read_places();
	auto it = g_places.find(text_of(car->cNameSect()));
	return it == g_places.end() ? nullptr : &it->second;
}

SeatXform seat_xform(const Fmatrix& m)
{
	SeatXform r;
	r.i = seat_vec(m.i.x, m.i.y, m.i.z);
	r.j = seat_vec(m.j.x, m.j.y, m.j.z);
	r.k = seat_vec(m.k.x, m.k.y, m.k.z);
	r.c = seat_vec(m.c.x, m.c.y, m.c.z);
	return r;
}

Fmatrix fmatrix(const SeatXform& s)
{
	Fmatrix m;
	m.identity();
	m.i.set(s.i.x, s.i.y, s.i.z);
	m.j.set(s.j.x, s.j.y, s.j.z);
	m.k.set(s.k.x, s.k.y, s.k.z);
	m.c.set(s.c.x, s.c.y, s.c.z);
	return m;
}

// The bone of the driver's place as CCar::attach_Actor picks it (coop_vehicles_seat.cpp seat_transform): car_definition driver_place
// of the model's user data, else the root.
u16 driver_bone(IKinematics* K)
{
	CInifile* ini = K->LL_UserData();
	u16 bone = BI_NONE;
	if (ini && ini->line_exist("car_definition", "driver_place"))
		bone = K->LL_BoneID(ini->r_string("car_definition", "driver_place"));
	if (bone == BI_NONE)
		bone = K->LL_GetBoneRoot();
	return bone;
}

// The base of a seat in the car's space: its bone's model transform (`driver`: the driver's place), the identity for a point.
bool seat_base(IKinematics* K, const SeatPlace& p, SeatXform& out)
{
	out = SeatXform();
	if (p.bone.empty())
		return true;
	const u16 bone = p.bone == kDriverPlaceBone ? driver_bone(K) : K->LL_BoneID(p.bone.c_str());
	if (bone == BI_NONE)
		return false;
	out = seat_xform(K->LL_GetBoneInstance(bone).mTransform);
	return true;
}

// Seat `seat` (>= 1) of `car` in the car's space.
bool passenger_local(CCar* car, u8 seat, SeatXform& out)
{
	if (!car || seat == kDriverSeat || seat >= car_seat_count(car))
		return false;
	const CarSeats* s = places_of(car);
	IKinematics* K = smart_cast<IKinematics*>(car->Visual());
	SeatXform base;
	if (!s || !K || !seat_base(K, s->seat[seat], base))
		return false;
	out = seat_local(base, s->seat[seat]);
	return true;
}

Point3 p3(const Fvector& v)
{
	Point3 p;
	p.x = v.x;
	p.y = v.y;
	p.z = v.z;
	return p;
}

// The test of coop_vehicles_seat.cpp stand_point on this machine's copy (the same static world): ground under the point within 4 m,
// room for a standing body above it, no wall between it and the car. On success `p` is moved onto the ground.
bool stand_here(CCar* car, Fvector& p)
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

// Where he stands up: the door of his seat, else the nearest free point beside the car (exit_candidates), the door when none is
// free: what coop_vehicles_seat.cpp exit_position finds for his proxy, here on the copy.
Fvector exit_here(CCar* car, u8 seat)
{
	Fvector door = car->ExitPosition();
	passenger_exit(car, seat, door);
	const Fbox& box = car->BoundingBox();
	Fvector half, local_centre, centre;
	box.getradius(half);
	box.getcenter(local_centre);
	car->XFORM().transform_tiny(centre, local_centre);
	for (const Point3& c : exit_candidates(p3(door), p3(centre), p3(car->XFORM().i), p3(car->XFORM().k), half.x, half.z))
	{
		Fvector p = Fvector().set(c.x, c.y, c.z);
		if (stand_here(car, p))
			return p;
	}
	return door;
}

CCar* car_copy(u16 host_id)
{
	if (host_id == kNoCar || !g_pGameLevel)
		return nullptr;
	CObject* o = Level().Objects.net_Find(join::to_local(host_id));
	return o && !o->getDestroy() ? smart_cast<CCar*>(o) : nullptr;
}

// The stock car camera's limits (system.ltx [car_firsteye_cam], relative to the car); a pair of zeros is no limit, as CCameraBase
// clamps only when one of them is set.
LookLimits read_limits()
{
	LookLimits lim;
	const char* section = "car_firsteye_cam";
	if (!pSettings || !pSettings->section_exist(section))
		return lim;
	if (pSettings->line_exist(section, "lim_yaw"))
	{
		const Fvector2 y = pSettings->r_fvector2(section, "lim_yaw");
		const bool clamp_yaw = y.x != 0.f || y.y != 0.f;
		lim.yaw_lo = clamp_yaw ? y.x : -kSeatPi;
		lim.yaw_hi = clamp_yaw ? y.y : kSeatPi;
	}
	if (pSettings->line_exist(section, "lim_pitch"))
	{
		const Fvector2 p = pSettings->r_fvector2(section, "lim_pitch");
		const bool clamp_pitch = p.x != 0.f || p.y != 0.f;
		lim.pitch_lo = clamp_pitch ? p.x : -1.5f;
		lim.pitch_hi = clamp_pitch ? p.y : 1.5f;
	}
	return lim;
}

// The passenger's eye in his seat's space: the section's `eye`, else the driver's camera (car_definition camera_pos) over his seat.
Fvector eye_of(CCar* car)
{
	IKinematics* K = smart_cast<IKinematics*>(car->Visual());
	CInifile* ini = K ? K->LL_UserData() : nullptr;
	const bool has_camera = ini && ini->line_exist("car_definition", "camera_pos");
	const Fvector camera = has_camera ? ini->r_fvector3("car_definition", "camera_pos") : Fvector().set(0.f, 0.f, 0.f);
	const SeatXform driver = K ? seat_xform(K->LL_GetBoneInstance(driver_bone(K)).mTransform) : SeatXform();
	const SeatVec e = seat_eye(places_of(car), has_camera, seat_vec(camera.x, camera.y, camera.z), driver);
	return Fvector().set(e.x, e.y, e.z);
}

// The end of the stock F path (ActorInput.cpp CActor::use_Holder) after a successful use_HolderEx: the torch off on the way in, the
// active item's animation ended both ways.
void stock_use_tail(CActor* a, bool entered)
{
	if (entered)
	{
		CAttachableItem* I = a->attachedItem(CLSID_DEVICE_TORCH);
		if (I)
		{
			CTorch* torch = smart_cast<CTorch*>(I);
			if (torch)
				torch->Switch(false);
		}
	}
	if (a->inventory().ActiveItem())
	{
		CHudItem* hi = smart_cast<CHudItem*>(a->inventory().ActiveItem());
		if (hi)
			hi->OnAnimationEnd(hi->GetState());
	}
}

// CActor::attach_Vehicle (Actor.cpp), the lines after a successful attach of the Owner, for an actor who is not the copy's Owner: the
// holder pointer and its id stay empty, the decider's own-actor row is not his. The walking sway (pCamBobbing, a protected member)
// goes in the shedule hook of Actor.cpp while he rides.
void sit(CActor* a, CCar* car)
{
	a->inventory().SetPrevActiveSlot(a->inventory().GetActiveSlot());
	a->inventory().SetActiveSlot(NO_ACTIVE_SLOT);
	a->SetWeaponHideState(INV_STATE_BLOCK_ALL, true);

	// not stock: the controller's record keeps the velocity he walked with while the controller is gone, and coop_objects.cpp's push
	// check (client_push_check) reads it for an actor in no holder
	a->character_physics_support()->movement()->SetVelocity(0.f, 0.f, 0.f);
	a->character_physics_support()->movement()->DestroyCharacter();

	if (actor_camera_shell)
		destroy_physics_shell(actor_camera_shell);

	IKinematics* K = smart_cast<IKinematics*>(a->Visual());
	K->LL_GetBoneInstance(K->LL_BoneID("bip01_spine")).reset_callback();
	K->LL_GetBoneInstance(K->LL_BoneID("bip01_spine1")).reset_callback();
	K->LL_GetBoneInstance(K->LL_BoneID("bip01_spine2")).reset_callback();

	if (car->IsRemoteControl() == false)
	{
		u16 anim_type = car->DriverAnimationType();
		SVehicleAnimCollection& anims = a->m_vehicle_anims->m_vehicles_type_collections[anim_type];
		IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(a->Visual());
		R_ASSERT(V);
		V->PlayCycle(anims.idles[0], FALSE);
		a->CStepManager::on_animation_start(MotionID(), 0);
	}

	a->callback(GameObject::eAttachVehicle)(car->lua_game_object());
}

// CActor::detach_Vehicle (Actor.cpp) for an actor who is not the copy's Owner, standing at `at`. `car` null: the copy is gone (the
// stock's `if (pholder)`: no box test; actor_on_detach_vehicle with nil, see the end). False: the stock box test refused
// (`box_test`), he still sits.
bool stand_up(CActor* a, CCar* car, const Fvector& at, bool box_test)
{
	CPhysicsShell* shell = car ? car->PPhysicsShell() : nullptr;
	if (box_test && shell)
	{
		shell->SplitterHolderDeactivate();
		if (!a->character_physics_support()->movement()->ActivateBoxDynamic(0))
		{
			shell->SplitterHolderActivate();
			return false;
		}
		shell->SplitterHolderActivate();
	}

	a->SetWeaponHideState(INV_STATE_BLOCK_ALL, false);
	a->inventory().SetActiveSlot(a->inventory().GetPrevActiveSlot());
	a->inventory().SetPrevActiveSlot(NO_ACTIVE_SLOT);

	a->character_physics_support()->movement()->CreateCharacter();
	a->character_physics_support()->movement()->SetPosition(at);
	a->character_physics_support()->movement()->SetVelocity(car ? car->ExitVelocity() : Fvector().set(0.f, 0.f, 0.f));
	if (car)
		a->cam_Active()->Direction().set(Fvector().setHP(car->Direction().getH(), 0.0F));

	a->SetCallbacks();
	IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(a->Visual());
	R_ASSERT(V);
	V->PlayCycle(a->m_anims->m_normal.legs_idle);
	V->PlayCycle(a->m_anims->m_normal.m_torso_idle);

	if (car)
		a->callback(GameObject::eDetachVehicle)(car->lua_game_object());
	else // the copy is gone; the flags mods keep from the callback clear all the same (movement allowed, in a car). Straight to
		// SendScriptCallback with nil, not through the stock binder: its handler (bind_stalker_ext.actor_on_detach_vehicle) calls
		// obj:section() after it, a Lua error on nil
		script::queue_callback("actor_on_detach_vehicle", {script::cb_nil()});
	return true;
}

void forget()
{
	g = Ride();
}

// The seat is this world's. The stock actor's hooks run in every process and world, ride_frame only inside a
// session (sync::frame): a ride of another world (a load, a level change) or of a session that is over answers nothing.
bool on_here() { return g.on && g.world == join::world_generation(); }

void place(CActor* a, CCar* car)
{
	Fmatrix seat;
	if (passenger_seat(car, g.seat, seat))
		a->XFORM().set(seat); // as CCar::VisualUpdate puts the Owner: the car's XFORM times the seat
}

void refresh_exit(CCar* car, u32 now, bool now_please)
{
	if (!car || (!now_please && g.has_exit && u32(now - g.exit_at) < kRideExitEveryMs))
		return;
	g.exit = exit_here(car, g.seat);
	g.has_exit = true;
	g.exit_at = now;
}

// Out of the seat: at the decider's point when VehicleSeat released carried one (ride_exit_point), else at the copy's own (the door,
// the nearest free point). True: he stands; false: the stock box test refused (again next frame, without it after kRideForceOutMs;
// `force_now`: without it at once).
bool leave(CActor* a, CCar* car, u32 now, bool force_now)
{
	if (!g.leaving)
	{
		g.leaving = true;
		g.leave_since = now;
		g.leave_said = false;
	}
	if (!g.decider_exit)
		refresh_exit(car, now, true);
	const Fvector at = g.decider_exit ? g.decider_at : g.has_exit ? g.exit : a->Position();
	const bool force = force_now || u32(now - g.leave_since) >= kRideForceOutMs;
	if (!stand_up(a, car, at, !force))
	{
		++g_c.box_retries;
		if (!g.leave_said && u32(now - g.leave_since) >= kRideStuckSayMs)
		{
			g.leave_said = true;
			::Msg("! [coop] vehicle: the stock box test keeps the passenger in seat %u of car %u (no room for the actor's box): "
			      "out without it in %u ms",
			    u32(g.seat), u32(g.car), kRideForceOutMs);
		}
		return false;
	}
	if (force)
		++g_c.forced_out;
	if (g.decider_exit)
		++g_c.decider_exits;
	++g_c.ends;
	stock_use_tail(a, false);
	::Msg("[coop] vehicle: out of seat %u of car %u (a passenger) at %.2f,%.2f,%.2f", u32(g.seat), u32(g.car), at.x, at.y, at.z);
	// a seat granted meanwhile (another car) waits for the next frame
	Ride next;
	next.due = g.due;
	next.due_car = g.due_car;
	next.due_seat = g.due_seat;
	next.due_since = g.due_since;
	next.due_world = g.due_world;
	next.link = g.link;
	g = next;
	return true;
}

void begin(CActor* a, u32 now)
{
	const u16 car_id = g.due_car;
	const u8 seat = g.due_seat;
	if (!g.link.still_mine || !g.link.still_mine(car_id, seat) || !a->g_Alive())
	{
		g.due = false; // released or lost before he sat (the decider lets a dead one out itself): nothing to undo
		return;
	}
	CCar* car = car_copy(car_id);
	if (!car)
	{
		if (u32(now - g.due_since) >= kRideNoCarMs)
		{
			++g_c.no_car;
			g.due = false;
			::Msg("! [coop] vehicle: car %u of the passenger's seat %u is not here: asking out", u32(car_id), u32(seat));
			hud_text::say("st_coop_f5_vehicles_ride_no_car", "The vehicle is not here: you get out");
			if (g.link.ask_out)
				g.link.ask_out();
		}
		return;
	}
	Fmatrix m;
	if (!passenger_seat(car, seat, m))
	{
		++g_c.no_seat;
		g.due = false;
		::Msg("! [coop] vehicle: car %u has no seat %u here (configs\\coop_vehicles.ltx, its model): asking out", u32(car_id), u32(seat));
		hud_text::say("st_coop_f5_vehicles_ride_no_seat", "This seat is not known here: you get out");
		if (g.link.ask_out)
			g.link.ask_out();
		return;
	}
	if (a->Holder())
	{
		++g_c.another_holder;
		a->detach_Vehicle(true); // another holder here (a mounted gun, the copy he drove): the stock exit first
		if (a->Holder())
			return; // its box test refused: next frame
	}
	sit(a, car);
	stock_use_tail(a, true);
	// first person, the camera's free look of before ended as CActor::camUpdateFreelook ends it (eflDisabling's last state)
	if (a->active_cam() != eacFirstEye)
		a->cam_Set(eacFirstEye);
	if (a->cam_freelook != eflDisabled)
	{
		CCameraBase* first = a->cam_FirstEye();
		first->lim_yaw.set(0.f, 0.f);
		first->bClampYaw = false;
		first->lim_pitch.set(-1.5f, 1.5f);
		a->cam_freelook = eflDisabled;
	}
	const RideLink link = g.link;
	g = Ride();
	g.link = link;
	g.on = true;
	g.car = car_id;
	g.seat = seat;
	g.actor = a;
	g.world = join::world_generation();
	g.limits = read_limits();
	g.look = ride_look_start(g.limits);
	g.eye = eye_of(car);
	place(a, car);
	refresh_exit(car, now, true);
	++g_c.rides;
	::Msg("[coop] vehicle: in car %u seat %u (a passenger)", u32(car_id), u32(seat));
	hud_text::say("st_coop_f5_vehicles_ride_on", "You ride as a passenger: the use key gets you out");
}
} // namespace

// ---------------------------------------------------------------- the places (coop_vehicles_ride.h)

u8 car_seat_count(CCar* car)
{
	const CarSeats* s = car ? places_of(car) : nullptr;
	if (!s || s->count <= 1)
		return 1;
	IKinematics* K = smart_cast<IKinematics*>(car->Visual());
	if (!K)
		return 1;
	const std::string key = std::string(text_of(car->cNameSect())) + "|" + text_of(car->cNameVisual());
	auto it = g_counted.find(key);
	if (it != g_counted.end())
		return it->second;
	u8 count = 1;
	for (u8 i = 1; i < s->count; ++i)
	{
		SeatXform base;
		if (!seat_base(K, s->seat[i], base))
		{
			::Msg("! [coop] configs\\coop_vehicles.ltx [%s] seat_%u: the model %s has no bone %s: %u place(s)", text_of(car->cNameSect()),
			    u32(i), text_of(car->cNameVisual()), s->seat[i].bone.c_str(), u32(i));
			break;
		}
		count = u8(i + 1);
	}
	g_counted[key] = count;
	return count;
}

bool passenger_seat(CCar* car, u8 seat, Fmatrix& out)
{
	SeatXform local;
	if (!passenger_local(car, seat, local))
		return false;
	out.mul_43(car->XFORM(), fmatrix(local));
	return true;
}

bool passenger_exit(CCar* car, u8 seat, Fvector& out)
{
	SeatXform local;
	if (!passenger_local(car, seat, local))
		return false;
	const CarSeats* s = places_of(car);
	const Fbox& box = car->BoundingBox();
	const SeatVec p = seat_exit_local(seat_vec(box.x1, box.y1, box.z1), seat_vec(box.x2, box.y2, box.z2), local.c,
	    exit_side_of(s->seat[seat], local.c));
	car->XFORM().transform_tiny(out, Fvector().set(p.x, p.y, p.z));
	return true;
}

// ---------------------------------------------------------------- API (coop_vehicles.h)

void ride_begin(u16 car, u8 seat, const RideLink& link)
{
	g.due = true;
	g.due_car = car;
	g.due_seat = seat;
	g.due_since = now32();
	g.due_world = join::world_generation();
	g.link = link;
}

void ride_exit_point(u16 car, u8 seat, const Fvector& at)
{
	if (!on_here() || g.car != car || g.seat != seat)
		return; // not the seat he rides in here (one granted and not taken yet ends without standing up)
	g.decider_exit = true;
	g.decider_at = at;
}

void ride_session_over()
{
	// sync::reset (Manager::stop, a lost session, a rejoin, the group's transition): ride_frame runs no more. A live passenger still
	// seated in this world stands up here as the stock detach_Vehicle stands the driver up, without the box test (the seat is nobody's
	// now); a ride of another world, a dead one's or a seat not taken yet is only forgotten.
	if (on_here() && world_up())
	{
		CActor* a = Actor();
		if (a && a == g.actor && a->g_Alive())
		{
			++g_c.sessions_over;
			::Msg("[coop] vehicle: the session is over: the passenger stands up from seat %u of car %u", u32(g.seat), u32(g.car));
			leave(a, car_copy(g.car), now32(), true);
		}
	}
	forget();
}

void ride_frame()
{
	if (!g.on && !g.due)
		return;
	CActor* a = world_up() ? Actor() : nullptr;
	if (!a || (g.on && (a != g.actor || g.world != join::world_generation())) || (g.due && g.due_world != join::world_generation()))
	{
		// the world went (a level change, a load, the session's end): his actor went with it, nothing of the seat is left to undo
		++g_c.worlds_gone;
		forget();
		return;
	}
	const u32 now = now32();
	if (g.on)
	{
		CCar* car = car_copy(g.car);
		const bool mine = g.link.still_mine && g.link.still_mine(g.car, g.seat);
		const bool other_car = g.due && g.due_car != g.car;
		if (car && mine && !other_car && a->g_Alive() && !g.leaving)
		{
			if (g.due)
			{
				// another seat of the same car (a switch of seat): he moves over
				g.due = false;
				g.seat = g.due_seat;
				g.look = ride_look_start(g.limits);
				g.has_exit = false;
				g.decider_exit = false;
				++g_c.seat_changes;
				::Msg("[coop] vehicle: to seat %u of car %u (a passenger)", u32(g.seat), u32(g.car));
			}
			place(a, car);
			refresh_exit(car, now, false);
			return;
		}
		if (!car && mine && !g.leaving)
		{
			// the copy went from this machine while the decider still seats him: out here and out there
			++g_c.car_lost;
			::Msg("! [coop] vehicle: car %u of the passenger's seat is gone here: asking out", u32(g.car));
			if (g.link.ask_out)
				g.link.ask_out();
		}
		if (!leave(a, car, now, false))
		{
			if (car)
				place(a, car);
			return;
		}
	}
	if (g.due)
		begin(a, now);
}

bool riding() { return on_here(); }

// ---------------------------------------------------------------- the stock actor's hooks (coop_vehicles_ride.h)

bool ride_camera(CActor* actor, float fov)
{
	if (!on_here() || !actor || actor != g.actor)
		return false;
	CCar* car = car_copy(g.car);
	Fmatrix seat;
	if (!car || !passenger_seat(car, g.seat, seat))
		return false;
	actor->XFORM().set(seat);
	CCameraBase* C = actor->cam_Active();
	look_follow(g.look, C->yaw, C->pitch, g.limits);
	float yaw = 0.f, pitch = 0.f;
	look_world(seat_xform(seat), g.look, yaw, pitch);
	C->yaw = yaw;
	C->pitch = pitch;
	Fvector eye;
	seat.transform_tiny(eye, g.eye);
	Fvector dangle = {0.f, 0.f, 0.f};
	C->Update(eye, dangle);
	C->f_fov = fov;
	// the tail of CActor::cam_Update: the actor's camera manager, then the level's and the device for the viewed entity
	actor->Cameras().UpdateFromCamera(C);
	if (Level().CurrentEntity() == actor)
	{
		Level().Cameras().UpdateFromCamera(C);
		if (eacFirstEye == actor->active_cam() && !Level().Cameras().GetCamEffector(cefDemo) && !Device.m_SecondViewport.IsSVPActive())
			actor->Cameras().ApplyDevice(VIEWPORT_NEAR);
	}
	return true;
}

void ride_actor_died()
{
	if (!on_here())
		return;
	CActor* a = Actor();
	if (!a || a != g.actor)
		return;
	++g_c.deaths;
	leave(a, car_copy(g.car), now32(), false); // the decider lets him out itself (Death); a refused box test: ride_frame again
}

bool ride_key(int cmd)
{
	if (!on_here())
		return false;
	if (cmd == kUSE && !g.leaving)
	{
		CCar* car = car_copy(g.car);
		if (car && car->ExitLocked())
		{
			++g_c.exit_locked; // the stock detach_Vehicle(false) refuses it: nothing happens
			return true;
		}
		++g_c.asked_out;
		if (g.link.ask_out)
			g.link.ask_out();
	}
	return true; // nothing else while he rides: the stock driver's keys go to his car alone
}

bool ride_hold(int cmd)
{
	if (!on_here())
		return false;
	return !(cmd == kUP || cmd == kDOWN || cmd == kLEFT || cmd == kRIGHT);
}

void ride_print_status()
{
	if (on_here())
		::Msg("[coop]   vehicles here: a passenger in seat %u of car %u%s", u32(g.seat), u32(g.car), g.leaving ? " (standing up)" : "");
	::Msg("[coop]   vehicles here, rides: %llu, seat changes %llu, out %llu (at the decider's point %llu, without the box test %llu, box "
	      "retries %llu, dead %llu, session over %llu), car not here %llu, seat not known %llu, car gone %llu, asked out %llu, exit "
	      "locked %llu, worlds gone %llu, another holder first %llu; places of %u car section(s)",
	    g_c.rides, g_c.seat_changes, g_c.ends, g_c.decider_exits, g_c.forced_out, g_c.box_retries, g_c.deaths, g_c.sessions_over,
	    g_c.no_car, g_c.no_seat, g_c.car_lost, g_c.asked_out, g_c.exit_locked, g_c.worlds_gone, g_c.another_holder,
	    u32(g_places.size()));
}
} // namespace vehicles
} // namespace coop
