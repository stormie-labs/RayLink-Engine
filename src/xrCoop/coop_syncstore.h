// Anomaly Coop — the coop.sync store: shared variables by scope, their value format on the wire and their limits
// (SYNC_SPEC §6.1, §6.5; F2 T13). Mods declare shared data through it without network code; coop::syncapi (F2 T14)
// wires it to Lua, the network and the host save.
//
// Scopes (ScopeId):
//   - World: one; only the host writes it; kept in the host save;
//   - Session: one; only the host writes it; lives for the session;
//   - Player: one per player, keyed by his player key digest (player_key: ledger::character_key of his character, the
//     "key" line of its custom data), not by a peer id or an entity id: a player who reconnects (a new peer id) or a
//     save that gives his character another id still finds his data (Rulings F2-13, F2-18). Written by his player or
//     the host; kept in the host save;
//   - Object: one per object, by its id in the host's numbering (arg); written by the host or the object's owner; it
//     goes with the object and is dropped when the object is destroyed.
// Values (Value): nil, number (a finite double), string, bool, vector (3 finite floats), a flat table (string keys,
// values number / string / bool / vector). Setting nil removes the key.
//
// Versions. Every change carries a version; a change is applied only if it is newer than the key's last change, or,
// for a key that is not there, than the last removal in its scope (else Stale). The contract for syncapi: versions are
// stamped by the host from one counter that only grows (top_version() + 1 for the next change, carried on in the host
// save), so every change in a scope is newer than every change before it; versions start at 1. A client's store holds
// only what the host sent. A full copy (encode_scope / decode_scope) carries the versions, so after a client takes it,
// a change from before the copy is Stale (Review Focus 2).
//
// Limits (every container is bounded; an overflow is refused, never silent, and counted in Stats):
//   - a key: 1..kMaxKey bytes (a table's keys: 0..kMaxKey); a value: kMaxValueBytes encoded;
//   - a scope: World 4 MiB, Session 1 MiB, Player 1 MiB, Object 64 KiB, in entry_bytes(): the encoded entry plus a fixed
//     cost per key and per table field (what it takes in memory beyond its bytes), so many tiny keys or fields cannot
//     outgrow the limit in memory;
//   - scopes: at most kMaxPlayerScopes players and kMaxObjectScopes objects, all of them together at most kMaxPoolBytes
//     (the pool); World and Session have their own room outside it, so clients filling the pool cannot starve the host.
// Everything from the network is checked by type, size and finiteness before it is used (read_set, read_value,
// decode_scope); a refusal leaves the outputs and the store as they were.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_validate.h"
#include "coop_wire.h"

namespace coop
{
namespace syncstore
{
// ---------------------------------------------------------------- values

enum class ValueType : uint8_t
{
	Nil,
	Number,
	String,
	Bool,
	Vec3,
	Table,
};
constexpr uint8_t kValueTypeCount = 6;

// One value. Only the members of its type mean anything. A Table holds (key, scalar value) pairs; the store keeps them in
// canonical order (canonicalize: by key, keys unique).
struct Value
{
	ValueType type = ValueType::Nil;
	double num = 0;
	bool b = false;
	float v[3] = {0.f, 0.f, 0.f};
	std::string str;
	std::vector<std::pair<std::string, Value>> table;
};

inline Value make_nil() { return Value(); }

inline Value make_number(double x)
{
	Value r;
	r.type = ValueType::Number;
	r.num = x;
	return r;
}

inline Value make_string(std::string s)
{
	Value r;
	r.type = ValueType::String;
	r.str = std::move(s);
	return r;
}

inline Value make_bool(bool x)
{
	Value r;
	r.type = ValueType::Bool;
	r.b = x;
	return r;
}

inline Value make_vec3(float x, float y, float z)
{
	Value r;
	r.type = ValueType::Vec3;
	r.v[0] = x;
	r.v[1] = y;
	r.v[2] = z;
	return r;
}

inline Value make_table(std::vector<std::pair<std::string, Value>> fields)
{
	Value r;
	r.type = ValueType::Table;
	r.table = std::move(fields);
	return r;
}

// ---------------------------------------------------------------- scopes

struct ScopeId
{
	enum Kind : uint8_t
	{
		World,
		Session,
		Player,
		Object,
	};
	Kind kind = World;
	uint32_t arg = 0; // Object: the object's id in the host's numbering; 0 for the other kinds
	std::string player_key; // Player: the player key digest (kPlayerKeyDigits lowercase hex); empty for the other kinds

