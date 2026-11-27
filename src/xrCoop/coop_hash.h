// Anomaly Coop — FNV-1a, the one hash of names and states that the two ends of a session compare.
// Every digest and wire tag built on it must stay bit-exact: the same basis, the same prime, the same fold. A place that
// hashes something else (a word, a length prefix) feeds it through fnv1a32_step / fnv1a64_step itself.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace coop
{
constexpr uint32_t kFnv32Basis = 2166136261u;
constexpr uint32_t kFnv32Prime = 16777619u;
constexpr uint64_t kFnv64Basis = 14695981039346656037ull;
constexpr uint64_t kFnv64Prime = 1099511628211ull;

// One step: `v` is a byte, or a whole word where a digest mixes words (itemtags::state_digest).
constexpr uint32_t fnv1a32_step(uint32_t h, uint32_t v) { return (h ^ v) * kFnv32Prime; }
constexpr uint64_t fnv1a64_step(uint64_t h, uint64_t v) { return (h ^ v) * kFnv64Prime; }

inline uint32_t fnv1a32(std::string_view text)
{
	uint32_t h = kFnv32Basis;
	for (const char c : text)
		h = fnv1a32_step(h, uint8_t(c));
	return h;
}

// A section name as the engine hands it over: nullptr hashes as "".
inline uint32_t fnv1a32(const char* text) { return fnv1a32(text ? std::string_view(text) : std::string_view()); }

inline uint32_t fnv1a32(const uint8_t* data, size_t size)
{
	uint32_t h = kFnv32Basis;
	for (size_t i = 0; i < size; ++i)
		h = fnv1a32_step(h, data[i]);
	return h;
}

inline uint64_t fnv1a64(const uint8_t* data, size_t size, uint64_t h = kFnv64Basis)
{
	for (size_t i = 0; i < size; ++i)
		h = fnv1a64_step(h, data[i]);
	return h;
}

// A 32-bit hash as a u16 tag: its two halves xor-ed.
constexpr uint16_t fold16(uint32_t h) { return uint16_t((h >> 16) ^ (h & 0xffffu)); }
} // namespace coop
