// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — vehicles for players, a player's machine (docs/VEHICLES.md sections 5, 7 and 14).
//
// The car of a player's level is the decider's: the co-op host's process, in the Server mode the map server of his level (the world
// server tunnels the four messages, Scope::Map). On a player's machine every car is a kinematic copy posed from the decider's
// snapshots (coop_objects.cpp). Here:
//   - F at a car (GEG_PLAYER_ATTACH_HOLDER of the own actor, coop_events.cpp) runs on the copy what the stock runs before
//     attach_Actor (Engaged, EnterLocked, the door test CCar::Use with the camera) and then asks the decider (VehicleReq enter, any
//     seat) instead of sitting; F in the car (GEG_PLAYER_DETACH_HOLDER) asks out (VehicleReq exit);
//   - VehicleSeat granted to me, seat 0: the stock CActor::attach_Vehicle(car, true) on the copy (camera, driver animation, the
//     callbacks actor_on_attach_vehicle of the mods), and the copy stays kinematic (Car.cpp: mirror_copy skips its physics parts);
//   - his keys on the copy (CarInput.cpp) become VehicleInput: at a change, every kInputRepeatMs while he drives; the camera keys stay;
//   - VehicleSeat released for me: the stock detach_Vehicle(true), then the actor stands where the decider says (as the stock puts him
//     at ExitPosition());
//   - a request with no answer for kAnswerTimeoutMs: a line on the HUD, the request forgotten;
//   - the copy he drives shows its latest pose extrapolated, not the 100 ms buffer (driving_car(), coop_objects.cpp).
// Nothing here knows the mode: the requests go up through the mode's delivery (net::client_send -> mode().up) and the answers come
// from whoever decides. VehicleState goes to state_on_state (coop_vehicles_state.cpp). Main thread only.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_vehicles.h"
#include "coop_vehicles_client.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_hud_text.h"
#include "coop_lua.h"
#include "coop_world.h"
#include "xrCoop/coop_vehicle_input_rules.h"
#include "xrCoop/coop_vehicle_state_rules.h" // TapLatch

#include <cstdlib>

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "../xrEngine/CameraBase.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xrphysics/PhysicsShell.h"
#include "Level.h"
#include "Actor.h"
#include "Car.h"
#include "Torch.h"
#include "HudItem.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "HUDManager.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "xr_level_controller.h"
#include "clsid_game.h"

// The key values of the pure rules are the engine's (test_vehicle_input.cpp reads the same enum from the source).
static_assert(coop::vehicles::keys::kLEFT == int(kLEFT) && coop::vehicles::keys::kRIGHT == int(kRIGHT), "EGameActions moved");
static_assert(coop::vehicles::keys::kUP == int(kUP) && coop::vehicles::keys::kDOWN == int(kDOWN), "EGameActions moved");
static_assert(coop::vehicles::keys::kJUMP == int(kJUMP) && coop::vehicles::keys::kCROUCH == int(kCROUCH), "EGameActions moved");
static_assert(coop::vehicles::keys::kACCEL == int(kACCEL) && coop::vehicles::keys::kFWD == int(kFWD), "EGameActions moved");
static_assert(coop::vehicles::keys::kBACK == int(kBACK) && coop::vehicles::keys::kL_STRAFE == int(kL_STRAFE), "EGameActions moved");
static_assert(coop::vehicles::keys::kR_STRAFE == int(kR_STRAFE) && coop::vehicles::keys::kCAM_1 == int(kCAM_1), "EGameActions moved");
static_assert(coop::vehicles::keys::kCAM_2 == int(kCAM_2) && coop::vehicles::keys::kCAM_3 == int(kCAM_3), "EGameActions moved");
static_assert(coop::vehicles::keys::kCAM_ZOOM_IN == int(kCAM_ZOOM_IN) && coop::vehicles::keys::kCAM_ZOOM_OUT == int(kCAM_ZOOM_OUT),
    "EGameActions moved");
static_assert(coop::vehicles::keys::kTORCH == int(kTORCH) && coop::vehicles::keys::kDETECTOR == int(kDETECTOR), "EGameActions moved");
static_assert(coop::vehicles::keys::kWPN_FIRE == int(kWPN_FIRE) && coop::vehicles::keys::kWPN_ZOOM == int(kWPN_ZOOM), "EGameActions moved");
static_assert(coop::vehicles::keys::kWPN_FUNC == int(kWPN_FUNC) && coop::vehicles::keys::kUSE == int(kUSE), "EGameActions moved");

