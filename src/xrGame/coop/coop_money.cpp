// Anomaly Coop — player money lives on the host. See coop_money.h and xrCoop/coop_ledger.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_money.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_levels.h"
#include "coop_sync.h"
#include "coop_entities.h"
#include "coop_events.h"
#include "coop_dialog.h"
#include "coop_script.h"
#include "coop_world.h"
#include "xrCoop/coop_ledger.h"
#include "xrCoop/coop_limits.h"
#include "coop_lua.h" // lua_api: the registrar, the argument readers
#include "xrCoop/coop_lua_errors.h" // lua_errors::LogBudget

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
constexpr u32 kVerdictCount = ledger::kVerdictCount;
constexpr int kMaxSetting = 2000000000; // console and ltx values are ints
constexpr LPCSTR kSection = "coop_money";

// ---------------------------------------------------------------- settings
int g_gain_burst = int(ledger::Policy().script_gain_burst);
int g_gain_per_min = int(ledger::Policy().script_gain_per_min);
int g_dialog_cap = int(ledger::Policy().dialog_gain_cap);
int g_dialog_per_min = int(ledger::Policy().dialog_gain_per_min);
int g_max_money = int(ledger::Policy().max_money);
// 1.7 (the lowest stock mech_discount) x 1.0 (the economy slider `upgrade` of
// ui_options.script starts at 1.0; plugins\difficulty.ltx 1.3 is only a preset's value).
float g_upgrade_min_percent = 1.7f;
bool g_scripted_upgrades = false; // upgrades that name no mechanic (a script's) are refused
// The session's transfer rules, pushed by the host's Lua (coop.money_set_rules). Kept for the
// process: a new session or a world load does not widen them (a host that played a session with transfers off starts the
// next one with them off until its Lua has pushed again), the host's Lua says so when they change. Before it has said
// anything: the options' defaults, transfers on, kDefaultTransferMax at a time. Not fail-closed: a mod that
// calls coop.money_give on a host without the economy script keeps working.
bool g_transfer_on = true;
u64 g_transfer_max = ledger::kDefaultTransferMax;
// How fast one character may give money away (Policy::transfer_burst, transfer_per_min).
int g_transfer_burst = int(ledger::Policy().transfer_burst);
int g_transfer_per_min = int(ledger::Policy().transfer_per_min);

ledger::Policy policy()
{
	ledger::Policy p;
	p.transfer_on = g_transfer_on;
	p.transfer_max = g_transfer_max;
	p.transfer_burst = u32(_max(g_transfer_burst, 0));
	p.transfer_per_min = u32(_max(g_transfer_per_min, 0));
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
	g_transfer_burst = read_setting(ini, "transfer_burst", g_transfer_burst, 0);
	g_transfer_per_min = read_setting(ini, "transfer_per_min", g_transfer_per_min, 0);
	if (ini.line_exist(kSection, "upgrade_min_percent"))
	{
		const float f = ini.r_float(kSection, "upgrade_min_percent");
		if (_valid(f) && f >= 0.f && f <= 100.f)
			g_upgrade_min_percent = f;
	}
	if (ini.line_exist(kSection, "scripted_upgrades"))
		g_scripted_upgrades = !!ini.r_bool(kSection, "scripted_upgrades");
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
	CMD4(CCC_Integer, "coop_money_transfer_burst", &g_transfer_burst, 0, kMaxSetting);
	CMD4(CCC_Integer, "coop_money_transfer_per_min", &g_transfer_per_min, 0, kMaxSetting);
}

lua_errors::LogBudget g_log{20}; // the module's log lines: the first 20, then every 100th

