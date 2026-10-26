// Anomaly Coop — the layout of a player's equipment: which item sits in which slot, on the belt, or (for a thing that
// would not go there by itself) in the backpack.
//
// Why it exists (live 2026-10-23, build 9, GAMMA: "all my gear came off and lay in the backpack, as if I had just
// entered the game"): a client's slots and belt live on his machine only. His character on the host is a CAI_Stalker
// proxy, which has no belt and fills its own slots in the order it took the things; when it goes offline (the player
// left, was dropped, the group changed level) ALife clears the client data of its items, and the client data is what
// holds an item's place (CInventoryItem::m_ItemCurrPlace). The player who comes back loads that save: every item with
// default_to_ruck (outfit, helmet, backpack, artefacts, detector, PDA) falls into the backpack.
//
// So the owner's machine reports the layout (tail of PlayerVitals), the host keeps it as one line in the proxy's
// custom data ([coop] gear = ...), and the returning client's items are given their place while they spawn
// (coop_equip.cpp on_item_spawn), before the engine takes them into the inventory.
//
// An entry names an item by its id (host numbering) and a tag of its section: an id that was given to another thing
// since the report (a stash, a trade) must not move that thing.
//
// Pure C++17, header-only, no engine dependencies: used by coop_equip.cpp and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop_wire.h"

namespace coop
{
namespace gear
{
// The numbers of eItemPlace (inventory_space.h) without Undefined.
enum class Place : uint8_t
{
	Slot = 1,
	Belt = 2,
	Ruck = 3
};

struct Entry
{
	uint16_t item = 0xffff; // host numbering
	uint16_t tag = 0; // tag_of(section)
	Place place = Place::Ruck;
	uint8_t slot = 0; // Place::Slot only: 1..last slot

