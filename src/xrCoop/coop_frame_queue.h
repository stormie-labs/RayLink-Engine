// Anomaly Coop — a queue drained by a budget per frame.
// What a frame's budget leaves waits at the head of the queue for the next frame, ahead of what came meanwhile: an event is
// lost only when the queue is full, the oldest first, and every such loss is counted. A lost "stop" of a sound leaves it
// playing, so nothing goes silently.
// Two vectors live as long as the queue (what waits, the frame's share) and swap each frame: once grown, a frame allocates
// nothing.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace coop
{
template <class T>
class FrameQueue
{
public:
	FrameQueue(size_t cap, size_t per_frame) : cap_(cap), per_frame_(per_frame) {}

	void push(T&& e)
	{
		if (waiting_.size() >= cap_)
		{
			waiting_.erase(waiting_.begin());
			++dropped_;
		}
		waiting_.push_back(std::move(e));
	}

	// Fires at most per_frame events, oldest first. What `fire` pushes waits for the next frame, behind the unfired rest.
	template <class Fire>
	void run(Fire&& fire)
	{
		taken_.swap(waiting_); // taken_ is empty between frames: waiting_ starts the frame empty, its capacity kept
		const size_t n = std::min(taken_.size(), per_frame_);
		for (size_t i = 0; i < n; ++i)
			fire(taken_[i]); // a push from here goes to waiting_, never into the vector being read
		if (n < taken_.size())
			waiting_.insert(waiting_.begin(), std::make_move_iterator(taken_.begin() + std::ptrdiff_t(n)),
			    std::make_move_iterator(taken_.end()));
		taken_.clear();
		if (waiting_.size() > cap_)
		{
			const size_t over = waiting_.size() - cap_;
			waiting_.erase(waiting_.begin(), waiting_.begin() + std::ptrdiff_t(over));
			dropped_ += over;
		}
	}

	// Empties the queue; the count of losses stays (a status line counts over the whole run).
	void clear() { waiting_.clear(); }
	bool empty() const { return waiting_.empty(); }
	size_t size() const { return waiting_.size(); }
	uint64_t dropped() const { return dropped_; }

private:
	size_t cap_;
	size_t per_frame_;
	std::vector<T> waiting_;
	std::vector<T> taken_;
	uint64_t dropped_ = 0;
};
}
