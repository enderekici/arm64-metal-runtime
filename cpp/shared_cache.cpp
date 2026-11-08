#include "shared_cache.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace arm64metal {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

class MappedFile {
public:
    explicit MappedFile(const std::string& path) : path_(path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("Cannot open dyld cache file " + path + ": " + std::strerror(errno));
        struct stat info {};
        if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
            const int saved = errno;
            ::close(fd);
            fail("Cannot stat dyld cache file " + path + ": " + std::strerror(saved));
        }
        size_ = static_cast<std::size_t>(info.st_size);
        data_ = static_cast<const std::uint8_t*>(::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0));
        const int saved = errno;
        ::close(fd);
        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            fail("Cannot map dyld cache file " + path + ": " + std::strerror(saved));
        }
    }
    ~MappedFile() { if (data_ != nullptr) ::munmap(const_cast<std::uint8_t*>(data_), size_); }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::size_t size() const noexcept { return size_; }
    const std::uint8_t* data() const noexcept { return data_; }
    const std::string& path() const noexcept { return path_; }

    const std::uint8_t* at(std::uint64_t offset, std::uint64_t count) const {
        if (offset > size_ || count > size_ - offset) fail("Dyld cache file range is invalid: " + path_);
        return data_ + static_cast<std::size_t>(offset);
    }

private:
    std::string path_;
    std::size_t size_ = 0;
    const std::uint8_t* data_ = nullptr;
};

std::uint64_t readWord(const std::uint8_t* data, std::size_t size, std::size_t offset, unsigned width) {
    if (width == 0 || width > 8 || offset > size || width > size - offset) fail("Truncated Mach-O integer");
    std::uint64_t value = 0;
    for (unsigned index = 0; index < width; ++index) value |= static_cast<std::uint64_t>(data[offset + index]) << (8 * index);
    return value;
}

std::uint64_t readWord(const std::vector<std::uint8_t>& data, std::size_t offset, unsigned width) {
    return readWord(data.data(), data.size(), offset, width);
}

std::uint64_t readWord(const MappedFile& file, std::size_t offset, unsigned width) {
    return readWord(file.data(), file.size(), offset, width);
}

std::uint64_t uleb(const std::uint8_t* bytes, std::size_t size, std::size_t& cursor) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    for (unsigned index = 0; index < 10; ++index) {
        if (cursor >= size) fail("Truncated export-trie ULEB128");
        const auto byte = bytes[cursor++];
        if (shift == 63 && (byte & 0x7e) != 0) fail("Overflowing export-trie ULEB128");
        result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return result;
        shift += 7;
    }
    fail("Oversized export-trie ULEB128");
}

struct ExportEntry {
    enum class Kind { Address, Reexport } kind;
    std::uint64_t offset = 0;
    std::uint64_t ordinal = 0;
    std::string imported;
};

std::unordered_map<std::string, ExportEntry> parseExports(const std::vector<std::uint8_t>& trie,
                                                          std::uint64_t memorySize) {
    constexpr std::uint64_t reexportFlag = 0x08;
    constexpr std::uint64_t resolverFlag = 0x10;
    std::unordered_map<std::string, ExportEntry> result;
    std::unordered_set<std::uint64_t> active;
    std::function<void(std::uint64_t, std::string, unsigned)> visit =
        [&](std::uint64_t node, std::string prefix, unsigned depth) {
            if (depth > 512 || node >= trie.size() || !active.insert(node).second) fail("Invalid or cyclic export trie");
            std::size_t cursor = static_cast<std::size_t>(node);
            const auto terminalSize = uleb(trie.data(), trie.size(), cursor);
            if (terminalSize > trie.size() - cursor) fail("Export trie terminal exceeds payload");
            const auto terminalEnd = cursor + static_cast<std::size_t>(terminalSize);
            if (terminalSize != 0) {
                const auto flags = uleb(trie.data(), terminalEnd, cursor);
                if ((flags & reexportFlag) != 0) {
                    const auto ordinal = uleb(trie.data(), terminalEnd, cursor);
                    const auto* start = reinterpret_cast<const char*>(trie.data() + cursor);
                    const auto* end = static_cast<const char*>(std::memchr(start, 0, terminalEnd - cursor));
                    if (end == nullptr) fail("Invalid export-trie reexport name");
                    result.emplace(prefix, ExportEntry{ExportEntry::Kind::Reexport, 0, ordinal,
                                                       std::string(start, end)});
                } else if ((flags & (0x03U | resolverFlag)) == 0) {
                    const auto offset = uleb(trie.data(), terminalEnd, cursor);
                    if (offset >= memorySize) fail("Export-trie address exceeds image");
                    result.emplace(prefix, ExportEntry{ExportEntry::Kind::Address, offset, 0, {}});
                }
            }
            cursor = terminalEnd;
            if (cursor >= trie.size()) fail("Truncated export-trie child count");
            const auto childCount = trie[cursor++];
            for (unsigned child = 0; child < childCount; ++child) {
                const auto* start = reinterpret_cast<const char*>(trie.data() + cursor);
                const auto* end = static_cast<const char*>(std::memchr(start, 0, trie.size() - cursor));
                if (end == nullptr) fail("Invalid export-trie edge");
                const std::string edge(start, end);
                cursor += edge.size() + 1;
                const auto next = uleb(trie.data(), trie.size(), cursor);
                if (next >= trie.size()) fail("Invalid export-trie child offset");
                visit(next, prefix + edge, depth + 1);
            }
            active.erase(node);
        };
    if (!trie.empty()) visit(0, {}, 0);
    return result;
}

}  // namespace

struct SharedCache::Impl {
    struct Mapping {
        std::uint64_t start = 0;
        std::uint64_t size = 0;
        std::uint64_t fileOffset = 0;
        std::shared_ptr<MappedFile> file;
        std::optional<std::pair<std::size_t, std::size_t>> slideInfo;
        std::uint64_t flags = 0;
        std::uint32_t protection = 0;
        std::uint32_t maximumProtection = 0;
    };

    struct Image {
        struct Section {
            std::string segmentName;
            std::string sectionName;
            std::uint64_t address = 0;
            std::uint64_t size = 0;
        };
        struct TlvSection {
            std::uint64_t address = 0;
            std::uint64_t size = 0;
            std::uint32_t alignment = 0;
            std::uint32_t type = 0;
        };
        struct InitializerSection {
            std::uint64_t address = 0;
            std::uint64_t size = 0;
            std::uint32_t type = 0;
        };
        std::uint64_t address = 0;
        std::string installName;
        std::vector<Segment> segments;
        std::vector<TlvSection> tlvSections;
        std::vector<InitializerSection> initializerSections;
        std::vector<Section> sections;
        std::vector<std::uint64_t> routinesInitializers;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> text;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> exports;
        std::vector<std::string> reexports;
        std::vector<Dependency> dependencies;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> lazyLoads;
    };

    std::vector<Mapping> mappings;
    std::unordered_map<std::string, std::uint64_t> images;
    std::uint64_t base = 0;
    std::uint64_t mappedSize = 0;
    std::uint64_t objcOptsOffset = 0;
    std::uint64_t objcOptsSize = 0;
    std::uint64_t swiftOptsOffset = 0;
    std::uint64_t swiftOptsSize = 0;
    std::optional<bool> hasNewFormatTlvs;
    std::vector<std::uint8_t> uuid;
    std::vector<CacheRegion> publicRegions;
    std::vector<SharedCache::TproMapping> tproMappings;

