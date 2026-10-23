// Anomaly Coop — talk lock for NPC dialogs. See coop_dialog.h.
#include "stdafx.h"
#include "pch_script.h" // B8: the host asks coop_npc_meet.talk_verdict
#include "coop_dialog.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"
#include "coop_money.h"
#include "coop_requests.h"
#include "xrCoop/coop_talks.h" // Ruling F2-34 (F2 T12 round 2)

#include "Level.h"
#include "Actor.h"
#include "InventoryOwner.h"
#include "entity_alive.h"
#include "UIGameSP.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "script_engine.h"
#include "string_table.h"
#include "ui/UIStatic.h"
#include "ui/UITalkWnd.h"

namespace coop
{
namespace dialog
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u64 kLockTimeoutMs = 120000; // host: client lock without keep-alive
constexpr u64 kKeepAliveMs = 30000; // client: refresh period while talking
constexpr u64 kPendingTimeoutMs = 10000; // client: a request without answer may be repeated after this
constexpr u64 kStartWaitMs = 3000; // client: granted talk waits this long for the refused window to hide
// F2 T1 fix round 1 (Ruling F2-1): a client's character <-> the NPC on the host when it asks for the lock. The stock
// window of an NPC closes beyond 3 m (CUIActorMenu::CheckDistance); twice that for the lag of the character on the host,
// as kTradeReach in coop_trade. A lock is the condition for dialog money (coop_money).
constexpr float kTalkReach = 6.f;

// B8: the results and the reason bytes are in xrCoop/coop_talks.h (shared with the unit tests).
using namespace talks::lock; // Granted, Busy, Gone, Refused, TooFar, NotWilling

// ---------------------------------------------------------------- host state
struct HostLock
{
	PeerId holder = kInvalidPeer;
	u64 since = 0;
	u64 last_seen = 0;
};
xr_map<u16, HostLock> g_locks; // npc (host id) -> holder
u16 g_host_talk = kNoId; // npc the host's own actor talks to under a lock
// Ruling F2-34 (F2 T12 round 2): per player, the NPCs whose lock just ended (talks::Recent keeps the last 2). One entry
// per player in the session; a player's entry goes when he leaves.
xr_map<PeerId, talks::Recent> g_recent;

// ---------------------------------------------------------------- client state
struct ClientState
{
	u16 pending = kNoId; // asked the host, no answer yet
	bool pending_break = false;
	u64 pending_at = 0;
	u16 granted = kNoId; // lock we hold (host id)
	u64 last_keepalive = 0;
	u16 start = kNoId; // granted: start the talk in frame()
	bool start_break = false;
	u64 start_at = 0;
};
ClientState g_cl;

// Talk window refused at open; it hides on its next Update. Not part of the session state.
bool g_refused = false;

u64 g_requests = 0, g_grants = 0, g_denies = 0, g_timeouts = 0, g_released_left = 0, g_host_refused = 0;
u64 g_not_willing = 0, g_verdict_errors = 0, g_forced = 0, g_not_started = 0; // B8

u64 now_ms() { return mgr().now_ms(); }

CInventoryOwner* find_owner(u16 id)
{
	if (!g_pGameLevel || id == kNoId)
		return nullptr;
	CObject* o = Level().Objects.net_Find(id);
	return o ? smart_cast<CInventoryOwner*>(o) : nullptr;
}

bool alive(CInventoryOwner* io)
{
	CEntityAlive* e = io ? smart_cast<CEntityAlive*>(io) : nullptr;
	return e && e->g_Alive();
}

u16 id_of(CInventoryOwner* io)
{
	CGameObject* go = io ? smart_cast<CGameObject*>(io) : nullptr;
	return go ? go->ID() : kNoId;
}

const char* name_of(u16 id)
{
	CObject* o = g_pGameLevel ? Level().Objects.net_Find(id) : nullptr;
	return o ? o->cName().c_str() : "?";
}

// True if the local actor is in a talk with object `local_id`.
bool actor_talks_to(u16 local_id)
{
	CActor* a = Actor();
	return a && a->IsTalking() && id_of(a->GetTalkPartner()) == local_id;
}

CUITalkWnd* talk_window()
{
	CUIGameSP* ui = CurrentGameUI() ? smart_cast<CUIGameSP*>(CurrentGameUI()) : nullptr;
	return ui ? ui->TalkMenu : nullptr;
}

// ---------------------------------------------------------------- host

void host_send_result(PeerId to, u16 npc, u8 result, talks::Reason why = talks::Reason::None)
{
	Writer w(4);
	talks::write_lock_result(w, {npc, result, why}); // NotWilling carries the reason (B8)
	net::host_send(to, Msg::DialogLockResult, w);
}

void host_send_revoke(PeerId to, u16 npc)
{
	Writer w(4);
	w.u16(npc);
	net::host_send(to, Msg::DialogUnlock, w);
}

// Ruling F2-34: the lock of `holder` on `npc` ends now (coop.talk_recent remembers it a while).
void note_ended(PeerId holder, u16 npc)
{
	if (holder != kInvalidPeer)
		g_recent[holder].ended(npc, now_ms());
}

// Releases every lock of `who` except the one on `keep`.
void host_release_peer(PeerId who, u16 keep)
{
	for (auto it = g_locks.begin(); it != g_locks.end();)
	{
		if (it->second.holder == who && it->first != keep)
		{
			if (who == kHostPeer && g_host_talk == it->first)
				g_host_talk = kNoId;
			note_ended(who, it->first); // F2-34
			it = g_locks.erase(it);
		}
		else
			++it;
	}
}

// B8: under lua_pcall, arg 1 the NPC's game object, arg 2 the game object of the player's character (the proxy). Asks
// coop_npc_meet.talk_verdict, which judges with the meet rules of the running game for THAT character: returns true, or
// false and the name of the reason. Without the script: true.
int lua_talk_verdict(lua_State* L)
{
	lua_getglobal(L, "coop_npc_meet"); // _G.__index loads the module if it was not yet
	if (lua_istable(L, -1))
	{
		lua_getfield(L, -1, "talk_verdict");
		if (lua_isfunction(L, -1))
		{
			lua_pushvalue(L, 1);
			lua_pushvalue(L, 2);
			lua_call(L, 2, 2);
			return 2;
		}
	}
	lua_pushboolean(L, 1);
	lua_pushnil(L);
	return 2;
}

// B8: why the host's meet rules refuse the NPC `io` to the character `me` (both host objects); None = allowed. The
// client's own puppet flags are not asked: they are a mirror, and without the answer of the running game's rules (a
// stock rule that G.A.M.M.A. does not apply) they muted NPCs the host could talk to. A script that is missing or fails
// lets the talk through: the checks of this file (alive, distance, one talker) still stand.
talks::Reason talk_verdict(CInventoryOwner* io, CObject* me)
{
	CGameObject* npc = io ? smart_cast<CGameObject*>(io) : nullptr;
	CGameObject* who = me ? smart_cast<CGameObject*>(me) : nullptr;
	CScriptGameObject* lua_npc = npc ? npc->lua_game_object() : nullptr;
	CScriptGameObject* lua_who = who ? who->lua_game_object() : nullptr;
	lua_State* L = (lua_npc && lua_who) ? ::ai().script_engine().lua() : nullptr;
	if (!L || !lua_checkstack(L, 8))
		return talks::Reason::None;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &lua_talk_verdict);
	::luabind::object(L, lua_npc).pushvalue();
	::luabind::object(L, lua_who).pushvalue();
	if (lua_pcall(L, 2, 2, 0) != 0)
	{
		if (g_verdict_errors++ < 5)
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] coop_npc_meet.talk_verdict(%u) failed (the talk is let through): %s", id_of(io), err ? err : "?");
		}
		lua_settop(L, top);
		return talks::Reason::None;
	}
	talks::Reason why = talks::Reason::None;
	if (lua_isboolean(L, -2) && !lua_toboolean(L, -2))
		why = talks::reason_from_name(lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr); // a refusal is never None
	lua_settop(L, top);
	return why;
}

