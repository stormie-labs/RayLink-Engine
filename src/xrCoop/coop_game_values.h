// Anomaly Coop — "Game settings", the part of the engine side that needs no engine (docs/GAME_SETTINGS.md §1.2, §3.4,
// §3.5). A host's game carries its own values for the 17 console rules of the co-op (friendly fire, death, autosave...).
// They are set before the world starts and must not stay in the machine's console (user.ltx) when the level is left:
//   - kGameCvars    the white list: the only console commands Lua may set for a game (coop.game.set_cvars);
//   - CvarLayer     sets them, remembers the machine's value the first time and gives it back (coop.game.release_cvars,
//                   called when the level is left); the console is reached through the CvarConsole interface;
//   - Pending       the new game that waits in memory between the "new game" screen and the world start;
//   - on_world_start  what the start of a world does to the pending game and the world's group.
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop/coop_game_settings.cpp) and by the unit-test exe.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_protocol.h" // char_name_valid

namespace coop
{
namespace game
{
// ---------------------------------------------------------------- the white list

// The game rules (§1.2, rows 1..17): the console commands of the co-op host. Everything else (coop_nick, fov, the money
// limits, the sounds) is the machine's own and stays out of a game. coop_death_share is registered by the death build;
// a command the console does not know is refused by CvarLayer::apply, so the list may run ahead of the console.
inline constexpr std::string_view kGameCvars[] = {
	"coop_friendly_fire",
	"coop_max_new_characters",
	"coop_share_rewards",
	"coop_death_mode",
	"coop_death_drop",
	"coop_bleedout",
	"coop_revive_health",
	"coop_respawn_delay",
	"coop_contusion",
	"coop_death_money",
	"coop_death_wear",
	"coop_death_rank",
	"coop_hardcore_new_kit",
	"coop_checkpoints_group",
	"coop_autosave_interval",
	"coop_autosave_count",
	"coop_death_share",
};
inline constexpr size_t kGameCvarCount = sizeof(kGameCvars) / sizeof(kGameCvars[0]);
static_assert(kGameCvarCount == 17, "docs/GAME_SETTINGS.md §1.2 lists 17 game rules");

// The longest value of a rule: a number or a token name (the longest of them, "checkpoint", has ten characters).
constexpr size_t kMaxCvarValue = 64;

// The new game that waits for its world: the encoded record from Lua, at most this many bytes (docs §3.3: a record is a
// few KiB; the limit keeps a script's mistake from holding memory).
constexpr size_t kMaxPendingPayload = 256 * 1024;

// Exactly a name of the list: no case folding (the console would find "Coop_Death_Mode" too, and the layer would then
// hold one command under two spellings, taking the first game value for the machine's), no spaces.
inline bool cvar_allowed(std::string_view name)
{
	for (const std::string_view& allowed : kGameCvars)
		if (name == allowed)
			return true;
	return false;
}

// A value that goes into the console line "<name> <value>" as one token: 1..64 characters of [A-Za-z0-9_.+-]. A number
// ("0.3", "-1", "60.": the console prints a float with its zeros cut), a token name ("checkpoint"). No separators: ';'
// would start a second command, a line break or a space would split the argument, a quote is not ours to interpret.
// Not "nan" or "inf" (nor anything that starts with them, with or without a sign): the console reads a float with atof,
// and a NaN passes the range check of CCC_Float (every comparison with it is false) and would be stored.
inline bool cvar_value_valid(std::string_view value)
{
	if (value.empty() || value.size() > kMaxCvarValue)
		return false;
	for (const char ch : value)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		    c == '.' || c == '+' || c == '-';
		if (!ok)
			return false;
	}
	const std::string_view digits = (value[0] == '+' || value[0] == '-') ? value.substr(1) : value;
	for (const char* word : {"nan", "inf"})
	{
		bool match = digits.size() >= 3;
		for (size_t i = 0; match && i < 3; ++i)
			match = (digits[i] | 0x20) == word[i]; // ASCII letters only: the loop above let nothing else through
		if (match)
			return false;
	}
	return true;
}

// The world's group (docs §8): a character name, 1..64 of [A-Za-z0-9_.-] (the same check the join flow makes of the
// faction it sends: protocol 9, CharOfferMsg.faction).
inline bool world_faction_valid(const std::string& faction) { return coop::char_name_valid(faction); }

// ---------------------------------------------------------------- the layer over the machine's console

// The console as the layer needs it. The engine implements it over `Console` (coop_game_settings.cpp), the tests over a map.
class CvarConsole
{
public:
	virtual ~CvarConsole() = default;
	// The current value of the command as its Status prints it; false if the console has no such command.
	virtual bool get(const std::string& name, std::string& value) = 0;
	// Runs "<name> <value>".
	virtual void set(const std::string& name, const std::string& value) = 0;
};

enum class CvarVerdict
{
	Applied, // the command was run (the console itself refuses a number out of its range, with a message)
	NotAllowed, // the name is not in kGameCvars
	BadValue, // the value is not a plain token (cvar_value_valid)
	Unknown, // the console has no such command, or shows a value that could not be written back ("?", empty)
};

// The game's values over the machine's. The first apply of a command remembers what the console showed (the machine's
// value); later applies of the same command only change the console; release writes the remembered values back and
// forgets them, so the next apply takes the machine's value of that moment. Not thread safe: one Lua thread at a time.
class CvarLayer
{
public:
	CvarVerdict apply(CvarConsole& console, const std::string& name, const std::string& value)
	{
		if (!cvar_allowed(name))
			return CvarVerdict::NotAllowed;
		if (!cvar_value_valid(value))
			return CvarVerdict::BadValue;
		if (!machine(name))
		{
			std::string current;
			// A value the console shows but release could not write back is not touched at all: a game value that
			// cannot be taken back would stay in user.ltx.
			if (!console.get(name, current) || !cvar_value_valid(current))
				return CvarVerdict::Unknown;
			held_.emplace_back(name, std::move(current));
		}
		console.set(name, value);
		return CvarVerdict::Applied;
	}

