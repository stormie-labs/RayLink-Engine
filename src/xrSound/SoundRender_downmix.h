// Anomaly Coop: a stereo source played as a positional (3D) sound, sm_Mono3D (TZ R59, section 6.14). OpenAL places only mono
// buffers in space, so the stock engine plays every stereo source flat (SoundRender_Core.cpp play / play_at_pos /
// play_no_feedback). With sm_Mono3D the sound target mixes each stereo block down to mono as it streams.
// Pure C++17, header-only, no engine dependencies: tests/test_sound_downmix.cpp includes it as it is. Engine side:
// CSoundRender_Core::play* (the decision), CSoundRender_TargetA::fill_block (the mixing).
//
// The emitter keeps counting in the SOURCE's bytes (16-bit stereo, 4 per frame): its cursor, length, end of sound, loop,
// attach_tail, rewind and calc_cursor are those of stock. Only the block handed to OpenAL changes: N whole stereo frames in,
// N mono samples out at the same rate, so the same frames last the same time.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sound_downmix
{
constexpr uint32_t kStereoFrameBytes = 4; // 16-bit L, 16-bit R
constexpr uint32_t kMonoFrameBytes = 2;

// Whether a NEW emitter mixes its source down: only on request (sm_Mono3D), only a stereo source, and not under sm_2D (a flat
// sound keeps its stock stereo). Latched for the emitter's life: OpenAL keeps one format per buffer queue.
inline bool starts_downmixed(bool asked_2d, bool asked_mono3d, uint32_t channels)
{
	return asked_mono3d && !asked_2d && channels == 2;
}

// Whether the emitter plays flat after a play call. Stock: sm_2D, or a stereo source. A source mixed down stays positional
// unless sm_2D is asked. With downmixed == false this is exactly the stock condition.
inline bool plays_flat(bool asked_2d, uint32_t channels, bool downmixed)
{
	return asked_2d || (channels == 2 && !downmixed);
}

// One mono sample of a stereo frame: the mean of L and R, truncated toward zero (symmetric, no DC drift), clamped to 16 bit.
// The mean of two 16-bit values always fits: the clamp only guards.
inline int16_t mix(int16_t l, int16_t r)
{
	const int32_t m = (int32_t(l) + int32_t(r)) / 2;
	return int16_t(m < -32768 ? -32768 : (m > 32767 ? 32767 : m));
}

// The bytes of one target block: the most whole stereo frames that fit both buffers. source_bytes is what the emitter is asked
// for (a multiple of 4, so its cursor stays on a frame and L stays L), mono_bytes what OpenAL gets (half of it).
struct Block
{
	uint32_t source_bytes;
	uint32_t mono_bytes;
};

inline Block plan_block(uint32_t stereo_capacity, uint32_t mono_capacity)
{
	uint32_t frames = stereo_capacity / kStereoFrameBytes;
	const uint32_t fit = mono_capacity / kMonoFrameBytes;
	if (fit < frames)
		frames = fit;
	return Block{frames * kStereoFrameBytes, frames * kMonoFrameBytes};
}

// Interleaved 16-bit little-endian stereo (as ov_read fills the cache, bigendianp 0) -> 16-bit little-endian mono, one sample
// per whole frame; a trailing partial frame is ignored. Returns the mono bytes written. out may be in itself (in place: sample
// i is written after frame i is read, and later frames lie further on); any other overlap is not allowed.
inline uint32_t stereo_to_mono_s16le(const uint8_t* in, uint32_t in_bytes, uint8_t* out)
{
	const uint32_t frames = in_bytes / kStereoFrameBytes;
	for (uint32_t i = 0; i < frames; ++i)
	{
		const uint8_t* f = in + size_t(i) * kStereoFrameBytes;
		const int16_t l = int16_t(uint16_t(f[0]) | uint16_t(uint16_t(f[1]) << 8));
		const int16_t r = int16_t(uint16_t(f[2]) | uint16_t(uint16_t(f[3]) << 8));
		const uint16_t m = uint16_t(mix(l, r));
		uint8_t* o = out + size_t(i) * kMonoFrameBytes;
		o[0] = uint8_t(m & 0xff);
		o[1] = uint8_t(m >> 8);
	}
	return frames * kMonoFrameBytes;
}

// Seconds that `bytes` of 16-bit PCM with `channels` channels last at `rate` Hz (0 for no rate or no channels). A mono block
// and the stereo bytes it was made from last the same: seconds_of(b.mono_bytes, rate, 1) == seconds_of(b.source_bytes, rate, 2).
inline double seconds_of(uint64_t bytes, uint32_t rate, uint32_t channels)
{
	return rate && channels ? double(bytes) / (double(rate) * 2.0 * double(channels)) : 0.0;
}
} // namespace sound_downmix
