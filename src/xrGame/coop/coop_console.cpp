#include "stdafx.h"
#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_sync.h"

#include <cstdlib>

namespace coop
{
namespace
{
// "1.2.3.4:5678" / "1.2.3.4" / "[::1]:5678" -> address + port.
bool parse_endpoint(LPCSTR args, xr_string& address, u16& port)
{
	xr_string s = args;
	while (!s.empty() && s.front() == ' ') s.erase(0, 1);
	while (!s.empty() && s.back() == ' ') s.pop_back();
	if (s.empty())
		return false;
	port = kDefaultPort;
	size_t colon = s.rfind(':');
	if (s.front() == '[')
	{
		size_t close = s.find(']');
		if (close == xr_string::npos)
			return false;
		address = s.substr(1, close - 1);
		if (close + 1 < s.size() && s[close + 1] == ':')
			port = (u16)std::atoi(s.c_str() + close + 2);
		return !address.empty() && port != 0;
	}
	if (colon != xr_string::npos && s.find(':') == colon)
	{
		address = s.substr(0, colon);
		port = (u16)std::atoi(s.c_str() + colon + 1);
	}
	else
		address = s;
	return !address.empty() && port != 0;
}

class CCC_CoopHost : public IConsole_Command
{
public:
	CCC_CoopHost(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		u16 port = kDefaultPort;
		if (args && *args)
			port = (u16)std::atoi(args);
		if (!port)
		{
			::Msg("! usage: coop_host [port]");
			return;
		}
		mgr().start_host(port);
	}
	void Info(TInfo& I) override { xr_strcpy(I, "host a co-op session for the current game: coop_host [port]"); }
};

class CCC_CoopConnect : public IConsole_Command
{
public:
	CCC_CoopConnect(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		xr_string address;
		u16 port = 0;
		if (!parse_endpoint(args, address, port))
		{
			::Msg("! usage: coop_connect <ip>[:port]");
			return;
		}
		mgr().start_client(address.c_str(), port);
	}
	void Info(TInfo& I) override { xr_strcpy(I, "join a co-op session: coop_connect <ip>[:port]"); }
};

class CCC_CoopDisconnect : public IConsole_Command
{
public:
	CCC_CoopDisconnect(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { mgr().stop("left the session"); }
	void Info(TInfo& I) override { xr_strcpy(I, "leave or close the co-op session"); }
};

class CCC_CoopStatus : public IConsole_Command
{
public:
	CCC_CoopStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { mgr().print_status(); }
	void Info(TInfo& I) override { xr_strcpy(I, "co-op session status, players, RTT and traffic by message type"); }
};

class CCC_CoopChat : public IConsole_Command
{
public:
	CCC_CoopChat(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }
	void Execute(LPCSTR args) override { mgr().send_chat(args); }
	void Info(TInfo& I) override { xr_strcpy(I, "send a chat line to all players"); }
};

class CCC_CoopNetSim : public IConsole_Command
{
public:
	CCC_CoopNetSim(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		NetSim sim;
		if (args && *args && xr_strcmp(args, "off"))
		{
			unsigned lag = 0, jitter = 0;
			float loss_pct = 0.f;
			const int n = sscanf(args, "%u %u %f", &lag, &jitter, &loss_pct);
			if (n < 1)
			{
				::Msg("! usage: coop_netsim <lag_ms> [jitter_ms] [loss_percent] | off");
				return;
			}
			sim.lag_ms = lag;
			sim.jitter_ms = jitter;
			sim.loss = loss_pct / 100.f;
		}
		mgr().set_netsim(sim);
	}
	void Info(TInfo& I) override { xr_strcpy(I, "emulate a bad network: coop_netsim <lag_ms> [jitter_ms] [loss_%] | off"); }
};

class CCC_CoopDump : public IConsole_Command
{
public:
	CCC_CoopDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override { sync::dump_state(args); }
	void Info(TInfo& I) override { xr_strcpy(I, "write a comparable world-state snapshot to the log: coop_dump [object id]"); }
};
} // namespace

void register_console_commands()
{
	CMD1(CCC_CoopHost, "coop_host");
	CMD1(CCC_CoopConnect, "coop_connect");
	CMD1(CCC_CoopDisconnect, "coop_disconnect");
	CMD1(CCC_CoopStatus, "coop_status");
	CMD1(CCC_CoopChat, "coop_chat");
	CMD1(CCC_CoopNetSim, "coop_netsim");
	CMD1(CCC_CoopDump, "coop_dump");
	CMD3(CCC_String, "coop_nick", g_coop_nick, sizeof(g_coop_nick));
}
} // namespace coop
