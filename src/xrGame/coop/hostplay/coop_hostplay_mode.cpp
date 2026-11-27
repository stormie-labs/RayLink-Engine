// Кооп: только там, где хост играет сам (docs/MODES.md)
// Anomaly Coop — the co-op's modes (xrCoop/coop_mode.h, docs/MODES.md section 4): the co-op host, who plays and leads his one map for
// everybody, and a co-op client. Nothing of the server is here: no map server, no bridge, no level filter, no transfer.
// The modes say where net::host_broadcast, host_stream and client_send go in these two processes, and which handler tables a
// co-op host installs.
#include "stdafx.h"
#include "coop/coop_manager.h"
#include "coop/coop_join.h"
#include "coop/coop_net.h"
#include "coop/coop_sync.h"
#include "coop/coop_world.h"
#include "coop_hostplay_steam.h" // Steam is the co-op's (docs/MODES.md section 8)
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_roles.h"

namespace coop
{
namespace hostplay
{
namespace
{
// tests/test_mode_answers.cpp reads these rows and compares them with the table of tests/support/fake_mode.h
constexpr ModeAnswers kCoopHost{/*actor_is_player*/ true, /*decides_world*/ true, /*leads_level*/ true, /*group_travels*/ true,
    host_roles()};
constexpr ModeAnswers kCoopClient{/*actor_is_player*/ true, /*decides_world*/ false, /*leads_level*/ false, /*group_travels*/ true,
    client_roles()};

// What both co-op processes answer alike: one map, the host's; no map server, so every object is the host's, online or offline
// there; no world server to send an operator's line to. Each answer is said here, none is left to Mode's defaults.
class CoopModeBase : public ModeWithAnswers
{
public:
	using ModeWithAnswers::ModeWithAnswers;

	bool facts_of(u16, levels::BodyFacts&) const override { return false; } // the objects are on the host's map
	void frame() override {}
	void session_over() override {}

	bool online_elsewhere(u16) const override { return false; }
	bool binder_may_update(CGameObject*) override { return true; }
	void held_update() override {}
	void switched_offline(u16) override {}
	bool takes_own_ids() const override { return false; }
	u16 own_free_id() override { return kNoId; }

	// Every character stands on the host's map: nobody joins elsewhere or waits for another process; the actor is a player.
	LevelSide* level() override { return nullptr; }
	bool reaches(PeerId, Msg) const override { return true; }
	bool led_elsewhere(PeerId) const override { return false; }
	bool to_level_of(PeerId, Msg, const Writer&, Lane) override { return false; }
	bool join_from_bridge(PeerId, Msg) override { return false; }
	bool join_save(PeerId, CSE_ALifeHumanStalker&) override { return false; }
	bool level_ready(u16) const override { return true; }
	void on_world_live() override {}
	const char* load_text(u16, bool) const override { return nullptr; }
	bool ghost_said(const ghost::Said&) const override { return false; }
	// no world server in the co-op: an operator's line has nowhere to go
	OperatorSend send_operator_command(const std::string&) override { return OperatorSend::Unreachable; }
	// the session is the world's and every character stands on the host's one map: a peer who leaves it quit the game (the
	// group's move to another map releases the seats before it starts, coop_join.cpp release_all_seats)
	Departure departure(PeerId, u16) const override { return Departure::Disconnected; }
};

// The co-op host: the world and the one map are his process's, he plays (his character is the actor, 0), every player's character
// stands on his map.
class CoopHostMode final : public CoopModeBase
{
public:
	CoopHostMode() : CoopModeBase(kCoopHost) {}
	const char* name() const override { return "co-op host"; }

	// the host writes every object of its world
	writer::Writer writer_of(const writer::RootFacts&) const override { return writer::this_process(); }

	Where where(PeerId peer) const override
	{
		if (peer != kHostPeer && sync::host_peer_joining(peer))
			return Where::Joining; // his character comes with the world he loads (WorldLoaded)
		return Where::Here; // one map for everybody
	}
	Decider decider_of(u16 character) const override
	{
		return character == 0 || join::peer_of_proxy(character) != kInvalidPeer ? Decider::This : Decider::Nobody;
	}

