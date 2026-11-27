#include "stdafx.h"
#include "pch_script.h"
#include "coop_join_impl.h"
#include "coop_manager.h"
#include "coop_sync.h"
#include "coop_hud_text.h"
#include "coop_entities.h"
#include "coop_net.h"
#include "coop_survival.h"
#include "xrCoop/coop_shapes.h"
#include "xrCoop/coop_mode.h"

#include "../../xrEngine/CameraBase.h"
#include "ai_space.h"
#include "coop_lua.h"
#include "level_graph.h"
#include "Level.h"
#include "Actor.h"
#include "level_changer.h"
#include "space_restrictor.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"

extern xr_vector<CLevelChanger*> g_lchangers; // level_changer.cpp: the exits of the level that are online

namespace coop
{
namespace join
{
BoardState g_board;

namespace
{
// How near a player must be to a level exit to claim it, metres from its nearest shape.
// Stock: an exit takes the actor when its shapes touch his bounding sphere (CCF_Shape::Contact, about 1 m around the
// middle of the body). The host sees the player's character where the player was 0.1-0.3 s ago (interpolation delay
// and one-way latency): 2-3 m back at a sprint. 5 m covers both (transition::kReachM, tested).
constexpr float kVoteReachM = transition::kReachM;

// Host: after the group is sent off the old world still runs a few frames (the level change is a message the world takes in
// the next frames); nobody claims an exit in them, or the group would go again from a world that is about to go.
constexpr u64 kAfterReleaseMs = 4000;

} // namespace

// A level changer of the level the host is on, from its own ALife, nullptr if `id` is none (or one the
// host's scripts have switched off). The level change is built from its data, never from a client's bytes.
CSE_ALifeLevelChanger* exit_here(u16 id)
{
	if (id == kNoId || !g_pGameLevel || !ai().get_alife())
		return nullptr;
	CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(ai().alife().objects().object(id, true));
	if (!lc)
		return nullptr;
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(lc->m_tGraphID) || !gg.valid_vertex_id(lc->m_tNextGraphID) ||
	    gg.vertex(lc->m_tGraphID)->level_id() != ai().alife().graph().level().level_id())
		return nullptr;
	CObject* object = Level().Objects.net_Find(id);
	const CLevelChanger* changer = object ? object->cast_level_changer() : nullptr;
	if (changer && !changer->IsLevelChangerEnabled())
		return nullptr;
	return lc;
}

namespace
{

// The level changer of this level whose destination our engine wrote into a level change packet
// (CLevelChanger and its dialog write the changer's own m_tNext* values), kNoId if none (jump_to_level and the like).
u16 level_changer_of(NET_Packet& P)
{
	constexpr u32 kBody = sizeof(GameGraph::_GRAPH_ID) + sizeof(u32) + 2 * sizeof(Fvector);
	if (!ai().get_alife() || P.B.count < sizeof(u16) + kBody)
		return kNoId;
	const u32 read_at = P.r_pos;
	P.r_pos = sizeof(u16); // after the message type
	GameGraph::_GRAPH_ID graph;
	u32 node;
	Fvector position, angles;
	P.r(&graph, sizeof(graph));
	P.r(&node, sizeof(node));
	P.r_vec3(position);
	P.r_vec3(angles);
	P.r_pos = read_at;
	const CGameGraph& gg = ai().game_graph();
	const GameGraph::_LEVEL_ID here = ai().alife().graph().level().level_id();
	for (const auto& it : ai().alife().objects().objects())
	{
		CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(it.second);
		if (lc && lc->m_tNextGraphID == graph && lc->m_dwNextNodeID == node && lc->m_tNextPosition.similar(position) &&
		    lc->m_tAngles.similar(angles) && gg.valid_vertex_id(lc->m_tGraphID) &&
		    gg.vertex(lc->m_tGraphID)->level_id() == here)
			return lc->ID;
	}
	return kNoId;
}

} // namespace

// Host: metres from `p` to the nearest shape of a level exit (shapes::kFar if it has none).
// The exit's transform is built as CGameObject::net_Spawn builds its XFORM, each box as CCF_Shape::Contact does.
// Any space restrictor's (a level changer is one): a route restrictor's, a route's drop point to a changer's.
float exit_distance(const CSE_ALifeSpaceRestrictor& lc, const Fvector& p)
{
	Fmatrix xf;
	xf.setXYZ(lc.o_Angle);
	xf.c.set(lc.o_Position);
	const float at[3] = {p.x, p.y, p.z};
	float best = shapes::kFar;
	for (const CShapeData::shape_def& s : lc.shapes)
	{
		float d = shapes::kFar;
		if (s.type == CShapeData::cfSphere)
		{
			Fvector center;
			xf.transform_tiny(center, s.data.sphere.P);
			const float c[3] = {center.x, center.y, center.z};
			d = shapes::to_sphere(at, c, s.data.sphere.R);
		}
		else if (s.type == CShapeData::cfBox)
		{
			Fmatrix box;
			box.mul_43(xf, s.data.box);
			const float i[3] = {box.i.x, box.i.y, box.i.z};
			const float j[3] = {box.j.x, box.j.y, box.j.z};
			const float k[3] = {box.k.x, box.k.y, box.k.z};
			const float c[3] = {box.c.x, box.c.y, box.c.z};
			d = shapes::to_box(at, i, j, k, c);
		}
		if (d < best)
			best = d;
	}
	return best;
}

// ---------------------------------------------------------------- the waiting point of a level change
// xrCoop/coop_transition.h has the rule and the messages. Every machine runs transition_step() each frame: it finds the
// exit zone its player stands in (the stock zone of a CLevelChanger, or a route restrictor that leads into
// one: route_touched), says so (TransitionVote: the host player's over the local link), or that he comes along from afar
// (the key held away from the exit: transition_consent), keeps the player from walking off the edge of the level while he
// waits, and keeps the numbers the Lua placard reads (coop.transition()). The host also runs the board (host_board_step):
// who is ready, when the group goes (the Going state: every screen darkens; kGoFadeMs later host_release, the same level
// change coop_accept_transition always made), and tells everybody (TransitionState).

// The stock name of the level game vertex `gv` is on ("l01_escape"), empty if unreadable.
std::string level_name_of(GameGraph::_GRAPH_ID gv)
{
	if (!ai().get_game_graph())
		return std::string();
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(gv))
		return std::string();
	const char* name = gg.header().level(gg.vertex(gv)->level_id()).name().c_str();
	return name ? std::string(name) : std::string();
}

