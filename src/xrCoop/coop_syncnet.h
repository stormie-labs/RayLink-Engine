// Anomaly Coop — coop.sync on the network (SYNC_SPEC §6.1-6.3, §6.5; F2 T14): the scope names of the Lua API, the five
// sync messages, how the host stamps a change and who hears of it, what a client shows of its own changes until the
// host answers, full copies at join, the host's limits on requests and a client's requests in flight. The store is
// coop_syncstore.h (F2 T13); the glue to the engine, Lua and the host save is xrGame/coop/coop_sync_api.cpp.
//
// Messages (Msg 0x0191-0x0195), all on Lane::Reliable: one ordered lane, so a full copy and the changes after it arrive
// in the order they were made (Bulk is another ENet channel, with no order against Reliable).
//   SyncSet     client -> host: u32 seq, scope, varu key length + key, value                   (write_client_set)
//               host -> client: u8 flags, u32 by, [u32 ack if kAck], then kRefused: u8 SetResult; kDrop: an Object
//                               scope; else the change as write_set writes it (scope, key, u64 version, value)
//   SyncFull    host -> client: u8 flags (kFirst: clear the store first; kLast), varu count, count x Store::encode_scope
//   SyncRequest client -> host: u32 id, varu name length + name, value args                     (write_request)
//   SyncResult  host -> client: u32 id, u8 ok (0 / 1), value result                              (write_result)
//   SyncCast    host -> client: varu name length + name, value args                              (write_cast)
// Scopes, keys and values as coop_syncstore.h writes them; everything read is checked by type, size and finiteness, and
// a refused read leaves its output as it was.
//
// Versions (the T13 contract). The host stamps every change from one counter that only grows: store.top_version() + 1
// (host_set). A client's store holds only what the host sent. A client's own change shows at once on that client
// (ClientView) and goes to the host with a seq; the host answers every change of a client, in order, with kAck and that
// seq: the change as it applied it, or kRefused and the reason. An answer to seq N answers every older one still
// waiting too: the host takes a client's changes in order, so one it never answered never reached it.
//
// Who hears of a change (host_set): World, Session and Object scopes go to every client; a Player scope goes to its own
// player only (goes_to), never to the others. A full copy at join holds the same (encode_full): World, Session, every
// Object, his own Player scope; the removal records of freed scopes stay on the host (Store::scopes lists only scopes
// that hold keys). A destroyed object's scope goes everywhere (host_drop, kDrop).
//
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_ledger.h"
#include "coop_protocol.h"
#include "coop_syncstore.h"
#include "coop_wire.h"

