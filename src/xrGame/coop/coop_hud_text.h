// Anomaly Coop: text the player reads on the HUD. A string of text/*/st_coop_*.xml in his language with the caller's English
// fallback (tr), a translated format that is safe to fill (tr_format), the HUD toast that shows a line (toast), and the messages of the
// coop layer (say, say_n). The log keeps the English fallback.
#pragma once

namespace coop
{
namespace hud_text
{
// The string `id` of the string table, `fallback` while the table does not have it (translate answers the id itself then).
xr_string tr(const char* id, const char* fallback);

// tr() of a format the caller fills with one value: the translation only when it has exactly one '%' and that one is followed by
// `conv` ('d', 's') (death::one_conversion), else `fallback`. A broken translation never reaches sprintf with the wrong arguments.
xr_string tr_format(const char* id, const char* fallback, char conv);

// Shows `text` in the HUD's toast (the "item_used" custom static); nothing without a level and its UI.
void toast(const char* text);

// The line `id` (tr) in the HUD's toast, its English `fallback` in the log ("[coop] <fallback>", also without a level).
void say(const char* id, const char* fallback);

// The format `id` (tr_format, conversion 'u') filled with `n` in the HUD's toast. Nothing in the log: a caller may show it every second.
void say_n(const char* id, const char* fallback, u32 n);
} // namespace hud_text
} // namespace coop
