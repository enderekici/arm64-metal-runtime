#ifndef ARM64METAL_TRANSLATE_CORE_HPP
#define ARM64METAL_TRANSLATE_CORE_HPP

#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace arm64metal::translate_core {

enum class Opcode : std::uint32_t {
  MoveWideZero = 1,
  Return = 2,
  MoveWideNot = 3,
  MoveWideKeep = 4,
  Branch = 5,
  LoadUnsignedImmediate = 6,
  StoreUnsignedImmediate = 7,
  BranchZero = 8,
  AddImmediate = 9,
  SubImmediate = 10,
  AddImmediateSetFlags = 11,
  SubImmediateSetFlags = 12,
  LoadPair = 13,
  StorePair = 14,
  BranchCondition = 15,
  ConditionalSelect = 16,
  LoadUnscaledWriteback = 17,
  StoreUnscaledWriteback = 18,
  AddSubShiftedRegister = 19,
  LoadSignedScalar = 20,
  AddSubExtendedRegister = 22,
};

// Fixed-width semantic operands suitable for explicit interpreter packets.
// Fields unused by an opcode are zero. BranchZero uses source for Rt,
// immediate for CBNZ sense, and shift for its signed byte displacement; its
// full-width target and fallthrough PCs are carried in Instruction::next.
// Add/Sub immediate uses destination/source/immediate/shift/width fields.
// The set-flags variants use Rd=31 for the CMP/CMN aliases and Rn=31 as SP.
// Scalar unsigned memory uses width 8/16 for LDRB/H and STRB/H (loads zero
// extend Wt), or width 32/64 for LDR/STR; shift scales the unsigned offset.
// Pair memory uses destination=Rt, source=Rn, immediate=signed byte offset,
// shift=(addressing mode << 5) | Rt2 (1=post, 2=offset, 3=pre), and width=32 or 64.
// Scalar unscaled/writeback memory uses destination=Rt, source=Rn,
// immediate=signed byte offset, shift=addressing mode (0=unscaled, 1=post,
// 3=pre), and width=8/16/32/64. Narrow loads zero extend Wt. Opcodes 17/18
// select load/store.
// Signed scalar loads use destination=Rt, source=Rn, immediate=byte offset,
// shift=(target-is-X << 2) | mode (0=scaled unsigned, 1=unscaled, 2=post,
// 3=pre), and width=8/16/32 access bits.
// BranchCondition uses source for the condition and immediate for byte offset.
// Conditional select uses destination=Rd, source=Rn, immediate=Rm,
// shift=(variant << 4) | condition, and width=32 or 64.
// ADD/SUB shifted register uses destination=Rd, source=Rn, immediate=Rm,
// shift=(setFlags << 9) | (subtract << 8) | (shiftType << 6) | amount,
// and width=32 or 64. Shift types 0/1/2 are LSL/LSR/ASR; register 31 is ZR.
// ADD/SUB extended register uses destination=Rd, source=Rn, immediate=Rm,
// shift=(setFlags << 7) | (subtract << 6) | (option << 3) | amount,
// and width=32 or 64. Rn/Rd register 31 is SP; Rm register 31 is ZR.
struct Operation {
  Opcode opcode;
  std::uint32_t destination;
  std::uint32_t source;
  std::uint32_t immediate;
  std::uint32_t shift;
  std::uint32_t width;
};

using OperationPacket = std::array<std::uint32_t, 6>;

OperationPacket serialize_operation(const Operation& operation);

enum class LoweringMode {
  Process,
  Function,
};

// One lowered ARM64 instruction. `code` contains generated Metal statements;
// `next` lists guest PCs which may execute next. `operation` retains typed
// semantics for the GPU interpreter while `code` remains the translator form.
struct Instruction {
  std::vector<std::string> code;
  std::vector<std::int64_t> next;
  std::optional<Operation> operation;

  Instruction() = default;
  Instruction(std::vector<std::string> statements,
              std::vector<std::int64_t> successors,
              std::optional<Operation> typed_operation = std::nullopt)
      : code(std::move(statements)),
        next(std::move(successors)),
        operation(std::move(typed_operation)) {}

  // An instruction ends its basic block if control flow is not straight-line.
  bool ends_block_at(std::int64_t pc) const;
};

std::optional<Operation> decode_operation(std::uint32_t bits);
Instruction make_operation_instruction(Operation operation,
                                       std::vector<std::int64_t> next,
                                       LoweringMode mode,
                                       std::uint64_t guest_code_limit = 0);

// Decode one supported integer ARM64 instruction to the runtime's Metal form.
// Throws std::runtime_error for unsupported encodings. `address_suffix` is
// appended to generated PC literals (normally "u" or "ul").
Instruction decode(std::uint32_t bits, std::int64_t pc,
                   const std::string& address_suffix = "u");

// Return reachable instruction addresses that begin emitted switch cases.
std::set<std::int64_t> block_entries(
    const std::map<std::int64_t, Instruction>& instructions,
    std::int64_t entry);

// Render basic blocks as Metal switch cases, matching Translate.swift's
// ordering, indentation, fallthrough handling, and PC literal suffix.
std::string render_blocks(
    const std::map<std::int64_t, Instruction>& instructions,
    std::int64_t entry, const std::string& address_suffix = "u");

}  // namespace arm64metal::translate_core

#endif  // ARM64METAL_TRANSLATE_CORE_HPP
