// The journal of a map server (Msg LevelJournal 0x01E0, LevelJournalAck 0x01E1). Every registry fact of a map server's frame
// goes into one LevelJournal on the reliable lane (the lane of LevelXferUp: a frame's facts are applied before that frame's
// hand-back), and the world server answers each batch with a LevelJournalAck. A batch is read whole before any entry is applied:
// one entry that does not read refuses the batch, so a fault never applies half a frame.
//
// An entry is {u8 op, varu len, body[len]}; `op` is a LevelEntityHead::Op:
//   per-object ops (names_object: Destroy, Parent, Died, State, Item, Spawn, Tent, Stash, Logic): body = ObjectHead {u16 id, u16
//       section crc, u32 gen}, then the LevelEntity layout of the op, whole. That layout starts with the same id (checked), so
//       the LevelEntity readers take layout_of(entry) unchanged. Fixed layouts are checked to their size: Destroy {u16 id},
//       Parent {u16 item, u16 parent, vec3 pos}, Tent {u16 tent, u16 character}, Stash {u16 character, u8 things stay}; Logic is
//       LevelLogicBody, Item is ItemFactsBody (the ItemState layout plus a source byte); Died {u16 victim, u16 killer, state},
//       State {u16 character, ...} and Spawn (the EntitySpawn layout) end in a tail of their own readers.
//   Money: the LevelEntity layout {u32 peer, u64 delta, u8 trade}, no head (a player's book is no object).
//   Joined, Left: MemberBody. Info: InfoBody. Ask: AskBody. IdAsk: no body. Carry: CarryBody. Done: DoneBody. Flushed: FlushedBody.
//   Report (6) is no journal op: it stays a LevelEntity of its own (the answer to LevelResync).
// The head's `gen` is the keeper generation of the object's root (the squad of a member, the holder chain of an item) as the
// sender holds it, whatever that root is now, 0 when the sender does not hold it: facts about a held unit carry its keeper
// generation and are refused for an older grant (same_object, coop_writer_rules.h, compares it with the world's). The hold
// decides, not the kind: the body of a held unit that died stays held (no Joined, no release ends it) and is named with the
// hold's gen on both sides; a player's character is never held, 0. One rule for both sides: levels::root_gen
// (coop_levels_root_gen.h). The head's `crc` is section_crc of the object's section (for a Spawn the new object's).
//
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../coop_hash.h"
#include "../coop_wire.h"
#include "coop_levels_wire.h"

