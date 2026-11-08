#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arm64metal {

class SharedCache {
public:
    static constexpr std::int64_t guestBase = 0x1'0000'0000LL;
    static constexpr std::int64_t guestLimit = 1LL << 44;
    static constexpr const char* swiftPrespecializedLibrary = "/usr/lib/libswiftPrespecialized.dylib";

    struct Authentication {
        std::uint16_t diversity;
        bool addressDiversity;
        bool dataKey;
    };

    struct Rebase {
        std::uint64_t location;
        std::uint64_t target;
        std::uint8_t highByte;
        std::optional<Authentication> authentication;
    };

    struct Segment {
        std::string name;
        std::uint64_t address;
        std::uint64_t size;
        std::uint64_t fileOffset;
        std::uint64_t fileSize;
        bool writable;
    };

    struct Dependency {
        std::string name;
        bool weak;
        bool reexported;
        bool upward = false;
        bool delayed_init = false;
    };

    struct GuestImage {
        std::vector<Segment> segments;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> text;
        std::unordered_map<std::string, std::int64_t> exports;
        std::vector<Dependency> dependencies;
    };

    // Section geometry for the image's static Mach-O thread-local variables.
    // All addresses are translated to the guest cache window. Callback section
    // ranges contain pointers; their values must be read after cache rebases.
    struct TlvMetadata {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> thunk_ranges;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> template_ranges;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> init_callback_ranges;
        std::uint64_t template_address = 0;
        std::uint64_t template_size = 0;
        std::uint64_t template_alignment = 1;
        bool all_zero_fill = true;
    };

    enum class InitializerKind { routines_command, function_pointer, function_offset };

    struct Initializer {
        InitializerKind kind;
        std::uint64_t slot_address;
        std::uint64_t target_address;
        std::uint8_t high_byte = 0;
        std::optional<Authentication> authentication;
    };

    struct Export {
        std::uint64_t offset;
        std::uint32_t firstWord;
    };

    struct ResolvedExport {
        std::int64_t guest_address;
        std::int64_t guest_header;
        std::string install_name;
    };

    struct HeaderTable {
        std::int64_t address;
        std::uint64_t size;
    };

    struct MessageSendOffsets {
        std::int64_t address;
        std::uint32_t count;
    };

    struct CacheRegion {
        std::int64_t start;
        std::uint64_t size;
        std::uint64_t flags;
        std::uint32_t protection;
        std::uint32_t maximumProtection;
    };

    struct HostExports {
        std::unordered_map<std::int64_t, std::string> functions;
        std::unordered_map<std::uint64_t, std::string> data;
    };

    struct DataBinding {
        Rebase rebase;
        std::string name;
    };

    explicit SharedCache();
    ~SharedCache();
    SharedCache(SharedCache&&) noexcept;
    SharedCache& operator=(SharedCache&&) noexcept;
    SharedCache(const SharedCache&) = delete;
    SharedCache& operator=(const SharedCache&) = delete;

    std::uint64_t mappedSize() const noexcept;
    const std::vector<std::uint8_t>& uuid() const noexcept;
    const std::unordered_map<std::string, std::uint64_t>& images() const noexcept;
    const std::vector<CacheRegion>& cacheRegions() const noexcept;
    std::optional<bool> newFormatTlvs() const noexcept;
    std::optional<std::int64_t> tlvGetAddrFastEntry() const;

    std::vector<std::uint8_t> bytes(std::uint64_t address, std::uint64_t count) const;
    std::int64_t guestAddress(std::uint64_t address) const;
    std::uint64_t originalAddress(std::int64_t guestAddress) const;
    std::optional<Export> exportSymbol(const std::string& path, const std::string& name) const;
    std::optional<ResolvedExport> resolveExport(const std::string& path,
                                                const std::string& name) const;
    std::optional<GuestImage> guestImage(const std::string& path) const;
    // Root-first breadth-first closure of declared cached dylib dependencies.
    // Weak dependencies are included when their install name exists in cache.
    // Returned guest headers are deduplicated by address, including aliases.
    std::vector<std::int64_t> declaredDependencyHeaders(const std::string& path) const;
    std::optional<TlvMetadata> tlvMetadata(const std::string& path) const;
    std::vector<Initializer> initializers(const std::string& path) const;
    std::vector<Rebase> rebases(const std::string& path) const;
    std::vector<Rebase> rebases(std::uint64_t start, std::uint64_t end,
                                const std::unordered_set<std::uint64_t>* targets = nullptr) const;
    HostExports hostExportAddresses(const std::unordered_map<std::string, int>& hostServiceIds,
                                    const std::unordered_set<std::string>& guestMemoryImports) const;
    std::vector<DataBinding> mergedDataBindings(const std::unordered_map<std::uint64_t, std::string>& targets) const;
    std::optional<std::string> preboundLazyDependency(const std::string& owner, std::uint64_t flag) const;
    std::vector<std::pair<std::uint64_t, std::string>> preboundLazyDependencies(
        const std::string& owner) const;
    std::optional<std::int64_t> localeKeyAddress() const;
    bool isImmutable(std::int64_t start, std::int64_t end) const noexcept;
    bool isCacheDataConst(std::int64_t start, std::int64_t end) const noexcept;
    std::optional<Segment> constantRebaseSlot(std::int64_t guestAddress) const;
    std::optional<std::uint32_t> selectorStubInstruction(std::int64_t guestAddress) const;
    std::optional<std::int64_t> stubTarget(std::int64_t guestAddress) const;
    std::pair<std::int64_t, std::int64_t> selectorWindow(std::int64_t guestAddress,
                                                        std::uint64_t count) const;
    std::optional<std::size_t> selectorStringSize(std::int64_t guestAddress,
                                                  std::size_t limit) const;

    std::optional<HeaderTable> objcHeaderTable(bool writable) const;
    std::uint32_t objcClassCount() const;
    std::optional<std::int64_t> objcImageHeader(std::size_t index) const;
    std::optional<std::size_t> objcHeaderIndex(std::int64_t header) const;
    std::optional<MessageSendOffsets> objcMessageSendOffsets() const;
    std::optional<std::int64_t> swiftPrespecializedAddress() const;
    std::optional<std::int64_t> swiftConformance(
        std::uint64_t protocolAddress, std::uint64_t metadata, std::uint64_t type,
        const std::function<bool(std::size_t)>& isLoaded) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arm64metal
