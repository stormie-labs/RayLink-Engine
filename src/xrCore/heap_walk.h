// The bytes in use on a Win32 heap, counted by walking it under the heap's own lock (xrMemory::mem_usage).
// HeapWalk over a heap that other threads change at the same time may never end (the Win32 HeapWalk contract: lock the
// heap for the walk). An unlocked walk on the stat_memory_async thread, started at launch while the main thread loads,
// once never returned and kept mem_lock; the main thread then waited on mem_lock in IGame_Persistent::Prefetch for good.
// Under HeapLock the other threads' allocations wait for the walk instead (about half a second on a 1 GB heap).
//
// Header-only, no engine calls: the unit tests include this file as it is (anomaly-coop tests/test_heap_walk.cpp).
#pragma once

#include <windows.h>

#include <stddef.h>

namespace xr_heap
{
struct WalkResult
{
	size_t used = 0; // bytes of the busy entries
	bool complete = false; // the walk reached the end of the heap
	unsigned long error = 0; // GetLastError after the walk (ERROR_NO_MORE_ITEMS when complete), or of a failed HeapLock
};

// visit(entry) sees every entry while the heap is locked: it must not allocate on that heap.
template <class Visit>
WalkResult walk_locked(HANDLE heap, Visit&& visit)
{
	WalkResult r;
	if (!HeapLock(heap))
	{
		r.error = GetLastError();
		return r;
	}
	PROCESS_HEAP_ENTRY entry = {};
	while (HeapWalk(heap, &entry))
	{
		if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
			r.used += entry.cbData;
		visit(static_cast<const PROCESS_HEAP_ENTRY&>(entry));
	}
	r.error = GetLastError();
	r.complete = r.error == ERROR_NO_MORE_ITEMS;
	HeapUnlock(heap);
	return r;
}

inline WalkResult used_bytes(HANDLE heap)
{
	return walk_locked(heap, [](const PROCESS_HEAP_ENTRY&) {});
}
} // namespace xr_heap
