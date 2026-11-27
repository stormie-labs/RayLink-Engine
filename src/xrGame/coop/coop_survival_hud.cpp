// Anomaly Coop — player survival: what the player reads of it. The feed of events, the engine's own HUD lines of a player who is
// down, dead or reviving, and coop.revive_progress for a Lua HUD. See coop_survival.h.
#include "stdafx.h"
#include "coop_survival_state.h"
#include "coop_manager.h"
#include "coop_join.h"

#include "../../xrEngine/GameFont.h"
#include "ui_base.h"

#include <cstdarg>

namespace coop
{
namespace survival
{
// ---------------------------------------------------------------- state (coop_survival_state.h)

xr_vector<HudLine> g_hud;
u64 g_lua_polled = 0; // when Lua last asked coop.revive_progress (0 = never): its HUD draws the down / revive text then
xr_deque<FeedLine> g_feed;

namespace
{
constexpr u32 kSentShownMs = 3000; // the full ring of a finished hold waits this long for the host's Done (coop.revive_progress)
} // namespace

// A feed line: `shown` on the HUD, `logged` in the log (the English text our tools and reports read).
void feed_line(const char* shown, const char* logged)
{
	::Msg("[coop] %s", logged);
	FeedLine l;
	l.text = shown;
	l.until = mgr().now_ms() + 7000;
	g_feed.push_back(l);
	while (g_feed.size() > 5)
		g_feed.pop_front();
}

void feed(const char* fmt, ...)
{
	string512 buf;
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	buf[sizeof(buf) - 1] = 0;
	feed_line(buf, buf);
}

// Feed lines the player reads in his language (text/*/st_coop_survival.xml); the log keeps the English fallback. `fallback`
// has exactly one conversion (%s / %d).
void feed_tr(const char* id, const char* fallback)
{
	feed_line(tr(id, fallback).c_str(), fallback);
}

void feed_tr_s(const char* id, const char* fallback, const char* value)
{
	string512 shown, logged;
	xr_sprintf(logged, fallback, value);
	xr_sprintf(shown, tr_format(id, fallback, 's').c_str(), value);
	feed_line(shown, logged);
}

void feed_tr_d(const char* id, const char* fallback, int value)
{
	string512 shown, logged;
	xr_sprintf(logged, fallback, value);
	xr_sprintf(shown, tr_format(id, fallback, 'd').c_str(), value);
	feed_line(shown, logged);
}

void build_hud(u64 t)
{
	g_hud.clear();
	auto line = [](const char* text, u32 color, bool big)
	{
		HudLine l;
		l.text = text;
		l.color = color;
		l.big = big;
		g_hud.push_back(l);
	};
	string256 buf;
	// a Lua HUD that polls coop.revive_progress draws the down and revive state (ring, timer, texts) itself
	const bool lua_hud = g_lua_polled && t - g_lua_polled < kLuaHudMs;
	switch (g_me.life)
	{
	case Life::Wounded:
	{
		if (lua_hud)
			break;
		line(tr("st_coop_hud_down", "YOU ARE DOWN").c_str(), 0xffe03c3c, true);
		const u32 left = g_me.bleed_end > t ? u32((g_me.bleed_end - t + 999) / 1000) : 0;
		xr_sprintf(buf, tr_format("st_coop_hud_bleeding", "Bleeding out: %d s. A teammate can revive you with a medkit.", 'd').c_str(),
		    int(left));
		line(buf, 0xfff0f0f0, false);
		if (g_me.giveup_start)
		{
			xr_sprintf(buf, "%s %u%%", tr("st_coop_revive_giving_up", "Giving up...").c_str(),
			    u32(_min<u64>(100, (t - g_me.giveup_start) * 100 / kGiveUpHoldMs)));
			line(buf, 0xffffa040, false);
		}
		else
			line(tr("st_coop_revive_give_up", "Hold USE to give up.").c_str(), 0xffa0a0a0, false);
		break;
	}
	case Life::Dead:
		if (!g_me.evac_end)
		{
			line(tr("st_coop_hud_dead", "YOU DIED").c_str(), 0xffe03c3c, true);
			line(tr("st_coop_hud_wait_host", "Waiting for the host...").c_str(), 0xfff0f0f0, false);
			break;
		}
		{
			line(tr("st_coop_evac_title", "You were pulled out...").c_str(), 0xffe03c3c, true);
			const int left = g_me.evac_end > t ? int((g_me.evac_end - t + 999) / 1000) : 0;
			xr_sprintf(buf, tr_format("st_coop_evac_wait", "Back at your camp in %d s", 'd').c_str(), left);
			line(buf, 0xfff0f0f0, false);
			if (g_spec.valid && g_me.spectate != kNoId)
			{
				xr_sprintf(buf, tr_format("st_coop_evac_watch", "Watching: %s", 's').c_str(),
				    nick_of(join::to_host(g_me.spectate)).c_str());
				line(buf, 0xfff0f0f0, false);
				line(tr("st_coop_hud_spectate_keys", "Fire: next player   Aim: previous player").c_str(), 0xffa0a0a0, false);
			}
			else
				line(tr("st_coop_evac_no_watch", "Nobody here to watch.").c_str(), 0xffa0a0a0, false);
		}
		break;
	case Life::Respawning: line(tr("st_coop_hud_respawning", "Respawning...").c_str(), 0xfff0f0f0, true); break;
	case Life::Spectating: // out of a hardcore run: watch the others, or start a new character
	{
		line(tr("st_coop_hardcore_out", "Your character is gone.").c_str(), 0xffe03c3c, true);
		if (g_me.spectate != kNoId)
		{
			xr_sprintf(buf, tr_format("st_coop_evac_watch", "Watching: %s", 's').c_str(),
			    nick_of(join::to_host(g_me.spectate)).c_str());
			line(buf, 0xfff0f0f0, false);
			line(tr("st_coop_hud_spectate_keys", "Fire: next player   Aim: previous player").c_str(), 0xffa0a0a0, false);
		}
		else
			line(tr("st_coop_evac_no_allies", "Nobody is left alive. The world is not rolled back.").c_str(),
			    0xffa0a0a0, false);
		if (g_me.newchar_asked)
			line(tr("st_coop_hud_newchar_starting", "Starting a new character...").c_str(), 0xffffa040, false);
		else if (g_me.newchar_start)
		{
			xr_sprintf(buf, "%s %u%%", tr("st_coop_hud_newchar", "New character...").c_str(),
			    u32(_min<u64>(100, (t - g_me.newchar_start) * 100 / kNewCharHoldMs)));
			line(buf, 0xffffa040, false);
		}
		else
			line(tr("st_coop_hardcore_new_hold", "Hold USE for 3 s to start a new character").c_str(), 0xfff0f0f0, false);
		break;
	}
	default: break;
	}
	if (g_me.revive_target != kNoId && !lua_hud)
	{
		// tr_format lets only the nick's %s through, so the progress appended here is the format's second value
		xr_sprintf(buf, (tr_format("st_coop_hud_reviving", "Reviving %s...", 's') + " %u%%").c_str(),
		    nick_of(join::to_host(g_me.revive_target)).c_str(),
		    u32(_min<u64>(100, (t - g_me.revive_start) * 100 / kReviveHoldMs)));
		line(buf, 0xff80ff80, false);
	}
	while (!g_feed.empty() && g_feed.front().until <= t)
		g_feed.pop_front();
}

bool revive_progress(ReviveProgress& out)
{
	out = ReviveProgress();
	const u64 t = mgr().now_ms();
	g_lua_polled = t ? t : 1; // a Lua HUD is there: the C++ down / revive text steps aside (build_hud)
	if (!live())
		return false;
	if (g_me.revive_target != kNoId && !is_down(g_me.life))
	{
		const u16 target_host = join::to_host(g_me.revive_target);
		const u64 held = t > g_me.revive_start ? t - g_me.revive_start : 0;
		out.reviving = true;
		out.target_local = g_me.revive_target;
		out.target_peer = peer_of_host_id(target_host);
		out.target_nick = nick_of(target_host);
		out.reviving_fraction = revive::fraction(held, kReviveHoldMs);
		out.reviving_left_ms = held < kReviveHoldMs ? u32(kReviveHoldMs - held) : 0;
		out.item_kind = revive_item_kind(g_me.revive_item_sect.c_str());
		out.item_section = g_me.revive_item_sect;
	}
	else if (g_me.sent_target != kNoId && t - g_me.sent_at < kSentShownMs && !is_down(g_me.life))
	{
		// The hold ran its time, the Request is out: the ring stays full until the host's Done (or its refusal) comes
		out.reviving = true;
		out.reviving_sent = true;
		out.target_local = local_of_host_id(g_me.sent_target);
		out.target_peer = peer_of_host_id(g_me.sent_target);
		out.target_nick = nick_of(g_me.sent_target);
		out.reviving_fraction = 1.f;
		out.item_kind = revive_item_kind(g_me.sent_item_sect.c_str());
		out.item_section = g_me.sent_item_sect;
	}
	if (g_me.life == Life::Wounded)
	{
		out.wounded = true;
		out.bleed_left_ms = g_me.bleed_end > t ? u32(g_me.bleed_end - t) : 0;
		out.bleed_total_ms = g_rules.bleedout_ms;
		if (g_me.lift.active(t, g_me.seq))
		{
			out.revived_by = true;
			out.by_peer = g_me.lift.peer() == kInvalidPeer ? peer_of_host_id(g_me.lift.reviver()) : PeerId(g_me.lift.peer());
			out.by_local = local_of_host_id(g_me.lift.reviver());
			out.by_nick = nick_of(g_me.lift.reviver());
			out.by_fraction = g_me.lift.fraction(t);
			out.by_left_ms = g_me.lift.ms_left(t);
		}
		if (g_me.giveup_start)
		{
			const u64 held = t > g_me.giveup_start ? t - g_me.giveup_start : 0;
			out.giving_up = true;
			out.giveup_fraction = revive::fraction(held, kGiveUpHoldMs);
			out.giveup_left_ms = held < kGiveUpHoldMs ? u32(kGiveUpHoldMs - held) : 0;
		}
	}
	return out.reviving || out.wounded;
}

void render_hud()
{
	if (!live() || (g_hud.empty() && g_feed.empty()))
		return;
	CGameFont* big = UI().Font().pFontGraffiti32Russian;
	CGameFont* normal = UI().Font().pFontLetterica18Russian;
	if (!big || !normal)
		return;
	const float cx = float(Device.dwWidth) * 0.5f;
	float y = float(Device.dwHeight) * 0.62f;
	for (const HudLine& l : g_hud)
	{
		CGameFont* f = l.big ? big : normal;
		f->SetAligment(CGameFont::alCenter);
		f->SetColor(l.color);
		f->Out(cx, y, "%s", l.text.c_str());
		y += f->GetHeight() * 1.4f + 4.f;
	}
	float fy = float(Device.dwHeight) * 0.22f;
	for (const FeedLine& l : g_feed)
	{
		normal->SetAligment(CGameFont::alLeft);
		normal->SetColor(0xffe8e8c8);
		normal->Out(float(Device.dwWidth) * 0.02f, fy, "%s", l.text.c_str());
		fy += normal->GetHeight() * 1.3f + 2.f;
	}
	// shared UI fonts: leave them as found (each queued line keeps its own alignment)
	big->SetAligment(CGameFont::alLeft);
	normal->SetAligment(CGameFont::alLeft);
}

} // namespace survival
} // namespace coop