	static ScopeId world() { return ScopeId(); }
	static ScopeId session()
	{
		ScopeId s;
		s.kind = Session;
		return s;
	}
	// `key_digest`: ledger::character_key of the player's character.
	static ScopeId player(std::string key_digest)
	{
		ScopeId s;
		s.kind = Player;
		s.player_key = std::move(key_digest);
		return s;
	}
	static ScopeId object(uint32_t id)
	{
		ScopeId s;
		s.kind = Object;
		s.arg = id;
		return s;
	}
};
constexpr uint8_t kKindCount = 4;

inline bool operator==(const ScopeId& a, const ScopeId& b)
{
	return a.kind == b.kind && a.arg == b.arg && a.player_key == b.player_key;
}
inline bool operator!=(const ScopeId& a, const ScopeId& b) { return !(a == b); }
inline bool operator<(const ScopeId& a, const ScopeId& b)
{
	if (a.kind != b.kind)
		return a.kind < b.kind;
	if (a.arg != b.arg)
		return a.arg < b.arg;
	return a.player_key < b.player_key;
}

// ---------------------------------------------------------------- limits

constexpr size_t kMaxKey = 128;
constexpr size_t kMaxValueBytes = 16384; // encoded (encoded_size)
constexpr size_t kMaxWorldBytes = size_t(4) << 20;
constexpr size_t kMaxSessionBytes = size_t(1) << 20;
constexpr size_t kMaxPlayerBytes = size_t(1) << 20;
constexpr size_t kMaxObjectBytes = size_t(64) << 10;
// F2 T13: bounds of the whole store (lesson of the F2 reviews: every container is bounded per key, per scope and in
// total). 256 players over the life of one host save (a session admits at most 64 new characters, Ruling F2-10);
// 65536 objects: the whole id space of ALife (u16).
constexpr size_t kMaxPlayerScopes = 256;
constexpr size_t kMaxObjectScopes = 65536;
constexpr size_t kMaxPoolBytes = size_t(16) << 20; // all Player and Object scopes together
constexpr size_t kMaxTotalBytes = kMaxWorldBytes + kMaxSessionBytes + kMaxPoolBytes;
// What a key and a table field take in memory beyond their bytes (a map node, a string, a Value), counted in the scope.
constexpr size_t kEntryOverhead = 64;
constexpr size_t kFieldOverhead = 32;
// A player key digest: sha256 as lowercase hex (coop::kKeyDigestHexDigits).
constexpr size_t kPlayerKeyDigits = 64;

inline size_t scope_limit(ScopeId::Kind kind)
{
	switch (kind)
	{
	case ScopeId::World:
		return kMaxWorldBytes;
	case ScopeId::Session:
		return kMaxSessionBytes;
	case ScopeId::Player:
		return kMaxPlayerBytes;
	case ScopeId::Object:
		return kMaxObjectBytes;
	}
	return 0;
}

inline size_t max_scopes(ScopeId::Kind kind)
{
	switch (kind)
	{
	case ScopeId::World:
	case ScopeId::Session:
		return 1;
	case ScopeId::Player:
		return kMaxPlayerScopes;
	case ScopeId::Object:
		return kMaxObjectScopes;
	}
	return 0;
}

// Player and Object scopes share the pool (kMaxPoolBytes).
inline bool pooled(ScopeId::Kind kind) { return kind == ScopeId::Player || kind == ScopeId::Object; }

enum class SetResult : uint8_t
{
	Ok, // changed
	Same, // newer, but the value is the one already there (or nil for a missing key): nothing changed
	BadKey, // the key is empty or longer than kMaxKey, or the scope is not well formed (scope_ok)
	BadValue, // unknown type, NaN / inf, a table in a table, nil in a table, a repeated or too long table key
	TooBig, // more than kMaxValueBytes encoded
	ScopeFull, // over the scope's limit, the pool, or the number of scopes (Stats says which)
	NotAllowed, // this machine may not write the scope (may_set)
	Stale, // not newer than what the store holds
};
constexpr size_t kSetResultCount = 8;

// Who may write a scope. by_host: the host writes; by_owner: the player of a Player scope, the owner of an Object.
inline bool may_set(ScopeId::Kind kind, bool by_host, bool by_owner)
{
	switch (kind)
	{
	case ScopeId::World:
	case ScopeId::Session:
		return by_host;
	case ScopeId::Player:
	case ScopeId::Object:
		return by_host || by_owner;
	}
	return false;
}

inline bool key_ok(std::string_view key) { return !key.empty() && key.size() <= kMaxKey; }

inline bool player_key_ok(std::string_view key)
{
	if (key.size() != kPlayerKeyDigits)
		return false;
	for (const char c : key)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	return true;
}

// One form per scope, so that one scope has one id: World / Session (arg 0, no key), Player (arg 0, a key digest),
// Object (no key).
inline bool scope_ok(const ScopeId& s)
{
	switch (s.kind)
	{
	case ScopeId::World:
	case ScopeId::Session:
		return s.arg == 0 && s.player_key.empty();
	case ScopeId::Player:
		return s.arg == 0 && player_key_ok(s.player_key);
	case ScopeId::Object:
		return s.player_key.empty();
	}
	return false;
}

// ---------------------------------------------------------------- value checks

// Bytes of an unsigned LEB128 (Writer::varu).
inline size_t varu_size(uint64_t n)
{
	size_t size = 1;
	while (n >= 0x80)
	{
		n >>= 7;
		++size;
	}
	return size;
}

// The size of write_value's bytes.
inline size_t encoded_size(const Value& v)
{
	size_t n = 1;
	switch (v.type)
	{
	case ValueType::Nil:
		break;
	case ValueType::Number:
		n += 8;
		break;
	case ValueType::String:
		n += varu_size(v.str.size()) + v.str.size();
		break;
	case ValueType::Bool:
		n += 1;
		break;
	case ValueType::Vec3:
		n += 12;
		break;
	case ValueType::Table:
		n += varu_size(v.table.size());
		for (const auto& f : v.table)
			n += varu_size(f.first.size()) + f.first.size() + encoded_size(f.second);
		break;
	}
	return n;
}

// Puts a table's fields in canonical order (by key). Other values stay as they are.
inline void canonicalize(Value& v)
{
	if (v.type != ValueType::Table)
		return;
	std::sort(v.table.begin(), v.table.end(),
	    [](const std::pair<std::string, Value>& a, const std::pair<std::string, Value>& b) { return a.first < b.first; });
}

// A value a table may hold: number, string, bool, vector; numbers finite.
inline bool scalar_ok(const Value& v)
{
	switch (v.type)
	{
	case ValueType::Number:
		return std::isfinite(v.num);
	case ValueType::String:
	case ValueType::Bool:
		return true;
	case ValueType::Vec3:
		return validate::finite3(v.v);
	case ValueType::Nil:
	case ValueType::Table:
		return false;
	}
	return false;
}

// Ok, BadValue or TooBig. A table must be in canonical order: keys strictly increasing (sorted, none repeated).
inline SetResult check_value(const Value& v)
{
	switch (v.type)
	{
	case ValueType::Nil:
		break;
	case ValueType::Number:
	case ValueType::String:
	case ValueType::Bool:
	case ValueType::Vec3:
		if (!scalar_ok(v))
			return SetResult::BadValue;
		break;
	case ValueType::Table:
		for (size_t i = 0; i < v.table.size(); ++i)
		{
			const auto& f = v.table[i];
			if (f.first.size() > kMaxKey || !scalar_ok(f.second))
				return SetResult::BadValue;
			if (i && !(v.table[i - 1].first < f.first))
				return SetResult::BadValue;
		}
		break;
	default:
		return SetResult::BadValue;
	}
	return encoded_size(v) > kMaxValueBytes ? SetResult::TooBig : SetResult::Ok;
}

// Same type and same contents (tables in canonical order).
inline bool same(const Value& a, const Value& b)
{
	if (a.type != b.type)
		return false;
	switch (a.type)
	{
	case ValueType::Nil:
		return true;
	case ValueType::Number:
		return a.num == b.num;
	case ValueType::String:
		return a.str == b.str;
	case ValueType::Bool:
		return a.b == b.b;
	case ValueType::Vec3:
		return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2];
	case ValueType::Table:
		if (a.table.size() != b.table.size())
			return false;
		for (size_t i = 0; i < a.table.size(); ++i)
			if (a.table[i].first != b.table[i].first || !same(a.table[i].second, b.table[i].second))
				return false;
		return true;
	}
	return false;
}

// What one entry counts in its scope: its encoded bytes (key, version, value) plus kEntryOverhead, plus kFieldOverhead
// per table field.
inline size_t entry_bytes(std::string_view key, const Value& v)
{
	const size_t fields = v.type == ValueType::Table ? v.table.size() : 0;
	return varu_size(key.size()) + key.size() + 8 + encoded_size(v) + kEntryOverhead + kFieldOverhead * fields;
}

// ---------------------------------------------------------------- wire
//
// Value: u8 type, then Number: f64 bits (u64); String: varu length + bytes; Bool: u8 0 / 1; Vec3: 3 x f32;
//   Table: varu count, count x (varu key length + key bytes, a scalar value); Nil: nothing.
// Scope: u8 kind, then Player: varu length + the key digest; Object: u32 id; World / Session: nothing.
// Set (write_set): scope, varu key length + key, u64 version, value.
// Full copy of a scope (encode_scope): scope, u64 the newest removal, varu count, count x (key, u64 version, value),
//   keys in increasing order.

static_assert(sizeof(double) == sizeof(uint64_t), "a number goes on the wire as 64 bits");

inline void write_value(Writer& w, const Value& v)
{
	w.u8(uint8_t(v.type));
	switch (v.type)
	{
	case ValueType::Nil:
		break;
	case ValueType::Number:
	{
		uint64_t bits;
		std::memcpy(&bits, &v.num, sizeof(bits));
		w.u64(bits);
		break;
	}
	case ValueType::String:
		w.varu(v.str.size());
		w.bytes(v.str.data(), v.str.size());
		break;
	case ValueType::Bool:
		w.u8(v.b ? 1 : 0);
		break;
	case ValueType::Vec3:
		w.vec3(v.v);
		break;
	case ValueType::Table:
		w.varu(v.table.size());
		for (const auto& f : v.table)
		{
			w.varu(f.first.size());
			w.bytes(f.first.data(), f.first.size());
			write_value(w, f.second);
		}
		break;
	}
}

inline void write_scope(Writer& w, const ScopeId& s)
{
	w.u8(uint8_t(s.kind));
	if (s.kind == ScopeId::Player)
	{
		w.varu(s.player_key.size());
		w.bytes(s.player_key.data(), s.player_key.size());
	}
	else if (s.kind == ScopeId::Object)
		w.u32(s.arg);
}

inline void write_set(Writer& w, const ScopeId& scope, std::string_view key, const Value& value, uint64_t version)
{
	write_scope(w, scope);
	w.varu(key.size());
	w.bytes(key.data(), key.size());
	w.u64(version);
	write_value(w, value);
}

namespace detail
{
// A length-prefixed string of at most `max` bytes. Its length is checked before anything is allocated.
inline bool read_text(Reader& r, size_t max, std::string& out)
{
	const uint64_t len = r.varu();
	if (!r.ok() || len > max || len > r.remaining())
		return false;
	out.assign(reinterpret_cast<const char*>(r.cursor()), size_t(len));
	return r.skip(size_t(len));
}

// The smallest table field on the wire: an empty key (1), a type (1), a bool (1).
constexpr size_t kMinFieldWire = 3;

// The payload of a value of `type` (its type byte read). `field`: a table's field (scalars only). `start`: where the
// value began, so that the whole of it stays within kMaxValueBytes while it is read.
inline bool read_payload(Reader& r, ValueType type, Value& out, bool field, const uint8_t* start)
{
	out = Value();
	out.type = type;
	switch (type)
	{
	case ValueType::Nil:
		return !field;
	case ValueType::Number:
	{
		const uint64_t bits = r.u64();
		std::memcpy(&out.num, &bits, sizeof(bits));
		return r.ok() && std::isfinite(out.num);
	}
	case ValueType::String:
		return read_text(r, kMaxValueBytes, out.str);
	case ValueType::Bool:
	{
		const uint8_t b = r.u8();
		out.b = b != 0;
		return r.ok() && b <= 1;
	}
	case ValueType::Vec3:
		r.vec3(out.v);
		return r.ok() && validate::finite3(out.v);
	case ValueType::Table:
	{
		if (field)
			return false;
		const uint64_t count = r.varu();
		if (!r.ok() || count > r.remaining() / kMinFieldWire)
			return false;
		for (uint64_t i = 0; i < count; ++i)
		{
			std::pair<std::string, Value> f;
			if (!read_text(r, kMaxKey, f.first))
				return false;
			const uint8_t t = r.u8();
			if (!r.ok() || t >= kValueTypeCount || !read_payload(r, ValueType(t), f.second, true, start))
				return false;
			if (size_t(r.cursor() - start) > kMaxValueBytes)
				return false;
			out.table.push_back(std::move(f));
		}
		return true;
	}
	}
	return false;
}
} // namespace detail

// One value from the network: a known type, at most kMaxValueBytes, finite numbers, a flat table with unique keys
// (put in canonical order). false on anything else, `out` then as it was.
inline bool read_value(Reader& r, Value& out)
{
	const uint8_t* start = r.cursor();
	const uint8_t t = r.u8();
	if (!r.ok() || t >= kValueTypeCount)
		return false;
	Value v;
	if (!detail::read_payload(r, ValueType(t), v, false, start) || size_t(r.cursor() - start) > kMaxValueBytes)
		return false;
	canonicalize(v);
	if (check_value(v) != SetResult::Ok)
		return false;
	out = std::move(v);
	return true;
}

// A scope from the network: a known kind in its one form (scope_ok). false otherwise, `out` then as it was.
inline bool read_scope(Reader& r, ScopeId& out)
{
	const uint8_t k = r.u8();
	if (!r.ok() || k >= kKindCount)
		return false;
	ScopeId s;
	s.kind = ScopeId::Kind(k);
	if (s.kind == ScopeId::Player)
	{
		if (!detail::read_text(r, kPlayerKeyDigits, s.player_key))
			return false;
	}
	else if (s.kind == ScopeId::Object)
		s.arg = r.u32();
	if (!r.ok() || !scope_ok(s))
		return false;
	out = std::move(s);
	return true;
}

// One change from the network, checked: the scope's form, the key (1..kMaxKey), the value (read_value). false on
// anything else or a short packet; the outputs are then as they were. The caller counts refusals.
inline bool read_set(Reader& r, ScopeId& scope, std::string& key, Value& value, uint64_t& version)
{
	ScopeId s;
	std::string k;
	Value v;
	if (!read_scope(r, s) || !detail::read_text(r, kMaxKey, k) || !key_ok(k))
		return false;
	const uint64_t ver = r.u64();
	if (!r.ok() || !read_value(r, v))
		return false;
	scope = std::move(s);
	key = std::move(k);
	value = std::move(v);
	version = ver;
	return true;
}

// ---------------------------------------------------------------- store

// Refusals and copies, for coop_status. Every set / set_by is counted by its result; a ScopeFull also by its cause.
struct Stats
{
	uint64_t results[kSetResultCount] = {};
	uint64_t full_scope = 0; // ScopeFull: the scope's own limit
	uint64_t full_pool = 0; // ScopeFull: all Player and Object scopes together (kMaxPoolBytes)
	uint64_t full_count = 0; // ScopeFull: too many Player or Object scopes
	uint64_t decoded = 0; // full copies taken
	uint64_t decode_bad = 0; // full copies refused: malformed, short, a bad key or value
	uint64_t decode_full = 0; // full copies refused: over a limit
};

class Store
{
public:
	// Applies one change (nil removes the key). Does not check who writes: see set_by.
	SetResult set(const ScopeId& scope, std::string_view key, Value value, uint64_t version)
	{
		const SetResult r = apply(scope, key, std::move(value), version);
		++m_stats.results[size_t(r)];
		return r;
	}

