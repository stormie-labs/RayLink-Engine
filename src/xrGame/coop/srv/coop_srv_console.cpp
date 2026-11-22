// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — the server's console commands (docs/MODES.md section 6, "Консольные команды"; step S4 of
// docs/plans/2026-11-18-two-modes.md, unit A-S4H): srv_levels_status, srv_levels_audit, srv_max_map_servers, srv_mapserver_linger,
// srv_map_servers_rev. Moved from coop_levels.cpp (levels::register_console). Step S8 removes the old names for status and audit.
//
// The three settings are machine settings saved in user.ltx, under the srv_* name only (review A-REV-E-1, finding 7); their values live
// where the levels read them (coop_levels.cpp, levels::g_coop_max_map_servers ...), so a value read under either name is the one in
// force. The bounds are those of build 50 (levels::settle_machine_settings moves the old defaults once a machine).
// Registered once at the start of the process, with the Core's commands, by the server's layer (srv/coop_srv_mode.cpp
// ServerLayer::register_console, through the switchboard: coop_mode.cpp register_layers_console).
#include "stdafx.h"
#include "coop/srv/coop_dedicated.h"
#include "coop/coop_levels.h"
#include "coop/coop_manager.h"
#include "../../../xrEngine/XR_IOConsole.h"
#include "coop_host_console.h"
#include "../../../xrEngine/xr_ioc_cmd.h"

namespace coop
{
namespace levels
{
extern int g_coop_map_servers_rev; // coop_levels.cpp: saved, the two above were moved to build 50's defaults
} // namespace levels

namespace srv
{
namespace
{
class CCC_SrvLevelsStatus : public IConsole_Command
{
public:
	CCC_SrvLevelsStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { levels::print_status(); }
	void Info(TInfo& I) override { xr_strcpy(I, "the server's maps: the switches, this level, who is on it, the map servers"); }
};

class CCC_SrvLevelsAudit : public IConsole_Command
{
public:
	CCC_SrvLevelsAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { levels::xfer_audit_now(); }
	void Info(TInfo& I) override { xr_strcpy(I, "the server's maps: check now that every object has exactly one owner"); }
};

class CCC_SrvNetStats final : public IConsole_Command
{
public:
	CCC_SrvNetStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Info(TInfo& I) override { xr_strcpy(I, "ENet payload bytes per second over five seconds, by connection"); }
	void Execute(LPCSTR) override
	{
		uint64_t total_out = 0, total_in = 0;
		for (const PeerTraffic& stats : mgr().session().peer_traffic())
		{
			string32 peer;
			xr_sprintf(peer, "%u", unsigned(stats.id));
			print_rate(peer, stats.out_5s, stats.in_5s);
			total_out += stats.out_5s;
			total_in += stats.in_5s;
		}
		print_rate("total", total_out, total_in);
	}

private:
	static void print_rate(const char* peer, uint64_t out, uint64_t in)
	{
		string128 line;
		xr_sprintf(line, "@rl net peer=%s out=%llu in=%llu", peer,
			static_cast<unsigned long long>(out / 5), static_cast<unsigned long long>(in / 5));
		::Msg("%s", line);
		// The console's ordinary log filter shows raw @rl lines only at level 2.
		if (coop_console::log_level() < 2)
			coop_console::print(line);
	}
};

// The old coop_* name of a setting: typed, or read from user.ltx, it sets the same variable within the same
// bounds, and it is never saved. CCC_SaveCFG writes every command in name order and user.ltx runs in file order: with both names
// saved, the srv_* line (after the coop_* one) won over an edit of the coop_* one. A user.ltx of an older build (coop_* lines) is
// read once and saved under the srv_* names from then on.
class CCC_SrvOldName final : public CCC_Integer
{
public:
	CCC_SrvOldName(LPCSTR N, int* V, int lo, int hi) : CCC_Integer(N, V, lo, hi) {}
	void Save(IWriter*) override {}
};
} // namespace

void register_console()
{
	// machine setting, default 6: the most map servers this PC can carry. The cap in force is min(players + 1, this, 7) (host_map_cap)
	CMD4(CCC_Integer, "srv_max_map_servers", &levels::g_coop_max_map_servers, 0, 7);
	// machine setting, seconds, default 600: how long the map emptied last stays; every other empty map at most 60 s, none with < 2 GB free
	CMD4(CCC_Integer, "srv_mapserver_linger", &levels::g_coop_mapserver_linger, 0, 3600);
	CMD4(CCC_Integer, "srv_map_servers_rev", &levels::g_coop_map_servers_rev, 0, 1); // saved: the two above were moved to build 50's defaults
	CMD1(CCC_SrvLevelsStatus, "srv_levels_status");
	CMD1(CCC_SrvLevelsAudit, "srv_levels_audit");
	if (dedicated::on())
	{
		CMD1(CCC_SrvNetStats, "srv_netstats");
	}

	// Legacy names for loading the machine settings; user.ltx saves the srv_* names only.
	CMD4(CCC_SrvOldName, "coop_max_map_servers", &levels::g_coop_max_map_servers, 0, 7);
	CMD4(CCC_SrvOldName, "coop_mapserver_linger", &levels::g_coop_mapserver_linger, 0, 3600);
	CMD4(CCC_SrvOldName, "coop_map_servers_rev", &levels::g_coop_map_servers_rev, 0, 1);
}
} // namespace srv
} // namespace coop
