#include "guest_tlv.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace arm64metal {
namespace {

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error("guest TLV address overflow");
    }
    return left + right;
}

std::uint64_t slot_address(std::uint64_t threadTsd, std::uint32_t key) {
    if (threadTsd == 0) throw std::invalid_argument("guest TLV thread TSD is null");
    if (key >= GuestTlvRuntime::guestTsdSlotCount) {
        throw std::out_of_range("guest TLV key exceeds TSD capacity");
    }
    return checked_add(threadTsd, static_cast<std::uint64_t>(key) * sizeof(std::uint64_t));
}

void validate_callbacks(const GuestTlvRuntime::Read& read,
                        const GuestTlvRuntime::Write& write) {
    if (!read || !write) throw std::invalid_argument("guest TLV memory callbacks are required");
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw std::out_of_range("guest TLV descriptor is truncated");
    }
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint32_t>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

std::uint64_t read_u64_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t)) {
        throw std::out_of_range("guest TLV descriptor is truncated");
    }
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

}  // namespace

GuestTlvParsedDescriptor parse_guest_tlv_descriptor(
    const std::vector<std::uint8_t>& bytes, std::uint64_t descriptorAddress,
    GuestTlvDescriptorFormat format, const GuestTlvOwnerGeometry& owner) {
    constexpr std::uint64_t descriptorSize = 24;
    if (bytes.size() != descriptorSize) {
        throw std::invalid_argument("guest TLV descriptor must be exactly 24 bytes");
    }
    if (owner.templateSize == 0 ||
        owner.templateSize > std::numeric_limits<std::uint64_t>::max() - owner.templateBegin) {
        throw std::invalid_argument("guest TLV owner has invalid template geometry");
    }

    std::size_t ownerMatches = 0;
    for (const auto& range : owner.thunkRanges) {
        if (range.size > std::numeric_limits<std::uint64_t>::max() - range.begin) {
            throw std::invalid_argument("guest TLV owner has an overflowing thunk range");
        }
        const auto end = range.begin + range.size;
        if (descriptorAddress < range.begin || descriptorAddress > end ||
            descriptorSize > end - descriptorAddress) continue;
        if ((descriptorAddress - range.begin) % descriptorSize != 0) {
            throw std::invalid_argument("guest TLV descriptor is misaligned within its owner");
        }
        ++ownerMatches;
    }
    if (ownerMatches != 1) {
        throw std::invalid_argument("guest TLV descriptor does not have one unambiguous owner");
    }

    GuestTlvParsedDescriptor parsed{format, read_u64_le(bytes, 0), 0, 0,
                                    std::nullopt, 0};
    if (parsed.function == 0) {
        throw std::invalid_argument("guest TLV descriptor has no bootstrap function");
    }
    switch (format) {
        case GuestTlvDescriptorFormat::diskV1: {
            if (read_u64_le(bytes, 8) != 0) {
                throw std::invalid_argument("disk-v1 guest TLV key slot must be zero");
            }
            parsed.offset = read_u64_le(bytes, 16);
            if (parsed.offset >= owner.templateSize) {
                throw std::out_of_range("disk-v1 guest TLV offset exceeds its owner template");
            }
            break;
        }
        case GuestTlvDescriptorFormat::runtimeV2: {
            parsed.key = read_u32_le(bytes, 8);
            parsed.offset = read_u32_le(bytes, 12);
            const auto rawDelta = read_u32_le(bytes, 16);
            const auto signedDelta = rawDelta <= static_cast<std::uint32_t>(
                                     std::numeric_limits<std::int32_t>::max())
                ? static_cast<std::int64_t>(rawDelta)
                : static_cast<std::int64_t>(rawDelta) - (std::int64_t{1} << 32U);
            const auto contentSize = read_u32_le(bytes, 20);
            if (parsed.key == 0 || parsed.key >= GuestTlvRuntime::guestTsdSlotCount) {
                throw std::out_of_range("runtime-v2 guest TLV key exceeds TSD capacity");
            }
            if (parsed.offset >= owner.templateSize) {
                throw std::out_of_range("runtime-v2 guest TLV offset exceeds its owner template");
            }
            if (contentSize == 0 || contentSize != owner.templateSize) {
                throw std::invalid_argument("runtime-v2 guest TLV content size disagrees with its owner");
            }
            if (signedDelta == 0) {
                if (!owner.allZeroFill) {
                    throw std::invalid_argument(
                        "runtime-v2 zero delta conflicts with nonzero owner template data");
                }
            } else {
                if (owner.allZeroFill) {
                    throw std::invalid_argument(
                        "runtime-v2 nonzero delta conflicts with zero-fill owner template");
                }
                if (descriptorAddress > std::numeric_limits<std::uint64_t>::max() - 16) {
                    throw std::overflow_error("runtime-v2 guest TLV delta base overflows");
                }
                const auto deltaBase = descriptorAddress + 16;
                std::uint64_t contentAddress = 0;
                if (signedDelta > 0) {
                    contentAddress = checked_add(deltaBase,
                        static_cast<std::uint64_t>(signedDelta));
                } else {
                    const auto magnitude = static_cast<std::uint64_t>(-signedDelta);
                    if (magnitude > deltaBase) {
                        throw std::overflow_error("runtime-v2 guest TLV delta underflows");
                    }
                    contentAddress = deltaBase - magnitude;
                }
                const auto templateEnd = owner.templateBegin + owner.templateSize;
                if (contentAddress < owner.templateBegin || contentAddress > templateEnd ||
                    contentSize > templateEnd - contentAddress) {
                    throw std::out_of_range(
                        "runtime-v2 guest TLV initial content is outside its owner template");
                }
                parsed.initialContentAddress = contentAddress;
            }
            parsed.initialContentSize = contentSize;
            break;
        }
        default:
            throw std::invalid_argument("unknown guest TLV descriptor format");
    }
    return parsed;
}

