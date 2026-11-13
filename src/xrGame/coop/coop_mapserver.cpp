#include "stdafx.h"
#include "coop_mapserver.h"
#include "coop_autosave.h"
#include "coop_join.h"
#include "coop_levels.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_sync.h"
#include "xrCoop/coop_soft_move.h"
#include "xrCoop/coop_operator.h" // op::redact

#include "../../xrEngine/XR_IOConsole.h"
#include "Actor.h"
#include "Actor_Flags.h"
#include "ai_space.h"
#include "CharacterPhysicsSupport.h"
#include "game_graph.h"
#include "Level.h"
#include "PHMovementControl.h"

#include <cstdio>
#include <map>
#include <psapi.h>
#include <string>

#pragma comment(lib, "psapi.lib")

// F3a split levels: the map-server processes (T9): see coop_mapserver.h.

namespace coop
{
namespace mapserver
{
namespace
{
namespace wire = levels::wire;

constexpr u32 kStateEveryMs = 5000; // a map server's report of itself
constexpr u32 kDrainGraceMs = 1000; // it says Stopped this long before the world server gives up on it

// ---------------------------------------------------------------- the host's process

struct Server
{
	levels::ServerLife life;
	BridgeLink link; // the session's connection is this process's
	u32 k = 0; // its number: fsgame_ms<k>.ltx, its slice of ids, bridge_peer(k)
	HANDLE process = nullptr;
	u32 epoch = 0; // the world it was started for
	wire::ServerState said = wire::ServerState::Loading;
	wire::MapServerStateMsg last; // its last report
	bool gave_up_told = false;
	bool kept = false; // it stays through the host's move
	bool lost_meanwhile = false; // it was lost during that move (on_lost)
};

struct Host
{
	std::map<u16, Server> servers; // by level
	std::map<u16, softmove::ServerVerdict> verdicts; // why kept or not at the last host_stop_for_move, by level: a log line, nothing else.
	                                                 // Apart from `servers`: a server that stopped may be gone from there by the go
	std::vector<PeerId> bridges; // Server::k of those that run (host_bridges)
	bool hold_launch = false; // the host's move: nothing starts until its save is written
	HANDLE job = nullptr; // the processes end with this one, however it ends
	u32 started = 0, crashed = 0;
} g;

// ---------------------------------------------------------------- a map server

struct Me
{
	bool parsed = false;
	bool is = false;
	Args args;
	VirtualPeers peers;
	bool ready_said = false;
	bool draining = false;
	u64 drain_since = 0;
	u64 state_at = 0;
	u64 frame_at = 0;
	u64 frame_sum = 0;
	u32 frames = 0;
	u32 frame_max = 0;
	bool quitting = false;
	// who came and who is in since the last frame: the session reports a virtual peer that left after the frame's handlers, and the same
	// player may stand in one table as gone and as new
	std::vector<Visitor> came;
	std::vector<PeerId> in_world;
} me;

std::string read_text(const std::string& path)
{
	std::string out;
	if (FILE* f = std::fopen(path.c_str(), "rb"))
	{
		char chunk[4096];
		size_t n;
		while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
			out.append(chunk, n);
		std::fclose(f);
	}
	return out;
}

bool write_text(const std::string& path, const std::string& text)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	std::fclose(f);
	return ok;
}

bool file_exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Where the engine found this process's fsgame (CLocatorAPI searchForFsltx: the working directory, its parent, the exe's folder, its
// parent).
bool find_fsltx(const std::string& name, std::string& found)
{
	const std::string app = Core.ApplicationPath;
	const std::string where[] = {name, "../" + name, app + name, app + "../" + name};
	for (const std::string& path : where)
		if (file_exists(path))
		{
			found = path;
			return true;
		}
	return false;
}

// A vertex of the level for its ghost to stand on.
u16 vertex_of_level(u16 level)
{
	const CGameGraph& gg = ai().game_graph();
	for (u32 v = 0, n = gg.header().vertex_count(); v < n; ++v)
		if (u16(gg.vertex(GameGraph::_GRAPH_ID(v))->level_id()) == level)
			return u16(v);
	return 0xffff;
}

u32 free_number()
{
	for (u32 k = 1; k <= kMaxBridges; ++k)
	{
		bool used = false;
		for (const auto& it : g.servers)
			used = used || it.second.k == k;
		if (!used)
			return k;
	}
	return 0;
}

void list_bridges()
{
	g.bridges.clear();
	for (const auto& it : g.servers)
		if (it.second.k && (it.second.process || mgr().session().bridge_connected(bridge_peer(it.second.k))))
			g.bridges.push_back(bridge_peer(it.second.k));
}

// The fsgame and the appdata of map server k: the host's gamedata and settings, a folder of its own for its log and its saves.
bool prepare_files(u32 k, std::string& fsltx)
{
	std::string found;
	if (!find_fsltx(fsltx_of(Core.Params), found))
	{
		::Msg("! [coop] map server: cannot find this game's fsgame ('%s')", fsltx_of(Core.Params).c_str());
		return false;
	}
	std::string text, dir;
	if (!rewrite_fsgame(read_text(found), k, text, dir))
	{
		::Msg("! [coop] map server: '%s' has no $app_data_root$ line", found.c_str());
		return false;
	}
	fsltx = fsltx_name(found, k);
	if (!write_text(fsltx, text))
	{
		::Msg("! [coop] map server: cannot write '%s'", fsltx.c_str());
		return false;
	}
	string_path root;
	FS.update_path(root, "$app_data_root$", "");
	const std::string mine = sibling_dir(root, appdata_name(k));
	CreateDirectoryA(mine.c_str(), nullptr);
	const std::string user = read_text(std::string(root) + "user.ltx");
	if (!write_text(mine + "user.ltx", map_server_user_ltx(user)))
	{
		::Msg("! [coop] map server: cannot write its user.ltx in '%s'", mine.c_str());
		return false;
	}
	const std::string options = read_text(std::string(root) + "axr_options.ltx");
	if (!options.empty())
		write_text(mine + "axr_options.ltx", options); // the machine's mod settings; the game's own come with the world
	return true;
}

// Ends it, and with it whatever it had open: a process that hangs is of no use to anybody.
void end_process(Server& s)
{
	if (!s.process)
		return;
	TerminateProcess(s.process, 1);
	CloseHandle(s.process);
	s.process = nullptr;
}

bool launch(u16 level, Server& s, u32 epoch)
{
	if (!s.k)
		s.k = free_number();
	Args a;
	a.level = level;
	a.graph = vertex_of_level(level);
	a.port = mgr().session().listen_port();
	if (!s.k || a.graph == 0xffff)
		return false;
	levels::host_reserve_leader_ids(false); // a number over coop_max_map_servers: its slice leaves the world before the world goes to it
	a.token = generate_player_key(); // 128 random bits, BCrypt
	std::string fsltx;
	if (!prepare_files(s.k, fsltx))
		return false;
	mgr().session().issue_bridge(s.k, a.token);

	char exe[MAX_PATH];
	if (!GetModuleFileNameA(nullptr, exe, MAX_PATH))
		return false;
	std::string line = std::string("\"") + exe + "\" " + command_tail(a, s.k, fsltx);
	STARTUPINFOA si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));
	// MO2's virtual files reach a child by themselves (USVFS hooks CreateProcess): plan §1.8, a live check
	if (!CreateProcessA(exe, &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
	{
		::Msg("! [coop] map server: cannot start it for level %u: error %lu", u32(level), GetLastError());
		mgr().session().revoke_bridge(s.k);
		return false;
	}
	CloseHandle(pi.hThread);
	s.process = pi.hProcess;
	s.epoch = epoch;
	s.said = wire::ServerState::Loading;
	sync::host_begin_join(bridge_peer(s.k)); // what the world does from now on reaches it after its load
	if (!g.job)
	{
		g.job = CreateJobObjectA(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
		ZeroMemory(&info, sizeof(info));
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (g.job)
			SetInformationJobObject(g.job, JobObjectExtendedLimitInformation, &info, sizeof(info));
	}
	if (g.job)
		AssignProcessToJobObject(g.job, s.process);
	++g.started;
	::Msg("[coop] map server %u starts for level %u (%s, process %lu)", s.k, u32(level), fsltx.c_str(), pi.dwProcessId);
	return true;
}

// What is left of its connection goes: what it held comes home, and nothing that comes of that connection later counts.
void cut_bridge(Server& s)
{
	const PeerId bridge = bridge_peer(s.k);
	s.link.cut();
	if (mgr().session().bridge_connected(bridge))
		mgr().session().kick(bridge, RejectReason::Kicked, "this map server is over");
	levels::xfer_host_disconnect(bridge);
	net::host_drop(bridge);
	sync::host_end_join(bridge);
}

// Its process is gone, and the world server did not ask for it: everything of its comes home at its last state, its players load
// anew (coop_levels_xfer.cpp, join::host_rejoin_level) and a new process starts for them unless it has failed too often.
void on_lost(u16 level, Server& s)
{
	if (s.life.state() == levels::ServerState::Off || s.life.state() == levels::ServerState::Lost)
		return;
	if (s.kept)
	{
		s.lost_meanwhile = true; // its players are parked and the world it would bring things home to is in the making: host_soft_over
		return;
	}
	const bool asked = s.life.state() == levels::ServerState::Stopping;
	cut_bridge(s);
	if (asked)
	{
		s.life.stopped();
		return;
	}
	end_process(s); // a link that broke with the process alive: it has no place in the world any more
	s.life.lost();
	++g.crashed;
	::Msg("! [coop] map server of level %u is gone: its objects are at their last confirmed state, %u failure(s) in a row", u32(level),
	    s.life.failures());
	if (!s.life.gave_up())
		join::host_rejoin_level(level, "st_coop_levels_server_restart");
}

void watch_process(u16 level, Server& s)
{
	if (!s.process || WaitForSingleObject(s.process, 0) != WAIT_OBJECT_0)
		return;
	// What it ended with tells a fault from an order: 1 is the engine's own end after a fatal error, 3 its watchdog after one, and
	// 0xC0000005 and the like a fault nobody caught. Live 2026-11-13: the first map server of a world vanished in its load, its log
	// cut in the middle of a line, no dump and nothing in Windows' own log - and the world server did not say how it had ended.
	DWORD code = 0;
	if (GetExitCodeProcess(s.process, &code))
		::Msg("[coop] map server %u: its process ended with code %lu (0x%08lX)", s.k, code, code);
	else
		::Msg("[coop] map server %u: its process ended, the code is not known (error %lu)", s.k, GetLastError());
	CloseHandle(s.process);
	s.process = nullptr;
	on_lost(level, s);
}

void send_order(const Server& s, u16 level, wire::MapServerOrderMsg::Order order)
{
	wire::MapServerOrderMsg m;
	m.order = order;
	m.level = level;
	Writer w(4);
	m.write(w);
	mgr().session().send(bridge_peer(s.k), Lane::Reliable, Msg::MapServerOrder, w);
}

Server* server_of_bridge(PeerId bridge, u16* level = nullptr)
{
	for (auto& it : g.servers)
		if (it.second.k && bridge_peer(it.second.k) == bridge)
		{
			if (level)
				*level = it.first;
			return &it.second;
		}
	return nullptr;
}

void host_on_bridge_joined(PeerId bridge)
{
	if (Server* s = server_of_bridge(bridge))
		s->link.joined();
	sync::host_begin_join(bridge);
	list_bridges();
	levels::host_map_changed(); // it learns its level's players and the rules from the table
}

void host_on_bridge_left(PeerId bridge, const std::string& reason)
{
	u16 level = wire::kNoLevel;
	Server* s = server_of_bridge(bridge, &level);
	::Msg("[coop] map server %u left: %s", bridge_index(bridge), reason.c_str());
	if (s && s->link.left())
		on_lost(level, *s);
	list_bridges();
}

void host_on_state(PeerId from, Reader& r)
{
	wire::MapServerStateMsg m;
	u16 level = wire::kNoLevel;
	Server* s = server_of_bridge(from, &level);
	if (!s || !m.read(r) || m.level != level)
		return;
	s->last = m;
	const wire::ServerState state = wire::ServerState(m.state);
	if (state == s->said)
		return;
	s->said = state;
	switch (state)
	{
	case wire::ServerState::Ready:
		s->life.ready();
		levels::xfer_host_connect(from);
		levels::host_map_changed();
		::Msg("[coop] map server %u leads level %u", s->k, u32(level));
		break;
	case wire::ServerState::Stopped: ::Msg("[coop] map server %u has handed everything over", s->k); break;
	default: break;
	}
}

// ---------------------------------------------------------------- a map server

void say_state(wire::ServerState state)
{
	wire::MapServerStateMsg m;
	m.state = u8(state);
	m.level = me.args.level;
	m.frame_avg_ms = me.frames ? u32(me.frame_sum / me.frames) : 0;
	m.frame_max_ms = me.frame_max;
	m.online = levels::xfer_held();
	PROCESS_MEMORY_COUNTERS_EX mem;
	ZeroMemory(&mem, sizeof(mem));
	mem.cb = sizeof(mem);
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem)))
		m.mem_mb = u32(mem.PrivateUsage / (1024 * 1024));
	Writer w(24);
	m.write(w);
	net::client_send(Msg::MapServerState, w);
	me.frame_sum = 0;
	me.frames = 0;
	me.frame_max = 0;
}