// Host: is anybody else in the session? A player who has joined counts while he loads (the board waits for him, at most
// transition::kLoadWaitMs). With nobody else the host's level changes are the stock ones.
bool host_has_company()
{
	if (!mode().decides_world())
		return false;
	// wherever he stands: a player on another level is company too
	bool company = false;
	mgr().session().for_each_joined([&](const PeerInfo& p) { company = company || p.state == PeerState::Joined; });
	return company;
}

namespace
{
// The group waits at exits on this machine: the stock "go?" dialog and the silent change of the zone stay off there. Only where a
// person plays this process's actor: a ghost (a map server's, the world server's) has no exit to wait at.
bool group_waits_here()
{
	if (!mode().actor_is_player())
		return false; // nobody plays its actor
	return mode().decides_world() ? host_has_company() : mgr().is_live_client();
}
} // namespace

// A level exit anywhere in the world: a player of another level stands at it, the host consents from here. The game graph
// is every level's.
CSE_ALifeLevelChanger* level_changer_of_goal(u16 id)
{
	if (CSE_ALifeLevelChanger* here = exit_here(id))
		return here;
	if (id == kNoId || !ai().get_alife())
		return nullptr;
	CSE_ALifeLevelChanger* lc = smart_cast<CSE_ALifeLevelChanger*>(ai().alife().objects().object(id, true));
	return lc && ai().game_graph().valid_vertex_id(lc->m_tGraphID) && ai().game_graph().valid_vertex_id(lc->m_tNextGraphID) ? lc : nullptr;
}

namespace
{
// Host: a player the board counts as able to walk: survival::Life::Alive; one who is not in the world yet is taken for alive.
bool host_player_alive(const PeerInfo& p)
{
	if (p.local)
	{
		CActor* a = g_actor ? Actor() : nullptr;
		return a && a->g_Alive() && survival::life_of(0) == survival::Life::Alive;
	}
	const u16 proxy = proxy_of_peer(p.id);
	const bool in_world = !sync::host_peer_joining(p.id) && host_player_in_world(proxy);
	return !in_world || survival::life_of(proxy) == survival::Life::Alive;
}

// On the level's map: inside the AI map's box and the physics volume (past that the host refuses the player's state,
// "position outside the level"). The edge of the level is where a waiting player is held.
bool on_map(const Fvector& p)
{
	if (ai().get_level_graph() && !ai().level_graph().valid_vertex_position(p))
		return false;
	const Fbox& box = Level().ObjectSpace.GetBoundingVolume();
	return p.x >= box.x1 && p.x <= box.x2 && p.z >= box.z1 && p.z <= box.z2;
}

// The enabled exit whose zone this actor touches (the stock rule: CLevelChanger::feel_touch_contact), its id in HOST
// numbering, kNoId if none.
u16 exit_touched(CActor* a)
{
	for (CLevelChanger* lc : g_lchangers)
		if (lc && lc->IsLevelChangerEnabled() && lc->feel_touch_contact(a))
			return to_host(lc->ID());
	return kNoId;
}

// A space restrictor of the level the host is on that is no level changer (a route's mark), nullptr if
// `id` is none.
CSE_ALifeSpaceRestrictor* restrictor_here(u16 id)
{
	if (id == kNoId || !g_pGameLevel || !ai().get_alife())
		return nullptr;
	CSE_ALifeSpaceRestrictor* r = smart_cast<CSE_ALifeSpaceRestrictor*>(ai().alife().objects().object(id, true));
	if (!r || smart_cast<CSE_ALifeLevelChanger*>(r))
		return nullptr;
	const CGameGraph& gg = ai().game_graph();
	if (!gg.valid_vertex_id(r->m_tGraphID) || gg.vertex(r->m_tGraphID)->level_id() != ai().alife().graph().level().level_id())
		return nullptr;
	return r;
}

// Host: a space restrictor of `level` that is no level changer, the host's level or a map server's: there it is a record of the world.
CSE_ALifeSpaceRestrictor* restrictor_of_level(u16 id, levels::LevelId level)
{
	if (id == kNoId || !ai().get_alife())
		return nullptr;
	CSE_ALifeSpaceRestrictor* r = smart_cast<CSE_ALifeSpaceRestrictor*>(ai().alife().objects().object(id, true));
	return r && !smart_cast<CSE_ALifeLevelChanger*>(r) && levels::level_of_vertex(r->m_tGraphID) == level ? r : nullptr;
}

// The route restrictor (one this machine learned, coop.transition_route) this actor stands in, whose level
// changer is enabled: that changer in HOST numbering, kNoId if none; `route` gets the restrictor (host numbering). Inside is
// the restrictor's own test of the actor's position (as the stock on_actor_inside asks it, CScriptGameObject::inside), every
// frame: the restrictor's logic fires its sr_teleport once per entry of ANY player (coop_npc_logic evaluates it for the
// group), so a second player who walks in while the first stands there would never be told.
u16 route_touched(CActor* a, u16& route)
{
	route = kNoId;
	if (!a->g_Alive() || g_board.routes.all().empty())
		return kNoId;
	Fsphere at;
	at.P = a->Position();
	at.R = EPS_L;
	for (const transition::Routes::Entry& e : g_board.routes.all())
	{
		CObject* object = Level().Objects.net_Find(e.restrictor);
		const CSpaceRestrictor* r = object ? object->cast_restrictor() : nullptr;
		if (!r || !r->inside(at))
			continue;
		CObject* door = Level().Objects.net_Find(e.changer);
		const CLevelChanger* lc = door ? door->cast_level_changer() : nullptr;
		if (!lc || !lc->IsLevelChangerEnabled())
			continue;
		route = to_host(e.restrictor);
		return to_host(e.changer);
	}
	return kNoId;
}

// The level `goal` leads to, for the placard: an exit's own, or the guide trip's.
std::string goal_level_name(u32 goal)
{
	if (transition::is_guide(goal))
		return g_board.guide.level;
	const CSE_ALifeLevelChanger* lc = level_changer_of_goal(u16(goal));
	return lc ? level_name_of(lc->m_tNextGraphID) : std::string();
}
} // namespace

