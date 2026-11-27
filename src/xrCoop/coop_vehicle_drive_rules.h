// Anomaly Coop — vehicles for players (docs/VEHICLES.md §7-§10): the decider's pure half.
//
// The decider of a player's level (the co-op host; in the Server mode the map server of his level) seats players in cars,
// drives a client's car without an Owner and lets him out. What it does is the same in both modes; these rules do not know
// which mode runs:
//   - the driver's held keys and press counters (InputStep, coop_vehicle_rules.h) -> the ordered stock calls of
//     CCar::OnKeyboardPress / OnKeyboardRelease (CarInput.cpp:154-257) and the weapon's aim (plan_drive);
//   - what CCar::attach_Actor / detach_Actor (Car.cpp:762-831) do to the car, line by line, kept or dropped for a driver who
//     is not the car's Owner (kAttachLines / kDetachLines; coop_vehicles_seat.cpp runs the kept ones in this order);
//   - the checks of the client's word (coop_trust, section [coop_trust] of mod_system_coop_trust.ltx): log | refuse | off,
//     counted and logged as coop_trust.script pass() does (trust_pass);
//   - why a seat is released by the decider itself (release_reason over session_facts: the session's facts and the mode's
//     answers about the player, how his peer left among them, VEH-DISC), the points beside a car where a released sitter may
//     stand (exit_candidates), who came to the map and gets the seat table (Arrivals), the coop_status counters.
//
// Pure C++17, header-only, no engine dependencies: used by xrGame/coop/coop_vehicles_seat.cpp and by the unit-test exe
// (tests/test_vehicle_drive_rules.cpp, tests/test_vehicle_departure.cpp).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coop_mode.h"
#include "coop_protocol.h"
#include "coop_text.h"
#include "coop_vehicle_rules.h"

namespace coop
{
namespace vehicles
{
// ---------------------------------------------------------------- the driver's keys -> the stock calls

// What the decider calls on the car. Each key is the command CCar::OnKeyboardPress / OnKeyboardRelease takes
// (xr_level_controller.h): Forward kFWD, Back kBACK, Left kL_STRAFE, Right kR_STRAFE, Brake kJUMP, TransUp kACCEL,
// TransDown kCROUCH, Fire kWPN_FIRE, WeaponSwitch kWPN_FUNC, Engine kDETECTOR, Lights kTORCH. Aim is not a key: the
// weapon's SetParam(CCarWeapon::eWpnDesiredDir, Fvector2(yaw, pitch)) (CarWeapon.cpp:294-301).
enum class DriveKey : uint8_t
{
	Forward,
	Back,
	Left,
	Right,
	Brake,
	TransUp,
	TransDown,
	Fire,
	WeaponSwitch,
	Engine,
	Lights,
	Aim,
};

struct DriveCall
{
	DriveKey key = DriveKey::Forward;
	bool press = true; // OnKeyboardPress; false: OnKeyboardRelease (Aim: always true)