    explicit Impl() {
        const std::string filename = "dyld_shared_cache_arm64e";
        const std::array<std::string, 2> dirs = {
            "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld",
            "/System/Library/dyld"};
        std::string path;
        for (const auto& dir : dirs) {
            const auto candidate = dir + "/" + filename;
            if (std::filesystem::exists(candidate)) { path = candidate; break; }
        }
        if (path.empty()) fail("Unable to locate the ARM64e dyld shared cache");
        auto main = std::make_shared<MappedFile>(path);
        const auto headerSize = cacheHeader(*main);
        // Apple dyld_cache_format.h places newFormatTLVs at bit 12 of the
        // uint32_t flags word at offset 220 in dyld_cache_header.
        if (headerSize >= 224) {
            constexpr std::uint32_t newFormatTlvsMask = 1U << 12U;
            hasNewFormatTlvs = (readWord(*main, 220, 4) & newFormatTlvsMask) != 0;
        }
        base = readWord(*main, 224, 8);
        mappedSize = readWord(*main, 232, 8);
        if (mappedSize == 0 || mappedSize > static_cast<std::uint64_t>(guestLimit - guestBase) ||
            mappedSize > std::numeric_limits<std::uint64_t>::max() - base) fail("Invalid dyld shared-cache span");
        swiftOptsOffset = headerSize >= 392 ? readWord(*main, 376, 8) : 0;
        swiftOptsSize = headerSize >= 392 ? readWord(*main, 384, 8) : 0;
        objcOptsOffset = headerSize >= 480 ? readWord(*main, 464, 8) : 0;
        objcOptsSize = headerSize >= 480 ? readWord(*main, 472, 8) : 0;
        uuid.assign(main->data() + 88, main->data() + 104);
        if (headerSize < 456) fail("Unsupported dyld shared-cache header");
        const auto subOffset = static_cast<std::size_t>(readWord(*main, 392, 4));
        const auto subCount = static_cast<std::size_t>(readWord(*main, 396, 4));
        const auto imageOffset = static_cast<std::size_t>(readWord(*main, 448, 4));
        const auto imageCount = static_cast<std::size_t>(readWord(*main, 452, 4));
        const std::size_t subStride = headerSize > 456 ? 56 : 24;
        if (subOffset > main->size() || subCount > (main->size() - subOffset) / subStride ||
            imageOffset > main->size() || imageCount > (main->size() - imageOffset) / 32) {
            fail("Invalid dyld shared-cache index");
        }
        for (std::size_t index = 0; index < imageCount; ++index) {
            const auto entry = imageOffset + index * 32;
            const auto address = readWord(*main, entry, 8);
            const auto pathOffset = readWord(*main, entry + 24, 4);
            if (pathOffset >= main->size()) fail("Invalid dyld shared-cache install name");
            const auto* start = reinterpret_cast<const char*>(main->data() + pathOffset);
            const auto* end = static_cast<const char*>(std::memchr(start, 0, main->size() - pathOffset));
            if (end == nullptr || end == start) fail("Invalid dyld shared-cache install name");
            images.emplace(std::string(start, end), address);
        }
        addMappings(main);
        for (std::size_t index = 0; index < subCount; ++index) {
            const auto entry = subOffset + index * subStride;
            std::string suffix;
            if (subStride == 56) {
                const auto* start = reinterpret_cast<const char*>(main->data() + entry + 24);
                const auto* end = static_cast<const char*>(std::memchr(start, 0, 32));
                suffix.assign(start, end == nullptr ? start + 32 : end);
            } else {
                suffix = "." + std::to_string(index + 1);
            }
            if (suffix.empty() || suffix.front() != '.' || suffix.find('/') != std::string::npos) {
                fail("Invalid dyld subcache suffix");
            }
            auto file = std::make_shared<MappedFile>(path + suffix);
            if (file->size() < 104 || std::memcmp(main->data() + entry, file->data() + 88, 16) != 0) {
                fail("Dyld subcache UUID mismatch");
            }
            addMappings(file);
        }
        if (headerSize >= 520) {
            constexpr std::size_t tproOffsetField = 512;
            constexpr std::size_t tproCountField = 516;
            constexpr std::size_t tproEntrySize = 16;
            constexpr std::uint64_t tproDataFlag = 1ULL << 6U;
            const auto tableOffset = static_cast<std::size_t>(readWord(*main, tproOffsetField, 4));
            const auto tableCount = static_cast<std::size_t>(readWord(*main, tproCountField, 4));
            if (tableCount != 0) {
                if (tableOffset == 0 || tableOffset > main->size() ||
                    tableCount > (main->size() - tableOffset) / tproEntrySize) {
                    fail("Invalid dyld shared-cache TPRO mapping table");
                }
                tproMappings.reserve(tableCount);
                for (std::size_t index = 0; index < tableCount; ++index) {
                    const auto entry = tableOffset + index * tproEntrySize;
                    const auto unslidAddress = readWord(*main, entry, 8);
                    const auto size = readWord(*main, entry + 8, 8);
                    if (size == 0 || size > std::numeric_limits<std::uint64_t>::max() - unslidAddress) {
                        fail("Invalid dyld shared-cache TPRO mapping range");
                    }
                    const auto mapping = std::find_if(mappings.begin(), mappings.end(),
                        [unslidAddress, size](const Mapping& candidate) {
                            return (candidate.flags & tproDataFlag) != 0 &&
                                unslidAddress >= candidate.start &&
                                unslidAddress - candidate.start < candidate.size &&
                                size <= candidate.size - (unslidAddress - candidate.start);
                        });
                    if (mapping == mappings.end()) {
                        fail("Dyld shared-cache TPRO range is outside mapped cache data");
                    }
                    const auto guestAddress = this->guestAddress(unslidAddress);
                    const auto guestLastAddress = this->guestAddress(unslidAddress + size - 1);
                    if (guestLastAddress < guestAddress ||
                        static_cast<std::uint64_t>(guestLastAddress - guestAddress) != size - 1) {
                        fail("Dyld shared-cache TPRO range exceeds guest address space");
                    }
                    tproMappings.push_back({unslidAddress, size, guestAddress});
                }
            }
        }
        publicRegions.reserve(mappings.size());
        for (const auto& mapping : mappings) {
            publicRegions.push_back(CacheRegion{guestAddress(mapping.start), mapping.size, mapping.flags,
                                                mapping.protection, mapping.maximumProtection});
        }
    }

    static std::size_t cacheHeader(const MappedFile& file) {
        const auto* magic = reinterpret_cast<const char*>(file.data());
        const std::string_view value(magic, strnlen(magic, 16));
        if (value != "dyld_v1  arm64e") fail("Unsupported dyld shared-cache magic");
        return static_cast<std::size_t>(readWord(file, 16, 4));
    }