// Plan §1.6: nobody plays the actor of a map server. It has to exist (a level has one, the stock code asks for it), and that is all: no
// AI sees or hears it (npc_ai::ghost), nothing hurts it, nothing bumps into it, no player sees it (it sends no stream at all,
// net::client_send), it watches nothing and keeps nothing online (observer_distance, viewers), and it is nobody's seat.
void server_on_order(PeerId from, Reader& r)
{
	wire::MapServerOrderMsg m;
	if (from != kHostPeer || !is_map_server() || !m.read(r) || m.level != me.args.level)
		return;
	if (m.order == wire::MapServerOrderMsg::Serve || me.draining)
		return;
	me.draining = true;
	me.drain_since = mgr().now_ms();
	say_state(wire::ServerState::Draining);
	::Msg("[coop] map server: the world server wants everything back");
}
} // namespace

void ghost_frame()
{
	if (!g_actor)
		return;
	psActorFlags.set(AF_GODMODE, TRUE);
	CActor* a = smart_cast<CActor*>(Level().CurrentEntity());
	if (a && a->getVisible())
	{
		a->setVisible(FALSE);
		if (CCharacterPhysicsSupport* physics = a->character_physics_support())
			if (physics->movement())
				physics->movement()->SetNonInteractive(true);
	}
}

