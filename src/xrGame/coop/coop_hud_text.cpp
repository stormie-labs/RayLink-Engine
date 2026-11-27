#include "stdafx.h"
#include "coop_hud_text.h"
#include "xrCoop/coop_death_rules.h" // death::one_conversion

#include "Level.h"
#include "UIGameCustom.h"
#include "string_table.h"
#include "ui/UIStatic.h"

namespace coop
{
namespace hud_text
{
xr_string tr(const char* id, const char* fallback)
{
	const shared_str s = CStringTable().translate(id);
	return s.size() && xr_strcmp(s.c_str(), id) != 0 ? xr_string(s.c_str()) : xr_string(fallback);
}

xr_string tr_format(const char* id, const char* fallback, char conv)
{
	const xr_string f = tr(id, fallback);
	return death::one_conversion(f.c_str(), conv) ? f : xr_string(fallback);
}

void toast(const char* text)
{
	if (!g_pGameLevel || !CurrentGameUI())
		return;
	StaticDrawableWrapper* s = CurrentGameUI()->AddCustomStatic("item_used", true);
	if (s && s->wnd())
		s->wnd()->TextItemControl()->SetText(text);
}

void say(const char* id, const char* fallback)
{
	::Msg("[coop] %s", fallback);
	if (g_pGameLevel && CurrentGameUI())
		toast(tr(id, fallback).c_str());
}

void say_n(const char* id, const char* fallback, u32 n)
{
	if (!g_pGameLevel || !CurrentGameUI())
		return;
	string512 text;
	xr_sprintf(text, tr_format(id, fallback, 'u').c_str(), n);
	toast(text);
}
} // namespace hud_text
} // namespace coop