namespace coop
{
namespace levels
{
namespace journal
{
constexpr size_t kMaxEntries = 16384; // entries of one batch (a surge frame: deaths, drops, item facts of some hundred NPCs)
constexpr size_t kMaxBody = 64 * 1024; // one entry (an EntitySpawn with its spawn data is some KiB; a carry part is at most 60 KiB)
constexpr size_t kMaxBatch = 8 * 1024 * 1024; // bytes of one batch (Session::send takes 16 MiB); a bigger frame goes as two batches
constexpr uint8_t kLastOp = wire::LevelEntityHead::Flushed;

// Why the world refused an entry (LevelJournalAck). The world's acceptance (coop_levels_accept.h) picks the reason.
enum Why : uint8_t
{
	NotWriter = 1, // the sender does not write the object (for a Spawn the new object's root, for a Parent the old or the new root)
	Identity = 2, // id, section crc or keeper generation differ from the world's object
	PastFence = 3, // the entry came after the sender's fence (its level move, a grant handed back)
	Unknown = 4, // the world has no such object
	NotOwnLevel = 5, // an ask about an object whose root does not stand on the sender's level
	IdRange = 6, // a spawn under an id that is not of the sender's slice, or that the world has under another object
	Ghost = 7, // the target is the ghost actor: nobody
	Malformed = 8, // the entry's layout does not read where the world reads it
	Refused = 9, // the world's own rule refused it (a deal, a logic, a carry conflict)
};
constexpr uint8_t kLastWhy = Refused;

// An op a journal carries: the LevelEntity ops but Report, and the journal's own.
inline bool known_op(uint8_t op) { return op <= kLastOp && op != wire::LevelEntityHead::Report; }

// The ops whose body starts with an ObjectHead.
inline bool names_object(uint8_t op)
{
	using H = wire::LevelEntityHead;
	switch (op)
	{
	case H::Destroy:
	case H::Parent:
	case H::Died:
	case H::State:
	case H::Item:
	case H::Spawn:
	case H::Tent:
	case H::Stash:
	case H::Logic: return true;
	default: break;
	}
	return false;
}

// For the logs; "?" for a value no op has.
inline const char* op_name(uint8_t op)
{
	using H = wire::LevelEntityHead;
	switch (op)
	{
	case H::Destroy: return "Destroy";
	case H::Parent: return "Parent";
	case H::Died: return "Died";
	case H::State: return "State";
	case H::Item: return "ItemFacts";
	case H::Spawn: return "Spawn";
	case H::Report: return "Report";
	case H::Money: return "Money";
	case H::Tent: return "Tent";
	case H::Stash: return "Stash";
	case H::Logic: return "Logic";
	case H::Joined: return "Joined";
	case H::Left: return "Left";
	case H::Info: return "Info";
	case H::Ask: return "Ask";
	case H::IdAsk: return "IdAsk";
	case H::Carry: return "Carry";
	case H::Done: return "Done";
	case H::Flushed: return "Flushed";
	default: break;
	}
	return "?";
}

inline const char* why_name(uint8_t why)
{
	switch (why)
	{
	case NotWriter: return "not the writer";
	case Identity: return "another object";
	case PastFence: return "past the fence";
	case Unknown: return "no such object";
	case NotOwnLevel: return "not on its level";
	case IdRange: return "id not of its slice";
	case Ghost: return "the ghost";
	case Malformed: return "malformed";
	case Refused: return "refused";
	default: break;
	}
	return "?";
}

// The u16 of an object's section in an ObjectHead and an AskBody: FNV-1a 32 of the name, its two halves xor-ed. Every process of a
// session is one exe, so one name is one value everywhere; two sections may share a value (1 in 65536): the id and the keeper
// generation are the rest of an identity.
inline uint16_t section_crc(std::string_view section) { return fold16(fnv1a32(section)); }

// The head of every per-object entry. Wire: u16 id, u16 crc, u32 gen.
struct ObjectHead
{
	static constexpr size_t kSize = 2 + 2 + 4;

	uint16_t id = wire::kNoObject;
	uint16_t crc = 0;
	uint32_t gen = 0;

	void write(Writer& w) const
	{
		w.u16(id);
		w.u16(crc);
		w.u32(gen);
	}

	bool read(Reader& r)
	{
		ObjectHead tmp;
		tmp.id = r.u16();
		tmp.crc = r.u16();
		tmp.gen = r.u32();
		if (!r.ok() || tmp.id == wire::kNoObject)
		{
			*this = ObjectHead();
			return false;
		}
		*this = tmp;
		return true;
	}
};

// One entry of a batch: its op and its body as sent (for a per-object op the head first). The world applies it in
// host_apply_fact (coop_levels_alife.h).
struct Entry
{
	uint8_t op = wire::LevelEntityHead::Destroy;
	std::vector<uint8_t> body;

	bool operator==(const Entry& o) const { return op == o.op && body == o.body; }
};

// Joined, Left: a squad and one member (stock CSE_ALifeOnlineOfflineGroup::register_member / unregister_member). Wire: u16 squad,
// u16 member.
struct MemberBody
{
	uint16_t squad = wire::kNoObject;
	uint16_t member = wire::kNoObject;

	bool fits() const { return squad != wire::kNoObject && member != wire::kNoObject && squad != member; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(squad);
		w.u16(member);
		return true;
	}

	bool read(Reader& r)
	{
		MemberBody tmp;
		tmp.squad = r.u16();
		tmp.member = r.u16();
		if (!r.ok() || !tmp.fits())
		{
			*this = MemberBody();
			return false;
		}
		*this = tmp;
		return true;
	}
};

// Info: an info portion of a non-actor object, given (on 1) or taken (on 0) by its writer. Wire: u16 id, u8 on, str info.
struct InfoBody
{
	static constexpr size_t kMaxInfo = 256;

	uint16_t id = wire::kNoObject;
	uint8_t on = 1;
	std::string info;

	bool fits() const { return id != wire::kNoObject && on <= 1 && !info.empty() && wire::detail::printable(info, kMaxInfo); }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(id);
		w.u8(on);
		w.str(info);
		return true;
	}