bool parsed_key()
{
	if (!me.parsed)
	{
		me.parsed = true;
		me.is = parse_args(Core.Params, me.args);
		if (Core.Params && strstr(Core.Params, kKey) && !me.is)
			::Msg("! [coop] map server: the key -coop_mapserver cannot be read: '%s'", op::redact(Core.Params).c_str());
	}
	return me.is;
}

const Args& key() { return me.args; }

void install_server(Session& s)
{
	s.on(Msg::Tunnel, &trunk_host_on_tunnel);
	s.on(Msg::MapServerState, &host_on_state);
	s.on_bridge_joined = &host_on_bridge_joined;
	s.on_bridge_left = &host_on_bridge_left;
}

void install_client(Endpoint& e)
{
	e.on(Msg::Tunnel, &trunk_server_on_tunnel);
	e.on(Msg::MapServerOrder, &server_on_order);
}

void host_frame()
{
	if (!mgr().is_host())
		return;
	const u64 now = mgr().now_ms();
	for (const u16 level : levels::host_map_levels())
		g.servers[level];
	for (auto it = g.servers.begin(); it != g.servers.end();)
	{
		const u16 level = it->first;
		Server& s = it->second;
		s.life.linger_ms = u32(levels::g_coop_mapserver_linger) * 1000;
		s.life.players(levels::host_map_players(level), now);
		watch_process(level, s);
		if (s.life.gave_up())
		{
			if (!s.gave_up_told)
			{
				s.gave_up_told = true;
				::Msg("! [coop] level %u has no map server, %u starts failed: its players go to the host's level", u32(level), s.life.failures());
				join::host_rejoin_level(level, "st_coop_levels_server_failed");
			}
		}
		else if (s.life.want_start())
		{
			s.gave_up_told = false;
			if (!g.hold_launch && !s.process && world_up())
			{
				autosave::request_before_transfer(); // requirement 8: written before it can take anything, it needs far longer to start
				s.life.started(now);
				if (!launch(level, s, join::host_world_epoch()))
					s.life.lost();
				list_bridges();
			}
		}
		else if (s.life.want_stop(now))
		{
			switch (s.life.state())
			{
			case levels::ServerState::Starting:
			case levels::ServerState::Stopping:
				::Msg("! [coop] map server %u of level %u does not answer: it is ended", s.k, u32(level));
				end_process(s);
				cut_bridge(s);
				s.life.lost();
				break;
			default:
				send_order(s, level, wire::MapServerOrderMsg::Stop);
				s.life.stopping(now);
				::Msg("[coop] map server %u of level %u: nobody came back, it goes", s.k, u32(level));
				break;
			}
		}
		const bool over = s.life.state() == levels::ServerState::Off || s.life.state() == levels::ServerState::Lost;
		if (over && !s.process && s.life.count() == 0)
		{
			mgr().session().revoke_bridge(s.k);
			it = g.servers.erase(it);
			list_bridges();
			continue;
		}
		++it;
	}
}

