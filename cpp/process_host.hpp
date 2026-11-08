#pragma once
#include "guest_state.hpp"

// CPU-side process bootstrap and guest memory state for the ARM64-to-Metal
// runtime. Guest instructions remain in translated Metal kernels.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace arm64metal {

inline constexpr std::size_t guestVectorStateStart = 806;
inline constexpr std::size_t guestVectorRegisterCount = 64;
inline constexpr std::size_t guestFPCR = 870;
inline constexpr std::size_t guestFPSR = 871;
inline constexpr std::size_t guestTokenCount = 872;
inline constexpr std::size_t guestTokenCapacity = 873;
inline constexpr std::size_t guestFaultAddress = 874;
inline constexpr std::size_t guestCacheFaultAddress = 875;
inline constexpr std::size_t guestThreadPointer = 876;
inline constexpr std::size_t guestCacheFaultWidth = 877;
inline constexpr std::size_t guestPACGAKey = 878;
inline constexpr std::size_t guestTokenStart = 880;
inline constexpr std::int32_t guestUnixSyslogCaptureDescriptor = -1;
inline constexpr std::int32_t guestUnixClosedEndpoint = -2;
inline constexpr std::uint64_t guestCodeLimit = std::uint64_t{1} << 48;
inline constexpr std::uint64_t guestInitialTokenCapacity = 4096;
inline constexpr std::uint64_t guestMachTaskPort = 0x103;
inline constexpr std::uint64_t guestMachHostPort = 0x104;
inline constexpr std::uint64_t guestMachBootstrapPort = 0x105;
inline constexpr std::uint64_t guestMainThreadID = 1;
inline constexpr std::uint64_t guestThreadLocalPrefixBytes = 0x100;

struct GuestMemoryRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    [[nodiscard]] std::uint64_t size() const noexcept { return end - begin; }
};

struct GuestMemoryMapping {
    std::uint64_t logicalBegin = 0;
    std::uint64_t logicalEnd = 0;
    std::uint64_t backingBegin = 0;
    bool writable = false;
    bool readable = true;
};

using GuestMemoryMappingResolver =
    std::function<bool(std::uint64_t address, std::uint64_t length, bool writable)>;

struct ProcessBootstrapInput {
    // `mappedImage` is the already-relocated guest image backing with guest
    // virtual addresses represented as offsets into this buffer.
    std::vector<std::uint8_t> mappedImage;
    std::vector<std::uint8_t> defaultRuneLocaleBytes;
    std::vector<GuestMemoryRange> readOnlyRanges;
    std::uint64_t entry = 0;
    std::uint64_t mappingBackingEnd = 0;
    std::uint64_t stackBytes = 8U * 1024U * 1024U;
    std::uint64_t heapBytes = 64U * 1024U * 1024U;
    std::size_t initialAuthenticationEntries = 4096;
    std::string executablePath;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    std::uint64_t pageSize = 4096;
    std::uint64_t kernelPageSize = 4096;
    std::uint64_t guestTaskPort = guestMachTaskPort;
    std::uint64_t mainQueue = 0;
    std::uint64_t mainQueueState = 0;
    std::uint64_t mainQueueStateValue = 0;
    std::uint64_t mainHeader = 0;
    std::uint64_t activePlatform = 1;
    std::uint64_t objectiveCClassCount = 0;
    std::uint64_t randomCanary = 1;
    std::size_t runeLocaleByteCount = 2048;
    std::uint64_t randomPACGAKey0 = 0;
    std::uint64_t randomPACGAKey1 = 0;
};