	bool read(Reader& r)
	{
		InfoBody tmp;
		tmp.id = r.u16();
		tmp.on = r.u8();
		tmp.info = r.str();
		if (!r.ok() || !tmp.fits())
		{
			*this = InfoBody();
			return false;
		}
		*this = std::move(tmp);
		return true;
	}
};

// Ask: what a map server's script wants of an object of its level it does not write: the world executes it when it writes the
// object and its root stands on the sender's level, and the map server does not apply it first. Wire: u8 what, u16 id, u16 crc
// (section_crc of the object as the sender has it), then str info for GiveInfo and DisableInfo alone.
struct AskBody
{
	enum What : uint8_t
	{
		Release = 1,
		GiveInfo = 2,
		DisableInfo = 3,
	};

	uint8_t what = Release;
	uint16_t id = wire::kNoObject;
	uint16_t crc = 0;
	std::string info; // GiveInfo, DisableInfo: the info portion; empty for Release

	bool names_info() const { return what == GiveInfo || what == DisableInfo; }

	bool fits() const
	{
		if (what < Release || what > DisableInfo || id == wire::kNoObject)
			return false;
		return names_info() ? !info.empty() && wire::detail::printable(info, InfoBody::kMaxInfo) : info.empty();
	}

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u8(what);
		w.u16(id);
		w.u16(crc);
		if (names_info())
			w.str(info);
		return true;
	}

	bool read(Reader& r)
	{
		AskBody tmp;
		tmp.what = r.u8();
		tmp.id = r.u16();
		tmp.crc = r.u16();
		if (r.ok() && tmp.names_info())
			tmp.info = r.str();
		if (!r.ok() || !tmp.fits())
		{
			*this = AskBody();
			return false;
		}
		*this = std::move(tmp);
		return true;
	}
};

// Carry: part `part` of `total` of the leaves a map server carries from its modules' save_state to the world save (the blob is
// coop_srv_carry.script's). Wire: u16 part, u16 total, then the blob: the rest of the body, 1 .. kMaxPart bytes.
struct CarryBody
{
	static constexpr size_t kMaxPart = 60 * 1024;

	uint16_t part = 0;
	uint16_t total = 1;
	std::vector<uint8_t> blob;

	bool fits() const { return total >= 1 && part < total && !blob.empty() && blob.size() <= kMaxPart; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(part);
		w.u16(total);
		w.bytes(blob.data(), blob.size());
		return true;
	}

	bool read(Reader& r)
	{
		*this = CarryBody();
		CarryBody tmp;
		tmp.part = r.u16();
		tmp.total = r.u16();
		if (!r.ok() || !wire::detail::read_tail(r, tmp.blob, kMaxPart) || !tmp.fits())
			return false;
		*this = std::move(tmp);
		return true;
	}
};

// Done: a numbered op that consumes or pays ran here, with the answer the executor recorded; the world keeps it with the effect
// and hands it to the next map server. `origin` and `op` are the op's number {origin character, seq} as its Lua makes it. Wire:
// u32 origin, u32 op, str answer (0 .. kMaxAnswer bytes).
struct DoneBody
{
	static constexpr size_t kMaxAnswer = 8 * 1024;

	uint32_t origin = 0;
	uint32_t op = 0;
	std::string answer;

	bool fits() const { return answer.size() <= kMaxAnswer; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(origin);
		w.u32(op);
		w.str(answer);
		return true;
	}

	bool read(Reader& r)
	{
		DoneBody tmp;
		tmp.origin = r.u32();
		tmp.op = r.u32();
		tmp.answer = r.str();
		if (!r.ok() || !tmp.fits())
		{
			*this = DoneBody();
			return false;
		}
		*this = std::move(tmp);
		return true;
	}
};

// Flushed: the answer to LevelFlush{round}: every fact before it is in the journal. Wire: u32 round (>= 1, as FlushRound's).
struct FlushedBody
{
	uint32_t round = 0;

	bool fits() const { return round != 0; }

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(round);
		return true;
	}

	bool read(Reader& r)
	{
		FlushedBody tmp;
		tmp.round = r.u32();
		if (!r.ok() || !tmp.fits())
		{
			*this = FlushedBody();
			return false;
		}
		*this = tmp;
		return true;
	}
};