namespace coop
{
namespace vehicles
{
namespace
{
// The stock exit (CActor::detach_Vehicle) refuses while the actor's box does not fit where he sits (ActivateBoxDynamic): it is tried
// again every frame; the log says so once after this long.
constexpr u32 kExitStuckMs = 1000;
// coop_vehicle_drive: the longest a stand may hold keys with one command.
constexpr u32 kDriveMaxMs = 60000;

// The seat the decider gave this machine's actor.
struct Mine
{
	u16 car = kNoCar; // host numbering
	u8 seat = kNoSeat;
	bool attach_due = false; // granted seat 0: the attach waits for the frame
	bool attached = false; // the local actor sits in the copy (the stock attach_Vehicle went through)
	bool leave_due = false; // released (or lost): out, at `exit` when `has_exit`
	bool has_exit = false;
	Fvector exit{};
	Release why = Release::Exit;
	u32 leave_since = 0;
	bool leave_said = false;
	bool exit_wanted = false; // a VehicleReq exit waits for its slot (RequestTrack)
};

struct Counters
{
	u64 enters_sent = 0, exits_sent = 0, granted = 0, refused = 0, released = 0, timeouts = 0, inputs = 0;
	u64 doors = 0, locked_here = 0, too_soon_here = 0, bad_seat = 0, attach_failed = 0, lost = 0, passenger_out = 0;
	u64 out_of_path = 0, stray = 0, exit_retries = 0, states = 0;
	u64 by_refusal[u8(Refusal::Last) + 1] = {};
	u64 by_release[u8(Release::Last) + 1] = {};
};

SeatTable g_table; // the mirror of the decider's table (VehicleSeat)
RequestTrack g_req;
InputSender g_input;
TapLatch g_tap; // a held key tapped between two VehicleInput messages is held until one is taken
Mine g_mine;
Counters g_c;
u32 g_world = 0; // join::world_generation() of the world the mirror and his own seat belong to

// The decider forgets its table at a world change (a load, a level change) without a released, and arrivals
// send the sitters of the new world only: a row of the old world kept another player seated here (drawn in the car, his snapshots
// skipped). A new world forgets the mirror, the request, the input, the latch and his own seat.
void client_world()
{
	const u32 w = join::world_generation();
	if (w == g_world)
		return;
	g_world = w;
	g_table.clear();
	g_req.reset();
	g_input.stop();
	g_tap.reset();
	g_mine = Mine();
}

// The HUD line of a refusal, by Refusal (literal ids: tests/tools/test_strings.py finds them).
struct Text
{
	const char* id;
	const char* fallback;
};
const Text kRefusalText[u8(Refusal::Last) + 1] = {
	{"st_coop_f5_vehicles_refused", "Cannot get into the vehicle now"},
	{"st_coop_f5_vehicles_refused_no_car", "This vehicle is not here any more"},
	{"st_coop_f5_vehicles_refused_unsupported", "This vehicle cannot be driven in co-op"},
	{"st_coop_f5_vehicles_refused_destroyed", "The vehicle is wrecked"},
	{"st_coop_f5_vehicles_refused_dead", "You are dead"},
	{"st_coop_f5_vehicles_refused_wounded", "Wounded: you cannot get in"},
	{"st_coop_f5_vehicles_refused_already_seated", "You already sit in a vehicle"},
	{"st_coop_f5_vehicles_refused_carrying", "Put the body down first"},
	{"st_coop_f5_vehicles_refused_locked", "The vehicle is locked"},
	{"st_coop_f5_vehicles_refused_no_such_seat", "The vehicle has no such seat"},
	{"st_coop_f5_vehicles_refused_seat_taken", "Somebody sits there"},
	{"st_coop_f5_vehicles_refused_full", "The vehicle is full"},
	{"st_coop_f5_vehicles_refused_not_seated", "You do not sit in this vehicle"},
	{"st_coop_f5_vehicles_refused_exit_locked", "The doors are locked"},
	{"st_coop_f5_vehicles_refused_no_character", "You are not on this level yet"},
	{"st_coop_f5_vehicles_refused_too_soon", "Not so fast"},
	{"st_coop_f5_vehicles_refused_too_far", "The vehicle is too far away"},
};

u32 now32() { return u32(mgr().now_ms()); }

CCar* find_car(u16 host_id) { return host_id == kNoCar ? nullptr : smart_cast<CCar*>(world::object_here(join::to_local(host_id))); }

u16 host_id_of(const CGameObject* go) { return go ? join::to_host(u16(go->ID())) : kNoCar; }

// A player's machine in the world: requests go out, answers are acted on.
bool here_live() { return is_live_mirror() && world_up(); }

void send_req(ReqOp op, u16 car, u8 seat, u32 now)
{
	ReqMsg m;
	m.op = op;
	m.car = car;
	m.seat = seat;
	Writer w(8);
	write(w, m);
	net::client_send(Msg::VehicleReq, w);
	g_req.sent(op, car, now);
	if (op == ReqOp::Exit)
		++g_c.exits_sent;
	else
		++g_c.enters_sent;
	::Msg("[coop] vehicle: asked %s, car %u seat %u", op_name(op), u32(car), u32(seat));
}

bool enter(u16 car, u8 seat, u32 now)
{
	if (g_mine.car != kNoCar)
		return true; // one seat: he sits (or a granted is on its way); F in his own car asks out instead
	if (g_req.pending())
		return true; // one request at a time: the answer comes, or the timeout
	if (!g_req.may_send(now))
	{
		++g_c.too_soon_here; // quicker than kRequestMinIntervalMs: the decider's trust check would only write it
		return true;
	}
	send_req(ReqOp::Enter, car, seat, now);
	return true;
}

// He asks out of the seat the decider gave him: now, or at the next free slot of RequestTrack (client_frame).
void want_exit(u32 now)
{
	if (g_mine.car == kNoCar)
		return;
	if (g_req.pending() && g_req.op() == ReqOp::Exit)
		return;
	if (g_req.may_send(now))
	{
		send_req(ReqOp::Exit, g_mine.car, kAnySeat, now);
		g_mine.exit_wanted = false;
	}
	else
		g_mine.exit_wanted = true;
}

// The bone the stock attach_Actor seats the owner on (Car.cpp: car_definition driver_place, else the root): m_sits_transforms[0].
bool driver_place(CCar* car, Fmatrix& out)
{
	IKinematics* K = smart_cast<IKinematics*>(car->Visual());
	CInifile* ini = K ? K->LL_UserData() : nullptr;
	if (!K)
		return false;
	u16 id = K->LL_GetBoneRoot();
	if (ini && ini->line_exist("car_definition", "driver_place"))
		id = K->LL_BoneID(ini->r_string("car_definition", "driver_place"));
	if (id == BI_NONE)
		return false;
	out.set(K->LL_GetBoneInstance(id).mTransform);
	return true;
}

// The weapon's fire point of the copy as CCarWeapon::UpdateBarrelDir takes it (the fire bone of mounted_weapon_definition in the car's
// space, times the car's XFORM). The copy's weapon is never switched on here, so its own m_fire_pos is never updated.
bool fire_point(CCar* car, Fvector& out)
{
	IKinematics* K = smart_cast<IKinematics*>(car->Visual());
	CInifile* ini = K ? K->LL_UserData() : nullptr;
	if (!ini || !ini->line_exist("mounted_weapon_definition", "fire_bone"))
		return false;
	const u16 bone = K->LL_BoneID(ini->r_string("mounted_weapon_definition", "fire_bone"));
	if (bone == BI_NONE)
		return false;
	Fmatrix m;
	m.set(K->LL_GetTransform(bone));
	m.mulA_43(car->XFORM());
	out.set(0.f, 0.f, 0.f);
	m.transform_tiny(out);
	return true;
}

// The end of the stock F path after a successful use_HolderEx (ActorInput.cpp CActor::use_Holder): the torch off on the way in, the
// active item's animation ended both ways.
void stock_use_tail(CActor* actor, bool entered)
{
	if (entered)
	{
		CAttachableItem* I = actor->attachedItem(CLSID_DEVICE_TORCH);
		if (I)
		{
			CTorch* torch = smart_cast<CTorch*>(I);
			if (torch)
				torch->Switch(false);
		}
	}
	if (actor->inventory().ActiveItem())
	{
		CHudItem* hi = smart_cast<CHudItem*>(actor->inventory().ActiveItem());
		if (hi)
			hi->OnAnimationEnd(hi->GetState());
	}
}

void say_release(Release why)
{
	switch (why)
	{
	case Release::Wounded:
		hud_text::say("st_coop_f5_vehicles_released_wounded", "Wounded: you fell out of the vehicle");
		break;
	case Release::LeftLevel:
		hud_text::say("st_coop_f5_vehicles_released_left_level", "Out of the vehicle: the group leaves the level");
		break;
	case Release::CarDestroyed:
		hud_text::say("st_coop_f5_vehicles_released_car_destroyed", "The vehicle is wrecked");
		break;
	case Release::CarGone: hud_text::say("st_coop_f5_vehicles_released_car_gone", "The vehicle is gone"); break;
	case Release::Script: hud_text::say("st_coop_f5_vehicles_released_script", "You were taken out of the vehicle"); break;
	default: break; // he asked, he died, he left: nothing to say
	}
}

// ---------------------------------------------------------------- VehicleSeat

void client_on_seat(PeerId, Reader& r)
{
	SeatMsg m;
	if (!read(r, m))
	{
		if (++g_c.bad_seat <= 5)
			::Msg("! [coop] vehicle: a VehicleSeat that does not read was dropped");
		return;
	}
	if (!is_mirror())
		return; // the co-op host player's link: his own actor drives by the stock path (coop_vehicles_seat.cpp records it)
	client_world(); // a row of another world is no row of this one
	g_table.apply(m);
	const u16 me = join::my_proxy();
	const bool answered = g_req.answer(m, me);
	Seated mine;
	mine.car = g_mine.car;
	mine.seat = g_mine.seat;
	mine.character = me;
	switch (seat_step(m, me, mine))
	{
	case SeatStep::Refused:
		if (!answered)
			break;
		++g_c.refused;
		++g_c.by_refusal[m.reason];
		::Msg("[coop] vehicle: car %u refused (%s)", u32(m.car), refusal_name(m.refusal()));
		if (m.refusal() == Refusal::NotSeated && g_mine.car == m.car)
		{
			// the decider has him in no seat of it (a release crossed his request): out here, at the copy's own exit
			g_mine.exit_wanted = false;
			if (g_mine.attached || g_mine.attach_due)
			{
				g_mine.leave_due = true;
				g_mine.has_exit = false;
				g_mine.why = Release::Exit;
				g_mine.leave_since = now32();
			}
			else
				g_mine = Mine();
			break;
		}
		hud_text::say(kRefusalText[m.reason].id, kRefusalText[m.reason].fallback);
		break;
	case SeatStep::Attach:
		++g_c.granted;
		if (g_mine.car != kNoCar && g_mine.car != m.car && g_mine.attached)
			::Msg("! [coop] vehicle: granted car %u while in car %u: out of that one here first", u32(m.car), u32(g_mine.car));
		g_mine = Mine();
		g_mine.car = m.car;
		g_mine.seat = m.seat;
		g_mine.attach_due = true;
		::Msg("[coop] vehicle: car %u seat %u granted", u32(m.car), u32(m.seat));
		break;
	case SeatStep::Ride:
		++g_c.granted;
		if (g_mine.attached || g_mine.attach_due)
		{
			// from the driver's seat to a passenger's (a switch of seat, which this machine never asks for in 58): out of the copy here
			// first, then out of the seat
			++g_c.passenger_out;
			g_mine.leave_due = true;
			g_mine.has_exit = false;
			g_mine.leave_since = now32();
			g_mine.car = m.car;
			g_mine.seat = m.seat;
			::Msg("! [coop] vehicle: seat %u of car %u granted while in the driver's seat here: asking out", u32(m.seat), u32(m.car));
			want_exit(now32());
			break;
		}
		// A passenger's seat. The record stays this machine's (coop.vehicle.status, a release or a loss ends it); the ride
		// (coop_vehicles_ride.cpp) sits him there, follows the record and asks out through it.
		g_mine = Mine();
		g_mine.car = m.car;
		g_mine.seat = m.seat;
		::Msg("[coop] vehicle: car %u seat %u granted (a passenger)", u32(m.car), u32(m.seat));
		{
			RideLink link;
			link.still_mine = [](u16 car, u8 seat) { return g_mine.car == car && g_mine.seat == seat && !g_mine.leave_due; };
			link.ask_out = [] { want_exit(now32()); };
			ride_begin(m.car, m.seat, link);
		}
		break;
	case SeatStep::Leave:
		++g_c.released;
		++g_c.by_release[m.reason];
		if (g_mine.car != m.car)
		{
			// not the seat this machine has him in (out of it here already, or never in it): nothing to undo here
			::Msg("[coop] vehicle: out of car %u seat %u (%s), not sitting in it here", u32(m.car), u32(m.seat), release_name(m.release()));
			break;
		}
		g_mine.leave_due = true;
		g_mine.has_exit = true;
		g_mine.exit.set(m.exit[0], m.exit[1], m.exit[2]);
		g_mine.why = m.release();
		g_mine.leave_since = now32();
		g_mine.exit_wanted = false;
		if (g_mine.seat != kDriverSeat)
			ride_exit_point(m.car, m.seat, g_mine.exit); // a passenger stands where the decider stood him
		::Msg("[coop] vehicle: out of car %u seat %u (%s) at %.2f,%.2f,%.2f", u32(m.car), u32(m.seat), release_name(m.release()),
		    m.exit[0], m.exit[1], m.exit[2]);
		break;
	case SeatStep::Lost:
		if (g_mine.leave_due)
			break; // already released by the decider: keep its exit position and reason
		++g_c.lost;
		g_mine.leave_due = true; // the decider has another character there: out here, at the copy's own exit
		g_mine.has_exit = false;
		g_mine.why = Release::Script;
		g_mine.leave_since = now32();
		g_mine.exit_wanted = false;
		::Msg("! [coop] vehicle: car %u seat %u is somebody else's by the decider: out here", u32(m.car), u32(m.seat));
		break;
	default: break;
	}
}

// ---------------------------------------------------------------- the frame steps

void step_leave(CActor* actor, u32 now)
{
	if (!g_mine.leave_due)
		return;
	// Out by the decider's word: no VehicleInput while the stock exit below waits for room (the decider has
	// him in no seat and drops it as NotDriving)
	g_input.stop();
	CCar* car = find_car(g_mine.car);
	if (car && actor->Holder() == car)
	{
		actor->detach_Vehicle(true); // the stock exit; Car.cpp detach_Actor leaves the copy kinematic (mirror_copy)
		if (actor->Holder() == car)
		{
			// the stock box test refused (ActivateBoxDynamic): again next frame, as a stock F would be pressed again
			++g_c.exit_retries;
			if (!g_mine.leave_said && u32(now - g_mine.leave_since) >= kExitStuckMs)
			{
				g_mine.leave_said = true;
				::Msg("! [coop] vehicle: the stock exit from car %u refuses (no room for the actor's box), tried every frame",
				    u32(g_mine.car));
			}
			return;
		}
		stock_use_tail(actor, false);
	}
	// the stock detach put him at the copy's ExitPosition(); the decider's exit position is where his character stands now
	if (g_mine.has_exit && g_mine.attached && actor->g_Alive() && actor->character_physics_support() &&
	    actor->character_physics_support()->movement())
		actor->character_physics_support()->movement()->SetPosition(g_mine.exit);
	say_release(g_mine.why);
	g_input.stop();
	const bool give_back = g_mine.seat != kDriverSeat && g_mine.exit_wanted; // a passenger's seat waits to be given back
	const u16 car_id = g_mine.car;
	const u8 seat = g_mine.seat;
	g_mine = Mine();
	if (give_back)
	{
		g_mine.car = car_id;
		g_mine.seat = seat;
		want_exit(now);
	}
}

void step_attach(CActor* actor, u32 now)
{
	if (!g_mine.attach_due)
		return;
	CCar* car = find_car(g_mine.car);
	if (!car || !actor->g_Alive())
	{
		++g_c.attach_failed;
		::Msg("! [coop] vehicle: granted car %u is not here (or the actor is dead): asking out", u32(g_mine.car));
		g_mine.attach_due = false;
		want_exit(now);
		return;
	}
	CHolderCustom* held = actor->Holder();
	if (held && held != smart_cast<CHolderCustom*>(car))
	{
		actor->detach_Vehicle(true); // another holder here (a mounted gun, the copy of another car): the stock exit first
		if (actor->Holder())
			return; // its box test refused: next frame
	}
	if (actor->Holder() != smart_cast<CHolderCustom*>(car))
	{
		// bForce: EnterLocked and the door test ran before the request (client_on_use); attach_Actor keeps the copy kinematic
		if (!actor->attach_Vehicle(car, true) || actor->Holder() != smart_cast<CHolderCustom*>(car))
		{
			++g_c.attach_failed;
			::Msg("! [coop] vehicle: the stock attach to car %u refused (an owner, or the copy is wrecked): asking out",
			    u32(g_mine.car));
			g_mine.attach_due = false;
			want_exit(now);
			return;
		}
		stock_use_tail(actor, true);
	}
	// the stock VisualUpdate puts the owner at his seat only while the shell is enabled; a parked copy sleeps
	Fmatrix seat;
	if (driver_place(car, seat))
		actor->XFORM().mul_43(car->XFORM(), seat);
	g_mine.attach_due = false;
	g_mine.attached = true;
	g_input.start(g_mine.car, car->HasWeapon());
	g_tap.reset();
	::Msg("[coop] vehicle: in car %u seat %u", u32(g_mine.car), u32(g_mine.seat));
}

void step_watch(CActor* actor, u32 now)
{
	if (g_mine.attached && !g_mine.leave_due)
	{
		CCar* car = find_car(g_mine.car);
		if (!car || actor->Holder() != smart_cast<CHolderCustom*>(car))
		{
			// out here outside this path: a script's detach_vehicle (ui_sr_teleport), the stock exit of a copy that exploded or went
			// (CarExplode, net_Destroy), a death. The decider still has him in the seat: he asks out.
			++g_c.out_of_path;
			g_input.stop();
			g_mine.attached = false;
			if (!actor->g_Alive())
				return; // a death: the decider releases him itself (Death)
			::Msg("[coop] vehicle: out of car %u on this machine (not by the decider): asking out", u32(g_mine.car));
			want_exit(now);
			return;
		}
		Fmatrix seat;
		if (!car->PPhysicsShell() || !car->PPhysicsShell()->isEnabled())
			if (driver_place(car, seat))
				actor->XFORM().mul_43(car->XFORM(), seat);
		return;
	}
	if (g_mine.car == kNoCar && !g_mine.attach_due && smart_cast<CCar*>(actor->Holder()))
	{
		// a car nobody gave him (the stock on_requested_spawn of a save's holder, a mod's attach on this machine): the copy is
		// kinematic, the decider never seated him
		++g_c.stray;
		::Msg("! [coop] vehicle: the actor sat in a car here without the decider: out (the stock exit)");
		actor->detach_Vehicle(true);
	}
}

void step_input(u32 now)
{
	if (!g_input.driving() || !g_input.due(now))
		return;
	const InputMsg m = g_input.take(now);
	// The keys tapped before this message went out with it pressed; their release goes in the next one
	for (int held_key : g_tap.taken())
		g_input.release(held_key);
	Writer w(16);
	write(w, m);
	net::client_send(Msg::VehicleInput, w, Lane::Unreliable);
	++g_c.inputs;
}

// A held key (a gear, the brake, fire, a drive key) pressed and released before its press went out in a
// VehicleInput (a quick tap at a low frame rate: both in one frame) is held again until the next message is taken (step_input), so
// the decider sees the press, then the release. `before`: the sender's held bits before this key.
void latch_tap(int key, bool pressed, u16 before)
{
	const KeyRule* r = key_rule(key);
	if (!r || r->use != KeyUse::Held)
		return;
	if (pressed)
		g_tap.pressed(key, u16(g_input.held() & ~before));
	else if (!g_tap.release_now(key, r->held))
		g_input.press(key);
}

// ---------------------------------------------------------------- Lua: coop.vehicle

// The grant (car in local numbering) and the client counters, even when there is no grant.
int l_status(lua_State* L)
{
	if (!is_mirror() || g_mine.car == kNoCar || g_mine.leave_due)
		lua_pushnil(L);
	else
	{
		lua_createtable(L, 0, 2);
		lua_pushnumber(L, lua_Number(join::to_local(g_mine.car)));
		lua_setfield(L, -2, "car");
		lua_pushnumber(L, lua_Number(g_mine.seat));
		lua_setfield(L, -2, "seat");
	}
	lua_createtable(L, 0, 2);
	lua_pushnumber(L, lua_Number(g_c.enters_sent + g_c.exits_sent));
	lua_setfield(L, -2, "requests");
	lua_pushnumber(L, lua_Number(g_c.refused));
	lua_setfield(L, -2, "refused");
	return 2;
}

int l_attached(lua_State* L)
{
	lua_pushboolean(L, local_seat_attached());
	return 1;
}

// coop.vehicle.exit() -> bool: asks out of that seat (true: asked now or at the next slot; false: he sits in none).
int l_exit(lua_State* L)
{
	const bool seated = is_mirror() && g_mine.car != kNoCar && !g_mine.leave_due;
	if (seated)
		want_exit(now32());
	lua_pushboolean(L, seated ? 1 : 0);
	return 1;
}

// ---------------------------------------------------------------- the stand's console

void status_line()
{
	Fvector p{};
	if (world_up() && Actor())
		p = Actor()->Position();
	if (g_mine.car != kNoCar && !g_mine.leave_due)
		::Msg("[coop] vehicle: car %u seat %u pos %.2f,%.2f,%.2f", u32(g_mine.car), u32(g_mine.seat), p.x, p.y, p.z);
	else
		::Msg("[coop] vehicle: car none seat none pos %.2f,%.2f,%.2f", p.x, p.y, p.z);
}

// The stand drives through these on a player's machine in the world. No debug gate: the server's (coop_debug) answers by the mode, which
// the Core does not ask (check_layers L1); the commands give nothing F and the keys do not (enter: a car within kEnterMaxDistance).
bool stand_allowed(const char* command)
{
	if (!here_live())
	{
		::Msg("! [coop] %s: a player's machine in the world only", command);
		return false;
	}
	return true;
}

const char* skip_spaces(const char* s)
{
	while (s && (*s == ' ' || *s == '\t'))
		++s;
	return s;
}

// A whole number in [0, max] at `s` (decimal, 0x hex with `base` 0); false: none, or out of range.
bool read_number(const char*& s, unsigned long max, int base, unsigned long& out)
{
	s = skip_spaces(s);
	if (!s || !*s)
		return false;
	char* end = nullptr;
	const unsigned long v = std::strtoul(s, &end, base);
	if (end == s || v > max || (*end && *end != ' ' && *end != '\t'))
		return false;
	s = end;
	out = v;
	return true;
}

CCar* nearest_car()
{
	CActor* a = Actor();
	CCar* best = nullptr;
	float best_d = flt_max;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CCar* car = smart_cast<CCar*>(Level().Objects.o_get_by_iterator(i));
		if (!car || car->getDestroy())
			continue;
		const float d = car->Position().distance_to(a->Position());
		if (d < best_d)
		{
			best_d = d;
			best = car;
		}
	}
	return best;
}

class CCC_VehicleEnter : public IConsole_Command
{
public:
	CCC_VehicleEnter(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		if (!stand_allowed("coop_vehicle_enter"))
			return;
		const char* s = skip_spaces(args);
		CCar* car = nullptr;
		if (s && !strncmp(s, "nearest", 7))
		{
			car = nearest_car();
			s += 7;
		}
		else
		{
			unsigned long id = 0;
			if (!read_number(s, 0xfffe, 10, id))
			{
				::Msg("! [coop] coop_vehicle_enter <id (host numbering) | nearest> [seat 0..%u]", u32(kMaxSeats - 1));
				return;
			}
			car = find_car(u16(id));
		}
		unsigned long seat = kAnySeat;
		const char* rest = skip_spaces(s);
		if (rest && *rest && !read_number(s, kMaxSeats - 1, 10, seat))
		{
			::Msg("! [coop] coop_vehicle_enter: a seat is 0..%u", u32(kMaxSeats - 1));
			return;
		}
		if (!car)
		{
			::Msg("! [coop] coop_vehicle_enter: no such car here");
			return;
		}
		if (!(car->Position().distance_to(Actor()->Position()) <= kEnterMaxDistance))
		{
			::Msg("! [coop] coop_vehicle_enter: the car is farther than %.0f m", kEnterMaxDistance);
			return;
		}
		if (g_mine.car != kNoCar || g_req.pending() || !g_req.may_send(now32()))
		{
			::Msg("! [coop] coop_vehicle_enter: seated, or a request waits");
			return;
		}
		enter(host_id_of(car), u8(seat), now32());
	}
	void Info(TInfo& I) override { xr_strcpy(I, "co-op stand: ask the decider for a seat: <car id | nearest> [seat]"); }
};

class CCC_VehicleDrive : public IConsole_Command
{
public:
	CCC_VehicleDrive(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		if (!stand_allowed("coop_vehicle_drive"))
			return;
		const char* s = args;
		unsigned long bits = 0, ms = 0;
		if (!read_number(s, 0xffff, 0, bits) || !read_number(s, kDriveMaxMs, 10, ms))
		{
			::Msg("! [coop] coop_vehicle_drive <held bits (VehicleInput, 0x..)> <ms up to %u>", kDriveMaxMs);
			return;
		}
		if (!g_input.driving())
		{
			::Msg("! [coop] coop_vehicle_drive: this player drives no car");
			return;
		}
		g_input.force(u16(bits), now32(), u32(ms));
		::Msg("[coop] vehicle: drive 0x%04x for %u ms", u32(g_input.held_at(now32())), u32(ms));
	}
	void Info(TInfo& I) override { xr_strcpy(I, "co-op stand: hold these VehicleInput bits instead of the keys: <bits> <ms>"); }
};

class CCC_VehicleExit : public IConsole_Command
{
public:
	CCC_VehicleExit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		if (!stand_allowed("coop_vehicle_exit"))
			return;
		if (g_mine.car == kNoCar || g_mine.leave_due)
		{
			::Msg("! [coop] coop_vehicle_exit: this player sits in no car");
			return;
		}
		want_exit(now32());
	}
	void Info(TInfo& I) override { xr_strcpy(I, "co-op stand: ask the decider out of the car"); }
};

class CCC_VehicleStatus : public IConsole_Command
{
public:
	CCC_VehicleStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		status_line();
		const char* s = skip_spaces(args);
		if (s && !strncmp(s, "full", 4))
			client_print_status();
	}
	void Info(TInfo& I) override { xr_strcpy(I, "co-op: this player's car and seat [full: the counters]"); }
};
} // namespace

