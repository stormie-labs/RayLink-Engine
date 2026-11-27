// Сервер: только сервер мира и серверы карт (docs/MODES.md)
// Anomaly Coop — what a server's process costs: memory, Lua, ALife, the frame. One line in the log every 60 s on a server's
// process, and the console command coop_mapserver_stats on any process, the host's too. The frame is the work between two reads
// of the numbers, without a map server's sleep to its 30 frames a second: its budget is 33 ms.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_mapserver_stats.h"
#include "../coop_manager.h"
#include "../coop_levels.h"
#include "../coop_ai.h" // print_tracks_status
#include "../coop_debug.h" // log_address_space
#include "../../../xrEngine/XR_IOConsole.h"
#include "../../../xrEngine/xr_ioc_cmd.h"

#include "ai_space.h"
#include "script_engine.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"

#include <psapi.h>
#pragma comment(lib, "psapi.lib")

namespace coop
{
namespace srv
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
	// a server's first stats line comes with what its memory is made of: nobody can ask a hidden process on its console
	static bool s_map_logged = false;
	if (g_coop_headless && !s_map_logged)
	{
		s_map_logged = true;
		debug::log_address_space();
	}
	::Msg("[coop] coop_mapserver_stats: private %llu MB, working set %llu MB, lua %d MB, alife %u objects, %u online, "
	    "frame avg %.1f ms, worst %u ms, %u frames",
	    u64(memory.PrivateUsage) / (1024 * 1024), u64(memory.WorkingSetSize) / (1024 * 1024), lua_kb / 1024, alife_total,
	    alife_online, average_ms, frames.worst_ms, frames.frames);
	npc_ai::print_tracks_status(); // a process without a window: did the animations of its stalkers go on

	// Where a frame goes, by the engine's own timers. They run only with "rs_stats on" (device.cpp: g_bEnableStatGather), and each
	// is the engine's smoothed value of the last frames, in ms. For finding what to speed up: a measurement, not a setting.
	if (g_bEnableStatGather && Device.Statistic)
	{
		const CStats& s = *Device.Statistic;
		::Msg("[coop] coop_mapserver_stats: frame parts, ms: engine %.2f (scheduler %.2f, objects %.2f, physics %.2f, ai think %.2f, "
		      "ai vision %.2f, ai path %.2f, sound %.2f, net client %.2f, net server %.2f), render %.2f (calc %.2f, draw %.2f, "
		      "animation %.2f, lights %.2f, shadows %.2f, details %.2f, hud %.2f); fps %.1f; objects updated %u of %u (active %u)",
		    s.EngineTOTAL.result, s.Sheduler.result, s.UpdateClient.result, s.Physics.result, s.AI_Think.result, s.AI_Vis.result,
		    s.AI_Path.result, s.Sound.result, s.netClient1.result + s.netClient2.result, s.netServer.result, s.RenderTOTAL.result,
		    s.RenderCALC.result, s.RenderDUMP.result, s.Animation.result, s.RenderDUMP_Lights.result, s.RenderDUMP_Srender.result,
		    s.RenderDUMP_DT_Render.result + s.RenderDUMP_DT_VIS.result, s.RenderDUMP_HUD.result, s.fFPS, s.UpdateClient_updated,
		    s.UpdateClient_total, s.UpdateClient_active);
	}
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
	PROF_EVENT("coop/coop_mapserver_stats/mapserver_stats_frame");
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

void register_mapserver_stats_console()
{
	CMD1(CCC_CoopMapserverStats, "coop_mapserver_stats");
}
} // namespace srv
} // namespace coop