// ItemFacts (op Item of a journal): the condition, uses, upgrades or addon flags of an item the map server writes changed,
// players' items included. The layout of ItemState State after its op byte, plus the source of the change. Wire: u16 item, str
// section, u8 flags, varu n, n bytes of the ItemState blob (1 .. kMaxBlob), u8 source.
struct ItemFactsBody
{
	enum Source : uint8_t
	{
		Loot = 0, // the wear its level gave loot (the LevelEntity Item)
		Decider = 1, // the writer's deliberate write (a script's set_condition, set_remaining_uses, an upgrade it installed)
		Owner = 2, // the owner's report the writer took (his own use, on the latest version of the field: coop_item_tags.h fresh)
	};
	static constexpr size_t kMaxSection = 64;
	static constexpr size_t kMaxBlob = 4096; // kMaxItemState (coop_spawn.h): the ItemState's own bound
	static constexpr uint8_t kFlagAuthority = 1; // the one flag an ItemState has (coop_items.cpp kFlagAuthority)

	uint16_t item = wire::kNoObject;
	std::string section;
	uint8_t flags = 0;
	std::vector<uint8_t> blob;
	uint8_t source = Loot;

	bool fits() const
	{
		return item != wire::kNoObject && !section.empty() && wire::detail::printable(section, kMaxSection) &&
		    (flags & ~kFlagAuthority) == 0 && !blob.empty() && blob.size() <= kMaxBlob && source <= Owner;
	}

	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u16(item);
		w.str(section);
		w.u8(flags);
		w.varu(blob.size());
		w.bytes(blob.data(), blob.size());
		w.u8(source);
		return true;
	}

	bool read(Reader& r)
	{
		*this = ItemFactsBody();
		ItemFactsBody tmp;
		tmp.item = r.u16();
		tmp.section = r.str();
		tmp.flags = r.u8();
		const uint64_t n = r.varu();
		if (!r.ok() || n == 0 || n > kMaxBlob || n > r.remaining())
			return false;
		tmp.blob.resize(size_t(n));
		if (!r.bytes(tmp.blob.data(), size_t(n)))
			return false;
		tmp.source = r.u8();
		if (!r.ok() || !tmp.fits())
			return false;
		*this = std::move(tmp);
		return true;
	}
};

// `r` reads as B with nothing left over.
template <class B>
bool reads_whole(Reader r, B& out)
{
	return out.read(r) && r.remaining() == 0;
}

// Does `body` read as the body of `op`? The batch's reader asks it of every entry before it keeps one, the writer and the builders
// before they write one.
inline bool body_ok(uint8_t op, const uint8_t* data, size_t size)
{
	using H = wire::LevelEntityHead;
	if (!known_op(op) || size > kMaxBody)
		return false;
	Reader r(data, size);
	if (names_object(op))
	{
		ObjectHead head;
		if (!head.read(r))
			return false;
		const Reader layout(r.cursor(), r.remaining());
		Reader peek = layout;
		if (peek.u16() != head.id || !peek.ok())
			return false; // the LevelEntity layout starts with the object the head names
		const size_t n = layout.remaining();
		switch (op)
		{
		case H::Destroy: return n == 2;
		case H::Parent: return n == 2 + 2 + 3 * 4;
		case H::Tent: return n == 2 + 2;
		case H::Stash: return n == 2 + 1;
		case H::Died: return n >= 2 + 2;
		case H::Item:
		{
			ItemFactsBody b;
			return reads_whole(layout, b);
		}
		case H::Logic:
		{
			wire::LevelLogicBody b;
			return reads_whole(layout, b);
		}
		default: return true; // State, Spawn: their tails are their own readers'
		}
	}
	switch (op)
	{
	case H::Money:
	{
		const uint32_t peer = r.u32();
		r.u64();
		const uint8_t trade = r.u8();
		return r.ok() && r.remaining() == 0 && wire::detail::slot_ok(peer) && trade <= 1;
	}
	case H::Joined:
	case H::Left:
	{
		MemberBody b;
		return reads_whole(r, b);
	}
	case H::Info:
	{
		InfoBody b;
		return reads_whole(r, b);
	}
	case H::Ask:
	{
		AskBody b;
		return reads_whole(r, b);
	}
	case H::IdAsk: return size == 0;
	case H::Carry:
	{
		CarryBody b;
		return reads_whole(r, b);
	}
	case H::Done:
	{
		DoneBody b;
		return reads_whole(r, b);
	}
	case H::Flushed:
	{
		FlushedBody b;
		return reads_whole(r, b);
	}
	default: break;
	}
	return false;
}

