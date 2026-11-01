// Anomaly Coop — a client's PDA log. The log is kept under the actor's id and a client's actor is object 0 of the host's save
// (coop_join.h), so every world from the host hands him the host's log: its session notices, its deals, its talks.
// A client's log is what came to him in this session: it is written down as it comes and goes back on the actor of each new
// world. Whoever is not a client keeps the log the save has.
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

namespace coop::news_log
{
// A line reached the log of the actor.
template <class Log, class Entry>
void on_news(bool client, Log& own, const Entry& entry)
{
	if (client)
		own.push_back(entry);
}

// The actor of a new world has read its log from the save.
template <class Log>
void on_actor_spawn(bool client, Log& actor_log, const Log& own)
{
	if (client)
		actor_log = own;
}
} // namespace coop::news_log