bool host_serves(u16 level)
{
	const auto it = g.servers.find(level);
	return it != g.servers.end() && !it->second.life.gave_up() &&
	    (it->second.life.running() || (it->second.life.state() == levels::ServerState::Lost && it->second.life.count() > 0));
}

bool host_ready(u16 level)
{
	const auto it = g.servers.find(level);
	if (it == g.servers.end())
		return false;
	const levels::ServerState s = it->second.life.state();
	return s == levels::ServerState::Ready || s == levels::ServerState::Serving || s == levels::ServerState::Draining;
}

bool host_can_serve(u16 level)
{
	const auto it = g.servers.find(level);
	if (it != g.servers.end() && it->second.life.gave_up())
		return false;
	return host_serves(level) || host_running() < u32(levels::g_coop_max_map_servers);
}

u32 host_running()
{
	u32 n = 0;
	for (const auto& it : g.servers)
		n += it.second.life.running() ? 1u : 0u;
	return n;
}

wire::ServerState host_state_of(u16 level)
{
	if (host_ready(level))
		return wire::ServerState::Ready;
	const auto it = g.servers.find(level);
	if (it != g.servers.end() && it->second.life.state() == levels::ServerState::Stopping)
		return wire::ServerState::Draining;
	return wire::ServerState::Loading;
}