    void addMappings(const std::shared_ptr<MappedFile>& file) {
        const auto header = cacheHeader(*file);
        const auto modernCount = header >= 320 ? readWord(*file, 316, 4) : 0;
        const std::size_t stride = modernCount > 0 ? 56 : 32;
        const auto offset = modernCount > 0 ? readWord(*file, 312, 4) : header;
        const auto count = modernCount > 0 ? modernCount : readWord(*file, 20, 4);
        if (offset > file->size() || count > (file->size() - offset) / stride) fail("Invalid dyld shared-cache mapping table");
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto entry = static_cast<std::size_t>(offset + index * stride);
            Mapping mapping;
            mapping.start = readWord(*file, entry, 8);
            mapping.size = readWord(*file, entry + 8, 8);
            mapping.fileOffset = readWord(*file, entry + 16, 8);
            if (mapping.size > std::numeric_limits<std::uint64_t>::max() - mapping.start ||
                mapping.fileOffset > file->size() || mapping.size > file->size() - mapping.fileOffset) {
                fail("Invalid dyld shared-cache mapping range");
            }
            if (modernCount > 0) {
                const auto infoOffset = readWord(*file, entry + 24, 8);
                const auto infoSize = readWord(*file, entry + 32, 8);
                if (infoOffset > file->size() || infoSize > file->size() - infoOffset) fail("Invalid dyld slide info range");
                if (infoSize != 0) mapping.slideInfo = std::make_pair(static_cast<std::size_t>(infoOffset), static_cast<std::size_t>(infoSize));
                mapping.flags = readWord(*file, entry + 40, 8);
                mapping.maximumProtection = static_cast<std::uint32_t>(readWord(*file, entry + 48, 4));
                mapping.protection = static_cast<std::uint32_t>(readWord(*file, entry + 52, 4));
            } else {
                mapping.maximumProtection = static_cast<std::uint32_t>(readWord(*file, entry + 24, 4));
                mapping.protection = static_cast<std::uint32_t>(readWord(*file, entry + 28, 4));
            }
            mapping.file = file;
            mappings.push_back(std::move(mapping));
        }
    }

    std::vector<std::uint8_t> bytes(std::uint64_t address, std::uint64_t count) const {
        for (const auto& mapping : mappings) {
            if (address < mapping.start || address - mapping.start > mapping.size ||
                count > mapping.size - (address - mapping.start)) continue;
            if (count > std::numeric_limits<std::size_t>::max()) fail("Dyld cache read is too large");
            const auto* source = mapping.file->at(mapping.fileOffset + (address - mapping.start), count);
            return std::vector<std::uint8_t>(source, source + static_cast<std::size_t>(count));
        }
        fail("Dyld shared-cache address is unmapped");
    }

    std::int64_t guestAddress(std::uint64_t address) const {
        if (address < base || address - base >= static_cast<std::uint64_t>(SharedCache::guestLimit - SharedCache::guestBase)) {
            fail("Dyld cache address exceeds guest window");
        }
        return SharedCache::guestBase + static_cast<std::int64_t>(address - base);
    }

    std::uint64_t originalAddress(std::int64_t address) const {
        if (address < SharedCache::guestBase || address >= SharedCache::guestLimit) fail("Guest address is outside dyld cache window");
        return base + static_cast<std::uint64_t>(address - SharedCache::guestBase);
    }

    Image image(const std::string& path) const {
        const auto found = images.find(path);
        if (found == images.end()) fail("Image is absent from the dyld shared cache: " + path);
        const auto address = found->second;
        const auto header = bytes(address, 32);
        if (readWord(header, 0, 4) != 0xfeedfacf || readWord(header, 4, 4) != 0x0100000c) {
            fail("Invalid cached ARM64 Mach-O header");
        }
        const auto count = static_cast<std::size_t>(readWord(header, 16, 4));
        const auto commandSize = static_cast<std::size_t>(readWord(header, 20, 4));
        if (count > commandSize / 8) fail("Invalid cached Mach-O load commands");
        const auto commands = bytes(address + 32, commandSize);
        Image result;
        result.address = address;
        std::size_t cursor = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto command = readWord(commands, cursor, 4);
            const auto size = static_cast<std::size_t>(readWord(commands, cursor + 4, 4));
            if (size < 8 || size > commandSize - cursor) fail("Invalid cached Mach-O load command");
            auto read = [&](std::size_t offset, unsigned width) { return readWord(commands, cursor + offset, width); };
            if (command == 0x19 && size >= 72) {
                const auto* nameBytes = reinterpret_cast<const char*>(commands.data() + cursor + 8);
                const auto* nameEnd = static_cast<const char*>(std::memchr(nameBytes, 0, 16));
                const std::string name(nameBytes, nameEnd == nullptr ? nameBytes + 16 : nameEnd);
                const auto start = read(24, 8), segmentSize = read(32, 8), fileSize = read(48, 8);
                if (segmentSize > std::numeric_limits<std::uint64_t>::max() - start || fileSize > segmentSize) fail("Invalid cached Mach-O segment");
                const bool writable = (read(60, 4) & 2) != 0 && (read(68, 4) & 0x10) == 0;
                result.segments.push_back(Segment{name, start, segmentSize, read(40, 8), fileSize, writable});
                const auto sectionCount = static_cast<std::size_t>(read(64, 4));
                if (sectionCount > (size - 72) / 80) fail("Invalid cached Mach-O sections");
                for (std::size_t section = 0; section < sectionCount; ++section) {
                    const auto sectionOffset = 72 + section * 80;
                    const auto readFixedName = [&](std::size_t offset) {
                        const auto* name = reinterpret_cast<const char*>(
                            commands.data() + cursor + sectionOffset + offset);
                        const auto* end = static_cast<const char*>(std::memchr(name, 0, 16));
                        return std::string(name, end == nullptr ? name + 16 : end);
                    };
                    const auto sectionName = readFixedName(0);
                    const auto sectionSegmentName = readFixedName(16);
                    const auto sectionAddress = read(sectionOffset + 32, 8);
                    const auto sectionSize = read(sectionOffset + 40, 8);
                    if (sectionAddress < start || sectionAddress - start > segmentSize ||
                        sectionSize > segmentSize - (sectionAddress - start)) {
                        fail("Invalid cached Mach-O section");
                    }
                    result.sections.push_back({sectionSegmentName, sectionName,
                                               sectionAddress, sectionSize});
                    const auto type = static_cast<std::uint32_t>(read(sectionOffset + 64, 4) & 0xffU);
                    if (type == 0x09 || type == 0x16) {
                        result.initializerSections.push_back({sectionAddress, sectionSize, type});
                    }
                    if (type != 0x11 && type != 0x12 && type != 0x13 && type != 0x15) continue;
                    const auto alignment = static_cast<std::uint32_t>(read(sectionOffset + 52, 4));
                    if (alignment >= 63 || sectionAddress < start ||
                        sectionAddress - start > segmentSize ||
                        sectionSize > segmentSize - (sectionAddress - start)) {
                        fail("Invalid cached Mach-O thread-local section");
                    }
                    result.tlvSections.push_back({sectionAddress, sectionSize, alignment, type});
                }
                if (name == "__TEXT") {
                    for (std::size_t section = 0; section < sectionCount; ++section) {
                        const auto sectionOffset = 72 + section * 80;
                        if ((read(sectionOffset + 64, 4) & 0x80000400) == 0) continue;
                        const auto textStart = read(sectionOffset + 32, 8), textSize = read(sectionOffset + 40, 8);
                        if (textSize > std::numeric_limits<std::uint64_t>::max() - textStart) fail("Invalid cached Mach-O text");
                        result.text.emplace_back(textStart, textStart + textSize);
                    }
                }
            }
            if (command == 0x1a) {
                if (size < 72) fail("Truncated cached LC_ROUTINES_64 command");
                const auto initializer = read(8, 8);
                if (initializer != 0) result.routinesInitializers.push_back(initializer);
            }
            if (command == 0xd) {
                if (size < 24) fail("Truncated cached LC_ID_DYLIB command");
                const auto nameOffset = static_cast<std::size_t>(read(8, 4));
                if (nameOffset < 24 || nameOffset >= size) fail("Invalid cached dylib install name");
                const auto* start = reinterpret_cast<const char*>(commands.data() + cursor + nameOffset);
                const auto* end = static_cast<const char*>(std::memchr(start, 0, size - nameOffset));
                if (end == nullptr) fail("Invalid cached dylib install name");
                result.installName.assign(start, end);
            }
            if (command == 0x80000033 && size >= 16) result.exports = {{read(8, 4), read(12, 4)}};
            else if ((command == 0x22 || command == 0x80000022) && size >= 48 && !result.exports) result.exports = {{read(40, 4), read(44, 4)}};
            if (command == 0x3a && size >= 16) result.lazyLoads.emplace_back(read(8, 4), read(12, 4));
            if (command == 0xc || command == 0x80000018 || command == 0x8000001f || command == 0x20 || command == 0x80000023) {
                if (size < 24) fail("Truncated cached dylib dependency");
                const auto nameOffset = static_cast<std::size_t>(read(8, 4));
                const bool usesFlags = read(12, 4) == 0x1a741800;
                std::uint32_t flags = 0;
                if (usesFlags) {
                    if ((command != 0xc && command != 0x80000018) || size < 28 || nameOffset != 28)
                        fail("Invalid cached dylib use command");
                    flags = static_cast<std::uint32_t>(read(24, 4));
                    if ((flags & ~0x0fU) != 0) fail("Unsupported cached dylib use flags");
                }
                if (nameOffset < 24 || nameOffset >= size) fail("Invalid cached dylib dependency name");
                const auto* start = reinterpret_cast<const char*>(commands.data() + cursor + nameOffset);
                const auto* end = static_cast<const char*>(std::memchr(start, 0, size - nameOffset));
                if (end == nullptr) fail("Invalid cached dylib dependency name");
                const bool weak = usesFlags ? (flags & 1U) != 0 : command == 0x80000018;
                const bool reexported = usesFlags ? (flags & 2U) != 0 : command == 0x8000001f;
                const bool upward = usesFlags ? (flags & 4U) != 0 : command == 0x80000023;
                result.dependencies.push_back(Dependency{std::string(start, end), weak,
                                                        reexported, upward, (flags & 8U) != 0});
                if (reexported) result.reexports.emplace_back(start, end);
            }
            cursor += size;
        }
        return result;
    }

    std::unordered_map<std::string, ExportEntry> exportTable(const Image& image) const {
        const auto linkedit = std::find_if(image.segments.begin(), image.segments.end(), [](const Segment& s) { return s.name == "__LINKEDIT"; });
        if (linkedit == image.segments.end() || !image.exports || image.exports->second == 0 ||
            image.exports->first < linkedit->fileOffset || image.exports->first - linkedit->fileOffset > linkedit->fileSize ||
            image.exports->second > linkedit->fileSize - (image.exports->first - linkedit->fileOffset)) {
            fail("Cached Mach-O export trie is missing or invalid");
        }
        const auto trie = bytes(linkedit->address + (image.exports->first - linkedit->fileOffset), image.exports->second);
        return parseExports(trie, std::numeric_limits<std::uint64_t>::max());
    }

    std::vector<Rebase> rebases(std::uint64_t start, std::uint64_t end,
                                const std::unordered_set<std::uint64_t>* targets) const {
        if (end < start) fail("Invalid dyld rebase range");
        std::vector<Rebase> result;
        for (const auto& mapping : mappings) {
            if (!mapping.slideInfo || mapping.size == 0) continue;
            const auto low = std::max(start, mapping.start);
            const auto high = std::min(end, mapping.start + mapping.size);
            if (low >= high) continue;
            const auto infoStart = mapping.slideInfo->first;
            const auto infoSize = mapping.slideInfo->second;
            auto readInfo = [&](std::size_t offset, unsigned width) {
                if (offset > infoSize || width > infoSize - offset) fail("Truncated dyld slide info");
                return readWord(*mapping.file, infoStart + offset, width);
            };
            if (readInfo(0, 4) != 5) fail("Unsupported dyld slide info version");
            const auto pageSize = readInfo(4, 4), count = readInfo(8, 4), slideBase = readInfo(16, 8);
            if ((pageSize != 4096 && pageSize != 16384) || infoSize < 24 || count > (infoSize - 24) / 2 || count != mapping.size / pageSize) {
                fail("Invalid dyld slide page table");
            }
            const auto firstPage = (low - mapping.start) / pageSize;
            const auto lastPage = (high - 1 - mapping.start) / pageSize;
            for (auto page = firstPage; page <= lastPage; ++page) {
                auto chainOffset = readInfo(24 + static_cast<std::size_t>(page) * 2, 2);
                if (chainOffset == 0xffff) continue;
                while (true) {
                    if ((chainOffset & 7) != 0 || chainOffset > pageSize - 8) fail("Invalid dyld slide chain offset");
                    const auto location = mapping.start + page * pageSize + chainOffset;
                    if (location >= high) break;
                    const auto raw = readWord(*mapping.file, static_cast<std::size_t>(mapping.fileOffset + page * pageSize + chainOffset), 8);
                    if (location >= low) {
                        if (high - location < 8) fail("Dyld rebase crosses a segment boundary");
                        const auto runtimeOffset = raw & ((1ULL << 34) - 1);
                        if (runtimeOffset > std::numeric_limits<std::uint64_t>::max() - slideBase) fail("Dyld rebase target overflow");
                        const auto target = slideBase + runtimeOffset;
                        if (targets == nullptr || targets->find(target) != targets->end()) {
                            const bool auth = (raw >> 63) != 0;
                            std::optional<Authentication> authentication;
                            if (auth) authentication = Authentication{static_cast<std::uint16_t>(raw >> 34), ((raw >> 50) & 1) != 0, ((raw >> 51) & 1) != 0};
                            result.push_back(Rebase{location, target, auth ? std::uint8_t{0} : static_cast<std::uint8_t>(raw >> 34), authentication});
                        }
                    }
                    const auto next = (raw >> 52) & 0x7ff;
                    if (next == 0) break;
                    chainOffset += next * 8;
                }
            }
        }
        return result;
    }
};

