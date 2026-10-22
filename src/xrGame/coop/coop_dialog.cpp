// Anomaly Coop — talk lock for NPC dialogs. See coop_dialog.h.
#include "stdafx.h"
#include "coop_dialog.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_sync.h"

#include "Level.h"
#include "Actor.h"
#include "InventoryOwner.h"
#include "entity_alive.h"
#include "UIGameSP.h"
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

enum LockResult : u8
{
	Granted = 0,
	Busy = 1, // another player talks to this NPC
	Gone = 2, // no such NPC on the host or it is dead
	Refused = 3, // not a talk partner (player avatar)
};

const char* result_name(u8 r)
{
	switch (r)
	{
	case Granted: return "granted";
	case Busy: return "busy";
	case Gone: return "gone";
	case Refused: return "refused";
	default: return "?";
	}
}

// ---------------------------------------------------------------- host state
struct HostLock
{
	PeerId holder = kInvalidPeer;
	u64 since = 0;
	u64 last_seen = 0;
};
xr_map<u16, HostLock> g_locks; // npc (host id) -> holder
u16 g_host_talk = kNoId; // npc the host's own actor talks to under a lock

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

void host_send_result(PeerId to, u16 npc, u8 result)
{
	Writer w(4);
	w.u16(npc);
	w.u8(result);
	net::host_send(to, Msg::DialogLockResult, w);
}

void host_send_revoke(PeerId to, u16 npc)
{
	Writer w(4);
	w.u16(npc);
	net::host_send(to, Msg::DialogUnlock, w);
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
			it = g_locks.erase(it);
		}
		else
			++it;
	}
}

u8 host_try_lock(u16 npc, PeerId who)
{
	if (world::is_player_proxy(npc))
		return Refused;
	CInventoryOwner* io = find_owner(npc);
	if (!io || !alive(io))
		return Gone;
	auto it = g_locks.find(npc);
	if (it != g_locks.end() && it->second.holder != who)
		return Busy;
	// The host's actor may be talking without a lock (talk started before the session).
	if (who != kHostPeer && actor_talks_to(npc))
		return Busy;
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
	const u8 result = host_try_lock(npc, from);
	if (result == Granted)
		++g_grants;
	else
		++g_denies;
	const PeerInfo* p = mgr().session().peer(from);
	::Msg("[coop] talk lock %u (%s) for <%s>: %s", npc, name_of(npc), p ? p->nick.c_str() : "?", result_name(result));
	host_send_result(from, npc, result);
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
		g_locks.erase(it);
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
	Writer w(4);
	w.u16(g_cl.granted);
	net::client_send(Msg::DialogUnlock, w);
	g_cl.granted = kNoId;
	g_cl.start = kNoId;
}

void notify_denied(u8 result)
{
	if (result == Busy)
		hud_message("st_coop_talk_busy", "Busy: talking to another player");
	else if (result == Refused)
		hud_message("st_coop_talk_refused", "You cannot talk to another player this way");
	else
		::Msg("[coop] talk refused by the host: %s", result_name(result));
}

void client_on_result(PeerId, Reader& r)
{
	if (!mgr().is_client())
		return;
	const u16 npc = r.u16();
	const u8 result = r.u8();
	if (!r.ok())
		return;
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
		notify_denied(result);
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

void client_frame()
{
	const u64 now = now_ms();
	if (g_cl.pending != kNoId && now - g_cl.pending_at > kPendingTimeoutMs)
		g_cl.pending = kNoId;

	if (g_cl.start != kNoId)
	{
		CUITalkWnd* wnd = talk_window();
		if (Device.Paused())
			client_release(); // the player went to the menu meanwhile; he clicks again later
		else if (wnd && wnd->IsShown())
		{
			// The refused window has not hidden yet, or another talk is open.
			if (now - g_cl.start_at > kStartWaitMs)
				client_release();
		}
		else
		{
			const u16 local = join::to_local(g_cl.start);
			const bool brk = g_cl.start_break;
			g_cl.start = kNoId;
			CInventoryOwner* io = find_owner(local);
			CActor* a = Actor();
			if (io && alive(io) && a && a->g_Alive() && !a->IsTalking())
				a->RunTalkDialog(io, brk);
			if (!actor_talks_to(local))
				client_release(); // the talk did not start (NPC no longer willing, actor busy)
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

void print_status()
{
	if (mgr().is_host())
	{
		const u64 now = now_ms();
		::Msg("[coop]   talk locks: %u held; %llu requests, %llu granted, %llu denied, %llu timeouts, %llu dropped on leave, "
		    "%llu host talks refused",
		    (u32)g_locks.size(), g_requests, g_grants, g_denies, g_timeouts, g_released_left, g_host_refused);
		for (const auto& it : g_locks)
		{
			::Msg("[coop]     npc %5u %-28s held by #%u for %llus (seen %llus ago)", it.first, name_of(it.first),
			    it.second.holder, (now - it.second.since) / 1000, (now - it.second.last_seen) / 1000);
		}
	}
	else if (mgr().is_client())
	{
		::Msg("[coop]   talk: lock %u, pending %u, start %u; %llu requests, %llu granted, %llu denied", g_cl.granted,
		    g_cl.pending, g_cl.start, g_requests, g_grants, g_denies);
	}
}
} // namespace dialog
} // namespace coop
