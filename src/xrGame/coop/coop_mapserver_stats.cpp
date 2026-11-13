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

// What the process's memory is made of, as the system sees it (owner 2026-11-13: fewer gigabytes; a server's private memory is
// some 700 MB more than the engine's own heap, and nothing said what that is). Committed memory by kind: private (heaps, stacks,
// VirtualAlloc), mapped files, images of the exe and its DLLs; then the largest private allocations, each as the whole range one
// VirtualAlloc made. A report for the log; it changes nothing.
void log_address_space()
{
	struct Block
	{
		u64 base = 0;
		u64 committed = 0;
		u64 reserved = 0;
	};
	xr_vector<Block> blocks;
	u64 private_mb = 0, mapped_mb = 0, image_mb = 0, reserved_mb = 0;
	MEMORY_BASIC_INFORMATION info;
	for (const u8* p = nullptr; VirtualQuery(p, &info, sizeof(info)) == sizeof(info);)
	{
		const u64 size = u64(info.RegionSize);
		if (info.State == MEM_COMMIT)
		{
			if (info.Type == MEM_PRIVATE)
				private_mb += size;
			else if (info.Type == MEM_MAPPED)
				mapped_mb += size;
			else
				image_mb += size;
		}
		else if (info.State == MEM_RESERVE)
			reserved_mb += size;
		if (info.State != MEM_FREE && info.Type == MEM_PRIVATE)
		{
			const u64 base = u64(reinterpret_cast<uintptr_t>(info.AllocationBase));
			if (blocks.empty() || blocks.back().base != base)
			{
				Block b;
				b.base = base;
				blocks.push_back(b);
			}
			(info.State == MEM_COMMIT ? blocks.back().committed : blocks.back().reserved) += size;
		}
		const u8* next = static_cast<const u8*>(info.BaseAddress) + info.RegionSize;
		if (next <= p)
			break; // the end of the address space
		p = next;
	}
	const u64 mb = 1024 * 1024;
	::Msg("[coop] coop_memory_map: committed: private %llu MB, mapped files %llu MB, images %llu MB; reserved and not committed "
	      "%llu MB; %u private allocations",
	    private_mb / mb, mapped_mb / mb, image_mb / mb, reserved_mb / mb, u32(blocks.size()));
	// by size: how many allocations of each size class, and how much they hold
	static const u64 kClass[] = {1 * mb, 4 * mb, 16 * mb, 64 * mb, 256 * mb, ~u64(0)};
	static const char* const kName[] = {"under 1 MB", "1-4 MB", "4-16 MB", "16-64 MB", "64-256 MB", "256 MB and more"};
	u32 count[6] = {};
	u64 total[6] = {};
	for (const Block& b : blocks)
		for (int i = 0; i < 6; ++i)
			if (b.committed < kClass[i])
			{
				++count[i];
				total[i] += b.committed;
				break;
			}
	for (int i = 0; i < 6; ++i)
		if (count[i])
			::Msg("[coop]   private allocations %-15s %6u, %5llu MB", kName[i], count[i], total[i] / mb);
	std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.committed > b.committed; });
	const size_t shown = blocks.size() < 12 ? blocks.size() : 12;
	for (size_t i = 0; i < shown; ++i)
		::Msg("[coop]   %5llu MB committed, %5llu MB more reserved, at %016llx", blocks[i].committed / mb, blocks[i].reserved / mb,
		    blocks[i].base);
}

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
		log_address_space();
	}
	::Msg("[coop] coop_mapserver_stats: private %llu MB, working set %llu MB, lua %d MB, alife %u objects, %u online, "
	    "frame avg %.1f ms, worst %u ms, %u frames",
	    u64(memory.PrivateUsage) / (1024 * 1024), u64(memory.WorkingSetSize) / (1024 * 1024), lua_kb / 1024, alife_total,
	    alife_online, average_ms, frames.worst_ms, frames.frames);

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

// Where the items of the configs live (owner 2026-11-13: fewer gigabytes): the CInifile objects alive now by file, the largest
// first, and the DLTX cache of parsed files. A report for the log; it changes nothing.
class CCC_CoopIniStats : public IConsole_Command
{
public:
	CCC_CoopIniStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		xr_vector<CInifile::LiveRow> rows;
		u64 instances = 0, sections = 0, items = 0, distinct = 0;
		CInifile::LiveStats(rows, instances, sections, items, distinct);
		u64 cache_files = 0, cache_bytes = 0, cache_sections = 0;
		CInifile::GetCacheStats(cache_files, cache_bytes, cache_sections);
		::Msg("[coop] coop_ini_stats: %llu ini objects alive in %u files: %llu sections, %llu items, of them %llu in memory once "
		      "(%llu MB, %u bytes an item; the rest is shared with another object); the cache of parsed files: %llu files, "
		      "%llu sections, %llu MB by its own count",
		    instances, u32(rows.size()), sections, items, distinct, distinct * sizeof(CInifile::Item) / (1024 * 1024),
		    u32(sizeof(CInifile::Item)), cache_files, cache_sections, cache_bytes / (1024 * 1024));
		const size_t shown = rows.size() < 30 ? rows.size() : 30;
		for (size_t i = 0; i < shown; ++i)
			::Msg("[coop]   %9llu items %7llu sections x%-4u %s", rows[i].items, rows[i].sections, rows[i].instances, rows[i].file.c_str());
	}
	void Info(TInfo& I) override { xr_strcpy(I, "this process: the ini objects alive by file, the largest first, and the cache of parsed files"); }
};

class CCC_CoopMemoryMap : public IConsole_Command
{
public:
	CCC_CoopMemoryMap(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { log_address_space(); }
	void Info(TInfo& I) override { xr_strcpy(I, "this process: committed memory by kind and the largest private allocations"); }
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
	CMD1(CCC_CoopIniStats, "coop_ini_stats");
	CMD1(CCC_CoopMemoryMap, "coop_memory_map");
}
} // namespace coop