namespace coop
{
namespace syncnet
{
using syncstore::ScopeId;
using syncstore::SetResult;
using syncstore::Store;
using syncstore::Value;

// ---------------------------------------------------------------- scope names of the Lua API

// "world", "session", "player" (this machine's player), "player:<peer>" (a connected player, on the host),
// "object:<id>" (an object by its id in this machine's numbering), "object" (every object: on_change only).
struct ScopeName
{
	enum Kind : uint8_t
	{
		World,
		Session,
		Player,
		PlayerOf,
		Object,
		AnyObject,
	};
	Kind kind = World;
	uint32_t peer = 0; // PlayerOf
	uint16_t id = 0; // Object: local numbering
};

namespace detail
{
// A decimal number in its one spelling: digits only, no sign, no leading zero, at most `max` (19 digits fit a u64).
inline bool parse_uint(std::string_view s, uint64_t max, uint64_t& out)
{
	if (s.empty() || s.size() > 19 || (s.size() > 1 && s[0] == '0'))
		return false;
	uint64_t v = 0;
	for (const char c : s)
	{
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + uint64_t(c - '0');
	}
	if (v > max)
		return false;
	out = v;
	return true;
}
} // namespace detail

// false (out untouched) for anything but the spellings above; a peer id below kInvalidPeer, an object id below 0xffff.
inline bool parse_scope(std::string_view text, ScopeName& out)
{
	ScopeName s;
	uint64_t n = 0;
	if (text == "world")
		s.kind = ScopeName::World;
	else if (text == "session")
		s.kind = ScopeName::Session;
	else if (text == "player")
		s.kind = ScopeName::Player;
	else if (text == "object")
		s.kind = ScopeName::AnyObject;
	else if (text.substr(0, 7) == "player:" && detail::parse_uint(text.substr(7), uint64_t(kInvalidPeer) - 1, n))
	{
		s.kind = ScopeName::PlayerOf;
		s.peer = uint32_t(n);
	}
	else if (text.substr(0, 7) == "object:" && detail::parse_uint(text.substr(7), 0xFFFE, n))
	{
		s.kind = ScopeName::Object;
		s.id = uint16_t(n);
	}
	else
		return false;
	out = s;
	return true;
}

// ---------------------------------------------------------------- names of requests and effects

constexpr size_t kMaxName = 64;

// As coop.send_event checks an event name: 1..kMaxName printable ASCII characters, no spaces.
inline bool name_ok(std::string_view name)
{
	if (name.empty() || name.size() > kMaxName)
		return false;
	for (const char ch : name)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c <= 0x20 || c >= 0x7f)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------- keys of a Lua table

// A table value has string keys (coop_syncstore.h). A Lua array index travels as kIntKeyTag + its decimal, so that it
// comes back as a number and not as the string "1"; a string key from Lua may not start with the tag.
constexpr char kIntKeyTag = '\x01';
constexpr int64_t kMaxIntKey = int64_t(1) << 53; // integers a Lua number holds exactly

// The key of index `k` (|k| <= kMaxIntKey).
inline std::string int_key(int64_t k)
{
	std::string s(1, kIntKeyTag);
	s += std::to_string(k);
	return s;
}

// The index of a key made by int_key: the tag, then a decimal in its one spelling (no '+', no leading zero, no "-0"),
// |k| <= kMaxIntKey. false (out untouched) otherwise.
inline bool int_key_of(std::string_view key, int64_t& out)
{
	if (key.size() < 2 || key[0] != kIntKeyTag)
		return false;
	std::string_view digits = key.substr(1);
	const bool negative = digits[0] == '-';
	if (negative)
		digits = digits.substr(1);
	uint64_t v = 0;
	if (!detail::parse_uint(digits, uint64_t(kMaxIntKey), v) || (negative && v == 0))
		return false;
	out = negative ? -int64_t(v) : int64_t(v);
	return true;
}

// A string key of a Lua table that can travel as it is.
inline bool text_key_ok(std::string_view key) { return key.empty() || key[0] != kIntKeyTag; }

// ---------------------------------------------------------------- wire

// SyncSet from a client: a change of his own player data or of an object he holds.
struct ClientSet
{
	uint32_t seq = 0;
	ScopeId scope;
	std::string key;
	Value value;
};

inline void write_client_set(Writer& w, const ClientSet& m)
{
	w.u32(m.seq);
	syncstore::write_scope(w, m.scope);
	w.varu(m.key.size());
	w.bytes(m.key.data(), m.key.size());
	syncstore::write_value(w, m.value);
}

inline bool read_client_set(Reader& r, ClientSet& out)
{
	ClientSet m;
	m.seq = r.u32();
	if (!r.ok() || !syncstore::read_scope(r, m.scope) || !syncstore::detail::read_text(r, syncstore::kMaxKey, m.key) ||
	    !syncstore::key_ok(m.key) || !syncstore::read_value(r, m.value))
		return false;
	out = std::move(m);
	return true;
}

enum HostSetFlags : uint8_t
{
	kAck = 1, // answers the change `ack` of the client it goes to (and every older one still waiting)
	kRefused = 2, // with kAck: that change was refused (`refused` says why); no change follows
	kDrop = 4, // an object was destroyed: its scope goes; no change follows
	kKnownSetFlags = kAck | kRefused | kDrop,
};

// SyncSet from the host.
struct HostSet
{
	uint8_t flags = 0;
	PeerId by = kHostPeer; // who made the change (by_peer of coop.sync.on_change)
	uint32_t ack = 0; // kAck
	SetResult refused = SetResult::Ok; // kRefused
	ScopeId scope; // the change's, or (kDrop) the dropped one
	std::string key;
	Value value; // nil: the key was removed
	uint64_t version = 0;

