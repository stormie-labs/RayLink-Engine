// Anomaly Coop — the pure part of the host checkpoint store (F3 Task 6, CHECKPOINTS_DEATH.md §2): the CheckpointState
// message, the player's slot in his character's custom data, the arrival spots of a group level change and the
// client's mirror of the records the host shares. The engine side is xrGame/coop/coop_checkpoints.cpp.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "coop_checkpoint_rules.h"
#include "coop_wire.h"

namespace coop
{
namespace checkpoint
{
// Characters with a checkpoint that one machine keeps: the host's store and a client's mirror. A world has one player
// character per player key that ever joined it (at most coop_max_new_characters new ones per session, F2-10 b);
// what is over the bound is refused and logged, never grown.
constexpr size_t kMaxSlots = 256;

// ---------------------------------------------------------------- Msg::CheckpointState

// Flags of CheckpointState: u16 character (host numbering), u8 flags, then the record unless kStateCleared.
constexpr uint8_t kStateRollback = 1; // the source of the camp was lost: this is the previous one (or none)
constexpr uint8_t kStateCleared = 2;  // the player has no checkpoint; no record follows (kind None is never sent)

struct StateMsg
{
	uint16_t character = 0xffff;
	uint8_t flags = 0;
	Record record; // valid unless cleared()
	bool rollback() const { return (flags & kStateRollback) != 0; }
	bool cleared() const { return (flags & kStateCleared) != 0; }
};

// The current record of a slot as the host sends it: a record that is not valid goes as "cleared".
inline void write_state(Writer& w, uint16_t character, bool rollback, const Record& cur)
{
	const bool has = cur.valid();
	w.u16(character);
	w.u8(uint8_t((rollback ? kStateRollback : 0) | (has ? 0 : kStateCleared)));
	if (has)
		write(w, cur);
}

// Strict: a known character id, only the two flag bits, a valid record exactly when not cleared and nothing after it.
// On false `out` is left as it was.
inline bool read_state(Reader& rd, StateMsg& out)
{
	StateMsg m;
	m.character = rd.u16();
	m.flags = rd.u8();
	if (!rd.ok() || m.character == 0xffff || (m.flags & ~(kStateRollback | kStateCleared)) != 0)
		return false;
	if (!m.cleared() && !read(rd, m.record))
		return false;
	if (rd.remaining() != 0)
		return false;
	out = m;
	return true;
}

// ---------------------------------------------------------------- custom data
// Until the player scope of F2 has a C++ API, a player's slot lives in the custom data of his character (the host's
// actor, a client's proxy): section [coop_checkpoint], keys `cur` and `prev` (encode()), and on the host's actor also
// `arrival`. Custom data goes into the host's save with the character (CSE_ALifeObject::STATE_Write), so a load gets
// the checkpoints of that save and nothing else.
constexpr const char* kIniSection = "coop_checkpoint";
constexpr const char* kIniCur = "cur";
constexpr const char* kIniPrev = "prev";
constexpr const char* kIniArrival = "arrival";

namespace detail
{
inline void record_to_ini(std::string& text, const char* key, const Record& r)
{
	const std::string line = encode(r);
	if (line.empty())
		ini_erase(text, kIniSection, key);
	else
		ini_set(text, kIniSection, key, line);
}

// True with the record of `key`; false if there is none or it does not decode (then `corrupt` is set).
inline bool record_from_ini(const std::string& text, const char* key, Record& out, bool& corrupt)
{
	corrupt = false;
	std::string line;
	if (!ini_get(text, kIniSection, key, line))
		return false;
	if (decode(line, out))
		return true;
	corrupt = true;
	return false;
}
} // namespace detail

// Writes both records of the slot; a record that is not valid removes its key. Other sections, keys and line breaks
// stay as they are.
inline void slot_to_ini(std::string& text, const Slot& s)
{
	detail::record_to_ini(text, kIniCur, s.cur);
	detail::record_to_ini(text, kIniPrev, s.prev);
}

// The slot kept in the text. A line that does not decode is no record (counted in `corrupt`). A previous record
// without a current one moves up: set() would otherwise push the empty current record over it.
inline Slot slot_from_ini(const std::string& text, unsigned* corrupt = nullptr)
{
	Slot s;
	unsigned bad = 0;
	bool c = false;
	if (!detail::record_from_ini(text, kIniCur, s.cur, c) && c)
		++bad;
	if (!detail::record_from_ini(text, kIniPrev, s.prev, c) && c)
		++bad;
	if (!s.cur.valid() && s.prev.valid())
	{
		s.cur = s.prev;
		s.prev = Record();
	}
	if (corrupt)
		*corrupt = bad;
	return s;
}

// The host's own arrival point (the last level change), kept beside his slot.
inline void arrival_to_ini(std::string& text, const Record& r) { detail::record_to_ini(text, kIniArrival, r); }

// True with the arrival point; false if there is none or it does not decode (`corrupt` tells which). `out` is left as
// it was on false.
inline bool arrival_from_ini(const std::string& text, Record& out, bool* corrupt = nullptr)
{
	bool c = false;
	Record r;
	const bool ok = detail::record_from_ini(text, kIniArrival, r, c);
	if (corrupt)
		*corrupt = c;
	if (ok)
		out = r;
	return ok;
}

// ---------------------------------------------------------------- arrival spots

// A group level change: the n-th player in the world (0 = the host) arrives 0.8 m * n along x from the host, so the
// players do not respawn inside each other. The engine falls back to the host's own spot where the AI map has no
// vertex.
constexpr float kArrivalStep = 0.8f;
inline void arrival_spot(const float host[3], unsigned n, float out[3])
{
	out[0] = host[0] + kArrivalStep * float(n);
	out[1] = host[1];
	out[2] = host[2];
}

// ---------------------------------------------------------------- visibility

// Who sees a player's checkpoint: its owner always, the others while the host shares them (coop_checkpoints_group).
// `viewer` and `owner` are characters in one numbering (the host's actor is 0 on the host).
inline bool may_see(uint16_t viewer, uint16_t owner, bool group) { return group || viewer == owner; }

// ---------------------------------------------------------------- host store

// A change the host is to announce (frame() sends the character's record as it is then).
struct Pending
{
	uint16_t character = 0xffff;
	bool rollback = false;
};

// One CheckpointState of a whole list: the character, the flag and his current record (not valid = cleared).
struct Outgoing
{
	uint16_t character = 0xffff;
	bool rollback = false;
	Record cur;
};

// A character is queued once (a later change of the same frame updates the entry); twice the store for the ones that
// just left it.
constexpr size_t kMaxPending = 2 * kMaxSlots;

// The host's cache of the slots and the queue of changes to announce. The slots live in the characters' custom data;
// the cache follows them (it is what the players were last told) and serves the whole lists. Bounded: kMaxSlots slots,
// kMaxPending changes.
class HostStore
{
public:
	// A slot just read back from where it lives. If what the players were told no longer holds (the current record
	// changed or went away: the character is gone, or its id belongs to another character now) the change is queued
	// and true returned. A slot first met here is only cached: nothing about it was announced from this cache (a
	// player who comes into the world gets it with the whole list).
	bool observe(uint16_t id, const Slot& s)
	{
		const auto it = m_slots.find(id);
		const bool told = it != m_slots.end();
		const bool differs = told && (!s.cur.valid() || encode(it->second.cur) != encode(s.cur));
		put(id, s);
		if (differs)
			queue(id, false);
		return differs;
	}