	bool operator==(const DriveCall& o) const { return key == o.key && press == o.press; }
	bool operator!=(const DriveCall& o) const { return !(*this == o); }
};

struct HeldKey
{
	uint16_t bit;
	DriveKey key;
};

// The releases, in the order of the cases of CCar::OnKeyboardRelease (CarInput.cpp:239-256): kACCEL (no call), kFWD, kBACK,
// kL_STRAFE, kR_STRAFE, kJUMP, kWPN_FIRE; kCROUCH has no case there (it calls nothing), it comes last.
constexpr HeldKey kReleaseOrder[] = {
	{kHeldTransUp, DriveKey::TransUp}, {kHeldForward, DriveKey::Forward}, {kHeldBack, DriveKey::Back},
	{kHeldLeft, DriveKey::Left}, {kHeldRight, DriveKey::Right}, {kHeldBrake, DriveKey::Brake},
	{kHeldFire, DriveKey::Fire}, {kHeldTransDown, DriveKey::TransDown},
};

// The presses of held keys, in the order of the cases of CCar::OnKeyboardPress (CarInput.cpp:180-214): kACCEL, kCROUCH,
// kFWD, kBACK, kR_STRAFE, kL_STRAFE, kJUMP, (kDETECTOR), kWPN_FUNC, (kTORCH), kWPN_FIRE. The weapon switch (kWPN_FUNC) is not
// a held key: plan_drive puts it between kJUMP and kWPN_FIRE, where its case is.
constexpr HeldKey kPressOrder[] = {
	{kHeldTransUp, DriveKey::TransUp}, {kHeldTransDown, DriveKey::TransDown}, {kHeldForward, DriveKey::Forward},
	{kHeldBack, DriveKey::Back}, {kHeldRight, DriveKey::Right}, {kHeldLeft, DriveKey::Left}, {kHeldBrake, DriveKey::Brake},
};

// The stock calls of one InputStep (InputTrack::accept or ::tick), in this order:
//   1. the released keys (kReleaseOrder): a key the driver let go ends before anything new starts, as on a keyboard;
//   2. the engine switches (SwitchEngine per press, kDETECTOR), then the head lights (SwitchHeadLights, kTORCH): before the
//      new presses, because a PressForward with the engine off drives nothing (Car.cpp:1193 Drive() returns while
//      !b_engine_on): "engine on + forward" in one message moves the car as the two presses of a player do;
//   3. the pressed keys (kPressOrder), the weapon switch (kWPN_FUNC) where its case is, then kWPN_FIRE: the weapon is on
//      before it fires;
//   4. the aim, when the message has one and the car has a weapon.
// Only edges are called: a key held before and after calls nothing (stock calls a key once per press and once per release;
// a ReleaseForward of a key that is not held is not a no-op, Car.cpp:1476-1489). The weapon switch is pressed when the
// driver's switch bit changed and the car's weapon (`weapon_active`, CCarWeapon::IsActive) is not in the new position;
// never without a weapon (`has_weapon`, CCar::HasWeapon: stock kWPN_FUNC does nothing then). The order of two keys of one
// message is the decider's (a client frame's DirectInput order is not on the wire: docs/VEHICLES.md §7).
inline std::vector<DriveCall> plan_drive(const InputStep& s, bool has_weapon, bool weapon_active)
{
	std::vector<DriveCall> out;
	if (!s.applied())
		return out;
	const uint16_t before = uint16_t(s.before & kHeldState);
	const uint16_t held = uint16_t(s.held & kHeldState);
	for (const HeldKey& k : kReleaseOrder)
		if ((before & k.bit) && !(held & k.bit))
			out.push_back({k.key, false});
	for (uint8_t i = 0; i < s.engine_toggles; ++i)
		out.push_back({DriveKey::Engine, true});
	for (uint8_t i = 0; i < s.lights_toggles; ++i)
		out.push_back({DriveKey::Lights, true});
	for (const HeldKey& k : kPressOrder)
		if (!(before & k.bit) && (held & k.bit))
			out.push_back({k.key, true});
	const bool switch_was = (before & kHeldWeapon) != 0;
	const bool switch_now = (held & kHeldWeapon) != 0;
	if (has_weapon && switch_was != switch_now && weapon_active != switch_now)
		out.push_back({DriveKey::WeaponSwitch, true});
	if (!(before & kHeldFire) && (held & kHeldFire))
		out.push_back({DriveKey::Fire, true});
	if (s.aim && has_weapon)
		out.push_back({DriveKey::Aim, true});
	return out;
}

// ---------------------------------------------------------------- attach / detach of a driver who is not the Owner

// What the decider does to a car when a client takes or leaves seat 0. Every line of CCar::attach_Actor / detach_Actor
// (Car.cpp) with kept / dropped and why; coop_vehicles_seat.cpp runs the kept steps in this order.
enum class CarStep : uint8_t
{
	None, // a dropped line
	EnablePhysics, // PPhysicsShell()->Enable()
	AddObstacleCallback, // PPhysicsShell()->add_ObjectContactCallback(ActorObstacleCallback)
	ProcessingActivate, // processing_activate()
	ReleaseHandBreak, // ReleaseHandBreak()
	RemoveObstacleCallback, // PPhysicsShell()->remove_ObjectContactCallback(ActorObstacleCallback)
	NeutralDrive, // NeutralDrive()
	Unclutch, // Unclutch()
	ResetKeys, // ResetKeys()
	RpmToMin, // m_current_rpm = m_min_rpm
	HandBreak, // HandBreak()
	ProcessingDeactivate, // processing_deactivate()
};

struct StockLine
{
	uint16_t line; // Car.cpp
	const char* code;
	CarStep step; // CarStep::None: dropped
	const char* why;
};

constexpr StockLine kAttachLines[] = {
	{791, "if (Owner() || CPHDestroyable::Destroyed()) return false", CarStep::None,
	    "the verdict: seat 0 by the seat table (the decider's own actor is in it at his stock attach), Destroyed"},
	{792, "CHolderCustom::attach_Actor(actor)", CarStep::None,
	    "no Owner: the proxy is a CAI_Stalker, whose attach_Holder takes a machine gun only (ai_stalker.cpp:1720-1748)"},
	{797, "driver_place bone of car_definition, else the root; Owner()->setVisible(0) without the bone", CarStep::None,
	    "seat_transform(car, 0) picks the same bone; the proxy is hidden only when the model has no driver_place"},
	{805, "m_sits_transforms.push_back(instance.mTransform)", CarStep::None,
	    "seat_transform reads the bone every frame (stock: the vector grows at each attach, [0] is read)"},
	{807, "OnCameraChange(ectFirst)", CarStep::None, "no camera: the client's camera is his copy's"},
	{808, "PPhysicsShell()->Enable()", CarStep::EnablePhysics, "wakes the car's physics"},
	{809, "PPhysicsShell()->add_ObjectContactCallback(ActorObstacleCallback)", CarStep::AddObstacleCallback,
	    "the car stops at the actor-obstacle materials, else a client drives through the invisible walls of the map's edge"},
	{811, "processing_activate()", CarStep::ProcessingActivate, "the car updates (paired with processing_deactivate)"},
	{812, "ReleaseHandBreak()", CarStep::ReleaseHandBreak, "lets the wheels go"},
	{824, "m_car_drone->attach_Actor(actor); m_visual_camera.attach_Actor(actor)", CarStep::None,
	    "a drone or remote-control car is refused (Unsupported); the visual camera is the Owner's"},
};

constexpr StockLine kDetachLines[] = {
	{764, "if (!Owner()) return", CarStep::None, "the caller: seat 0 was a client's"},
	{765, "Owner()->setVisible(1)", CarStep::None, "the proxy is shown when it leaves its seat (unpark)"},
	{766, "CHolderCustom::detach_Actor()", CarStep::None, "no Owner"},
	{767, "PPhysicsShell()->remove_ObjectContactCallback(ActorObstacleCallback)", CarStep::RemoveObstacleCallback, ""},
	{768, "NeutralDrive()", CarStep::NeutralDrive, ""},
	{769, "Unclutch()", CarStep::Unclutch, ""},
	{770, "ResetKeys()", CarStep::ResetKeys, "the keys are reset without the Release* calls, as in stock"},
	{771, "m_current_rpm = m_min_rpm", CarStep::RpmToMin, ""},
	{775, "HandBreak()", CarStep::HandBreak, "a car left by its driver stands"},
	{776, "processing_deactivate()", CarStep::ProcessingDeactivate, ""},
	{778, "DBgClearPlots()", CarStep::None, "DEBUG builds only"},
	{783, "m_car_drone->detach_Actor(); m_visual_camera.detach_Actor()", CarStep::None, "as at attach"},
};

// ---------------------------------------------------------------- checks of the client's word (coop_trust)

// A new check of the decider (this unit): the door test of CCar::Enter with the body's head and look (Car.cpp:861-875), when
// it can be told here (a door whose state the decider knows: no joint, opened or broken; a jointed door opens only on the
// player's copy, docs/VEHICLES.md §18). Refused as TooFar ("not at a door of the car"). Permanent: a config key.
constexpr const char* kTrustEnterDoor = "veh_enter_door";
constexpr const char* kTrustSection = "coop_trust";
constexpr const char* kTrustDefaultKey = "default";

enum class TrustMode : uint8_t
{
	Log, // write what would be refused, let it through
	Refuse,
	Off, // the check does not run
};

inline const char* trust_mode_name(TrustMode m)
{
	switch (m)
	{
	case TrustMode::Log: return "log";
	case TrustMode::Refuse: return "refuse";
	case TrustMode::Off: return "off";
	}
	return "?";
}

// "log" | "refuse" | "off" (the values coop_trust.script accepts); anything else leaves `out` and returns false.
inline bool parse_trust_mode(const char* text, TrustMode& out)
{
	if (!text)
		return false;
	if (ascii::equal(text, "log"))
		out = TrustMode::Log;
	else if (ascii::equal(text, "refuse"))
		out = TrustMode::Refuse;
	else if (ascii::equal(text, "off"))
		out = TrustMode::Off;
	else
		return false;
	return true;
}

struct TrustCount
{
	uint32_t checked = 0;
	uint32_t failed = 0;
	uint32_t refused = 0;
};

struct TrustResult
{
	bool pass = true; // let the request through
	bool log = false; // write the "[coop-trust] ! <name>: ..." line now
};

// Lines per check before only every 64th failure is written (coop_trust.script LOG_FIRST).
constexpr uint32_t kTrustLogFirst = 8;
constexpr uint32_t kTrustLogEvery = 64;

// coop_trust.script pass(): off -> through, nothing counted; else checked; ok -> through; a failure is counted, refused in
// "refuse", logged for the first kTrustLogFirst failures and every kTrustLogEvery-th after.
inline TrustResult trust_pass(TrustMode mode, bool ok, TrustCount& c)
{
	TrustResult r;
	if (mode == TrustMode::Off)
		return r;
	++c.checked;
	if (ok)
		return r;
	++c.failed;
	const bool refuse = mode == TrustMode::Refuse;
	if (refuse)
		++c.refused;
	r.log = c.failed <= kTrustLogFirst || c.failed % kTrustLogEvery == 0;
	r.pass = !refuse;
	return r;
}

// The verdicts of coop_vehicle_rules.h with their checks of the client's word passed: what the game rules alone say. The
// decider refuses on these; the trust checks run after it, one by one, each through trust_pass (in "log" each writes its own
// line, so a "log" build says everything a "refuse" build would refuse).
inline Refusal enter_rules(EnterFacts f)
{
	f.since_last_ms = UINT32_MAX;
	f.distance = 0.f;
	return enter(f);
}

inline Refusal exit_rules(ExitFacts f)
{
	f.since_last_ms = UINT32_MAX;
	return exit(f);
}

inline Refusal switch_rules(SwitchFacts f)
{
	f.since_last_ms = UINT32_MAX;
	return switch_to(f);
}

// ---------------------------------------------------------------- releases the decider makes itself

// What the decider sees of one seated client each frame (docs/VEHICLES.md §9). The decider's own actor is not here: its
// stock attach / detach hooks keep its row.
struct SitterFacts
{
	bool in_session = true; // his peer is still a joined peer of the session that seated him
	bool same_character = true; // his peer still plays the character that sits (join::proxy_of_peer)
	Departure gone = Departure::Disconnected; // how his peer left the session: mode departure(peer, character), read when not in it
	bool on_map = true; // his character is on this map: mode where(peer) answers Here (asked whether he is in the session or not)
	bool body_online = true; // his character's object is online here
	bool alive = true; // not dead, not out of the run, not respawning
	bool wounded = false; // coop_survival: down
	bool car_online = true; // the car's object is online here
	bool car_destroyed = false; // dead, exploded (b_exploded) or CPHDestroyable::Destroyed
};

// The release of a seated player whose player plays another character now, or loads the world again for one (the co-op host's
// rejoin, coop_join.cpp host_rejoin_peer; a map server's LevelMap that names him on its level with another character or none):
// LeftLevel while the mode does not place him on this map (he loads it), else Disconnect. Its own case, kept as it was before
// VEH-DISC.
inline Release replaced_release(bool on_map) { return on_map ? Release::Disconnect : Release::LeftLevel; }

// The release of a seated player whose peer left the session, by the mode's fact of how (Mode::departure; docs/VEHICLES.md §9):
// he quit the world - Disconnect; he went to another map of it - LeftLevel; another character of his took his place - as above.
inline Release departure_release(Departure gone, bool on_map)
{
	switch (gone)
	{
	case Departure::Disconnected: return Release::Disconnect;
	case Departure::LeftLevel: return Release::LeftLevel;
	case Departure::Replaced: break;
	}
	return replaced_release(on_map);
}

// Release(0): he stays. The first that holds, in this order: Disconnect / LeftLevel, CarGone, CarDestroyed, Death, Wounded
// (the car's end is told before what it did to him: an explosion sets b_exploded in the frame it hits; its blast comes
// later, Explosive.cpp GenExplodeEvent). A player gone from the session: how he left, as the mode says (not LeftLevel for a
// map server's drop of the virtual peer of a player who quit). In the session with another
// character: replaced_release. In the session, not on this map: LeftLevel (the co-op: he loads the world again).
inline Release release_reason(const SitterFacts& f)
{
	if (!f.in_session)
		return departure_release(f.gone, f.on_map);
	if (!f.same_character)
		return replaced_release(f.on_map);
	if (!f.on_map)
		return Release::LeftLevel;
	if (!f.car_online)
		return Release::CarGone;
	if (f.car_destroyed)
		return Release::CarDestroyed;
	if (!f.body_online || !f.alive)
		return Release::Death;
	if (f.wounded)
		return Release::Wounded;
	return Release(0);
}

// The facts the session and the mode give of one seated player (release_by_facts, coop_vehicles_seat.cpp): `joined` his peer is a
// joined peer of the session that seated him, `plays` the character that peer plays now (kNoId: none), `character` the one that
// sits. The mode is asked about the player only: is he on this map, and, once his peer is gone, how it left.
inline void session_facts(SitterFacts& f, const Mode& m, PeerId peer, uint16_t character, bool joined, uint16_t plays)
{
	f.in_session = joined;
	f.same_character = plays == character;
	f.on_map = m.where(peer) == Where::Here;
	f.gone = joined ? Departure::Disconnected : m.departure(peer, character);
}

// ---------------------------------------------------------------- where a released sitter stands

struct Point3
{
	float x = 0.f;
	float y = 0.f;
	float z = 0.f;
};

// Beyond the car's half width / half length: a standing body (a stalker's capsule is ~0.35 m wide) and a step.
constexpr float kExitClearance = 1.f;
// The stock exit point, four sides, four corners.
constexpr size_t kExitCandidates = 9;

// The points where a released sitter may stand, the first free one wins (the decider tests each: ground under it, room for a
// standing body, no wall between it and the car). First the stock exit point (CCar::ExitPosition: the exit of the first door,
// the car's position without doors; Car.cpp:2423-2428, what CActor::detach_Vehicle uses), then the points beside the car at
// kExitClearance beyond its box, the ones nearest to the stock point first (the stock refuses an exit when the body does not
// fit, Actor.cpp:1572-1576; the decider looks for the nearest free point instead). `right` and `forward` are the car's unit
// axes (XFORM i, k), `centre` its box centre; y of the side points is the centre's (the decider finds the ground).
inline std::vector<Point3> exit_candidates(const Point3& stock, const Point3& centre, const Point3& right, const Point3& forward,
    float half_width, float half_length)
{
	std::vector<Point3> sides;
	const float w = half_width + kExitClearance;
	const float l = half_length + kExitClearance;
	const float steps[8][2] = {{1.f, 0.f}, {-1.f, 0.f}, {0.f, -1.f}, {0.f, 1.f}, {1.f, -1.f}, {-1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
	for (const auto& s : steps)
	{
		Point3 p;
		p.x = centre.x + right.x * w * s[0] + forward.x * l * s[1];
		p.y = centre.y;
		p.z = centre.z + right.z * w * s[0] + forward.z * l * s[1];
		sides.push_back(p);
	}
	auto flat2 = [&stock](const Point3& p)
	{
		const float dx = p.x - stock.x;
		const float dz = p.z - stock.z;
		return dx * dx + dz * dz;
	};
	std::stable_sort(sides.begin(), sides.end(), [&flat2](const Point3& a, const Point3& b) { return flat2(a) < flat2(b); });
	std::vector<Point3> out;
	out.reserve(kExitCandidates);
	out.push_back(stock);
	out.insert(out.end(), sides.begin(), sides.end());
	return out;
}

// ---------------------------------------------------------------- the seat table for a player who comes to the map

// The decider sends a player who comes to its map, or finishes loading it, a VehicleSeat granted for every held seat (the
// world gate drops the decider's VehicleSeat on a player whose world is not up: coop_world_gate.h, test_protocol_f5.cpp).
// Every frame it lists the session's peers with "on this map now"; update() returns the ones that are now and were not at
// the last call, and forgets the ones that are gone (one who leaves and comes back is listed again).
class Arrivals
{
public:
	std::vector<PeerId> update(const std::vector<std::pair<PeerId, bool>>& peers)
	{
		std::vector<PeerId> came;
		std::set<PeerId> now;
		for (const auto& p : peers)
		{
			if (!p.second)
				continue;
			now.insert(p.first);
			if (!m_here.count(p.first))
				came.push_back(p.first);
		}
		m_here.swap(now);
		return came;
	}

	void clear() { m_here.clear(); }
	size_t here() const { return m_here.size(); }

private:
	std::set<PeerId> m_here;
};

// ---------------------------------------------------------------- coop_status

struct SeatStats
{
	uint32_t granted = 0;
	uint32_t refused[uint8_t(Refusal::Last) + 1] = {};
	uint32_t released[uint8_t(Release::Last) + 1] = {};
	uint32_t requests_bad = 0; // VehicleReq that did not read
	uint32_t inputs_applied = 0;
	uint32_t inputs_old = 0; // a duplicate or an older seq
	uint32_t inputs_not_driving = 0; // from a player who drives nothing here
	uint32_t inputs_wrong_car = 0; // for a car he does not drive
	uint32_t inputs_over_rate = 0; // over the RateGate (veh_input_rate)
	uint32_t inputs_bad = 0; // VehicleInput that did not read
	uint32_t stale = 0; // InputTrack::tick: every key released, the hand brake held
	uint32_t own_refused = 0; // the decider's own actor kept out of a car whose seat 0 a client holds

	void refuse(Refusal r)
	{
		if (uint8_t(r) <= uint8_t(Refusal::Last))
			++refused[uint8_t(r)];
	}
	void release(Release r)
	{
		if (uint8_t(r) <= uint8_t(Release::Last))
			++released[uint8_t(r)];
	}
};

namespace detail
{
inline std::string by_reason_refused(const SeatStats& s, uint32_t& total)
{
	std::string out;
	total = 0;
	for (uint8_t i = 1; i <= uint8_t(Refusal::Last); ++i)
	{
		if (!s.refused[i])
			continue;
		total += s.refused[i];
		out += (out.empty() ? "" : ", ") + std::string(refusal_name(Refusal(i))) + " " + std::to_string(s.refused[i]);
	}
	return out;
}

inline std::string by_reason_released(const SeatStats& s, uint32_t& total)
{
	std::string out;
	total = 0;
	for (uint8_t i = 1; i <= uint8_t(Release::Last); ++i)
	{
		if (!s.released[i])
			continue;
		total += s.released[i];
		out += (out.empty() ? "" : ", ") + std::string(release_name(Release(i))) + " " + std::to_string(s.released[i]);
	}
	return out;
}
} // namespace detail

// The counters of the coop_status line (the seat table and the trust counters are added by the caller).
inline std::string stats_text(const SeatStats& s, uint32_t inputs_per_sec)
{
	uint32_t refused = 0, released = 0;
	const std::string refused_by = detail::by_reason_refused(s, refused);
	const std::string released_by = detail::by_reason_released(s, released);
	std::string out = "granted " + std::to_string(s.granted);
	out += ", refused " + std::to_string(refused) + (refused_by.empty() ? "" : " (" + refused_by + ")");
	out += ", released " + std::to_string(released) + (released_by.empty() ? "" : " (" + released_by + ")");
	out += ", inputs " + std::to_string(inputs_per_sec) + "/s (applied " + std::to_string(s.inputs_applied) + ", old " +
	    std::to_string(s.inputs_old) + ", not driving " + std::to_string(s.inputs_not_driving) + ", wrong car " +
	    std::to_string(s.inputs_wrong_car) + ", over rate " + std::to_string(s.inputs_over_rate) + ", bad " +
	    std::to_string(s.inputs_bad) + ")";
	out += ", stale " + std::to_string(s.stale) + ", own actor kept out " + std::to_string(s.own_refused) + ", bad requests " +
	    std::to_string(s.requests_bad);
	return out;
}

// Applied inputs a second: the count of the last whole second.
class PerSecond
{
public:
	void add(uint64_t now_ms)
	{
		roll(now_ms);
		++m_count;
	}
	uint32_t rate(uint64_t now_ms)
	{
		roll(now_ms);
		return m_last;
	}

private:
	void roll(uint64_t now_ms)
	{
		if (now_ms - m_start < 1000)
			return;
		m_last = now_ms - m_start < 2000 ? m_count : 0;
		m_count = 0;
		m_start = now_ms;
	}
	uint64_t m_start = 0;
	uint32_t m_count = 0;
	uint32_t m_last = 0;
};
} // namespace vehicles
} // namespace coop