u8 host_try_lock(u16 npc, PeerId who, talks::Reason* why = nullptr)
{
	if (world::is_player_proxy(npc))
		return Refused;
	CInventoryOwner* io = find_owner(npc);
	if (!io || !alive(io))
		return Gone;
	// F2 T1 fix round 1 (Ruling F2-1): a client talks only to an NPC his character stands by on the host (the stock
	// distance exception for NeedOsoznanieMode as in CUIActorMenu::CheckDistance). The host's own talk is local.
	if (who != kHostPeer && !io->NeedOsoznanieMode())
	{
		CObject* me = g_pGameLevel ? Level().Objects.net_Find(join::proxy_of_peer(who)) : nullptr;
		CObject* them = smart_cast<CGameObject*>(io);
		if (!me || !them || me->Position().distance_to(them->Position()) > kTalkReach)
			return TooFar;
	}
	auto it = g_locks.find(npc);
	if (it != g_locks.end() && it->second.holder != who)
		return Busy;
	// The host's actor may be talking without a lock (talk started before the session).
	if (who != kHostPeer && actor_talks_to(npc))
		return Busy;
	// B8: the decision whether this NPC talks to a client's character is the host's, for THAT character (B8, build 10:
	// the flag a puppet carries on the client is only a mirror; it muted NPCs the host could talk to). The host's own
	// talk is decided by the engine (OfferTalk) as always.
	if (who != kHostPeer)
	{
		CObject* me = g_pGameLevel ? Level().Objects.net_Find(join::proxy_of_peer(who)) : nullptr;
		const talks::Reason r = talk_verdict(io, me);
		if (r != talks::Reason::None)
		{
			if (why)
				*why = r;
			return NotWilling;
		}
	}
	host_release_peer(who, npc); // one talk per player
	const u64 now = now_ms();
	HostLock& l = g_locks[npc];
	if (l.holder != who)
		l.since = now;
	l.holder = who;
	l.last_seen = now;
	return Granted;
}