	// A change the host made (a new camp, a rollback): cached and queued. False when `id` is new and the cache is full.
	bool change(uint16_t id, const Slot& s, bool rollback)
	{
		if (!put(id, s))
			return false;
		queue(id, rollback);
		return true;
	}

	const Slot* find(uint16_t id) const
	{
		const auto it = m_slots.find(id);
		return it == m_slots.end() ? nullptr : &it->second;
	}
	const std::map<uint16_t, Slot>& slots() const { return m_slots; }

	// What frame() announces now; the queue empties.
	std::vector<Pending> take_pending()
	{
		std::vector<Pending> out;
		out.swap(m_pending);
		return out;
	}

	// The whole list for the player of character `viewer` (he came into the world, or the group view switched): the
	// records he may see; his own id as cleared when he has none (his copy may hold the camp of a character that had
	// this id before); with `clear_others` (the group view was just turned off) the others' as cleared.
	std::vector<Outgoing> view_for(uint16_t viewer, bool group, bool clear_others) const
	{
		std::vector<Outgoing> out;
		if (m_slots.find(viewer) == m_slots.end())
		{
			Outgoing own;
			own.character = viewer;
			out.push_back(own);
		}
		for (const auto& it : m_slots)
		{
			Outgoing o;
			o.character = it.first;
			if (may_see(viewer, it.first, group))
				o.cur = it.second.cur;
			else if (!clear_others)
				continue;
			out.push_back(o);
		}
		return out;
	}

