// Anomaly Coop — the staging file of a new world, coop_new_world.ltx (docs/plans/2026-11-12-split-levels-native.md §4.2, stage 5). The
// creator chose a world in the game's new world screen (or in the launcher); the world server, started with
//   -coop_dedicated <port> -coop_hidden -coop_new -coop_stage <file>
// reads this file before its world starts (coop_dedicated.cpp first_frame): the game's record is built from it (coop_dedicated.script
// stage_new_world -> coop_game_settings.stage_new), and the creator's character is made from [creator] when he joins with the operator's
// token (coop_join.cpp host_make_creator), without the choice window. After a successful stage the server renames the file to <file>.used.
// Pure C++17, header-only, no engine dependencies: used by xrGame (coop_dedicated.cpp, coop_newchar.h) and by the unit-test exe. The game's
// Lua (stage 6b) and the launcher (Rust, rl-core) write it; this comment is the contract.
//
// ---------------------------------------------------------------- the format
//
// Text, bytes as they are (UTF-8 or cp1251 for names and titles; the server does not convert them). An UTF-8 BOM at the very start is
// skipped. Lines end with "\n" or "\r\n". At most kMaxFileBytes bytes, every line at most kMaxLineBytes bytes (without its end).
//   - a blank line, or a line whose first non-blank character is ';' or '#': a comment. No comment after a value: ';' is part of it.
//   - "[name]": a section, blanks around it allowed. A section that is not one of the three below is skipped with all its lines.
//   - "key = value": blanks (space, tab) around the key and the value are dropped. A value in double quotes ("  x ") keeps what is
//     between them, blanks included; there are no escapes and no quote inside. An empty value is allowed (it means "none").
//   - anything else is refused: a key line before the first section, a line without '=', an empty key, a broken "[".
//   - no byte below 0x20 or 0x7F in a key or a value (a tab around them is a blank).
//
// [world] (required, once)
//   faction    required. The world's faction, a stock name: 1..64 of a-z A-Z 0-9 _ . -   ("stalker", "bandit", "ecolog", ...)
//   id         the record's id ("%08x-%04x" from the game, or the launcher's): 1..64 of a-z A-Z 0-9 _ - . Absent or empty: the server makes one.
//   name       the record's name (the creator's character's name as the game lists the world), 0..64 bytes of text. Empty: [creator] name.
//   title      what the player called the world, 0..64 bytes of text.
//   story      0 | 1 | false | true. Absent: the game's default (on).
//   routes     0 | 1 | false | true. Absent: the game's default (off).
//   difficulty 1..3. Absent: the game's default (1).
//   economy    1..3. Absent: the game's default (1).
// [values] (optional, once): the keys of the game's record (docs/GAME_SETTINGS.md §3.3), at most kMaxValues lines.
//   "<src>|<path>" = value     the key may be quoted or bare. src is one of o (stock options), m (MCM), c (console), g / e (difficulty
//                              factors), s (other values of the save); path 1..kMaxKeyBytes-2 bytes. The value is text, at most
//                              kMaxValueBytes bytes: o / m / c as the game writes them, g / e / s a number or true / false.
// [creator] (optional, once). Without it nobody gets a character from the file: the creator chooses in the window like anybody.
//   name       his character's name, 0..64 bytes of text. (Today the character is named by the player's nick; see the report of stage 5.)
//   icon       his character's icon, empty or 1..64 of a-z A-Z 0-9 _ . -
//   loadout    the item sections he gets, comma separated, one entry per copy, the free kit included (as the stock screen's
//              new_game_loadout): each entry 1..64 of a-z A-Z 0-9 _ . -, empty entries are dropped, at most kMaxLoadout entries. The server
//              gives exactly this list and the ammo of its weapons; an entry that is no item section of the game is skipped with a log line.
//   money      0..kMaxMoney, a whole number. Absent: 0.
//   map        his start: an id of the faction's start list (plugins\new_game_start_locations.ltx [<faction>_start_locations], the stock
//              screen's new_game_map), empty or 1..64 of a-z A-Z 0-9 _ . -. Not a start of the world's faction: the faction's usual start.
//   bare       0 | 1 | false | true: he chose to start with nothing (no items; the money as above). Absent: 0.
//   An empty loadout with bare 0 is no choice: he gets the window at his first join, as a world made without a loadout.
//
// Refused (the whole file, the server does not start the world and says "@rl error bad_args ..."): a broken line, a key or a section that
// is known and given twice, a known key with a value outside its limits, no [world] or no faction, a file or a line too long, too many values.
// Ignored: unknown sections with all their lines, unknown keys in the three known sections.
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace coop
{
namespace world_stage
{
constexpr char kFileName[] = "coop_new_world.ltx";
constexpr char kUsedSuffix[] = ".used";

constexpr size_t kMaxFileBytes = 256 * 1024;
constexpr size_t kMaxLineBytes = 4096;
constexpr size_t kMaxValues = 2048;
constexpr size_t kMaxKeyBytes = 256; // "<src>|<path>" as a whole
constexpr size_t kMaxValueBytes = 1024;
constexpr size_t kMaxName = 64; // a name of text: [world] name, [creator] name
constexpr size_t kMaxTitle = 64; // coop_game_record.script MAX_TITLE
constexpr size_t kMaxWord = 64; // a word: faction, id, icon, map, a loadout entry (kMaxCharName of the protocol)
constexpr size_t kMaxLoadout = 64; // CharChoiceMsg::kMaxBuy (coop_newchar.h checks it)
constexpr int64_t kMaxMoney = 10000000;
constexpr int kMaxPreset = 3; // difficulty, economy: 1..3

enum class Flag : int8_t
{
	Absent = -1,
	Off = 0,
	On = 1,
};

struct World
{
	std::string id, name, title, faction;
	Flag story = Flag::Absent;
	Flag routes = Flag::Absent;
	int difficulty = 0; // 0: absent
	int economy = 0; // 0: absent
	bool operator==(const World& o) const
	{
		return id == o.id && name == o.name && title == o.title && faction == o.faction && story == o.story && routes == o.routes &&
		    difficulty == o.difficulty && economy == o.economy;
	}
};

struct Creator
{
	bool present = false; // the file has [creator]
	std::string name, icon, map;
	std::vector<std::string> loadout;
	int64_t money = 0;
	bool bare = false;
	bool operator==(const Creator& o) const
	{
		return present == o.present && name == o.name && icon == o.icon && map == o.map && loadout == o.loadout && money == o.money &&
		    bare == o.bare;
	}
};

struct Stage
{
	World world;
	std::vector<std::pair<std::string, std::string>> values; // in the order of the file, no key twice
	Creator creator;
	bool operator==(const Stage& o) const { return world == o.world && values == o.values && creator == o.creator; }
};

namespace detail
{
inline bool blank(char c) { return c == ' ' || c == '\t'; }

inline std::string trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && blank(s[a]))
		++a;
	while (b > a && blank(s[b - 1]))
		--b;
	return s.substr(a, b - a);
}

inline bool word_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

// 1..kMaxWord word characters (protocol char_name_valid); `id` also refuses '.', so that an id is a file's stem and no "..".
inline bool word(const std::string& s, bool dots = true)
{
	if (s.empty() || s.size() > kMaxWord)
		return false;
	for (char c : s)
		if (!word_char(c) || (!dots && c == '.'))
			return false;
	return true;
}

inline bool text_ok(const std::string& s)
{
	for (char c : s)
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
			return false;
	return true;
}

inline bool flag(const std::string& v, Flag& out)
{
	if (v == "1" || v == "true")
		out = Flag::On;
	else if (v == "0" || v == "false")
		out = Flag::Off;
	else
		return false;
	return true;
}

// A whole number 0..max without a sign, blanks or a fraction.
inline bool whole(const std::string& v, int64_t max, int64_t& out)
{
	if (v.empty() || v.size() > 18)
		return false;
	int64_t n = 0;
	for (char c : v)
	{
		if (c < '0' || c > '9')
			return false;
		n = n * 10 + (c - '0');
	}
	if (n > max)
		return false;
	out = n;
	return true;
}

inline std::string unquote(const std::string& v)
{
	if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
		return v.substr(1, v.size() - 2);
	return v;
}

inline bool value_key(const std::string& k)
{
	if (k.size() < 3 || k.size() > kMaxKeyBytes || k[1] != '|')
		return false;
	const char s = k[0];
	return s == 'o' || s == 'm' || s == 'c' || s == 'g' || s == 'e' || s == 's';
}

inline std::string at_line(size_t n, const std::string& what) { return "line " + std::to_string(n) + ": " + what; }
} // namespace detail

