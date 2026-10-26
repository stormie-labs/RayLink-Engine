// Anomaly Coop — player characters: own actor snapshots out, other players' avatars in.
#pragma once

#include "xrCoop/coop_session.h"

struct SInvItemPlace;
class CSE_Abstract;

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

void install_server(Session& s); // F3a T4: the world server's handlers (on the hosting session)
void install_client(Endpoint& e); // F3a T4: the client's (a remote client's session, the host player's LocalLink)
void reset();
void frame(); // capture & send own state, interpolate avatars
void print_status();
void on_peer_left(PeerId id);

// Equipment (visual, item in hands, torch) and vitals of players: coop_equip.cpp.
void equip_install_server(Session& s); // F3a T4: PlayerEquip, PlayerVitals
void equip_install_client(Endpoint& e); // F3a T4: PlayerEquip
void equip_reset();
void equip_frame();
void equip_resend(); // force a full resend (after join)
void equip_print_status();
// CInventoryItem::net_Spawn hook: while a client loads the world it came back to, the things of its own character
// (children of its actor) get the place they had (slot, belt, backpack) from the layout the host kept
// (xrCoop/coop_gear.h), before the engine takes them into the inventory. Nothing on the host or outside that load.
void on_item_spawn(SInvItemPlace& place, const CSE_Abstract& spawn_entity);

// Called every frame for each avatar object from its UpdateCL (see CAI_Stalker hook):
// moves the visual to the interpolated state. Returns false if no state yet.
bool drive_avatar(class CAI_Stalker* avatar);
// coop_equip.cpp calls it right after ChangeVisual on an avatar (R3 В4): the new visual plays nothing, so the
// motions applied so far are forgotten and drive_avatar plays the owner's current ones again.
void on_avatar_visual_changed(u16 avatar_id);

// --- read-only views of the last state an avatar was driven with (local numbering; W5) ---
// Movement flags (actor EMoveCommand, low 16 bits as sent); 0 if the avatar was never driven.
u32 avatar_mstate(u16 avatar_id);
// View direction of the player (the owner's torso yaw/pitch as CActor keeps them: direction =
// Fvector().setHP(yaw, -pitch)). False if the avatar was never driven.
bool avatar_view(u16 avatar_id, float& yaw, float& pitch);
// coop_equip.cpp: safe mode (weapon lowered) of the player whose avatar is `avatar_id`, as last reported in
// PlayerEquip; false if not known.
bool avatar_safemode(u16 avatar_id, bool& lowered);
} // namespace players
} // namespace coop
