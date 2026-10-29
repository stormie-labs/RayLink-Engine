// Bounded instrument intent. Resource paths and motion names never come from the peer.
#pragma once
#include "coop_wire.h"
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace coop { namespace instrument {
constexpr uint32_t kMaxSongMs = 600000;
constexpr uint32_t kTransitionMs = 15000;
constexpr uint32_t kNoMotion = 0xffff;
template <class FileSystem, class Path>
bool resource_exists(FileSystem& fs, Path& path, const char* alias, const char* name, const char* extension)
{
    // The four-argument overload concatenates the extension and cannot accept nullptr.
    if (extension)
        return fs.exist(path, alias, name, extension);
    return fs.exist(path, alias, name);
}
struct State
{
    uint8_t kind = 0, phase = 0, track = 0; // kind 1 guitar/2 harmonica; phase 1 enter/2 play/3 leave
    uint32_t serial = 0, age_ms = 0, phase_ms = 0, duration_ms = 0;
    bool valid() const
    {
        if (!kind) return !phase && !track && !serial && !age_ms && !phase_ms && !duration_ms;
        if (kind > 2 || phase < 1 || phase > 3 || !serial || age_ms > kMaxSongMs + 2*kTransitionMs ||
            phase_ms > age_ms || duration_ms > kMaxSongMs) return false;
        if (phase == 1) return !track && !duration_ms && phase_ms <= kTransitionMs;
        if (phase == 2) return track >= 1 && track <= (kind == 1 ? 28 : 5) && duration_ms > 0 && phase_ms <= duration_ms;
        return !track && !duration_ms && phase_ms <= kTransitionMs;
    }
    void write(Writer& w) const
    {
        w.u8(kind); w.u8(phase); w.u8(track); w.u32(serial); w.u32(age_ms); w.u32(phase_ms); w.u32(duration_ms);
    }
    bool read(Reader& r)
    {
        State s;
        s.kind=r.u8(); s.phase=r.u8(); s.track=r.u8(); s.serial=r.u32(); s.age_ms=r.u32(); s.phase_ms=r.u32(); s.duration_ms=r.u32();
        if (!r.ok() || !s.valid()) return false;
        *this=s; return true;
    }
};
inline void advance(State& s, uint32_t dt)
{
    if (!s.kind) return;
    dt = std::min(dt, kMaxSongMs + 2*kTransitionMs);
    s.age_ms += dt; s.phase_ms += dt;
    if (s.phase == 2 && s.phase_ms >= s.duration_ms)
    {
        s.phase_ms -= s.duration_ms; s.phase=3; s.track=0; s.duration_ms=0;
    }
    if ((s.phase != 2 && s.phase_ms >= kTransitionMs) || s.age_ms > kMaxSongMs + 2*kTransitionMs) s=State();
}
// Smooth time only inside the same song/transition. A new serial or phase is a discrete event.
inline State interpolate(const State& a, const State& b, float factor)
{
    const float k=std::isfinite(factor) ? std::max(0.f,std::min(1.f,factor)) : 0.f;
    State out=k<.5f ? a : b;
    if (a.kind && a.kind==b.kind && a.serial==b.serial && a.phase==b.phase &&
        a.track==b.track && a.duration_ms==b.duration_ms)
    {
        out.age_ms=uint32_t(double(a.age_ms)+(double(b.age_ms)-double(a.age_ms))*k+.5);
        out.phase_ms=uint32_t(double(a.phase_ms)+(double(b.phase_ms)-double(a.phase_ms))*k+.5);
    }
    return out;
}
inline bool finite3(float x, float y, float z)
{
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
inline bool stationary(uint32_t movement, float x, float y, float z, bool alive)
{
    // actor_defs.h: direction 0..3, jump/fall/landing/climb 7..11. Crouch/turn alone are allowed.
    return alive && !(movement & 0xf8f) && finite3(x,y,z) &&
        x*x+y*y+z*z < .16f;
}
inline bool motion_present(uint32_t m) { return m != kNoMotion; }
inline bool prefer_blend(bool live, bool fading, float weight, bool best_live, bool best_fading, float best_weight)
{
    return live && (!best_live || (best_fading && !fading) || (fading == best_fading && weight >= best_weight));
}
// Live b29 (2026-10-29): client NPC puppets flashed a T-pose in fights. A stalker's hit flinch (character_hit_animations.cpp)
// plays hit_downl/r, hitback17... on channels 2 and 3 with weight = hit power (1..3), and those channels ADD key deltas
// over the motion's first frame (Animation.cpp: {lerp, add}); the motions themselves are authored on the bind pose. The
// host picked the heavier/later flinch, the client replayed it as a channel-0 cycle: the bind pose. Only channel 0 is a pose.
constexpr uint8_t kMirrorChannel = 0;
inline bool mirrored_channel(uint8_t channel) { return channel == kMirrorChannel; }
struct BlendView { bool live = false, fading = false; float weight = 0.f; uint8_t channel = 0; };
// Host capture: the blend of one bone partition the mirror replays (index), or n when there is none.
template <class View>
uint32_t pick_mirror_blend(uint32_t n, View view)
{
    uint32_t best = n;
    BlendView b;
    for (uint32_t i = 0; i < n; ++i)
    {
        const BlendView c = view(i);
        if (mirrored_channel(c.channel) &&
            prefer_blend(c.live, c.fading, c.weight, best != n, best != n && b.fading, best != n ? b.weight : 0.f))
        {
            best = i;
            b = c;
        }
    }
    return best;
}
inline bool phase_needs_sync(float host, float local) { return std::fabs(host-local) > .25f; }
} }