	void clear_cache() { m_slots.clear(); } // a new world (queued changes still go out)
	void clear()
	{
		m_slots.clear();
		m_pending.clear();
	}
	uint64_t not_kept() const { return m_not_kept; } // a new character over kMaxSlots
	uint64_t not_announced() const { return m_not_announced; } // a change over kMaxPending

private:
	// Caches the slot (an empty one leaves the cache). False when `id` is new and kMaxSlots are kept.
	bool put(uint16_t id, const Slot& s)
	{
		if (!s.cur.valid())
		{
			m_slots.erase(id);
			return true;
		}
		const auto it = m_slots.find(id);
		if (it != m_slots.end())
		{
			it->second = s;
			return true;
		}
		if (m_slots.size() >= kMaxSlots)
		{
			++m_not_kept;
			return false;
		}
		m_slots.emplace(id, s);
		return true;
	}

	void queue(uint16_t id, bool rollback)
	{
		for (Pending& p : m_pending)
			if (p.character == id)
			{
				p.rollback = rollback; // the newest change decides what the players see
				return;
			}
		if (m_pending.size() >= kMaxPending)
		{
			++m_not_announced;
			return;
		}
		Pending p;
		p.character = id;
		p.rollback = rollback;
		m_pending.push_back(p);
	}

	std::map<uint16_t, Slot> m_slots;
	std::vector<Pending> m_pending;
	uint64_t m_not_kept = 0, m_not_announced = 0;
};

// ---------------------------------------------------------------- client mirror

// What a client knows of the checkpoints: the records the host sent, by character id (the caller picks the numbering;
// the engine keeps local ids). Its own always, the others only while the host shares them (coop_checkpoints_group).
class Mirror
{
public:
	// Applies the host's state for `id`: a record is kept, "cleared" removes it. False (nothing changes) when `id` is
	// new and kMaxSlots records are already kept.
	bool apply(uint16_t id, const StateMsg& m)
	{
		if (m.cleared() || !m.record.valid())
		{
			m_records.erase(id);
			return true;
		}
		const auto it = m_records.find(id);
		if (it != m_records.end())
		{
			it->second = m.record;
			return true;
		}
		if (m_records.size() >= kMaxSlots)
			return false;
		m_records.emplace(id, m.record);
		return true;
	}
	const Record* get(uint16_t id) const
	{
		const auto it = m_records.find(id);
		return it == m_records.end() ? nullptr : &it->second;
	}
	size_t size() const { return m_records.size(); }
	void clear() { m_records.clear(); }
	const std::map<uint16_t, Record>& records() const { return m_records; }

private:
	std::map<uint16_t, Record> m_records;
};
} // namespace checkpoint
} // namespace coop