const char* verdict_name(Verdict v)
{
	switch (v)
	{
	case Verdict::Applied: return "applied";
	case Verdict::Insufficient: return "not enough money";
	case Verdict::NoAllowance: return "over the allowance";
	case Verdict::OverMax: return "over the maximum";
	case Verdict::Malformed: return "malformed";
	case Verdict::TransfersOff: return "transfers are off";
	case Verdict::OverTransferLimit: return "over the transfer limit";
	case Verdict::TransferRate: return "too many transfers";
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
// leaving and joining again refills nothing; they refill from the moment the character is live
// (host_on_peer_in_world). Rules: xrCoop/coop_ledger.h HostBook.
ledger::HostBook g_book;
// In Server mode, a trade based on a stale map server balance can exceed the world's balance. TradeDebts (xrCoop/coop_ledger.h)
// keeps the shortfall for the session and applies the player's next income to it. Co-op uses the world's balance directly.
ledger::TradeDebts g_debts;

struct HostCounters
{
	u64 changes = 0, malformed = 0, no_character = 0, stale = 0, dropped_joining = 0, bad_receiver = 0;
	u64 gaps = 0; // requests that reveal missing sequence numbers at the host
	u64 notes_sent = 0; // answers that carried a refusal, gifts told to the receiver
	u64 verdicts[kVerdictCount] = {};
	u64 gain_script = 0, gain_dialog = 0; // RU
	u64 grants = 0, trades = 0, transfers = 0, capped = 0, joins = 0;
	u64 shortfalls = 0, shortfall_ru = 0, debt_paid = 0; // trade charges kept as debt (RU), RU of debt paid back
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
	CSE_Abstract* e = id == kNoId ? nullptr : world::server_entity(id);
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
		io->set_money(value, true); // GE_MONEY: the server entity keeps it, so does the save
	else if (CSE_ALifeTraderAbstract* t = owner_entity(holder))
		t->m_dwMoney = value;
}

// The player key a character is bound to (ledger::character_key of its custom data), empty if none.
std::string custom_data_key(const CSE_Abstract* e)
{
	return e && e->m_ini_string.size() ? ledger::character_key(e->m_ini_string.c_str()) : std::string();
}

// The key the money book knows a character by. A character without a key line (none of coop_join's)
// gets one of its id that no save snapshot holds: it counts as made in this session.
std::string character_key_of(u16 holder)
{
	std::string key = custom_data_key(world::server_entity(holder));
	if (key.empty())
	{
		char id[16];
		xr_sprintf(id, "#%u", u32(holder));
		key = id;
	}
	return key;
}

// Tells this machine's player what a note says (the Lua callback coop_money_note, coop_economy_money.script):
//   coop_money_note("received", giver_peer, amount, nil, nil)
//   coop_money_note("refused", receiver_peer | -1, change (negative: a spend or a transfer), why, cause)
// `why` and `cause` are the English names verdict_name / cause_name give.
void fire_note(const ledger::MoneyNote& n)
{
	using namespace script;
	if (n.kind == ledger::NoteKind::Received)
		queue_callback("coop_money_note", {cb_str("received"), cb_int(n.peer), cb_int(n.amount), cb_nil(), cb_nil()});
	else if (n.kind == ledger::NoteKind::Refused)
		queue_callback("coop_money_note", {cb_str("refused"), cb_int(n.cause == Cause::Transfer ? s64(n.peer) : s64(-1)),
		    cb_int(n.amount), cb_str(verdict_name(n.verdict)), cb_str(cause_name(n.cause))});
}

// The player's balance (and a note about it). The host player's too, over the internal channel: his actor holds
// the balance already (one object in the process), his machine is told the note from it (client_on_balance).
void send_balance(PeerId to, u32 balance, const ledger::MoneyNote& note = ledger::MoneyNote())
{
	if (note.kind != ledger::NoteKind::None)
		++g_h.notes_sent;
	ledger::MoneyBalanceMsg m;
	m.balance = balance;
	m.acked_seq = g_book.acked(to);
	m.note = note;
	Writer w(ledger::kMoneyBalanceSize);
	ledger::write(w, m);
	net::host_send(to, Msg::MoneyBalance, w); // queued while he loads the world
	levels::host_map_changed(); // his map server judges his next deal by the LevelMap (host_balance): now, not at the next 5 s
}

enum class Source : u8
{
	Grant, // host_adjust / host_set from the host (Lua, the engine)
	Trade, // host_adjust(..., "trade") from coop_trade
	Transfer, // the receiver's side of a transfer (counted with the transfer)
};

// Returns the portion of positive `credit` in RU left after paying `holder`'s trade debt. `peer` identifies the player for the log
// (kInvalidPeer if absent).
s64 pay_debt(PeerId peer, u16 holder, s64 credit, const char* why)
{
	if (g_debts.empty() || credit <= 0)
		return credit;
	const std::string key = character_key_of(holder);
	u32 paid = 0;
	g_debts.income(key, credit > s64(0xffffffffu) ? 0xffffffffu : u32(credit), paid);
	if (!paid)
		return credit;
	g_h.debt_paid += paid;
	::Msg("[coop] money of <%s>: %lld RU (%s) pay %u RU of his trade debt first, %u RU still owed", nick_of(peer), credit,
	    why ? why : "?", paid, g_debts.debt_of(key));
	return credit - s64(paid);
}

// A trade charge `delta` (< 0) can exceed the world's `balance` when the map server settles the goods using a stale LevelMap.
// Keep the unpaid amount as the player's trade debt.
void note_shortfall(PeerId peer, u16 holder, u32 balance, s64 delta)
{
	const u32 charge = -delta > s64(0xffffffffu) ? 0xffffffffu : u32(-delta);
	const std::string key = character_key_of(holder);
	u32 shortfall = 0;
	g_debts.debit(key, balance, charge, shortfall);
	++g_h.shortfalls;
	g_h.shortfall_ru += shortfall;
	if (g_h.shortfalls <= 8 || g_h.shortfalls % 64 == 0)
		::Msg("[coop-trust] trade_shortfall: <%s>: a trade charged %u RU, the world's balance was %u RU (his map server judged the deal "
		      "by an older one): %u RU kept as his debt, paid by his next income (%u RU owed; %llu shortfalls)",
		    nick_of(peer), charge, balance, shortfall, g_debts.debt_of(key), g_h.shortfalls);
}

// Host: changes a balance by `delta` within [0, max(max_money, balance)] and tells its player (with `note`).
// note_shortfall records the unpaid portion of a trade charge. Income pays trade debt first.
bool adjust(PeerId peer, s64 delta, const char* why, Source source,
    const ledger::MoneyNote& note = ledger::MoneyNote())
{
	if (!mode().decides_world())
		return false;
	const u16 holder = holder_of(peer);
	u32 balance = 0;
	if (holder == kNoId || !read_balance(holder, balance))
		return false;
	const bool short_trade = source == Source::Trade && delta < 0 && s64(balance) + delta < 0;
	if (short_trade)
		note_shortfall(peer, holder, balance, delta);
	else if (delta > 0)
		delta = pay_debt(peer, holder, delta, why);
	const s64 want = s64(balance) + delta;
	const u32 next = ledger::capped_balance(balance, delta, policy().max_money);
	if (source == Source::Grant)
		++g_h.grants;
	else if (source == Source::Trade)
		++g_h.trades;
	if (s64(next) != want && !short_trade) // a short trade's line is note_shortfall's
	{
		++g_h.capped;
		if (g_log.allow())
			::Msg("! [coop] money of <%s>: %u RU %+lld (%s) capped at %u", nick_of(peer), balance, delta, why, next);
	}
	if (next != balance)
		write_balance(holder, next);
	send_balance(peer, next, note);
	return true;
}

// The world's balances by peer for ledger::settle_transfer: set writes a giver's balance, add credits a receiver (and
// sends him his balance, with the giver's name).
struct WorldBalances
{
	PeerId giver = kInvalidPeer;
	void set(uint32_t peer, uint32_t value)
	{
		const u16 holder = holder_of(PeerId(peer));
		if (holder != kNoId)
			write_balance(holder, value);
	}
	void add(uint32_t peer, uint32_t credit)
	{
		adjust(PeerId(peer), s64(credit), "transfer", Source::Transfer, ledger::note_received(giver, credit));
	}
};

// A transfer's receiver as the ledger wants it: another player of the session (joined, or the host) who has a
// character, with his balance.
ledger::Receiver receiver_of(PeerId to)
{
	ledger::Receiver r;
	r.peer = to;
	if (to == kHostPeer && !mgr().session().local())
		return r; // a dedicated host: nobody plays there, the money would go to its ghost
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

// A player's change of his money (the host player's from peer 0 over the internal channel - his transfers; his
// other changes are the world's own, made on his actor directly).
void host_on_req(PeerId from, Reader& r)
{
	if (!mode().decides_world())
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
		if (g_log.allow())
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
	// D11: told first, with an `acked` that still stands before the hole: the client must not take them as answered.
	if (const u32 skipped = ledger::seq_gap(g_book.acked(from), q.seq))
	{
		++g_h.gaps;
		send_balance(from, balance, ledger::note_gap(g_book.acked(from) + 1, skipped));
	}
	// The talk lock is only the condition "a dialog is going on": the dialog allowance is the character's.
	levels::BodyFacts there; // on a map server's level the talk lock is its authority's
	const bool talking = dialog::host_lock_of(from) != kNoId || (mode().facts_of(join::proxy_of_peer(from), there) && there.talking);
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
			world.giver = from;
			ledger::settle_transfer(world, from, d.balance, q.ref, d.credit); // giver first
			++g_h.transfers;
		}
		else if (d.balance > balance) // a gain pays his trade debt first
			write_balance(holder, balance + u32(pay_debt(from, holder, s64(d.balance) - s64(balance), cause_name(q.cause))));
		else if (d.balance != balance)
			write_balance(holder, d.balance);
	}
	else if (q.cause == Cause::Transfer && (!to.present || to.peer == from))
		++g_h.bad_receiver;
	if (d.verdict != Verdict::Applied && g_log.allow())
		::Msg("[coop] money change #%u of <%s> refused: %+d RU (%s), %s; balance %u RU", q.seq, nick_of(from), q.delta,
		    cause_name(q.cause), verdict_name(d.verdict), d.balance);
	u32 now_holds = d.balance; // what the character holds after it all (the same unless a transfer went to himself)
	read_balance(holder, now_holds);
	send_balance(from, now_holds, d.note); // a refusal is told to the player with the answer
}

// ---------------------------------------------------------------- client state
struct ClientState
{
	bool anchored = false; // `last` follows the actor's money
	u32 last = 0; // the actor's money as we last saw or set it
	u32 host_balance = 0, acked = 0; // the newest MoneyBalance
	std::vector<ledger::Pending> pending; // changes sent (or queued) that the host has not answered
	xr_vector<ledger::MoneyReq> outbox; // queued for frame()
	ledger::UpgradeHolds holds; // payments requests carry, until their answers (client_hold)
	ledger::RefusalGate refusals; // how often a refusal of the host is told to the player (per cause)
};
// GE_MONEY may come from the game thread; the answers come with the network pump on the main thread.
xrCriticalSection g_cl_lock;
ClientState g_cl;
u32 g_cl_seq = 0; // process lifetime: an answer from an older world never covers a newer change
u32 g_hold_seq = 0; // process lifetime: a request of an older world never releases a newer hold
ClientSpendScope* g_scope = nullptr; // the innermost alive ClientSpendScope (main thread)

struct ClientCounters
{
	u64 sent = 0, balances = 0, set_by_host = 0, scoped = 0, gives = 0, malformed = 0;
	u64 holds = 0, unclaimed = 0;
	u64 gifts = 0, refusals = 0, refusals_muted = 0; // notes told to the player (gifts, refusals), and the muted
	u64 resent = 0; // changes resent to fill sequence gaps reported by the host
} g_c;
limits::Pacer g_cl_pacer; // D11: over the host's rate a ReqMoney is lost with no answer

// Under g_cl_lock.
void queue_change(s64 delta, Cause cause, PeerId ref)
{
	const s64 lim = s64(INT_MAX);
	const s32 d = s32(delta > lim ? lim : (delta < -lim ? -lim : delta)); // out of range: refused, then corrected
	const ledger::MoneyReq q{++g_cl_seq, d, cause, ref};
	g_cl.outbox.push_back(q);
	g_cl.pending.push_back({q.seq, q.delta, q.cause, q.ref});
}

// Under g_cl_lock. The answer's note is told once the balance is on screen. A mod that changes the money in a
// loop is refused over and over: only one refusal of a cause in a few seconds is told.
bool note_allowed(const ledger::MoneyNote& n)
{
	if (n.kind == ledger::NoteKind::Refused)
		return g_cl.refusals.allow(n.cause, mgr().now_ms());
	return n.kind == ledger::NoteKind::Received;
}

void tell_note(const ledger::MoneyNote& n, bool tell)
{
	if (n.kind == ledger::NoteKind::Received && tell)
		++g_c.gifts;
	else if (n.kind == ledger::NoteKind::Refused)
		++(tell ? g_c.refusals : g_c.refusals_muted);
	if (tell)
		fire_note(n);
}

// A player: the host's MoneyBalance. A remote client shows it (ledger::client_view) and is told its note. The
// host player's actor holds his balance itself (the world wrote it: one object in the process): he is only told the note.
void client_on_balance(PeerId from, Reader& r)
{
	if (from != kHostPeer || !mgr().active())
		return;
	ledger::MoneyBalanceMsg m;
	if (!ledger::read(r, m))
	{
		++g_c.malformed;
		// An incompatible MoneyBalance layout fails decoding and is logged under the rate limit.
		if (g_log.allow())
			::Msg("! [coop] malformed MoneyBalance from the host: is its build of the game different from ours?");
		return;
	}
	if (m.note.kind == ledger::NoteKind::Gap)
	{
		xrCriticalSectionGuard guard(g_cl_lock);
		for (const ledger::Pending& p : ledger::take_skipped(g_cl.pending, m.note.peer, u32(_max(m.note.amount, 0))))
		{
			queue_change(p.delta, p.cause, p.ref);
			++g_c.resent;
		}
		return; // its balance and `acked` say nothing new
	}
	if (mode().decides_world())
	{
		++g_c.balances;
		bool tell = false;
		{
			xrCriticalSectionGuard guard(g_cl_lock);
			tell = note_allowed(m.note);
		}
		tell_note(m.note, tell);
		return;
	}
	// Live first: Actor() asserts on GameID(), which has no level in the main menu. Protocol 8 makes a new player's
	// character before his world, so the host's money_set reaches him there (else the client crashes);
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
		// An upgrade paid here whose request is not answered yet stays out of it
		view = ledger::client_view(m.balance, m.acked_seq, g_cl.pending, policy().max_money, g_cl.holds.sum());
	}
	++g_c.balances;
	const u32 shown = a->get_money();
	if (shown != view)
	{
		++g_c.set_by_host;
		if (g_log.allow())
		{
			// The host's reason is the note that came with the balance (the wire has no other): a gift from a player, a
			// refusal of a change of ours. A change of the host's own (a trade, a grant, the death fee: host_adjust /
			// host_set) comes with no note; its `why` stays on the host. Only a refusal (money of ours undone) has "!".
			if (m.note.kind == ledger::NoteKind::Received)
				::Msg("[coop] money from the host: %u RU (shown %u RU, the host holds %u RU; the host's reason: %d RU from "
				      "player %u)",
				    view, shown, m.balance, m.note.amount, m.note.peer);
			else if (m.note.kind == ledger::NoteKind::Refused)
				::Msg("! [coop] money from the host: %u RU (shown %u RU, the host holds %u RU; the host's reason: refused a "
				      "%s change of %d RU, %s)",
				    view, shown, m.balance, cause_name(m.note.cause), m.note.amount, verdict_name(m.note.verdict));
			else
				::Msg("[coop] money from the host: %u RU (shown %u RU, the host holds %u RU; the host's reason: its own "
				      "change, no note: a trade, a grant or a fee)",
				    view, shown, m.balance);
		}
		entities::ClientApply apply; // our own GE_MONEY below is not a change of ours
		a->set_money(view, true);
	}
	bool tell = false;
	{
		xrCriticalSectionGuard guard(g_cl_lock);
		g_cl.anchored = true;
		g_cl.last = a->get_money();
		tell = note_allowed(m.note);
	}
	tell_note(m.note, tell);
}

