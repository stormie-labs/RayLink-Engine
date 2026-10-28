// Bounded NPC attachment descriptors and engine-independent reconciliation.
#pragma once

#include "coop_wire.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace coop { namespace attachments {
constexpr size_t kMaxItems = 8;
constexpr size_t kMaxBone = 63;

struct Item
{
    uint16_t id = 0xffff;
    std::string bone;
    float angles[3] = {};
    float position[3] = {};

    bool valid() const
    {
        if (!id || id == 0xffff || bone.empty() || bone.size() > kMaxBone)
            return false;
        for (const unsigned char c : bone)
            if (c < 0x20 || c == 0x7f)
                return false;
        for (size_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(angles[axis]) || !std::isfinite(position[axis]))
                return false;
        return true;
    }

    bool operator==(const Item& other) const
    {
        return id == other.id && bone == other.bone &&
            std::equal(angles, angles + 3, other.angles) &&
            std::equal(position, position + 3, other.position);
    }
};

struct State
{
    bool complete = true;
    std::vector<Item> items;

    bool valid() const
    {
        if (!complete)
            return items.empty();
        if (items.size() > kMaxItems)
            return false;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (!items[i].valid())
                return false;
            for (size_t j = 0; j < i; ++j)
                if (items[i].id == items[j].id)
                    return false;
        }
        return true;
    }

    bool read(Reader& r)
    {
        State next;
        const uint8_t count = r.u8();
        if (!r.ok())
            return false;
        if (count == 255)
            next.complete = false;
        else
        {
            if (count > kMaxItems)
                return false;
            next.items.reserve(count);
            for (uint8_t i = 0; i < count; ++i)
            {
                Item item;
                item.id = r.u16();
                const auto lengthStart = r.cursor();
                const uint64_t length = r.varu();
                // A canonical varu length <=63 occupies one byte. Bound before allocating.
                if (!r.ok() || !length || length > kMaxBone || r.cursor() - lengthStart != 1)
                    return false;
                char bone[kMaxBone];
                if (!r.bytes(bone, static_cast<size_t>(length)))
                    return false;
                item.bone.assign(bone, static_cast<size_t>(length));
                r.vec3(item.angles);
                r.vec3(item.position);
                if (!r.ok() || !item.valid())
                    return false;
                next.items.push_back(std::move(item));
            }
        }
        if (!next.valid())
            return false;
        *this = std::move(next);
        return true;
    }

    // The caller supplies a valid host state; incomplete states carry no entries.
    void write(Writer& w) const
    {
        w.u8(complete ? static_cast<uint8_t>(items.size()) : uint8_t(255));
        if (!complete)
            return;
        for (const Item& item : items)
        {
            w.u16(item.id);
            w.str(item.bone);
            w.vec3(item.angles);
            w.vec3(item.position);
        }
    }

    bool operator==(const State& other) const
    {
        return complete == other.complete && items == other.items;
    }
};

template <class Detach, class Attach>
bool reconcile(const State& want, const std::vector<uint16_t>& enabled, Detach detach, Attach attach)
{
    if (!want.complete || !want.valid())
        return false;
    for (const uint16_t id : enabled)
        if (std::none_of(want.items.begin(), want.items.end(),
                [id](const Item& item) { return item.id == id; }))
            detach(id);
    bool applied = true;
    for (const Item& item : want.items)
    {
        // Always try later descriptors, even when an earlier entity has not spawned yet.
        const bool attached = attach(item);
        applied = attached && applied;
    }
    return applied;
}
} } // namespace coop::attachments