	bool has_change() const { return (flags & (kRefused | kDrop)) == 0; }
};

// A SetResult that refuses a change.
inline bool is_refusal(SetResult r)
{
	switch (r)
	{
	case SetResult::BadKey:
	case SetResult::BadValue:
	case SetResult::TooBig:
	case SetResult::ScopeFull:
	case SetResult::NotAllowed:
	case SetResult::Stale: return true;
	case SetResult::Ok:
	case SetResult::Same: return false;
	}
	return false;
}

inline void write_host_set(Writer& w, const HostSet& m)
{
	w.u8(m.flags);
	w.u32(m.by);
	if (m.flags & kAck)
		w.u32(m.ack);
	if (m.flags & kRefused)
		w.u8(uint8_t(m.refused));
	else if (m.flags & kDrop)
		syncstore::write_scope(w, m.scope);
	else
		syncstore::write_set(w, m.scope, m.key, m.value, m.version);
}

// false (out untouched) on anything the host does not send: unknown flags, kRefused without kAck or with a result that
// is no refusal, kDrop with another flag or of a scope that is not an Object, a change the store would not read.
inline bool read_host_set(Reader& r, HostSet& out)
{
	HostSet m;
	m.flags = r.u8();
	m.by = r.u32();
	if (!r.ok() || (m.flags & ~kKnownSetFlags) || ((m.flags & kRefused) && !(m.flags & kAck)) ||
	    ((m.flags & kDrop) && m.flags != kDrop))
		return false;
	if (m.flags & kAck)
		m.ack = r.u32();
	if (m.flags & kRefused)
	{
		const uint8_t why = r.u8();
		if (!r.ok() || why >= syncstore::kSetResultCount || !is_refusal(SetResult(why)))
			return false;
		m.refused = SetResult(why);
	}
	else if (m.flags & kDrop)
	{
		if (!syncstore::read_scope(r, m.scope) || m.scope.kind != ScopeId::Object)
			return false;
	}
	else if (!syncstore::read_set(r, m.scope, m.key, m.value, m.version))
		return false;
	if (!r.ok())
		return false;
	out = std::move(m);
	return true;
}

// coop.sync.request, its answer, coop.sync.broadcast.
struct Request
{
	uint32_t id = 0;
	std::string name;
	Value args;
};
struct Result
{
	uint32_t id = 0;
	bool ok = false;
	Value value;
};
struct Cast
{
	std::string name;
	Value args;
};

namespace detail
{
inline void write_name(Writer& w, const std::string& name)
{
	w.varu(name.size());
	w.bytes(name.data(), name.size());
}
inline bool read_name(Reader& r, std::string& out)
{
	return syncstore::detail::read_text(r, kMaxName, out) && name_ok(out);
}
} // namespace detail

inline void write_request(Writer& w, const Request& m)
{
	w.u32(m.id);
	detail::write_name(w, m.name);
	syncstore::write_value(w, m.args);
}

inline bool read_request(Reader& r, Request& out)
{
	Request m;
	m.id = r.u32();
	if (!r.ok() || !detail::read_name(r, m.name) || !syncstore::read_value(r, m.args))
		return false;
	out = std::move(m);
	return true;
}

inline void write_result(Writer& w, const Result& m)
{
	w.u32(m.id);
	w.u8(m.ok ? 1 : 0);
	syncstore::write_value(w, m.value);
}

inline bool read_result(Reader& r, Result& out)
{
	Result m;
	m.id = r.u32();
	const uint8_t ok = r.u8();
	if (!r.ok() || ok > 1 || !syncstore::read_value(r, m.value))
		return false;
	m.ok = ok != 0;
	out = std::move(m);
	return true;
}

inline void write_cast(Writer& w, const Cast& m)
{
	detail::write_name(w, m.name);
	syncstore::write_value(w, m.args);
}

inline bool read_cast(Reader& r, Cast& out)
{
	Cast m;
	if (!detail::read_name(r, m.name) || !syncstore::read_value(r, m.args))
		return false;
	out = std::move(m);
	return true;
}

// ---------------------------------------------------------------- host: one change, and who hears of it

// One SyncSet the host sends: to every client in the world but `except`, or to one.
struct Delivery
{
	enum To : uint8_t
	{
		Everyone,
		One,
	};
	To to = Everyone;
	PeerId peer = kInvalidPeer; // One
	PeerId except = kInvalidPeer; // Everyone
	HostSet msg;
};

// A change of what a machine shows (coop.sync.on_change): `value` and `old` nil for none.
struct Change
{
	ScopeId scope;
	std::string key;
	Value value;
	Value old;
	PeerId by = kHostPeer;
};

// A Player scope reaches its own player only; every other scope reaches everybody.
inline bool goes_to(const ScopeId& scope, const std::string& player_digest)
{
	return scope.kind != ScopeId::Player || scope.player_key == player_digest;
}

// The Player scope of a character, by its custom data: the player key on its "key" line, as money and the spawn budget
// find it (ledger::character_key; Rulings F2-13, F2-18, F2-24), a legacy key line taken as its digest. false (out
// untouched) for a character without a player key (an NPC).
inline bool player_scope_of(const std::string& custom_data, ScopeId& out)
{
	std::string key = ledger::character_key(custom_data);
	if (!syncstore::player_key_ok(key))
		return false;
	out = ScopeId::player(std::move(key));
	return true;
}

// F2 T14 fix 1 (review I3, Ruling F2-31): the host's own data in the scopes clients write.
// - Keys "coop.*" of a player scope are the host's (F3 keeps a player's checkpoint there, syncapi::player_set): his own
//   player cannot write or remove them, so he cannot forge them.
// - Clients never take the room the host keeps for its own writes: kHostPlayerReserve of every player scope,
//   kHostObjectReserve of every object scope, kHostPoolReserve of the pool all player and object scopes share. A client
//   that fills his scope or the pool gets ScopeFull; the host still writes.
constexpr std::string_view kHostKeyPrefix = "coop.";
constexpr size_t kHostPlayerReserve = size_t(64) << 10;
constexpr size_t kHostObjectReserve = size_t(8) << 10;
constexpr size_t kHostPoolReserve = size_t(1) << 20;
static_assert(kHostPlayerReserve < syncstore::kMaxPlayerBytes && kHostObjectReserve < syncstore::kMaxObjectBytes &&
        kHostPoolReserve < syncstore::kMaxPoolBytes,
    "a client keeps room of his own");

inline bool host_key(const ScopeId& scope, std::string_view key)
{
	return scope.kind == ScopeId::Player && key.substr(0, kHostKeyPrefix.size()) == kHostKeyPrefix;
}

inline size_t host_scope_reserve(ScopeId::Kind kind)
{
	return kind == ScopeId::Player ? kHostPlayerReserve : kind == ScopeId::Object ? kHostObjectReserve : 0;
}

// A client's change of `scope`, before the store sees it. `owner`: he is the player of a player scope or holds the
// object. NotAllowed if he may not write the scope (may_set) or the key is the host's; ScopeFull if it would take the
// host's room (a removal only frees room); else Ok (the store checks the rest: form, size, versions).
inline SetResult client_check(const Store& store, const ScopeId& scope, std::string_view key, const Value& value, bool owner)
{
	if (!syncstore::may_set(scope.kind, false, owner) || host_key(scope, key))
		return SetResult::NotAllowed;
	if (value.type == syncstore::ValueType::Nil)
		return SetResult::Ok;
	const Value* had = store.get(scope, key);
	const size_t old = had ? syncstore::entry_bytes(key, *had) : 0; // what the store counts for it (Entry::cost)
	const size_t cost = syncstore::entry_bytes(key, value); // the order of a table's fields does not change it
	if (store.bytes(scope) - old + cost > syncstore::scope_limit(scope.kind) - host_scope_reserve(scope.kind))
		return SetResult::ScopeFull;
	if (syncstore::pooled(scope.kind) && store.pool_bytes() - old + cost > syncstore::kMaxPoolBytes - kHostPoolReserve)
		return SetResult::ScopeFull;
	return SetResult::Ok;
}

// Host: applies one change made by `by` (kHostPeer: the host's Lua or engine), stamped with the next version of the
// store's one counter, and says who hears of it in `out`. `by_owner`: `by` is the player of a Player scope or holds the
// object (may_set). `ack`: a client's change, answered to him with this seq whatever comes of it; nullptr for the
// host's own. `player_peer`: the connected player whose Player scope this is (kInvalidPeer: none, or not a Player
// scope). `event` (if given) gets the change when the store changed (Ok). Returns the store's result; a client's change
// goes through client_check first (fix 1, Ruling F2-31).
inline SetResult host_set(Store& store, const ScopeId& scope, std::string_view key, Value value, PeerId by, bool by_owner,
    const uint32_t* ack, PeerId player_peer, std::vector<Delivery>& out, Change* event = nullptr)
{
	Value old;
	if (const Value* was = store.get(scope, key))
		old = *was;
	const uint64_t version = store.top_version() + 1; // above kMaxVersion the store refuses it (BadValue)
	// F2 T14 fix 1 (Ruling F2-31): a client's change keeps off the host's keys and the host's room.
	SetResult r = by == kHostPeer ? SetResult::Ok : client_check(store, scope, key, value, by_owner);
	if (r == SetResult::Ok)
		r = store.set_by(scope, key, std::move(value), version, by == kHostPeer, by_owner);
	if (r != SetResult::Ok && r != SetResult::Same)
	{
		if (ack)
		{
			Delivery d;
			d.to = Delivery::One;
			d.peer = by;
			d.msg.flags = kAck | kRefused;
			d.msg.by = by;
			d.msg.ack = *ack;
			d.msg.refused = r;
			out.push_back(std::move(d));
		}
		return r;
	}
	HostSet m;
	m.by = by;
	m.scope = scope;
	m.key = std::string(key);
	if (const Value* now = store.get(scope, key))
		m.value = *now;
	m.version = version;
	if (r == SetResult::Ok)
	{
		if (scope.kind == ScopeId::Player)
		{
			if (player_peer != kInvalidPeer && !(ack && player_peer == by))
			{
				Delivery d;
				d.to = Delivery::One;
				d.peer = player_peer;
				d.msg = m;
				out.push_back(std::move(d));
			}
		}
		else
		{
			Delivery d;
			d.to = Delivery::Everyone;
			d.except = ack ? by : kInvalidPeer;
			d.msg = m;
			out.push_back(std::move(d));
		}
		if (event)
		{
			event->scope = scope;
			event->key = m.key;
			event->value = m.value;
			event->old = std::move(old);
			event->by = by;
		}
	}
	if (ack)
	{
		Delivery d;
		d.to = Delivery::One;
		d.peer = by;
		d.msg = std::move(m);
		d.msg.flags = kAck;
		d.msg.ack = *ack;
		out.push_back(std::move(d));
	}
	return r;
}

// Host: an object was destroyed: its scope goes here, and (if it had any keys) on every client. Only Object scopes;
// true if one went.
inline bool host_drop(Store& store, const ScopeId& scope, std::vector<Delivery>& out)
{
	if (scope.kind != ScopeId::Object)
		return false;
	const bool had = store.bytes(scope) != 0; // a scope exists while it holds a key, and every key costs
	store.drop(scope);
	if (!had)
		return false;
	Delivery d;
	d.to = Delivery::Everyone;
	d.msg.flags = kDrop;
	d.msg.scope = scope;
	out.push_back(std::move(d));
	return true;
}

// ---------------------------------------------------------------- full copies

enum FullFlags : uint8_t
{
	kFirst = 1, // the client clears its store before it takes this part
	kLast = 2,
};
constexpr size_t kFullPartBytes = size_t(256) << 10;
// The smallest scope copy: a kind (1), the newest removal (8), a count (1).
constexpr size_t kMinScopeWire = 10;

// Host: the full copy for the player whose key digest is `player_digest` (what goes_to him), as SyncFull payloads.
// A part ends after the scope that takes it to `part_bytes`; one scope is never split (a World scope fits a game
// message: at most kMaxWorldBytes against the session's 16 MiB). At least one part: the first has kFirst, the last
// kLast.
inline std::vector<Writer> encode_full(const Store& store, const std::string& player_digest, size_t part_bytes = kFullPartBytes)
{
	std::vector<Writer> parts;
	Writer body;
	uint64_t count = 0;
	const auto close = [&](bool last) {
		Writer w(body.size() + 16);
		w.u8(uint8_t((parts.empty() ? kFirst : 0) | (last ? kLast : 0)));
		w.varu(count);
		if (body.size())
			w.bytes(body.ptr(), body.size());
		parts.push_back(std::move(w));
		body.clear();
		count = 0;
	};
	for (const ScopeId& s : store.scopes())
	{
		if (!goes_to(s, player_digest))
			continue;
		store.encode_scope(body, s);
		++count;
		if (body.size() >= part_bytes)
			close(false);
	}
	close(true);
	return parts;
}

// Client: one SyncFull part into `store` (kFirst: cleared first). false on a malformed part: what it took before the
// fault stays. `last`: this part has kLast.
inline bool apply_full(Store& store, Reader& r, bool& last)
{
	const uint8_t flags = r.u8();
	const uint64_t count = r.varu();
	if (!r.ok() || (flags & ~(kFirst | kLast)) || count > r.remaining() / kMinScopeWire)
		return false;
	if (flags & kFirst)
		store.clear();
	for (uint64_t i = 0; i < count; ++i)
	{
		ScopeId id;
		if (!store.decode_scope(r, id))
			return false;
	}
	if (r.remaining() != 0)
		return false;
	last = (flags & kLast) != 0;
	return true;
}

// ---------------------------------------------------------------- client: its own changes until the host answers

// The changes a client sent and the host has not answered: what it shows for them meanwhile.
class ClientView
{
public:
	static constexpr size_t kMaxPending = 4096; // sends without an answer
	static constexpr uint64_t kAnswerTimeoutMs = 30000;
	using Key = std::pair<ScopeId, std::string>;
	using Gone = std::vector<std::pair<Key, Value>>;

