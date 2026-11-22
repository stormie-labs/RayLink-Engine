// Кооп: только там, где хост играет сам (docs/MODES.md)
// Anomaly Coop — the co-op's modes (xrCoop/coop_mode.h, docs/MODES.md section 4): the co-op host, who plays and leads his one map for
// everybody, and a co-op client. Nothing of the server is here: no map server, no bridge, no level filter, no transfer.
//
// What changed against the code of build 57 (S4, docs/plans/2026-11-18-two-modes.md): the bodies of net::host_broadcast,
// host_stream and client_send for these two processes, and the co-op host's handler tables (Manager::install_handlers, Hosting),
// without the server's: mapserver::install_server, the transfers' tables and the in-filter of the levels are the server modes'. In
// the co-op each of those found nothing to do (no bridge, no map server, split levels off), so a co-op process sees the same.
#include "stdafx.h"
#include "coop/coop_manager.h"
#include "coop/coop_join.h"
#include "coop/coop_net.h"
#include "coop/coop_sync.h"
#include "coop/coop_world.h"
#include "coop_hostplay_steam.h" // INTEG-C: Steam is the co-op's (docs/MODES.md section 8)
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_roles.h"

namespace coop
{
namespace join
{
void refuse_level_move(PeerId from, Reader& r); // coop_join.cpp: decode and reply with Refused/Invalid
}
namespace hostplay
{
namespace
{
// tests/test_mode_answers.cpp reads these rows and compares them with the table of tests/support/fake_mode.h
constexpr ModeAnswers kCoopHost{/*actor_is_player*/ true, /*decides_world*/ true, /*leads_level*/ true, /*group_travels*/ true,
    host_roles()};
constexpr ModeAnswers kCoopClient{/*actor_is_player*/ true, /*decides_world*/ false, /*leads_level*/ false, /*group_travels*/ true,
    client_roles()};

// The co-op host: the world and the one map are his process's, he plays (his character is the actor, 0), every player's character
// stands on his map.
class CoopHostMode final : public ModeWithAnswers
{
public:
	CoopHostMode() : ModeWithAnswers(kCoopHost) {}
	const char* name() const override { return "co-op host"; }

	// R32 W1: the host writes every object of its world (§8a A8: said here, not left to Mode's default).
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
	bool facts_of(u16, levels::BodyFacts&) const override { return false; } // his objects are here: he reads them

	// To every player of the session in the world (queued for a loading one), and to the host player over the LocalLink.
	void deliver(Msg type, const Writer& payload, Lane lane, PeerId except) override
	{
		Session& s = mgr().session();
		for (const PeerInfo& p : s.peers())
			if (p.id != except && p.state == PeerState::Joined)
				net::to_peer(p.id, type, payload, lane);
		if (except != kHostPeer && s.local())
			s.send(kHostPeer, lane, type, payload); // the link's gate decides (R1 5: no players() vector per message)
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
		cfg.host_plays = true; // protocol 31: his seat plays (Welcome.flags)
		cfg.mode = session_mode_of_seat(cfg.host_plays); // protocol 33: the Welcome's mode
		cfg.max_players = 8; // F5-SEAT-1 (TZ 6.7): co-op stays 8 seats, the host + 7 clients, as the Steam lobby
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
	void frame() override {}
	void session_over() override {}

	// No map server in the co-op: every object is the host's, online or offline here.
	bool online_elsewhere(u16) const override { return false; }
	bool binder_may_update(CGameObject*) override { return true; }
	void held_update() override {}
	void switched_offline(u16) override {}
	bool takes_own_ids() const override { return false; }
	u16 own_free_id() override { return 0xffff; }

	// A-S6-IF (S6): one map, his own books. Everything reaches every player; his only link is the host player's (A-S5S); every
	// character stands on his map, nobody joins elsewhere or waits for another process; his actor is a player.
	LevelSide* level() override { return nullptr; }
	bool reaches(PeerId, Msg) const override { return true; }
	void deliver_links(Msg type, const Writer& payload, Lane lane) override
	{
		Session& s = mgr().session();
		if (s.local())
			s.send(kHostPeer, lane, type, payload); // the host player over the LocalLink (his link's gate decides)
	}
	bool led_elsewhere(PeerId) const override { return false; }
	bool to_level_of(PeerId, Msg, const Writer&, Lane) override { return false; }
	bool join_from_bridge(PeerId, Msg) override { return false; }
	bool join_save(PeerId, CSE_ALifeHumanStalker&) override { return false; }
	bool level_ready(u16) const override { return true; }
	void on_world_live() override {}
	const char* load_text(u16, bool) const override { return nullptr; }
	bool ghost_said(const ghost::Said&) const override { return false; }
	// H-OPCMD (INTEG-C): no world server in the co-op: an operator's line has nowhere to go
	OperatorSend send_operator_command(const std::string&) override { return OperatorSend::Unreachable; }
	// VEH-DISC: his session is the world's and every character stands on his one map: whoever leaves it quit the game (his group's
	// move to another map releases the seats before it starts, coop_join.cpp release_all_seats)
	Departure departure(PeerId, u16) const override { return Departure::Disconnected; }
};

// A co-op client: he plays on the host's map, the host decides for every character, his objects are copies.
class CoopClientMode final : public ModeWithAnswers
{
public:
	CoopClientMode() : ModeWithAnswers(kCoopClient) {}
	const char* name() const override { return "co-op client"; }

