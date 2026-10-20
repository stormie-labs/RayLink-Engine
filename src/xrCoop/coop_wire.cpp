// Anomaly Coop — wire serialization implementation.
#include "coop_wire.h"

#include <cmath>
#include <cstring>

namespace coop
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;
}

// ---------------------------------------------------------------- Writer

void Writer::u8(uint8_t v)
{
	m_buf.push_back(v);
}

void Writer::u16(uint16_t v)
{
	m_buf.push_back(uint8_t(v));
	m_buf.push_back(uint8_t(v >> 8));
}

void Writer::u32(uint32_t v)
{
	for (int i = 0; i < 4; ++i)
		m_buf.push_back(uint8_t(v >> (8 * i)));
}

void Writer::u64(uint64_t v)
{
	for (int i = 0; i < 8; ++i)
		m_buf.push_back(uint8_t(v >> (8 * i)));
}

void Writer::i32(int32_t v)
{
	u32(static_cast<uint32_t>(v));
}

void Writer::f32(float v)
{
	uint32_t bits;
	static_assert(sizeof(bits) == sizeof(v), "float must be 32 bit");
	std::memcpy(&bits, &v, sizeof(bits));
	u32(bits);
}

void Writer::varu(uint64_t v)
{
	while (v >= 0x80)
	{
		m_buf.push_back(uint8_t(v) | 0x80);
		v >>= 7;
	}
	m_buf.push_back(uint8_t(v));
}

void Writer::str(std::string_view s)
{
	if (s.size() > kMaxString)
		s = s.substr(0, kMaxString);
	varu(s.size());
	bytes(s.data(), s.size());
}

void Writer::bytes(const void* data, size_t size)
{
	if (!size || !data)
		return;
	const uint8_t* p = static_cast<const uint8_t*>(data);
	m_buf.insert(m_buf.end(), p, p + size);
}

void Writer::vec3(const float v[3])
{
	f32(v[0]);
	f32(v[1]);
	f32(v[2]);
}

void Writer::angle16(float radians)
{
	double a = std::fmod(double(radians), kTwoPi);
	if (!(a == a)) // NaN -> 0
		a = 0.0;
	if (a < 0.0)
		a += kTwoPi;
	// Round to nearest step; 65536 wraps back to 0 (same angle).
	const long q = std::lround(a / kTwoPi * 65536.0);
	u16(uint16_t(q & 0xFFFF));
}

// ---------------------------------------------------------------- Reader

bool Reader::need(size_t n)
{
	if (m_failed)
		return false;
	if (size_t(m_end - m_p) < n)
	{
		m_failed = true;
		m_p = m_end;
		return false;
	}
	return true;
}

uint8_t Reader::u8()
{
	if (!need(1))
		return 0;
	return *m_p++;
}

uint16_t Reader::u16()
{
	if (!need(2))
		return 0;
	const uint16_t v = uint16_t(m_p[0] | (uint16_t(m_p[1]) << 8));
	m_p += 2;
	return v;
}

uint32_t Reader::u32()
{
	if (!need(4))
		return 0;
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i)
		v |= uint32_t(m_p[i]) << (8 * i);
	m_p += 4;
	return v;
}

uint64_t Reader::u64()
{
	if (!need(8))
		return 0;
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i)
		v |= uint64_t(m_p[i]) << (8 * i);
	m_p += 8;
	return v;
}

int32_t Reader::i32()
{
	return static_cast<int32_t>(u32());
}

float Reader::f32()
{
	const uint32_t bits = u32();
	float v;
	std::memcpy(&v, &bits, sizeof(v));
	return v;
}

uint64_t Reader::varu()
{
	uint64_t v = 0;
	for (int shift = 0; shift < 70; shift += 7)
	{
		if (!need(1))
			return 0;
		const uint8_t b = *m_p++;
		if (shift == 63 && (b & 0x7E)) // would overflow 64 bits
		{
			m_failed = true;
			m_p = m_end;
			return 0;
		}
		v |= uint64_t(b & 0x7F) << shift;
		if (!(b & 0x80))
			return v;
	}
	// More than 10 continuation bytes: malformed.
	m_failed = true;
	m_p = m_end;
	return 0;
}

std::string Reader::str()
{
	const uint64_t len = varu();
	if (m_failed)
		return std::string();
	if (len > Writer::kMaxString || !need(size_t(len)))
	{
		m_failed = true;
		m_p = m_end;
		return std::string();
	}
	std::string s(reinterpret_cast<const char*>(m_p), size_t(len));
	m_p += len;
	return s;
}

bool Reader::bytes(void* out, size_t size)
{
	if (!need(size))
		return false;
	if (size)
		std::memcpy(out, m_p, size);
	m_p += size;
	return true;
}

void Reader::vec3(float out[3])
{
	out[0] = f32();
	out[1] = f32();
	out[2] = f32();
}

float Reader::angle16()
{
	const uint16_t q = u16();
	return float(double(q) * (kTwoPi / 65536.0));
}

bool Reader::skip(size_t size)
{
	if (!need(size))
		return false;
	m_p += size;
	return true;
}
} // namespace coop
