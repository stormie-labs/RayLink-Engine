// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — a player's progress, checked where his record is kept: the module API. The bodies are coop_progress.cpp, on the
// pure rules of xrCoop/coop_progress_rules.h.
//
// A player's game tells his rank, reputation and community (Msg::Progress, coop_story.cpp). The process that keeps the record checks
// what he told before it is stored, applied to his character and sent on: the co-op host, in the Server mode the world server (the
// map servers of the levels tally what the player earned there). The same code in both modes. Every check goes through the
// section [coop_trust] of gamedata/configs/mod_system_coop_trust.ltx (log | refuse | off per name; progress_rank,
// progress_reputation, progress_community; "log" when nothing is set: the values pass as told, the "[coop-trust] ... would refuse"
// line and the counter are written).
//
// register_lua is called from coop_script.cpp, frame, reset and print_status from coop_sync.cpp: both modes run them.
#pragma once

#include "xrCoop/coop_protocol.h" // PeerId

struct lua_State;

namespace coop
{
namespace progress
{
// coop.progress.apply_verified_fight. coop.progress.community_ok(peer, community, index, character) and
// coop.progress.on_verified_fight are the keeper's script's to set (the coop_faction record, a disguise; the check of a fight fact).
void register_lua(lua_State* L);

// coop_story.cpp's handler of Msg::Progress where the record is kept calls it before it stores and applies the report, and
// its put-back of the reported values calls it too. `from`: the sender as the transport names him, never a field of the message;
// `character`: his character (host numbering). In: the rank, reputation and community he told; out: the values to store, apply and
// send on (in "refuse" the clamped ones). The meaning of the result: coop_progress_rules.h.
bool host_check(PeerId from, u16 character, int& rank, int& reputation, int& community);

// Main thread, every frame of a session while the world is up (sync::frame calls it under world_up(), as objects::frame).
// Every process calls it: it does nothing where it keeps no state.
void frame();

// The end of a session (sync::reset) forgets the book of stored values and the counters; every process calls it.
void reset();

// Its coop_status section (sync::print_status): the checks by name and verdict, the "would refuse" counters.
void print_status();

// The reputation an honest fight gives a player reaches the keeper of his record.
// coop_story.cpp: the keeper puts `delta` into the base of player `peer`'s stored report and tells his machine (RelReputationDelta about
// his character `character`, the keeper's numbering). The co-op host calls it for its own fights (host_flush_rep_deltas); the world
// server through coop.progress.apply_verified_fight, after its script checked a map server's fact. False: no player to apply it to.
bool host_apply_fight(PeerId peer, u16 character, int delta);
// coop_progress.cpp: a map server's fight fact (xrCoop/coop_fight_fact.h; `from`: the transport's sender) goes to the keeper's script,
// coop.progress.on_verified_fight(epoch, fact, from, character, delta), which checks the sender, his route and the order (coop_trust on
// every refusal) and applies it through coop.progress.apply_verified_fight. False: refused, or no keeper script here.
bool host_fight_fact(PeerId from, u32 epoch, u32 fact, u16 character, int delta);
} // namespace progress
} // namespace coop