	bool full() const { return m_sent.size() >= kMaxPending; }
	size_t waiting() const { return m_sent.size(); }
	size_t shown() const { return m_pending.size(); }

	// A change sent now: shown until answered. Its seq (never 0); 0 if full (nothing changes).
	uint32_t add(const ScopeId& scope, std::string_view key, Value v, uint64_t now_ms)
	{
		if (full())
			return 0;
		if (++m_next == 0)
			++m_next;
		Key k(scope, std::string(key));
		auto it = m_pending.find(k);
		if (it != m_pending.end())
		{
			m_by_seq.erase(it->second.seq);
			it->second.value = std::move(v);
			it->second.seq = m_next;
		}
		else
			m_pending.emplace(k, Entry{std::move(v), m_next});
		m_by_seq.emplace(m_next, std::move(k));
		m_sent.emplace_back(m_next, now_ms);
		return m_next;
	}

	// What is shown for a key while its change waits; nullptr if none waits.
	const Value* pending(const ScopeId& scope, std::string_view key) const
	{
		const auto it = m_pending.find(Key(scope, std::string(key)));
		return it == m_pending.end() ? nullptr : &it->second.value;
	}

	// The waiting keys of a scope that start with `prefix`.
	std::vector<std::pair<std::string, const Value*>> pending_keys(const ScopeId& scope, std::string_view prefix) const
	{
		std::vector<std::pair<std::string, const Value*>> out;
		for (auto it = m_pending.lower_bound(Key(scope, std::string(prefix)));
		     it != m_pending.end() && it->first.first == scope && it->first.second.compare(0, prefix.size(), prefix) == 0;
		     ++it)
			out.emplace_back(it->first.second, &it->second.value);
		return out;
	}