// ---------------------------------------------------------------- API (coop_vehicles.h)

bool local_seat_attached()
{
	if (!is_mirror() || !world_up())
		return false;
	CActor* actor = Actor();
	if (!actor)
		return false;
	if (riding())
		return true;
	CCar* car = find_car(g_mine.car);
	return car && actor->Holder() == car;
}

void install_client(Endpoint& e)
{
	g_table.clear();
	g_req.reset();
	g_input.stop();
	g_tap.reset();
	g_mine = Mine();
	g_world = join::world_generation(); // the session starts in this world
	g_c = Counters();
	e.on(Msg::VehicleSeat, &client_on_seat);
	e.on(Msg::VehicleState, [](PeerId, Reader& r) {
		if (!is_mirror())
			return; // the co-op host player's link: his cars are the real ones
		++g_c.states;
		state_on_state(r);
	});
}

void client_frame()
{
	if (!is_mirror() || !world_up())
		return; // the decider's own process and a server: their actors drive by the stock path or never
	client_world(); // what was this machine's in another world is forgotten before anything acts on it
	CActor* actor = Actor();
	if (!actor)
		return;
	const u32 now = now32();
	if (g_req.timed_out(now))
	{
		++g_c.timeouts;
		::Msg("[coop] vehicle: no answer to %s of car %u in %u ms: forgotten", op_name(g_req.op()), u32(g_req.car()),
		    kAnswerTimeoutMs);
		// One line for both modes (a co-op player has no map server)
		hud_text::say("st_coop_f5_vehstate_no_answer", "No answer about the vehicle: try again in a moment");
	}
	step_leave(actor, now);
	step_attach(actor, now);
	step_watch(actor, now);
	if (g_mine.exit_wanted && !g_mine.leave_due)
		want_exit(now);
	step_input(now);
}

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"status", &l_status},
		{"attached", &l_attached},
		{"exit", &l_exit},
		{nullptr, nullptr},
	};
	lua_api::fill(L, "vehicle", fns); // without the table coop: coop_script logs it
}