u16 level_id_of(GameGraph::_GRAPH_ID gv)
{
	if (!ai().get_game_graph() || !ai().game_graph().valid_vertex_id(gv))
		return levels::wire::kNoLevel;
	return u16(ai().game_graph().vertex(gv)->level_id());
}

// The stock name of game graph level `id` ("l01_escape"), empty if this machine's graph has no such level.
std::string level_name_by_id(u16 id)
{
	if (id == levels::wire::kNoLevel || !ai().get_game_graph())
		return std::string();
	const GameGraph::LEVEL_MAP& all = ai().game_graph().header().levels();
	const auto it = all.find(GameGraph::_LEVEL_ID(id));
	return it == all.end() ? std::string() : std::string(it->second.name().c_str());
}

namespace
{
// Host: what the board shows changed (who is ready and how, the state, the exit)? The distances do not count.
u32 board_signature(const transition::Status& st)
{
	u32 sig = 17u;
	sig = sig * 31u + u32(st.state);
	sig = sig * 31u + st.goal;
	sig = sig * 31u + st.ready;
	sig = sig * 31u + st.total;
	sig = sig * 31u + (st.load_ms != 0 ? 1u : 0u);
	for (const transition::Row& r : st.rows)
		sig = sig * 31u + r.id * 16u + u32(r.mark) * 2u + (r.consent ? 1u : 0u);
	return sig;
}

// Host: every player gets the board on a change (at most every 100 ms) and every kStateEveryMs while it is not idle, and
// once when it goes idle. The host player too, over the local link: his placard reads what the link brings him.
void host_board_send(const transition::Status& st, u64 now)
{
	const bool idle = st.state == transition::State::Idle;
	g_board.board_view = transition::state_message(st, idle ? std::string() : goal_level_name(st.goal));
	const u32 sig = board_signature(st);
	if (idle)
	{
		if (g_board.board_sent_idle)
			return;
	}
	else
	{
		const bool changed = g_board.board_sent_idle || sig != g_board.board_sent_sig;
		if (now - g_board.board_sent_at < (changed ? 100u : u64(transition::kStateEveryMs)))
			return;
	}
	g_board.board_sent_idle = idle;
	g_board.board_sent_sig = sig;
	g_board.board_sent_at = now;
	Writer w(48 + 8 * g_board.board_view.rows.size());
	g_board.board_view.write(w);
	// not queued: a loading client ignores it; the host player gets it over the local link (Session::broadcast)
	mgr().session().broadcast(Lane::Reliable, Msg::TransitionState, w);
}
} // namespace

// Host, ten times a second: the players as the board sees them, the board's verdict, the group's move.
void host_board_step(u64 now)
{
	if (now - g_board.board_ticked < 100)
		return;
	g_board.board_ticked = now;
	if (g_board.released_at != 0 && now - g_board.released_at < kAfterReleaseMs)
		return; // the group is on its way: the old world is about to go
	if (!g_pGameLevel || !ai().get_alife() || g_join.world_seen != g_world_generation)
		return; // no world (a level change, a load): nothing to run, the board was cleared with the old world
	// The group goes. Every player's screen darkens on the board's Going state (coop_hud_transition.script, the
	// host player's over the local link); the host's own level change starts kGoFadeMs after the go, so that his darkening is
	// on the screen before his process stops drawing to write the transition's save. Nothing else runs meanwhile.
	if (g_board.going)
	{
		if (now - g_board.going_at >= transition::kGoFadeMs)
		{
			g_board.going = false;
			host_release(g_board.going_goal);
		}
		else
			host_board_send(g_board.going_status, now); // the Going state again (every kStateEveryMs): a lost one is no darkening
		return;
	}
	const bool company = host_has_company();
	if (!company && g_board.board.empty() && g_board.board_view.state == transition::State::Idle)
		return;
	if (!company)
	{
		g_board.board.clear(); // the others left: the host's changes are the stock ones again
		g_board.guide.set = false;
	}
	// The members are the server's players(): the host player is peer 0 among them (over the local link), not a member
	// added apart; below the protocol his character is this process's actor. The board is the co-op's: everybody stands on
	// the host's level (the world server's has no claim: nobody claims an exit where the group does not travel together,
	// transition_step).
	std::vector<transition::Member> members;
	for (const PeerInfo& p : mgr().session().players())
	{
		if (p.state != PeerState::Joined)
			continue;
		transition::Member m;
		m.id = p.id;
		if (p.local)
		{
			m.remote = false; // the local link loses nothing: his claim needs no refresh to live
			m.in_world = g_actor != nullptr;
		}
		else
			m.in_world = !sync::host_peer_joining(p.id) && host_player_in_world(proxy_of_peer(p.id));
		m.alive = host_player_alive(p);
		members.push_back(m);
	}
	// metres from a player to an exit's changer, or to the route restrictor a claim of his stands in (its place)
	const transition::DistFn dist = [](u32 id, u32 where) -> float
	{
		if (transition::is_guide(where) || where == transition::kNoPlace)
			return -1.f;
		CObject* body = id == 0 ? (g_actor ? Actor() : nullptr) : Level().Objects.net_Find(proxy_of_peer(id));
		if (where == transition::kTripPlace)
		{
			// the one who asked for a trip, on a map server's level too: there the world has his record
			const CSE_ALifeDynamicObject* record = !body && ai().get_alife() ? ai().alife().objects().object(proxy_of_peer(id), true) : nullptr;
			return body ? body->Position().distance_to(g_board.guide.from_position) : record ? record->o_Position.distance_to(g_board.guide.from_position) : -1.f;
		}
		if (!body)
			return -1.f;
		if (transition::is_route_place(where))
		{
			const CSE_ALifeSpaceRestrictor* r = restrictor_here(transition::route_of_place(where));
			return r ? exit_distance(*r, body->Position()) : -1.f;
		}
		const CSE_ALifeLevelChanger* lc = exit_here(u16(where));
		return lc ? exit_distance(*lc, body->Position()) : -1.f;
	};
	const transition::Status st = g_board.board.tick(members, dist, now);
	g_board.board_lead_known = st.state != transition::State::Idle;
	g_board.board_lead = st.goal;
	if (st.expired)
		::Msg("[coop] transition: the guide trip lapsed, not everybody said yes in %u s", transition::kGuideLifeMs / 1000);
	if (st.called_off)
		::Msg("[coop] transition: the trip to '%s' is off, '%s' took it back", g_board.guide.level.c_str(), nick_of_peer(g_board.guide.by).c_str());
	if (st.state == transition::State::Idle)
		g_board.guide.set = false;
	if (st.go)
	{
		g_board.board_sent_at = 0; // the Going state goes out now, whatever went out a moment ago
		g_board.going = true;
		g_board.going_at = now;
		g_board.going_goal = st.goal;
		g_board.going_status = st;
		::Msg("[coop] transition: %u of %u ready, the group goes to '%s' (the screens darken, the load in %u ms)", st.ready,
		    st.total, goal_level_name(st.goal).c_str(), transition::kGoFadeMs);
	}
	host_board_send(st, now);
}

