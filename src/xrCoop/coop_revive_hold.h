// Anomaly Coop — the revive hold made visible (HUD v2 part 2, docs/HUD_V2.md "Поднятие союзника").
// A teammate revives a wounded player by holding USE with a medkit for kHoldMs (coop_survival.cpp). The reviver's machine
// counts the hold and the host checks the revive when it is over (Request). So that the wounded sees the hold while it
// goes on (a ring in the reviver's colour), and the reviver gets a truthful "done", five ops join Msg::PlayerRevive
// (`u8 op` first, all layouts below follow it; ids in host numbering; protocol 8, not released):
//   Begin    reviver -> host         {u16 target, u16 seq, u16 item, u16 held_ms}  USE was held held_ms when it went out
//   Cancel   reviver -> host         {u16 target, u16 seq}                         he let go / went away / fell
//   Reviving host -> target owner    {u16 target, u16 seq, u16 reviver, u32 reviver_peer, u32 total_ms, u32 held_ms}
//   Stopped  host -> target owner    {u16 target, u16 seq, u16 reviver}            the hold ended without a revive
//   Done     host -> reviver         {u16 target, u16 seq}                         the revive went through
// The host checks a Begin as a Request (verdict: target wounded on this fall, in range, the reviver standing with the item)
// and answers a refusal with Refused, so a hold that cannot end well ends at once. One reviver holds one wounded at a time:
// a second one (a Begin, and a Request too) is refused as busy until the first one ends or his lock runs out. The lock is the
// hold's length and kGraceMs, and no Begin of the same reviver stretches it past kHoldMs + kGraceMs from his first one (a
// Cancel in between does not start it over), so a client that sends Begins, or a Cancel before each Begin, cannot keep the
// others away (HostHold below). The wounded's owner animates the ring from
// `held_ms` / `total_ms` on his own clock; a hold whose Stopped never came lapses kGraceMs after its end; a reviver who leaves
// mid-hold has his lock dropped and the wounded told (Stopped), as for a Cancel. No Reviving / Stopped reaches anybody but
// the wounded.
// Rate: the reviver announces a hold only after kAnnounceAfterMs of USE (a tap sends nothing), at most one Begin per
// kAnnounceGapMs (the host takes none closer than kHostBeginGapMs), and a Cancel only for an announced hold, so Begin + Cancel
// (or Begin + Request) is at most a pair per kAnnounceGapMs, within the PlayerRevive row of coop_limits.h
// (tests/test_revive_hold.cpp mashes the button against it).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstdint>

#include "coop_protocol.h" // Writer, Reader

