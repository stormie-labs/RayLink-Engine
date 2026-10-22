// Anomaly Coop — checkpoint rules (F3, CHECKPOINTS_DEATH.md §2): the record of a player's camp, the two-deep slot with
// its rollback, the respawn choice, the "stood by the fire long enough" timer and the ini helpers for the character's
// custom data. The host decides everything here; the engine layer only feeds in what it sees.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "coop_wire.h"

namespace coop
{
namespace checkpoint
{
// What made the camp. Numbers are stored in saves and sent on the wire: do not reorder.
enum class Kind : uint8_t
{
	None = 0,
	Campfire = 1, // a CZoneCampfire that is on
	Rest = 2,     // a rest spot (bed, shelter)
	Tent = 3,     // a tent the player put up
	Arrival = 4,  // the arrival point of a level change
	Story = 5,    // a story checkpoint (a Lua trigger)
	Mod = 6       // an object a mod declared as a camp
};

inline const char* kind_name(Kind k)
{
	switch (k)
	{
	case Kind::Campfire: return "campfire";
	case Kind::Rest: return "rest";
	case Kind::Tent: return "tent";
	case Kind::Arrival: return "arrival";
	case Kind::Story: return "story";
	case Kind::Mod: return "mod";
	case Kind::None:
	default: return "none";
	}
}

// Arrival and story records come from no object.
constexpr uint16_t kNoSource = 0xffff;

// A level name as the engine writes it in a record: 1..64 characters of [A-Za-z0-9_] (no '|', no blanks, no line
// breaks, so the text form and the ini value cannot be torn apart).
inline bool level_ok(const std::string& s)
{
	if (s.empty() || s.size() > 64)
		return false;
	for (const char c : s)
	{
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok)
			return false;
	}
	return true;
}

struct Record
{
	std::string level;
	float pos[3] = {0, 0, 0};
	uint32_t level_vertex = 0xffffffff;
	uint16_t game_vertex = 0xffff;
	Kind kind = Kind::None;
	uint16_t source = kNoSource;
	uint64_t game_time_ms = 0;

