// Anomaly Coop — the server's part of a session (docs/MODES.md section 4).
//
// A world server's Session takes the bridges of its map servers (Hello role MapServer, a token issued here, from this PC only, no
// player slot) and an operator (the creator's game from this PC with the token of the server's -coop_operator holds the host's
// rights); a map server's Session hosts its level with no transport of its own, through its connection to the world server (its
// players are virtual peers behind Msg::Tunnel). All of that is ServerPart below: the SessionPart (xrCoop/coop_session.h) the
// server's layer adds to a Session. The Core's Session runs the connections and asks the part at fixed points; the tokens, the
// verdicts, the ids of the bridges, the Tunnel and the hooks of the game are here (A-S4Sb moved them out of Session).
//
// Server code reaches all of it through the view SessionSrv only: coop_mapserver.cpp, coop_trunk.cpp, coop_levels_xfer.cpp,
// srv/coop_srv_mode.cpp. The Core and the co-op never do: L3 refuses an include of this file, L11 a call of the old Session
// members, and Session has none of them any more. A co-op session never gets this part: no bridge, no virtual peer, no operator.
//
// Two ways in, one each (A-S4Sb):
//   - the operator's token: the world server's mode arms it in its configure (srv/coop_srv_mode.cpp), right before
//     Manager::start_host hosts; that host() takes it, and no SessionConfig ever holds it;
//   - a map server's level: its mode makes the next host() of its Session the level's, carried by its world connection (install,
//     before Manager::start_map_server connects and hosts). host_virtual is the same two steps at once (the tests).
//
// Pure C++17, header-only (the game and the tests include it; no build line needed), single-threaded like the Session: every call
// on the thread that calls Session::update. A view holds its Session by reference and nothing else: make one where it is needed.
#pragma once

