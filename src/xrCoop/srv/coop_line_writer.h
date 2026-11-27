// The writer between the game's threads and a reader that may stop reading: the dedicated host's pipe to its parent and its console
// window (coop_host_console.cpp). A write to such a reader can wait without end: a parent that does not read fills the pipe, and the
// Windows console holds WriteConsole while its window's output is paused (a mouse selection, Pause or Ctrl+S, a drag of its scroll bar).
// The game's threads therefore never write: they push into a bounded OutputQueue (coop_world_server_rules.h), one thread of this writer
// pops and hands each line to the sink it was started with. A held sink costs the game nothing but the oldest lines, and the queue says
// how many went.
//
// Lifetime: the thread is detached and shares the state with the writer object, so neither outlives the other's memory; a writer that
// is destroyed turns itself off and its thread ends after the line it is writing. No joinable thread exists at any time.
// Off is for good, and a writer that takes no more lines takes no lock: at process exit ExitProcess may kill its thread inside the
// lock, and a line the logger still hands over after close() or stop() must return instead of waiting on that lock for ever.
// Pure C++17, header-only, no engine dependencies: used by xrEngine (coop_host_console.cpp) and by the unit-test exe.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "coop_world_server_rules.h"

namespace coop
{
namespace world_server
{
// `Lock` is the state's mutex: std::mutex, or a test's lock that counts and holds.
template <class Lock>
class BasicLineWriter
{
public:
	// Writes one line. False: the reader is gone for good, the writer turns itself off.
	using Sink = std::function<bool(const std::string&)>;
	// Starts `entry(arg)` on a new detached thread; false when the system refused it (then `entry` never runs). The shape of the CRT's
	// _beginthread, so the engine passes that and a refused thread is an answer, not the end of the process.
	using Starter = bool (*)(void (*entry)(void*), void* arg);

	explicit BasicLineWriter(OutputQueue queue = OutputQueue()) : m_state(std::make_shared<State>(std::move(queue))) {}

	~BasicLineWriter() { stop(); }

	BasicLineWriter(const BasicLineWriter&) = delete;
	BasicLineWriter& operator=(const BasicLineWriter&) = delete;

	// Starts the thread once; from here lines are taken. False: the thread was refused (the writer is off for good), or it was stopped
	// before. A second start changes nothing and answers on().
	bool start(Sink sink, Starter starter)
	{
		State& s = *m_state;
		{
			std::lock_guard<Lock> hold(s.lock);
			if (s.started)
				return s.on;
			s.started = true;
			s.on = true;
			s.accepting = true;
			s.sink = std::move(sink);
		}
		auto* arg = new std::shared_ptr<State>(m_state); // the thread's own share of the state, freed by the thread
		if (starter(entry, arg))
			return true;
		delete arg;
		std::lock_guard<Lock> hold(s.lock);
		s.turn_off();
		return false;
	}

	// Off for good: the queued lines go, later pushes are ignored, and from here no call of this writer takes its lock. A line being
	// written is finished by the thread, which then ends. Bounded: takes the lock once, never waits for the sink.
	void stop()
	{
		State& s = *m_state;
		if (!s.on && s.started)
			return; // already off for good
		std::lock_guard<Lock> hold(s.lock);
		s.started = true; // a writer stopped before its start never starts
		s.turn_off();
	}

	// No more lines are taken; the lines already queued are still written, in order. Takes no lock, and from here push takes none
	// either. For the end of the process: the last lines pushed before it (a parent waits for them) are not thrown away the way
	// stop() throws them.
	void close() { m_state->accepting = false; }

	// Any thread. Holds the caller for the queue's mutex only, never calls the sink; ignored, without the lock, once the writer takes
	// no more lines.
	void push(std::string line, bool rl)
	{
		State& s = *m_state;
		if (!s.accepting)
			return;
		{
			std::lock_guard<Lock> hold(s.lock);
			if (!s.accepting)
				return;
			s.queue.push(std::move(line), rl);
		}
		s.wake.notify_all();
	}

	// Waits up to `ms` until nothing is queued and no line is being written. False on the timeout, or when the writer is off.
	bool flush(unsigned ms)
	{
		State& s = *m_state;
		if (!s.on)
			return false;
		std::unique_lock<Lock> hold(s.lock);
		return s.wake.wait_for(hold, std::chrono::milliseconds(ms), [&s] { return !s.on || (s.queue.empty() && !s.writing); }) && s.on;
	}

	// Lines are still taken: started, not closed, not stopped, and the sink has not said its reader is gone.
	bool on() const { return m_state->accepting; }

private:
	struct State
	{
		explicit State(OutputQueue q) : queue(std::move(q)) {}

		// Under `lock`.
		void turn_off()
		{
			on = false;
			accepting = false;
			std::string line;
			while (queue.pop(line))
			{
			}
			wake.notify_all();
		}

		Lock lock;
		std::condition_variable_any wake; // the writer waits for lines, flush waits for the end of them
		OutputQueue queue;
		Sink sink;
		// Written under `lock` (close() clears `accepting` without it) and read before it: a writer that is off, or takes no more
		// lines, takes no lock.
		std::atomic<bool> started{false};
		std::atomic<bool> on{false}; // the thread runs and writes what is queued
		std::atomic<bool> accepting{false}; // push takes lines
		bool writing = false; // under `lock`: the writer holds a line it has not written yet
	};

	static void entry(void* arg)
	{
		std::unique_ptr<std::shared_ptr<State>> own(static_cast<std::shared_ptr<State>*>(arg));
		run(*own);
	}

	static void run(const std::shared_ptr<State>& state)
	{
		State& s = *state;
		std::unique_lock<Lock> hold(s.lock);
		for (;;)
		{
			s.wake.wait(hold, [&s] { return !s.on || !s.queue.empty(); });
			if (!s.on)
				return;
			std::string line;
			s.queue.pop(line);
			s.writing = true;
			hold.unlock();
			const bool ok = s.sink(line); // out of the lock: the game's pushes go on while the reader holds this call
			hold.lock();
			s.writing = false;
			if (!ok)
				s.turn_off();
			else
				s.wake.notify_all(); // flush waits on the same condition
		}
	}

	std::shared_ptr<State> m_state;
};

using LineWriter = BasicLineWriter<std::mutex>;
} // namespace world_server
} // namespace coop