void register_console()
{
	CMD1(CCC_VehicleEnter, "coop_vehicle_enter");
	CMD1(CCC_VehicleDrive, "coop_vehicle_drive");
	CMD1(CCC_VehicleExit, "coop_vehicle_exit");
	CMD1(CCC_VehicleStatus, "coop_vehicle_status");
}

bool mirror_copy(const CCar* car) { return car && is_mirror(); }

bool client_on_use(CGameObject* go)
{
	CCar* car = smart_cast<CCar*>(go);
	if (!car || !here_live())
		return false;
	CActor* actor = Actor();
	if (!actor)
		return false;
	const u32 now = now32();
	const u16 id = host_id_of(car);
	if (g_mine.leave_due)
		return true; // on his way out already
	// in it: F asks out (also LevelChangeCars' press of USE before a level change)
	if (actor->Holder() == smart_cast<CHolderCustom*>(car) || (g_mine.car != kNoCar && g_mine.car == id))
	{
		if (car->ExitLocked())
		{
			++g_c.locked_here; // the stock detach_Vehicle(false) refuses it: nothing happens
			return true;
		}
		want_exit(now);
		return true;
	}
	// the stock path up to attach_Actor, on the copy: Actor_Events.cpp `if (!holder->Engaged())`, then CActor::attach_Vehicle(car, false):
	// EnterLocked, then CCar::Use with the camera (the first F at a closed door opens it, the next one enters)
	if (car->Engaged())
		return true;
	if (car->EnterLocked())
	{
		++g_c.locked_here;
		return true;
	}
	Fvector center;
	actor->Center(center);
	if (!car->Use(Device.vCameraPosition, Device.vCameraDirection, center))
	{
		++g_c.doors;
		return true;
	}
	return enter(id, kAnySeat, now);
}