// The file's text -> `out`. False with `why` (with the line's number where there is one), `out` untouched.
inline bool parse(const std::string& text, Stage& out, std::string& why)
{
	using namespace detail;
	if (text.size() > kMaxFileBytes)
	{
		why = "the file has more than " + std::to_string(kMaxFileBytes) + " bytes";
		return false;
	}
	Stage s;
	enum class Sec
	{
		None,
		World,
		Values,
		Creator,
		Other,
	} sec = Sec::None;
	bool had_world = false, had_values = false;
	std::set<std::string> keys; // of the current known section
	std::set<std::string> value_keys;
	size_t pos = (text.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
	size_t n = 0;
	while (pos < text.size())
	{
		size_t end = text.find('\n', pos);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		++n;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.size() > kMaxLineBytes)
		{
			why = at_line(n, "longer than " + std::to_string(kMaxLineBytes) + " bytes");
			return false;
		}
		const std::string t = trim(line);
		if (t.empty() || t[0] == ';' || t[0] == '#')
			continue;
		if (t[0] == '[')
		{
			if (t.back() != ']' || t.size() < 3)
			{
				why = at_line(n, "a broken section header");
				return false;
			}
			const std::string name = trim(t.substr(1, t.size() - 2));
			Sec next = Sec::Other;
			bool* seen = nullptr;
			if (name == "world")
				next = Sec::World, seen = &had_world;
			else if (name == "values")
				next = Sec::Values, seen = &had_values;
			else if (name == "creator")
				next = Sec::Creator, seen = &s.creator.present;
			if (seen)
			{
				if (*seen)
				{
					why = at_line(n, "[" + name + "] twice");
					return false;
				}
				*seen = true;
			}
			sec = next;
			keys.clear();
			continue;
		}
		if (sec == Sec::Other)
			continue;
		const size_t eq = t.find('=');
		if (sec == Sec::None || eq == std::string::npos)
		{
			why = at_line(n, sec == Sec::None ? "a key outside a section" : "neither a section nor key = value");
			return false;
		}
		const std::string key = unquote(trim(t.substr(0, eq)));
		const std::string value = unquote(trim(t.substr(eq + 1)));
		if (key.empty())
		{
			why = at_line(n, "an empty key");
			return false;
		}
		if (!text_ok(key) || !text_ok(value))
		{
			why = at_line(n, "a control character in '" + key + "'");
			return false;
		}
		if (sec == Sec::Values)
		{
			if (!value_key(key))
			{
				why = at_line(n, key.size() > kMaxKeyBytes ? "the key is longer than " + std::to_string(kMaxKeyBytes) + " bytes" :
				    (key.size() >= 2 && key[1] == '|' && key.size() < 3 ? "a key without a path" : "a key without a source o|m|c|g|e|s"));
				return false;
			}
			if (value.size() > kMaxValueBytes)
			{
				why = at_line(n, "the value of " + key + " is longer than " + std::to_string(kMaxValueBytes) + " bytes");
				return false;
			}
			if (!value_keys.insert(key).second)
			{
				why = at_line(n, key + " twice");
				return false;
			}
			if (s.values.size() >= kMaxValues)
			{
				why = at_line(n, "more than " + std::to_string(kMaxValues) + " values");
				return false;
			}
			s.values.emplace_back(key, value);
			continue;
		}
		const bool world = sec == Sec::World;
		const bool known = world ? (key == "faction" || key == "id" || key == "name" || key == "title" || key == "story" ||
		                               key == "routes" || key == "difficulty" || key == "economy")
		                         : (key == "name" || key == "icon" || key == "loadout" || key == "money" || key == "map" || key == "bare");
		if (!known)
			continue; // a key of a later version
		if (!keys.insert(key).second)
		{
			why = at_line(n, "[" + std::string(world ? "world" : "creator") + "] " + key + " twice");
			return false;
		}
		const std::string where = "[" + std::string(world ? "world" : "creator") + "] " + key;
		bool ok = true;
		int64_t num = 0;
		if (world)
		{
			World& w = s.world;
			if (key == "faction")
				ok = word(value), w.faction = value;
			else if (key == "id")
				ok = value.empty() || word(value, false), w.id = value;
			else if (key == "name")
				ok = value.size() <= kMaxName, w.name = value;
			else if (key == "title")
				ok = value.size() <= kMaxTitle, w.title = value;
			else if (key == "story")
				ok = flag(value, w.story);
			else if (key == "routes")
				ok = flag(value, w.routes);
			else if (key == "difficulty")
				ok = whole(value, kMaxPreset, num) && num >= 1, w.difficulty = int(num);
			else if (key == "economy")
				ok = whole(value, kMaxPreset, num) && num >= 1, w.economy = int(num);
		}
		else
		{
			Creator& c = s.creator;
			if (key == "name")
				ok = value.size() <= kMaxName, c.name = value;
			else if (key == "icon")
				ok = value.empty() || word(value), c.icon = value;
			else if (key == "map")
				ok = value.empty() || word(value), c.map = value;
			else if (key == "money")
				ok = whole(value, kMaxMoney, num), c.money = num;
			else if (key == "bare")
			{
				Flag f = Flag::Absent;
				ok = flag(value, f);
				c.bare = f == Flag::On;
			}
			else if (key == "loadout")
			{
				size_t at = 0;
				while (ok && at <= value.size())
				{
					size_t comma = value.find(',', at);
					if (comma == std::string::npos)
						comma = value.size();
					const std::string entry = trim(value.substr(at, comma - at));
					at = comma + 1;
					if (entry.empty())
						continue;
					if (!word(entry) || c.loadout.size() >= kMaxLoadout)
						ok = false;
					else
						c.loadout.push_back(entry);
				}
			}
		}
		if (!ok)
		{
			why = at_line(n, where + ": '" + value.substr(0, 64) + "' is outside its limits");
			return false;
		}
	}
	if (!had_world)
	{
		why = "no [world] section";
		return false;
	}
	if (s.world.faction.empty())
	{
		why = "[world] has no faction";
		return false;
	}
	out = std::move(s);
	return true;
}