void host_on_lock(PeerId from, Reader& r)
{
	if (!mgr().is_host() || from == kHostPeer)
		return;
	const u16 npc = r.u16();
	const bool keepalive = r.u8() != 0;
	if (!r.ok() || sync::host_peer_joining(from))
		return;
	if (keepalive)
	{
		auto it = g_locks.find(npc);
		if (it != g_locks.end() && it->second.holder == from)
			it->second.last_seen = now_ms();
		else
			host_send_revoke(from, npc); // the client thinks it talks, the host does not know it
		return;
	}
	++g_requests;
	talks::Reason why = talks::Reason::None;
	const u8 result = host_try_lock(npc, from, &why);
	if (result == Granted)
		++g_grants;
	else
		++g_denies;
	if (result == NotWilling)
		++g_not_willing;
	const PeerInfo* p = mgr().session().peer(from);
	const char* tail = result == NotWilling ? talks::reason_name(why) : "";
	::Msg("[coop] talk lock %u (%s) for <%s>: %s%s%s", npc, name_of(npc), p ? p->nick.c_str() : "?",
	    talks::lock::name(result), *tail ? " - " : "", tail);
	host_send_result(from, npc, result, why);
}

void host_on_unlock(PeerId from, Reader& r)
{
	if (!mgr().is_host() || from == kHostPeer)
		return;
	const u16 npc = r.u16();
	if (!r.ok())
		return;
	auto it = g_locks.find(npc);
	if (it != g_locks.end() && it->second.holder == from)
	{
		note_ended(from, npc); // F2-34: the talk window closed; a recruit event may arrive right after
		g_locks.erase(it);
	}
}

void host_frame()
{
	const u64 now = now_ms();
	// The host's own talk ended without the window hook (scripted stop, death, level change).
	if (g_host_talk != kNoId && !actor_talks_to(g_host_talk))
	{
		auto it = g_locks.find(g_host_talk);
		if (it != g_locks.end() && it->second.holder == kHostPeer)
			g_locks.erase(it);
		g_host_talk = kNoId;
	}
	for (auto it = g_locks.begin(); it != g_locks.end();)
	{
		const HostLock& l = it->second;
		bool drop = false;
		if (l.holder != kHostPeer && now - l.last_seen > kLockTimeoutMs)
		{
			::Msg("[coop] talk lock %u (%s) of #%u timed out", it->first, name_of(it->first), l.holder);
			++g_timeouts;
			drop = true;
		}
		else if (l.holder != kHostPeer && !alive(find_owner(it->first)))
		{
			::Msg("[coop] talk lock %u released: the NPC is gone", it->first);
			drop = true;
		}
		if (drop)
		{
			if (l.holder != kHostPeer)
				host_send_revoke(l.holder, it->first);
			note_ended(l.holder, it->first); // F2-34
			it = g_locks.erase(it);
		}
		else
			++it;
	}
}

