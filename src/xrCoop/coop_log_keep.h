// Anomaly Coop: the game log of the last runs is kept (pure rules; the file work is xrCore log.cpp coop_keep_previous_log).
// Live 2026-11-07: reports ("2 of my 3 artefacts vanished", "handler error") came after the next launch had overwritten
// logs\xray_<nick>.log. Before the new log truncates it, the old one moves to
//   logs\old\<its name without .log>_<yyyy-mm-dd_hh-mm-ss of its last write, local time>.log
// (a second launch within the same second gets _2 .. _9), and logs\old keeps the newest kKeep archives of that log. Nothing
// else in logs\old is ever touched. Tests: tests/test_log_keep.cpp.
#pragma once

// Only headers the precompiled header of xrCore has already: a new one after its #pragma deprecated (sprintf_s & co) warns.
#include <algorithm>
#include <string>
#include <vector>

namespace coop
{
namespace log_keep
{
constexpr size_t kKeep = 10; // archives of one log in logs\old
constexpr const char* kFolder = "old"; // under the folder of the log
constexpr unsigned kMaxSameSecond = 9; // archive_name n = 1..9

struct Stamp
{
	int year, month, day, hour, minute, second;
};

// The folder of `path` with its trailing separator ("" when there is none).
inline std::string folder_of(const std::string& path)
{
	const size_t cut = path.find_last_of("\\/");
	return cut == std::string::npos ? std::string() : path.substr(0, cut + 1);
}

inline char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

inline bool same_text(const std::string& a, size_t at, const std::string& b) // a[at..] starts with b, any letter case
{
	if (a.size() < at + b.size())
		return false;
	for (size_t i = 0; i < b.size(); ++i)
		if (lower(a[at + i]) != lower(b[i]))
			return false;
	return true;
}

// "C:\\...\\logs\\xray_bob.log" -> "xray_bob"; "" when the name does not end in .log or is nothing but it.
inline std::string stem_of(const std::string& path)
{
	const std::string name = path.substr(folder_of(path).size());
	static const std::string ext = ".log";
	if (name.size() <= ext.size() || !same_text(name, name.size() - ext.size(), ext))
		return std::string();
	return name.substr(0, name.size() - ext.size());
}

// `v` with at least `width` digits, zeros in front ("7", 2 -> "07"); a negative number reads as 0.
inline std::string padded(int v, size_t width)
{
	std::string s;
	unsigned u = v > 0 ? unsigned(v) : 0u;
	do
	{
		s.insert(s.begin(), char('0' + u % 10));
		u /= 10;
	} while (u != 0);
	while (s.size() < width)
		s.insert(s.begin(), '0');
	return s;
}

// "2026-11-07_21-05-09"
inline std::string stamp_text(const Stamp& t)
{
	return padded(t.year, 4) + "-" + padded(t.month, 2) + "-" + padded(t.day, 2) + "_" + padded(t.hour, 2) + "-" +
	    padded(t.minute, 2) + "-" + padded(t.second, 2);
}

// "<stem>_<stamp>.log"; n > 1: "<stem>_<stamp>_<n>.log" (the n-th launch within that second).
inline std::string archive_name(const std::string& stem, const Stamp& t, unsigned n = 1)
{
	std::string s = stem + "_" + stamp_text(t);
	if (n > 1)
		s += "_" + padded(int(n), 1);
	return s + ".log";
}

// The first archive name that `exists` does not know (n = 1..kMaxSameSecond); "" when all are taken (the log is not moved).
template <class Exists>
std::string free_name(const std::string& stem, const Stamp& t, Exists exists)
{
	for (unsigned n = 1; n <= kMaxSameSecond; ++n)
	{
		std::string name = archive_name(stem, t, n);
		if (!exists(name))
			return name;
	}
	return std::string();
}

// An empty log tells nothing: it is overwritten as before.
inline bool worth_keeping(unsigned long long size)
{
	return size > 0;
}

inline bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

// Is `name` an archive_name of `stem` (any letter case)? Only those are ever pruned.
inline bool is_archive_of(const std::string& name, const std::string& stem)
{
	if (stem.empty() || !same_text(name, 0, stem + "_"))
		return false;
	static const char kPattern[] = "dddd-dd-dd_dd-dd-dd"; // stamp_text
	const size_t at = stem.size() + 1;
	const size_t len = sizeof(kPattern) - 1;
	if (name.size() < at + len)
		return false;
	for (size_t i = 0; i < len; ++i)
	{
		const char c = name[at + i];
		if (kPattern[i] == 'd' ? !is_digit(c) : c != kPattern[i])
			return false;
	}
	std::string rest = name.substr(at + len);
	if (rest.size() == 6 && rest[0] == '_' && rest[1] >= '2' && rest[1] <= char('0' + kMaxSameSecond))
		rest = rest.substr(2);
	return rest.size() == 4 && same_text(rest, 0, ".log");
}

// The archives of `stem` among `names` that go: all but the newest `keep` (the names sort by their time).
inline std::vector<std::string> to_prune(const std::vector<std::string>& names, const std::string& stem, size_t keep = kKeep)
{
	std::vector<std::string> ours;
	for (const std::string& n : names)
		if (is_archive_of(n, stem))
			ours.push_back(n);
	if (ours.size() <= keep)
		return {};
	const size_t skip = stem.size() + 1; // the stem's letter case does not order them
	std::sort(ours.begin(), ours.end(),
	    [skip](const std::string& a, const std::string& b) { return a.compare(skip, std::string::npos, b, skip, std::string::npos) < 0; });
	ours.resize(ours.size() - keep);
	return ours;
}
} // namespace log_keep
} // namespace coop