// ---------------------------------------------------------------- Lua
// The numbers of coop.money_*: cut toward zero as give_money(int) does (lua_api::arg_trunc).
// A peer id 0 .. kInvalidPeer - 1.
bool arg_peer(lua_State* L, int idx, PeerId& out)
{
	s64 v = 0;
	if (!lua_api::arg_trunc(L, idx, 0.0, double(kInvalidPeer - 1), v))
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

// The host player gives money as a client does - a ReqMoney (Transfer) to his world server over the internal
// channel, from the next frame (client_flush); the world settles it by the same rules and rates as a client's and
// answers him with a MoneyBalance (a refusal is his note). His actor's money is the world's balance: nothing is taken
// here (the world takes it when it settles), only checked.
int host_player_give(lua_State* L, PeerId to, u32 amount)
{
	if (to == kHostPeer)
		return refuse(L, "yourself");
	if (!mgr().session().peer(to))
		return refuse(L, "no such player");
	u32 balance = 0;
	if (!read_balance(holder_of(kHostPeer), balance))
		return refuse(L, "no character");
	if (balance < amount)
		return refuse(L, verdict_name(Verdict::Insufficient));
	xrCriticalSectionGuard guard(g_cl_lock);
	const s32 delta = s32(-_min(s64(amount), s64(INT_MAX))); // as queue_change: out of range is refused there
	const ledger::MoneyReq q{++g_cl_seq, delta, Cause::Transfer, to}; // no pending: there is no view to correct
	g_cl.outbox.push_back(q);
	++g_c.gives;
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
	if (!arg_peer(L, 1, to) || !lua_api::arg_trunc(L, 2, 1.0, double(max_money()), amount))
		return refuse(L, "bad arguments");
	return mode().decides_world() ? host_player_give(L, to, u32(amount)) : client_give(L, to, u32(amount));
}

// coop.money_grant(peer, amount) -> ok, why: the authority of a level; a map server's goes to the book of the world (host_adjust). A negative
// amount takes money (never below 0).
int l_money_grant(lua_State* L)
{
	if (!(mode().decides_world() || mode().leads_level()))
		return refuse(L, "host only");
	PeerId peer = kInvalidPeer;
	s64 amount = 0;
	const double cap = double(max_money());
	if (!arg_peer(L, 1, peer) || !lua_api::arg_trunc(L, 2, -cap, cap, amount) || amount == 0)
		return refuse(L, "bad arguments");
	return host_adjust(peer, amount, "grant") ? done(L) : refuse(L, "no character");
}

// coop.money_grant_to(character, amount) -> added | nil, why: host.
int l_money_grant_to(lua_State* L)
{
	if (!mode().decides_world())
		return refuse(L, "host only");
	s64 character = 0, amount = 0;
	if (!lua_api::arg_trunc(L, 1, 1.0, double(kNoId - 1), character) || !lua_api::arg_trunc(L, 2, 1.0, double(max_money()), amount))
		return refuse(L, "bad arguments");
	u32 added = 0;
	if (!host_add_to_character(u16(character), u32(amount), "group reward", added))
		return refuse(L, "no character");
	lua_pushnumber(L, lua_Number(added));
	return 1;
}

// coop.money_set(peer, amount) -> ok, why: host.
int l_money_set(lua_State* L)
{
	if (!mode().decides_world())
		return refuse(L, "host only");
	PeerId peer = kInvalidPeer;
	s64 amount = 0;
	if (!arg_peer(L, 1, peer) || !lua_api::arg_trunc(L, 2, 0.0, double(max_money()), amount))
		return refuse(L, "bad arguments");
	return host_set(peer, u32(amount), "set") ? done(L) : refuse(L, "no character");
}

// coop.money_set_rules(enabled, max_transfer) -> ok, why: host. The session's rules for money between players
// (money_transfer, max_money_transfer): the host's Lua pushes them, the ledger holds a client to them.
int l_money_set_rules(lua_State* L)
{
	if (!mode().decides_world())
		return refuse(L, "host only");
	s64 limit = 0;
	if (lua_type(L, 1) != LUA_TBOOLEAN || !lua_api::arg_trunc(L, 2, 0.0, double(UINT32_MAX), limit))
		return refuse(L, "bad arguments");
	const bool on = lua_toboolean(L, 1) != 0;
	if (on != g_transfer_on || u64(limit) != g_transfer_max)
		::Msg("[coop] money transfers between players: %s, at most %lld RU at a time", on ? "on" : "off", limit);
	g_transfer_on = on;
	g_transfer_max = u64(limit);
	return done(L);
}

// coop.money_of(peer) -> RU | nil: host; on a map server what the world server's table says of a player of its level.
int l_money_of(lua_State* L)
{
	PeerId peer = kInvalidPeer;
	u32 balance = 0;
	// A local proxy with no balance yet still reports zero.
	if (const LevelSide* side = mode().level())
	{
		if (arg_peer(L, 1, peer) && join::proxy_of_peer(peer) != kNoId)
		{
			side->level_balance(peer, balance);
			lua_pushnumber(L, lua_Number(balance));
		}
		else
			lua_pushnil(L);
		return 1;
	}
	const u16 holder = mode().decides_world() && arg_peer(L, 1, peer) ? holder_of(peer) : kNoId;
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
	const u16 npc = mode().decides_world() && arg_peer(L, 1, peer) ? dialog::host_lock_of(peer) : kNoId;
	if (npc != kNoId)
		lua_pushnumber(L, lua_Number(npc));
	else
		lua_pushnil(L);
	return 1;
}
} // namespace

