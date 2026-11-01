// Anomaly Coop — Device.dwTimeContinual, the clock of every UI animation, while the game window is away. Time away does not
// count, and the stock game took it off at once when the window came back. Frames run while it is away, so the clock ran on and
// then jumped back by the whole span. Whatever had started meanwhile stood in its future: a main menu built while a fullscreen
// client was alt-tabbed out had the fade of its titles at 0 on the first move of the pointer (live 2026-11-01). So it stands still
// from the moment the window goes, and comes back to the same value: it never goes back.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstdint>

namespace coop
{
// timer: ms since the start. inactive_total: the spans away that are over. inactive_start: the timer when the window went.
inline uint32_t continual_ms(uint32_t timer, uint32_t inactive_total, uint32_t inactive_start, bool active)
{
	return (active ? timer : inactive_start) - inactive_total;
}
} // namespace coop