	// The host answered `seq`, and so every older send. The keys whose shown value goes, with that value. `lost`: older
	// sends that were never answered.
	Gone answered(uint32_t seq, size_t& lost)
	{
		lost = 0;
		while (!m_sent.empty() && m_sent.front().first <= seq)
		{
			if (m_sent.front().first < seq)
				++lost;
			m_sent.pop_front();
		}
		return take_up_to(seq);
	}

	// Sends older than kAnswerTimeoutMs are given up (counted in `given_up`), and what they show goes.
	Gone expire(uint64_t now_ms, size_t& given_up)
	{
		given_up = 0;
		uint32_t last = 0;
		while (!m_sent.empty() && m_sent.front().second + kAnswerTimeoutMs <= now_ms)
		{
			last = m_sent.front().first;
			m_sent.pop_front();
			++given_up;
		}
		return given_up ? take_up_to(last) : Gone();
	}

	// Every send waits from `now_ms` on (made while the client loaded the world: they leave when it is in).
	void touch_all(uint64_t now_ms)
	{
		for (auto& s : m_sent)
			s.second = now_ms;
	}

	// The host dropped a scope: what is shown in it goes (the sends stay waiting for their answers).
	Gone drop_scope(const ScopeId& scope)
	{
		Gone out;
		auto it = m_pending.lower_bound(Key(scope, std::string()));
		while (it != m_pending.end() && it->first.first == scope)
		{
			m_by_seq.erase(it->second.seq);
			out.emplace_back(it->first, std::move(it->second.value));
			it = m_pending.erase(it);
		}
		return out;
	}

