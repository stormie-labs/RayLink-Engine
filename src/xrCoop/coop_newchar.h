// Anomaly Coop — a player with a new key chooses his character before the world (protocol 8, owner 2026-10-23).
//
// Host (coop_join.cpp): a JoinRequest of a key the world has no character for is answered with Msg::CharOffer, and the
// player is choosing (Choosers) until his Msg::CharChoice comes. The host's Lua (coop_menu_char.host_plan_character)
// checks the choice and picks the start of the faction; the engine makes the character there, the Lua gives the kit and
// the money (host_make_character), then the world save goes to the player. A refused choice is offered again with the
// reason; a player who does not choose in kChoiceTimeoutMs is dropped.
// Client (coop_join.cpp): shows the offer in the choice window (coop_menu_char.join_offer, main menu) and sends what
// the window gives back (join_choice) as a CharChoice.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "coop_protocol.h"

namespace coop
{
namespace newchar
{
// Host: how long a player may keep the choice window open. He holds a player slot and a join in flight meanwhile.
constexpr uint64_t kChoiceTimeoutMs = 10ull * 60 * 1000;

// F3a (split levels): every player is on the host's level until then, so a faction start on another level puts a new
// character next to the host (coop_join host_place). F3a turns this on and lets him start on his faction's level.
constexpr bool kOwnLevels = false;

// The point the host's Lua picked for a new character can be used: it is a point of the game graph (`valid`) and on the
// host's level, or (F3a) on any level.
constexpr bool point_usable(bool valid, bool on_host_level) { return valid && (on_host_level || kOwnLevels); }

// Host: the players who are choosing a new character (an offer is out) and since when.
class Choosers
{
public:
	// `peer` was offered a new character, again after a refused choice: he is choosing. The time limit runs from the
	// first offer: refusals do not extend it.
	void offer(PeerId peer, uint64_t now_ms)
	{
		auto it = m_peers.find(peer);
		if (it == m_peers.end())
			it = m_peers.emplace(peer, Entry{false, now_ms}).first;
		it->second.open = true;
	}
	bool choosing(PeerId peer) const
	{
		const auto it = m_peers.find(peer);
		return it != m_peers.end() && it->second.open;
	}
	// Offered and not done yet: choosing, or his answer is being checked. His join waits for him, not for a world load.
	bool in_choice(PeerId peer) const { return m_peers.find(peer) != m_peers.end(); }
	// A CharChoice from `peer`: true if he was offered a character and has not answered that offer yet. The answer is
	// taken (he is not choosing while the host checks it); a refusal offers again, success forgets him.
	bool take(PeerId peer)
	{
		const auto it = m_peers.find(peer);
		if (it == m_peers.end() || !it->second.open)
			return false;
		it->second.open = false;
		return true;
	}
	// He got his character, left the session or was dropped.
	void forget(PeerId peer) { m_peers.erase(peer); }
	// A load or a level change replaced the world: offers are void, every player rejoins after SessionReset and is
	// asked again (a new clock).
	void world_replaced() { m_peers.clear(); }
	// The players who have been at it for longer than kChoiceTimeoutMs since their first offer.
	std::vector<PeerId> expired(uint64_t now_ms) const
	{
		std::vector<PeerId> out;
		for (const auto& it : m_peers)
			if (now_ms > it.second.since_ms && now_ms - it.second.since_ms > kChoiceTimeoutMs)
				out.push_back(it.first);
		return out;
	}
	size_t count() const { return m_peers.size(); }

private:
	struct Entry
	{
		bool open = false; // an offer waits for his answer
		uint64_t since_ms = 0; // the first offer
	};
	std::map<PeerId, Entry> m_peers; // at most kMaxPlayers - 1
};

// Client: the choice as the window gives it (faction, "sec,sec,..." of the bought gear; spaces around a section and
// empty entries are dropped). False if it is not a choice the host would read (CharChoiceMsg::valid): not sent.
inline bool make_choice(const std::string& faction, const std::string& buy_csv, CharChoiceMsg& out)
{
	CharChoiceMsg m;
	m.faction = faction;
	size_t at = 0;
	while (at <= buy_csv.size())
	{
		size_t end = buy_csv.find(',', at);
		if (end == std::string::npos)
			end = buy_csv.size();
		size_t a = at, b = end;
		while (a < b && (buy_csv[a] == ' ' || buy_csv[a] == '\t'))
			++a;
		while (b > a && (buy_csv[b - 1] == ' ' || buy_csv[b - 1] == '\t'))
			--b;
		if (b > a)
		{
			if (m.buy.size() > CharChoiceMsg::kMaxBuy)
				return false;
			m.buy.emplace_back(buy_csv, a, b - a);
		}
		at = end + 1;
	}
	if (!m.valid())
		return false;
	out = std::move(m);
	return true;
}

// Host: the bought gear for the Lua check ("sec,sec,..."; sections never hold a comma: char_name_valid).
inline std::string buy_csv(const CharChoiceMsg& c)
{
	std::string out;
	for (const std::string& s : c.buy)
	{
		if (!out.empty())
			out += ',';
		out += s;
	}
	return out;
}
} // namespace newchar
} // namespace coop