SharedCache::SharedCache() : impl_(std::make_unique<Impl>()) {}
SharedCache::~SharedCache() = default;
SharedCache::SharedCache(SharedCache&&) noexcept = default;
SharedCache& SharedCache::operator=(SharedCache&&) noexcept = default;
std::uint64_t SharedCache::mappedSize() const noexcept { return impl_->mappedSize; }
const std::vector<std::uint8_t>& SharedCache::uuid() const noexcept { return impl_->uuid; }
const std::unordered_map<std::string, std::uint64_t>& SharedCache::images() const noexcept { return impl_->images; }
const std::vector<SharedCache::CacheRegion>& SharedCache::cacheRegions() const noexcept { return impl_->publicRegions; }
const std::vector<SharedCache::TproMapping>& SharedCache::tproMappings() const noexcept { return impl_->tproMappings; }
std::optional<bool> SharedCache::newFormatTlvs() const noexcept { return impl_->hasNewFormatTlvs; }

std::optional<std::int64_t> SharedCache::tlvGetAddrFastEntry() const {
    if (!impl_->hasNewFormatTlvs.value_or(false)) return std::nullopt;
    constexpr const char* libdyldPath = "/usr/lib/system/libdyld.dylib";
    if (impl_->images.find(libdyldPath) == impl_->images.end()) return std::nullopt;
    const auto libdyld = impl_->image(libdyldPath);
    constexpr std::uint32_t nop = 0xd503201fU;
    constexpr std::uint32_t branchMask = 0xfc000000U;
    constexpr std::uint32_t branch = 0x14000000U;
    constexpr std::array<std::uint32_t, 8> fastEntry = {
        0xb9400810U,  // ldr w16, [x0, #8] (TLV_Thunkv2.key)
        0xd53bd071U,  // mrs x17, TPIDRRO_EL0
        0x927df231U,  // and x17, x17, #-8
        0xf8707a31U,  // ldr x17, [x17, x16, lsl #3]
        0xb4000091U,  // cbz x17, lazy-allocation path
        0xb9400c10U,  // ldr w16, [x0, #12] (TLV_Thunkv2.offset)
        0x8b100220U,  // add x0, x17, x16
        0xd65f03c0U   // ret
    };
    std::optional<std::uint64_t> match;
    for (const auto& range : libdyld.text) {
        if (range.second < range.first || range.second - range.first < 8 + fastEntry.size() * 4) {
            continue;
        }
        const auto codeSize = range.second - range.first;
        if (codeSize > 16 * 1024 * 1024) fail("Cached libdyld text range exceeds scan bound");
        const auto code = impl_->bytes(range.first, codeSize);
        const auto readInstruction = [&code](std::size_t offset) {
            return static_cast<std::uint32_t>(readWord(code, offset, 4));
        };
        for (std::size_t offset = 0; offset + 8 + fastEntry.size() * 4 <= code.size(); offset += 4) {
            if ((readInstruction(offset) & branchMask) != branch ||
                readInstruction(offset + 4) != nop) continue;
            bool valid = true;
            for (std::size_t instruction = 0; instruction < fastEntry.size(); ++instruction) {
                if (readInstruction(offset + 8 + instruction * 4) != fastEntry[instruction]) {
                    valid = false;
                    break;
                }
            }
            if (!valid) continue;
            const auto address = range.first + offset + 8;
            if (match) return std::nullopt;
            match = address;
        }
    }
    if (!match) return std::nullopt;
    return guestAddress(*match);
}

std::vector<std::uint8_t> SharedCache::bytes(std::uint64_t address, std::uint64_t count) const { return impl_->bytes(address, count); }
std::int64_t SharedCache::guestAddress(std::uint64_t address) const { return impl_->guestAddress(address); }
std::uint64_t SharedCache::originalAddress(std::int64_t address) const { return impl_->originalAddress(address); }

std::optional<SharedCache::Export> SharedCache::exportSymbol(const std::string& path, const std::string& name) const {
    const auto image = impl_->image(path);
    const auto table = impl_->exportTable(image);
    const auto found = table.find(name);
    if (found == table.end() || found->second.kind != ExportEntry::Kind::Address) return std::nullopt;
    const auto word = impl_->bytes(image.address + found->second.offset, 4);
    return Export{found->second.offset, static_cast<std::uint32_t>(readWord(word, 0, 4))};
}

std::optional<SharedCache::ResolvedExport> SharedCache::resolveExport(
    const std::string& path, const std::string& name) const {
    constexpr std::size_t maximumVisited = 4096;
    struct Lookup {
        std::string path;
        std::string symbol;
        bool operator==(const Lookup& other) const {
            return path == other.path && symbol == other.symbol;
        }
    };
    struct LookupHash {
        std::size_t operator()(const Lookup& lookup) const {
            return std::hash<std::string>{}(lookup.path) ^
                (std::hash<std::string>{}(lookup.symbol) << 1U);
        }
    };

    std::unordered_map<std::string, Impl::Image> parsedImages;
    std::unordered_map<std::string, std::unordered_map<std::string, ExportEntry>> tables;
    std::unordered_set<Lookup, LookupHash> active;
    std::size_t visited = 0;
    const auto getImage = [this, &parsedImages](const std::string& imagePath)
        -> const Impl::Image& {
        auto found = parsedImages.find(imagePath);
        if (found == parsedImages.end()) {
            found = parsedImages.emplace(imagePath, impl_->image(imagePath)).first;
        }
        return found->second;
    };
    const auto getTable = [this, &tables, &getImage](const std::string& imagePath)
        -> const std::unordered_map<std::string, ExportEntry>& {
        auto found = tables.find(imagePath);
        if (found == tables.end()) {
            found = tables.emplace(imagePath, impl_->exportTable(getImage(imagePath))).first;
        }
        return found->second;
    };
    const auto canonicalPath = [this](const std::string& imagePath,
                                      const Impl::Image& image) {
        if (!image.installName.empty()) {
            const auto canonical = impl_->images.find(image.installName);
            if (canonical != impl_->images.end() && canonical->second == image.address) {
                return image.installName;
            }
        }
        return imagePath;
    };

    std::function<std::optional<ResolvedExport>(const std::string&, const std::string&)> resolve;
    resolve = [&](const std::string& imagePath, const std::string& symbol)
        -> std::optional<ResolvedExport> {
        if (impl_->images.find(imagePath) == impl_->images.end()) return std::nullopt;
        const Lookup lookup{imagePath, symbol};
        if (!active.insert(lookup).second) return std::nullopt;
        if (++visited > maximumVisited) fail("Cached re-export lookup exceeds 4096 images");

        const auto& image = getImage(imagePath);
        const auto& exports = getTable(imagePath);
        const auto exported = exports.find(symbol);
        if (exported != exports.end()) {
            if (exported->second.kind == ExportEntry::Kind::Address) {
                if (exported->second.offset >
                    std::numeric_limits<std::uint64_t>::max() - image.address) {
                    fail("Cached export address overflows");
                }
                active.erase(lookup);
                return ResolvedExport{
                    guestAddress(image.address + exported->second.offset),
                    guestAddress(image.address), canonicalPath(imagePath, image)};
            }
            const auto ordinal = exported->second.ordinal;
            if (ordinal == 0 || ordinal > image.dependencies.size()) {
                active.erase(lookup);
                return std::nullopt;
            }
            const auto& dependency = image.dependencies[static_cast<std::size_t>(ordinal - 1)];
            const auto& imported = exported->second.imported.empty()
                ? symbol : exported->second.imported;
            auto result = resolve(dependency.name, imported);
            active.erase(lookup);
            return result;
        }

        for (const auto& dependency : image.dependencies) {
            if (!dependency.reexported) continue;
            if (auto result = resolve(dependency.name, symbol)) {
                active.erase(lookup);
                return result;
            }
        }
        active.erase(lookup);
        return std::nullopt;
    };

    return resolve(path, name);
}

