// Anomaly Coop — a monster's special ability against a remote player (audit A5, 2026-10-24: "A-Life must live around
// every player"). The stock abilities (a giant's kick, a controller's psy tube, a burer's gravi ...) address the host's own
// actor only: a client next to such a monster got plain hits at most. Here the host decides the ability for the monster's
// enemy - a player's character - and the player's machine plays its effects on its own actor (camera and PP effectors,
// the slow-down, the damage: each machine hurts its own actor).
//
// Wire: MonsterEffect (host -> the targeted player) u16 monster (host numbering), u8 kind, u8 phase, f32 value.
#pragma once

class CObject;
class CBaseMonster;

namespace coop
{
class Session;
class Endpoint;

namespace monster_fx
{
enum Kind : u8
{
	kGiantKick = 1, // a pseudogiant's stomp: value = the hit value (0..1) at that distance
	kKinds = 16,
};

// Host: `kind` hits `target`. A remote player's character: sent to his machine, true. Anything else (this machine's actor,
// an NPC, no session): false, the caller plays it as the stock does.
bool host_send(const CObject* target, const CObject* monster, u8 kind, u8 phase, float value);

// Client: an ability the host sent is playing on this machine's actor now; its strikes are authorised.
bool client_playing();

void install_server(Session& s);
void install_client(Endpoint& e);
void print_status();
} // namespace monster_fx
} // namespace coop
