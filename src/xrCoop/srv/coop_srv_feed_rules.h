// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the feed lines of a server world (S5, unit A-S6J; TZ R7: "строки ленты «ушёл на карту / пришёл с карты»"): a player
// of a server world reads in his PDA feed where the others went, "<nick> went to <level>" and, on the level he stands on, "<nick> came
// from <level>".
//
// Who decides where a player stands: the world server, in its table of seats (LevelMap, xrCoop/coop_levels_wire.h), which it sends to
// every player and map server whenever it changes. What a player's machine has: that table (levels::level_of_player), the level it has
// loaded itself and the nicks of the session. Nobody sends anything for the feed: a player's machine reads the changes of the table it
// gets (srv/coop_srv_join.cpp feed_frame), so the lines need no message, no protocol and no trust in a client's word.
// Co-op: none. The group moves together and no table goes out (a co-op process never runs the server's layer).
//
// Pure C++17, no engine dependencies: tests/test_srv_join_feed.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coop
{
namespace srv
{
namespace feed
{
constexpr uint16_t kNoLevel = 0xFFFF; // levels::wire::kNoLevel: on no level, or not known
constexpr size_t kMaxNick = 32; // the bytes of a nick a line shows

// Another player of the session and the level the world server's table puts him on now (kNoLevel: not known).
struct Row
{
	uint32_t peer;
	uint16_t level;
};

enum class Kind : uint8_t
{
	WentTo, // he left for `level`, a level that is not this player's
	CameFrom, // he came onto this player's level from `level`
};

struct Line
{
	uint32_t peer;
	Kind kind;
	uint16_t level;
};

// The last level of every other player this machine saw, and the lines its changes make.
class Watch
{
public:
	// `rows`: every other player of the session (not this machine's own, not a seat that plays nobody) with his level now; `mine`: the
	// level this player stands on. A player whose known level changed: "came from <old>" when he is on `mine` now, else "went to
	// <new>". The first sight of a player (a join) and a row of no known level say nothing; a player no row names any more is
	// forgotten (he left the session: when he comes back it is a join). `mine` kNoLevel (this machine loads a world): nothing is compared
	// and nothing changes, the next call compares with the table seen last, so a move A -> B -> C meanwhile is one line.
	std::vector<Line> step(const std::vector<Row>& rows, uint16_t mine)
	{
		std::vector<Line> out;
		if (mine == kNoLevel)
			return out;
		std::map<uint32_t, uint16_t> now;
		for (const Row& r : rows)
		{
			const auto was = m_known.find(r.peer);
			if (r.level == kNoLevel)
			{
				if (was != m_known.end())
					now[r.peer] = was->second; // not known now: what was known stays
				continue;
			}
			now[r.peer] = r.level;
			if (was == m_known.end() || was->second == r.level)
				continue;
			if (r.level == mine)
				out.push_back(Line{r.peer, Kind::CameFrom, was->second});
			else
				out.push_back(Line{r.peer, Kind::WentTo, r.level});
		}
		m_known.swap(now);
		return out;
	}
	// A new session: nobody is known.
	void reset() { m_known.clear(); }
	size_t known() const { return m_known.size(); }

private:
	std::map<uint32_t, uint16_t> m_known;
};

// The text of a line: `tmpl` (a string of st_srv_feed.xml) with every "{nick}" and "{level}" replaced. Nothing else of it is read: neither
// the template nor the nick is ever a printf format.
inline std::string fill(const std::string& tmpl, const std::string& nick, const std::string& level)
{
	static const std::string kNick = "{nick}";
	static const std::string kLevel = "{level}";
	std::string out;
	out.reserve(tmpl.size() + nick.size() + level.size());
	for (size_t i = 0; i < tmpl.size();)
	{
		if (tmpl.compare(i, kNick.size(), kNick) == 0)
		{
			out += nick;
			i += kNick.size();
		}
		else if (tmpl.compare(i, kLevel.size(), kLevel) == 0)
		{
			out += level;
			i += kLevel.size();
		}
		else
			out += tmpl[i++];
	}
	return out;
}

// A nick as a line shows it: a nick is the player's own text, so the bytes a UI text reads as markup ('%' starts a colour, %c[...]) and
// the control characters (a line break) are dropped, and it is cut to kMaxNick bytes; "?" when nothing is left.
inline std::string shown_nick(const std::string& nick)
{
	std::string out;
	for (const char c : nick)
	{
		if (out.size() >= kMaxNick)
			break;
		const unsigned char u = static_cast<unsigned char>(c);
		if (c == '%' || u < 0x20 || u == 0x7F)
			continue;
		out += c;
	}
	return out.empty() ? std::string("?") : out;
}
} // namespace feed
} // namespace srv
} // namespace coop