const std::vector<PeerId>& host_bridges() { return g.bridges; }

PeerId bridge_of(u16 level)
{
	const auto it = g.servers.find(level);
	if (it == g.servers.end() || !it->second.k)
		return kInvalidPeer;
	const PeerId bridge = bridge_peer(it->second.k);
	return mgr().session().bridge_connected(bridge) ? bridge : kInvalidPeer;
}

u16 level_of_bridge(PeerId bridge)
{
	u16 level = wire::kNoLevel;
	server_of_bridge(bridge, &level);
	return level;
}

u32 host_highest_number()
{
	u32 highest = 0;
	for (const auto& it : g.servers)
		highest = _max(highest, it.second.k);
	return highest;
}

void host_stop_all() { host_stop_for_move(wire::kNoLevel, false); }

void host_stop_for_move(u16 target, bool soft)
{
	const u64 now = mgr().now_ms();
	g.hold_launch = true;
	g.verdicts.clear();
	for (auto& it : g.servers)
	{
		Server& s = it.second;
		const levels::ServerState state = s.life.state();
		const bool leads = (state == levels::ServerState::Ready || state == levels::ServerState::Serving) && bridge_of(it.first) != kInvalidPeer;
		s.kept = softmove::keeps_server(soft, leads, it.first, target);
		g.verdicts[it.first] = softmove::server_verdict(soft, leads, it.first, target);
		if (s.kept)
			continue;
		if (s.life.state() == levels::ServerState::Starting)
		{
			end_process(s);
			cut_bridge(s);
			s.life.lost();
		}
		else if (s.life.running())
		{
			send_order(s, it.first, wire::MapServerOrderMsg::Stop);
			s.life.stopping(now);
		}
	}
}

