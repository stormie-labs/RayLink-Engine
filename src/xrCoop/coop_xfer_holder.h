// Anomaly Coop - F3a split levels: ownership transfer, the map server's side (plan 2026-10-24-f3a §1.7; the protocol
// and its rulings are in coop_xfer.h).
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include "coop_xfer_base.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace coop
{
namespace xfer
{
// A map server's side: what it owns, asks for, gives back; it repeats until answered. The engine creates the object on
// a Create event, deletes it on Remove, freezes it while frozen(); Refused ends a request (no such object, or aborted).
class Holder
{
public:
	struct Event
	{
		enum class Kind : uint8_t
		{
			Create, // a grant accepted: make the object from `state`
			Remove, // delete the object (released, aborted, dropped)
			Refused // a request ended without an object
		};
		Kind kind;
		uint16_t id;
		std::vector<uint8_t> state;
	};

	explicit Holder(Peer self) : m_self(self) {}

	// A player came near: the object should be online here.
	std::vector<Packet> want_online(uint16_t id, uint64_t now)
	{
		auto it = m_objs.find(id);
		if (it != m_objs.end())
		{
			if (it->second.st == St::Releasing || it->second.st == St::Committing)
				it->second.again = true; // taken again once the release is through
			return {};
		}
		Obj o;
		o.st = St::Requesting;
		arm(o, now);
		m_objs.emplace(id, std::move(o));
		return {out(Op::TakeReq, id, 0)};
	}

	// The object should go back to the world with `state` (frozen from now on). For a request not answered yet: the
	// request is taken back (a grant that still comes is declined); for a spawn not answered yet: released right after.
	std::vector<Packet> want_offline(uint16_t id, std::vector<uint8_t> state, uint64_t now)
	{
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Requesting:
			m_objs.erase(it);
			return {};
		case St::Spawning:
			if (!o.release_after) // frozen from the first call on: its state stays
			{
				o.release_after = true;
				o.state = std::move(state);
			}
			return {};
		case St::Held:
			o.st = St::Releasing;
			o.state = std::move(state);
			arm(o, now);
			return {out(Op::ReleasePrepare, id, o.gen, o.state)};
		case St::Releasing:
		case St::Committing:
			o.again = false;
			return {};
		case St::Destroying:
			return {};
		}
		return {};
	}

	// The map server made an object under a leased id.
	std::vector<Packet> spawned(uint16_t id, std::vector<uint8_t> state, uint64_t now)
	{
		if (m_objs.count(id))
			return {};
		Obj o;
		o.st = St::Spawning;
		o.state = std::move(state);
		arm(o, now);
		auto& ref = m_objs.emplace(id, std::move(o)).first->second;
		return {out(Op::SpawnPrepare, id, 0, ref.state)};
	}

	// The engine destroyed the object (it is gone here already).
	std::vector<Packet> destroyed(uint16_t id, uint64_t now)
	{
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		if (o.st != St::Held && !(o.st == St::Spawning && !o.release_after))
			return {}; // frozen for a release, deleted already, or only asked for
		o.st = St::Destroying; // a spawn not answered yet: gen 0 (R4)
		o.state.clear();
		arm(o, now);
		return {out(Op::Destroy, id, o.gen)};
	}

	// The state changed within ownership: streamed to the world (repeated until acknowledged).
	std::vector<Packet> changed(uint16_t id, std::vector<uint8_t> state, uint64_t now)
	{
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		if (o.st != St::Held && !(o.st == St::Spawning && !o.release_after))
			return {}; // frozen or not here
		o.state = std::move(state);
		++o.seq;
		if (o.st != St::Held || !o.committed)
			return {}; // sent once the world has answered
		arm(o, now);
		return {out(Op::Snapshot, id, o.gen, snapshot_payload(o.seq, o.state))};
	}

	std::vector<Packet> on_packet(const Packet& p, uint64_t now)
	{
		if (p.to != m_self || p.from != kWorld)
			return {};
		switch (p.op)
		{
		case Op::Grant: return on_grant(p, now);
		case Op::Done: return on_done(p, now);
		case Op::Ack: return on_ack(p, now);
		case Op::Abort: return on_abort(p, now);
		case Op::Drop: on_drop(p); return {};
		case Op::Snapshot: return on_snapshot_ack(p, now);
		default: return {};
		}
	}

	std::vector<Packet> tick(uint64_t now)
	{
		std::vector<Packet> v;
		for (auto& kv : m_objs)
		{
			Obj& o = kv.second;
			const uint16_t id = kv.first;
			if (now < o.next_retry)
				continue;
			switch (o.st)
			{
			case St::Requesting: v.push_back(out(Op::TakeReq, id, 0)); break;
			case St::Spawning: v.push_back(out(Op::SpawnPrepare, id, 0, o.state)); break;
			case St::Held:
				if (!o.committed)
					v.push_back(out(Op::TakeCommit, id, o.gen));
				else if (o.acked < o.seq)
					v.push_back(out(Op::Snapshot, id, o.gen, snapshot_payload(o.seq, o.state)));
				else
				{
					o.next_retry = kNever;
					continue;
				}
				break;
			case St::Releasing: v.push_back(out(Op::ReleasePrepare, id, o.gen, o.state)); break;
			case St::Committing: v.push_back(out(Op::ReleaseCommit, id, o.gen)); break;
			case St::Destroying: v.push_back(out(Op::Destroy, id, o.gen)); break;
			}
			++o.tries;
			++m_retries;
			o.next_retry = now + detail::backoff(o.tries);
		}
		return v;
	}

	// Writes the object: it exists here and nobody else may run it (frozen ones included, until the world's Ack).
	bool authors(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && (o->st == St::Held || o->st == St::Releasing || o->st == St::Spawning);
	}
	// Frozen for its release: AI and physics stop, requests on it are refused (Reason::Moving).
	bool frozen(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && (o->st == St::Releasing || (o->st == St::Spawning && o->release_after));
	}
	// Something is still going on for the id (a request, a transfer, a destroy, an unacknowledged change).
	bool busy(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && !settled(*o);
	}
	bool idle() const
	{
		for (const auto& kv : m_objs)
			if (!settled(kv.second))
				return false;
		return true;
	}
	bool requesting(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && o->st == St::Requesting;
	}
	std::vector<uint16_t> ids() const
	{
		std::vector<uint16_t> v;
		v.reserve(m_objs.size());
		for (const auto& kv : m_objs)
			v.push_back(kv.first);
		return v;
	}
	const std::vector<uint8_t>* state(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && authors(id) ? &o->state : nullptr;
	}
	// The gen it holds the object under (0: not held, or a spawn not answered yet).
	uint32_t gen(uint16_t id) const
	{
		const Obj* o = find(id);
		return o && authors(id) ? o->gen : 0;
	}
	// Everything it writes, for the auditor and the level hash (parent and alive are the engine's to fill).
	std::vector<Entry> report() const
	{
		std::vector<Entry> v;
		for (const auto& kv : m_objs)
			if (authors(kv.first))
				v.push_back(Entry{kv.first, kv.second.gen, kNoParent, true, digest(kv.second.state)});
		return v;
	}
	std::vector<Event> take_events()
	{
		std::vector<Event> e;
		e.swap(m_events);
		return e;
	}
	Peer self() const { return m_self; }
	// Its own repeats (TakeReq, TakeCommit, Snapshot, ReleasePrepare, ReleaseCommit, SpawnPrepare, Destroy).
	uint64_t retries() const { return m_retries; }

private:
	enum class St : uint8_t
	{
		Requesting, // TakeReq until Grant
		Spawning, // SpawnPrepare until Ack (writes it)
		Held, // writes it; TakeCommit until Done, then the snapshot stream
		Releasing, // frozen, ReleasePrepare until Ack (still writes it)
		Committing, // deleted, ReleaseCommit until Done
		Destroying // deleted, Destroy until Done
	};
	struct Obj
	{
		St st = St::Requesting;
		uint32_t gen = 0; // Held/Releasing/Destroying: the gen held; Committing: the release's gen; Spawning: 0
		bool committed = false; // Held: the world answered the take (Done) or the spawn (Ack)
		bool release_after = false; // Spawning: give it back as soon as the spawn is answered
		bool again = false; // Releasing/Committing: ask for it again once the release is through
		std::vector<uint8_t> state;
		uint32_t seq = 0, acked = 0; // the snapshot stream
		uint64_t next_retry = 0;
		uint32_t tries = 0;
	};
	static constexpr uint64_t kNever = ~uint64_t(0);

	Peer m_self;
	std::map<uint16_t, Obj> m_objs;
	std::map<uint16_t, uint32_t> m_tomb; // per id: the newest gen finished here, never made again
	// Per id: the newest gen of a grant it answered AbortAck - "I do not hold (id, gen) and never will". Kept apart
	// from m_tomb: it bars grants, never the Ack of its own spawn or the Done of its own destroy (review C1).
	std::map<uint16_t, uint32_t> m_declined;
	uint64_t m_retries = 0;
	std::vector<Event> m_events;

	const Obj* find(uint16_t id) const
	{
		auto it = m_objs.find(id);
		return it == m_objs.end() ? nullptr : &it->second;
	}
	static bool settled(const Obj& o) { return o.st == St::Held && o.committed && o.acked >= o.seq; }
	uint32_t tomb(uint16_t id) const
	{
		auto it = m_tomb.find(id);
		return it == m_tomb.end() ? 0 : it->second;
	}
	void bury(uint16_t id, uint32_t gen)
	{
		uint32_t& t = m_tomb[id];
		t = std::max(t, gen);
	}
	// It promises never to hold (id, gen): every AbortAck goes through here (review C1).
	Packet decline(uint16_t id, uint32_t gen)
	{
		uint32_t& d = m_declined[id];
		d = std::max(d, gen);
		return out(Op::AbortAck, id, gen);
	}
	// No grant up to this gen is ever made here: incarnations finished here and grants declined.
	uint32_t barred(uint16_t id) const
	{
		auto d = m_declined.find(id);
		return std::max(tomb(id), d == m_declined.end() ? 0u : d->second);
	}
	static void arm(Obj& o, uint64_t now)
	{
		o.tries = 0;
		o.next_retry = now + detail::backoff(0);
	}
	Packet out(Op op, uint16_t id, uint32_t gen, std::vector<uint8_t> payload = {}) const
	{
		return detail::packet(op, id, gen, m_self, kWorld, std::move(payload));
	}

	std::vector<Packet> on_grant(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		if (x == 0)
			return {};
		auto it = m_objs.find(id);
		if (it != m_objs.end())
		{
			Obj& o = it->second;
			switch (o.st)
			{
			case St::Held:
				if (x == o.gen)
					return {out(Op::TakeCommit, id, x)}; // the grant repeated: the commit repeated
				if (x > o.gen && x > barred(id))
				{
					// The world grants a newer incarnation: it holds that this one is not here. Follow the world (R2).
					bury(id, o.gen);
					m_events.push_back(Event{Event::Kind::Remove, id, {}});
					o.gen = x;
					o.committed = false;
					o.state = p.payload;
					o.seq = o.acked = 0;
					arm(o, now);
					m_events.push_back(Event{Event::Kind::Create, id, o.state});
					return {out(Op::TakeCommit, id, x)};
				}
				return {}; // an older grant, or one it promised never to hold: it keeps its own (R2)
			case St::Spawning:
			case St::Releasing:
				return {}; // writes an incarnation of it: never declined (R2); the world's abort will come
			case St::Requesting:
				if (x <= barred(id))
					break; // a gen it finished or declined: declined again, never made (review C1)
				o.st = St::Held;
				o.gen = x;
				o.committed = false;
				o.state = p.payload;
				o.seq = o.acked = 0;
				arm(o, now);
				m_events.push_back(Event{Event::Kind::Create, id, o.state});
				return {out(Op::TakeCommit, id, x)};
			case St::Committing:
			case St::Destroying:
				break; // deleted here
			}
		}
		// Not wanted, deleted here, or barred: declined - it will never hold this gen (R2).
		return {decline(id, x)};
	}

	std::vector<Packet> on_done(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Held:
			if (x == o.gen && !o.committed)
			{
				o.committed = true;
				if (o.acked < o.seq)
				{
					arm(o, now);
					return {out(Op::Snapshot, id, o.gen, snapshot_payload(o.seq, o.state))};
				}
			}
			return {};
		case St::Committing:
			if (x == o.gen)
			{
				const bool again = o.again;
				bury(id, x);
				m_objs.erase(it);
				if (again)
					return want_online(id, now);
			}
			return {};
		case St::Destroying:
			if (x > std::max(o.gen, tomb(id)))
			{
				bury(id, x);
				m_objs.erase(it);
			}
			return {};
		default:
			return {};
		}
	}

	std::vector<Packet> on_ack(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return {};
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Spawning:
			if (x <= tomb(id))
				return {};
			o.st = St::Held;
			o.gen = x;
			o.committed = true;
			o.acked = 0;
			if (o.release_after)
			{
				o.st = St::Releasing;
				arm(o, now);
				return {out(Op::ReleasePrepare, id, x, o.state)};
			}
			if (o.seq)
			{
				arm(o, now);
				return {out(Op::Snapshot, id, x, snapshot_payload(o.seq, o.state))};
			}
			return {};
		case St::Releasing:
			if (x <= o.gen)
				return {};
			bury(id, o.gen);
			m_events.push_back(Event{Event::Kind::Remove, id, {}});
			o.st = St::Committing;
			o.gen = x;
			o.state.clear();
			arm(o, now);
			return {out(Op::ReleaseCommit, id, x)};
		case St::Committing:
			if (x == o.gen)
				return {out(Op::ReleaseCommit, id, x)}; // the Ack repeated
			return {};
		default:
			return {};
		}
	}

	// Every answer is an AbortAck and every AbortAck is a promise kept by decline(): (id, x) is never made here later,
	// whatever state the id is in (review C1: Committing, Spawning, Destroying too).
	std::vector<Packet> on_abort(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		std::vector<Packet> v{decline(id, x)};
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return v; // nothing of it here
		Obj& o = it->second;
		switch (o.st)
		{
		case St::Requesting:
			// Refused (no such object) or the grant it never got was taken back.
			m_events.push_back(Event{Event::Kind::Refused, id, {}});
			m_objs.erase(it);
			return v;
		case St::Held:
		case St::Releasing:
		case St::Destroying:
			if (o.gen != 0 && x >= o.gen)
			{
				// The world takes back the grant this incarnation came from (never committed). The engine drops the
				// object and the request it came from ends without it (Refused); a wish to take it again stays.
				const bool again = o.st == St::Releasing && o.again;
				if (o.st != St::Destroying)
				{
					m_events.push_back(Event{Event::Kind::Remove, id, {}});
					m_events.push_back(Event{Event::Kind::Refused, id, {}});
				}
				bury(id, o.gen);
				m_objs.erase(it);
				if (again)
				{
					std::vector<Packet> ask = want_online(id, now);
					v.insert(v.end(), ask.begin(), ask.end());
				}
			}
			return v;
		case St::Spawning:
		case St::Committing:
			return v; // it holds no incarnation up to x; Committing keeps its `again`
		}
		return v;
	}

	void on_drop(const Packet& p)
	{
		const uint16_t id = p.key.id;
		const uint32_t x = p.key.gen;
		auto it = m_objs.find(id);
		if (it == m_objs.end())
			return;
		Obj& o = it->second;
		const bool writes = o.st == St::Held || o.st == St::Releasing || o.st == St::Spawning;
		if ((writes || o.st == St::Destroying) && o.gen <= x)
		{
			if (writes)
				m_events.push_back(Event{Event::Kind::Remove, id, {}});
			bury(id, o.gen);
			m_objs.erase(it);
		}
	}

	std::vector<Packet> on_snapshot_ack(const Packet& p, uint64_t now)
	{
		const uint16_t id = p.key.id;
		auto it = m_objs.find(id);
		if (it == m_objs.end() || it->second.st != St::Held || it->second.gen != p.key.gen)
			return {};
		Obj& o = it->second;
		uint32_t seq = 0;
		std::vector<uint8_t> unused;
		if (!read_snapshot(p.payload, seq, unused))
			return {};
		o.committed = true; // the world takes snapshots from the owner only
		if (seq == 0)
		{
			// The world asks for the state (a resync).
			if (!o.seq)
				o.seq = 1;
			o.acked = 0;
			arm(o, now);
			return {out(Op::Snapshot, id, o.gen, snapshot_payload(o.seq, o.state))};
		}
		o.acked = std::max(o.acked, std::min(seq, o.seq));
		return {};
	}
};
} // namespace xfer
} // namespace coop