inline bool body_ok(uint8_t op, const std::vector<uint8_t>& body) { return body_ok(op, body.data(), body.size()); }

// An entry of a per-object op: the head, then the LevelEntity layout `layout` of the op (which starts with h.id). False, `out` empty, for
// another op or for what body_ok refuses.
inline bool object_entry(uint8_t op, const ObjectHead& h, const Writer& layout, Entry& out)
{
	out = Entry();
	if (!names_object(op))
		return false;
	Writer w(ObjectHead::kSize + layout.size());
	h.write(w);
	w.bytes(layout.ptr(), layout.size());
	if (!body_ok(op, w.ptr(), w.size()))
		return false;
	out.op = op;
	out.body = w.data();
	return true;
}

// An entry of an op without a head, from its body (MemberBody, InfoBody, AskBody, CarryBody, DoneBody, FlushedBody). False, `out`
// empty, for a per-object op or a body the op does not take.
template <class B>
bool body_entry(uint8_t op, const B& body, Entry& out)
{
	out = Entry();
	if (names_object(op))
		return false;
	Writer w;
	if (!body.write(w) || !body_ok(op, w.ptr(), w.size()))
		return false;
	out.op = op;
	out.body = w.data();
	return true;
}

// The IdAsk entry: no body.
inline Entry id_ask_entry()
{
	Entry e;
	e.op = wire::LevelEntityHead::IdAsk;
	return e;
}

// The head of a per-object entry. False (`h` cleared) for another op or a body without one.
inline bool object_of(const Entry& e, ObjectHead& h)
{
	h = ObjectHead();
	if (!names_object(e.op))
		return false;
	Reader r(e.body.data(), e.body.size());
	return h.read(r);
}

// The LevelEntity layout of a per-object entry, after its head; an empty Reader for another op.
inline Reader layout_of(const Entry& e)
{
	if (!names_object(e.op) || e.body.size() < ObjectHead::kSize)
		return Reader(nullptr, 0);
	return Reader(e.body.data() + ObjectHead::kSize, e.body.size() - ObjectHead::kSize);
}
} // namespace journal

namespace wire
{
// ---------------------------------------------------------------- LevelJournal (0x01E0), LevelJournalAck (0x01E1)

// LevelJournal wire: u32 seq, u16 n, n x {u8 op, varu len, body[len]} (namespace journal above). `seq` counts the batches of one map
// server from 1; a batch has 1 .. kMaxEntries entries and at most kMaxBatch bytes. read() checks every entry (journal::body_ok)
// before it keeps any: a batch with one entry that does not read is refused whole (seq 0, no entries).
struct LevelJournalMsg
{
	uint32_t seq = 0;
	std::vector<journal::Entry> entries;

	size_t wire_size() const
	{
		size_t n = 4 + 2;
		for (const journal::Entry& e : entries)
			n += 1 + Writer::varu_size(e.body.size()) + e.body.size();
		return n;
	}

	// What read() takes: a seq, 1 .. kMaxEntries entries that each read, kMaxBatch bytes at most.
	bool fits() const
	{
		if (seq == 0 || entries.empty() || entries.size() > journal::kMaxEntries || wire_size() > journal::kMaxBatch)
			return false;
		for (const journal::Entry& e : entries)
			if (!journal::body_ok(e.op, e.body))
				return false;
		return true;
	}

	// False, nothing written, for a batch read() would refuse: an entry that does not read would cost the world the whole frame.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(seq);
		w.u16(uint16_t(entries.size()));
		for (const journal::Entry& e : entries)
		{
			w.u8(e.op);
			w.varu(e.body.size());
			w.bytes(e.body.data(), e.body.size());
		}
		return true;
	}