	// R32 W1 (§8a A8): a player's machine writes nothing; its host writes every object it has a copy of.
	writer::Writer writer_of(const writer::RootFacts&) const override { return writer::other(kHostPeer); }

	Where where(PeerId) const override { return Where::Here; } // the host's one map
	Decider decider_of(u16 character) const override
	{
		return character == 0 || world::avatar_peer(character) != kInvalidPeer ? Decider::Other : Decider::Nobody;
	}
	bool facts_of(u16, levels::BodyFacts&) const override { return false; }

	void deliver(Msg, const Writer&, Lane, PeerId) override {} // a client changes nothing of the world for the others
	void stream(Msg, const Writer&, PeerId) override {}
	void up(Msg type, const Writer& payload, Lane lane) override { mgr().session().send(kHostPeer, lane, type, payload); }

	void configure(SessionConfig&) const override {} // a client's config is made before its Welcome
	void install(ModeTables&) override {} // the Core's client tables are there from its start (Manager::start_client)
	void frame() override {}
	void session_over() override {}

	bool online_elsewhere(u16) const override { return false; }
	bool binder_may_update(CGameObject*) override { return true; }
	void held_update() override {}
	void switched_offline(u16) override {}
	bool takes_own_ids() const override { return false; }
	u16 own_free_id() override { return 0xffff; }

	// A-S6-IF (S6): he sends nobody anything of the world, joins nobody, and his loading screen waits for the players (the Core's line).
	LevelSide* level() override { return nullptr; }
	bool reaches(PeerId, Msg) const override { return true; }
	void deliver_links(Msg, const Writer&, Lane) override {}
	bool led_elsewhere(PeerId) const override { return false; }
	bool to_level_of(PeerId, Msg, const Writer&, Lane) override { return false; }
	bool join_from_bridge(PeerId, Msg) override { return false; }
	bool join_save(PeerId, CSE_ALifeHumanStalker&) override { return false; }
	bool level_ready(u16) const override { return true; }
	void on_world_live() override {}
	const char* load_text(u16, bool) const override { return nullptr; }
	bool ghost_said(const ghost::Said&) const override { return false; }
	// H-OPCMD (INTEG-C): no world server in the co-op: an operator's line has nowhere to go
	OperatorSend send_operator_command(const std::string&) override { return OperatorSend::Unreachable; }
	// VEH-DISC: it seats nobody; a peer that leaves the host's session quit the game
	Departure departure(PeerId, u16) const override { return Departure::Disconnected; }
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
	// (INTEG-C) and cancels them there.
	bool boot_frame() override { return false; }
	// INTEG-C (rv_A-S6-E-net fix 1): the Steam invite the launcher accepted, polled before a session as during one (every process, as
	// the Core's autostart polled it: a server's process has no RAYLINK_STEAM_JOIN)
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