namespace coop
{
namespace revive
{
// The `u8 op` of Msg::PlayerRevive. Request .. Evacuate are F3 Task 8's (coop_survival.h).
enum Op : uint8_t
{
	OpRequest = 0, // reviver -> host: the hold is over, revive
	OpRevive = 1,
	OpRespawn = 2,
	OpSpectate = 3,
	OpRefused = 4, // host -> reviver
	OpEvacuate = 5,
	OpBegin = 6,
	OpCancel = 7,
	OpReviving = 8,
	OpStopped = 9,
	OpDone = 10,
};

// What a client may send under this message: the others are the host's orders.
inline bool is_client_op(uint8_t op) { return op == OpRequest || op == OpBegin || op == OpCancel; }

enum Refusal : uint8_t
{
	RefNone = 0,
	RefNotWounded = 1,
	RefTooFar = 2,
	RefNoMedkit = 3,
	RefBusy = 4,
	RefYouAreDown = 5,
	RefSelf = 6,
};

constexpr uint32_t kHoldMs = 3000; // USE on a wounded teammate with a medkit (coop_survival kReviveHoldMs)
constexpr uint32_t kGiveUpMs = 3000; // the wounded holds USE to bleed out at once (coop_survival kGiveUpHoldMs)
constexpr uint32_t kMaxHoldMs = 60000; // a length the wounded's owner takes from the host
constexpr uint32_t kAnnounceAfterMs = 250; // a hold is announced once USE was held this long
constexpr uint32_t kAnnounceGapMs = 2000; // at most one Begin per reviver in this time
constexpr uint32_t kGraceMs = 1500; // a hold that came to its end waits this long for the Revive / Stopped, then lapses
constexpr uint32_t kBeginJitterMs = 500; // what the net may squeeze out of the gap between two honest Begins on their way
constexpr uint32_t kHostBeginGapMs = kAnnounceGapMs - kBeginJitterMs; // the host takes no Begin of one reviver closer than this

// 0..1: `elapsed_ms` of `total_ms`.
inline float fraction(uint64_t elapsed_ms, uint32_t total_ms)
{
	if (total_ms == 0)
		return 1.f;
	const double f = double(elapsed_ms) / double(total_ms);
	return f >= 1. ? 1.f : float(f);
}

// ---------------------------------------------------------------- messages (the op is written, the reader has read it)

struct BeginMsg
{
	uint16_t target = 0xffff, seq = 0, item = 0xffff, held_ms = 0;
	void write(Writer& w) const
	{
		w.u8(OpBegin);
		w.u16(target);
		w.u16(seq);
		w.u16(item);
		w.u16(held_ms);
	}
	bool read(Reader& r)
	{
		target = r.u16();
		seq = r.u16();
		item = r.u16();
		held_ms = r.u16();
		return r.ok();
	}
};

struct CancelMsg
{
	uint16_t target = 0xffff, seq = 0;
	void write(Writer& w) const
	{
		w.u8(OpCancel);
		w.u16(target);
		w.u16(seq);
	}
	bool read(Reader& r)
	{
		target = r.u16();
		seq = r.u16();
		return r.ok();
	}
};

struct RevivingMsg
{
	uint16_t target = 0xffff, seq = 0, reviver = 0xffff;
	uint32_t reviver_peer = 0xffffffffu, total_ms = kHoldMs, held_ms = 0;
	void write(Writer& w) const
	{
		w.u8(OpReviving);
		w.u16(target);
		w.u16(seq);
		w.u16(reviver);
		w.u32(reviver_peer);
		w.u32(total_ms);
		w.u32(held_ms);
	}
	bool read(Reader& r)
	{
		target = r.u16();
		seq = r.u16();
		reviver = r.u16();
		reviver_peer = r.u32();
		total_ms = r.u32();
		held_ms = r.u32();
		return r.ok();
	}
};

struct StoppedMsg
{
	uint16_t target = 0xffff, seq = 0, reviver = 0xffff;
	void write(Writer& w) const
	{
		w.u8(OpStopped);
		w.u16(target);
		w.u16(seq);
		w.u16(reviver);
	}
	bool read(Reader& r)
	{
		target = r.u16();
		seq = r.u16();
		reviver = r.u16();
		return r.ok();
	}
};

struct DoneMsg
{
	uint16_t target = 0xffff, seq = 0;
	void write(Writer& w) const
	{
		w.u8(OpDone);
		w.u16(target);
		w.u16(seq);
	}
	bool read(Reader& r)
	{
		target = r.u16();
		seq = r.u16();
		return r.ok();
	}
};

// ---------------------------------------------------------------- the host's verdict on a Begin or a Request

// What the host found out; the defaults are "all good". Gathering the facts needs the world (coop_survival.cpp).
struct Facts
{
	bool self = false; // the reviver is the wounded
	bool target_wounded = true; // the target is wounded on the fall the reviver names
	bool reviver_down = false; // the reviver is down himself
	bool busy = false; // a revive of this fall is in order already, or another reviver holds him
	bool in_range = true; // both are in the world and within the host's range
	bool has_item = true; // the item is a medkit / stimpack the reviver carries
};

// The first thing that fails, in the order of F3 Task 8's checks (self, wounded, down, busy, range, item).
inline Refusal verdict(const Facts& f)
{
	if (f.self)
		return RefSelf;
	if (!f.target_wounded)
		return RefNotWounded;
	if (f.reviver_down)
		return RefYouAreDown;
	if (f.busy)
		return RefBusy;
	if (!f.in_range)
		return RefTooFar;
	if (!f.has_item)
		return RefNoMedkit;
	return RefNone;
}

// ---------------------------------------------------------------- the host's hold on one wounded character

// The one reviver the host lets hold this character, on the fall `seq`. Kept per character (coop_survival HostRec).
//
// The lock is a courtesy to the honest, never a way to keep a wounded from his helpers (R5 I-1: a client that sent a Begin
// every 4 s, and never a Request or a Cancel, held the lock for good and the wounded bled out). So:
//  - A Begin opens the lock for the rest of the hold and the grace (`kHoldMs - held_ms + kGraceMs`). The same reviver may
//    begin again inside his CHAIN (a Cancel got lost, he pressed again), but the lock never runs past the end of the chain:
//    its first Begin + kHoldMs + kGraceMs (the ceiling).
//  - A Cancel closes the lock, not the chain (round 2 of I-1: a Cancel sent right before each Begin kept the lock shut 99 %
//    of the time). A Begin of the same reviver that comes less than kAnnounceGapMs after the lock was last his is still the
//    same chain, with the same ceiling. Once the lock has been free of him for a whole kAnnounceGapMs the others have had a
//    window, and the next Begin starts a new chain; so does a revive (close) and a reviver who left (drop_reviver).
//  - A chain that is used up (the ceiling is reached, or its lock ran out by itself without a Revive / Cancel) locks its
//    reviver out of a new Begin until the lock has been free of him for kAnnounceGapMs, counted from the end of the lock or
//    from his Cancel. Anybody else may take the lock in that window, and if somebody does, it holds against him.
//  - One reviver's Begins on one wounded come at least kHostBeginGapMs apart (the announcer sends them kAnnounceGapMs apart;
//    the difference is for the net's jitter), Cancels or not: a closer one is Busy and changes nothing.
// What it costs an honest player: one who lets go and presses again over and over for 4.5 s without ever finishing is turned
// away once, and has to press again two seconds on. A hold that completes (the Request) ends the chain and costs nothing.
// A Request is judged by `busy_for` as a Begin is (coop_survival): another reviver's lock turns it away.
class HostHold
{
public:
	enum class Begin : uint8_t
	{
		Started, // tell the wounded's owner (Reviving)
		Busy // another reviver holds him, or this one is too fast / locked out: refuse
	};