std::optional<SharedCache::GuestImage> SharedCache::guestImage(const std::string& path) const {
    if (impl_->images.find(path) == impl_->images.end()) return std::nullopt;
    const auto root = impl_->image(path);
    struct Lookup { std::string path; std::string symbol; bool operator==(const Lookup& other) const { return path == other.path && symbol == other.symbol; } };
    struct Hash { std::size_t operator()(const Lookup& v) const { return std::hash<std::string>{}(v.path) ^ (std::hash<std::string>{}(v.symbol) << 1); } };
    std::unordered_map<std::string, Impl::Image> parsed{{path, root}};
    std::unordered_map<std::string, std::unordered_map<std::string, ExportEntry>> tables;
    auto getImage = [&](const std::string& key) -> const Impl::Image& {
        auto found = parsed.find(key);
        if (found == parsed.end()) found = parsed.emplace(key, impl_->image(key)).first;
        return found->second;
    };
    auto getTable = [&](const std::string& key) -> const std::unordered_map<std::string, ExportEntry>& {
        auto found = tables.find(key);
        if (found == tables.end()) found = tables.emplace(key, impl_->exportTable(getImage(key))).first;
        return found->second;
    };
    std::unordered_set<Lookup, Hash> active;
    std::function<std::optional<std::int64_t>(const std::string&, const std::string&)> resolve =
        [&](const std::string& ownerPath, const std::string& symbol) -> std::optional<std::int64_t> {
            const Lookup key{ownerPath, symbol};
            if (!active.insert(key).second || impl_->images.find(ownerPath) == impl_->images.end()) return std::nullopt;
            const auto& owner = getImage(ownerPath);
            const auto& exports = getTable(ownerPath);
            const auto found = exports.find(symbol);
            if (found != exports.end() && found->second.kind == ExportEntry::Kind::Address) {
                active.erase(key);
                return impl_->guestAddress(owner.address + found->second.offset);
            }
            if (found != exports.end() && found->second.kind == ExportEntry::Kind::Reexport &&
                found->second.ordinal > 0 && found->second.ordinal <= owner.dependencies.size()) {
                const auto& imported = found->second.imported.empty() ? symbol : found->second.imported;
                auto value = resolve(owner.dependencies[found->second.ordinal - 1].name, imported);
                active.erase(key);
                return value;
            }
            for (const auto& dependency : owner.dependencies) {
                if (dependency.reexported) {
                    auto value = resolve(dependency.name, symbol);
                    if (value) { active.erase(key); return value; }
                }
            }
            active.erase(key);
            return std::nullopt;
        };
    std::unordered_set<std::string> seen;
    std::function<void(const std::string&, std::set<std::string>&)> addNames =
        [&](const std::string& ownerPath, std::set<std::string>& names) {
            if (!seen.insert(ownerPath).second || impl_->images.find(ownerPath) == impl_->images.end()) return;
            const auto& owner = getImage(ownerPath);
            for (const auto& item : getTable(ownerPath)) names.insert(item.first);
            for (const auto& dependency : owner.dependencies) if (dependency.reexported) addNames(dependency.name, names);
        };
    std::set<std::string> names;
    addNames(path, names);
    GuestImage result;
    result.segments = root.segments;
    result.segments.erase(std::remove_if(result.segments.begin(), result.segments.end(), [](const Segment& segment) {
        return segment.name == "__LINKEDIT" || segment.size == 0;
    }), result.segments.end());
    result.text = root.text;
    result.dependencies = root.dependencies;
    for (const auto& name : names) {
        active.clear();
        if (auto address = resolve(path, name)) result.exports.emplace(name, *address);
    }
    return result;
}

std::vector<std::int64_t> SharedCache::declaredDependencyHeaders(
    const std::string& path) const {
    constexpr std::size_t maximumImages = 4096;
    constexpr std::size_t maximumDependencies = 65536;
    if (impl_->images.find(path) == impl_->images.end()) return {};

    std::vector<std::int64_t> result;
    std::vector<std::pair<std::string, std::uint64_t>> pending;
    std::unordered_set<std::uint64_t> seenHeaders;
    pending.emplace_back(path, impl_->images.at(path));
    std::size_t dependencyCount = 0;

    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto& current = pending[cursor];
        const auto header = static_cast<std::uint64_t>(guestAddress(current.second));
        if (!seenHeaders.insert(header).second) continue;
        if (seenHeaders.size() > maximumImages) {
            fail("Declared cached dependency closure exceeds 4096 images");
        }
        result.push_back(static_cast<std::int64_t>(header));

        const auto image = impl_->image(current.first);
        for (const auto& dependency : image.dependencies) {
            if (++dependencyCount > maximumDependencies) {
                fail("Declared cached dependency closure exceeds 65536 edges");
            }
            const auto found = impl_->images.find(dependency.name);
            if (found == impl_->images.end()) continue;
            pending.emplace_back(dependency.name, found->second);
        }
    }
    return result;
}

std::optional<SharedCache::TlvMetadata> SharedCache::tlvMetadata(
    const std::string& path) const {
    if (impl_->images.find(path) == impl_->images.end()) return std::nullopt;
    const auto image = impl_->image(path);
    TlvMetadata result;
    std::uint64_t templateStart = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t templateEnd = 0;
    bool hasTemplate = false;
    for (const auto& section : image.tlvSections) {
        const auto address = static_cast<std::uint64_t>(guestAddress(section.address));
        if (section.size > std::numeric_limits<std::uint64_t>::max() - address) {
            fail("Cached thread-local section exceeds guest address range");
        }
        if (section.type == 0x13) {
            result.thunk_ranges.emplace_back(address, section.size);
        } else if (section.type == 0x15) {
            result.init_callback_ranges.emplace_back(address, section.size);
        } else {
            result.template_ranges.emplace_back(address, section.size);
            templateStart = std::min(templateStart, address);
            templateEnd = std::max(templateEnd, address + section.size);
            result.template_alignment = std::max(
                result.template_alignment, std::uint64_t{1} << section.alignment);
            if (section.type == 0x11) result.all_zero_fill = false;
            hasTemplate = true;
        }
    }
    if (!hasTemplate && result.thunk_ranges.empty() &&
        result.init_callback_ranges.empty()) return std::nullopt;
    if (hasTemplate) {
        result.template_address = templateStart;
        result.template_size = templateEnd - templateStart;
    }
    return result;
}

std::optional<SharedCache::PointerCell> SharedCache::dyldApisCell() const {
    constexpr const char* path = "/usr/lib/system/libdyld.dylib";
    const auto image = impl_->image(path);
    const Impl::Image::Section* match = nullptr;
    for (const auto& section : image.sections) {
        if (section.segmentName != "__TPRO_CONST" || section.sectionName != "__dyld_apis") {
            continue;
        }
        if (match != nullptr) fail("Duplicate cached __dyld_apis sections");
        match = &section;
    }
    if (match == nullptr) return std::nullopt;
    if (match->size != sizeof(std::uint64_t)) {
        fail("Unsupported cached __dyld_apis pointer-cell size");
    }
    (void)bytes(match->address, sizeof(std::uint64_t));
    return PointerCell{guestAddress(match->address), match->size};
}

std::vector<SharedCache::Initializer> SharedCache::initializers(
    const std::string& path) const {
    const auto image = impl_->image(path);
    constexpr std::uint64_t maximumInitializers = 1U << 20;
    std::vector<Initializer> result;
    const auto isExecutable = [&image](std::uint64_t address) {
        return std::any_of(image.text.begin(), image.text.end(), [address](const auto& range) {
            return (address & 3U) == 0 && address >= range.first &&
                range.second - range.first >= 4 &&
                address - range.first <= range.second - range.first - 4;
        });
    };
    const auto add = [&](InitializerKind kind, std::uint64_t slot,
                         std::uint64_t target, std::uint8_t highByte,
                         std::optional<Authentication> authentication) {
        if (!isExecutable(target)) fail("Cached initializer target is outside executable image text");
        if (result.size() >= maximumInitializers) fail("Cached image has too many initializers");
        const auto guestSlot = slot == 0 ? std::uint64_t{0}
            : static_cast<std::uint64_t>(guestAddress(slot));
        result.push_back({kind, guestSlot, static_cast<std::uint64_t>(guestAddress(target)),
                          highByte, authentication});
    };

    for (const auto target : image.routinesInitializers) {
        add(InitializerKind::routines_command, 0, target, 0, std::nullopt);
    }

    std::optional<std::unordered_map<std::uint64_t, Rebase>> rebasesBySlot;
    for (const auto& section : image.initializerSections) {
        if (section.type == 0x09) {
            if ((section.size & 7U) != 0 || section.size / 8 > maximumInitializers) {
                fail("Invalid cached function-pointer initializer section");
            }
            if (!rebasesBySlot) {
                rebasesBySlot.emplace();
                for (const auto& rebase : rebases(path)) {
                    rebasesBySlot->emplace(rebase.location, rebase);
                }
            }
            for (std::uint64_t offset = 0; offset < section.size; offset += 8) {
                const auto slot = section.address + offset;
                if (const auto found = rebasesBySlot->find(slot);
                    found != rebasesBySlot->end()) {
                    const auto& rebase = found->second;
                    add(InitializerKind::function_pointer, slot, rebase.target,
                        rebase.highByte, rebase.authentication);
                    continue;
                }
                const auto raw = readWord(bytes(slot, 8), 0, 8);
                if (raw != 0 && isExecutable(raw)) {
                    add(InitializerKind::function_pointer, slot, raw, 0, std::nullopt);
                } else if (raw != 0) {
                    fail("Cached function-pointer initializer has no usable rebase");
                }
            }
        } else if (section.type == 0x16) {
            if ((section.size & 3U) != 0 || section.size / 4 > maximumInitializers) {
                fail("Invalid cached function-offset initializer section");
            }
            for (std::uint64_t offset = 0; offset < section.size; offset += 4) {
                const auto relative = readWord(bytes(section.address + offset, 4), 0, 4);
                if (relative > std::numeric_limits<std::uint64_t>::max() - image.address) {
                    fail("Cached function-offset initializer overflows image address");
                }
                const auto target = image.address + relative;
                add(InitializerKind::function_offset, section.address + offset,
                    target, 0, std::nullopt);
            }
        }
    }
    return result;
}

