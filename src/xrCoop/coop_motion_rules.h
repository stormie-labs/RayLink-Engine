// Anomaly Coop - the script cycle of a world physic object across machines (live 2026-11-09, the X-16 levers).
//
// A ph_button (levers and switches: X-16 yan_labx16_switcher1..3 and _primary, Pripyat pas_b400, ...) shows its state only by the
// cycle its section plays: stock ph_button.script reset_scheme calls object:play_cycle(st.anim, st.blending), e.g.
// lab_primary_switcher_idle (up) and lab_primary_switcher_off (down). CScriptGameObject::play_cycle starts a new blend of the
// visual; it is not CPhysicObject's own blend (m_anim_blend, the startup animation) whose phase the EntityState Anim sync
// carries. A client's press runs on the host (coop_objects client_intercept_use), the client's copy of the logic never enters
// the pressed section, so the lever went down on the host alone, and a player who came in later saw every lever up.
//
// Now the host (the map authority) owns these cycles. Every script play_cycle on a free physic object is said to the players
// of its world (EntityState kind Motion), and its last word on each object to a player who enters (host_on_peer_ready). A
// client replays the call as the script made it - the same cycle, the same mix-in - moved on by the time the host has played
// it, and holds its copy to that word: once a word is here, its own logic's play_cycle on that object is dropped, and a copy
// that comes online again gets the word again. Nothing else of the logic runs for it: no use callback, no section switch.
// Single player and a session without the hook: no change (the host records only in a session).
//
// Wire (Msg::EntityState, kind Motion): u8 count, count x {u16 object (host numbering), str cycle, u8 mix_in, u32 played_ms}.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "coop_wire.h"

namespace coop
{
namespace motion
{
constexpr size_t kMaxCycleName = 64; // a motion name of an .omf; longer is no cycle
constexpr uint64_t kCheckMs = 250; // client: how often its copies are held to the host's word
constexpr uint32_t kMaxPlayedMs = 0x7fffffffu;

struct Word
{
	uint16_t id = 0xFFFF; // host numbering on the wire, local after the client maps it
	std::string cycle;
	bool mix_in = true;
	uint32_t played_ms = 0; // how long the host has played it when the word left
};

inline void write_word(Writer& w, const Word& word)
{
	w.u16(word.id);
	w.str(word.cycle);
	w.u8(word.mix_in ? 1 : 0);
	w.u32(word.played_ms);
}

// False on an overrun, no cycle or a name no motion has, a mix-in that is no bool, or a time past kMaxPlayedMs.
inline bool read_word(Reader& r, Word& word)
{
	word.id = r.u16();
	word.cycle = r.str();
	const uint8_t mix = r.u8();
	word.played_ms = r.u32();
	word.mix_in = mix != 0;
	return r.ok() && !word.cycle.empty() && word.cycle.size() <= kMaxCycleName && mix <= 1 &&
		word.played_ms <= kMaxPlayedMs;
}

// Host: the last cycle a script played on each object of this world, and when.
class Book
{
public:
	struct Entry
	{
		std::string cycle;
		bool mix_in = true;
		uint64_t at = 0;
	};

	void note(uint16_t id, const std::string& cycle, bool mix_in, uint64_t now_ms)
	{
		Entry& e = m_entries[id];
		e.cycle = cycle;
		e.mix_in = mix_in;
		e.at = now_ms;
	}
	bool has(uint16_t id) const { return m_entries.count(id) != 0; }
	// The word on `id` as it leaves at `now_ms`; false if no script played a cycle on it.
	bool word(uint16_t id, uint64_t now_ms, Word& out) const
	{
		const auto it = m_entries.find(id);
		if (it == m_entries.end())
			return false;
		out.id = id;
		out.cycle = it->second.cycle;
		out.mix_in = it->second.mix_in;
		out.played_ms = played(it->second.at, now_ms);
		return true;
	}
	void clear() { m_entries.clear(); }
	size_t size() const { return m_entries.size(); }
	const std::map<uint16_t, Entry>& entries() const { return m_entries; }

	static uint32_t played(uint64_t at, uint64_t now_ms)
	{
		const uint64_t d = now_ms > at ? now_ms - at : 0;
		return d > kMaxPlayedMs ? kMaxPlayedMs : uint32_t(d);
	}

private:
	std::map<uint16_t, Entry> m_entries;
};

// Client: a script's own play_cycle on a copy of a world physic object. Dropped once the host's word on that object is here
// (the host's cycle holds); before it, and on the host, in single player, on anything but a free physic object: stock.
enum class LocalCycle : uint8_t
{
	Play,
	Drop,
};
inline LocalCycle local_cycle(bool mirror, bool free_physic_object, bool host_word_here)
{
	return mirror && free_physic_object && host_word_here ? LocalCycle::Drop : LocalCycle::Play;
}

// Client: the copy must (re)play the host's word: a word that just came always (the host's call, as made), else when the copy
// is another object than the one it was played on (the object went offline and came back) or no longer shows the cycle.
inline bool must_play(bool fresh_word, bool same_copy, bool cycle_shown)
{
	return fresh_word || !same_copy || !cycle_shown;
}

// The phase a blend of the host's cycle, just started here, is put at: the host has played it `played_s` seconds at `speed`.
// A stop-at-end cycle stops at its end (CBlend::update_time: timeTotal - end_eps), a looped one wraps. Never negative.
inline float phase_after(float played_s, float speed, float total, bool stop_at_end, float end_eps)
{
	if (!(played_s > 0.f) || !(total > 0.f) || !std::isfinite(played_s) || !std::isfinite(speed))
		return 0.f;
	const float t = played_s * std::fabs(speed);
	if (stop_at_end)
	{
		const float end = total - end_eps;
		return end <= 0.f ? 0.f : (t < end ? t : end);
	}
	return std::fmod(t, total);
}
} // namespace motion
} // namespace coop