// ---------------------------------------------------------------- client

void client_send_lock(u16 npc, bool keepalive)
{
	Writer w(4);
	w.u16(npc);
	w.u8(keepalive ? 1 : 0);
	net::client_send(Msg::DialogLock, w);
}

void client_release()
{
	if (g_cl.granted == kNoId)
		return;
	// F2 T1 / T2: the talk's money changes and spawns (a quest item given on the closing click) reach the host
	// while it still sees the lock; the outbox flush includes the money flush
	requests::client_flush_outbox();
	Writer w(4);
	w.u16(g_cl.granted);
	net::client_send(Msg::DialogUnlock, w);
	g_cl.granted = kNoId;
	g_cl.start = kNoId;
}

void notify_denied(u8 result, talks::Reason why = talks::Reason::None)
{
	if (result == NotWilling) // B8: the host's meet rules for our character say no; the line tells which
		hud_message(talks::reason_string_id(why), talks::reason_fallback(why));
	else if (result == Busy)
		hud_message("st_coop_talk_busy", "Busy: talking to another player");
	else if (result == Refused)
		hud_message("st_coop_talk_refused", "You cannot talk to another player this way");
	else if (result == TooFar) // F2 T6b: the host sees the player's character far from the NPC
		hud_message("st_coop_talk_too_far", "You are too far away to talk");
	else
		::Msg("[coop] talk refused by the host: %s", talks::lock::name(result));
}

void client_on_result(PeerId, Reader& r)
{
	if (!mgr().is_client())
		return;
	talks::LockResultMsg msg;
	if (!talks::read_lock_result(r, msg))
		return;
	const u16 npc = msg.npc;
	const u8 result = msg.result;
	if (npc != g_cl.pending)
	{
		// Late or repeated answer. A lock we do not want must not stay on the host.
		if (result == Granted && npc != g_cl.granted)
		{
			Writer w(4);
			w.u16(npc);
			net::client_send(Msg::DialogUnlock, w);
		}
		return;
	}
	g_cl.pending = kNoId;
	if (result != Granted)
	{
		++g_denies;
		notify_denied(result, msg.reason);
		return;
	}
	++g_grants;
	if (g_cl.granted != kNoId && g_cl.granted != npc)
		client_release();
	g_cl.granted = npc;
	g_cl.last_keepalive = now_ms();
	g_cl.start = npc;
	g_cl.start_break = g_cl.pending_break;
	g_cl.start_at = now_ms();
}

void client_on_revoke(PeerId, Reader& r)
{
	if (!mgr().is_client())
		return;
	const u16 npc = r.u16();
	if (!r.ok() || npc != g_cl.granted)
		return;
	g_cl.granted = kNoId;
	g_cl.start = kNoId;
	const u16 local = join::to_local(npc);
	if (actor_talks_to(local))
	{
		Actor()->StopTalk(); // the talk window closes itself on its next update
		hud_message("st_coop_talk_revoked", "The conversation was interrupted by the host");
	}
}

// B8: one line in the log when a talk the host allowed did not start here, with the cause (talks::start_block). Read
// right after the attempt, so it is the state that refused.
void log_not_started(u16 host_npc, u16 local, bool forced)
{
	CInventoryOwner* io = find_owner(local);
	CActor* a = Actor();
	CUITalkWnd* wnd = talk_window();
	talks::StartState st;
	st.npc_found = io != nullptr;
	st.npc_alive = alive(io);
	st.actor_alive = a && a->g_Alive();
	st.paused = Device.Paused();
	st.actor_talking = a && a->IsTalking();
	st.window_shown = wnd && wnd->IsShown();
	st.partner_talking = io && io->IsTalking();
	st.talk_enabled = io && io->IsTalkEnabled();
	++g_not_started;
	::Msg("[coop] talk with %u (%s) was allowed by the host but did not start: %s%s", host_npc, name_of(local),
	    talks::start_block_name(talks::start_block(st)), forced ? " (talk had to be forced on here)" : "");
}