struct ProcessBootstrapResult {
    std::vector<std::uint8_t> memory;
    std::vector<std::uint64_t> registers;
    std::vector<GuestMemoryRange> readOnlyRanges;
    GuestMemoryRange stack;
    GuestMemoryRange heap;
    std::uint64_t argcAddress = 0;
    std::uint64_t argvAddress = 0;
    std::uint64_t environmentAddress = 0;
    std::uint64_t programName = 0;
    std::uint64_t vectorBase = 0;
    std::uint64_t appleAddress = 0;
    std::uint64_t stdoutSlot = 0;
    std::uint64_t localeSlot = 0;
    std::uint64_t stderrSlot = 0;
    std::uint64_t optindSlot = 0;
    std::uint64_t environSlot = 0;
    std::uint64_t optargSlot = 0;
    std::uint64_t canarySlot = 0;
    std::uint64_t runeLocaleSlot = 0;
    std::uint64_t errnoSlot = 0;
    std::uint64_t stdinSlot = 0;
    std::uint64_t vmPageSizeSlot = 0;
    std::uint64_t vmPageShiftSlot = 0;
    std::uint64_t vmKernelPageSizeSlot = 0;
    std::uint64_t taskPortSlot = 0;
    std::uint64_t bootstrapPortSlot = 0;
    std::uint64_t guestTaskPort = guestMachTaskPort;
    std::uint64_t pageSize = 4096;
    std::uint64_t mainHeader = 0;
    std::uint64_t activePlatform = 1;
    std::uint64_t objectiveCClassCount = 0;
    std::string resolvedExecutablePath;
    std::uint64_t mainThreadSpecificData = 0;
    std::uint64_t scratchAddress = 0;
    std::map<std::string, std::uint64_t> environmentValueAddresses;
    std::map<std::string, std::uint64_t> modeledGlobalAddresses;
};

// Creates the Darwin-style argv/env stack and modeled libSystem globals. The
// initial guest register file and stack pointers match the process launcher.
ProcessBootstrapResult build_process_bootstrap(ProcessBootstrapInput input);

// Guest pointers are checked against a fixed backing buffer. All accesses use
// guest virtual offsets, including those for the stack and heap.
class GuestMemory {
public:
    explicit GuestMemory(std::vector<std::uint8_t> bytes,
                         std::vector<GuestMemoryRange> readOnlyRanges = {},
                         std::vector<GuestMemoryMapping> mappings = {});
    GuestMemory(std::uint8_t* bytes, std::size_t size,
                std::vector<GuestMemoryRange> readOnlyRanges = {},
                std::vector<GuestMemoryMapping> mappings = {});

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint8_t* buffer(std::uint64_t address, std::uint64_t length,
                                      bool writable = false);
    [[nodiscard]] const std::uint8_t* buffer(std::uint64_t address,
                                            std::uint64_t length) const;
    [[nodiscard]] std::uint64_t contiguous_readable_size(std::uint64_t address) const;
    [[nodiscard]] bool is_guest_readable(std::uint64_t address,
                                         std::uint64_t length) const noexcept;
    void add_guest_range(GuestMemoryRange range);
    [[nodiscard]] bool is_readonly(std::uint64_t address, std::uint64_t length) const;
    [[nodiscard]] bool is_protection_fault(std::uint64_t address) const;
    void rebind(std::uint8_t* bytes, std::size_t size);
    void add_mapping(GuestMemoryMapping mapping);
    void replace_mappings(const std::vector<std::uint64_t>& logicalBegins,
                          const std::vector<GuestMemoryMapping>& replacements);
    void remove_mapping(std::uint64_t logicalBegin);
    void set_mapping_permissions(std::uint64_t logicalBegin, bool writable);
    void set_mapping_permissions(std::uint64_t logicalBegin, bool readable, bool writable);
    void set_mapping_resolver(GuestMemoryMappingResolver resolver);
    void protect_readonly(GuestMemoryRange range);
    void unprotect_readonly(GuestMemoryRange range);
    [[nodiscard]] std::vector<std::uint8_t>& bytes() noexcept { return bytes_; }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    void validate_logical_interval(std::uint64_t address, std::uint64_t length) const;
    std::vector<std::uint8_t> bytes_;
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    bool external_ = false;
    std::vector<GuestMemoryRange> readOnlyRanges_;
    std::vector<GuestMemoryMapping> mappings_;
    std::vector<GuestMemoryRange> guestRanges_;
    GuestMemoryMappingResolver mappingResolver_;
};

struct HostServiceResult {
    bool exited = false;
    std::int32_t exitStatus = 0;
};

