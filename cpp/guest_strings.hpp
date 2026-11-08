#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arm64metal {

// C++ equivalent of the Swift instruction fragment consumed by the Metal
// process translator. `next` contains guest PCs reachable after this fragment.
struct GuestInstruction {
    std::vector<std::string> code;
    std::vector<std::uint64_t> next;
};

// Maps a guest address/count pair to a shared-cache backing offset expression.
// The final argument is true for a guest write and false for a read.
using GuestOffsetMapper =
    std::function<std::string(std::string_view address, std::string_view count, bool write)>;

// Lower a supported system string import to Metal code. Returns nullopt for
// unrelated imports. The generated code performs all copying in the guest
// Metal kernel and signals guest memory faults through x[33]/x[34].
std::optional<GuestInstruction> guest_string_call(
    std::string_view name,
    std::optional<std::uint64_t> return_address,
    bool cached,
    const GuestOffsetMapper& map_offset,
    std::uint64_t guest_code_limit = (1ULL << 48));

}  // namespace arm64metal