// ---------------------------------------------------------------- API

void init()
{
	load_settings();
	register_console();
}

void register_commands() { register_console(); }

void install_server(Session& s) { s.on(Msg::ReqMoney, &host_on_req); }

void install_client(Endpoint& e) { e.on(Msg::MoneyBalance, &client_on_balance); }

void reset()
{
	g_book.reset(); // a new session (or none): nothing is kept
	g_debts.clear(); // the trade debts are the session's
	// except the transfer rules (g_transfer_*): the host's Lua says when they change, and says them again every 2 s
	xrCriticalSectionGuard guard(g_cl_lock);
	g_cl = ClientState(); // g_cl_seq goes on: see its comment
}

void on_session_started()
{
	reset();
	if (!mode().decides_world() || !ai().get_alife())
		return;
	// The player characters in the world now are "from the save" (returning players keep full
	// allowances). One the host makes later in this session for a player key it has not seen starts empty. Known by
	// the key they are bound to their players by, not by id.
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
	if (mgr().is_live_client() || mode().decides_world()) // the host player's transfers go out too
		client_flush(true);
}

void on_peer_left(PeerId id) { g_book.peer_left(id); } // the character keeps its allowances

void host_on_peer_in_world(PeerId id)
{
	// The client drops a MoneyBalance until it is live, and the save it loaded may be older than its
	// balance (a grant while it loaded). One answer now anchors it: its pending list is empty at this point.
	const u16 holder = mode().decides_world() && id != kHostPeer ? holder_of(id) : kNoId;
	u32 balance = 0;
	if (holder == kNoId || !read_balance(holder, balance))
		return;
	// The character is live. Its allowances refill from now (a new one starts empty, a returning one
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
	if (mode().decides_world())
	{
		::Msg("[coop]   money: changes in %llu, refused insufficient/allowance/max %llu/%llu/%llu, by the transfer rules "
		      "off/limit/rate %llu/%llu/%llu (%llu malformed, %llu to nobody, %llu stale, %llu without a character, %llu dropped "
		      "while rejoining), gains script %llu dialog %llu RU, grants %llu, trades %llu (%llu capped), transfers "
		      "%llu, %llu balances sent on joining, %llu notes sent",
		    g_h.changes, g_h.verdicts[u32(Verdict::Insufficient)], g_h.verdicts[u32(Verdict::NoAllowance)],
		    g_h.verdicts[u32(Verdict::OverMax)], g_h.verdicts[u32(Verdict::TransfersOff)],
		    g_h.verdicts[u32(Verdict::OverTransferLimit)], g_h.verdicts[u32(Verdict::TransferRate)],
		    g_h.verdicts[u32(Verdict::Malformed)] + g_h.malformed,
		    g_h.bad_receiver, g_h.stale, g_h.no_character, g_h.dropped_joining, g_h.gain_script, g_h.gain_dialog,
		    g_h.grants, g_h.trades, g_h.capped, g_h.transfers, g_h.joins, g_h.notes_sent);
		::Msg("[coop]   money: %llu requests showed seqs never seen here (D11, the client sends those again)", g_h.gaps);
		::Msg("[coop]   money: %llu trade charges larger than the world's balance (%llu RU) kept as debt, %llu RU of it paid back; "
		      "%u character(s) owe %llu RU now (%llu RU untracked)",
		    g_h.shortfalls, g_h.shortfall_ru, g_h.debt_paid, (u32)g_debts.size(), g_debts.total(), g_debts.untracked());
		::Msg("[coop]   money settings: gains %u in a row + %u per minute, dialogs %u in a row + %u per minute, max %u, "
		      "upgrades from %.2f x cost, upgrades without a mechanic %s, transfers between players %s (at most %llu RU "
		      "each, %u in a row + %u per minute)",
		    p.script_gain_burst, p.script_gain_per_min, p.dialog_gain_cap, p.dialog_gain_per_min, p.max_money,
		    g_upgrade_min_percent, g_scripted_upgrades ? "taken" : "refused", p.transfer_on ? "on" : "off", p.transfer_max,
		    p.transfer_burst, p.transfer_per_min);
		for (const auto& it : g_book.connections())
		{
			const u16 holder = holder_of(it.first);
			const std::string key = holder != kNoId ? character_key_of(holder) : std::string();
			const ledger::CharacterAllowances* a = key.empty() ? nullptr : g_book.allowances(key);
			const bool saved = !key.empty() && g_book.from_save(key);
			// Not used yet: full for a character from the save, empty for a new one.
			const auto shown = [saved](const ledger::Allowance* x, u32 burst)
			{ return x && x->started ? x->tokens : (saved ? double(burst) : 0.0); };
			::Msg("[coop]     #%u <%s>: %u RU, answered #%u, character %u (%s), allowances script %.0f / dialog %.0f RU",
			    it.first, nick_of(it.first), host_balance(it.first), it.second, holder, saved ? "from the save" : "new",
			    shown(a ? &a->script : nullptr, p.script_gain_burst), shown(a ? &a->dialog : nullptr, p.dialog_gain_cap));
		}
		::Msg("[coop]   money (the host player, over the internal channel): transfers asked %llu, sent %llu, balances in "
		      "%llu; told: %llu gifts, %llu refusals (%llu muted)",
		    g_c.gives, g_c.sent, g_c.balances, g_c.gifts, g_c.refusals, g_c.refusals_muted);
		return;
	}
	if (!mgr().active())
		return;
	xrCriticalSectionGuard guard(g_cl_lock);
	::Msg("[coop]   money: %llu changes sent, %u waiting (last #%u); host balance %u RU (answered #%u), %llu balances in, "
	      "%llu set by the host; %llu spends in scopes, %llu gives; upgrades paid here %llu (%u unanswered, %llu RU), "
	      "%llu scope spends sent as changes; told: %llu gifts, %llu refusals (%llu muted)",
	    g_c.sent, (u32)g_cl.pending.size(), g_cl_seq, g_cl.host_balance, g_cl.acked, g_c.balances, g_c.set_by_host,
	    g_c.scoped, g_c.gives, g_c.holds, (u32)g_cl.holds.size(), g_cl.holds.sum(), g_c.unclaimed, g_c.gifts,
	    g_c.refusals, g_c.refusals_muted);
}

