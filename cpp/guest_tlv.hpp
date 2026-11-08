#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace arm64metal {

struct GuestTlvRange {
    std::uint64_t begin;
    std::uint64_t size;
};

enum class GuestTlvDescriptorFormat { diskV1, runtimeV2 };

struct GuestTlvOwnerGeometry {
    std::vector<GuestTlvRange> thunkRanges;
    std::uint64_t templateBegin = 0;
    std::uint64_t templateSize = 0;
    bool allZeroFill = false;
};

struct GuestTlvParsedDescriptor {
    GuestTlvDescriptorFormat format;
    std::uint64_t function;
    std::uint32_t key;
    std::uint64_t offset;
    std::optional<std::uint64_t> initialContentAddress;
    std::uint32_t initialContentSize = 0;
};

// Parses an explicitly identified 24-byte disk-v1 or runtime-v2 descriptor.
// `descriptorAddress` and all owner geometry use guest logical addresses.
[[nodiscard]] GuestTlvParsedDescriptor parse_guest_tlv_descriptor(
    const std::vector<std::uint8_t>& bytes, std::uint64_t descriptorAddress,
    GuestTlvDescriptorFormat format, const GuestTlvOwnerGeometry& owner);

// Materializes static Mach-O TLV images in guest memory. The descriptor key is
// the guest TSD slot index and descriptorOffset is its byte offset in the image.
// Initializers/destructors for dynamically initialized C++ thread_local values
// are outside this runtime's contract.
class GuestTlvRuntime {
public:
    static constexpr std::uint32_t guestTsdSlotCount = 1024;

    using Allocate = std::function<std::optional<std::uint64_t>(
        std::uint64_t size, std::uint64_t alignment)>;
    using Release = std::function<void(std::uint64_t address)>;
    using Read = std::function<void(
        std::uint64_t address, std::uint8_t* destination, std::size_t size)>;
    using Write = std::function<void(
        std::uint64_t address, const std::uint8_t* source, std::size_t size)>;

    // initializedBytes is the __thread_data template; zeroFillBytes is the
    // __thread_bss tail. Alignment is the combined image's required alignment.
    void register_image(std::uint32_t key,
                        std::vector<std::uint8_t> initializedBytes,
                        std::uint64_t zeroFillBytes,
                        std::uint64_t alignment);

    // Returns the guest address for one variable within the image. The TSD slot
    // is published only after the image is fully initialized in guest memory.
    std::optional<std::uint64_t> resolve(
        std::uint64_t threadTsd, std::uint32_t key,
        std::uint64_t descriptorOffset, const Allocate& allocate,
        const Release& release, const Read& read, const Write& write);

    // Clears registered slots and releases this thread's guest image blocks.
    void release_thread(std::uint64_t threadTsd, const Release& release,
                        const Write& write);

private:
    struct Image {
        std::vector<std::uint8_t> initialBytes;
        std::uint64_t zeroFillBytes;
        std::uint64_t alignment;
    };
    struct Block {
        std::uint64_t address;
        std::uint64_t size;
    };

    std::map<std::uint32_t, Image> images_;
    std::map<std::pair<std::uint64_t, std::uint32_t>, Block> blocks_;
};

}  // namespace arm64metal