void client_frame()
{
	const u64 now = now_ms();
	if (g_cl.pending != kNoId && now - g_cl.pending_at > kPendingTimeoutMs)
		g_cl.pending = kNoId;

	if (g_cl.start != kNoId)
	{
		CUITalkWnd* wnd = talk_window();
		if (Device.Paused())
		{
			log_not_started(g_cl.start, join::to_local(g_cl.start), false); // B8
			client_release(); // the player went to the menu meanwhile; he clicks again later
		}
		else if (wnd && wnd->IsShown())
		{
			// The refused window has not hidden yet, or another talk is open.
			if (now - g_cl.start_at > kStartWaitMs)
			{
				log_not_started(g_cl.start, join::to_local(g_cl.start), false); // B8
				client_release();
			}
		}
		else
		{
			const u16 host_npc = g_cl.start;
			const u16 local = join::to_local(host_npc);
			const bool brk = g_cl.start_break;
			g_cl.start = kNoId;
			CInventoryOwner* io = find_owner(local);
			CActor* a = Actor();
			bool forced = false;
			if (io && alive(io) && a && a->g_Alive() && !a->IsTalking())
			{
				// B8: the host judged this NPC for our character and allowed the talk. The talk flag of a puppet is
				// only what the host last sent (and a puppet never runs xr_meet); it must not veto the host's answer.
				if (!io->IsTalkEnabled())
				{
					io->EnableTalk();
					forced = true;
					++g_forced;
				}
				a->RunTalkDialog(io, brk);
			}
			if (!actor_talks_to(local))
			{
				log_not_started(host_npc, local, forced); // B8: the NPC died, we are busy, the engine refused
				client_release();
			}
		}
	}

	if (g_cl.granted != kNoId && g_cl.start == kNoId)
	{
		if (!actor_talks_to(join::to_local(g_cl.granted)))
			client_release(); // the talk ended without the window hook
		else if (now - g_cl.last_keepalive >= kKeepAliveMs)
		{
			client_send_lock(g_cl.granted, true);
			g_cl.last_keepalive = now;
		}
	}
}
} // namespace

// ---------------------------------------------------------------- API

void install(Session& s)
{
	s.on(Msg::DialogLock, &host_on_lock);
	s.on(Msg::DialogLockResult, &client_on_result);
	s.on(Msg::DialogUnlock, [](PeerId from, Reader& r)
	{
		if (mgr().is_host())
			host_on_unlock(from, r);
		else
			client_on_revoke(from, r);
	});
}

void frame()
{
	if (mgr().is_host())
		host_frame();
	else if (mgr().is_live_client())
		client_frame();
}

void reset()
{
	g_locks.clear();
	g_recent.clear(); // F2-34
	g_host_talk = kNoId;
	g_cl = ClientState();
}

void on_peer_left(PeerId id)
{
	if (!mgr().is_host())
		return;
	u32 n = 0;
	for (auto it = g_locks.begin(); it != g_locks.end();)
	{
		if (it->second.holder == id)
		{
			::Msg("[coop] talk lock %u (%s) released: #%u left", it->first, name_of(it->first), id);
			it = g_locks.erase(it);
			++n;
		}
		else
			++it;
	}
	g_released_left += n;
	g_recent.erase(id); // F2-34: his recent talks go with him
}

bool may_start_talk(CInventoryOwner* partner, bool disable_break)
{
	// W9: a player's character never talks, in a session or not (its talk is off, there is no "talk" tip): refuse
	// silently, like any NPC with talk disabled. Checked by section: a client learns other avatars' ids late.
	if (partner && world::is_player_character(smart_cast<CGameObject*>(partner)))
		return false;
	if (!partner || !net::live())
		return true;
	const u16 local = id_of(partner);
	if (local == kNoId || local == 0)
		return true;
	if (world::is_player_proxy(local))
		return false; // (a registered avatar is a player character: caught above)
	if (mgr().is_host())
	{
		const u8 result = host_try_lock(local, kHostPeer);
		if (result == Granted)
		{
			g_host_talk = local;
			return true;
		}
		if (result == Busy)
		{
			++g_host_refused;
			notify_denied(Busy);
			return false;
		}
		return true; // the engine decides (OfferTalk checked the NPC already)
	}
	const u16 npc = join::to_host(local);
	if (g_cl.granted == npc)
		return true;
	if (g_cl.granted != kNoId)
		client_release(); // a new talk replaces the old one
	if (g_cl.pending == npc && now_ms() - g_cl.pending_at < kPendingTimeoutMs)
		return false; // already asked, the answer starts the talk
	g_cl.pending = npc;
	g_cl.pending_break = disable_break;
	g_cl.pending_at = now_ms();
	++g_requests;
	client_send_lock(npc, false);
	return false;
}