u32 max_money() { return policy().max_money; }

float upgrade_min_percent() { return g_upgrade_min_percent; }

bool scripted_upgrades() { return g_scripted_upgrades; }

// The book is the world server's. A map server judges a deal by the last balance the world server's LevelMap named, and what the
// deal changes goes up at once (coop_levels_xfer.cpp LevelEntity Money); the player's MoneyBalance comes from the world server.
u32 host_balance(PeerId peer)
{
	u32 balance = 0;
	if (const LevelSide* side = mode().level())
	{
		side->level_balance(peer, balance);
		return balance;
	}
	const u16 holder = mode().decides_world() ? holder_of(peer) : kNoId;
	return holder != kNoId && read_balance(holder, balance) ? balance : 0;
}

bool host_adjust(PeerId peer, s64 delta, const char* why)
{
	// Trades are counted apart from grants (coop_trade passes "trade").
	const bool trade = why && !xr_strcmp(why, "trade");
	if (LevelSide* side = mode().level())
	{
		side->level_money_change(peer, delta, trade);
		return true;
	}
	return adjust(peer, delta, why, trade ? Source::Trade : Source::Grant);
}

bool host_add_to_character(u16 character, u32 amount, const char* why, u32& added)
{
	added = 0;
	if (!mode().decides_world() || !world::is_player_character(world::server_entity(character)))
		return false;
	u32 balance = 0;
	if (!read_balance(character, balance))
		return false;
	// An income pays his trade debt first (adjust and pay_debt below); `added` is what his balance gains
	const u32 owed = g_debts.empty() ? 0u : g_debts.debt_of(character_key_of(character));
	const u32 to_debt = owed < amount ? owed : amount;
	added = ledger::capped_balance(balance, s64(amount) - s64(to_debt), policy().max_money) - balance;
	for (const PeerInfo& p : mgr().session().peers())
		if (join::proxy_of_peer(p.id) == character) // he is here (or loading the world): his balance is sent to him
			return adjust(p.id, s64(amount), why, Source::Grant);
	const u32 next = ledger::capped_balance(balance, pay_debt(kInvalidPeer, character, s64(amount), why), policy().max_money);
	++g_h.grants;
	if (next != balance)
		write_balance(character, next);
	return true;
}

