// Anomaly Coop — talking to NPCs (SPEC R-8.1, R-8.3, R-7.8).
//
// A dialog runs on the machine of the player who talks: his actor, faction, reputation, money and
// info portions decide the branches (R-8.3), so every player gets the quest and faction dialogs of
// his own character (R-7.8). World changes made by dialog actions (items, money, infos, tasks) are
// routed to the host by the item and story layers. What is shared here is the talk lock: one player
// talks to an NPC at a time (R-8.1).
//
// Lock messages (ids in host numbering, reliable lane):
//   DialogLock        client -> host   u16 npc, u8 keepalive     ask for the lock / refresh it
//   DialogLockResult  host -> client   u16 npc, u8 LockResult    0 granted, 1 busy, 2 gone, 3 refused
//   DialogUnlock      client -> host   u16 npc                   release (talk window closed)
//                     host -> client   u16 npc                   lock revoked (timeout, NPC gone)
// Host: grants when nobody else (the host included) holds the NPC; a player holds at most one lock.
// Locks are released on DialogUnlock, when the holder leaves, and after 120 s without a keep-alive
// (clients refresh every 30 s while talking). The host's own talk takes the lock at once and is
// refused while a client holds the NPC. Repeats change nothing (P6).
// Client: the talk window does not open until the host granted the lock; then the talk is started
// again by the engine's own path (CActor::RunTalkDialog). Denied: a HUD message "busy".
//
// Included from UI files: keep it free of engine includes.
#pragma once

#include "xrCoop/coop_protocol.h"

class CInventoryOwner;

namespace coop
{
class Session;

namespace dialog
{
void install(Session& s);
void frame(); // main thread, every frame
void reset(); // session started / ended / world replaced
void on_peer_left(PeerId id); // host: drop the locks of a player that left
void print_status();

// May a talk with `partner` start on this machine now? No session: true.
// Host: takes the lock for the host's actor, false while a client holds `partner`.
// Client: true only while the host granted us the lock; otherwise a request goes out and the talk
// is started again when it is granted. Safe to call twice for one talk (idempotent).
// Also usable as an early hook in CActor::RunTalkDialog (before OfferTalk) by the core.
bool may_start_talk(CInventoryOwner* partner, bool disable_break);

// W11, host: the player holding the talk lock of NPC `npc` (host numbering), kHostPeer for the host's own
// talk, kInvalidPeer if nobody (or not the host). The NPC meet logic (Lua coop.talk_holder) stands the NPC
// still and turns it to a client that talks to it on his machine.
PeerId host_talk_holder(u16 npc);

// F2 T1, client: the NPC whose talk lock we hold (host numbering), 0xffff if none. Money changes made meanwhile go to
// the host as dialog changes (coop_money); they are sent before the lock is released.
u16 client_partner();
// F2 T1, host: the NPC whose talk lock player `peer` holds (host numbering, kHostPeer: the host's own talk), 0xffff if
// none or not the host.
u16 host_lock_of(PeerId peer);

// --- engine hooks (ui/UITalkWnd.cpp) ---
// CUITalkWnd::Show(true), before InitTalkDialog. false: do not initialize; the talk state of both
// sides is stopped here and the window hides itself on its next Update (talk_window_refused).
bool on_talk_window_open(CInventoryOwner* partner, bool disable_break);
// CUITalkWnd::Update: true while a refused window must hide (call HideDialog and return).
bool talk_window_refused();
// CUITalkWnd::Show(false): releases the lock held for this talk.
void on_talk_window_closed();

// Short HUD line (and a console line) for the local player. `string_id` is looked up in the string
// table; if it is missing, `fallback` is shown. Shared by coop_trade.
void hud_message(const char* string_id, const char* fallback);
} // namespace dialog
} // namespace coop