	void clear()
	{
		m_pending.clear();
		m_by_seq.clear();
		m_sent.clear();
	}

private:
	struct Entry
	{
		Value value;
		uint32_t seq = 0; // the newest send of the key
	};

	Gone take_up_to(uint32_t seq)
	{
		Gone out;
		while (!m_by_seq.empty() && m_by_seq.begin()->first <= seq)
		{
			const auto p = m_pending.find(m_by_seq.begin()->second);
			out.emplace_back(p->first, std::move(p->second.value));
			m_pending.erase(p);
			m_by_seq.erase(m_by_seq.begin());
		}
		return out;
	}

	std::map<Key, Entry> m_pending; // at most one entry per send in m_sent
	std::map<uint32_t, Key> m_by_seq; // the newest send of each shown key
	std::deque<std::pair<uint32_t, uint64_t>> m_sent; // (seq, when), oldest first
	uint32_t m_next = 0;
};

// A client's coop.sync: the host's store, its own changes shown over it (ClientView), and the changes of what it shows
// (for coop.sync.on_change) as messages arrive.
class ClientCore
{
public:
	struct Stats
	{
		uint64_t applied = 0; // host changes taken
		uint64_t stale = 0; // host changes not newer than what the store had (a replay after a full copy)
		uint64_t acks = 0; // answers to our changes
		uint64_t refused = 0; // ...of which refusals
		uint64_t lost = 0; // our changes the host never answered (an answer to a newer one came)
		uint64_t expired = 0; // our changes given up after ClientView::kAnswerTimeoutMs
		uint64_t dropped = 0; // object scopes dropped by the host
		uint64_t full_parts = 0; // full copy parts taken
		uint64_t full_bad = 0; // full copy parts refused (malformed)
	};

	// What this machine shows: its own waiting change, else the host's value; nil if none.
	Value shown(const ScopeId& scope, std::string_view key) const
	{
		if (const Value* p = m_view.pending(scope, key))
			return *p;
		const Value* v = m_store.get(scope, key);
		return v ? *v : Value();
	}

	// The keys shown in a scope that start with `prefix`, in increasing order.
	std::vector<std::pair<std::string, Value>> keys(const ScopeId& scope, std::string_view prefix) const
	{
		std::map<std::string, Value> m;
		for (const auto& kv : m_store.keys(scope, prefix))
			m[kv.first] = *kv.second;
		for (const auto& kv : m_view.pending_keys(scope, prefix))
		{
			if (kv.second->type == syncstore::ValueType::Nil)
				m.erase(kv.first);
			else
				m[kv.first] = *kv.second;
		}
		return std::vector<std::pair<std::string, Value>>(m.begin(), m.end());
	}