	// To every player of the session in the world (queued for a loading one), and to the host player over the LocalLink.
	void deliver(Msg type, const Writer& payload, Lane lane, PeerId except) override
	{
		Session& s = mgr().session();
		s.for_each_joined([&](const PeerInfo& p) {
			if (p.id != except && p.state == PeerState::Joined)
				net::to_peer(p.id, type, payload, lane);
		});
		if (except != kHostPeer && s.local())
			s.send(kHostPeer, lane, type, payload); // the link's gate decides
	}
	void stream(Msg type, const Writer& payload, PeerId except) override
	{
		mgr().session().broadcast(Lane::Unreliable, type, payload, except);
	}
	// The host player is a client of his own process: what he sends goes over the LocalLink to its handlers (from = 0).
	void up(Msg type, const Writer& payload, Lane lane) override
	{
		if (mgr().session().local())
			mgr().local_link().push_to_server(type, lane, payload);
	}

	void configure(SessionConfig& cfg) const override
	{
		cfg.host_plays = true; // his seat plays (Welcome.flags)
		cfg.mode = session_mode_of_seat(cfg.host_plays); // the Welcome's mode
		cfg.max_players = 8; // co-op has 8 seats, the host + 7 clients, as the Steam lobby
	}
	// The world server's handlers on the session, the host player's (a client of it) on the link. No server table: no in-filter.
	void install(ModeTables& tables) override
	{
		tables.session.set_in_filter(nullptr);
		join::install_server(tables.session);
		tables.session.on(Msg::LevelMove, &join::refuse_level_move);
		sync::install_server(tables.session);
		join::install_client(tables.link);
		sync::install_client(tables.link);
	}

	// the host player's link is his only one
	void deliver_links(Msg type, const Writer& payload, Lane lane) override
	{
		Session& s = mgr().session();
		if (s.local())
			s.send(kHostPeer, lane, type, payload); // the host player over the LocalLink (his link's gate decides)
	}
};

// A co-op client: he plays on the host's map, the host decides for every character, his objects are copies.
class CoopClientMode final : public CoopModeBase
{
public:
	CoopClientMode() : CoopModeBase(kCoopClient) {}
	const char* name() const override { return "co-op client"; }

	// a player's machine writes nothing; its host writes every object it has a copy of
	writer::Writer writer_of(const writer::RootFacts&) const override { return writer::other(kHostPeer); }

	Where where(PeerId) const override { return Where::Here; } // the host's one map
	Decider decider_of(u16 character) const override
	{
		return character == 0 || world::avatar_peer(character) != kInvalidPeer ? Decider::Other : Decider::Nobody;
	}

	void deliver(Msg, const Writer&, Lane, PeerId) override {} // a client changes nothing of the world for the others
	void stream(Msg, const Writer&, PeerId) override {}
	void up(Msg type, const Writer& payload, Lane lane) override { mgr().session().send(kHostPeer, lane, type, payload); }

	void configure(SessionConfig&) const override {} // a client's config is made before its Welcome
	void install(ModeTables&) override {} // the Core's client tables are there from its start (Manager::start_client)

	// he sends nobody anything of the world, and his loading screen waits for the players (the Core's line)
	void deliver_links(Msg, const Writer&, Lane) override {}
};

CoopHostMode& host_mode()
{
	static CoopHostMode m;
	return m;
}

CoopClientMode& client_mode()
{
	static CoopClientMode m;
	return m;
}

class CoopLayer final : public Layer
{
public:
	const char* name() const override { return "co-op"; }
	// -coop_host and -coop_connect stay with the Core's autostart (coop_manager.cpp); a Steam invite comes to it through join_request
	// and cancels them there.
	bool boot_frame() override { return false; }
	// The Steam invite the launcher accepted, polled before a session as during one (every process, as the Core's autostart polled
	// it: a server's process has no RAYLINK_STEAM_JOIN)
	bool join_request(std::string& address, std::string& via) override
	{
		if (!steam::poll(mgr().now_ms(), address))
			return false;
		via = "a Steam invite";
		return true;
	}
	// coop.steam_invite_ready / coop.steam_invite, after the Core's coop.* (coop_script.cpp register_layers_lua)
	void register_lua(lua_State* L) override { steam::register_lua(L); }
	// A host that the server layer did not take is the co-op host; a client is the co-op's when its Welcome says so.
	Mode* mode_of(Entry entry, SessionMode said) override
	{
		switch (entry)
		{
		case Entry::Host: return &host_mode();
		case Entry::Client: return said == SessionMode::Coop ? &client_mode() : nullptr;
		case Entry::MapServer: break;
		}
		return nullptr;
	}
};
} // namespace

Layer& layer()
{
	static CoopLayer l;
	return l;
}
} // namespace hostplay
} // namespace coop