u16 driving_car() { return g_input.driving() && g_mine.attached ? join::to_local(g_input.car()) : kNoCar; }

// ---------------------------------------------------------------- API (coop_vehicles_client.h)

bool client_key(CCar* car, int key, bool pressed)
{
	if (!mirror_copy(car) || key_use(key) == KeyUse::Local)
		return false;
	if (!g_input.driving() || g_input.car() != host_id_of(car))
		return true; // a copy is never driven here, and no input goes out for a car he does not drive
	const u16 before = g_input.held();
	const bool took = pressed ? g_input.press(key) : g_input.release(key);
	latch_tap(key, pressed, before); // a tap is not lost
	// CarInput.cpp: after Press*/Release* of a steering key the stock turns the owner's hands; the owner of the copy is this machine's actor
	if (key == kL_STRAFE || key == kR_STRAFE)
		if (CActor* a = Actor())
			if (a->Holder() == smart_cast<CHolderCustom*>(car))
				a->steer_Vehicle(!pressed ? 0.f : (key == kR_STRAFE ? 1.f : -1.f));
	return took;
}

void client_mouse_aim(CCar* car)
{
	if (!mirror_copy(car) || !g_input.driving() || !g_input.has_weapon() || g_input.car() != host_id_of(car))
		return;
	if (!(g_input.held() & kHeldWeapon))
		return; // the stock aims only while the weapon is on (m_car_weapon->IsActive())
	CCameraBase* cam = car->Camera();
	if (!cam)
		return;
	// CarInput.cpp OnMouseMove: the point the camera looks at, then CCarWeapon::SetParam(eWpnDesiredPos): from the fire point to it
	Fvector pos = cam->Position();
	Fvector cam_dir = cam->Direction();
	collide::rq_result& RQ = HUD().GetRQ();
	pos.mad(cam_dir, RQ.range > 3.f ? RQ.range : 30.f);
	Fvector fire;
	if (!fire_point(car, fire))
		return;
	Fvector dir;
	dir.sub(pos, fire).normalize_safe();
	float h = 0.f, p = 0.f;
	dir.getHP(h, p);
	g_input.set_aim(h, p); // the decider: SetParam(eWpnDesiredDir, Fvector2(h, p)) = m_destEnemyDir.setHP(h, p)
}

