#include "stdafx.h"
#include "pch_script.h"
// F3a T5: what a map server costs (plan 1.8): memory, Lua, ALife, the frame. One line in the log every 60 s on a map server, and
// the console command coop_mapserver_stats on any process, the host's too. The frame is the work between two reads of the
// numbers, without a map server's sleep to its 30 frames a second: its budget is 33 ms.
#include "coop_manager.h"
#include "coop_levels.h"
#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"

#include "ai_space.h"
#include "script_engine.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"

#include <psapi.h>
#pragma comment(lib, "psapi.lib")

namespace coop
{
namespace
{
constexpr u32 kStatsPeriodMs = 60000;

void log_stats()
{
	PROCESS_MEMORY_COUNTERS_EX memory = {};
	memory.cb = sizeof(memory);
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));

	lua_State* const lua = ai().script_engine().lua();
	const int lua_kb = lua ? lua_gc(lua, LUA_GCCOUNT, 0) : 0;

	u32 alife_total = 0;
	u32 alife_online = 0;
	if (ai().get_alife())
	{
		for (const auto& object : ai().alife().objects().objects())
		{
			++alife_total;
			if (object.second->m_bOnline)
				++alife_online;
		}
	}

	const CRenderDevice::FrameWork frames = Device.frame_work;
	Device.frame_work = CRenderDevice::FrameWork();
	const float average_ms = frames.frames ? float(frames.total_ms) / float(frames.frames) : 0.f;

	if (is_map_server())
		levels::xfer_print_status(); // the line that says where a level stays empty
	::Msg("[coop] coop_mapserver_stats: private %llu MB, working set %llu MB, lua %d MB, alife %u objects, %u online, "
	    "frame avg %.1f ms, worst %u ms, %u frames",
	    u64(memory.PrivateUsage) / (1024 * 1024), u64(memory.WorkingSetSize) / (1024 * 1024), lua_kb / 1024, alife_total,
	    alife_online, average_ms, frames.worst_ms, frames.frames);
}

class CCC_CoopMapserverStats : public IConsole_Command
{
public:
	CCC_CoopMapserverStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { log_stats(); }
	void Info(TInfo& I) override { xr_strcpy(I, "this process: memory, Lua, ALife objects, the frame since the last line"); }
};
} // namespace

void mapserver_stats_frame()
{
	if (!g_coop_headless)
		return;

	static u32 next_ms = 0;
	const u32 now_ms = GetTickCount();
	if (!next_ms)
		next_ms = now_ms + kStatsPeriodMs;
	if (int(now_ms - next_ms) < 0)
		return;
	next_ms = now_ms + kStatsPeriodMs;
	log_stats();
	xrLogger::FlushLog(); // the stock flush waits for a freeze or the console: nobody reads this process but through its log
}

void mapserver_stats_register_console()
{
	CMD1(CCC_CoopMapserverStats, "coop_mapserver_stats");
}
} // namespace coop
