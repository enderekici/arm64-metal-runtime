#pragma once

// dyld's objc "init" notification (load_images, which runs +load methods)
// is delivered per image just before that image's initializers, in
// initializer order. This tracks which mapped objc images still need it.

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace arm64metal {

struct ObjCLoadImagesState {
    std::uint64_t callback = 0;                        // objc load_images
    std::map<std::uint64_t, std::uint64_t> records;    // header -> mapped-info record
    std::set<std::uint64_t> notified;

    // Records still owed a notification for every image up to and including
    // `through` in `order`; `all` also drains images outside `order`.
    std::vector<std::uint64_t> take_through(const std::vector<std::uint64_t>& order,
                                            std::uint64_t through, bool all) {
        std::vector<std::uint64_t> pending;
        if (callback == 0) return pending;
        const auto add = [&](std::uint64_t header) {
            const auto record = records.find(header);
            if (record == records.end() || !notified.insert(header).second) return;
            pending.push_back(record->second);
        };
        for (const auto header : order) {
            add(header);
            if (!all && header == through) return pending;
        }
        if (all) {
            for (const auto& entry : records) add(entry.first);
        }
        return pending;
    }
};

}  // namespace arm64metal