bool host_all_stopped()
{
	for (const auto& it : g.servers)
		if (!it.second.kept && (it.second.life.running() || it.second.life.state() == levels::ServerState::Stopping))
			return false;
	return true;
}

std::vector<Session::SoftServer> host_kept()
{
	std::vector<Session::SoftServer> out;
	for (const auto& it : g.servers)
		if (it.second.kept)
		{
			Session::SoftServer row;
			row.bridge = bridge_peer(it.second.k);
			row.level = it.first;
			out.push_back(row);
		}
	return out;
}

softmove::ServerVerdict host_verdict(u16 level)
{
	const auto it = g.verdicts.find(level);
	return it == g.verdicts.end() ? softmove::ServerVerdict::None : it->second;
}

bool host_keeps(u16 level)
{
	const auto it = g.servers.find(level);
	return it != g.servers.end() && it->second.kept;
}

void host_soft_over(bool world_is_new)
{
	for (auto& it : g.servers)
	{
		Server& s = it.second;
		const bool stayed = s.kept;
		s.kept = false;
		if (s.lost_meanwhile)
		{
			s.lost_meanwhile = false;
			on_lost(it.first, s);
		}
		// one that did not stay and did not finish handing over in its 15 s: the book stayed, its part of it comes home by the book
		else if (world_is_new && !stayed && s.k)
			levels::xfer_host_disconnect(bridge_peer(s.k));
	}
	list_bridges();
}

void host_launch_now(u32 epoch)
{
	g.hold_launch = false;
	const u64 now = mgr().now_ms();
	for (const u16 level : levels::host_map_levels())
	{
		Server& s = g.servers[level];
		s.life.players(levels::host_map_players(level), now);
		if (s.life.want_start() && !s.process) // one on its way out: host_frame starts this level when it is gone
		{
			s.life.started(now);
			if (!launch(level, s, epoch))
				s.life.lost();
		}
	}
	list_bridges();
}

