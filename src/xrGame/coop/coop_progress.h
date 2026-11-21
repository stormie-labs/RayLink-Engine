// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — a player's progress, checked where his record is kept (F5, row 9 of F2, build 57; temp/b58/f5_plan.md section 6):
// the module API. Declarations only (F5-PROTO); the bodies are F5-PRG-2's, engine/src/xrGame/coop/coop_progress.cpp, on the pure
// rules of engine/src/xrCoop/coop_progress_rules.h.
//
// A player's game tells his rank, reputation and community (Msg::Progress, coop_story.cpp). The process that keeps the record checks
// what he told before it is stored, applied to his character and sent on: the co-op host, in the Server mode the world server (the
// map servers of the levels tally what the player earned there: F5-PRG-4). The same code in both modes. Every check goes through the
// section [coop_trust] of gamedata/configs/mod_system_coop_trust.ltx (log | refuse | off per name; progress_rank,
// progress_reputation, progress_community; build 57 ships "log": the values pass as told, the "[coop-trust] ... would refuse" line
// and the counter are written).
//
// F5-PROTO registers register_lua (coop_script.cpp), frame and print_status (coop_sync.cpp), as for coop_money.h: both modes run
// them.
#pragma once

#include "xrCoop/coop_protocol.h" // PeerId

struct lua_State;

namespace coop
{
namespace progress
{
// F5-PRG-2: coop.progress.budget(peer, rank_points, reputation_points) and coop.progress.community_ok(peer, community), the
// callback Lua sets (the coop_faction record, a disguise).
void register_lua(lua_State* L);

// F5-PRG-2: coop_story.cpp's handler of Msg::Progress where the record is kept calls it before it stores and applies the report, and
// its put-back of the reported values calls it too. `from`: the sender as the transport names him, never a field of the message;
// `character`: his character (host numbering). In: the rank, reputation and community he told; out: the values to store, apply and
// send on (in "refuse" the clamped ones). The meaning of the result: F5-PRG-2 (coop_progress_rules.h).
bool host_check(PeerId from, u16 character, int& rank, int& reputation, int& community);

// F5-PRG-2: main thread, every frame of a session while the world is up (sync::frame calls it under world_up(), as objects::frame).
// Every process calls it: it does nothing where it keeps no state.
void frame();

// F5-REV-1 note 2: the end of a session (sync::reset) forgets the book of stored values and the counters; every process calls it.
void reset();

// F5-PRG-2: its coop_status section (sync::print_status): the checks by name and verdict, the "would refuse" counters.
void print_status();

// INTEG-C (F5-PRG-4, rv_F5-PRG-4 blocker 2): the reputation an honest fight gives a player reaches the keeper of his record.
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
