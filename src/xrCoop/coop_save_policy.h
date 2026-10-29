#pragma once

#include <string_view>

namespace coop::save_policy
{
constexpr const char* kClientStem = "__raylink_client_world";
constexpr const char* kCacheFile = "raylink\\cache\\client_world";
constexpr const char* kLegacyStem = "coop_session";

inline bool is_client_cache(std::string_view stem)
{
    const std::string_view reserved(kClientStem);
    if (stem.size() != reserved.size())
        return false;
    for (size_t i = 0; i < stem.size(); ++i)
    {
        const char c = stem[i];
        const char lower = c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
        if (lower != reserved[i])
            return false;
    }
    return true;
}

inline bool may_save_world(bool client, bool mirror)
{
    return !client && !mirror;
}
} // namespace coop::save_policy