// The text of a stage, as parse reads it back (tests; the game and the launcher write the same form). Values that need it are quoted.
inline std::string write(const Stage& s)
{
	auto val = [](const std::string& v) {
		const bool quote = !v.empty() && (detail::blank(v.front()) || detail::blank(v.back()) || v.front() == '"');
		return quote ? "\"" + v + "\"" : v;
	};
	auto flag_text = [](Flag f) { return f == Flag::On ? "1" : "0"; };
	std::string o = "[world]\n";
	o += "faction = " + s.world.faction + "\n";
	if (!s.world.id.empty())
		o += "id = " + s.world.id + "\n";
	o += "name = " + val(s.world.name) + "\n";
	o += "title = " + val(s.world.title) + "\n";
	if (s.world.story != Flag::Absent)
		o += std::string("story = ") + flag_text(s.world.story) + "\n";
	if (s.world.routes != Flag::Absent)
		o += std::string("routes = ") + flag_text(s.world.routes) + "\n";
	if (s.world.difficulty > 0)
		o += "difficulty = " + std::to_string(s.world.difficulty) + "\n";
	if (s.world.economy > 0)
		o += "economy = " + std::to_string(s.world.economy) + "\n";
	if (!s.values.empty())
	{
		o += "\n[values]\n";
		for (const auto& kv : s.values)
			o += "\"" + kv.first + "\" = " + val(kv.second) + "\n";
	}
	if (s.creator.present)
	{
		o += "\n[creator]\n";
		o += "name = " + val(s.creator.name) + "\n";
		o += "icon = " + s.creator.icon + "\n";
		std::string list;
		for (const std::string& e : s.creator.loadout)
			list += (list.empty() ? "" : ",") + e;
		o += "loadout = " + list + "\n";
		o += "money = " + std::to_string(s.creator.money) + "\n";
		o += "map = " + s.creator.map + "\n";
		o += std::string("bare = ") + (s.creator.bare ? "1" : "0") + "\n";
	}
	return o;
}
} // namespace world_stage
} // namespace coop