std::vector<SharedCache::Rebase> SharedCache::rebases(const std::string& path) const {
    const auto source = impl_->image(path);
    std::vector<Rebase> result;
    for (const auto& segment : source.segments) {
        if (segment.name == "__LINKEDIT" || segment.size == 0) continue;
        auto part = impl_->rebases(segment.address, segment.address + segment.size, nullptr);
        result.insert(result.end(), part.begin(), part.end());
    }
    std::sort(result.begin(), result.end(), [](const Rebase& a, const Rebase& b) { return a.location < b.location; });
    return result;
}
std::vector<SharedCache::Rebase> SharedCache::rebases(std::uint64_t start, std::uint64_t end,
                                                      const std::unordered_set<std::uint64_t>* targets) const {
    return impl_->rebases(start, end, targets);
}

SharedCache::HostExports SharedCache::hostExportAddresses(
    const std::unordered_map<std::string, int>& hostServiceIds,
    const std::unordered_set<std::string>& guestMemoryImports) const {
    const std::unordered_map<std::string, std::string> platformMemory = {
        {"___bzero", "_bzero"}, {"__platform_bzero", "_bzero"}, {"__platform_memmove", "_memmove"},
        {"__platform_memset", "_memset"}, {"__platform_memset_pattern4", "_memset_pattern4"},
        {"__platform_memset_pattern8", "_memset_pattern8"}, {"__platform_memset_pattern16", "_memset_pattern16"}};
    const std::unordered_set<std::string> modeledData = {
        "___stack_chk_guard", "___stdoutp", "___stderrp", "___stdinp", "___mb_cur_max", "_optind",
        "_optarg", "_environ", "_vm_page_size", "_vm_page_shift", "_vm_kernel_page_size", "_mach_task_self_"};
    std::vector<std::string> pending{"/usr/lib/libSystem.B.dylib"};
    std::unordered_set<std::string> visited;
    std::unordered_set<std::int64_t> ambiguous;
    HostExports result;
    while (!pending.empty()) {
        const auto path = pending.back();
        pending.pop_back();
        if (impl_->images.find(path) == impl_->images.end() || !visited.insert(path).second) continue;
        const auto source = impl_->image(path);
        pending.insert(pending.end(), source.reexports.begin(), source.reexports.end());
        if (!source.exports || source.exports->second == 0) continue;
        for (const auto& entry : impl_->exportTable(source)) {
            if (entry.second.kind != ExportEntry::Kind::Address) continue;
            const auto alias = platformMemory.find(entry.first);
            const std::string name = alias == platformMemory.end() ? entry.first : alias->second;
            if (modeledData.find(name) == modeledData.end() && hostServiceIds.find(name) == hostServiceIds.end() &&
                guestMemoryImports.find(name) == guestMemoryImports.end()) continue;
            const auto address = source.address + entry.second.offset;
            if (modeledData.find(name) != modeledData.end()) {
                result.data.emplace(address, name);
                continue;
            }
            const bool inText = std::any_of(source.text.begin(), source.text.end(), [address](const auto& range) {
                return address >= range.first && address < range.second;
            });
            if (!inText) continue;
            const auto guest = guestAddress(address);
            const auto prior = result.functions.find(guest);
            if (prior != result.functions.end() && prior->second != name) {
                const auto priorId = hostServiceIds.find(prior->second), nextId = hostServiceIds.find(name);
                if (priorId == hostServiceIds.end() || nextId == hostServiceIds.end() || priorId->second != nextId->second) {
                    ambiguous.insert(guest);
                }
            } else {
                result.functions[guest] = name;
            }
        }
    }
    for (const auto address : ambiguous) result.functions.erase(address);
    return result;
}

std::vector<SharedCache::DataBinding> SharedCache::mergedDataBindings(
    const std::unordered_map<std::uint64_t, std::string>& targets) const {
    std::unordered_set<std::uint64_t> addresses;
    addresses.reserve(targets.size());
    for (const auto& target : targets) addresses.insert(target.first);
    std::vector<DataBinding> result;
    for (const auto& mapping : impl_->mappings) {
        if ((mapping.flags & 4) == 0 || mapping.size == 0) continue;
        for (const auto& rebase : impl_->rebases(mapping.start, mapping.start + mapping.size, &addresses)) {
            if (rebase.authentication || rebase.highByte != 0) continue;
            const auto found = targets.find(rebase.target);
            if (found != targets.end()) result.push_back(DataBinding{rebase, found->second});
        }
    }
    return result;
}

std::optional<std::string> SharedCache::preboundLazyDependency(const std::string& owner,
                                                               std::uint64_t flag) const {
    const auto source = impl_->image(owner);
    const auto headerAddress = guestAddress(source.address);
    if (headerAddress < 0) return std::nullopt;
    const auto header = static_cast<std::uint64_t>(headerAddress);
    if (flag < header || (flag & 3U) != 0) return std::nullopt;
    for (const auto& dependency : preboundLazyDependencies(owner)) {
        if (dependency.first == flag) return dependency.second;
    }
    return std::nullopt;
}

std::vector<std::pair<std::uint64_t, std::string>>
SharedCache::preboundLazyDependencies(const std::string& owner) const {
    const auto source = impl_->image(owner);
    const auto linkedit = std::find_if(source.segments.begin(), source.segments.end(),
        [](const Segment& segment) { return segment.name == "__LINKEDIT"; });
    if (linkedit == source.segments.end()) return {};
    const auto headerAddress = guestAddress(source.address);
    if (headerAddress < 0) fail("Invalid cached lazy-load image header");
    const auto header = static_cast<std::uint64_t>(headerAddress);
    constexpr std::uint64_t maximumRecordSize = 1024 * 1024;
    constexpr std::size_t maximumRecords = 1024;
    std::vector<std::pair<std::uint64_t, std::string>> result;
    for (const auto& record : source.lazyLoads) {
        if (record.second < 24 || record.second > maximumRecordSize ||
            record.first < linkedit->fileOffset ||
            record.first - linkedit->fileOffset > linkedit->fileSize ||
            record.second > linkedit->fileSize - (record.first - linkedit->fileOffset)) {
            fail("Invalid cached lazy-load record");
        }
        const auto blob = bytes(linkedit->address + (record.first - linkedit->fileOffset),
                                record.second);
        if ((readWord(blob, 8, 2) & 2U) == 0) continue;
        const auto relativeFlag = readWord(blob, 4, 4);
        if (relativeFlag > std::numeric_limits<std::uint64_t>::max() - header) {
            fail("Invalid cached lazy-load flag address");
        }
        const auto flag = header + relativeFlag;
        if ((flag & 3U) != 0) fail("Unaligned cached lazy-load flag address");
        if (relativeFlag > std::numeric_limits<std::uint64_t>::max() - source.address) {
            fail("Invalid cached lazy-load flag address");
        }
        const auto originalFlag = source.address + relativeFlag;
        const bool mappedFlag = std::any_of(source.segments.begin(), source.segments.end(),
            [originalFlag](const Segment& segment) {
                return originalFlag >= segment.address &&
                       originalFlag - segment.address < segment.size;
            });
        if (!mappedFlag) fail("Cached lazy-load flag is outside its owning image");
        const auto nameOffset = static_cast<std::size_t>(readWord(blob, 0, 4));
        if (nameOffset < 24 || nameOffset >= blob.size()) {
            fail("Invalid cached lazy dependency path");
        }
        const auto* name = reinterpret_cast<const char*>(blob.data() + nameOffset);
        const auto* end = static_cast<const char*>(std::memchr(name, 0, blob.size() - nameOffset));
        if (end == nullptr || end == name) fail("Invalid cached lazy dependency path");
        if (result.size() >= maximumRecords) fail("Cached image has too many lazy-load records");
        result.emplace_back(flag, std::string(name, end));
    }
    return result;
}

std::optional<std::int64_t> SharedCache::localeKeyAddress() const {
    const auto libc = impl_->image("/usr/lib/system/libsystem_c.dylib");
    const auto exports = impl_->exportTable(libc);
    const auto symbol = exports.find("_strtoll");
    if (symbol == exports.end() || symbol->second.kind != ExportEntry::Kind::Address) return std::nullopt;
    const auto pc = libc.address + symbol->second.offset;
    const auto code = bytes(pc, 16);
    const auto adrp = readWord(code, 0, 4), load = readWord(code, 4, 4);
    if ((adrp & 0x9f00001f) != 0x90000008 || (load & 0xffc003ff) != 0xf9400108 ||
        readWord(code, 8, 4) != 0xd53bd069 || readWord(code, 12, 4) != 0xf8687928) return std::nullopt;
    const auto immediate = static_cast<std::uint64_t>(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3));
    const auto pages = immediate >= (1ULL << 20) ? static_cast<std::int64_t>(immediate) - (1LL << 21) : static_cast<std::int64_t>(immediate);
    const auto slotSigned = static_cast<std::int64_t>(pc & ~4095ULL) + pages * 4096 + static_cast<std::int64_t>((load >> 10) & 0xfff) * 8;
    if (slotSigned < 0) return std::nullopt;
    const auto slot = static_cast<std::uint64_t>(slotSigned);
    const bool inWritableFileBytes = std::any_of(libc.segments.begin(), libc.segments.end(), [slot](const Segment& segment) {
        return segment.writable && slot >= segment.address && slot - segment.address < segment.fileSize &&
            segment.fileSize - (slot - segment.address) >= 8;
    });
    if (!inWritableFileBytes || readWord(bytes(slot, 8), 0, 8) != std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    return guestAddress(slot);
}