bool on_talk_window_open(CInventoryOwner* partner, bool disable_break)
{
	g_refused = false;
	if (may_start_talk(partner, disable_break))
		return true;
	g_refused = true;
	// Undo what CActor::RunTalkDialog started: both sides stop talking, nothing was initialized.
	if (CActor* a = Actor())
		a->StopTalk();
	if (partner && partner->IsTalking())
		partner->StopTalk();
	return false;
}

bool talk_window_refused() { return g_refused; }

void on_talk_window_closed()
{
	if (g_refused)
	{
		g_refused = false; // a refused window never held a lock
		return;
	}
	if (!net::live())
		return;
	if (mgr().is_host())
	{
		if (g_host_talk != kNoId)
		{
			auto it = g_locks.find(g_host_talk);
			if (it != g_locks.end() && it->second.holder == kHostPeer)
				g_locks.erase(it);
			g_host_talk = kNoId;
		}
	}
	else if (g_cl.start == kNoId)
		client_release();
}

void hud_message(const char* string_id, const char* fallback)
{
	::Msg("[coop] %s", fallback);
	if (!g_pGameLevel || !CurrentGameUI())
		return;
	const shared_str tr = CStringTable().translate(string_id);
	const char* text = (tr.size() && xr_strcmp(tr.c_str(), string_id) != 0) ? tr.c_str() : fallback;
	StaticDrawableWrapper* s = CurrentGameUI()->AddCustomStatic("item_used", true);
	if (s && s->wnd())
		s->wnd()->TextItemControl()->SetText(text);
}

PeerId host_talk_holder(u16 npc) // W11
{
	if (!mgr().is_host())
		return kInvalidPeer;
	auto it = g_locks.find(npc);
	return it == g_locks.end() ? kInvalidPeer : it->second.holder;
}

u16 client_partner() { return mgr().is_client() ? g_cl.granted : kNoId; } // F2 T1

bool host_talked_recently(PeerId peer, u16 npc, u32 window_ms) // Ruling F2-34
{
	if (!mgr().is_host() || peer == kInvalidPeer)
		return false;
	const auto it = g_recent.find(peer);
	static const talks::Recent kNone;
	return talks::talked_recently(host_lock_of(peer), it == g_recent.end() ? kNone : it->second, npc, now_ms(), window_ms);
}

u16 host_lock_of(PeerId peer) // F2 T1
{
	if (!mgr().is_host())
		return kNoId;
	for (const auto& it : g_locks)
		if (it.second.holder == peer)
			return it.first; // a player holds at most one lock (host_try_lock)
	return kNoId;
}

void print_status()
{
	if (mgr().is_host())
	{
		const u64 now = now_ms();
		::Msg("[coop]   talk locks: %u held; %llu requests, %llu granted, %llu denied (%llu the NPC would not talk), "
		    "%llu timeouts, %llu dropped on leave, %llu host talks refused, %llu rule errors",
		    (u32)g_locks.size(), g_requests, g_grants, g_denies, g_not_willing, g_timeouts, g_released_left, g_host_refused,
		    g_verdict_errors);
		for (const auto& it : g_locks)
		{
			::Msg("[coop]     npc %5u %-28s held by #%u for %llus (seen %llus ago)", it.first, name_of(it.first),
			    it.second.holder, (now - it.second.since) / 1000, (now - it.second.last_seen) / 1000);
		}
	}
	else if (mgr().is_client())
	{
		::Msg("[coop]   talk: lock %u, pending %u, start %u; %llu requests, %llu granted, %llu denied; %llu started with talk forced "
		    "on, %llu allowed but not started", g_cl.granted, g_cl.pending, g_cl.start, g_requests, g_grants, g_denies, g_forced,
		    g_not_started);
	}
}
} // namespace dialog
} // namespace coop