// Both roles, every frame. See the header of this section.
void transition_step(u64 now)
{
	CActor* a = (g_pGameLevel && g_actor && ai().get_alife()) ? Actor() : nullptr;
	if (!a || !group_waits_here())
	{
		g_board.want = false;
		g_board.zone_in = false;
		g_board.zone_changer = kNoId;
		g_board.zone_route = kNoId;
		g_board.withdrawn_from = kNoId;
		g_board.guide_yes = false;
		g_board.consent = false;
		g_board.hold.reset();
		g_board.intent.reset();
		return; // the host's board drops his claim by itself (host_board_step)
	}
	u16 route = kNoId;
	u16 touched = exit_touched(a);
	if (touched == kNoId)
		touched = route_touched(a, route); // a route restrictor into a changer is that changer's exit
	const bool alive = a->g_Alive() && survival::life_of(0) == survival::Life::Alive;
	if (touched == kNoId || touched != g_board.withdrawn_from)
		g_board.withdrawn_from = kNoId; // he left the zone he had withdrawn from (or stands in another one)
	if (touched == kNoId)
	{
		g_load.move_refused = kNoId;
		g_load.move_board = false; // he left the zone: the next exit is asked about anew
	}
	if (!mode().group_travels() && alive && touched != kNoId && !g_load.move_board)
		client_level_move(touched, now); // no board at a free exit, he goes alone (a server's player; it checks he is live)
	if (!mode().decides_world() && g_load.move_changer == kNoId && g_load.move_sent_at != 0 && now - g_load.move_sent_at >= kTripAnswerMs)
	{
		g_load.move_sent_at = 0;
		::Msg("! [coop] a trip: no answer from the host in %u s", u32(kTripAnswerMs / 1000));
		client_stays_on_level();
		hud_text::say("st_coop_levels_refused_invalid", "This exit cannot be used right now.");
	}
	// where the group does not travel together nobody claims an exit but a player the decider sent to the board (a level entered together)
	g_board.zone_in = touched != kNoId && alive && (mode().group_travels() || g_load.move_board);
	g_board.zone_changer = touched;
	g_board.zone_route = g_board.zone_in ? route : kNoId;
	bool want = g_board.zone_in && touched != g_board.withdrawn_from;
	u32 goal = want ? transition::goal_of_exit(touched) : 0;
	u8 op = route != kNoId ? transition::VoteRoute : transition::VoteReady;
	// At an exit he is there (it replaces "go with the group"); away from it, the key he held (transition_consent)
	// says he comes along while the board waits at that exit (client_on_transition_state ends it with that wait)
	if (g_board.zone_in)
		g_board.consent = false;
	else if (g_board.consent && alive && g_board.view.state != transition::State::Idle && g_board.view.kind == 0 &&
	    u32(g_board.view.changer) == g_board.consent_goal)
	{
		want = true;
		goal = g_board.consent_goal;
		op = transition::VoteConsent;
	}
	// A yes to the guide trip on the board (a client's confirm; the host player's game asked for the trip, host_gate_level_change)
	// holds while the board shows that trip; the board's message clears it when the trip is over (client_on_transition_state).
	if (g_board.guide_yes && g_board.view.state != transition::State::Idle && g_board.view.kind == 1)
	{
		want = true;
		goal = transition::kGuideGoal;
		op = transition::VoteReady;
	}
	if (want && !g_board.want)
		g_board.held_note = false;
	g_board.want = want;
	g_board.want_goal = goal;
	// Every player says it with TransitionVote, the host player too: net::client_send puts his on the local link and
	// the world server takes it as anybody's (host_on_transition_vote, from = 0). His placard's view comes back the same way.
	{
		const u32 self = mgr().self();
		bool view_ready = false;
		if (g_board.view.state != transition::State::Idle)
			for (const transition::Row& r : g_board.view.rows)
				view_ready = view_ready || (r.id == self && r.mark == transition::Mark::Ready);
		const u32 view_goal = g_board.view.kind == 1 ? transition::kGuideGoal : u32(g_board.view.changer);
		const transition::Send send = g_board.intent.wants(want, goal, view_ready, view_goal, now, op);
		if (send != transition::Send::Nothing && g_board.vote_pacer.take(Msg::TransitionVote, now))
		{
			const bool ready = send == transition::Send::Ready;
			transition::VoteMsg v;
			v.op = ready ? op : transition::VoteCancel;
			v.changer = (ready && !transition::is_guide(goal)) ? u16(goal) : transition::kNoChanger;
			v.route = ready && op == transition::VoteRoute ? g_board.zone_route : transition::kNoChanger;
			Writer w(8);
			v.write(w);
			net::client_send(Msg::TransitionVote, w);
			g_board.intent.sent(now, ready ? op : g_board.intent.said);
			++g_board.votes_sent;
		}
	}
	// The hold: a player waiting at an exit does not walk off the edge of the level (one who comes along from afar is free).
	const Fvector pos = a->Position();
	const float at[3] = {pos.x, pos.y, pos.z};
	float back[3] = {0.f, 0.f, 0.f};
	if (g_board.hold.step(want && !transition::is_guide(goal) && op != transition::VoteConsent, g_board.zone_in, on_map(pos), at, back))
	{
		Fvector to;
		to.set(back[0], back[1], back[2]);
		CCameraBase* cam = a->cam_Active();
		Fvector dir;
		dir.set(cam ? -cam->pitch : 0.f, cam ? -cam->yaw : 0.f, 0.f); // the view stays as it is (MoveActor sets it)
		a->MoveActor(to, dir);
		++g_board.holds_applied;
		if (!g_board.held_note)
		{
			g_board.held_note = true;
			::Msg("[coop] transition: waiting at an exit, the edge of the level holds (put back %.1f m)", pos.distance_to(to));
		}
	}
	// The numbers the placard reads, ten times a second
	if (now - g_board.zone_scanned >= 100)
	{
		g_board.zone_scanned = now;
		g_board.exit_known = false;
		g_board.my_dist = -1.f;
		if (g_board.view.state != transition::State::Idle && g_board.view.kind == 0)
		{
			const CSE_ALifeLevelChanger* lc = exit_here(to_local(g_board.view.changer));
			if (lc)
			{
				g_board.exit_known = true;
				g_board.exit_pos = lc->o_Position;
				g_board.my_dist = exit_distance(*lc, pos);
			}
		}
	}
}

