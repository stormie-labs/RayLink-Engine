// Anomaly Coop — player characters: own actor snapshots out, other players' avatars in.
#pragma once

#include "xrCoop/coop_session.h"

namespace coop
{
namespace players
{
// One movement/animation sample of a player character.
struct State
{
	u32 time_ms = 0; // sender clock
	Fvector pos{};
	Fvector vel{};
	float body_yaw = 0.f; // model orientation
	float torso_yaw = 0.f;
	float torso_pitch = 0.f;
	u32 mstate = 0; // actor movement flags (crouch, sprint, jump...)
	u32 legs = 0, torso = 0, head = 0; // MotionID values on the character visual
	u8 active_slot = 0;
	u16 active_item = 0xffff; // item id in host numbering

	void write(Writer& w) const;
	bool read(Reader& r);
};

void install(Session& s);
void reset();
void frame(); // capture & send own state, interpolate avatars
void print_status();
void on_peer_left(PeerId id);

// Called every frame for each avatar object from its UpdateCL (see CAI_Stalker hook):
// moves the visual to the interpolated state. Returns false if no state yet.
bool drive_avatar(class CAI_Stalker* avatar);
} // namespace players
} // namespace coop