	// set() after may_set(): NotAllowed (counted) if this writer may not write the scope.
	SetResult set_by(const ScopeId& scope, std::string_view key, Value value, uint64_t version, bool by_host,
	    bool by_owner)
	{
		if (!may_set(scope.kind, by_host, by_owner))
		{
			++m_stats.results[size_t(SetResult::NotAllowed)];
			return SetResult::NotAllowed;
		}
		return set(scope, key, std::move(value), version);
	}

	// nullptr if there is none. Valid until the next change of the store.
	const Value* get(const ScopeId& scope, std::string_view key) const
	{
		const Entry* e = find(scope, key);
		return e ? &e->value : nullptr;
	}

	// The version of a key's last change; 0 if there is none.
	uint64_t version_of(const ScopeId& scope, std::string_view key) const
	{
		const Entry* e = find(scope, key);
		return e ? e->version : 0;
	}

	// The keys of a scope that start with `prefix` ("" for all), in increasing order. Valid until the next change.
	std::vector<std::pair<std::string, const Value*>> keys(const ScopeId& scope, std::string_view prefix) const
	{
		std::vector<std::pair<std::string, const Value*>> out;
		const auto it = m_scopes.find(scope);
		if (it == m_scopes.end())
			return out;
		const auto& entries = it->second.entries;
		for (auto e = entries.lower_bound(prefix);
		     e != entries.end() && e->first.compare(0, prefix.size(), prefix) == 0; ++e)
			out.emplace_back(e->first, &e->second.value);
		return out;
	}