// A new world (a level change, a load, the end of the session): nothing of the old one counts.
void transition_world_replaced()
{
	g_load.move_changer = kNoId;
	g_load.move_sent_at = 0;
	g_load.move_refused = kNoId;
	g_load.move_held = false; // a move held for his detach was of the old world
	g_load.move_hold.clear();
	g_load.move_board = false;
	g_board.released_at = 0; // the world the group was sent off from is gone
	g_board.board.clear();
	g_board.guide.set = false;
	g_board.board_lead_known = false;
	g_board.board_sent_idle = true;
	g_board.view = transition::StateMsg();
	g_board.board_view = transition::StateMsg();
	g_board.want = false;
	g_board.zone_in = false;
	g_board.zone_changer = kNoId;
	g_board.zone_route = kNoId;
	g_board.withdrawn_from = kNoId;
	g_board.guide_yes = false;
	g_board.consent = false;
	g_board.going = false; // a go whose level change never came (a load meanwhile) is over with its world
	g_board.routes.clear(); // the route restrictors were the old level's
	g_board.exit_known = false;
	g_board.my_dist = -1.f;
	g_board.hold.reset();
	g_board.intent.reset();
}

namespace
{
const char* mark_name(transition::Mark m)
{
	switch (m)
	{
	case transition::Mark::Ready: return "ready";
	case transition::Mark::Away: return "away";
	case transition::Mark::Loading: return "loading";
	case transition::Mark::Down: return "down";
	case transition::Mark::Elsewhere: return "elsewhere";
	}
	return "away";
}

// coop.transition() -> nil | table, the board as this player's placard shows it (scripts/coop_hud_transition.script):
//   state "waiting" | "going" (the group goes now, the screen darkens), kind "exit" | "guide", level (stock name,
//   "l01_escape"), changer (exit id, host numbering), ready, total, load_ms (left of the loaders' minute, 0 = none),
//   rows = { {peer, mark "ready"|"away"|"loading"|"down"|"elsewhere", dist (m, to the exit; left out if unknown),
//            consent (true: he comes along from afar); left out otherwise}, ... },
//   me = { peer, host, in_zone (I stand in an exit's zone or a route restrictor into it), ready (the board has me ready),
//          dist (m to the exit), confirm (a guide trip I can say yes to), consent (I held the key to come along) },
//   pos = {x, y, z} of the exit on this machine (left out for a guide trip).
// nil when nobody waits, outside a session, or when the host's board has been silent for 3 s.
int l_transition(lua_State* L)
{
	const u64 now = mgr().now_ms();
	const transition::StateMsg& v = g_board.view;
	const bool fresh = now - g_board.view_at <= 3000; // the host player's view comes over the link like anybody's
	if (!mgr().active() || !group_waits_here() || v.state == transition::State::Idle || !fresh)
	{
		lua_pushnil(L);
		return 1;
	}
	const u64 age = now > g_board.view_at ? now - g_board.view_at : 0;
	const u32 self = mgr().self();
	lua_newtable(L);
	lua_api::set_str(L, "state", v.state == transition::State::Going ? "going" : "waiting");
	lua_api::set_str(L, "kind", v.kind == 1 ? "guide" : "exit");
	lua_api::set_str(L, "level", v.level.c_str());
	if (v.kind == 0)
		lua_api::set_num(L, "changer", lua_Number(v.changer));
	lua_api::set_num(L, "ready", lua_Number(v.ready));
	lua_api::set_num(L, "total", lua_Number(v.total));
	lua_api::set_num(L, "load_ms", lua_Number(v.load_ms > age ? v.load_ms - age : 0));
	bool me_ready = false;
	lua_newtable(L);
	int index = 0;
	for (const transition::Row& r : v.rows)
	{
		lua_newtable(L);
		lua_api::set_num(L, "peer", lua_Number(r.id));
		lua_api::set_str(L, "mark", mark_name(r.mark));
		if (r.dist_m != transition::kUnknownDist)
			lua_api::set_num(L, "dist", lua_Number(r.dist_m));
		if (r.consent)
			lua_api::set_bool(L, "consent", true); // he comes along from afar
		lua_rawseti(L, -2, ++index);
		me_ready = me_ready || (r.id == self && r.mark == transition::Mark::Ready);
	}
	lua_setfield(L, -2, "rows");
	lua_newtable(L);
	lua_api::set_num(L, "peer", lua_Number(self));
	lua_api::set_bool(L, "host", mode().decides_world());
	lua_api::set_bool(L, "in_zone", g_board.zone_in);
	lua_api::set_bool(L, "ready", me_ready);
	if (g_board.my_dist >= 0.f && g_board.my_dist < 1.0e6f)
		lua_api::set_num(L, "dist", lua_Number(g_board.my_dist));
	lua_api::set_bool(L, "confirm", v.kind == 1 && !me_ready && !g_board.guide_yes);
	lua_api::set_bool(L, "consent", g_board.consent);
	lua_setfield(L, -2, "me");
	if (g_board.exit_known)
	{
		lua_newtable(L);
		lua_api::set_num(L, "x", lua_Number(g_board.exit_pos.x));
		lua_api::set_num(L, "y", lua_Number(g_board.exit_pos.y));
		lua_api::set_num(L, "z", lua_Number(g_board.exit_pos.z));
		lua_setfield(L, -2, "pos");
	}
	return 1;
}

// Coop.transition_route(restrictor_id, x, y, z) -> level changer id | nil (this machine's numbering). A route
// restrictor of sr_teleport_sections.ltx (scripts\coop_transition_route.script) drops its traveller at (x, y, z): the enabled
// level changer of this level that holds that point (transition::route_exit). While the group waits at exits here
// (group_waits_here: a client in the host's world, a host with company) the pair is remembered for this world and the
// player who stands inside that restrictor stands at that changer's exit (transition_step); the script then leaves the stock
// fade and teleport out. nil: no session with company, not a space restrictor, or a route inside the level (the stock
// teleport stays).
int l_transition_route(lua_State* L)
{
	const bool args = lua_type(L, 1) == LUA_TNUMBER && lua_type(L, 2) == LUA_TNUMBER && lua_type(L, 3) == LUA_TNUMBER &&
	    lua_type(L, 4) == LUA_TNUMBER;
	const lua_Number id = args ? lua_tonumber(L, 1) : -1.;
	if (!args || !mgr().active() || !group_waits_here() || !g_pGameLevel || !ai().get_alife() || !(id >= 0. && id < 65535.))
	{
		lua_pushnil(L);
		return 1;
	}
	const u16 restrictor = u16(id);
	CObject* object = Level().Objects.net_Find(restrictor);
	Fvector drop;
	drop.set(float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4)));
	if (!object || !object->cast_restrictor() || object->cast_level_changer() || !_valid(drop))
	{
		lua_pushnil(L);
		return 1;
	}
	std::vector<transition::ExitDistance> exits;
	for (CLevelChanger* lc : g_lchangers)
	{
		if (!lc || !lc->IsLevelChangerEnabled())
			continue;
		const CSE_ALifeLevelChanger* se = exit_here(lc->ID());
		exits.push_back(transition::ExitDistance{lc->ID(), se ? exit_distance(*se, drop) : -1.f});
	}
	const u16 changer = transition::route_exit(exits);
	if (changer == transition::kNoChanger)
	{
		g_board.routes.forget(restrictor);
		lua_pushnil(L);
		return 1;
	}
	if (g_board.routes.changer_of(restrictor) != changer && g_board.routes.add(restrictor, changer))
		::Msg("[coop] transition: route '%s' leads into level exit %u: standing in it is standing at that exit",
		    object->cName().c_str(), u32(to_host(changer)));
	lua_pushnumber(L, lua_Number(changer));
	return 1;
}
} // namespace

