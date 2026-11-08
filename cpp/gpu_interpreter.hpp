#pragma once

#include "metal_backend.hpp"
#include "translate_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace arm64metal::gpu_interpreter {

inline constexpr std::uint32_t packetBlockStart = 1U;
inline constexpr std::uint32_t packetBlockEnd = 2U;

// Fixed-width host/device packet. Control-flow edges stay 64-bit guest PCs,
// separate from the canonical 24-byte operation serialization.
struct InstructionPacket {
    translate_core::OperationPacket operation{};
    std::uint64_t pc = 0;
    std::uint64_t successor0 = 0;
    std::uint64_t successor1 = 0;
    std::uint32_t successorCount = 0;
    std::uint32_t flags = 0;
};

static_assert(sizeof(InstructionPacket) == 56);
static_assert(offsetof(InstructionPacket, operation) == 0);
static_assert(offsetof(InstructionPacket, pc) == 24);
static_assert(offsetof(InstructionPacket, successor0) == 32);
static_assert(offsetof(InstructionPacket, successor1) == 40);
static_assert(offsetof(InstructionPacket, successorCount) == 48);
static_assert(offsetof(InstructionPacket, flags) == 52);

inline constexpr std::size_t processRegisterCount = 37;
inline constexpr std::size_t programCounterIndex = 33;
inline constexpr std::size_t processStatusIndex = 34;
inline constexpr std::size_t blockBudgetIndex = 36;
inline constexpr std::size_t returnRegisterIndex = 30;
inline constexpr std::size_t resultRegisterIndex = 0;
inline constexpr std::uint64_t noReturnAddress = UINT64_MAX;

using ProcessState = std::array<std::uint64_t, processRegisterCount>;

struct Result {
    ProcessState state{};
    DispatchTiming timing{};
};

struct MemoryAccessContext {
    SharedBuffer* guestMemoryBuffer = nullptr;
    SharedBuffer* guestSizeBuffer = nullptr;
    SharedBuffer* writableBuffer = nullptr;
    SharedBuffer* residentBuffer = nullptr;
    SharedBuffer* memoryConfigBuffer = nullptr;
};

// Loads one fixed AOT Metal interpreter library. Guest programs never
// contribute shader source. The same instance can dispatch any sequence.
class Interpreter final {
public:
    Interpreter(const Backend& backend, std::string_view libraryPath);
    [[nodiscard]] static Interpreter fromSourceFile(const Backend& backend,
                                                   std::string_view sourcePath);

    [[nodiscard]] Result run(const Backend& backend,
                             const std::vector<InstructionPacket>& packets,
                             ProcessState initialState) const;
    [[nodiscard]] Result dispatch(const Backend& backend,
                                  const std::vector<InstructionPacket>& packets,
                                  SharedBuffer& processState,
                                  const MemoryAccessContext* memoryAccess = nullptr) const;

private:
    explicit Interpreter(ComputeKernel kernel);
    ComputeKernel kernel_;
};

}  // namespace arm64metal::gpu_interpreter
