// Anomaly Coop — player characters: own actor snapshots out, other players' avatars in.
#pragma once

#include "xrCoop/coop_session.h"
#include "xrCoop/coop_instrument.h"

struct SInvItemPlace;
class CSE_Abstract;

class CTorch;

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
	instrument::State instrument;

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
// Live 2026-11-07, host: the arrival of a client's character on a new level. At the go: his last report (the vitals the
// proxy keeps) and where he was put; at his WorldLoaded the window starts, his first report kWindowMs later is logged.
void arrival_host_leaves(PeerId peer, u16 proxy, const char* nick, const char* level, float x, float y, float z);
void arrival_host_entered(PeerId peer);
void equip_frame();
void equip_loading_frame(); // build 56: a client still loading the world gets its condition back at once (coop_equip.cpp)
void equip_resend(); // force a full resend (after join)
// Build 56 (live 2026-11-18, "everything a client has must come through a transition exactly as in a usual game"): a client's
// new actor is built from the HOST's client data. Six numbers of the player's own condition came back from the host's
// record (`vitals =`, up to kVitalsMs old); the rest (wounds by bone and kind, alcohol, the medicine at work, condition
// flags, boosters with their seconds) was the host's, or was put back roughly by a script. In a load that continues the
// same world this process lives on, so his actor's save (CActor::save: the condition, the slot in his hands, the quick
// slots, the PDA map filters) is kept here the moment the load is announced (the old world and the own actor are still
// up), and loaded into the new actor by CActor::load itself: what a single player's level change does.
// Client: the load into the world of `to_epoch` is announced. Not live now (a second announcement before he was in): what
// is held stays and goes to the new epoch.
void condition_carry_take(u32 to_epoch);
// What is held is void: the next world does not continue this one (the host loaded a save), or the session is another one.
void condition_carry_drop();
// CActor::load, at its end (the host's bytes are read, the save's stream is in step): on a client that loads a world for
// which it holds its own actor's save, that save is loaded over the host's - the player's own condition, slot in hands,
// quick slots and PDA map filters, before his items come and before the first update.
void own_actor_loaded(class CActor& actor);
// The health held for the world of `epoch`, for the role swap: the actor stands up with it (the host's record is up to
// kVitalsMs older). False: nothing is held for that world.
bool condition_carry_health(u32 epoch, float& health);
// For the role swap, what the save's entities say and the actor's own save does not: the stance he left in (crouch, slow
// walk: CActor::net_Spawn reads them from the actor entity's mstate, the host's actor's in his save) and his head torch
// if it was lit (CTorch::net_Spawn reads the item entity's m_active; the host keeps every torch of a player's character
// dark, so the save says off). False: nothing is held for the world of `epoch` (a join, a rejoin: he stands, light off).
bool condition_carry_extras(u32 epoch, u16& stance, u16& lit_torch);
// True on a client whose condition in this world came from what it held (not from the host's record): the scripts leave
// his boosters alone then (coop.condition_carried, coop_core_player).
bool condition_carried();
// CActorCondition::UpdateCondition: the own actor of a client that is still loading the world (precache, the held screen).
// Its condition does not run: a single player's does not either between his save and his first frame, and until the
// restore (the first frame the world is up) it is the host's.
bool own_condition_held(const class CEntityCondition* condition);
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
// Host or map server: where a player's own machine last said he stands (his newest PlayerState) and how old that is. False
// when he never reported here (live 2026-11-08: the hit journal compares it with his character here).
bool reported_position(u16 avatar_id, Fvector& pos, u64& age_ms);
// Host or map server: the speed (m/s) of the player's actor as his newest PlayerState says (|vel|). False when he never
// reported here. Build 51: a burning fuzz / liana wakes by the speed of who walks in it, and a driven avatar's capsule
// has none of its own (CHairsZone::CheckForAwaking).
bool reported_speed(u16 avatar_id, float& speed);
// coop_equip.cpp: safe mode (weapon lowered) of the player whose avatar is `avatar_id`, as last reported in
// PlayerEquip; false if not known.
bool avatar_safemode(u16 avatar_id, bool& lowered);
// coop_equip.cpp: the player's head torch is on, as last reported in PlayerEquip. The torch items of an avatar stay dark
// (coop_equip draws the head torch itself): the AI's light estimate of a player's character reads this.
bool avatar_headlamp(u16 avatar_id);
// coop_equip.cpp: a torch of a player goes on or off on this machine (CTorch::Switch): one line in the log, no effect.
void on_torch_switch(const CTorch& torch, bool on);
} // namespace players
} // namespace coop