	// `reviver` held USE for `held_ms` when his Begin came. Somebody else is Busy until the first one's hold has ended (length
	// + grace) or been stopped; the same reviver is Busy when he is too fast or just timed out (see above), else Started.
	Begin begin(uint64_t now, uint16_t seq, uint16_t reviver, uint32_t held_ms, uint32_t total_ms = kHoldMs)
	{
		if (busy_for(now, seq, reviver))
			return Begin::Busy;
		const bool again = m_began && m_seq == seq && m_reviver == reviver; // his Begin was the last one taken on this fall
		bool in_chain = false;
		if (again)
		{
			if (now < m_last + kHostBeginGapMs)
				return Begin::Busy; // faster than an honest client sends
			// the last moment the lock was his: now (it is open), where it ran out, or his Cancel
			const uint64_t rest = m_open ? m_end : m_rest;
			if (now < rest + kAnnounceGapMs) // the lock has not been free of him for a whole gap: the others had no window
			{
				const bool ran_out = m_open && now >= m_end;
				if (ran_out || now >= m_first + total_ms + kGraceMs)
					return Begin::Busy; // his chain is used up: the others first
				in_chain = true;
			}
		}
		if (!in_chain)
			m_first = now; // a new chain
		const uint64_t want = now + (total_ms > held_ms ? total_ms - held_ms : 0) + kGraceMs;
		const uint64_t cap = m_first + total_ms + kGraceMs;
		m_began = m_open = true;
		m_seq = seq;
		m_reviver = reviver;
		m_last = now;
		m_end = want < cap ? want : cap;
		return Begin::Started;
	}
	// Somebody other than `reviver` holds him on this fall.
	bool busy_for(uint64_t now, uint16_t seq, uint16_t reviver) const { return open_at(now, seq) && m_reviver != reviver; }
	// A hold of this fall is open (and has not lapsed).
	bool open_at(uint64_t now, uint16_t seq) const { return m_open && m_seq == seq && now < m_end; }
	// `reviver`'s hold of this fall ends at `now` (Cancel, or his Request was refused): true if it was open, so the wounded is
	// told. The lock is closed, his chain is not (see above): the time of the Cancel is what the next Begin is measured from.
	bool stop(uint64_t now, uint16_t seq, uint16_t reviver)
	{
		if (!m_open || m_seq != seq || m_reviver != reviver)
			return false;
		m_open = false;
		m_rest = now < m_end ? now : m_end; // a Cancel that comes after the lock ran out changes nothing
		return true;
	}
	// The reviver went away (left the session): his chain is over; true if the lock was his, so the wounded is told (`seq()`
	// is its fall).
	bool drop_reviver(uint16_t reviver)
	{
		if (!m_began || m_reviver != reviver)
			return false;
		const bool was_open = m_open;
		m_open = m_began = false;
		return was_open;
	}
	// The fall of the last hold taken (what the Stopped for a dropped lock names).
	uint16_t seq() const { return m_seq; }
	// The revive went through, or he is no longer wounded: nothing is held, no chain goes on.
	void close() { m_open = m_began = false; }

private:
	bool m_open = false;
	bool m_began = false; // a chain goes on: m_seq / m_reviver / m_first / m_last mean something (they outlive the lock)
	uint16_t m_seq = 0, m_reviver = 0xffff;
	uint64_t m_end = 0; // the lock runs until here (never past m_first + the hold + kGraceMs)
	uint64_t m_first = 0; // the first Begin of the chain the lock belongs to
	uint64_t m_last = 0; // the last Begin taken (the host's pace)
	uint64_t m_rest = 0; // where the lock last was his when a Cancel closed it
};

// ---------------------------------------------------------------- the hold as the wounded's owner shows it

// What the host's Reviving says, run on the owner's clock: the ring of the reviver's colour fills from `held_ms` to
// `total_ms` in real time, stays full for kGraceMs waiting for the Revive, then goes.
class LiftView
{
public:
	// False for a nonsense length (nothing is shown). A newer Reviving replaces an older one.
	bool on_reviving(uint64_t now, uint16_t seq, uint16_t reviver, uint32_t peer, uint32_t total_ms, uint32_t held_ms)
	{
		if (total_ms == 0 || total_ms > kMaxHoldMs)
			return false;
		m_on = true;
		m_seq = seq;
		m_reviver = reviver;
		m_peer = peer;
		m_total = total_ms;
		m_start = int64_t(now) - int64_t(held_ms > total_ms ? total_ms : held_ms);
		return true;
	}
	// His Stopped: only the reviver's own, for this fall.
	void on_stopped(uint16_t seq, uint16_t reviver)
	{
		if (m_on && m_seq == seq && m_reviver == reviver)
			m_on = false;
	}
	void clear() { m_on = false; }