void host_world_replaced(u32 epoch)
{
	g.hold_launch = false;
	const u64 now = mgr().now_ms();
	for (auto& it : g.servers)
	{
		Server& s = it.second;
		if (s.kept)
		{
			s.epoch = epoch; // this is its world too: it never left it
			continue;
		}
		if (s.epoch == epoch || !s.process)
			continue;
		// its world is gone: it has nothing to hand over. Nothing but a SessionReset reaches a process of another epoch.
		if (s.life.state() == levels::ServerState::Starting)
		{
			end_process(s);
			cut_bridge(s);
			s.life.lost();
		}
		else
		{
			Writer w(32);
			w.str("the host replaced the world");
			w.u32(epoch);
			mgr().session().send(bridge_peer(s.k), Lane::Reliable, Msg::SessionReset, w);
			s.life.stopping(now);
		}
	}
}

void server_frame()
{
	if (!is_map_server())
		return;
	const u64 now = mgr().now_ms();
	if (me.frame_at)
	{
		const u32 ms = u32(now - me.frame_at);
		me.frame_sum += ms;
		me.frame_max = _max(me.frame_max, ms);
		++me.frames;
	}
	me.frame_at = now;
	for (const Visitor& v : me.came)
		join::server_player_came(v.peer, v.proxy);
	me.came.clear();
	for (const PeerId peer : me.in_world)
		join::server_player_in_world(peer);
	me.in_world.clear();
	if (world_up())
		ghost_frame();
	if (!me.ready_said && mgr().is_live_client() && world_up() && !Device.dwPrecacheFrame)
	{
		me.ready_said = true;
		me.state_at = now;
		say_state(wire::ServerState::Ready);
		::Msg("[coop] map server: level %u is up", u32(me.args.level));
		for (const auto& it : me.peers.on())
			if (it.second.in_world)
				join::server_player_in_world(it.first); // he was there before its world
	}
	else if (me.ready_said && now >= me.state_at + kStateEveryMs)
	{
		me.state_at = now;
		say_state(me.draining ? wire::ServerState::Draining : wire::ServerState::Ready);
	}
	if (!me.draining)
		return;
	levels::xfer_hand_back(now);
	if (levels::xfer_held() == 0 || now >= me.drain_since + levels::kStopTimeoutMs - kDrainGraceMs)
	{
		levels::logic_hand_over(); // the last look, and on the lane of Stopped: the host's move saves after it hears that this process left
		say_state(wire::ServerState::Stopped);
		me.draining = false;
		mgr().world_session().hold_connection(0);
		server_session_over();
	}
}

bool server_leads() { return me.ready_said; }

void server_session_over()
{
	if (!me.is || me.quitting)
		return;
	me.quitting = true;
	::Msg("[coop] map server: over");
	Console->Execute("quit");
}

void server_visitor_came(const Visitor& v) { me.came.push_back(v); }
void server_visitor_in_world(PeerId peer) { me.in_world.push_back(peer); }

void reset()
{
	me.peers.clear();
	me.came.clear();
	me.in_world.clear();
	me.ready_said = false;
	me.draining = false;
	for (auto& it : g.servers)
		end_process(it.second); // the session is over: its host's process does not leave them behind
	g.servers.clear();
	g.bridges.clear();
	g.hold_launch = false;
	levels::xfer_quiet(false);
}

void print_status()
{
	if (mgr().is_host())
	{
		for (const auto& it : g.servers)
		{
			const Server& s = it.second;
			::Msg("[coop] map server %u, level %u: %u player(s), %u failure(s), frame %u/%u ms, %u object(s), %u MB", s.k, u32(it.first),
			    s.life.count(), s.life.failures(), s.last.frame_avg_ms, s.last.frame_max_ms, s.last.online, s.last.mem_mb);
		}
		::Msg("[coop] map servers: %u running of %d, %u started, %u crashed", host_running(), levels::g_coop_max_map_servers, g.started,
		    g.crashed);
	}
	else if (is_map_server())
		::Msg("[coop] this process is the map server of level %u, %u player(s) on it", u32(me.args.level), u32(me.peers.on().size()));
}

VirtualPeers& trunk_peers() { return me.peers; }
} // namespace mapserver
} // namespace coop