	bool read(Reader& r)
	{
		*this = LevelJournalMsg();
		const size_t start = r.remaining();
		LevelJournalMsg tmp;
		tmp.seq = r.u32();
		const uint16_t n = r.u16();
		// every entry is at least its op and a one-byte length: a count the packet cannot hold is refused before anything is kept
		if (!r.ok() || tmp.seq == 0 || n == 0 || n > journal::kMaxEntries || size_t(n) * 2 > r.remaining())
			return false;
		tmp.entries.reserve(n);
		for (uint16_t i = 0; i < n; ++i)
		{
			journal::Entry e;
			e.op = r.u8();
			const uint64_t len = r.varu();
			if (!r.ok() || len > journal::kMaxBody || len > r.remaining())
				return false;
			e.body.resize(size_t(len));
			if (len && !r.bytes(e.body.data(), size_t(len)))
				return false;
			if (!journal::body_ok(e.op, e.body))
				return false;
			tmp.entries.push_back(std::move(e));
		}
		if (start - r.remaining() > journal::kMaxBatch)
			return false;
		*this = std::move(tmp);
		return true;
	}
};

// LevelJournalAck wire: u32 upto, u8 n, n x {u32 seq, u8 op, u16 id, u8 why}, u8 has_block, [u16 first, u16 count]. Every batch of
// the bridge up to `upto` is applied but the entries the rows name: the world refused them (journal::Why; `id` kNoObject for an op
// that names no object), and the sender repairs toward the world (it releases what the world refused to create, keeping the
// id reserved; it fetches what the world refused to change or remove). The block answers an IdAsk: the ids first .. first + count
// - 1 are the sender's from now on.
struct LevelJournalAckMsg
{
	static constexpr size_t kMaxRefusals = 255; // a u8 count; more go in another Ack with the same upto

	struct Refusal
	{
		uint32_t seq = 0;
		uint8_t op = 0;
		uint16_t id = kNoObject;
		uint8_t why = 0;
	};

	uint32_t upto = 0;
	std::vector<Refusal> refusals;
	bool has_block = false;
	uint16_t first = kNoObject;
	uint16_t count = 0;

	static bool refusal_ok(const Refusal& f, uint32_t upto)
	{
		return f.seq != 0 && f.seq <= upto && journal::known_op(f.op) && f.why >= 1 && f.why <= journal::kLastWhy;
	}
	// ids first .. first + count - 1, none of them kNoObject
	static bool block_ok(uint16_t first, uint16_t count) { return count != 0 && uint32_t(first) + count <= kNoObject; }

	// What read() takes: every Ack answers a batch (upto >= 1), its rows name batches it acknowledges, ops and reasons that exist.
	bool fits() const
	{
		if (upto == 0 || refusals.size() > kMaxRefusals || (has_block && !block_ok(first, count)))
			return false;
		for (const Refusal& f : refusals)
			if (!refusal_ok(f, upto))
				return false;
		return true;
	}

	// False, nothing written, for an Ack read() would refuse: a row cut away would be an entry the sender takes for applied.
	[[nodiscard]] bool write(Writer& w) const
	{
		if (!fits())
			return false;
		w.u32(upto);
		w.u8(uint8_t(refusals.size()));
		for (const Refusal& f : refusals)
		{
			w.u32(f.seq);
			w.u8(f.op);
			w.u16(f.id);
			w.u8(f.why);
		}
		w.u8(has_block ? 1 : 0);
		if (has_block)
		{
			w.u16(first);
			w.u16(count);
		}
		return true;
	}

	bool read(Reader& r)
	{
		*this = LevelJournalAckMsg();
		LevelJournalAckMsg tmp;
		tmp.upto = r.u32();
		const uint8_t n = r.u8();
		if (!r.ok() || tmp.upto == 0)
			return false;
		tmp.refusals.reserve(n);
		for (uint8_t i = 0; i < n; ++i)
		{
			Refusal f;
			f.seq = r.u32();
			f.op = r.u8();
			f.id = r.u16();
			f.why = r.u8();
			if (!r.ok() || !refusal_ok(f, tmp.upto))
				return false;
			tmp.refusals.push_back(f);
		}
		const uint8_t block = r.u8();
		if (!r.ok() || block > 1)
			return false;
		tmp.has_block = block != 0;
		if (tmp.has_block)
		{
			tmp.first = r.u16();
			tmp.count = r.u16();
			if (!r.ok() || !block_ok(tmp.first, tmp.count))
				return false;
		}
		*this = std::move(tmp);
		return true;
	}
};
} // namespace wire
} // namespace levels
} // namespace coop