	// Writes the machine's values back, last applied first; returns how many. Nothing held: nothing is written.
	size_t release(CvarConsole& console)
	{
		std::vector<std::pair<std::string, std::string>> held;
		held.swap(held_); // cleared before the console is touched
		for (size_t i = held.size(); i-- > 0;)
			console.set(held[i].first, held[i].second);
		return held.size();
	}

	// The machine's value of a command whose game value is set now; nullptr when none is (the console shows the machine's).
	const std::string* machine(const std::string& name) const
	{
		for (const auto& h : held_)
			if (h.first == name)
				return &h.second;
		return nullptr;
	}

	size_t held() const { return held_.size(); }

private:
	std::vector<std::pair<std::string, std::string>> held_; // name, machine value; in the order of the first apply
};

// ---------------------------------------------------------------- the new game that waits for its world

// "New game" in the menu stages the record (coop.game.stage_new) and starts the stock new game; the world start finds it
// (on_world_start reads the group) and Lua takes it (coop.game.take_new) from before_world_start.
class Pending
{
public:
	// Stages a game, replacing the one staged before. False (and nothing staged, the old one dropped too: a screen that
	// failed to stage its game must not start the world with the previous one) for an empty payload, one over
	// kMaxPendingPayload, or a faction that is not a name.
	bool stage(std::string payload, const std::string& faction)
	{
		clear();
		if (payload.empty() || payload.size() > kMaxPendingPayload || !world_faction_valid(faction))
			return false;
		payload_ = std::move(payload);
		faction_ = faction;
		staged_ = true;
		return true;
	}

	// Hands the game over and forgets it. False, outputs untouched, when nothing is staged.
	bool take(std::string& payload, std::string& faction)
	{
		if (!staged_)
			return false;
		payload = std::move(payload_);
		faction = std::move(faction_);
		clear();
		return true;
	}

	void clear()
	{
		staged_ = false;
		payload_.clear();
		faction_.clear();
	}

	bool staged() const { return staged_; }
	// The staged game's group, "" when none is staged.
	const std::string& faction() const { return faction_; }

private:
	bool staged_ = false;
	std::string payload_;
	std::string faction_;
};

// ---------------------------------------------------------------- the start of a world

// IGame_Persistent::params::m_new_or_load, compared exactly like alife_simulator.cpp does. Other: neither (the engine then
// tries a load and falls back to a new game); the co-op leaves such a start alone.
enum class StartMode
{
	New,
	Load,
	Other
};

inline StartMode start_mode(const char* new_or_load)
{
	const std::string_view s = new_or_load ? std::string_view(new_or_load) : std::string_view();
	if (s == "new")
		return StartMode::New;
	if (s == "load")
		return StartMode::Load;
	return StartMode::Other;
}

// The word Lua's coop_game_settings.before_world_start(mode, save) gets; "" for Other.
inline const char* start_mode_name(StartMode mode)
{
	switch (mode)
	{
	case StartMode::New: return "new";
	case StartMode::Load: return "load";
	case StartMode::Other: break;
	}
	return "";
}

// What a world start does before Lua is called:
//   New    the world's group is the staged game's ("" with nothing staged: single player, a console `start`; the group
//          of the game before does not stay). The game stays staged: Lua takes it in the call that follows.
//   Load   no staged game is meant for this world: a stale one (a new game that never started) is dropped, so a later
//          new game cannot pick it up. The group of the game before does not stay either; Lua sets the loaded game's.
//   Other  nothing changes.
inline void on_world_start(StartMode mode, Pending& pending, std::string& world_faction)
{
	switch (mode)
	{
	case StartMode::New:
		world_faction = pending.faction();
		break;
	case StartMode::Load:
		pending.clear();
		world_faction.clear();
		break;
	case StartMode::Other:
		break;
	}
}
} // namespace game
} // namespace coop
