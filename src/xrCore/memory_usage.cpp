#include "stdafx.h"
#include <malloc.h>
#include <errno.h>
#include "heap_walk.h"

XRCORE_API void vminfo(size_t* _free, size_t* reserved, size_t* committed)
{
	MEMORY_BASIC_INFORMATION memory_info;
	memory_info.BaseAddress = 0;
	*_free = *reserved = *committed = 0;
	while (VirtualQuery(memory_info.BaseAddress, &memory_info, sizeof(memory_info)))
	{
		switch (memory_info.State)
		{
		case MEM_FREE:
			*_free += memory_info.RegionSize;
			break;
		case MEM_RESERVE:
			*reserved += memory_info.RegionSize;
			break;
		case MEM_COMMIT:
			*committed += memory_info.RegionSize;
			break;
		}
		memory_info.BaseAddress = (char*)memory_info.BaseAddress + memory_info.RegionSize;
	}
}

xrCriticalSection mem_lock;
XRCORE_API void log_vminfo()
{
    xrCriticalSectionGuard g(mem_lock);
	PROF_EVENT("log_vminfo");
	size_t w_free, w_reserved, w_committed;
	vminfo(&w_free, &w_reserved, &w_committed);
	Msg(
		"* [win32]: free[%lld K], reserved[%lld K], committed[%lld K]",
		w_free / 1024,
		w_reserved / 1024,
		w_committed / 1024
	);
}

size_t xrMemory::mem_usage(bool assert)
{
    xrCriticalSectionGuard g(mem_lock);
	PROF_EVENT("mem_usage");
	// coop: the CRT heap is walked under its lock (heap_walk.h); _heapwalk walked it unlocked.
	const xr_heap::WalkResult walk = xr_heap::used_bytes(reinterpret_cast<HANDLE>(_get_heap_handle()));
	if (!walk.complete && assert)
		FATAL("bad heap walk");
	return walk.used;
}
