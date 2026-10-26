// Anomaly Coop — wire serialization (little-endian, bounds-checked).
// Pure C++17, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coop
{
// Appends values to a growable byte buffer.
class Writer
{
public:
	Writer() = default;
	explicit Writer(size_t reserve) { m_buf.reserve(reserve); }

	void u8(uint8_t v);
	void u16(uint16_t v);
	void u32(uint32_t v);
	void u64(uint64_t v);
	void i32(int32_t v);
	void f32(float v);
	void boolean(bool v) { u8(v ? 1 : 0); }
	// Unsigned LEB128.
	void varu(uint64_t v);
	// varu length + raw bytes. Strings longer than kMaxString are truncated.
	void str(std::string_view s);
	void bytes(const void* data, size_t size);
	void vec3(const float v[3]);
	// Angle in radians quantized to 16 bits over [0, 2*pi).
	void angle16(float radians);

	const std::vector<uint8_t>& data() const { return m_buf; }
	const uint8_t* ptr() const { return m_buf.data(); }
	size_t size() const { return m_buf.size(); }
	void clear() { m_buf.clear(); }

	static constexpr size_t kMaxString = 64 * 1024;

private:
	std::vector<uint8_t> m_buf;
};

// Reads values from a byte range. Never throws and never reads out of bounds:
// on overrun it sets the failed flag and every later read returns zero/empty.
class Reader
{
public:
	Reader(const uint8_t* data, size_t size) : m_p(data), m_end(data + size) {}

	uint8_t u8();
	uint16_t u16();
	uint32_t u32();
	uint64_t u64();
	int32_t i32();
	float f32();
	bool boolean() { return u8() != 0; }
	uint64_t varu();
	std::string str();
	// Copies size bytes into out; false (and failed) on overrun.
	bool bytes(void* out, size_t size);
	void vec3(float out[3]);
	float angle16();

	bool ok() const { return !m_failed; }
	size_t remaining() const { return m_failed ? 0 : size_t(m_end - m_p); }
	const uint8_t* cursor() const { return m_p; }
	// Skips size bytes; false on overrun.
	bool skip(size_t size);

private:
	bool need(size_t n);

	const uint8_t* m_p;
	const uint8_t* m_end;
	bool m_failed = false;
};
} // namespace coop
