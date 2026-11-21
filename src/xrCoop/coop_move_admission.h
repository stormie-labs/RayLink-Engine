// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — admission of an item move into a container on the process that decides (F4 G6-1, finding m17:4).
//
// Hideout Furniture limits the weight of a stash only in the window of the player who moves an item
// (stash_capacities.script:89-116, ActorMenu_on_item_before_move: a view callback, xrCoop/coop_routes.h): two players
// filling one stash at once each saw the old weight, and the decider applied both (50 kg of 60, two moves of 6 kg: 62 kg).
// The decider now asks the mods' move rules (gamedata/scripts/coop_mods_move_limits.script admit, Lua) before apply_move of
// a move into a container: a client's ReqTake / ReqTransfer (coop_requests.cpp host_move) and this process's own script and
// window moves (host_transfer_item: G.A.M.M.A.'s ui_inventory moves items with transfer_item). The windows stay a preview.
//
// A move it let in reaches the container's game inventory - what the rules weigh, iterate_inventory_box - only when the game
// event queue runs, a frame later. Until then it is held in the Ledger, and the rules get the count and the weight of the
// held moves of that container. A held move is released when the container's game inventory shows it (Track::Landed),
// when the item went elsewhere or there is nothing here that weighs it (Track::Gone), when it was refused after it was held
// (apply_move did not do it: release), or kTtlMs after it was held.
//
// Verdicts:
//   Ok       a rule let it in: held until it lands
//   Full     a rule refused it, or the ledger still holds kMaxHeld moves after settling -> Reason::NotAllowed
//   Unknown  no rule spoke for this container, or asking failed: the move goes on as in the stock game and is not held
//
// The same in both modes: whichever process applies the move - the co-op host, or in the Server mode the map server of the
// player's level - keeps its own ledger for the containers of the map it leads; a player's machine never asks.
//
// Pure C++17, header-only, no engine dependencies: used by the engine (xrGame/coop/coop_requests.cpp) and the unit-test exe
// (tests/test_move_admission.cpp).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop
{
namespace admission
{
enum class Verdict : uint8_t
{
	Ok,
	Full,
	Unknown
};

// The move rules' answer as the engine read it: coop_mods_move_limits.admit returned true / false / nil, or the call failed
// (a Lua error, no Lua state, an answer that is not a boolean or nil).
enum class Answer : uint8_t
{
	Admit,
	Refuse,
	NoRule,
	Failed
};

// Where a held move stands now; the engine asks the game (coop_requests.cpp admission_track).
enum class Track : uint8_t
{
	Waiting, // the item's server object is in the container, its game inventory does not show it yet
	Landed,  // the container's game inventory shows it: the rules weigh it themselves now
	Gone     // the item is elsewhere, gone, or nothing here weighs the container
};

// The held moves of one container.
struct Pending
{
	uint32_t count = 0;
	float weight = 0.f;
};

constexpr uint16_t kNoObject = 0xffff;
constexpr uint32_t kMaxHeld = 8192; // every held move of this process, all containers (an item is held at most once)
constexpr uint64_t kTtlMs = 5000; // a held move lands a frame later; one that never shows is dropped after this
constexpr float kMaxWeight = 100000.f;

// A weight as the ledger adds it: the game's CInventoryItem::Weight (never a client's number), still bounded: not finite or
// below zero -> 0, above kMaxWeight -> kMaxWeight.
inline float held_weight(float w)
{
	if (!std::isfinite(w) || w <= 0.f)
		return 0.f;
	return w < kMaxWeight ? w : kMaxWeight;
}

inline Verdict verdict_of(Answer a)
{
	switch (a)
	{
	case Answer::Admit: return Verdict::Ok;
	case Answer::Refuse: return Verdict::Full;
	default: return Verdict::Unknown;
	}
}

class Ledger
{
public:
	// Drops settled or expired moves and returns how many were dropped. Usually only `container` is checked; when the
	// ledger is full, check every container so stale entries cannot block a move into a different container.
	template <class TrackFn>
	uint32_t settle(uint16_t container, uint64_t now, TrackFn&& track)
	{
		const bool full = held_.size() >= kMaxHeld;
		uint32_t dropped = 0;
		for (size_t i = 0; i < held_.size();)
		{
			const Held& h = held_[i];
			if ((full || h.container == container) && (expired(h, now) || track(h.item, h.container) != Track::Waiting))
			{
				held_[i] = held_.back();
				held_.pop_back();
				++dropped;
				continue;
			}
			++i;
		}
		return dropped;
	}

	Pending pending(uint16_t container) const
	{
		Pending p;
		for (const Held& h : held_)
			if (h.container == container)
			{
				++p.count;
				p.weight += h.weight;
			}
		return p;
	}

	// Holds a move that was let in. An earlier move of the same item is replaced (one item goes one way). False when the
	// ledger holds kMaxHeld moves even after the expired ones are dropped, or for no container / no item.
	bool hold(uint16_t container, uint16_t item, float weight, uint64_t now)
	{
		if (container == kNoObject || item == kNoObject)
			return false;
		release(item);
		if (held_.size() >= kMaxHeld)
			drop_expired(now);
		if (held_.size() >= kMaxHeld)
			return false;
		held_.push_back({item, container, held_weight(weight), now});
		return true;
	}

	// The move was refused after it was held (apply_move did not do it). True when it was held.
	bool release(uint16_t item)
	{
		for (size_t i = 0; i < held_.size(); ++i)
			if (held_[i].item == item)
			{
				held_[i] = held_.back();
				held_.pop_back();
				return true;
			}
		return false;
	}

	bool holds(uint16_t item) const
	{
		for (const Held& h : held_)
			if (h.item == item)
				return true;
		return false;
	}

	size_t size() const { return held_.size(); }
	void clear() { held_.clear(); }

private:
	struct Held
	{
		uint16_t item;
		uint16_t container;
		float weight;
		uint64_t at;
	};

	// Older than kTtlMs, or held "later" than now: the clock went back (a new session's clock).
	static bool expired(const Held& h, uint64_t now) { return now < h.at || now - h.at > kTtlMs; }

	void drop_expired(uint64_t now)
	{
		for (size_t i = 0; i < held_.size();)
		{
			if (expired(held_[i], now))
			{
				held_[i] = held_.back();
				held_.pop_back();
				continue;
			}
			++i;
		}
	}

	std::vector<Held> held_;
};

// One admission, after Ledger::settle of the container and the rules asked with its pending(): a move the rules let in is
// held (Ok), or Full when the ledger cannot hold it; a refusal is Full; no rule or a failed asking is Unknown. No container
// or no item: Unknown (nothing to hold, nothing judged).
inline Verdict decide(Ledger& ledger, Answer answer, uint16_t container, uint16_t item, float weight, uint64_t now)
{
	if (container == kNoObject || item == kNoObject)
		return Verdict::Unknown;
	const Verdict v = verdict_of(answer);
	if (v != Verdict::Ok)
		return v;
	return ledger.hold(container, item, weight, now) ? Verdict::Ok : Verdict::Full;
}
} // namespace admission
} // namespace coop
