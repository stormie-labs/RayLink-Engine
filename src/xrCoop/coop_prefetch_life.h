// Anomaly Coop — the life of a level's spawn prefetch thread (SPAWN_ANTIFREEZE, stock alpet/demonized code in
// xrGame/Level.cpp, present in single player too). The stock thread outlived its CLevel: ~CLevel freed the containers and
// only then told the thread to stop, without waiting, so a batch in flight went on writing into freed containers (a
// prefetch_event holds a NET_Packet with a 16 KB buffer: 0xC0000374, heap corruption, at level change/load); the stop
// flag and the lock were members of the freed CLevel; one static event served every level (the next CLevel overwrote it,
// the old thread waited on and closed the new one); a stop set just before the thread's own ResetEvent was lost.
// The protocol here:
// - everything the thread touches belongs to one Life of one level (its lock, its signal, its exit event, the stop flag)
//   plus that level's containers; nothing is static;
// - `closing` is written under the exclusive lock and read under the lock, and the thread resets the signal only while
//   `closing` is false (under the same lock): a stop is never swallowed;
// - the thread checks `closing` before every model and before every write, and once it is set writes nothing more;
// - the owner (~CLevel) sets `closing`, signals, and waits for `exited`, which the thread sets as its very last action;
//   only then it frees the containers and the Life. On timeout it frees nothing: everything a live thread may touch leaks.
// Pure C++17, header-only, no engine dependencies: Lock and Event are injected (xrSRWLock and a Win32 event in xrGame,
// std primitives in the unit-test exe).
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{
namespace prefetch
{
constexpr uint32_t kForever = 0xFFFFFFFFu; // Event::wait without a timeout (== INFINITE)
constexpr uint32_t kStopWaitMs = 30000;    // the longest ~CLevel waits for the thread; after that it leaks

// Lock:  lock() / unlock() (exclusive), lock_shared() / unlock_shared() (the names of std::shared_mutex).
// Event: manual reset; set(), reset(), bool wait(uint32_t ms) (true: signalled; kForever: no timeout).
template <class Lock>
class Exclusive
{
public:
	explicit Exclusive(Lock& l) : l_(l) { l_.lock(); }
	~Exclusive() { l_.unlock(); }
	Exclusive(const Exclusive&) = delete;
	Exclusive& operator=(const Exclusive&) = delete;

private:
	Lock& l_;
};

template <class Lock>
class Shared
{
public:
	explicit Shared(Lock& l) : l_(l) { l_.lock_shared(); }
	~Shared() { l_.unlock_shared(); }
	Shared(const Shared&) = delete;
	Shared& operator=(const Shared&) = delete;

private:
	Lock& l_;
};

// One level's thread. Lives on the heap apart from the level: on timeout the owner leaks it with the containers.
template <class Lock, class Event>
struct Life
{
	Lock lock;            // the level's lock over its spawn containers (CLevel::prefetch_lock points here)
	Event signal;         // work queued, or stop
	Event exited;         // set by the thread as its very last action
	bool closing = false; // the level is going away: written under the exclusive lock, read under the lock

	Life() = default;
	Life(const Life&) = delete;
	Life& operator=(const Life&) = delete;
};

// Producer (ProcessGameEvents), under the exclusive lock, after it queued a batch: wakes the thread.
template <class Lock, class Event>
void wake_locked(Life<Lock, Event>& life)
{
	life.signal.set();
}

// Thread, under the lock: the queue is drained. The signal is reset only while the level lives: a stop set before this
// reset would be lost and the thread would wait for ever.
template <class Lock, class Event>
void pause_locked(Life<Lock, Event>& life)
{
	if (!life.closing)
		life.signal.reset();
}

// The thread's loop. Work is the level's containers seen from the thread:
//   using Batch = ...                  a range of events, each with a range `models`
//   bool empty_locked()                the queue of events to prefetch is empty      (shared lock)
//   void take_locked(Batch&)           moves the queue into the batch                (exclusive lock)
//   bool known_locked(const Model&)    the model was prefetched already              (shared lock)
//   void prefetch(const Model&)        loads the model                               (no lock: the long part)
//   void remember_locked(const Model&) the model is prefetched                       (exclusive lock)
//   void deliver_locked(Batch&)        the batch goes to the spawn queue             (exclusive lock)
//   void leaving(size_t dropped)       the thread leaves; `dropped` events of a batch in flight were not delivered
// Runs until the owner stops it. Once `closing` is seen it touches no container; `exited` is its very last touch of
// anything of the level.
template <class Lock, class Event, class Work>
void run(Life<Lock, Event>& life, Work& work)
{
	size_t dropped = 0;
	for (;;)
	{
		life.signal.wait(kForever);
		{
			Shared<Lock> g(life.lock);
			if (life.closing)
				break;
			if (work.empty_locked())
			{
				pause_locked(life);
				continue;
			}
		}

		typename Work::Batch batch;
		{
			Exclusive<Lock> g(life.lock);
			if (life.closing)
				break;
			work.take_locked(batch);
			pause_locked(life);
		}

		bool stop = false;
		for (const auto& e : batch)
		{
			for (const auto& model : e.models)
			{
				bool known = false;
				{
					Shared<Lock> g(life.lock);
					stop = life.closing;
					if (!stop)
						known = work.known_locked(model);
				}
				if (stop)
					break;
				if (known)
					continue;
				work.prefetch(model);
				{
					Exclusive<Lock> g(life.lock);
					stop = life.closing;
					if (!stop)
						work.remember_locked(model);
				}
				if (stop)
					break;
			}
			if (stop)
				break;
		}

		if (!stop)
		{
			Exclusive<Lock> g(life.lock);
			stop = life.closing;
			if (!stop)
				work.deliver_locked(batch);
		}
		if (stop)
		{
			dropped = batch.size();
			break;
		}
	}
	work.leaving(dropped);
	life.exited.set(); // the very last touch: from here on the owner may free the Life and the containers
}

// Owner (~CLevel): tells the thread to stop and waits for it. True: the thread has left, everything it used may be freed.
// False: it did not leave in timeout_ms and may still touch the Life and the containers.
template <class Lock, class Event>
bool stop(Life<Lock, Event>& life, uint32_t timeout_ms)
{
	{
		Exclusive<Lock> g(life.lock);
		life.closing = true;
		life.signal.set(); // under the lock: the thread's pause_locked cannot come between
	}
	return life.exited.wait(timeout_ms);
}

// Owner: stop(), then free_all() (the containers and the Life itself) only once the thread has left. On timeout nothing
// is freed: the caller forgets the pointers and leaks them. Returns whether the thread left.
template <class Lock, class Event, class FreeAll>
bool shutdown(Life<Lock, Event>& life, uint32_t timeout_ms, FreeAll&& free_all)
{
	if (!stop(life, timeout_ms))
		return false;
	free_all();
	return true;
}
} // namespace prefetch
} // namespace coop