#include "coop_mapserver_rules.h"
#include "coop_operator.h"
#include "../coop_session.h"

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace coop
{
namespace srv
{
static_assert(mapserver::kMaxBridges <= kTrunkSlots, "the session keeps a transport slot for every map server of its world");
static_assert(kTokenSize == kHelloTokenSize, "a token travels in HelloMsg::token");
static_assert(std::is_same<op::Key, PlayerKey>::value, "the operator's token is bound to a player key");
static_assert(mapserver::kBridgeBase >= kMaxPeerSlots, "a bridge's id is no player slot (SessionPart::Admission::id)");

class ServerPart final : public SessionPart
{
public:
	using Token = srv::Token;
	using BridgeJoined = std::function<void(PeerId bridge)>;
	using BridgeLeft = std::function<void(PeerId bridge, const std::string& reason)>;
	using OperatorLine = std::function<void(PeerId from, const std::string& line)>;

	// Session::part: the tag of this kind of part, and its maker.
	static const void* kind()
	{
		static const char tag = 0;
		return &tag;
	}
	static std::unique_ptr<SessionPart> make() { return std::unique_ptr<SessionPart>(new ServerPart()); }

	~ServerPart() override
	{
		wipe(m_armed);
		wipe(m_token);
		wipe(m_bound_token);
	}

	// ------------------------------------------------------------------ what the view sets and asks (SessionSrv)

	Owner& owner() const { return *m_owner; }
	// A world server: the operator's token of the next host() (all zeros: no operator). That host() takes it; nothing else keeps it.
	void arm_operator(const Token& token) { m_armed = token; }
	// A map server: the next host() of this Session is its level's, carried by `world` (its connection to the world server).
	void level_through(Session& world) { m_level_next = &world; }
	bool carrying() const { return m_level_world != nullptr; }
	uint64_t refused() const { return m_refused; }
	// Map server k (1 .. kMaxBridges) connects once with this token; issuing again for k replaces it. Nothing for a k out of range or
	// a session that does not host.
	void issue(uint32_t k, const Token& token)
	{
		if (m_owner->session().role() != Role::Host || k == 0 || k > mapserver::kMaxBridges)
			return;
		revoke(k);
		m_tokens[token] = k;
	}
	void revoke(uint32_t k)
	{
		for (auto it = m_tokens.begin(); it != m_tokens.end();)
			it = it->second == k ? m_tokens.erase(it) : std::next(it);
	}
	// The game's hooks, kept across sessions like the handlers; they run from Session::update.
	BridgeJoined bridge_joined;
	BridgeLeft bridge_left;
	OperatorLine operator_line;

	// ------------------------------------------------------------------ SessionPart

	void attached(Owner& owner) override { m_owner = &owner; }

	Session* carrier() override { return m_level_next; }

	void host_starts(Session* carrier) override
	{
		m_token = m_armed; // the operator of this host: the token armed for it, none when nobody armed one
		wipe(m_armed);
		m_level_next = nullptr;
		m_level_world = carrier;
		if (carrier)
			say("map server: the host of its level, through the host's process");
	}

	void ended() override
	{
		m_tokens.clear();
		m_level_world = nullptr;
		wipe(m_armed);
		wipe(m_token);
	}

	// A map server of this PC, with the token issued for it, once. It takes the place of an old connection of the same number:
	// a new process started for it.
	Admission admit(const std::string& address, const HelloMsg& hello) override
	{
		Admission a;
		mapserver::Token token{};
		std::memcpy(token.data(), hello.token, token.size());
		const auto issued = m_tokens.find(token);
		switch (mapserver::hello_verdict(mapserver::loopback_address(address), issued != m_tokens.end()))
		{
		case mapserver::HelloVerdict::Accept: break;
		case mapserver::HelloVerdict::NotLoopback:
			a.text = "A map server connects from the host's own PC.";
			return a;
		case mapserver::HelloVerdict::BadToken:
			a.text = "This token was not issued by this session.";
			return a;
		}
		if (issued == m_tokens.end())
		{
			a.text = "This token was not issued by this session.";
			return a;
		}
		const uint32_t k = issued->second;
		m_tokens.erase(issued); // one process, one use
		a.accept = true;
		a.id = mapserver::bridge_peer(k);
		a.nick = "ms" + std::to_string(k);
		a.replaced = "a new map server took its place";
		return a;
	}

	void trunk_joined(PeerId id, const std::string& address) override
	{
		say("host: map server %u connected from %s", unsigned(mapserver::bridge_index(id)), address.c_str());
		m_owner->emit([this, id] {
			if (bridge_joined)
				bridge_joined(id);
		});
	}

	void trunk_left(PeerId id, const std::string& reason) override
	{
		say("host: map server %u left: %s", unsigned(mapserver::bridge_index(id)), reason.c_str());
		m_owner->emit([this, id, reason] {
			if (bridge_left)
				bridge_left(id, reason);
		});
	}

	// A map server is a client of the world (mapserver::bridge_takes).
	bool trunk_takes(Msg type) const override { return mapserver::bridge_takes(type); }

	// While an operator's token is set and its holder is not in, the last place is his.
	bool holds_place(const std::string& address, const HelloMsg& hello, bool holder_in) const override
	{
		return configured() && !holder_in && !would_be_operator(address, hello);
	}

	// coop_operator.h supplies the operator verdict for this player's Hello. Every verdict admits the player the same way.
	// Only the server log distinguishes the verdicts, and it omits the token.
	bool rights_of(const PeerInfo& player, const HelloMsg& hello) override
	{
		const bool presented = !op::token_none(hello.token, kHelloTokenSize);
		const bool now = configured();
		if (m_bound && !op::token_equal(m_bound_token.data(), m_token.data(), kHelloTokenSize))
			m_bound = false; // another token now: its binding starts anew
		const bool matches = op::token_equal(hello.token, m_token.data(), kHelloTokenSize) && presented && now;
		const op::Verdict v =
		    op::operator_verdict(mapserver::loopback_address(player.address), matches, m_bound ? &m_bound_key : nullptr, hello.key);
		switch (v)
		{
		case op::Verdict::Operator:
			if (!m_bound)
			{
				m_bound = true;
				m_bound_key = hello.key;
				m_bound_token = m_token;
			}
			say("host: operator: peer %u '%s' holds the host's rights", unsigned(player.id), player.nick.c_str());
			return true;
		case op::Verdict::OtherKey:
			say("! host: peer %u '%s' presented the operator token with another player key: an ordinary player", unsigned(player.id),
			    player.nick.c_str());
			return false;
		case op::Verdict::NoMatch:
			if (presented)
				say("host: peer %u '%s' presented a token that is not the operator's: an ordinary player", unsigned(player.id),
				    player.nick.c_str());
			return false;
		case op::Verdict::NotLoopback:
			if (presented)
				say("host: peer %u '%s' presented a token from another PC: not looked at, an ordinary player", unsigned(player.id),
				    player.nick.c_str());
			return false;
		}
		return false;
	}

	// After the rate, the sender, the length and the list (op::command_verdict). A refusal is counted and
	// logged (the first and every 100th: a player cannot fill the log), the sender is not kicked and gets no answer. What passes goes
	// to operator_line (dedicated::run_for), never to a console from here.
	void console_line(const PeerInfo& from, bool may, const std::string& line) override
	{
		const op::Command v = op::command_verdict(may, line);
		if (v != op::Command::Run)
		{
			++m_refused;
			if (m_refused != 1 && m_refused % 100 != 0)
				return;
			std::string shown = line.substr(0, 64);
			printable(shown);
			say("host: operator command from peer %u '%s' refused (%s): '%s'%s", unsigned(from.id), from.nick.c_str(),
			    op::command_name(v), shown.c_str(), line.size() > 64 ? "..." : "");
			return;
		}
		const PeerId id = from.id;
		say("host: operator command from peer %u: %s", unsigned(id), line.c_str());
		m_owner->emit([this, id, line] {
			if (operator_line)
				operator_line(id, line);
		});
	}

	// A map server: a message to a player of its level, up its world connection in a Tunnel.
	size_t carry(PeerId to, Lane lane, uint16_t type, const uint8_t* a, size_t na, const uint8_t* b, size_t nb) override
	{
		if (!m_level_world)
			return 0;
		levels::wire::TunnelMsg t;
		t.peer = to;
		t.flags = mapserver::tunnel_lane_flags(lane);
		t.packet.reserve(2 + na + nb);
		t.packet.push_back(uint8_t(type & 0xFF));
		t.packet.push_back(uint8_t(type >> 8));
		if (na)
			t.packet.insert(t.packet.end(), a, a + na);
		if (nb)
			t.packet.insert(t.packet.end(), b, b + nb);
		Writer w(t.packet.size() + 8);
		if (!t.write(w))
		{
			if (m_logged_size.insert(type).second)
				say("%s of %zu bytes is over what a Tunnel carries: dropped", msg_name(type), t.packet.size());
			return 0;
		}
		m_level_world->send(kHostPeer, lane, Msg::Tunnel, w);
		return t.packet.size();
	}

private:
	ServerPart() = default;

	bool configured() const { return !op::token_none(m_token.data(), kHelloTokenSize); }

	// What rights_of would answer, with nothing bound and nothing logged: asked before the place is given.
	bool would_be_operator(const std::string& address, const HelloMsg& hello) const
	{
		const bool presented = !op::token_none(hello.token, kHelloTokenSize);
		const bool bound = m_bound && op::token_equal(m_bound_token.data(), m_token.data(), kHelloTokenSize);
		const bool matches = op::token_equal(hello.token, m_token.data(), kHelloTokenSize) && presented && configured();
		return op::operator_verdict(mapserver::loopback_address(address), matches, bound ? &m_bound_key : nullptr, hello.key) ==
		    op::Verdict::Operator;
	}

	// One line of the session's log from a format of this file; what came over the network is only ever an argument.
	void say(const char* fmt, ...) const
	{
		char buf[1024];
		va_list ap;
		va_start(ap, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		buf[sizeof(buf) - 1] = 0;
		m_owner->log(buf);
	}

	// A line for the log as the session cleans a chat line: a tab and the line breaks become a blank, other control characters go.
	static void printable(std::string& s)
	{
		std::string out;
		out.reserve(s.size());
		for (unsigned char ch : s)
		{
			if (ch == '\t' || ch == '\n' || ch == '\r')
				out.push_back(' ');
			else if (ch >= 0x20 && ch != 0x7F)
				out.push_back(char(ch));
		}
		s.swap(out);
	}

	static void wipe(Token& t)
	{
		volatile uint8_t* p = t.data();
		for (size_t i = 0; i < t.size(); ++i)
			p[i] = 0;
	}

	Owner* m_owner = nullptr;
	// the bridges: token -> k, given out and not used yet
	std::map<mapserver::Token, uint32_t> m_tokens;
	// the operator: the token armed for the next host(), the one of this host, and the player key it is bound to for the life of the
	// Session (a new token in a later host() unbinds it)
	Token m_armed{};
	Token m_token{};
	bool m_bound = false;
	op::Key m_bound_key{};
	Token m_bound_token{};
	uint64_t m_refused = 0; // OperatorCommands refused after the rate limit, for the life of the Session
	// a map server's level: the world connection that carries the next host(), and the one that carries this one
	Session* m_level_next = nullptr;
	Session* m_level_world = nullptr;
	std::set<uint16_t> m_logged_size; // message types once logged as too long for a Tunnel
};

// The view: the server's API of a Session. Asking makes no part; setting makes the server's part the first time (Session::part). On a
// Session that holds no server's part (a co-op one) every question answers "none".
class SessionSrv
{
public:
	using Token = ServerPart::Token;
	using BridgeJoined = ServerPart::BridgeJoined;
	using BridgeLeft = ServerPart::BridgeLeft;
	using OperatorLine = ServerPart::OperatorLine;

	explicit SessionSrv(Session& s) : m_s(s) {}

	// ------------------------------------------------------------------ a world server

	// The operator's token of this Session's next host() (xrCoop/coop_operator.h; all zeros: no operator). The one way the token comes
	// in: the world server's mode arms it in its configure (srv/coop_srv_mode.cpp). The part keeps its own copy for that host() only;
	// the caller wipes its own. The first player key that presents it from the loopback becomes the operator for the life of the Session
	// (op::operator_verdict).
	void arm_operator(const Token& token) const
	{
		if (ServerPart* p = made())
			p->arm_operator(token);
	}
	// A line of the operator that passed the rate, the sender, the length and the list (op::command_verdict): the world server runs it
	// on its console (dedicated::run_for). Kept across sessions, like the handlers.
	void on_operator_command(OperatorLine fn) const
	{
		if (ServerPart* p = made())
			p->operator_line = std::move(fn);
	}
	// Operator lines refused after the rate limit, for the life of the Session.
	uint64_t operator_refused() const
	{
		const ServerPart* p = found();
		return p ? p->refused() : 0;
	}

	// The bridges of its map servers. Map server k (1 .. mapserver::kMaxBridges) connects once with the token issued for it, from
	// this PC; its peer id is mapserver::bridge_peer(k). Issuing again for k replaces its token; a k out of range is ignored, and so
	// is any call on a session that does not host.
	void issue_bridge(uint32_t k, const Token& token) const
	{
		if (ServerPart* p = made())
			p->issue(k, token);
	}
	void revoke_bridge(uint32_t k) const // a token nobody used goes: its process was given up on
	{
		if (ServerPart* p = found())
			p->revoke(k);
	}
	std::vector<PeerId> bridges() const // the connected ones
	{
		const ServerPart* p = found();
		return p ? p->owner().trunks() : std::vector<PeerId>();
	}
	bool bridge_connected(PeerId id) const
	{
		const ServerPart* p = found();
		return p && p->owner().trunk(id);
	}
	// Kept across sessions; called from Session::update.
	void on_bridge_joined(BridgeJoined fn) const
	{
		if (ServerPart* p = made())
			p->bridge_joined = std::move(fn);
	}
	void on_bridge_left(BridgeLeft fn) const
	{
		if (ServerPart* p = made())
			p->bridge_left = std::move(fn);
	}

	// ------------------------------------------------------------------ a map server

	// The next host() of this Session hosts its level through `world`, its connection to the world server (a client of the world with
	// Hello role MapServer), which must outlive this session. Its peers are the players of the level in the world server's numbering
	// (virtual_join); what the level's code sends one goes up `world` in a Msg::Tunnel, what he sent comes down in one
	// (virtual_receive). send(kHostPeer) and what a session does as a client go to `world`; self() is its bridge. The map server's
	// mode sets it at its install; Manager::start_map_server then connects `world` and hosts.
	void level_through(Session& world) const
	{
		if (ServerPart* p = made())
			p->level_through(world);
	}
	// level_through(world) and host(cfg) at once. false + err when the session is active already.
	bool host_virtual(const SessionConfig& cfg, Session& world, std::string* err) const
	{
		if (m_s.role() != Role::None)
		{
			if (err)
				*err = "session is already active";
			return false;
		}
		level_through(world);
		return m_s.host(cfg, err);
	}
	bool is_virtual() const
	{
		const ServerPart* p = found();
		return p && p->carrying();
	}
	void virtual_join(PeerId id, const std::string& nick) const // on_peer_joined follows
	{
		if (ServerPart* p = made())
			p->owner().carried_join(id, nick);
	}
	void virtual_leave(PeerId id, const std::string& reason) const // on_peer_left follows
	{
		if (ServerPart* p = made())
			p->owner().carried_leave(id, reason);
	}
	// A game message of player `from` out of a Tunnel (the 2-byte type first). Dropped if he is no player of this level.
	void virtual_receive(PeerId from, Lane lane, const uint8_t* packet, size_t size) const
	{
		if (ServerPart* p = made())
			p->owner().carried_receive(from, lane, packet, size);
	}

	Session& session() const { return m_s; }

private:
	ServerPart* found() const { return static_cast<ServerPart*>(m_s.part(ServerPart::kind(), nullptr)); }
	ServerPart* made() const { return static_cast<ServerPart*>(m_s.part(ServerPart::kind(), &ServerPart::make)); }

	Session& m_s;
};
} // namespace srv
} // namespace coop
