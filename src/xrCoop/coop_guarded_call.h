// Anomaly Coop — stops a C++ exception at a call whose caller could never catch it, and says what it was.
// The renderer's texture loads are such a call: they run in a task group nobody waits on, so what they throw stays in the
// group and the texture reads as loading for ever. SEH, because the render projects build without C++ exceptions.
// Anything that is not a C++ exception (an access violation, say) goes on to the crash handler.
// No engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>

namespace coop::guard
{
constexpr DWORD kCppException = 0xE06D7363;

// What MSVC's throw leaves behind on x64: all links are offsets from the image base in ExceptionInformation[3].
struct ThrowInfoView
{
	uint32_t attributes;
	int32_t unwind;
	int32_t forward;
	int32_t types;
};
struct CatchableView
{
	uint32_t properties;
	int32_t type;
	int32_t mdisp, pdisp, vdisp;
	int32_t size;
	int32_t copy;
};

// The decorated name of the type, then what() if it is a std::exception.
inline void describe(const EXCEPTION_RECORD& r, char* out, size_t cap)
{
	const auto* info = r.NumberParameters >= 4 ? reinterpret_cast<const ThrowInfoView*>(r.ExceptionInformation[2]) : nullptr;
	if (!info)
	{
		snprintf(out, cap, "C++ exception of unknown type");
		return;
	}
	const uintptr_t base = r.ExceptionInformation[3];
	const auto* list = reinterpret_cast<const int32_t*>(base + info->types);
	const char* top = "";
	const std::exception* std_exc = nullptr;
	for (int32_t i = 0; i < list[0]; ++i)
	{
		const auto& c = *reinterpret_cast<const CatchableView*>(base + list[1 + i]);
		const char* name = reinterpret_cast<const char*>(base + c.type) + 2 * sizeof(void*); // past type_info's two pointers
		if (i == 0) top = name;
		if (!strcmp(name, ".?AVexception@std@@"))
			std_exc = reinterpret_cast<const std::exception*>(r.ExceptionInformation[1] + c.mdisp);
	}
	snprintf(out, cap, "%s%s%s", top, std_exc ? ": " : "", std_exc ? std_exc->what() : "");
}

inline int filter(const EXCEPTION_POINTERS* e, char* why, size_t cap)
{
	if (e->ExceptionRecord->ExceptionCode != kCppException) return EXCEPTION_CONTINUE_SEARCH;
	describe(*e->ExceptionRecord, why, cap);
	return EXCEPTION_EXECUTE_HANDLER;
}

// True if fn came back on its own; false with `why` filled if a C++ exception left it.
inline bool call(void (*fn)(void*), void* arg, char* why, size_t cap)
{
	__try
	{
		fn(arg);
		return true;
	}
	__except (filter(GetExceptionInformation(), why, cap))
	{
		return false;
	}
}
}
