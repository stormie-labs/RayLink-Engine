// Anomaly Coop — a monster's special ability against a remote player (audit A5, 2026-10-24: "A-Life must live around
// every player"). The stock abilities (a giant's kick, a controller's psy tube, a burer's gravi ...) address the host's own
// actor only: a client next to such a monster got plain hits at most. Here the host decides the ability for the monster's
// enemy - a player's character - and the player's machine plays its effects on its own actor (camera and PP effectors,
// the slow-down, the damage: each machine hurts its own actor).
//
// Wire: MonsterEffect (host -> the targeted player) u16 monster (host numbering), u8 kind, u8 phase, f32 value. A flame
// (kPolterFlame) goes to every client, with its vec3 position and vec3 direction after the value.
#pragma once

class CObject;
class CBaseMonster;
class CPhysicsShellHolder;

namespace coop
{
class Session;
class Endpoint;

namespace monster_fx
{
enum Kind : u8
{
	kGiantKick = 1, // a pseudogiant's stomp: value = the hit value (0..1) at that distance
	kControllerTube = 2, // a controller's tube: phase = CControllerPsyHit::ECoopPhase, value = the pull's time (s)
	kControllerControl = 3, // a controller's psy blow (control_hit): camera and post-process shake, the sound
	kControllerStamina = 4, // a controller's melee blow drains stamina, may knock the weapon out
	kAntiAim = 5, // a burer felt the player's aim: phase = the camera effector's index, then the blow (stamina hit)
	kBurerGravi = 6, // a burer's gravi wave flies at the player (shown; its strike comes as PlayerHit)
	kBloodsuckerVampire = 7, // a bloodsucker's grip: phase 0 grabs (view turned to it, HUD and weapon away), 1 lets go
	kPsyDogAppear = 8, // a psy dog's phantom leaps out at the player: camera and post-process shake
	kMeleeHit = 9, // a monster's blow landed (its damage comes as PlayerHit): value = its damage. Claw marks, stagger, shove
	kPolterFlame = 10, // a fire poltergeist lit a flame (audit F5c): its particles and sound on every client
	kKinds = 16,
};

// Host: `kind` hits `target`. A remote player's character: sent to his machine, true. Anything else (this machine's actor,
// an NPC, no session): false, the caller plays it as the stock does.
bool host_send(const CObject* target, const CObject* monster, u8 kind, u8 phase, float value);
// Host: `monster`, a fire poltergeist, lit a flame at `at`, burning along `dir`: every client plays it on the puppet.
void host_flame(const CObject* monster, const Fvector& at, const Fvector& dir);

// Audit F4 (2026-10-31): host: `monster` threw `object` with telekinesis. A player's proxy takes no collision damage (a
// puppet skips the movement update that applies it), so its flight is followed and a remote player it passes gets the
// hit, as the monster's, once. `power`: the monster's own hit value; below zero, as hard as the object flies (burer).
void host_throw(CPhysicsShellHolder* object, const CObject* monster, float power = -1.f);

// Host: a monster's grip on a remote player's character (a controller's tube, a bloodsucker's vampire). One at a time
// per player, as the stock checks make it on the actor (the camera effector, the input handler). A hold whose
// monster is gone or dead counts no more.
void host_hold(u16 target, u16 monster);
void host_unhold(u16 target, u16 monster);
bool host_held(u16 target);

// Every frame (sync::frame). Client: effects that came before their monster (a phantom spawned this instant) play once it
// is here, or are dropped after a moment. Host: the flights of host_throw.
void frame(bool world_up);

// Client: an ability the host sent is playing on this machine's actor now; its strikes are authorised.
bool client_playing();

void install_server(Session& s);
void install_client(Endpoint& e);
void print_status();
} // namespace monster_fx
} // namespace coop
