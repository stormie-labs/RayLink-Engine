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
// a second one is refused as busy until the first one ends or his lock (the hold's length and kGraceMs) runs out. The
// wounded's owner animates the ring from `held_ms` / `total_ms` on his own clock; a hold whose Stopped never came (reviver
// gone) lapses kGraceMs after its end. No Reviving / Stopped reaches anybody but the wounded.
// Rate: the reviver announces a hold only after kAnnounceAfterMs of USE (a tap sends nothing), at most one Begin per
// kAnnounceGapMs, and a Cancel only for an announced hold, so Begin + Cancel (or Begin + Request) is at most a pair per
// kAnnounceGapMs, within the PlayerRevive row of coop_limits.h (tests/test_revive_hold.cpp mashes the button against it).
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
class HostHold
{
public:
	enum class Begin : uint8_t
	{
		Started, // tell the wounded's owner (Reviving)
		Busy // another reviver holds him: refuse
	};

	// `reviver` held USE for `held_ms` when his Begin came. The same reviver again restarts it (a Cancel got lost, he pressed
	// again); somebody else is Busy until the first one's hold has ended (length + grace) or been stopped.
	Begin begin(uint64_t now, uint16_t seq, uint16_t reviver, uint32_t held_ms, uint32_t total_ms = kHoldMs)
	{
		if (busy_for(now, seq, reviver))
			return Begin::Busy;
		m_open = true;
		m_seq = seq;
		m_reviver = reviver;
		m_end = now + (total_ms > held_ms ? total_ms - held_ms : 0) + kGraceMs;
		return Begin::Started;
	}
	// Somebody other than `reviver` holds him on this fall.
	bool busy_for(uint64_t now, uint16_t seq, uint16_t reviver) const { return open_at(now, seq) && m_reviver != reviver; }
	// A hold of this fall is open (and has not lapsed).
	bool open_at(uint64_t now, uint16_t seq) const { return m_open && m_seq == seq && now < m_end; }
	// `reviver`'s hold of this fall ends (Cancel, or his Request was refused): true if it was open, so the wounded is told.
	bool stop(uint16_t seq, uint16_t reviver)
	{
		if (!m_open || m_seq != seq || m_reviver != reviver)
			return false;
		m_open = false;
		return true;
	}
	// The revive went through, or he is no longer wounded: nothing is held.
	void close() { m_open = false; }

private:
	bool m_open = false;
	uint16_t m_seq = 0, m_reviver = 0xffff;
	uint64_t m_end = 0;
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
