// RayLink (crash triage, 2026-11-25, report RL-5TE8-EFGE): why a D3D device was lost, as text for the log and the FATAL.
// A FATAL said only the system's "The GPU device instance has been suspended. Use GetDeviceRemovedReason..." on the first
// call that met the lost device (a texture load), not why the device went away. The renderer asks the device and its
// adapter (dx10HW.cpp, xrDebug::set_device_lost_note) and formats the answer here.
//
// Pure and header-only: no D3D, no windows.h, no allocation, no engine calls (the crash path calls it, and it must not end
// in Debug again). The unit tests include this file as it is (anomaly-coop tests/test_gpu_device_lost.cpp).
#pragma once

#include <stddef.h>
#include <stdio.h>

namespace xr_gpu
{
// The HRESULT values of winerror.h (Windows SDK 10.0.26100.0). Its names are macros there, so these are spelled otherwise.
constexpr long code_ok = 0L;                                                       // S_OK
constexpr long code_invalid_call = static_cast<long>(0x887A0001UL);                // DXGI_ERROR_INVALID_CALL
constexpr long code_device_removed = static_cast<long>(0x887A0005UL);              // DXGI_ERROR_DEVICE_REMOVED
constexpr long code_device_hung = static_cast<long>(0x887A0006UL);                 // DXGI_ERROR_DEVICE_HUNG
constexpr long code_device_reset = static_cast<long>(0x887A0007UL);                // DXGI_ERROR_DEVICE_RESET
constexpr long code_driver_internal_error = static_cast<long>(0x887A0020UL);       // DXGI_ERROR_DRIVER_INTERNAL_ERROR
constexpr long code_out_of_memory = static_cast<long>(0x8007000EUL);               // E_OUTOFMEMORY

// The answers of a call that met a lost device. Only these get the renderer's note; every other HRESULT keeps its text.
constexpr bool is_device_lost(long code)
{
	return code == code_device_removed || code == code_device_reset || code == code_device_hung;
}

// What ID3D11Device::GetDeviceRemovedReason answers, by name; nullptr for a code without one here.
inline const char* reason_name(long code)
{
	switch (code)
	{
	case code_ok: return "S_OK";
	case code_device_hung: return "DXGI_ERROR_DEVICE_HUNG";
	case code_device_removed: return "DXGI_ERROR_DEVICE_REMOVED";
	case code_device_reset: return "DXGI_ERROR_DEVICE_RESET";
	case code_driver_internal_error: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
	case code_invalid_call: return "DXGI_ERROR_INVALID_CALL";
	case code_out_of_memory: return "E_OUTOFMEMORY";
	default: return nullptr;
	}
}

// The name, or "0x%08lX" for a code without one. out is always terminated (size > 0).
inline void reason_text(long code, char* out, size_t size)
{
	if (!out || !size)
		return;
	const char* name = reason_name(code);
	if (name)
		snprintf(out, size, "%s", name);
	else
		snprintf(out, size, "0x%08lX", static_cast<unsigned long>(code));
}

// The note, with a leading space (it goes after a description):
//   " [GetDeviceRemovedReason: DXGI_ERROR_DEVICE_HUNG; local video memory: 6512 MB used of 15872 MB budget]"
// and without the memory part when the adapter could not tell it (have_memory false: no IDXGIAdapter3, or its query failed).
// Bytes are shown in whole MB, rounded down. out is always terminated (size > 0); a short out cuts the note.
inline void format_note(char* out, size_t size, long reason, bool have_memory, unsigned long long used_bytes,
                        unsigned long long budget_bytes)
{
	if (!out || !size)
		return;
	char reason_buffer[40]; // the longest name, DXGI_ERROR_DRIVER_INTERNAL_ERROR, is 32
	reason_text(reason, reason_buffer, sizeof(reason_buffer));
	const unsigned long long mb = 1024ull * 1024ull;
	if (have_memory)
		snprintf(out, size, " [GetDeviceRemovedReason: %s; local video memory: %llu MB used of %llu MB budget]", reason_buffer,
		         used_bytes / mb, budget_bytes / mb);
	else
		snprintf(out, size, " [GetDeviceRemovedReason: %s]", reason_buffer);
}

// Puts note at the end of text, before the line break text ends with (FormatMessage ends its text with "\r\n"): the
// description stays one line and keeps its break. text holds size chars; it is terminated first (a text that filled its
// buffer loses its last char). When the room runs out the note is cut, never the break. No CRT string calls: the renderer
// sees strlen and memmove deprecated (xrCore vector.h).
inline void append_before_line_end(char* text, size_t size, const char* note)
{
	if (!text || !size || !note)
		return;
	text[size - 1] = 0;
	size_t length = 0;
	while (text[length])
		++length;
	size_t body = length;
	while (body > 0 && (text[body - 1] == '\r' || text[body - 1] == '\n'))
		--body;

	char tail[8];
	size_t tail_length = length - body;
	if (tail_length > sizeof(tail) - 1)
		tail_length = sizeof(tail) - 1;
	for (size_t i = 0; i < tail_length; ++i)
		tail[i] = text[body + i];

	const size_t room = size - 1 - body; // chars after the body, the terminator not counted
	if (tail_length > room)
		tail_length = room;
	size_t at = body;
	for (size_t i = 0; note[i] && at < size - 1 - tail_length; ++i)
		text[at++] = note[i];
	for (size_t i = 0; i < tail_length; ++i)
		text[at++] = tail[i];
	text[at] = 0;
}
} // namespace xr_gpu