bool SharedCache::isImmutable(std::int64_t start, std::int64_t end) const noexcept {
    if (start < guestBase || end < start || end >= guestLimit) return false;
    const auto address = impl_->base + static_cast<std::uint64_t>(start - guestBase);
    const auto limit = impl_->base + static_cast<std::uint64_t>(end - guestBase);
    return std::any_of(impl_->mappings.begin(), impl_->mappings.end(), [address, limit](const Impl::Mapping& mapping) {
        return address > mapping.start && limit < mapping.start + mapping.size && (mapping.maximumProtection & 2) == 0;
    });
}

bool SharedCache::isCacheDataConst(std::int64_t start, std::int64_t end) const noexcept {
    if (start < guestBase || end < start || end >= guestLimit) return false;
    const auto address = impl_->base + static_cast<std::uint64_t>(start - guestBase);
    const auto size = static_cast<std::uint64_t>(end - start);
    return std::any_of(impl_->mappings.begin(), impl_->mappings.end(), [address, size](const Impl::Mapping& mapping) {
        return (mapping.flags & 4) != 0 && (mapping.protection & 3) == 1 && (mapping.maximumProtection & 2) != 0 &&
            address >= mapping.start && address - mapping.start < mapping.size && size <= mapping.size - (address - mapping.start);
    });
}

std::optional<SharedCache::Segment> SharedCache::constantRebaseSlot(std::int64_t address) const {
    if (address < guestBase || address >= guestLimit) return std::nullopt;
    const auto original = impl_->base + static_cast<std::uint64_t>((address & ~7LL) - guestBase);
    const bool inConst = std::any_of(impl_->mappings.begin(), impl_->mappings.end(), [original](const Impl::Mapping& mapping) {
        return (mapping.flags & 4) != 0 && (mapping.protection & 3) == 1 && original >= mapping.start &&
            original - mapping.start < mapping.size && 8 <= mapping.size - (original - mapping.start);
    });
    if (!inConst || impl_->rebases(original, original + 8, nullptr).empty()) return std::nullopt;
    return Segment{"cache const rebase", original, 8, 0, 8, false};
}

std::optional<std::uint32_t> SharedCache::selectorStubInstruction(std::int64_t guest) const {
    if (guest < guestBase || guest >= guestLimit || (guest & 3) != 0) return std::nullopt;
    const auto offset = static_cast<std::uint64_t>(guest & 15);
    if (offset >= 12) return std::nullopt;
    const auto address = originalAddress(guest - static_cast<std::int64_t>(offset));
    const bool textStub = std::any_of(impl_->mappings.begin(), impl_->mappings.end(), [address](const Impl::Mapping& mapping) {
        return (mapping.protection & 4) != 0 && address >= mapping.start && address - mapping.start < mapping.size && 16 <= mapping.size - (address - mapping.start);
    });
    if (!textStub) return std::nullopt;
    const auto code = bytes(address, 16);
    if ((readWord(code, 0, 4) & 0x9f00001f) != 0x90000001 ||
        (readWord(code, 4, 4) & 0xffc003ff) != 0x91000021 ||
        (readWord(code, 8, 4) & 0xfc000000) != 0x14000000 || readWord(code, 12, 4) != 0xd4200020) return std::nullopt;
    return static_cast<std::uint32_t>(readWord(code, static_cast<std::size_t>(offset), 4));
}

std::optional<std::int64_t> SharedCache::stubTarget(std::int64_t guest) const {
    if (guest < guestBase || guest >= guestLimit || (guest & 15) != 0) return std::nullopt;
    const auto address = originalAddress(guest);
    const bool inStubs = std::any_of(impl_->mappings.begin(), impl_->mappings.end(), [address](const Impl::Mapping& mapping) {
        return (mapping.flags & 8) != 0 && address >= mapping.start && address - mapping.start < mapping.size && 16 <= mapping.size - (address - mapping.start);
    });
    if (!inStubs) return std::nullopt;
    const auto code = bytes(address, 16);
    const auto adrp = readWord(code, 0, 4), add = readWord(code, 4, 4);
    if ((adrp & 0x9f00001f) != 0x90000011 || (add & 0xffc003ff) != 0x91000231 ||
        readWord(code, 8, 4) != 0xf9400230 || readWord(code, 12, 4) != 0xd71f0a11) return std::nullopt;
    const auto immediate = static_cast<std::uint64_t>(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3));
    const auto pages = immediate >= (1ULL << 20) ? static_cast<std::int64_t>(immediate) - (1LL << 21) : static_cast<std::int64_t>(immediate);
    const auto slotSigned = static_cast<std::int64_t>(address & ~4095ULL) + pages * 4096 + static_cast<std::int64_t>((add >> 10) & 0xfff);
    if (slotSigned < 0 || (slotSigned & 7) != 0) return std::nullopt;
    const auto slot = static_cast<std::uint64_t>(slotSigned);
    const auto chain = impl_->rebases(slot, slot + 8, nullptr);
    if (chain.empty() || chain.front().location != slot || !chain.front().authentication) return std::nullopt;
    const auto auth = *chain.front().authentication;
    if (!auth.addressDiversity || auth.dataKey || auth.diversity != 0) return std::nullopt;
    return guestAddress(chain.front().target);
}

std::pair<std::int64_t, std::int64_t> SharedCache::selectorWindow(std::int64_t guest,
                                                                  std::uint64_t count) const {
    const auto address = originalAddress(guest);
    const auto found = std::find_if(impl_->mappings.begin(), impl_->mappings.end(), [address](const Impl::Mapping& mapping) {
        return address >= mapping.start && address - mapping.start < mapping.size;
    });
    if (found == impl_->mappings.end() || found->slideInfo || (found->protection & 3) != 1) fail("Selector is outside immutable cache mappings");
    const std::uint64_t windowSize = 16 * 1024;
    const auto windowStart = address & ~(windowSize - 1);
    const auto lower = std::max(windowStart, found->start);
    const auto requestedEnd = count > std::numeric_limits<std::uint64_t>::max() - address ? found->start + found->size : address + count;
    const auto upper = std::min(std::max(windowStart + windowSize, requestedEnd), found->start + found->size);
    const auto start = guestAddress(lower);
    return {start, start + static_cast<std::int64_t>(upper - lower)};
}

std::optional<std::size_t> SharedCache::selectorStringSize(std::int64_t guest, std::size_t limit) const {
    const auto address = originalAddress(guest);
    const auto found = std::find_if(impl_->mappings.begin(), impl_->mappings.end(), [address](const Impl::Mapping& mapping) {
        return address >= mapping.start && address - mapping.start < mapping.size && !mapping.slideInfo &&
            (mapping.maximumProtection & 2) == 0 && (mapping.protection & 1) != 0;
    });
    if (found == impl_->mappings.end()) return std::nullopt;
    const auto size = std::min<std::uint64_t>(limit, found->size - (address - found->start));
    const auto* start = found->file->at(found->fileOffset + address - found->start, size);
    const auto* end = static_cast<const std::uint8_t*>(std::memchr(start, 0, static_cast<std::size_t>(size)));
    if (end == nullptr) return std::nullopt;
    return static_cast<std::size_t>(end - start) + 1;
}

std::optional<SharedCache::HeaderTable> SharedCache::objcHeaderTable(bool writable) const {
    const auto offsetField = writable ? 16U : 8U;
    if (impl_->objcOptsOffset == 0 || impl_->objcOptsSize < offsetField + 8) return std::nullopt;
    const auto opts = bytes(impl_->base + impl_->objcOptsOffset, offsetField + 8);
    const auto offset = readWord(opts, offsetField, 8);
    if (offset == 0) return std::nullopt;
    const auto address = impl_->base + offset;
    const auto header = bytes(address, 8);
    const auto count = readWord(header, 0, 4), stride = readWord(header, 4, 4);
    if (stride < 8 || (stride & 7) != 0 || offset > impl_->mappedSize || impl_->mappedSize - offset < 8 ||
        count > (impl_->mappedSize - offset - 8) / stride) fail("Invalid Objective-C header table size");
    const auto size = 8 + count * stride;
    const bool validMapping = std::any_of(impl_->mappings.begin(), impl_->mappings.end(), [&](const Impl::Mapping& mapping) {
        const bool protections = writable ? (mapping.protection & 3) == 3 : !mapping.slideInfo &&
            (mapping.maximumProtection & 2) == 0 && (mapping.protection & 1) != 0;
        return protections && address >= mapping.start && address - mapping.start < mapping.size && size <= mapping.size - (address - mapping.start);
    });
    if (!validMapping) fail("Invalid Objective-C header table mapping");
    if (writable && !impl_->rebases(address, address + size, nullptr).empty()) fail("Unsupported rebase in writable Objective-C header table");
    return HeaderTable{guestAddress(address), size};
}