	// The scope goes with everything in it (an object destroyed; a reset before a full copy).
	void drop(const ScopeId& scope)
	{
		const auto it = m_scopes.find(scope);
		if (it == m_scopes.end())
			return;
		if (pooled(scope.kind))
			m_pool -= it->second.bytes;
		--m_count[size_t(scope.kind)];
		m_scopes.erase(it);
	}

	// Every scope goes (a client before the host's full copy at join; the host before loading its save). Stats stay.
	void clear()
	{
		m_scopes.clear();
		m_pool = 0;
		for (size_t& c : m_count)
			c = 0;
		m_top = 0;
	}

	// What the scope counts against its limit (entry_bytes of its entries); 0 if there is none.
	size_t bytes(const ScopeId& scope) const
	{
		const auto it = m_scopes.find(scope);
		return it == m_scopes.end() ? 0 : it->second.bytes;
	}

	// All Player and Object scopes together.
	size_t pool_bytes() const { return m_pool; }

	// The full copy of a scope (an empty one for a scope the store does not have: taking it removes the scope).
	void encode_scope(Writer& w, const ScopeId& scope) const
	{
		write_scope(w, scope);
		const auto it = m_scopes.find(scope);
		if (it == m_scopes.end())
		{
			w.u64(0);
			w.varu(0);
			return;
		}
		w.u64(it->second.removed);
		w.varu(it->second.entries.size());
		for (const auto& kv : it->second.entries)
		{
			w.varu(kv.first.size());
			w.bytes(kv.first.data(), kv.first.size());
			w.u64(kv.second.version);
			write_value(w, kv.second.value);
		}
	}