bool mirror_seat_of(u16 character, Seated& out)
{
	// The dolls of sitters (coop_vehicles_state.cpp); the mirror is filled on a player's machine only (client_on_seat)
	out = g_table.seat_of(character);
	return out.car != kNoCar;
}

bool client_request_enter(CGameObject* go)
{
	CCar* car = smart_cast<CCar*>(go);
	if (!car || !here_live())
		return false;
	return enter(host_id_of(car), kAnySeat, now32());
}

void client_print_status()
{
	::Msg("[coop]   vehicles here: asked %llu in / %llu out, granted %llu, refused %llu, released %llu, no answer %llu, inputs %llu, "
	      "states %llu; F at a door %llu, locked here %llu, too soon here %llu",
	    g_c.enters_sent, g_c.exits_sent, g_c.granted, g_c.refused, g_c.released, g_c.timeouts, g_c.inputs, g_c.states, g_c.doors,
	    g_c.locked_here, g_c.too_soon_here);
	::Msg("[coop]   vehicles here: attach failed %llu, seat lost %llu, passenger seats given back %llu, out not by the decider %llu, "
	      "sat without the decider %llu, exit retries %llu, bad VehicleSeat %llu; seat car %u seat %u attached %u, %u sitters known",
	    g_c.attach_failed, g_c.lost, g_c.passenger_out, g_c.out_of_path, g_c.stray, g_c.exit_retries, g_c.bad_seat,
	    u32(g_mine.car), u32(g_mine.seat), g_mine.attached ? 1u : 0u, u32(g_table.seated()));
	for (u8 i = 1; i <= u8(Refusal::Last); ++i)
		if (g_c.by_refusal[i])
			::Msg("[coop]   vehicles here: refused %s %llu", refusal_name(Refusal(i)), g_c.by_refusal[i]);
	for (u8 i = 1; i <= u8(Release::Last); ++i)
		if (g_c.by_release[i])
			::Msg("[coop]   vehicles here: released %s %llu", release_name(Release(i)), g_c.by_release[i]);
}
} // namespace vehicles
} // namespace coop
