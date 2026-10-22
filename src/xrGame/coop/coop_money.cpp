// Anomaly Coop — player money lives on the host (F2 T1, review R4 7). See coop_money.h and xrCoop/coop_ledger.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_money.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_sync.h"
#include "coop_entities.h"
#include "coop_events.h"
#include "coop_dialog.h"
#include "coop_world.h"
#include "xrCoop/coop_ledger.h"

#include <cmath>
#include <vector>

#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "InventoryOwner.h"
#include "xrServer_Objects_ALife.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"

namespace coop
{
namespace money
{
namespace
{
using ledger::Cause;
using ledger::Verdict;

constexpr u16 kNoId = 0xffff;
constexpr u32 kVerdictCount = u32(Verdict::Malformed) + 1;
constexpr int kMaxSetting = 2000000000; // console and ltx values are ints
constexpr LPCSTR kSection = "coop_money";

// ---------------------------------------------------------------- settings (Ruling 21, Ruling F2-1)
int g_gain_burst = int(ledger::Policy().script_gain_burst);
int g_gain_per_min = int(ledger::Policy().script_gain_per_min);
int g_dialog_cap = int(ledger::Policy().dialog_gain_cap);
int g_dialog_per_min = int(ledger::Policy().dialog_gain_per_min);
int g_max_money = int(ledger::Policy().max_money);
// F2 T5 fix round 1 (review 2): 1.7 (the lowest stock mech_discount) x 1.0 (the economy slider `upgrade` of
// ui_options.script starts at 1.0; plugins\difficulty.ltx 1.3 is only a preset's value).
float g_upgrade_min_percent = 1.7f;
bool g_scripted_upgrades = false; // F2 T5: upgrades that name no mechanic (a script's) are refused

ledger::Policy policy()
{
	ledger::Policy p;
	p.script_gain_burst = u32(_max(g_gain_burst, 0));
	p.script_gain_per_min = u32(_max(g_gain_per_min, 0));
	p.dialog_gain_cap = u32(_max(g_dialog_cap, 0));
	p.dialog_gain_per_min = u32(_max(g_dialog_per_min, 0));
	p.max_money = u32(_max(g_max_money, 1));
	return p;
}

int read_setting(CInifile& ini, LPCSTR key, int fallback, int lo)
{
	if (!ini.line_exist(kSection, key))
		return fallback;
	const int v = ini.r_s32(kSection, key);
	return v < lo ? lo : (v > kMaxSetting ? kMaxSetting : v);
}

// configs\coop_money.ltx, once per process: the console may change the values later (host settings).
void load_settings()
{
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;
	string_path path;
	FS.update_path(path, "$game_config$", "coop_money.ltx");
	if (!FS.exist(path))
	{
		::Msg("! [coop] configs\\coop_money.ltx not found: money settings are the defaults");
		return;
	}
	CInifile ini(path, TRUE, TRUE, FALSE);
	if (!ini.section_exist(kSection))
	{
		::Msg("! [coop] configs\\coop_money.ltx has no [%s]: money settings are the defaults", kSection);
		return;
	}
	g_gain_burst = read_setting(ini, "script_gain_burst", g_gain_burst, 0);
	g_gain_per_min = read_setting(ini, "script_gain_per_min", g_gain_per_min, 0);
	g_dialog_cap = read_setting(ini, "dialog_gain_cap", g_dialog_cap, 0);
	g_dialog_per_min = read_setting(ini, "dialog_gain_per_min", g_dialog_per_min, 0);
	g_max_money = read_setting(ini, "max_money", g_max_money, 1);
	if (ini.line_exist(kSection, "upgrade_min_percent"))
	{
		const float f = ini.r_float(kSection, "upgrade_min_percent");
		if (_valid(f) && f >= 0.f && f <= 100.f)
			g_upgrade_min_percent = f;
	}
	if (ini.line_exist(kSection, "scripted_upgrades"))
		g_scripted_upgrades = !!ini.r_bool(kSection, "scripted_upgrades"); // F2 T5
}

void register_console()
{
	static bool registered = false;
	if (registered || !Console)
		return;
	registered = true;
	CMD4(CCC_Integer, "coop_money_gain_burst", &g_gain_burst, 0, kMaxSetting);
	CMD4(CCC_Integer, "coop_money_gain_per_min", &g_gain_per_min, 0, kMaxSetting);
	CMD4(CCC_Integer, "coop_money_dialog_cap", &g_dialog_cap, 0, kMaxSetting);
	CMD4(CCC_Integer, "coop_money_dialog_per_min", &g_dialog_per_min, 0, kMaxSetting);
	CMD4(CCC_Integer, "coop_money_max", &g_max_money, 1, kMaxSetting);
}

u64 g_log_count = 0;
bool log_allowed() { return ++g_log_count <= 20 || (g_log_count % 100) == 0; }

const char* verdict_name(Verdict v)
{
	switch (v)
	{
	case Verdict::Applied: return "applied";
	case Verdict::Insufficient: return "not enough money";
	case Verdict::NoAllowance: return "over the allowance";
	case Verdict::OverMax: return "over the maximum";
	case Verdict::Malformed: return "malformed";
	}
	return "?";
}

const char* cause_name(Cause c)
{
	switch (c)
	{
	case Cause::Script: return "script";
	case Cause::Dialog: return "dialog";
	case Cause::Transfer: return "transfer";
	}
	return "?";
}

const char* nick_of(PeerId id)
{
	if (id == kHostPeer)
		return "host";
	const PeerInfo* p = mgr().session().peer(id);
	return p ? p->nick.c_str() : "?";
}

// ---------------------------------------------------------------- host state
// Per connection the newest answered change, per character (its player key) the allowances for the whole session:
// leaving and joining again refills nothing (fix round 1); they refill from the moment the character is live
// (host_on_peer_in_world, Ruling F2-20). Rules: xrCoop/coop_ledger.h HostBook.
ledger::HostBook g_book;

struct HostCounters
{
	u64 changes = 0, malformed = 0, no_character = 0, stale = 0, dropped_joining = 0, bad_receiver = 0;
	u64 verdicts[kVerdictCount] = {};
	u64 gain_script = 0, gain_dialog = 0; // RU
	u64 grants = 0, trades = 0, transfers = 0, capped = 0, joins = 0;
} g_h;

// The character that holds a player's money: the host's own actor, else his proxy (kNoId if none).
u16 holder_of(PeerId peer) { return peer == kHostPeer ? 0 : join::proxy_of_peer(peer); }

CInventoryOwner* owner_object(u16 id)
{
	if (id == kNoId || !g_pGameLevel)
		return nullptr;
	CGameObject* go = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return go && !go->getDestroy() ? smart_cast<CInventoryOwner*>(go) : nullptr;
}

CSE_ALifeTraderAbstract* owner_entity(u16 id)
{
	CSE_Abstract* e = id == kNoId ? nullptr : events::find_entity(id);
	return e ? smart_cast<CSE_ALifeTraderAbstract*>(e) : nullptr;
}

// Online: the game object's money; parked or loading (no game object): its server entity's.
bool read_balance(u16 holder, u32& out)
{
	if (CInventoryOwner* io = owner_object(holder))
	{
		out = io->get_money();
		return true;
	}
	if (CSE_ALifeTraderAbstract* t = owner_entity(holder))
	{
		out = t->m_dwMoney;
		return true;
	}
	return false;
}

void write_balance(u16 holder, u32 value)
{
	if (CInventoryOwner* io = owner_object(holder))
		io->set_money(value, true); // GE_MONEY: the server entity keeps it, so does the save (P4)
	else if (CSE_ALifeTraderAbstract* t = owner_entity(holder))
		t->m_dwMoney = value;
}

// The player key a character is bound to (ledger::character_key of its custom data), empty if none.
std::string custom_data_key(const CSE_Abstract* e)
{
	return e && e->m_ini_string.size() ? ledger::character_key(e->m_ini_string.c_str()) : std::string();
}

// The key the money book knows a character by (Ruling F2-13). A character without a key line (none of coop_join's)
// gets one of its id that no save snapshot holds: it counts as made in this session.
std::string character_key_of(u16 holder)
{
	std::string key = custom_data_key(events::find_entity(holder));
	if (key.empty())
	{
		char id[16];
		xr_sprintf(id, "#%u", u32(holder));
		key = id;
	}
	return key;
}

void send_balance(PeerId to, u32 balance)
{
	if (to == kHostPeer)
		return;
	const ledger::MoneyBalanceMsg m{balance, g_book.acked(to)};
	Writer w(ledger::kMoneyBalanceSize);
	ledger::write(w, m);
	net::host_send(to, Msg::MoneyBalance, w); // queued while he loads the world
}

enum class Source : u8
{
	Grant, // host_adjust / host_set from the host (Lua, the engine)
	Trade, // host_adjust(..., "trade") from coop_trade
	Transfer, // the receiver's side of a transfer (counted with the transfer)
};

// Host: changes a balance by `delta` within [0, max(max_money, balance)] and tells its player.
bool adjust(PeerId peer, s64 delta, const char* why, Source source)
{
	if (!mgr().is_host())
		return false;
	const u16 holder = holder_of(peer);
	u32 balance = 0;
	if (holder == kNoId || !read_balance(holder, balance))
		return false;
	const s64 hi = _max(s64(policy().max_money), s64(balance));
	const s64 want = s64(balance) + delta;
	const u32 next = u32(want < 0 ? 0 : (want > hi ? hi : want));
	if (source == Source::Grant)
		++g_h.grants;
	else if (source == Source::Trade)
		++g_h.trades;
	if (s64(next) != want)
	{
		++g_h.capped;
		if (log_allowed())
			::Msg("! [coop] money of <%s>: %u RU %+lld (%s) capped at %u", nick_of(peer), balance, delta, why, next);
	}
	if (next != balance)
		write_balance(holder, next);
	send_balance(peer, next);
	return true;
}

// The world's balances by peer for ledger::settle_transfer: set writes a giver's balance, add credits a receiver (and
// sends him his balance).
struct WorldBalances
{
	void set(uint32_t peer, uint32_t value)
	{
		const u16 holder = holder_of(PeerId(peer));
		if (holder != kNoId)
			write_balance(holder, value);
	}
	void add(uint32_t peer, uint32_t credit) { adjust(PeerId(peer), s64(credit), "transfer", Source::Transfer); }
};

// A transfer's receiver as the ledger wants it: another player of the session (joined, or the host) who has a
// character, with his balance.
ledger::Receiver receiver_of(PeerId to)
{
	ledger::Receiver r;
	r.peer = to;
	if (to != kHostPeer)
	{
		const PeerInfo* p = mgr().session().peer(to);
		if (!p || p->state != PeerState::Joined)
			return r;
	}
	const u16 holder = holder_of(to);
	r.present = holder != kNoId && read_balance(holder, r.balance);
	return r;
}

void host_on_req(PeerId from, Reader& r)
{
	if (!mgr().is_host() || from == kHostPeer)
		return;
	// A change made in a world that was replaced: the client dropped it at SessionReset, the save holds the money.
	if (sync::host_peer_joining(from) || Device.dwPrecacheFrame)
	{
		++g_h.dropped_joining;
		return;
	}
	ledger::MoneyReq q;
	if (!ledger::read(r, q))
	{
		++g_h.malformed;
		if (log_allowed())
			::Msg("! [coop] malformed ReqMoney from #%u", from);
		return;
	}
	++g_h.changes;
	const u16 holder = holder_of(from);
	u32 balance = 0;
	if (holder == kNoId || !read_balance(holder, balance))
	{
		++g_h.no_character; // not in this world: no balance to answer with
		return;
	}
	ledger::Receiver to;
	if (q.cause == Cause::Transfer)
		to = receiver_of(PeerId(q.ref));
	// The talk lock is only the condition "a dialog is going on": the dialog allowance is the character's (F2-1).
	const bool talking = dialog::host_lock_of(from) != kNoId;
	const ledger::Decision d =
	    g_book.on_request(from, character_key_of(holder), q, balance, talking, to, policy(), mgr().now_ms());
	if (d.stale)
	{
		++g_h.stale; // a repeat or an old change: the answer says where we are
		send_balance(from, balance);
		return;
	}
	++g_h.verdicts[u32(d.verdict)];
	if (d.verdict == Verdict::Applied)
	{
		g_h.gain_script += d.gain_script;
		g_h.gain_dialog += d.gain_dialog;
		if (d.credit)
		{
			WorldBalances world;
			ledger::settle_transfer(world, from, d.balance, q.ref, d.credit); // giver first (review round 2)
			++g_h.transfers;
		}
		else if (d.balance != balance)
			write_balance(holder, d.balance);
	}
	else if (q.cause == Cause::Transfer && (!to.present || to.peer == from))
		++g_h.bad_receiver;
	if (d.verdict != Verdict::Applied && log_allowed())
		::Msg("[coop] money change #%u of <%s> refused: %+d RU (%s), %s; balance %u RU", q.seq, nick_of(from), q.delta,
		    cause_name(q.cause), verdict_name(d.verdict), d.balance);
	u32 now_holds = d.balance; // what the character holds after it all (the same unless a transfer went to himself)
	read_balance(holder, now_holds);
	send_balance(from, now_holds);
}

// ---------------------------------------------------------------- client state
struct ClientState
{
	bool anchored = false; // `last` follows the actor's money
	u32 last = 0; // the actor's money as we last saw or set it
	u32 host_balance = 0, acked = 0; // the newest MoneyBalance
	std::vector<ledger::Pending> pending; // changes sent (or queued) that the host has not answered
	xr_vector<ledger::MoneyReq> outbox; // queued for frame()
	ledger::UpgradeHolds holds; // F2 T5: payments requests carry, until their answers (client_hold)
};
// GE_MONEY may come from the game thread; the answers come with the network pump on the main thread.
xrCriticalSection g_cl_lock;
ClientState g_cl;
u32 g_cl_seq = 0; // process lifetime: an answer from an older world never covers a newer change
u32 g_hold_seq = 0; // F2 T5, process lifetime: a request of an older world never releases a newer hold
ClientSpendScope* g_scope = nullptr; // the innermost alive ClientSpendScope (main thread)

struct ClientCounters
{
	u64 sent = 0, balances = 0, set_by_host = 0, scoped = 0, gives = 0, malformed = 0;
	u64 holds = 0, unclaimed = 0; // F2 T5
} g_c;

// Under g_cl_lock.
void queue_change(s64 delta, Cause cause, PeerId ref)
{
	const s64 lim = s64(INT_MAX);
	const s32 d = s32(delta > lim ? lim : (delta < -lim ? -lim : delta)); // out of range: refused, then corrected
	const ledger::MoneyReq q{++g_cl_seq, d, cause, ref};
	g_cl.outbox.push_back(q);
	g_cl.pending.push_back({q.seq, q.delta});
}

void client_on_balance(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().is_client())
		return;
	ledger::MoneyBalanceMsg m;
	if (!ledger::read(r, m))
	{
		++g_c.malformed;
		return;
	}
	// Live first: Actor() asserts on GameID(), which has no level in the main menu. Protocol 8 makes a new player's
	// character before his world, so the host's money_set reaches him there (night run 2, 2026-10-23: client crash);
	// host_on_peer_in_world sends the balance again once he is live.
	if (!mgr().is_live_client())
		return;
	CActor* a = Actor();
	if (!a)
		return;
	u32 view = 0;
	{
		xrCriticalSectionGuard guard(g_cl_lock);
		ledger::forget_acked(g_cl.pending, m.acked_seq);
		g_cl.host_balance = m.balance;
		g_cl.acked = m.acked_seq;
		// F2 T5: an upgrade paid here whose request is not answered yet stays out of it (T1 concern 3)
		view = ledger::client_view(m.balance, m.acked_seq, g_cl.pending, policy().max_money, g_cl.holds.sum());
	}
	++g_c.balances;
	const u32 shown = a->get_money();
	if (shown != view)
	{
		++g_c.set_by_host;
		if (log_allowed())
			::Msg("[coop] money from the host: %u RU (shown %u RU, the host holds %u RU)", view, shown, m.balance);
		entities::ClientApply apply; // our own GE_MONEY below is not a change of ours
		a->set_money(view, true);
	}
	xrCriticalSectionGuard guard(g_cl_lock);
	g_cl.anchored = true;
	g_cl.last = a->get_money();
}

// ---------------------------------------------------------------- Lua
// A whole number from argument `idx` within [lo, hi]: fractions are cut toward zero as give_money(int) does; NaN and
// infinities are refused (P8).
bool arg_int(lua_State* L, int idx, double lo, double hi, s64& out)
{
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	const double v = double(lua_tonumber(L, idx));
	if (!std::isfinite(v))
		return false;
	const double whole = std::trunc(v);
	if (whole < lo || whole > hi)
		return false;
	out = s64(whole);
	return true;
}

bool arg_peer(lua_State* L, int idx, PeerId& out)
{
	s64 v = 0;
	if (!arg_int(L, idx, 0.0, double(kInvalidPeer - 1), v))
		return false;
	out = PeerId(v);
	return true;
}

int refuse(lua_State* L, const char* why)
{
	lua_pushboolean(L, 0);
	lua_pushstring(L, why);
	return 2;
}

int done(lua_State* L)
{
	lua_pushboolean(L, 1);
	return 1;
}

int host_give(lua_State* L, PeerId to, u32 amount)
{
	if (to == kHostPeer)
		return refuse(L, "yourself");
	u32 balance = 0;
	if (!read_balance(holder_of(kHostPeer), balance))
		return refuse(L, "no character");
	u32 credit = 0;
	const Verdict v = ledger::check_transfer(kHostPeer, receiver_of(to), -s64(amount), balance, credit, policy());
	if (v != Verdict::Applied)
		return refuse(L, v == Verdict::Malformed ? "no such player" : verdict_name(v));
	WorldBalances world;
	ledger::settle_transfer(world, kHostPeer, balance, to, credit); // giver first (review round 2)
	++g_h.transfers;
	return done(L);
}

int client_give(lua_State* L, PeerId to, u32 amount)
{
	if (!mgr().is_live_client())
		return refuse(L, "not in the world");
	if (to == mgr().self())
		return refuse(L, "yourself");
	if (!mgr().session().peer(to))
		return refuse(L, "no such player");
	CActor* a = Actor();
	if (!a)
		return refuse(L, "no character");
	const u32 have = a->get_money();
	if (have < amount)
		return refuse(L, verdict_name(Verdict::Insufficient));
	{
		xrCriticalSectionGuard guard(g_cl_lock);
		queue_change(-s64(amount), Cause::Transfer, to); // the host credits the receiver or gives it back
	}
	{
		entities::ClientApply apply; // the transfer above is the change
		a->set_money(have - amount, true);
	}
	xrCriticalSectionGuard guard(g_cl_lock);
	g_cl.anchored = true;
	g_cl.last = a->get_money();
	++g_c.gives;
	return done(L);
}

// coop.money_give(peer, amount) -> ok, why: our money to another player; any machine.
int l_money_give(lua_State* L)
{
	if (!mgr().active())
		return refuse(L, "no session");
	PeerId to = kInvalidPeer;
	s64 amount = 0;
	if (!arg_peer(L, 1, to) || !arg_int(L, 2, 1.0, double(max_money()), amount))
		return refuse(L, "bad arguments");
	return mgr().is_host() ? host_give(L, to, u32(amount)) : client_give(L, to, u32(amount));
}

// coop.money_grant(peer, amount) -> ok, why: host; a negative amount takes money (never below 0).
int l_money_grant(lua_State* L)
{
	if (!mgr().is_host())
		return refuse(L, "host only");
	PeerId peer = kInvalidPeer;
	s64 amount = 0;
	const double cap = double(max_money());
	if (!arg_peer(L, 1, peer) || !arg_int(L, 2, -cap, cap, amount) || amount == 0)
		return refuse(L, "bad arguments");
	return host_adjust(peer, amount, "grant") ? done(L) : refuse(L, "no character");
}

// coop.money_set(peer, amount) -> ok, why: host.
int l_money_set(lua_State* L)
{
	if (!mgr().is_host())
		return refuse(L, "host only");
	PeerId peer = kInvalidPeer;
	s64 amount = 0;
	if (!arg_peer(L, 1, peer) || !arg_int(L, 2, 0.0, double(max_money()), amount))
		return refuse(L, "bad arguments");
	return host_set(peer, u32(amount), "set") ? done(L) : refuse(L, "no character");
}

// coop.money_of(peer) -> RU | nil: host.
int l_money_of(lua_State* L)
{
	PeerId peer = kInvalidPeer;
	u32 balance = 0;
	const u16 holder = mgr().is_host() && arg_peer(L, 1, peer) ? holder_of(peer) : kNoId;
	if (holder != kNoId && read_balance(holder, balance))
		lua_pushnumber(L, lua_Number(balance));
	else
		lua_pushnil(L);
	return 1;
}

// coop.talk_lock_of(peer) -> npc id (host numbering) | nil: host.
int l_talk_lock_of(lua_State* L)
{
	PeerId peer = kInvalidPeer;
	const u16 npc = mgr().is_host() && arg_peer(L, 1, peer) ? dialog::host_lock_of(peer) : kNoId;
	if (npc != kNoId)
		lua_pushnumber(L, lua_Number(npc));
	else
		lua_pushnil(L);
	return 1;
}
} // namespace

// ---------------------------------------------------------------- API

void install(Session& s)
{
	load_settings();
	register_console();
	s.on(Msg::ReqMoney, &host_on_req);
	s.on(Msg::MoneyBalance, &client_on_balance);
}

void reset()
{
	g_book.reset(); // a new session (or none): nothing is kept
	xrCriticalSectionGuard guard(g_cl_lock);
	g_cl = ClientState(); // g_cl_seq goes on: see its comment
}

void on_session_started()
{
	reset();
	if (!mgr().is_host() || !ai().get_alife())
		return;
	// Ruling F2-10: the player characters in the world now are "from the save" (returning players keep full
	// allowances). One the host makes later in this session for a player key it has not seen starts empty. Known by
	// the key they are bound to their players by, not by id (F2-13).
	std::vector<std::string> from_save;
	for (const auto& it : ai().alife().objects().objects())
		if (it.second && world::is_player_character(it.second))
		{
			std::string key = custom_data_key(it.second);
			if (!key.empty())
				from_save.push_back(std::move(key));
		}
	g_book.start_session(from_save);
	::Msg("[coop] money: %u player character(s) from the save keep their allowances; new ones start empty",
	    (u32)from_save.size());
}

void frame()
{
	if (mgr().is_live_client())
		client_flush();
}

void on_peer_left(PeerId id) { g_book.peer_left(id); } // the character keeps its allowances

void host_on_peer_in_world(PeerId id)
{
	// Fix round 1: the client drops a MoneyBalance until it is live, and the save it loaded may be older than its
	// balance (a grant while it loaded). One answer now anchors it: its pending list is empty at this point.
	const u16 holder = mgr().is_host() && id != kHostPeer ? holder_of(id) : kNoId;
	u32 balance = 0;
	if (holder == kNoId || !read_balance(holder, balance))
		return;
	// Ruling F2-20: the character is live. Its allowances refill from now (a new one starts empty, a returning one
	// keeps what it had), not from its first income; the time away pays nothing. money_set / host_set never touch them.
	g_book.peer_online(id, character_key_of(holder), mgr().now_ms());
	++g_h.joins;
	send_balance(id, balance);
}

void on_client_live()
{
	CActor* a = Actor();
	xrCriticalSectionGuard guard(g_cl_lock);
	g_cl = ClientState();
	g_cl.anchored = a != nullptr;
	g_cl.last = a ? a->get_money() : 0;
}

void print_status()
{
	const ledger::Policy p = policy();
	if (mgr().is_host())
	{
		::Msg("[coop]   money: changes in %llu, refused insufficient/allowance/max %llu/%llu/%llu (%llu malformed, %llu "
		      "to nobody, %llu stale, %llu without a character, %llu dropped while rejoining), gains script %llu dialog "
		      "%llu RU, grants %llu, trades %llu (%llu capped), transfers %llu, %llu balances sent on joining",
		    g_h.changes, g_h.verdicts[u32(Verdict::Insufficient)], g_h.verdicts[u32(Verdict::NoAllowance)],
		    g_h.verdicts[u32(Verdict::OverMax)], g_h.verdicts[u32(Verdict::Malformed)] + g_h.malformed,
		    g_h.bad_receiver, g_h.stale, g_h.no_character, g_h.dropped_joining, g_h.gain_script, g_h.gain_dialog,
		    g_h.grants, g_h.trades, g_h.capped, g_h.transfers, g_h.joins);
		::Msg("[coop]   money settings: gains %u in a row + %u per minute, dialogs %u in a row + %u per minute, max %u, "
		      "upgrades from %.2f x cost, upgrades without a mechanic %s",
		    p.script_gain_burst, p.script_gain_per_min, p.dialog_gain_cap, p.dialog_gain_per_min, p.max_money,
		    g_upgrade_min_percent, g_scripted_upgrades ? "taken" : "refused");
		for (const auto& it : g_book.connections())
		{
			const u16 holder = holder_of(it.first);
			const std::string key = holder != kNoId ? character_key_of(holder) : std::string();
			const ledger::CharacterAllowances* a = key.empty() ? nullptr : g_book.allowances(key);
			const bool saved = !key.empty() && g_book.from_save(key);
			// Not used yet: full for a character from the save, empty for a new one (F2-10).
			const auto shown = [saved](const ledger::Allowance* x, u32 burst)
			{ return x && x->started ? x->tokens : (saved ? double(burst) : 0.0); };
			::Msg("[coop]     #%u <%s>: %u RU, answered #%u, character %u (%s), allowances script %.0f / dialog %.0f RU",
			    it.first, nick_of(it.first), host_balance(it.first), it.second, holder, saved ? "from the save" : "new",
			    shown(a ? &a->script : nullptr, p.script_gain_burst), shown(a ? &a->dialog : nullptr, p.dialog_gain_cap));
		}
		return;
	}
	if (!mgr().is_client())
		return;
	xrCriticalSectionGuard guard(g_cl_lock);
	::Msg("[coop]   money: %llu changes sent, %u waiting (last #%u); host balance %u RU (answered #%u), %llu balances in, "
	      "%llu set by the host; %llu spends in scopes, %llu gives; upgrades paid here %llu (%u unanswered, %llu RU), "
	      "%llu scope spends sent as changes",
	    g_c.sent, (u32)g_cl.pending.size(), g_cl_seq, g_cl.host_balance, g_cl.acked, g_c.balances, g_c.set_by_host,
	    g_c.scoped, g_c.gives, g_c.holds, (u32)g_cl.holds.size(), g_cl.holds.sum(), g_c.unclaimed);
}

u32 max_money() { return policy().max_money; }

float upgrade_min_percent() { return g_upgrade_min_percent; }

bool scripted_upgrades() { return g_scripted_upgrades; }

u32 host_balance(PeerId peer)
{
	u32 balance = 0;
	const u16 holder = mgr().is_host() ? holder_of(peer) : kNoId;
	return holder != kNoId && read_balance(holder, balance) ? balance : 0;
}

bool host_adjust(PeerId peer, s64 delta, const char* why)
{
	// Fix round 1: trades are counted apart from grants (coop_trade passes "trade").
	const bool trade = why && !xr_strcmp(why, "trade");
	return adjust(peer, delta, why, trade ? Source::Trade : Source::Grant);
}

bool host_set(PeerId peer, u32 amount, const char* why)
{
	if (!mgr().is_host())
		return false;
	const u16 holder = holder_of(peer);
	u32 balance = 0;
	if (holder == kNoId || !read_balance(holder, balance))
		return false;
	const u32 next = _min(amount, policy().max_money);
	++g_h.grants;
	if (next != amount)
	{
		++g_h.capped;
		if (log_allowed())
			::Msg("! [coop] money of <%s> set to %u RU (%s), capped at %u", nick_of(peer), amount, why, next);
	}
	if (next != balance)
		write_balance(holder, next);
	send_balance(peer, next);
	return true;
}

void host_send_balance(PeerId peer)
{
	u32 balance = 0;
	const u16 holder = mgr().is_host() ? holder_of(peer) : kNoId;
	if (holder != kNoId && read_balance(holder, balance))
		send_balance(peer, balance);
}

void client_on_actor_money(u32 money)
{
	if (!mgr().is_live_client() || entities::client_applying())
		return; // not in the world, or the host's balance is being applied
	xrCriticalSectionGuard guard(g_cl_lock);
	if (!g_cl.anchored)
	{
		g_cl.anchored = true;
		g_cl.last = money;
		return;
	}
	const s64 delta = s64(money) - s64(g_cl.last);
	g_cl.last = money;
	if (delta == 0)
		return;
	if (g_scope && delta < 0)
	{
		const u64 sum = u64(g_scope->m_spent) + u64(-delta);
		g_scope->m_spent = sum > u64(u32(-1)) ? u32(-1) : u32(sum); // its request carries the sum (F2 T5)
		++g_c.scoped;
		return;
	}
	queue_change(delta, dialog::client_partner() != kNoId ? Cause::Dialog : Cause::Script, 0);
}

void client_flush()
{
	if (!mgr().is_live_client())
		return;
	xr_vector<ledger::MoneyReq> out;
	{
		xrCriticalSectionGuard guard(g_cl_lock);
		out.swap(g_cl.outbox);
	}
	for (const ledger::MoneyReq& q : out)
	{
		Writer w(ledger::kMoneyReqSize);
		ledger::write(w, q);
		net::client_send(Msg::ReqMoney, w);
		++g_c.sent;
	}
}

ClientSpendScope::ClientSpendScope() : m_outer(g_scope) { g_scope = this; }

ClientSpendScope::~ClientSpendScope() { g_scope = m_outer; }

u32 client_hold(u32 amount)
{
	if (!amount || !mgr().is_live_client())
		return 0;
	xrCriticalSectionGuard guard(g_cl_lock);
	if (++g_hold_seq == 0)
		++g_hold_seq; // 0 is "nothing held"
	g_cl.holds.hold(g_hold_seq, amount);
	++g_c.holds;
	return g_hold_seq;
}

void client_release(u32 key)
{
	if (!key)
		return;
	xrCriticalSectionGuard guard(g_cl_lock);
	g_cl.holds.release(key); // the money shown stays until the balance that follows the answer
}

void client_spend(u32 amount)
{
	if (!amount || !mgr().is_live_client() || entities::client_applying())
		return;
	xrCriticalSectionGuard guard(g_cl_lock);
	if (!g_cl.anchored)
		return; // the next balance from the host is the truth anyway
	++g_c.unclaimed;
	queue_change(-s64(amount), dialog::client_partner() != kNoId ? Cause::Dialog : Cause::Script, 0);
}

void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"money_give", &l_money_give},
		{"money_grant", &l_money_grant},
		{"money_set", &l_money_set},
		{"money_of", &l_money_of},
		{"talk_lock_of", &l_talk_lock_of},
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // coop_script logs it
	}
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_pop(L, 1);
}
} // namespace money
} // namespace coop