	// The ring is on for the fall `seq` (mine) at `now`.
	bool active(uint64_t now, uint16_t seq) const
	{
		return m_on && m_seq == seq && int64_t(now) < m_start + int64_t(m_total) + int64_t(kGraceMs);
	}
	float fraction(uint64_t now) const
	{
		if (!m_on)
			return 0.f;
		const int64_t elapsed = int64_t(now) - m_start;
		return revive::fraction(elapsed < 0 ? 0 : uint64_t(elapsed), m_total);
	}
	uint32_t ms_left(uint64_t now) const
	{
		if (!m_on)
			return 0;
		const int64_t left = m_start + int64_t(m_total) - int64_t(now);
		return left > 0 ? uint32_t(left) : 0;
	}
	uint16_t reviver() const { return m_reviver; }
	uint32_t peer() const { return m_peer; }

private:
	bool m_on = false;
	uint16_t m_seq = 0, m_reviver = 0xffff;
	uint32_t m_peer = 0xffffffffu, m_total = kHoldMs;
	int64_t m_start = 0;
};

// ---------------------------------------------------------------- the reviver's announcing machine

// Decides when the reviver's machine sends Begin and Cancel (see the rate paragraph of the header). It sends nothing itself.
class Announcer
{
public:
	enum class Act : uint8_t
	{
		None,
		Begin, // send a Begin with the length USE was held
		Cancel // send a Cancel
	};

	// USE went down on a wounded teammate.
	void start()
	{
		m_active = true;
		m_announced = false;
	}
	// Every frame of the hold; `held_ms` is how long USE has been held.
	Act step(uint64_t now, uint32_t held_ms)
	{
		if (!m_active || m_announced || held_ms < kAnnounceAfterMs)
			return Act::None;
		if (m_ever && now < m_last + kAnnounceGapMs)
			return Act::None; // announced late, in the middle of the hold, once the gap is over
		m_announced = m_ever = true;
		m_last = now;
		return Act::Begin;
	}
	// The hold ended early (let go, too far, fell): a Cancel only if the host was told of it.
	Act stop()
	{
		const bool was = m_active && m_announced;
		m_active = m_announced = false;
		return was ? Act::Cancel : Act::None;
	}
	// The hold ran its time: the Request replaces the Cancel (the host ends the hold with it).
	void done() { m_active = m_announced = false; }
	// A new actor: nothing of the old one counts.
	void reset() { *this = Announcer(); }

	bool announced() const { return m_announced; }

private:
	bool m_active = false, m_announced = false, m_ever = false;
	uint64_t m_last = 0;
};
} // namespace revive
} // namespace coop
