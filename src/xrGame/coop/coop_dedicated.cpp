#include "stdafx.h"
#include "coop_dedicated.h"
#include "coop_autosave.h"
#include "coop_levels.h"
#include "coop_manager.h"
#include "coop_mapserver.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/coop_host_console.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "date_time.h"
#include "../../xrEngine/Environment.h"
#include "game_graph.h"
#include "game_sv_single.h"
#include "Level.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

// See coop_dedicated.h.

namespace coop
{
namespace dedicated
{
namespace
{
constexpr u32 kTitleEveryMs = 2000;

struct State
{
	bool parsed = false;
	Args args;
	bool world_asked = false;
	bool new_world = false;
	bool quitting = false;
	bool quit_sent = false;
	u64 title_at = 0;
	bool usage_said = false;
} g;

// An answer to the operator: in the log, and in the window whatever its log level.
void say(const char* line)
{
	::Msg("%s", line);
	if (coop_console::log_level() < 1)
		coop_console::print(line);
}

void list_saves(std::vector<SaveFile>& out)
{
	string_path mask;
	FS.update_path(mask, "$game_saves$", "*" SAVE_EXTENSION);
	WIN32_FIND_DATAA found;
	const HANDLE h = FindFirstFileA(mask, &found);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		std::string name = found.cFileName;
		name.resize(name.size() - xr_strlen(SAVE_EXTENSION));
		SaveFile f;
		f.name = name;
		f.time = (u64(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
		out.push_back(f);
	} while (FindNextFileA(h, &found));
	FindClose(h);
}

bool save_exists(const std::string& name)
{
	string_path file;
	FS.update_path(file, "$game_saves$", (name + SAVE_EXTENSION).c_str());
	return GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES;
}

void start_world(World world, std::string save)
{
	if (world == World::Load)
	{
		if (save == kLastSave)
		{
			std::vector<SaveFile> files;
			list_saves(files);
			save = newest_save(files);
			if (save.empty())
			{
				::Msg("! [coop] dedicated: no saves in this folder, nothing to load: coop_world new");
				return;
			}
		}
		if (!save_name_ok(save) || !save_exists(save))
		{
			::Msg("! [coop] dedicated: no save '%s': coop_world list", save.c_str());
			return;
		}
	}
	g.world_asked = true;
	g.new_world = world == World::New;
	::Msg("[coop] dedicated: %s", world == World::New ? "a new world" : (std::string("the world of the save ") + save).c_str());
	Console->Execute(start_line(world, save).c_str());
}

// The first frame: what the keys ask for. A wrong key is said in the window, which stays open for the operator.
void first_frame()
{
	std::string why;
	if (!parse_args(Core.Params, g.args, why))
		::Msg("! [coop] dedicated: %s", why.c_str());
	::Msg("[coop] dedicated host, port %u, the build '%s'", u32(g.args.port), Manager::build_id());
	if (g.args.world != World::None)
		start_world(g.args.world, g.args.save);
	else
		::Msg("[coop] no world yet. coop_world new | coop_world last | coop_world <save> | coop_world list");
}

void run_line(const char* text)
{
	const Parsed line = parse_line(text);
	switch (line.kind)
	{
	case Line::Empty: break;
	case Line::Quit: g.quitting = true; break;
	case Line::Chat: mgr().send_chat(line.text.c_str()); break;
	case Line::Unknown: ::Msg("! [coop] '%s' is no command: help lists them", line.text.c_str()); break;
	case Line::Command: Console->Execute(line.text.c_str()); break;
	}
}

void update_title()
{
	const u64 now = mgr().now_ms();
	if (now < g.title_at)
		return;
	g.title_at = now + kTitleEveryMs;
	string256 title;
	if (mgr().active())
		xr_sprintf(title, "RayLink: dedicated host, port %u, %u player(s), %u map server(s)", u32(mgr().session().listen_port()),
		    u32(mgr().session().peers().size()), mapserver::host_running());
	else
		xr_sprintf(title, "RayLink: dedicated host, port %u, no session", u32(g.args.port));
	coop_console::set_title(title);
}

// The session goes first: its end writes the world's last save (Manager::stop, autosave::host_on_session_end) while the level still runs.
void quit_now()
{
	::Msg("[coop] dedicated: the world is saved, the host goes down");
	if (mgr().active())
		mgr().stop("the server is shutting down");
	coop_console::quit_done();
	Console->Execute("quit");
}

const char* level_name(PeerId peer)
{
	const char* name = levels::level_name(levels::level_of_player(peer));
	return name ? name : "no level";
}

// --- the commands

class CCC_CoopPlayers : public IConsole_Command
{
public:
	CCC_CoopPlayers(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		if (!mgr().is_host())
		{
			::Msg("! [coop] coop_players: only the host of a session has the list");
			return;
		}
		const std::vector<PeerInfo> peers = mgr().session().peers();
		string128 line;
		xr_sprintf(line, "[coop] %u player(s) in the session", u32(peers.size()));
		say(line);
		for (const PeerInfo& p : peers)
		{
			string256 row;
			xr_sprintf(row, "[coop]   #%u %-16s %-16s rtt %4u ms loss %4.1f%% %s", p.id, p.nick.c_str(), level_name(p.id), p.rtt_ms, p.packet_loss * 100.f,
			    p.state == PeerState::Joined ? "joined" : "connecting");
			say(row);
		}
	}
	void Info(TInfo& I) override { xr_strcpy(I, "host: the players of the session: id, nick, level, rtt, loss"); }
};

class CCC_CoopKick : public IConsole_Command
{
public:
	CCC_CoopKick(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }
	void Execute(LPCSTR args) override
	{
		if (!mgr().is_host())
		{
			::Msg("! [coop] coop_kick: only the host of a session can do this");
			return;
		}
		std::string who = args ? args : "";
		std::string why = "kicked by the host";
		const size_t blank = who.find(' ');
		if (blank != std::string::npos)
		{
			why = who.substr(blank + 1);
			who.erase(blank);
		}
		const std::vector<PeerInfo> peers = mgr().session().peers();
		const PeerInfo* target = nullptr;
		for (const PeerInfo& p : peers)
			if (std::to_string(p.id) == who || !_stricmp(p.nick.c_str(), who.c_str()))
				target = &p;
		if (!target)
		{
			::Msg("! [coop] coop_kick: nobody is '%s': coop_players", who.c_str());
			return;
		}
		::Msg("[coop] kicked #%u '%s': %s", target->id, target->nick.c_str(), why.c_str());
		mgr().session().kick(target->id, RejectReason::Kicked, why);
	}
	void Info(TInfo& I) override { xr_strcpy(I, "host: coop_kick <id|nick> [reason]: the player leaves the session, his character stays in the world"); }
};

// The clock of the world. Forward only: the stock clock has no way back, and neither do its timers.
void change_time(u32 minutes)
{
	game_sv_Single* single = g_pGameLevel && Level().Server ? smart_cast<game_sv_Single*>(Level().Server->game) : nullptr;
	if (!single || !ai().get_alife())
	{
		::Msg("! [coop] coop_time: no world");
		return;
	}
	const u32 seconds = minutes * 60;
	g_pGamePersistent->Environment().ChangeGameTime(float(seconds));
	single->alife().time_manager().change_game_time(seconds * 1000);
	worldstate::note_time_jump(s64(seconds) * 1000); // the sky's sections follow at once (no level_weathers forces it here)
}

class CCC_CoopTime : public IConsole_Command
{
public:
	CCC_CoopTime(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		if (!ai().get_alife() || !mgr().is_host())
		{
			::Msg("! [coop] coop_time: only the host of a world can do this");
			return;
		}
		if (args && *args)
		{
			u32 hour = 0, minute = 0;
			if (sscanf(args, "%u:%u", &hour, &minute) < 1 || hour > 23 || minute > 59)
			{
				::Msg("! usage: coop_time [hh[:mm]]");
				return;
			}
			u32 y, mo, d, h, mi, s, ms;
			split_time(ai().alife().time_manager().game_time(), y, mo, d, h, mi, s, ms);
			const u32 now = h * 60 + mi;
			const u32 target = hour * 60 + minute;
			change_time(target > now ? target - now : target + 24 * 60 - now);
		}
		u32 y, mo, d, h, mi, s, ms;
		split_time(ai().alife().time_manager().game_time(), y, mo, d, h, mi, s, ms);
		string128 line;
		xr_sprintf(line, "[coop] game time %04u.%02u.%02u %02u:%02u, factor %.1f", y, mo, d, h, mi, ai().alife().time_manager().time_factor());
		say(line);
	}
	void Info(TInfo& I) override { xr_strcpy(I, "host: the game time; coop_time hh[:mm] moves the world forward to that hour"); }
};

class CCC_CoopRestore : public IConsole_Command
{
public:
	CCC_CoopRestore(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = true;
		bLowerCaseArgs = false;
	}
	void Execute(LPCSTR args) override
	{
		if (!mgr().is_host())
		{
			::Msg("! [coop] coop_restore: only the host of a session can do this");
			return;
		}
		std::vector<SaveFile> files;
		list_saves(files);
		const std::vector<SaveFile> list = restore_list(std::move(files));
		const std::string what = args ? args : "";
		if (what.empty())
		{
			u32 n = 0;
			for (const SaveFile& f : list)
			{
				string256 row;
				xr_sprintf(row, "[coop]   %2u %s", ++n, f.name.c_str());
				say(row);
			}
			if (list.empty())
				say("[coop] no autosaves in this folder");
			return;
		}
		const std::string name = pick_restore(list, what);
		if (name.empty())
		{
			::Msg("! [coop] coop_restore: no autosave '%s': coop_restore lists them", what.c_str());
			return;
		}
		// the session stays: a new world is a new epoch, everybody loads it (join::on_world_created)
		::Msg("[coop] the world goes back to %s", name.c_str());
		Console->Execute(start_line(World::Load, name).c_str());
	}
	void Info(TInfo& I) override { xr_strcpy(I, "host: coop_restore lists the autosaves, coop_restore <n|name> loads one: everybody follows"); }
};

class CCC_CoopWorld : public IConsole_Command
{
public:
	CCC_CoopWorld(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }
	void Execute(LPCSTR args) override
	{
		const std::string what = args ? args : "";
		if (what == "list")
		{
			std::vector<SaveFile> files;
			list_saves(files);
			for (const SaveFile& f : files)
				say(("[coop]   " + f.name).c_str());
			return;
		}
		if (g.world_asked || g_pGameLevel)
		{
			::Msg("! [coop] coop_world: this process has its world, to go to another use coop_restore");
			return;
		}
		if (what == "new")
			start_world(World::New, "");
		else
			start_world(World::Load, what);
	}
	void Info(TInfo& I) override { xr_strcpy(I, "dedicated host: coop_world new | last | <save> | list"); }
};

class CCC_CoopConsoleLog : public IConsole_Command
{
public:
	CCC_CoopConsoleLog(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		if (args && *args)
			coop_console::set_log_level(std::atoi(args));
		::Msg("[coop] the window shows level %d of the log: 0 errors, 1 the session, 2 everything", coop_console::log_level());
	}
	void Info(TInfo& I) override { xr_strcpy(I, "dedicated host: what of the log the window shows: 0 errors, 1 the session, 2 everything"); }
};
} // namespace

bool on() { return g_coop_dedicated; }

const Args& args() { return g.args; }

bool new_world() { return g.new_world; }
bool quitting() { return g.quitting; }

void frame()
{
	if (!on())
		return;
	if (!g.parsed)
	{
		g.parsed = true;
		first_frame();
	}
	string1024 line;
	while (coop_console::next_line(line, sizeof(line)))
		run_line(line);
	if (coop_console::quit_asked())
		g.quitting = true;
	if (g.quit_sent)
		return;
	if (g.quitting)
	{
		g.quit_sent = true;
		quit_now();
		return;
	}
	if (world_up())
	{
		g.new_world = false; // the next world is a save (coop_restore)
		mapserver::ghost_frame();
	}
	update_title();
}

bool place_ghost(CALifeObjectRegistry& objects)
{
	const CGameGraph& graph = ai().game_graph();
	GameGraph::_LEVEL_ID level = GameGraph::_LEVEL_ID(-1);
	for (const auto& it : graph.header().levels())
		if (!xr_strcmp(it.second.name(), kServiceLevel))
			level = it.first;
	CSE_ALifeCreatureActor* ghost = smart_cast<CSE_ALifeCreatureActor*>(objects.object(0, true));
	if (!ghost || level == GameGraph::_LEVEL_ID(-1))
	{
		::Msg("! [coop] dedicated: no ghost: actor %p, level '%s' %s", ghost, kServiceLevel, level == GameGraph::_LEVEL_ID(-1) ? "not in the game" : "found");
		return false;
	}
	for (u32 v = 0, n = graph.header().vertex_count(); v < n; ++v)
	{
		const auto* vertex = graph.vertex(GameGraph::_GRAPH_ID(v));
		if (vertex->level_id() != level)
			continue;
		ghost->o_Position = vertex->level_point();
		ghost->m_tNodeID = vertex->level_vertex_id();
		ghost->m_tGraphID = GameGraph::_GRAPH_ID(v);
		ghost->m_holderID = u16(-1);
		ghost->set_health(1.f);
		::Msg("[coop] dedicated: the ghost stands on '%s', vertex %u", kServiceLevel, v);
		return true;
	}
	::Msg("! [coop] dedicated: level '%s' has no vertex in the game graph", kServiceLevel);
	return false;
}

void register_console()
{
	CMD1(CCC_CoopPlayers, "coop_players");
	CMD1(CCC_CoopKick, "coop_kick");
	CMD1(CCC_CoopTime, "coop_time");
	CMD1(CCC_CoopRestore, "coop_restore");
	CMD1(CCC_CoopWorld, "coop_world");
	CMD1(CCC_CoopConsoleLog, "coop_console_log");
}
} // namespace dedicated
} // namespace coop
