// Anomaly Coop — checks for numbers and names that come from the network (R3 K1, K2).
// The host is authoritative and trusts no peer (P8): a value from a packet is checked before it reaches the engine.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cmath>
#include <cstddef>

namespace coop
{
namespace validate
{
// Not NaN and not +-inf.
inline bool finite(float x)
{
	return std::isfinite(x);
}

inline bool finite3(const float v[3])
{
	return finite(v[0]) && finite(v[1]) && finite(v[2]);
}

// |v| <= max_speed. A non-finite component or a NaN / negative limit fails. Summed in double: squares of huge
// finite floats would overflow a float.
inline bool speed_ok(const float v[3], float max_speed)
{
	if (!finite3(v) || !(max_speed >= 0.f))
		return false;
	const double x = v[0], y = v[1], z = v[2];
	return std::sqrt(x * x + y * y + z * z) <= double(max_speed);
}

// p is inside [lo, hi] grown by margin on every side, borders included: same float math as Fbox::grow + contains.
// A non-finite point, box or margin fails.
inline bool in_box(const float p[3], const float lo[3], const float hi[3], float margin)
{
	if (!finite3(p) || !finite3(lo) || !finite3(hi) || !finite(margin))
		return false;
	for (std::size_t i = 0; i < 3; ++i)
		if (p[i] < lo[i] - margin || p[i] > hi[i] + margin)
			return false;
	return true;
}

// A resource name from a peer (model, section...): not empty, at most max_len chars, printable ASCII only
// (0x20..0x7E), no ".." (out of the resource folder) and no ':' (drive letter, NTFS stream).
// Reads at most max_len + 1 chars of s.
inline bool resource_name_ok(const char* s, std::size_t max_len)
{
	if (!s || !*s)
		return false;
	for (std::size_t n = 0; s[n]; ++n)
	{
		if (n >= max_len)
			return false;
		const unsigned char c = static_cast<unsigned char>(s[n]);
		if (c < 0x20 || c > 0x7E || c == ':')
			return false;
		if (c == '.' && s[n + 1] == '.')
			return false;
	}
	return true;
}

// The engine's "no bone" (BI_NONE = u16(-1)): the default of SHit, every hit path handles it.
constexpr unsigned kNoBone = 0xffffu;

// R3 В5: a hit's bone from a peer indexes the target's skeleton unchecked (CPHShell::applyImpulseTrace,
// LL_GetBoneInstance, hit scales, wounds): it is a bone of that skeleton or "no bone".
inline bool bone_ok(unsigned bone, unsigned bone_count)
{
	return bone == kNoBone || bone < bone_count;
}
} // namespace validate
} // namespace coop