	// Takes a full copy of one scope: the scope becomes exactly the copy (nothing of before stays). Everything is
	// checked first (form, keys in increasing order, values, versions, the scope's limit, the pool, the number of
	// scopes); a refused copy changes nothing and is counted. `out`: the scope taken; as it was on a refusal.
	bool decode_scope(Reader& r, ScopeId& out)
	{
		ScopeId id;
		Scope fresh;
		uint64_t top = 0;
		bool full = false;
		if (!read_copy(r, id, fresh, top, full))
		{
			++(full ? m_stats.decode_full : m_stats.decode_bad);
			return false;
		}
		const auto it = m_scopes.find(id);
		const size_t old = it == m_scopes.end() ? 0 : it->second.bytes;
		const bool keep = !fresh.entries.empty() || fresh.removed != 0;
		if ((pooled(id.kind) && m_pool - old + fresh.bytes > kMaxPoolBytes) ||
		    (keep && it == m_scopes.end() && m_count[size_t(id.kind)] >= max_scopes(id.kind)))
		{
			++m_stats.decode_full;
			return false;
		}
		if (pooled(id.kind))
			m_pool = m_pool - old + fresh.bytes;
		if (it != m_scopes.end())
		{
			if (keep)
				it->second = std::move(fresh);
			else
			{
				m_scopes.erase(it);
				--m_count[size_t(id.kind)];
			}
		}
		else if (keep)
		{
			m_scopes.emplace(id, std::move(fresh));
			++m_count[size_t(id.kind)];
		}
		m_top = std::max(m_top, top);
		out = std::move(id);
		++m_stats.decoded;
		return true;
	}

