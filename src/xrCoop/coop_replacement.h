#pragma once

#include "coop_spawn.h"
#include <string>
#include <vector>

namespace coop
{
namespace replacement
{
constexpr uint8_t kMarker = 0xE1;

struct Snapshot
{
	uint16_t source = 0xffff, target = 0xffff;
	uint64_t nonce = 0;
	std::string source_name;
	std::string metadata;
	std::vector<uint8_t> native;
};

inline bool encode(const Snapshot& value, std::vector<uint8_t>& out)
{
	out.clear();
	if (value.target == 0 || value.target == 0xffff || !value.nonce || value.source_name.size() > 255 ||
	    value.metadata.empty() || value.metadata.size() > kMaxItemState || value.native.size() > kMaxItemState)
		return false;
	Writer w;
	w.u8(kMarker);
	w.u16(value.source);
	w.u16(value.target);
	w.u64(value.nonce);
	w.str(value.source_name);
	w.str(value.metadata);
	w.varu(value.native.size());
	if (!value.native.empty())
		w.bytes(value.native.data(), value.native.size());
	if (w.data().size() > kMaxItemState)
		return false;
	out = w.data();
	return true;
}

inline bool decode(const uint8_t* bytes, size_t size, Snapshot& out)
{
	out = Snapshot();
	if (!bytes || !size || size > kMaxItemState)
		return false;
	Reader r(bytes, size);
	if (r.u8() != kMarker)
		return false;
	Snapshot value;
	value.source = r.u16();
	value.target = r.u16();
	value.nonce = r.u64();
	value.source_name = r.str();
	value.metadata = r.str();
	const uint64_t count = r.varu();
	if (!r.ok() || count > r.remaining() || count > kMaxItemState || value.source_name.size() > 255 ||
	    value.metadata.empty() || !value.nonce || value.target == 0 || value.target == 0xffff)
		return false;
	value.native.assign(r.cursor(), r.cursor() + size_t(count));
	if (!r.skip(size_t(count)) || r.remaining())
		return false;
	out = std::move(value);
	return true;
}

inline std::string token(const Snapshot& value)
{
	return value.source_name + "/" + std::to_string(value.target) + "/" + std::to_string(value.nonce);
}
} // namespace replacement
} // namespace coop
