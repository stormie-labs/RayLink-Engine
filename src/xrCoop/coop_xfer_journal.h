// Anomaly Coop - F3a split levels: the text of the transfer journal, section [coop_xfer] of the host actor's custom
// data (plan 2026-10-24-f3a §1.7 "journal", §4.1 requirement 2; coop_xfer.h R6, R8).
//   [coop_xfer]
//   v = 1
//   gen = <the ledger's gen counter>
//   n = <entries>
//   x<i> = <G|A|R>,<id>,<map server>,<gen>,<gen it held before the release>,<state in base64, R only>
// The reader is strict: anything malformed refuses the whole text (the objects then load at their last confirmed
// state).
// Pure C++17, header-only, no engine dependencies: used by the engine and the unit-test exe.
#pragma once

#include "coop_xfer_base.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace coop
{
namespace xfer
{
namespace detail
{
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline size_t base64_size(size_t n) { return (n + 2) / 3 * 4; }

inline std::string base64(const std::vector<uint8_t>& in)
{
	std::string out;
	out.reserve(base64_size(in.size()));
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3)
	{
		const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];
		out += kB64[v & 63];
	}
	if (i + 1 == in.size())
	{
		const uint32_t v = uint32_t(in[i]) << 16;
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += "==";
	}
	else if (i + 2 == in.size())
	{
		const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

inline int base64_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

// Strict: whole quads, the alphabet only, '=' only as the padding of the last quad.
inline bool unbase64(const std::string& in, std::vector<uint8_t>& out)
{
	out.clear();
	if (in.size() % 4)
		return false;
	out.reserve(in.size() / 4 * 3);
	for (size_t i = 0; i < in.size(); i += 4)
	{
		const bool last = i + 4 == in.size();
		int v[4];
		int pad = 0;
		for (int k = 0; k < 4; ++k)
		{
			const char c = in[i + size_t(k)];
			if (c == '=' && last && k >= 2)
			{
				v[k] = 0;
				++pad;
				continue;
			}
			if (pad)
				return false; // data after a '='
			v[k] = base64_value(c);
			if (v[k] < 0)
				return false;
		}
		const uint32_t q = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
		out.push_back(uint8_t(q >> 16));
		if (pad < 2)
			out.push_back(uint8_t(q >> 8));
		if (pad < 1)
			out.push_back(uint8_t(q));
	}
	return true;
}

// Strict unsigned decimal within [0, max].
inline bool parse_u32(const std::string& s, uint32_t max, uint32_t& out)
{
	if (s.empty() || s.size() > 10)
		return false;
	uint64_t v = 0;
	for (char c : s)
	{
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + uint64_t(c - '0');
	}
	if (v > max)
		return false;
	out = uint32_t(v);
	return true;
}

inline std::string trim(const std::string& s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
		--e;
	return s.substr(b, e - b);
}

// Upper bounds of the journal text: the header and one entry line besides its base64.
constexpr size_t kHeaderBytes = 64;
constexpr size_t kEntryBytes = 64;
} // namespace detail

// One transfer in flight as the journal keeps it.
struct JournalEntry
{
	char kind = 'G'; // G Granting, A Aborting, R Releasing
	uint16_t id = 0;
	Peer peer = kWorld;
	uint32_t gen = 0;
	uint32_t from_gen = 0; // R: the gen the map server held
	std::vector<uint8_t> payload; // R: the state to apply
};

// The text an entry with this payload takes at most (a reservation against kJournalMaxBytes).
inline size_t journal_entry_bytes(const std::vector<uint8_t>& payload)
{
	return detail::kEntryBytes + detail::base64_size(payload.size());
}

inline std::string write_journal(uint32_t gen, std::vector<JournalEntry> list)
{
	std::sort(list.begin(), list.end(), [](const JournalEntry& a, const JournalEntry& b) { return a.id < b.id; });
	std::string s = "[coop_xfer]\nv = 1\ngen = " + std::to_string(gen) + "\nn = " + std::to_string(list.size());
	s += "\n";
	for (size_t i = 0; i < list.size(); ++i)
	{
		const JournalEntry& j = list[i];
		s += "x" + std::to_string(i) + " = " + j.kind + "," + std::to_string(j.id) + "," + std::to_string(j.peer);
		s += "," + std::to_string(j.gen) + "," + std::to_string(j.from_gen) + ",";
		s += detail::base64(j.payload) + "\n";
	}
	return s;
}

// Reads a text written by write_journal (other sections may surround [coop_xfer]). False, and `gen_out`/`out`
// untouched, for anything malformed.
inline bool read_journal(const std::string& text, uint32_t& gen_out, std::vector<JournalEntry>& out)
{
	if (text.size() > kJournalMaxBytes)
		return false;
	std::vector<std::string> lines;
	bool in = false, found = false;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
			nl = text.size();
		const std::string line = detail::trim(text.substr(pos, nl - pos));
		pos = nl + 1;
		if (line.empty())
			continue;
		if (line[0] == '[')
		{
			if (found)
				break; // the next section
			in = found = line == "[coop_xfer]";
			continue;
		}
		if (in)
			lines.push_back(line);
	}
	if (!found)
		return false;
	std::map<std::string, std::string> kv;
	for (const std::string& line : lines)
	{
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			return false;
		const std::string key = detail::trim(line.substr(0, eq));
		if (key.empty() || !kv.emplace(key, detail::trim(line.substr(eq + 1))).second)
			return false;
	}
	uint32_t version = 0, gen = 0, n = 0;
	if (!kv.count("v") || !detail::parse_u32(kv["v"], 1, version) || version != 1)
		return false;
	if (!kv.count("gen") || !detail::parse_u32(kv["gen"], 0xffffffffu, gen))
		return false;
	if (!kv.count("n") || !detail::parse_u32(kv["n"], uint32_t(kJournalMaxEntries), n))
		return false;
	if (kv.size() != 3 + size_t(n))
		return false;
	std::vector<JournalEntry> loaded;
	std::set<uint16_t> seen;
	for (uint32_t i = 0; i < n; ++i)
	{
		auto it = kv.find("x" + std::to_string(i));
		if (it == kv.end())
			return false;
		std::vector<std::string> f;
		for (size_t b = 0;;)
		{
			const size_t c = it->second.find(',', b);
			f.push_back(it->second.substr(b, c == std::string::npos ? std::string::npos : c - b));
			if (c == std::string::npos)
				break;
			b = c + 1;
		}
		if (f.size() != 6 || f[0].size() != 1 || (f[0][0] != 'G' && f[0][0] != 'A' && f[0][0] != 'R'))
			return false;
		JournalEntry j;
		j.kind = f[0][0];
		uint32_t id = 0;
		if (!detail::parse_u32(f[1], 0xffff, id) || !detail::parse_u32(f[2], 0xffffffffu, j.peer) ||
		    j.peer == kWorld || !detail::parse_u32(f[3], 0xffffffffu, j.gen) || j.gen == 0 || j.gen > gen ||
		    !detail::parse_u32(f[4], 0xffffffffu, j.from_gen) || !detail::unbase64(f[5], j.payload))
			return false;
		if (j.kind != 'R' && (!j.payload.empty() || j.from_gen != 0))
			return false;
		j.id = uint16_t(id);
		if (!seen.insert(j.id).second)
			return false;
		loaded.push_back(std::move(j));
	}
	out = std::move(loaded);
	gen_out = gen;
	return true;
}
} // namespace xfer
} // namespace coop