// Host: a player's TransitionVote, the host player's over the local link included (one rule for all,
// transition::apply_vote). Ready at an exit (checked: an exit of this level, the player is in the world and within
// kVoteReachM of it), Ready without an exit (a yes to the host's guide trip), Route (within kVoteReachM of a route
// restrictor of this level into that exit), Consent ("go with the group" to an exit somebody else stands at), Cancel.
void host_on_transition_vote(PeerId from, Reader& r)
{
	if (!mode().decides_world())
		return;
	transition::VoteMsg v;
	if (!v.read(r))
	{
		if (++g_board.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] a malformed TransitionVote from #%u: ignored", from);
		return;
	}
	const std::string nick = nick_of_peer(from);
	const u64 now = mgr().now_ms();
	transition::VoteFacts f;
	if (v.op != transition::VoteCancel)
	{
		if (!g_pGameLevel || !ai().get_alife())
			return;
		if (g_board.going || (g_board.released_at != 0 && now - g_board.released_at < kAfterReleaseMs))
			return; // the group is on its way, the old world is about to go: no new claim in it
		// Only a player who is in the world claims, and only at that exit (a level changer of the host's own level)
		const CObject* body = player_body_here(from);
		f.in_world = body != nullptr;
		if (v.changer != transition::kNoChanger)
		{
			const CSE_ALifeLevelChanger* lc = level_changer_of_goal(v.changer);
			f.exit_known = lc != nullptr;
			if (lc && body)
				f.dist = exit_distance(*lc, body->Position());
		}
		if (v.op == transition::VoteRoute)
		{
			const CSE_ALifeSpaceRestrictor* route = restrictor_of_level(v.route, levels::level_of_player(from));
			f.route_known = route != nullptr;
			if (route && body)
				f.route_dist = exit_distance(*route, body->Position());
		}
		f.guide_running = g_board.guide.set && g_board.board.guide_on();
	}
	switch (transition::apply_vote(g_board.board, from, v, f, now))
	{
	case transition::Verdict::Released:
		g_board.board_ticked = 0; // the board runs on the next frame
		::Msg("[coop] transition: '%s' is not ready any more", nick.c_str());
		break;
	case transition::Verdict::Nothing:
		break;
	case transition::Verdict::Claimed:
		g_board.board_ticked = 0;
		if (v.op == transition::VoteConsent)
			::Msg("[coop] transition: '%s' goes with the group through level exit %u (from %.0f m)", nick.c_str(), v.changer,
			    f.dist);
		else if (v.op == transition::VoteRoute)
			::Msg("[coop] transition: '%s' is ready at level exit %u, in its route %u (%.1f m)", nick.c_str(), v.changer,
			    v.route, f.route_dist);
		else if (v.changer != transition::kNoChanger)
			::Msg("[coop] transition: '%s' is ready at level exit %u (%.1f m)", nick.c_str(), v.changer, f.dist);
		break;
	case transition::Verdict::Refreshed:
		g_board.board_ticked = 0;
		break;
	case transition::Verdict::NotInWorld:
		if (++g_board.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' says he is ready at level exit %u but is not in the world: ignored", nick.c_str(),
			    v.changer);
		break;
	case transition::Verdict::NoExit:
		if (++g_board.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' says he is ready at object %u, which is no level exit here: ignored", nick.c_str(),
			    v.changer);
		break;
	case transition::Verdict::TooFar:
		if (++g_board.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' says he is ready at level exit %u from %.1f m away (at most %.0f m%s): ignored", nick.c_str(),
			    v.changer, v.op == transition::VoteRoute ? f.route_dist : f.dist, kVoteReachM,
			    v.op == transition::VoteRoute ? " of its route" : "");
		break;
	case transition::Verdict::NoGuide:
		if (++g_board.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' says yes to a guide trip but there is none: ignored", nick.c_str());
		break;
	case transition::Verdict::NotWaited:
		if (++g_board.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' goes with the group through level exit %u, where nobody waits: ignored", nick.c_str(),
			    v.changer);
		break;
	case transition::Verdict::NoRoute:
		if (++g_board.votes_refused <= kRefusalsLogged)
			::Msg("! [coop] '%s' says he stands in route %u, which is no restrictor here: ignored", nick.c_str(), v.route);
		break;
	}
}

// A player: the world server's board. The placard reads it (coop.transition()). A remote client gets it from his
// session, the host player from the local link.
void client_on_transition_state(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().active())
		return;
	transition::StateMsg m;
	if (!m.read(r))
	{
		if (++g_board.view_bad <= kRefusalsLogged)
			::Msg("! [coop] an unreadable TransitionState from the host: ignored");
		return;
	}
	g_board.view = m;
	g_board.view_at = mgr().now_ms();
	if (m.state == transition::State::Idle || m.kind != 1)
		g_board.guide_yes = false;
	// "go with the group" is for the exit the board waits at; another exit, a guide trip, or no wait ends it
	if (m.state == transition::State::Idle || m.kind != 0 || u32(m.changer) != g_board.consent_goal)
		g_board.consent = false;
}

