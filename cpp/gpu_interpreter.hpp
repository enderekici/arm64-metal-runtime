#pragma once

#include "metal_backend.hpp"
#include "translate_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace arm64metal::gpu_interpreter {

// Fixed-width host/device packet. The first 24 bytes are the canonical
// translate_core operation serialization. The guest PC is appended as data.
struct InstructionPacket {
    translate_core::OperationPacket operation{};
    std::uint64_t pc = 0;
};

static_assert(sizeof(InstructionPacket) == 32);
static_assert(offsetof(InstructionPacket, operation) == 0);
static_assert(offsetof(InstructionPacket, pc) == 24);

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

// Compiles one fixed MSL interpreter kernel. Guest programs never contribute
// shader source. The same instance can dispatch any sequence in this slice.
class Interpreter final {
public:
    explicit Interpreter(const Backend& backend);

    [[nodiscard]] Result run(const Backend& backend,
                             const std::vector<InstructionPacket>& packets,
                             ProcessState initialState) const;
    [[nodiscard]] Result dispatch(const Backend& backend,
                                  const std::vector<InstructionPacket>& packets,
                                  SharedBuffer& processState) const;

private:
    ComputeKernel kernel_;
};

}  // namespace arm64metal::gpu_interpreter
