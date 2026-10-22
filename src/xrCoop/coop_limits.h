// Anomaly Coop — what a host takes from a client: message rates per type (F2 T3) and new player characters per
// session (Ruling F2-10 b). The game installs the rates (coop_join install) and keeps the character count (coop_join).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

#include "coop_protocol.h"

namespace coop
{
namespace limits
{
// Host, per client: `type` passes `burst` times in a row, then once more every `refill_ms` (Session::limit_rate, a
// token bucket per client). What exceeds it is dropped before its handler and counted (stats(type).rate_dropped,
// coop_status); the sender is not kicked: a burst may be honest. Every value leaves an honest client room.
struct RateRule
{
	Msg type;
	uint32_t burst;
	uint32_t refill_ms;
};

constexpr RateRule kClientRateRules[] = {
	// R2 I4: a client asks for an entity it misses (P3); a modified one could ask for the whole world in a loop, each
	// answer a full spawn packet. 20 per second after a burst.
	{Msg::StateDumpRequest, 64, 50},
	// T12 review (Ruling 18): an honest client votes when it touches a level exit (once, or each time the player says
	// yes to the stock dialog, which comes back every 5 s while he stays inside). One per 2 s after a burst.
	{Msg::TransitionVote, 4, 2000},
	// F2 T3: sent on a change and every 5 s, checked every 500 ms (coop_equip.cpp kEquipCheckMs, kEquipRefreshMs).
	{Msg::PlayerEquip, 8, 400},
	// F2 T3: every 2 s (coop_equip.cpp kVitalsMs).
	{Msg::PlayerVitals, 4, 1000},
	// F2 T3: map spots a player's game adds or removes (coop_story); a quest step adds a few at once.
	{Msg::MapSpot, 32, 250},
	// F2 T3: info portions a player's game gives (coop_story); a dialog or quest step gives a batch.
	{Msg::InfoPortion, 64, 50},
	// F2 T3: a full story resend for the client: after a hash mismatch (StoryHash every 10 s), at join, console.
	{Msg::StoryResync, 4, 5000},
	// F2 T3: Lua RPC of mods (coop_script), 50 per second after a burst.
	{Msg::ScriptEvent, 64, 20},
	// F2 T3: one per money change of the player's own game (coop_money queue_change), 10 per second after a burst.
	{Msg::ReqMoney, 32, 100},
	// F2 T1 review: a talk lock is asked once per talk window opened (a request waits for its answer up to 10 s) and
	// refreshed every 30 s while the talk lasts (coop_dialog.cpp kPendingTimeoutMs, kKeepAliveMs). 10 covers a player
	// clicking an NPC that is busy, then one per second.
	{Msg::DialogLock, 10, 1000},
};

// Ruling F2-10 b: a client picks its own player key, and a key the host's world has no character for gets a new one:
// new keys farmed the starting kit and money, and every such character stays in the save for good. A session makes
// at most this many (host setting later). A player whose key has a character in the world costs nothing.
constexpr uint32_t kMaxNewCharacters = 8;
// The refusal of a new key over the limit: the nearest reason of the protocol (no room in this session for one more
// new character), and why.
constexpr RejectReason kNoNewCharacterReason = RejectReason::SessionFull;
inline std::string no_new_character_text()
{
	return "this session has made all " + std::to_string(kMaxNewCharacters) +
		" new characters it allows; players who have a character in the host's world can still join";
}

// Host: the player keys (digests) a session made a new character for. A key that got one already costs nothing
// again: a load of a save from before he came takes his character away, and he gets a new one.
class NewCharacters
{
public:
	// A player asks for the world, which has no character for his key: true if he may get one.
	bool admits(const std::string& key) const { return m_keys.count(key) != 0 || m_keys.size() < kMaxNewCharacters; }
	// The host made a character for `key`.
	void add(const std::string& key) { m_keys.insert(key); }
	size_t count() const { return m_keys.size(); }

private:
	std::set<std::string> m_keys; // at most kMaxNewCharacters
};
} // namespace limits
} // namespace coop