bool accept_transition()
{
	// The waiting point's fallback (the chat's /go, the console): the group goes at once through the exit somebody waits
	// at, whoever is missing (the host has no key of his own for it).
	if (!mode().decides_world() || !g_pGameLevel || !ai().get_alife() || !g_board.board_lead_known)
	{
		::Msg("! [coop] no pending transition: nobody waits at a level exit");
		return false;
	}
	g_board.board.force(g_board.board_lead);
	g_board.board_ticked = 0; // the board runs on the next frame
	::Msg("[coop] the host moves the group now (exit %u)", g_board.board_lead);
	return true;
}

bool transition_consent()
{
	// One who is NOT at the exit the board waits at goes with the group from where he is (no distance); at the
	// exit there is nothing to hold (he withdraws by stepping out)
	// A refusal says why: a key pressed once and refused must not look like a key that does nothing
	const char* why = !group_waits_here()                          ? "nobody waits here (no session / not live)"
	    : g_board.zone_in                                              ? "you stand in an exit's zone yourself"
	    : g_board.view.state != transition::State::Waiting             ? "the board does not wait (idle or going)"
	    : g_board.view.kind != 0                                       ? "the board is a guide trip"
	    : g_board.view.changer == transition::kNoChanger              ? "the board names no exit"
	                                                             : nullptr;
	if (why)
	{
		::Msg("! [coop] transition: 'go with the group' refused: %s", why);
		return false;
	}
	g_board.consent = true;
	g_board.consent_goal = u32(g_board.view.changer);
	g_board.intent.reset(); // it goes out at the next frame (the host player's over the local link too)
	::Msg("[coop] transition: you go with the group through level exit %u", g_board.consent_goal);
	return true;
}

bool transition_cancel()
{
	if (!group_waits_here())
		return false;
	g_board.guide_yes = false;
	g_board.consent = false;
	if (g_board.zone_in)
		g_board.withdrawn_from = g_board.zone_changer; // no claim until he leaves this zone
	g_board.intent.reset(); // the cancel goes out at the next frame (the host player's over the local link too)
	return true;
}

bool transition_confirm()
{
	if (g_board.view.state == transition::State::Idle || g_board.view.kind != 1)
		return false;
	g_board.guide_yes = true;
	return true;
}

bool transition_owns_exit(bool exit_enabled, u16 /*next_vertex*/)
{
	// A client past Live (the host's Go came, his world is on its way): no dialog and no change of his own in the zone he still stands in
	if (exit_enabled && is_mirror() && !mgr().is_live_client())
		return true;
	return exit_enabled && group_waits_here();
}

bool exit_without_dialog(bool exit_enabled, u16 /*next_vertex*/)
{
	return exit_enabled && mgr().active() && mode().decides_world() && !group_waits_here();
}