std::uint32_t SharedCache::objcClassCount() const {
    if (impl_->objcOptsOffset == 0 || impl_->objcOptsSize < 40) return 0;
    const auto opts = bytes(impl_->base + impl_->objcOptsOffset, 40);
    const auto offset = readWord(opts, 32, 8);
    if (offset == 0) return 0;
    const auto table = bytes(impl_->base + offset, 20);
    const auto capacity = readWord(table, 4, 4), occupied = readWord(table, 8, 4);
    const auto tabSize = readWord(table, 16, 4) + 1;
    if (occupied > capacity || (tabSize & (tabSize - 1)) != 0) fail("Invalid Objective-C class table");
    const auto duplicateOffset = offset + 0x420 + tabSize + capacity * 13;
    const auto duplicates = bytes(impl_->base + duplicateOffset, 4);
    const auto total = occupied + readWord(duplicates, 0, 4);
    if (total > std::numeric_limits<std::uint32_t>::max()) fail("Invalid Objective-C class count");
    return static_cast<std::uint32_t>(total);
}

std::optional<std::int64_t> SharedCache::objcImageHeader(std::size_t index) const {
    const auto table = objcHeaderTable(false);
    if (!table) return std::nullopt;
    const auto address = originalAddress(table->address);
    const auto prefix = bytes(address, 8);
    const auto count = readWord(prefix, 0, 4), stride = readWord(prefix, 4, 4);
    if (index >= count) fail("Invalid Objective-C cache image index");
    const auto entry = address + 8 + index * stride;
    return guestAddress(entry + readWord(bytes(entry, 8), 0, 8));
}

std::optional<std::size_t> SharedCache::objcHeaderIndex(std::int64_t header) const {
    const auto table = objcHeaderTable(false);
    if (!table) return std::nullopt;
    const auto address = originalAddress(table->address);
    const auto prefix = bytes(address, 8);
    const auto count = static_cast<std::size_t>(readWord(prefix, 0, 4));
    const auto stride = static_cast<std::size_t>(readWord(prefix, 4, 4));
    const auto target = originalAddress(header);
    std::size_t lower = 0, upper = count;
    while (lower < upper) {
        const auto index = lower + (upper - lower) / 2;
        const auto entry = address + 8 + index * stride;
        const auto candidate = entry + readWord(bytes(entry, 8), 0, 8);
        if (candidate == target) return index;
        if (candidate < target) lower = index + 1;
        else upper = index;
    }
    return std::nullopt;
}

std::optional<SharedCache::MessageSendOffsets> SharedCache::objcMessageSendOffsets() const {
    if (impl_->objcOptsOffset == 0 || impl_->objcOptsSize < 0x88) return std::nullopt;
    const auto header = bytes(impl_->base + impl_->objcOptsOffset, 0x88);
    if (readWord(header, 0, 4) < 4) return std::nullopt;
    const auto descriptorOffset = readWord(header, 0x80, 8);
    if (descriptorOffset == 0) return std::nullopt;
    const auto descriptor = bytes(impl_->base + descriptorOffset, 16);
    const auto count = static_cast<std::uint32_t>(readWord(descriptor, 0, 4));
    const auto tableOffset = readWord(descriptor, 8, 8);
    if (count == 0 || tableOffset == 0) return std::nullopt;
    const auto size = static_cast<std::uint64_t>(count) * 8;
    if (tableOffset > static_cast<std::uint64_t>(guestLimit - guestBase) || size > static_cast<std::uint64_t>(guestLimit - guestBase) - tableOffset) fail("Invalid Objective-C message-send offsets table");
    const auto address = impl_->base + tableOffset;
    const bool valid = std::any_of(impl_->mappings.begin(), impl_->mappings.end(), [address, size](const Impl::Mapping& mapping) {
        return !mapping.slideInfo && (mapping.maximumProtection & 2) == 0 && address >= mapping.start &&
            address - mapping.start < mapping.size && size <= mapping.size - (address - mapping.start);
    });
    if (!valid) fail("Invalid Objective-C message-send offsets table");
    return MessageSendOffsets{guestAddress(address), count};
}

std::optional<std::int64_t> SharedCache::swiftPrespecializedAddress() const {
    if (impl_->swiftOptsOffset == 0 || impl_->swiftOptsSize < 40 || impl_->swiftOptsOffset >= impl_->mappedSize) return std::nullopt;
    const auto header = bytes(impl_->base + impl_->swiftOptsOffset, 40);
    if (readWord(header, 0, 4) < 2) return std::nullopt;
    const auto offset = readWord(header, 32, 8);
    if (offset == 0 || offset >= impl_->mappedSize || impl_->images.find(swiftPrespecializedLibrary) == impl_->images.end()) return std::nullopt;
    const auto address = impl_->base + offset;
    const auto owner = impl_->image(swiftPrespecializedLibrary);
    const bool inDataConst = std::any_of(owner.segments.begin(), owner.segments.end(), [address](const Segment& segment) {
        return segment.name == "__DATA_CONST" && address >= segment.address && address - segment.address < segment.fileSize;
    });
    if (!inDataConst) fail("Swift prespecialized data is outside its cache image");
    return guestAddress(address);
}

std::optional<std::int64_t> SharedCache::swiftConformance(
    std::uint64_t protocolAddress, std::uint64_t metadata, std::uint64_t type,
    const std::function<bool(std::size_t)>& isLoaded) const {
    const auto guestBase = static_cast<std::uint64_t>(SharedCache::guestBase);
    if (protocolAddress < guestBase || protocolAddress - guestBase >= impl_->mappedSize ||
        impl_->swiftOptsOffset == 0 || impl_->swiftOptsSize < 24 || impl_->swiftOptsOffset >= impl_->mappedSize) return std::nullopt;
    const auto header = bytes(impl_->base + impl_->swiftOptsOffset, 24);
    const auto protocolOffset = protocolAddress - guestBase;
    auto hashKey = [](std::uint64_t key, std::uint64_t salt) {
        std::uint64_t a = salt + key, b = salt, c = 0x9e3779b97f4a7c1bULL;
        const std::array<std::array<unsigned, 3>, 4> rounds = {{{43, 9, 8}, {38, 23, 5}, {35, 49, 11}, {12, 18, 22}}};
        for (const auto& r : rounds) {
            a = (a - b - c) ^ (c >> r[0]);
            b = (b - c - a) ^ (a << r[1]);
            c = (c - a - b) ^ (b >> r[2]);
        }
        return c;
    };
    for (const auto& item : std::array<std::pair<std::uint64_t, std::size_t>, 2>{{{type, 8}, {metadata, 16}}}) {
        const auto keyAddress = item.first;
        if (keyAddress < guestBase || keyAddress - guestBase >= impl_->mappedSize) continue;
        const auto tableOffset = readWord(header, item.second, 8);
        if (tableOffset == 0 || tableOffset >= impl_->mappedSize) continue;
        const auto table = impl_->base + tableOffset;
        constexpr std::uint64_t fixedSize = 32 + 256 * 4;
        const auto fixed = bytes(table, fixedSize);
        const auto capacity = readWord(fixed, 0, 4);
        if (capacity == 0) continue;
        const auto shift = readWord(fixed, 8, 4), mask = readWord(fixed, 12, 4), tabSize = readWord(fixed, 20, 4);
        const auto salt = readWord(fixed, 24, 8);
        const auto targetsStart = fixedSize + tabSize + capacity;
        const auto recordsStart = targetsStart + 4 * capacity;
        if (shift > 64 || mask >= tabSize || recordsStart > impl_->mappedSize - tableOffset) fail("Invalid Swift cache conformance table");
        const auto key = keyAddress - guestBase;
        const auto hash = hashKey(key, salt) ^ hashKey(protocolOffset, salt);
        const auto tab = readWord(bytes(table + fixedSize + (hash & mask), 1), 0, 1);
        const auto scramble = readWord(fixed, 32 + 4 * static_cast<std::size_t>(tab), 4);
        const auto bucket = static_cast<std::uint32_t>(shift == 64 ? 0 : hash >> shift) ^ scramble;
        if (bucket >= capacity) fail("Invalid Swift conformance bucket");
        const auto check = readWord(bytes(table + fixedSize + tabSize + bucket, 1), 0, 1);
        if (check != ((((key & 7) << 5) | 16))) continue;
        const auto target = readWord(bytes(table + targetsStart + 4 * bucket, 4), 0, 4);
        if (target == std::numeric_limits<std::uint32_t>::max()) continue;
        if (target < recordsStart) fail("Invalid Swift conformance target");
        auto address = table + target;
        while (true) {
            const auto record = bytes(address, 24);
            if (readWord(record, 0, 8) != key || readWord(record, 8, 8) != protocolOffset) break;
            const auto value = readWord(record, 16, 8);
            if (isLoaded(static_cast<std::size_t>(value >> 48))) {
                const auto offset = (value >> 1) & ((1ULL << 47) - 1);
                if (offset >= impl_->mappedSize) fail("Invalid Swift conformance descriptor");
                return guestAddress(impl_->base + offset);
            }
            if ((value & 1) == 0) break;
            address += 24;
        }
    }
    return std::nullopt;
}

}  // namespace arm64metal