	// The scopes the store has, in order.
	std::vector<ScopeId> scopes() const
	{
		std::vector<ScopeId> out;
		out.reserve(m_scopes.size());
		for (const auto& kv : m_scopes)
			out.push_back(kv.first);
		return out;
	}

	// The newest version the store has taken (a change or a copy) since it was made or cleared: the host stamps the
	// next change with top_version() + 1.
	uint64_t top_version() const { return m_top; }

	const Stats& stats() const { return m_stats; }

private:
	struct Entry
	{
		Value value;
		uint64_t version = 0;
		size_t cost = 0; // entry_bytes
	};
	struct Scope
	{
		std::map<std::string, Entry, std::less<>> entries;
		uint64_t removed = 0; // the newest version of a removal in this scope: older changes of missing keys are Stale
		size_t bytes = 0; // sum of the entries' cost
	};

	const Entry* find(const ScopeId& scope, std::string_view key) const
	{
		const auto it = m_scopes.find(scope);
		if (it == m_scopes.end())
			return nullptr;
		const auto e = it->second.entries.find(key);
		return e == it->second.entries.end() ? nullptr : &e->second;
	}

	SetResult apply(const ScopeId& scope, std::string_view key, Value value, uint64_t version)
	{
		if (!scope_ok(scope) || !key_ok(key))
			return SetResult::BadKey;
		canonicalize(value);
		const SetResult form = check_value(value);
		if (form != SetResult::Ok)
			return form;

		const auto sit = m_scopes.find(scope);
		Scope* sc = sit == m_scopes.end() ? nullptr : &sit->second;
		const auto eit = sc ? sc->entries.find(key) : decltype(sc->entries.end())();
		Entry* e = sc && eit != sc->entries.end() ? &eit->second : nullptr;
		if (version <= (e ? e->version : sc ? sc->removed : 0))
			return SetResult::Stale;

		if (value.type == ValueType::Nil)
		{
			m_top = std::max(m_top, version);
			if (!e)
			{
				if (sc)
					sc->removed = std::max(sc->removed, version);
				return SetResult::Same;
			}
			// The scope stays, empty or not: it keeps the version of the removal until it is dropped.
			sc->bytes -= e->cost;
			if (pooled(scope.kind))
				m_pool -= e->cost;
			sc->removed = std::max(sc->removed, version);
			sc->entries.erase(eit);
			return SetResult::Ok;
		}
		if (e && same(e->value, value))
		{
			e->version = version;
			m_top = std::max(m_top, version);
			return SetResult::Same;
		}

		const size_t cost = entry_bytes(key, value);
		const size_t old = e ? e->cost : 0;
		const size_t scope_after = (sc ? sc->bytes : 0) - old + cost;
		if (scope_after > scope_limit(scope.kind))
		{
			++m_stats.full_scope;
			return SetResult::ScopeFull;
		}
		if (pooled(scope.kind) && m_pool - old + cost > kMaxPoolBytes)
		{
			++m_stats.full_pool;
			return SetResult::ScopeFull;
		}
		if (!sc && m_count[size_t(scope.kind)] >= max_scopes(scope.kind))
		{
			++m_stats.full_count;
			return SetResult::ScopeFull;
		}

		if (!sc)
		{
			sc = &m_scopes[scope];
			++m_count[size_t(scope.kind)];
		}
		if (e)
		{
			e->value = std::move(value);
			e->version = version;
			e->cost = cost;
		}
		else
		{
			Entry fresh;
			fresh.value = std::move(value);
			fresh.version = version;
			fresh.cost = cost;
			sc->entries.emplace(std::string(key), std::move(fresh));
		}
		sc->bytes = scope_after;
		if (pooled(scope.kind))
			m_pool = m_pool - old + cost;
		m_top = std::max(m_top, version);
		return SetResult::Ok;
	}