bool host_gate_level_change(NET_Packet& P)
{
	// His world is the host's. Past Live the client's message filter lets this through, and the stock change would load a save of his own
	if (mgr().active() && !mode().decides_world() && !entities::client_applying())
		return true;
	if (!mode().decides_world() || g_board.releasing || !ai().get_alife())
		return false; // the stock change: not a host, or the board's own
	if (!host_has_company())
		return false; // the stock change: one player
	const u64 now = mgr().now_ms();
	const u16 changer = level_changer_of(P);
	if (changer != kNoId)
	{
		// The host's game went through an exit (a dialog opened before somebody joined, a script): the group waits at it.
		g_board.board.claim(0, transition::goal_of_exit(changer), now);
		g_board.board_ticked = 0;
		::Msg("[coop] transition: the host asks to go through level exit %u: the group waits for its players", changer);
		return true;
	}
	// No exit leads there: a guide, jump_to_level and the like. The trip is kept for when everybody has said yes.
	constexpr u32 kBody = sizeof(GameGraph::_GRAPH_ID) + sizeof(u32) + 2 * sizeof(Fvector);
	if (P.B.count < sizeof(u16) + kBody)
		return false; // not a level change we can read: the stock path
	const u32 read_at = P.r_pos;
	P.r_pos = sizeof(u16); // after the message type
	Fvector position, angles;
	GameGraph::_GRAPH_ID graph;
	u32 node;
	P.r(&graph, sizeof(graph));
	P.r(&node, sizeof(node));
	P.r_vec3(position);
	P.r_vec3(angles);
	P.r_pos = read_at;
	// a trip that waits keeps its place: the same way is a yes to it
	const transition::TripAsk ask = transition::ask_trip(g_board.guide.set && g_board.board.guide_on(), levels::level_of_vertex(g_board.guide.graph), levels::level_of_vertex(graph));
	if (ask != transition::TripAsk::New)
	{
		g_board.guide_yes = ask == transition::TripAsk::Joins;
		if (ask == transition::TripAsk::Busy)
			hud_text::say("st_coop_levels_refused_invalid", "This exit cannot be used right now.");
		return true;
	}
	g_board.guide.graph = graph;
	g_board.guide.node = node;
	g_board.guide.position = position;
	g_board.guide.angles = angles;
	g_board.guide.level = level_name_of(g_board.guide.graph);
	g_board.guide.from_position = g_actor ? Actor()->Position() : Fvector().set(0.f, 0.f, 0.f);
	g_board.guide.by = kHostPeer;
	g_board.guide.set = true;
	g_board.board.start_guide(0, now, transition::Trip::Lapses);
	g_board.board_ticked = 0;
	g_board.guide_yes = true; // the host player asked for it: his votes over the local link keep his yes (transition_step)
	::Msg("[coop] transition: the host's game asks to go to '%s' (a guide trip): the group waits for everybody's yes",
	    g_board.guide.level.c_str());
	return true;
}

void register_lua(lua_State* L)
{
	static const luaL_Reg kFns[] = {
	    {"transition", &l_transition}, // coop.transition()
	    {"transition_route", &l_transition_route}, // coop.transition_route(restrictor, x, y, z)
	    {nullptr, nullptr},
	};
	lua_api::fill(L, nullptr, kFns); // no coop table: coop_script logs it
}

// A level change with no exit behind it. It is asked of the world server with its point of arrival.
void client_level_trip(NET_Packet& P)
{
	constexpr u32 kBody = sizeof(GameGraph::_GRAPH_ID) + sizeof(u32) + 2 * sizeof(Fvector);
	const u64 now = mgr().now_ms();
	constexpr u64 kTripAgainMs = 5000;
	if (P.B.count < sizeof(u16) + kBody || g_load.move_held ||
	    (g_load.move_changer == kNoId && g_load.move_sent_at != 0 && now - g_load.move_sent_at < kTripAgainMs))
		return;
	// A stock ChangeLevel starts with a 3 s fade. The player must be standing to request a trip, as for an exit
	// (transition_step). The world server checks again with srv::departure::may_leave. If the client skips the request,
	// client_stays_on_level clears the stock fade and keeps the player on this level, as on a host refusal.
	const CActor* me = g_actor ? Actor() : nullptr;
	if (!me || !me->g_Alive() || survival::life_of(0) != survival::Life::Alive)
	{
		::Msg("! [coop] a trip: not asked, I am %s (a level is left standing)",
		    !me ? "without an actor" : !me->g_Alive() ? "dead" : survival::life_name(survival::life_of(0)));
		client_stays_on_level();
		return;
	}
	const u32 read_at = P.r_pos;
	P.r_pos = sizeof(u16); // after the message type
	GameGraph::_GRAPH_ID graph;
	u32 node;
	Fvector position, angles;
	P.r(&graph, sizeof(graph));
	P.r(&node, sizeof(node));
	P.r_vec3(position);
	P.r_vec3(angles);
	P.r_pos = read_at;
	levels::wire::LevelMoveMsg m;
	m.seq = ++g_load.move_seq;
	m.kind = levels::wire::LevelMoveMsg::Trip;
	m.graph = u16(graph);
	m.node = node;
	m.pos[0] = position.x;
	m.pos[1] = position.y;
	m.pos[2] = position.z;
	m.dir[0] = angles.x;
	m.dir[1] = angles.y;
	m.dir[2] = angles.z;
	g_load.move_changer = kNoId;
	g_board.guide_yes = true; // a level entered together turns it into a trip of the group: his own yes stands
	if (client_send_move(m, now)) // seated, it waits for his detach (client_seat_holds)
		::Msg("[coop] a trip to '%s': asked the host to let me go", level_name_of(graph).c_str());
}

void client_request_level_change(NET_Packet& P)
{
	if (!mgr().is_live_client())
		return;
	// The host builds the level change itself; it only learns which of its level changers we stand in. The stock
	// zone does not get this far in a session (transition_owns_exit): this is a level change our own game asked for.
	const u16 changer = level_changer_of(P);
	if (changer == kNoId && !mode().group_travels())
	{
		client_level_trip(P); // a guide, a fast travel, a story ChangeLevel: only he goes
		return;
	}
	if (changer == kNoId)
	{
		// Where the group travels together only the host moves it, and no level changer of this level
		// leads where this player's game asked to go (a fast travel, a guide, a story ChangeLevel). The co-op's scripts refuse such a
		// trip before it costs; one that still gets here is refused in the open: one line in the log a session, the
		// stock route's fade taken away (client_stays_on_level, as after a refusal of the host) and the line on the HUD each time.
		if (g_board.trips_refused++ == 0)
			::Msg("[coop] level exit: no level changer of this level leads there, the host is not asked (only the host moves the "
			      "group; the next ones are counted in coop_status)");
		client_stays_on_level();
		hud_text::say("st_coop_core_go_host_only", "Only the host can move the group.");
		return;
	}
	if (!mode().group_travels())
	{
		client_level_move(to_host(changer), mgr().now_ms()); // he goes alone (a server's player)
		return;
	}
	transition::VoteMsg v;
	v.op = transition::VoteReady;
	v.changer = to_host(changer);
	Writer w(4);
	v.write(w);
	net::client_send(Msg::TransitionVote, w);
	::Msg("[coop] level exit %u: asked the host to move the group", changer);
}
} // namespace join
} // namespace coop