	// Our own change, made by `self`: shown at once (an event if that changes what is shown). Its seq for the host;
	// 0 if too many wait for an answer (nothing changes). The caller checked the value and that we may write.
	uint32_t local_set(const ScopeId& scope, std::string_view key, Value v, PeerId self, uint64_t now_ms,
	    std::vector<Change>& events)
	{
		if (m_view.full())
			return 0;
		const Value before = shown(scope, key);
		const uint32_t seq = m_view.add(scope, key, std::move(v), now_ms);
		note(scope, std::string(key), before, self, events);
		return seq;
	}

	// One SyncSet from the host.
	void apply(const HostSet& m, std::vector<Change>& events)
	{
		if (m.flags & kDrop)
		{
			const std::vector<std::pair<std::string, Value>> before = keys(m.scope, "");
			m_store.drop(m.scope);
			m_view.drop_scope(m.scope);
			++m_stats.dropped;
			for (const auto& kv : before)
				events.push_back(Change{m.scope, kv.first, Value(), kv.second, m.by});
			return;
		}
		Value before_change;
		if (m.has_change())
		{
			before_change = shown(m.scope, m.key);
			if (m_store.set(m.scope, m.key, m.value, m.version) == SetResult::Stale)
				++m_stats.stale;
			else
				++m_stats.applied;
		}
		ClientView::Gone gone;
		if (m.flags & kAck)
		{
			size_t lost = 0;
			gone = m_view.answered(m.ack, lost);
			++m_stats.acks;
			m_stats.lost += lost;
			if (m.flags & kRefused)
				++m_stats.refused;
		}
		if (m.has_change())
			note(m.scope, m.key, before_change, m.by, events);
		for (auto& g : gone)
		{
			if (m.has_change() && g.first.first == m.scope && g.first.second == m.key)
				continue; // done above
			note(g.first.first, g.first.second, g.second, (m.flags & kRefused) ? kHostPeer : m.by, events);
		}
	}

	// One SyncFull part (kFirst clears the store; our waiting changes keep showing over it). No events: a full copy
	// comes at join, before the scripts of the new world exist. false: malformed (counted).
	bool apply_full(Reader& r)
	{
		bool last = false;
		if (!syncnet::apply_full(m_store, r, last))
		{
			++m_stats.full_bad;
			return false;
		}
		++m_stats.full_parts;
		return true;
	}

	// Our changes the host did not answer in time are given up; what they showed goes.
	void expire(uint64_t now_ms, std::vector<Change>& events)
	{
		size_t n = 0;
		ClientView::Gone gone = m_view.expire(now_ms, n);
		m_stats.expired += n;
		for (auto& g : gone)
			note(g.first.first, g.first.second, g.second, kHostPeer, events);
	}

	// Our changes made while loading the world leave when we are in it: they wait for an answer from then on.
	void touch_all(uint64_t now_ms) { m_view.touch_all(now_ms); }
	size_t waiting() const { return m_view.waiting(); }
	bool full() const { return m_view.full(); }
	// Session over, or the client's world replaced: nothing of the host's stays. Stats stay.
	void clear()
	{
		m_store.clear();
		m_view.clear();
	}
	Store& store() { return m_store; }
	const Store& store() const { return m_store; }
	const ClientView& view() const { return m_view; }
	const Stats& stats() const { return m_stats; }

private:
	// An event if what is shown for the key is not `before` any more.
	void note(const ScopeId& scope, const std::string& key, const Value& before, PeerId by, std::vector<Change>& events)
	{
		Value now = shown(scope, key);
		if (syncstore::same(before, now))
			return;
		events.push_back(Change{scope, key, std::move(now), before, by});
	}

	Store m_store;
	ClientView m_view;
	Stats m_stats;
};

// ---------------------------------------------------------------- host: limits of coop.sync.request

// Per (player, request name): `burst` in a row, then `per_sec` per second (a token bucket in millionths of a token, so
// that the arithmetic is exact). Default kDefaultBurst / kDefaultPerSec; coop.sync.limit(name, burst, per_sec) sets
// another one for a name. Bounded: kMaxRules names, kMaxBuckets (player, name) pairs (a request for a name nobody
// handles never gets a bucket: the caller answers it first).
class RequestLimits
{
public:
	static constexpr uint32_t kDefaultBurst = 20;
	static constexpr double kDefaultPerSec = 10.0;
	static constexpr uint32_t kMaxBurst = 1000;
	static constexpr double kMaxPerSec = 1000.0;
	static constexpr size_t kMaxRules = 256;
	static constexpr size_t kMaxBuckets = 4096;

	// false (nothing changes) for a bad name, burst outside 1..kMaxBurst, per_sec outside (0, kMaxPerSec] or NaN, or a
	// new name over kMaxRules. A known name takes the new rule; its buckets start again full.
	bool set_rule(std::string_view name, uint32_t burst, double per_sec)
	{
		if (!name_ok(name) || burst < 1 || burst > kMaxBurst || !(per_sec > 0.0) || !(per_sec <= kMaxPerSec))
			return false;
		const auto it = m_rules.find(name);
		if (it == m_rules.end() && m_rules.size() >= kMaxRules)
			return false;
		m_rules[std::string(name)] = Rule{burst, per_sec};
		for (auto b = m_buckets.begin(); b != m_buckets.end();)
			b = b->first.second == name ? m_buckets.erase(b) : std::next(b);
		return true;
	}