	// The smallest entry of a full copy: a 1-byte key (2), a version (8), a bool (2).
	static constexpr size_t kMinEntryWire = 12;

	// Reads and checks a full copy into `sc`; `top`: its newest version. `full`: refused for a limit, not its form.
	static bool read_copy(Reader& r, ScopeId& id, Scope& sc, uint64_t& top, bool& full)
	{
		if (!read_scope(r, id))
			return false;
		sc.removed = r.u64();
		const uint64_t count = r.varu();
		if (!r.ok() || count > r.remaining() / kMinEntryWire)
			return false;
		top = sc.removed;
		const std::string* prev = nullptr;
		for (uint64_t i = 0; i < count; ++i)
		{
			std::string key;
			Entry e;
			if (!detail::read_text(r, kMaxKey, key) || !key_ok(key) || (prev && !(*prev < key)))
				return false;
			e.version = r.u64();
			if (!r.ok() || e.version == 0 || !read_value(r, e.value) || e.value.type == ValueType::Nil)
				return false;
			e.cost = entry_bytes(key, e.value);
			sc.bytes += e.cost;
			if (sc.bytes > scope_limit(id.kind))
			{
				full = true;
				return false;
			}
			top = std::max(top, e.version);
			prev = &sc.entries.emplace_hint(sc.entries.end(), std::move(key), std::move(e))->first;
		}
		return r.ok();
	}

	std::map<ScopeId, Scope> m_scopes;
	size_t m_count[kKindCount] = {}; // scopes by kind
	size_t m_pool = 0; // bytes of all Player and Object scopes
	uint64_t m_top = 0;
	Stats m_stats;
};
} // namespace syncstore
} // namespace coop