	// kind is one of the six, the level name is sane, the position is finite. A record that is not valid is "no
	// record": it is never stored, encoded, sent or read.
	bool valid() const
	{
		if (kind == Kind::None || static_cast<uint8_t>(kind) > static_cast<uint8_t>(Kind::Mod))
			return false;
		if (!level_ok(level))
			return false;
		return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
	}
};

namespace detail
{
constexpr size_t kFields = 10;
constexpr uint64_t kU64Max = ~uint64_t(0);
constexpr double kFloatMax = 3.4028234663852886e+38;

// Decimal digits only, no sign, no blanks, no overflow, at most `max`.
inline bool parse_uint(const std::string& s, uint64_t max, uint64_t& out)
{
	if (s.empty() || s.size() > 20)
		return false;
	uint64_t v = 0;
	for (const char c : s)
	{
		if (c < '0' || c > '9')
			return false;
		const uint64_t digit = uint64_t(c - '0');
		if (v > (kU64Max - digit) / 10)
			return false;
		v = v * 10 + digit;
	}
	if (v > max)
		return false;
	out = v;
	return true;
}

// A plain decimal number that fits a float. "nan", "inf", hex and blanks are not numbers here.
inline bool parse_float(const std::string& s, float& out)
{
	if (s.empty() || s.size() > 40)
		return false;
	for (const char c : s)
	{
		const bool ok = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E';
		if (!ok)
			return false;
	}
	const char* begin = s.c_str();
	char* end = nullptr;
	const double d = std::strtod(begin, &end);
	if (end != begin + s.size())
		return false;
	if (!std::isfinite(d) || d > kFloatMax || d < -kFloatMax)
		return false;
	out = static_cast<float>(d);
	return true;
}
} // namespace detail

// "v1|<level>|<x>|<y>|<z>|<level_vertex>|<game_vertex>|<kind>|<source>|<time_ms>", floats with %.3f. This is the form
// stored in the character's custom data. A record that is not valid has no text form (empty string).
inline std::string encode(const Record& r)
{
	if (!r.valid())
		return std::string();
	char buf[512];
	const int n = std::snprintf(buf, sizeof(buf), "v1|%s|%.3f|%.3f|%.3f|%u|%u|%u|%u|%llu", r.level.c_str(),
		double(r.pos[0]), double(r.pos[1]), double(r.pos[2]), unsigned(r.level_vertex), unsigned(r.game_vertex),
		unsigned(static_cast<uint8_t>(r.kind)), unsigned(r.source),
		static_cast<unsigned long long>(r.game_time_ms));
	if (n <= 0 || size_t(n) >= sizeof(buf))
		return std::string();
	return std::string(buf, size_t(n));
}

// Strict: exactly the ten fields, every number a plain decimal in range, a valid record at the end. On false `out` is
// left as it was.
inline bool decode(const std::string& line, Record& out)
{
	std::string f[detail::kFields];
	size_t pos = 0;
	for (size_t i = 0; i < detail::kFields; ++i)
	{
		const size_t bar = line.find('|', pos);
		if (i + 1 < detail::kFields)
		{
			if (bar == std::string::npos)
				return false;
			f[i] = line.substr(pos, bar - pos);
			pos = bar + 1;
		}
		else
		{
			if (bar != std::string::npos) // an eleventh field
				return false;
			f[i] = line.substr(pos);
		}
	}
	if (f[0] != "v1")
		return false;

	Record r;
	r.level = f[1];
	for (int i = 0; i < 3; ++i)
		if (!detail::parse_float(f[2 + i], r.pos[i]))
			return false;
	uint64_t level_vertex = 0, game_vertex = 0, kind = 0, source = 0, time_ms = 0;
	if (!detail::parse_uint(f[5], 0xffffffffull, level_vertex) || !detail::parse_uint(f[6], 0xffffull, game_vertex) ||
		!detail::parse_uint(f[7], uint64_t(Kind::Mod), kind) || !detail::parse_uint(f[8], 0xffffull, source) ||
		!detail::parse_uint(f[9], detail::kU64Max, time_ms))
		return false;
	r.level_vertex = uint32_t(level_vertex);
	r.game_vertex = uint16_t(game_vertex);
	r.kind = static_cast<Kind>(kind);
	r.source = uint16_t(source);
	r.game_time_ms = time_ms;
	if (!r.valid())
		return false;
	out = r;
	return true;
}

// Wire form: level string, position, level vertex, game vertex, kind, source, time.
inline void write(Writer& w, const Record& r)
{
	w.str(r.level);
	w.vec3(r.pos);
	w.u32(r.level_vertex);
	w.u16(r.game_vertex);
	w.u8(static_cast<uint8_t>(r.kind));
	w.u16(r.source);
	w.u64(r.game_time_ms);
}

// False on a short packet or on content that is not a valid record; `out` is left as it was.
inline bool read(Reader& rd, Record& out)
{
	Record r;
	r.level = rd.str();
	rd.vec3(r.pos);
	r.level_vertex = rd.u32();
	r.game_vertex = rd.u16();
	const uint8_t kind = rd.u8();
	r.source = rd.u16();
	r.game_time_ms = rd.u64();
	if (!rd.ok() || kind > static_cast<uint8_t>(Kind::Mod))
		return false;
	r.kind = static_cast<Kind>(kind);
	if (!r.valid())
		return false;
	out = r;
	return true;
}

// The player's slot: the current camp and the one before it.
struct Slot
{
	Record cur, prev;
};

// A new camp. Visiting the same source again (same kind, same source) only refreshes the current record; anything
// else pushes the current record down to `prev`. A record that is not valid is ignored.
inline void set(Slot& s, const Record& r)
{
	if (!r.valid())
		return;
	if (s.cur.valid() && s.cur.kind == r.kind && s.cur.source == r.source)
	{
		s.cur = r;
		return;
	}
	s.prev = s.cur;
	s.cur = r;
}

// The object behind a camp is gone (burnt out, packed up, removed). The record it made is dropped; if that was the
// current one, the previous one takes over. True if the slot changed. "No source" never matches (arrival and story
// records come from no object).
inline bool source_lost(Slot& s, uint16_t source)
{
	if (source == kNoSource)
		return false;
	bool changed = false;
	if (s.prev.valid() && s.prev.source == source)
	{
		s.prev = Record();
		changed = true;
	}
	if (s.cur.valid() && s.cur.source == source)
	{
		s.cur = s.prev;
		s.prev = Record();
		changed = true;
	}
	return changed;
}

enum class Pick : uint8_t
{
	Current,
	Previous,
	Anchor,
	Arrival,
	Body
};

struct Choice
{
	Pick pick;
	float pos[3];
};

// Where a player comes back to life on `level`: the current record, else the previous one, else the level's anchor
// point (story spawn), else the arrival point, else where the body lies. `anchor` and `arrival` may be null.
// F3: a record on another level is skipped; F3a replaces this by a transition to that level (the one place is here).
inline Choice resolve(const Slot& s, const std::string& level, const float* anchor, const float* arrival,
	const float body[3])
{
	Choice c{};
	const auto take = [&c](Pick pick, const float* v) {
		c.pick = pick;
		for (int i = 0; i < 3; ++i)
			c.pos[i] = v ? v[i] : 0.0f;
	};
	if (s.cur.valid() && s.cur.level == level)
		take(Pick::Current, s.cur.pos);
	else if (s.prev.valid() && s.prev.level == level)
		take(Pick::Previous, s.prev.pos);
	else if (anchor)
		take(Pick::Anchor, anchor);
	else if (arrival)
		take(Pick::Arrival, arrival);
	else
		take(Pick::Body, body);
	return c;
}

// State of the "stood by the fire long enough" timer of one player.
struct Dwell
{
	uint16_t source = kNoSource;
	uint64_t since = 0;
	bool fired = false;
};

// Feed it every tick with the source the player stands at (kNoSource if none), whether they are in a fight and the host
// clock. Returns true once per visit, when the player has been near the same source for `dwell_ms` without a fight.
// A fight restarts the count (the fight itself does not end the visit, so a camp that was already set is not set
// again); leaving the source or moving to another one ends the visit. (`near` is a macro in <windows.h>, hence the
// parameter name.)
inline bool dwell_update(Dwell& d, uint16_t near_source, bool in_combat, uint64_t now_ms, uint32_t dwell_ms)
{
	if (near_source == kNoSource)
	{
		d = Dwell();
		return false;
	}
	if (d.source != near_source)
	{
		d.source = near_source;
		d.since = now_ms;
		d.fired = false;
	}
	else if (now_ms < d.since) // the clock went back: count from here
		d.since = now_ms;
	if (in_combat)
	{
		d.since = now_ms;
		return false;
	}
	if (d.fired || now_ms - d.since < dwell_ms)
		return false;
	d.fired = true;
	return true;
}

namespace detail
{
// One line of an ini text: [begin, end) without the line break, `next` is where the following line starts.
struct Line
{
	size_t begin = 0, end = 0, next = 0;
};

enum class LineKind : uint8_t
{
	Blank,
	Comment,
	Section, // [name]
	Pair,    // key = value
	Other    // anything else (a key without '=')
};

struct Parsed
{
	LineKind kind = LineKind::Blank;
	size_t name_b = 0, name_e = 0; // section name or key
	size_t val_b = 0, val_e = 0;   // value of a pair
};

inline bool next_line(const std::string& t, size_t& pos, Line& l)
{
	if (pos >= t.size())
		return false;
	const size_t nl = t.find('\n', pos);
	l.begin = pos;
	l.next = nl == std::string::npos ? t.size() : nl + 1;
	l.end = nl == std::string::npos ? t.size() : nl;
	if (l.end > l.begin && t[l.end - 1] == '\r')
		--l.end;
	pos = l.next;
	return true;
}

inline void trim(const std::string& t, size_t& b, size_t& e)
{
	while (b < e && (t[b] == ' ' || t[b] == '\t'))
		++b;
	while (e > b && (t[e - 1] == ' ' || t[e - 1] == '\t'))
		--e;
}

inline char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

// t[b, e) equals s, ASCII case aside (section and key names are case-insensitive in the engine's ini).
inline bool ieq(const std::string& t, size_t b, size_t e, const char* s)
{
	size_t i = 0;
	for (; b + i < e && s[i]; ++i)
		if (lower(t[b + i]) != lower(s[i]))
			return false;
	return b + i == e && s[i] == '\0';
}

inline Parsed parse_line(const std::string& t, const Line& l)
{
	Parsed p;
	size_t b = l.begin, e = l.end;
	trim(t, b, e);
	if (b == e)
		return p; // Blank
	if (t[b] == ';')
	{
		p.kind = LineKind::Comment;
		return p;
	}
	if (t[b] == '[')
	{
		size_t close = b + 1;
		while (close < e && t[close] != ']')
			++close;
		if (close == e)
		{
			p.kind = LineKind::Other;
			return p;
		}
		p.kind = LineKind::Section;
		p.name_b = b + 1;
		p.name_e = close;
		trim(t, p.name_b, p.name_e);
		return p;
	}
	size_t eq = b;
	while (eq < e && t[eq] != '=')
		++eq;
	if (eq == e)
	{
		p.kind = LineKind::Other;
		return p;
	}
	p.name_b = b;
	p.name_e = eq;
	trim(t, p.name_b, p.name_e);
	p.val_b = eq + 1;
	p.val_e = e;
	trim(t, p.val_b, p.val_e);
	p.kind = p.name_b == p.name_e ? LineKind::Other : LineKind::Pair;
	return p;
}

struct KeyHit
{
	Line line;
	size_t val_b = 0, val_e = 0;
};

struct Scan
{
	std::vector<KeyHit> keys;  // lines of the key inside blocks of the section, in text order
	bool section = false;      // a block of the section exists
	size_t insert_at = 0;      // in the first block: just after its last key line (or after its header)
};

inline Scan scan(const std::string& t, const char* section, const char* key)
{
	Scan r;
	bool in_sec = false, in_first = false;
	unsigned blocks = 0;
	size_t pos = 0;
	Line l;
	while (next_line(t, pos, l))
	{
		const Parsed p = parse_line(t, l);
		if (p.kind == LineKind::Section)
		{
			in_sec = ieq(t, p.name_b, p.name_e, section);
			if (in_sec)
				++blocks;
			in_first = in_sec && blocks == 1;
			if (in_first)
			{
				r.section = true;
				r.insert_at = l.next;
			}
			continue;
		}
		if (!in_sec)
			continue;
		if (p.kind == LineKind::Pair && ieq(t, p.name_b, p.name_e, key))
		{
			KeyHit h;
			h.line = l;
			h.val_b = p.val_b;
			h.val_e = p.val_e;
			r.keys.push_back(h);
		}
		if (in_first && (p.kind == LineKind::Pair || p.kind == LineKind::Other))
			r.insert_at = l.next;
	}
	return r;
}

// A line break of the text: CRLF if its first line break is one, else LF.
inline std::string eol_of(const std::string& t)
{
	const size_t nl = t.find('\n');
	return (nl != std::string::npos && nl > 0 && t[nl - 1] == '\r') ? std::string("\r\n") : std::string("\n");
}

inline void erase_lines(std::string& t, const std::vector<KeyHit>& keys, size_t first)
{
	for (size_t i = keys.size(); i-- > first;)
		t.erase(keys[i].line.begin, keys[i].line.next - keys[i].line.begin);
}
} // namespace detail

// Minimal ini access for the character's custom data (text with [sections] and "key = value" lines). Section and key
// names compare ignoring ASCII case, like the engine's ini; the first match wins. Other sections, comments and line
// breaks are left as they are.
inline bool ini_get(const std::string& text, const char* section, const char* key, std::string& value)
{
	const detail::Scan s = detail::scan(text, section, key);
	if (s.keys.empty())
		return false;
	value = text.substr(s.keys[0].val_b, s.keys[0].val_e - s.keys[0].val_b);
	return true;
}

// Sets `key = value` in the section. An existing line of the key is rewritten (extra duplicates are removed), a missing
// key is added at the end of its section, a missing section is appended at the end of the text. New lines use the
// text's own line break. A line break inside the value would tear the text apart: it becomes a blank.
inline void ini_set(std::string& text, const char* section, const char* key, const std::string& value)
{
	std::string one_line = value;
	for (char& c : one_line)
		if (c == '\r' || c == '\n')
			c = ' ';
	const std::string line = std::string(key) + " = " + one_line;
	const std::string eol = detail::eol_of(text);

	const detail::Scan s = detail::scan(text, section, key);
	if (!s.keys.empty())
	{
		detail::erase_lines(text, s.keys, 1);
		text.replace(s.keys[0].line.begin, s.keys[0].line.end - s.keys[0].line.begin, line);
	}
	else if (s.section)
	{
		std::string add;
		if (s.insert_at > 0 && text[s.insert_at - 1] != '\n')
			add += eol;
		add += line;
		add += eol;
		text.insert(s.insert_at, add);
	}
	else
	{
		if (!text.empty() && text.back() != '\n')
			text += eol;
		text += "[";
		text += section;
		text += "]";
		text += eol;
		text += line;
		text += eol;
	}
}

// Removes every line of the key from the section. The section header stays (an empty section is harmless).
inline void ini_erase(std::string& text, const char* section, const char* key)
{
	const detail::Scan s = detail::scan(text, section, key);
	detail::erase_lines(text, s.keys, 0);
}
} // namespace checkpoint
} // namespace coop