	bool operator==(const Entry& o) const
	{
		return item == o.item && tag == o.tag && place == o.place && slot == o.slot;
	}
};

// A capture lists slots first, then the belt, then the backpack: what the cap cuts is the backpack's tail.
constexpr size_t kMaxEntries = 160;
// The slot id takes 6 bits on the wire (SInvItemPlace::slot_id).
constexpr uint8_t kMaxSlotId = 63;
constexpr uint8_t kTailVersion = 1;
constexpr size_t kTailEntryBytes = 5;

// FNV-1a over the section name, folded to 16 bits.
inline uint16_t tag_of(const char* section)
{
	uint32_t h = 2166136261u;
	for (const char* p = section; p && *p; ++p)
	{
		h ^= uint8_t(*p);
		h *= 16777619u;
	}
	return uint16_t((h >> 16) ^ (h & 0xffffu));
}

// Drops what makes no sense (no item, an unknown place, a slot that does not exist, an item or a slot named twice —
// the first one stays), clears the slot of a belt / backpack entry, cuts at kMaxEntries. Returns how many entries it
// dropped. A bad layout is cleaned, not trusted: Inventory::Slot with strict placement does not check the slot.
inline size_t sanitize(std::vector<Entry>& v, uint8_t last_slot)
{
	std::vector<Entry> kept;
	kept.reserve(v.size() < kMaxEntries ? v.size() : kMaxEntries);
	std::set<uint16_t> items;
	std::set<uint8_t> slots;
	size_t dropped = 0;
	for (Entry e : v)
	{
		const bool place_ok = e.place == Place::Slot || e.place == Place::Belt || e.place == Place::Ruck;
		bool ok = e.item != 0xffff && place_ok && items.find(e.item) == items.end();
		if (ok && e.place == Place::Slot)
			ok = e.slot >= 1 && e.slot <= last_slot && e.slot <= kMaxSlotId && slots.find(e.slot) == slots.end();
		if (ok && kept.size() >= kMaxEntries)
			ok = false;
		if (!ok)
		{
			++dropped;
			continue;
		}
		if (e.place != Place::Slot)
			e.slot = 0;
		items.insert(e.item);
		if (e.place == Place::Slot)
			slots.insert(e.slot);
		kept.push_back(e);
	}
	v.swap(kept);
	return dropped;
}

// --- on the wire: the tail of PlayerVitals (client -> host), after the six floats ---
//   u8 version, varu count, count x { u16 item, u16 tag, u8 (place << 6 | slot) }
// A sender without the tail ends after the floats: Absent, the host keeps what it has.
enum class TailRead
{
	Absent,
	Ok,
	Bad
};

inline void write_tail(Writer& w, const std::vector<Entry>& v)
{
	const size_t n = v.size() < kMaxEntries ? v.size() : kMaxEntries;
	w.u8(kTailVersion);
	w.varu(n);
	for (size_t i = 0; i < n; ++i)
	{
		w.u16(v[i].item);
		w.u16(v[i].tag);
		w.u8(uint8_t((uint8_t(v[i].place) << 6) | (v[i].slot & kMaxSlotId)));
	}
}

// Bad: nothing is taken (half a layout would move half of a player's things), `out` is empty.
inline TailRead read_tail(Reader& r, std::vector<Entry>& out, uint8_t last_slot)
{
	out.clear();
	if (r.remaining() == 0)
		return TailRead::Absent;
	const uint8_t version = r.u8();
	const uint64_t count = r.varu();
	if (!r.ok() || version != kTailVersion || count > kMaxEntries || r.remaining() < count * kTailEntryBytes)
		return TailRead::Bad;
	out.reserve(size_t(count));
	for (uint64_t i = 0; i < count; ++i)
	{
		Entry e;
		e.item = r.u16();
		e.tag = r.u16();
		const uint8_t b = r.u8();
		e.place = Place(b >> 6);
		e.slot = uint8_t(b & kMaxSlotId);
		out.push_back(e);
	}
	if (!r.ok())
	{
		out.clear();
		return TailRead::Bad;
	}
	sanitize(out, last_slot);
	return TailRead::Ok;
}

// --- in the proxy's custom data: one ini value, "8537.1c3f.S7,853b.77a0.B,853d.4e12.R" ---
//   <item hex>.<tag hex>.<S<slot> | B | R>   (no ';' and no '=': both mean something in an ini line)
inline std::string to_text(const std::vector<Entry>& v)
{
	static const char kHex[] = "0123456789abcdef";
	const auto hex = [](std::string& s, uint16_t x)
	{
		bool started = false;
		for (int shift = 12; shift >= 0; shift -= 4)
		{
			const unsigned d = (x >> shift) & 0xfu;
			if (d || started || shift == 0)
			{
				s += kHex[d];
				started = true;
			}
		}
	};
	std::string s;
	const size_t n = v.size() < kMaxEntries ? v.size() : kMaxEntries;
	for (size_t i = 0; i < n; ++i)
	{
		if (!s.empty())
			s += ',';
		hex(s, v[i].item);
		s += '.';
		hex(s, v[i].tag);
		s += '.';
		switch (v[i].place)
		{
		case Place::Slot: s += 'S'; s += std::to_string(unsigned(v[i].slot)); break;
		case Place::Belt: s += 'B'; break;
		default: s += 'R'; break;
		}
	}
	return s;
}

namespace detail
{
// 1..4 hex digits.
inline bool parse_hex16(const std::string& s, uint16_t& out)
{
	if (s.empty() || s.size() > 4)
		return false;
	unsigned v = 0;
	for (char c : s)
	{
		unsigned d;
		if (c >= '0' && c <= '9')
			d = unsigned(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = unsigned(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = unsigned(c - 'A' + 10);
		else
			return false;
		v = v * 16 + d;
	}
	out = uint16_t(v);
	return true;
}

inline bool parse_entry(const std::string& tok, Entry& e)
{
	const size_t a = tok.find('.');
	const size_t b = a == std::string::npos ? a : tok.find('.', a + 1);
	if (b == std::string::npos || tok.find('.', b + 1) != std::string::npos)
		return false;
	if (!parse_hex16(tok.substr(0, a), e.item) || !parse_hex16(tok.substr(a + 1, b - a - 1), e.tag))
		return false;
	const std::string where = tok.substr(b + 1);
	if (where.empty())
		return false;
	switch (where[0])
	{
	case 'S':
	{
		if (where.size() < 2 || where.size() > 3)
			return false;
		unsigned slot = 0;
		for (size_t i = 1; i < where.size(); ++i)
		{
			if (where[i] < '0' || where[i] > '9')
				return false;
			slot = slot * 10 + unsigned(where[i] - '0');
		}
		e.place = Place::Slot;
		e.slot = uint8_t(slot);
		return true;
	}
	case 'B':
		e.place = Place::Belt;
		e.slot = 0;
		return where.size() == 1;
	case 'R':
		e.place = Place::Ruck;
		e.slot = 0;
		return where.size() == 1;
	default: return false;
	}
}
} // namespace detail

// Tolerant: a token that does not read is skipped, the rest is kept (a custom data line is edited by hand now and then).
// `out` is replaced. Returns how many tokens (or entries after sanitize) were refused.
inline size_t from_text(const std::string& text, std::vector<Entry>& out, uint8_t last_slot)
{
	out.clear();
	size_t bad = 0;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t end = text.find(',', pos);
		if (end == std::string::npos)
			end = text.size();
		size_t b = pos, e = end;
		while (b < e && (text[b] == ' ' || text[b] == '\t'))
			++b;
		while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r'))
			--e;
		if (e > b)
		{
			Entry entry;
			if (out.size() < kMaxEntries * 2 && detail::parse_entry(text.substr(b, e - b), entry))
				out.push_back(entry);
			else
				++bad;
		}
		pos = end + 1;
	}
	return bad + sanitize(out, last_slot);
}

// The returning client's lookup while its items spawn: item id -> the place it had.
class Table
{
public:
	void load(const std::vector<Entry>& v)
	{
		m_by_item.clear();
		for (const Entry& e : v)
			m_by_item.emplace(e.item, e);
	}
	void clear() { m_by_item.clear(); }
	size_t size() const { return m_by_item.size(); }
	// nullptr: not in the layout, or the id belongs to another section now.
	const Entry* find(uint16_t item, uint16_t tag) const
	{
		const auto it = m_by_item.find(item);
		return it != m_by_item.end() && it->second.tag == tag ? &it->second : nullptr;
	}

private:
	std::map<uint16_t, Entry> m_by_item;
};
} // namespace gear
} // namespace coop
