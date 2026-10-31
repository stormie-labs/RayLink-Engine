// A font family that is not described by a config section (a font a script registers at run time): which files make its
// tiers and which tier suits a screen height. Pure C++17, header-only, no engine dependencies (checked outside the engine
// by a small harness; the engine side is ResolveFontFamily in GameFont.cpp).
//
// A family `<base>` is the files <base>_h<N>.ini / .dds, each an atlas authored for a screen N px high, or <base>.ini / .dds
// alone, one atlas for every height.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace font_family
{
// `file_lc` is a file name of the family's directory in lower case ("ui_font_x_h1080.ini"), `name_lc` the lower case name of
// the family ("ui_font_x") and `lang_lc` the language suffix of the string table (font_prefix, lower case, usually empty).
// Returns N (1..99999, no leading zero, digits only) for "<name>_h<N><lang>.ini", else 0.
inline uint32_t parse_tier_file(const std::string& file_lc, const std::string& name_lc, const std::string& lang_lc)
{
	static const char kExt[] = ".ini";
	constexpr size_t kExtLen = sizeof(kExt) - 1;
	if (file_lc.size() <= kExtLen || file_lc.compare(file_lc.size() - kExtLen, kExtLen, kExt) != 0)
		return 0;
	const std::string stem = file_lc.substr(0, file_lc.size() - kExtLen);

	const size_t head = name_lc.size() + 2; // "<name>_h"
	if (stem.size() <= head + lang_lc.size() || stem.compare(0, name_lc.size(), name_lc) != 0 || stem[name_lc.size()] != '_' ||
		stem[name_lc.size() + 1] != 'h')
		return 0;
	if (!lang_lc.empty() && stem.compare(stem.size() - lang_lc.size(), lang_lc.size(), lang_lc) != 0)
		return 0;

	const size_t digits_end = stem.size() - lang_lc.size();
	if (digits_end <= head || digits_end - head > 5 || stem[head] == '0')
		return 0;
	uint32_t n = 0;
	for (size_t i = head; i < digits_end; ++i)
	{
		if (stem[i] < '0' || stem[i] > '9')
			return 0;
		n = n * 10 + uint32_t(stem[i] - '0');
	}
	return n;
}

// How far a tier authored for `authored` px is from a `height` px high target (the screen, or any height the caller wants the
// text sized for; may be fractional): the same on the log scale as the stock rungs (a tier twice as high and a tier half as
// high are equally far).
inline float distance(uint32_t authored, float height)
{
	if (!authored || !(height > 0.f))
		return 1e9f;
	return std::fabs(std::log(float(authored) / height));
}

// The tiers best first for a `height` px high target: the nearest on the log scale; a tie goes to the larger authored
// height; the same height twice counts once.
inline std::vector<uint32_t> by_preference(std::vector<uint32_t> authored, float height)
{
	std::sort(authored.begin(), authored.end());
	authored.erase(std::unique(authored.begin(), authored.end()), authored.end());
	std::stable_sort(authored.begin(), authored.end(), [height](uint32_t a, uint32_t b)
	{
		const float da = distance(a, height);
		const float db = distance(b, height);
		if (std::fabs(da - db) > 1e-4f)
			return da < db;
		return a > b;
	});
	return authored;
}
} // namespace font_family