// Host resources with guest-process lifetime. Guest FILE* values are opaque
// synthetic addresses and never expose host pointers to guest code.
struct HostServiceState {
    struct MappedFile {
        std::uint64_t size = 0;
        std::int64_t allocation = 0;
        void* hostAddress = nullptr;
    };
    struct GuestLineBuffer {
        std::uint64_t address = 0;
        std::uint64_t capacity = 0;
    };
    struct GuestUnixDatagram {
        std::vector<std::uint8_t> payload;
        std::optional<std::string> sourcePath;
        std::uint32_t sourceLength = 16;
    };
    struct GuestUnixSocket {
        std::int32_t type = 0;
        std::optional<std::string> boundPath;
        std::uint32_t boundAddressLength = 0;
        std::optional<std::int32_t> connectedPeer;
        bool connectedPeerClosed = false;
        bool captureSink = false;
        std::deque<GuestUnixDatagram> receiveQueue;
    };
    std::set<std::int32_t> openDescriptors;
    std::set<std::int32_t> socketDescriptors;
    std::map<std::int32_t, GuestUnixSocket> guestUnixSockets;
    std::map<std::string, std::int32_t> guestUnixSocketPaths;
    std::deque<GuestUnixDatagram> guestSyslogCapture;
    std::set<std::int32_t> writableDescriptors;
    std::map<std::int32_t, std::int32_t> standardDescriptorFlags;
    std::map<std::int32_t, std::int32_t> standardStatusFlags;
    std::set<std::uint64_t> closedStreams;
    std::map<std::uint64_t, std::FILE*> files;
    std::map<std::uint64_t, MappedFile> mappedFiles;
    std::map<std::uint64_t, GuestLineBuffer> fgetlnBuffers;
    // Native send rights are process-owned and never exposed as guest names.
    std::map<std::uint32_t, std::uint32_t> guestBootstrapSendRights;
    // Consumes the host send right on both success and allocation failure.
    std::function<std::optional<std::uint32_t>(std::uint32_t hostPort)>
        allocateGuestBootstrapSendRight;
    // Cached FILE record addresses mapped from authoritative dyld metadata to
    // the launcher's canonical standard-stream slot addresses.
    std::map<std::uint64_t, std::uint64_t> standardFileAliases;
    std::optional<GuestHeap> heap;
    std::uint64_t heapEnd = 0;
    std::function<bool(GuestMemory&, std::size_t minimumSize)> growGuestMemory;
    std::function<std::uint32_t(std::uint64_t size, std::uint64_t alignment,
                                std::uint32_t currentProtection,
                                std::uint32_t maximumProtection,
                                std::uint64_t& guestAddress, bool jitMapping)> machVmAllocate;
    std::function<std::uint32_t(std::uint64_t& guestAddress, std::uint64_t size,
                                std::uint32_t currentProtection,
                                std::uint32_t maximumProtection)> machVmOverwrite;
    std::function<std::uint32_t(std::uint64_t guestAddress, std::uint64_t size)>
        machVmDeallocate;
    std::function<std::uint32_t(std::uint64_t guestAddress, std::uint64_t size,
                                bool setMaximum, std::uint32_t protection)> machVmProtect;
    std::uint64_t nextFile = 0x7000000000000000ULL;
    std::size_t optionIndex = 1;
    std::size_t optionOffset = 1;
    GuestSignals signals;
    std::uint32_t queuedProcessSignals = 0;
    std::uint32_t threadPendingSignals = 0;
    std::uint32_t threadSignalMask = 0;
    bool jitWriteProtected = true;
    bool guestMapJitEligible = false;
    std::function<bool(bool)> setJitWriteProtection;

    HostServiceState() = default;
    HostServiceState(const HostServiceState&) = delete;
    HostServiceState& operator=(const HostServiceState&) = delete;
    ~HostServiceState();
    void ingest_host_signals();
};

// Services invoked by ProcessTranslation's import trampoline. x35 is the
// service number; x0-x3 carry the imported function arguments. For returning
// services the runner resumes at x33, as emitted by process_decoder.
// Unsupported services throw instead of returning a fabricated success.
[[nodiscard]] HostServiceResult dispatch_guest_host_service(
    std::vector<std::uint64_t>& registers, GuestMemory& memory,
    const ProcessBootstrapResult& bootstrap, HostServiceState& state,
    std::uint64_t service);

}  // namespace arm64metal