void GuestTlvRuntime::register_image(std::uint32_t key,
                                     std::vector<std::uint8_t> initializedBytes,
                                     std::uint64_t zeroFillBytes,
                                     std::uint64_t alignment) {
    if (key >= guestTsdSlotCount) throw std::out_of_range("guest TLV key exceeds TSD capacity");
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument("guest TLV alignment must be a nonzero power of two");
    }
    const auto initializedSize = static_cast<std::uint64_t>(initializedBytes.size());
    const auto imageSize = checked_add(initializedSize, zeroFillBytes);
    if (imageSize == 0 || imageSize > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("guest TLV image size is invalid");
    }
    if (images_.find(key) != images_.end()) {
        throw std::invalid_argument("guest TLV key is already registered");
    }
    images_.emplace(key, Image{std::move(initializedBytes), zeroFillBytes, alignment});
}

std::optional<std::uint64_t> GuestTlvRuntime::resolve(
    std::uint64_t threadTsd, std::uint32_t key, std::uint64_t descriptorOffset,
    const Allocate& allocate, const Release& release, const Read& read,
    const Write& write) {
    validate_callbacks(read, write);
    if (!allocate || !release) throw std::invalid_argument("guest TLV allocation callbacks are required");

    const auto imageIt = images_.find(key);
    if (imageIt == images_.end()) throw std::out_of_range("guest TLV image key is not registered");
    const auto& image = imageIt->second;
    const auto imageSize = checked_add(static_cast<std::uint64_t>(image.initialBytes.size()),
                                       image.zeroFillBytes);
    if (descriptorOffset >= imageSize) {
        throw std::out_of_range("guest TLV descriptor offset is outside its image");
    }
    const auto slot = slot_address(threadTsd, key);
    std::uint64_t existing = 0;
    read(slot, reinterpret_cast<std::uint8_t*>(&existing), sizeof(existing));

    const auto blockKey = std::make_pair(threadTsd, key);
    const auto known = blocks_.find(blockKey);
    if (existing != 0) {
        if (known == blocks_.end() || known->second.address != existing ||
            known->second.size != imageSize) {
            throw std::runtime_error("guest TLV TSD slot contains an unmanaged pointer");
        }
        return checked_add(existing, descriptorOffset);
    }
    if (known != blocks_.end()) {
        throw std::runtime_error("guest TLV TSD slot no longer matches its allocation");
    }

    std::vector<std::uint8_t> contents(static_cast<std::size_t>(imageSize), 0);
    std::copy(image.initialBytes.begin(), image.initialBytes.end(), contents.begin());
    const auto address = allocate(imageSize, image.alignment);
    if (!address) return std::nullopt;
    if (*address == 0 || *address % image.alignment != 0 ||
        *address > std::numeric_limits<std::uint64_t>::max() - imageSize) {
        release(*address);
        throw std::runtime_error("guest TLV allocator returned an invalid address or alignment");
    }

    try {
        write(*address, contents.data(), contents.size());
        const auto inserted = blocks_.emplace(blockKey, Block{*address, imageSize});
        if (!inserted.second) throw std::runtime_error("guest TLV allocation already exists");
        const std::uint64_t guestPointer = *address;
        write(slot, reinterpret_cast<const std::uint8_t*>(&guestPointer), sizeof(guestPointer));
    } catch (...) {
        blocks_.erase(blockKey);
        try {
            const std::uint64_t empty = 0;
            write(slot, reinterpret_cast<const std::uint8_t*>(&empty), sizeof(empty));
        } catch (...) {
        }
        release(*address);
        throw;
    }
    return checked_add(*address, descriptorOffset);
}

void GuestTlvRuntime::release_thread(std::uint64_t threadTsd, const Release& release,
                                    const Write& write) {
    if (!release || !write) throw std::invalid_argument("guest TLV teardown callbacks are required");
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        if (it->first.first != threadTsd) {
            ++it;
            continue;
        }
        const auto slot = slot_address(threadTsd, it->first.second);
        const std::uint64_t empty = 0;
        write(slot, reinterpret_cast<const std::uint8_t*>(&empty), sizeof(empty));
        release(it->second.address);
        it = blocks_.erase(it);
    }
}

}  // namespace arm64metal