	// One request of `name` from `peer` now: true if within its rule (counted), false if over it or no room for one
	// more bucket.
	bool allow(PeerId peer, std::string_view name, uint64_t now_ms)
	{
		const auto rit = m_rules.find(name);
		const Rule rule = rit == m_rules.end() ? Rule{kDefaultBurst, kDefaultPerSec} : rit->second;
		const uint64_t cap = uint64_t(rule.burst) * kUnit;
		auto key = std::make_pair(peer, std::string(name));
		auto it = m_buckets.find(key);
		if (it == m_buckets.end())
		{
			if (m_buckets.size() >= kMaxBuckets)
				return false;
			it = m_buckets.emplace(std::move(key), Bucket{cap, now_ms}).first;
		}
		Bucket& b = it->second;
		if (now_ms > b.at)
		{
			const double refill = double(now_ms - b.at) * rule.per_sec * double(kUnit) / 1000.0;
			const uint64_t add = refill >= double(cap) ? cap : uint64_t(refill);
			b.units = cap - b.units < add ? cap : b.units + add;
			b.at = now_ms;
		}
		if (b.units < kUnit)
			return false;
		b.units -= kUnit;
		return true;
	}

	// The player left: his buckets go (a new connection starts full).
	void forget_peer(PeerId peer)
	{
		for (auto b = m_buckets.begin(); b != m_buckets.end();)
			b = b->first.first == peer ? m_buckets.erase(b) : std::next(b);
	}
	void clear_buckets() { m_buckets.clear(); }
	// A new Lua state: its scripts set their rules again.
	void clear()
	{
		m_rules.clear();
		m_buckets.clear();
	}
	size_t rules() const { return m_rules.size(); }

private:
	static constexpr uint64_t kUnit = 1000000;
	struct Rule
	{
		uint32_t burst;
		double per_sec;
	};
	struct Bucket
	{
		uint64_t units;
		uint64_t at;
	};
	std::map<std::string, Rule, std::less<>> m_rules;
	std::map<std::pair<PeerId, std::string>, Bucket> m_buckets;
};

// ---------------------------------------------------------------- requests waiting for an answer

constexpr uint64_t kRequestTimeoutMs = 30000;

// A machine's coop.sync.request calls waiting for their answer, each with a payload (the Lua callback). Bounded.
template <class T>
class Requests
{
public:
	static constexpr size_t kMax = 256;

	bool full() const { return m_waiting.size() >= kMax; }
	size_t size() const { return m_waiting.size(); }

	// A request sent now: its id (never 0); 0 if full.
	uint32_t add(uint64_t now_ms, T payload)
	{
		if (full())
			return 0;
		do
		{
			if (++m_next == 0)
				++m_next;
		} while (m_waiting.count(m_next));
		m_waiting.emplace(m_next, Item{now_ms, std::move(payload)});
		return m_next;
	}

	// Its answer came: true (and the payload) if it was waiting.
	bool take(uint32_t id, T& out)
	{
		const auto it = m_waiting.find(id);
		if (it == m_waiting.end())
			return false;
		out = std::move(it->second.payload);
		m_waiting.erase(it);
		return true;
	}

	// Requests older than kRequestTimeoutMs, taken out.
	std::vector<std::pair<uint32_t, T>> expire(uint64_t now_ms)
	{
		std::vector<std::pair<uint32_t, T>> out;
		for (auto it = m_waiting.begin(); it != m_waiting.end();)
		{
			if (it->second.at + kRequestTimeoutMs <= now_ms)
			{
				out.emplace_back(it->first, std::move(it->second.payload));
				it = m_waiting.erase(it);
			}
			else
				++it;
		}
		return out;
	}

	// Every request waits from `now_ms` on (a client's requests made while it loaded the world: they leave when it is in).
	void touch_all(uint64_t now_ms)
	{
		for (auto& kv : m_waiting)
			kv.second.at = now_ms;
	}

	// Everything waiting, taken out.
	std::vector<std::pair<uint32_t, T>> take_all()
	{
		std::vector<std::pair<uint32_t, T>> out;
		for (auto& kv : m_waiting)
			out.emplace_back(kv.first, std::move(kv.second.payload));
		m_waiting.clear();
		return out;
	}

	void clear() { m_waiting.clear(); }

private:
	struct Item
	{
		uint64_t at;
		T payload;
	};
	std::map<uint32_t, Item> m_waiting;
	uint32_t m_next = 0;
};
} // namespace syncnet
} // namespace coop