bool host_set(PeerId peer, u32 amount, const char* why)
{
	if (!mode().decides_world())
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
		if (g_log.allow())
			::Msg("! [coop] money of <%s> set to %u RU (%s), capped at %u", nick_of(peer), amount, why, next);
	}
	if (next != balance)
		write_balance(holder, next);
	send_balance(peer, next);
	return true;
}

void host_send_balance(PeerId peer)
{
	if (LevelSide* side = mode().level())
	{
		side->level_money_change(peer, 0, false);
		return;
	}
	u32 balance = 0;
	const u16 holder = mode().decides_world() ? holder_of(peer) : kNoId;
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
		g_scope->m_spent = sum > u64(u32(-1)) ? u32(-1) : u32(sum); // its request carries the sum
		++g_c.scoped;
		return;
	}
	queue_change(delta, dialog::client_partner() != kNoId ? Cause::Dialog : Cause::Script, 0);
}

void client_flush(bool paced)
{
	if (!mgr().is_live_client() && !mode().decides_world())
		return;
	xr_vector<ledger::MoneyReq> out;
	{
		xrCriticalSectionGuard guard(g_cl_lock);
		const u64 now = mgr().now_ms();
		size_t n = paced ? 0 : g_cl.outbox.size();
		while (paced && n < g_cl.outbox.size() && g_cl_pacer.take(Msg::ReqMoney, now))
			++n;
		out.assign(g_cl.outbox.begin(), g_cl.outbox.begin() + n);
		g_cl.outbox.erase(g_cl.outbox.begin(), g_cl.outbox.begin() + n);
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
		{"money_grant_to", &l_money_grant_to},
		{"money_set", &l_money_set},
		{"money_set_rules", &l_money_set_rules},
		{"money_of", &l_money_of},
		{"talk_lock_of", &l_talk_lock_of},
		{nullptr, nullptr},
	};
	lua_api::fill(L, nullptr, fns); // coop_script logs a missing coop
}
} // namespace money
} // namespace coop
