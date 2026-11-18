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
  AddSubCarryRegister = 26,
  LogicalShiftedRegister = 23,
  LogicalImmediate = 24,
  TestBitBranch = 25,
  Adr = 27,
  BranchLink = 28,
  LogicalVariableShiftRegister = 29,
  BranchRegister = 30,
  IntegerMultiplyAccumulate = 31,
  ScalarFloatingPoint = 32,
  Bitfield = 33,
  IntegerBitOperation = 34,
  IntegerDivide = 35,
  LoadLiteral = 38,
  LoadUnsignedMinMax = 37,
  VectorCompareEqualZero = 39,
  VectorReduceExtreme = 40,
  VectorReverse = 41,
  VectorReduceSum = 42,
  LoadVectorMemory = 43,
  StoreVectorMemory = 44,
  SystemFlags = 45,  // immediate selects CFINV (0), XAFLAG (1) or AXFLAG (2)
  FlagManipulation = 46,
  VectorDot = 47,
  AuthenticatedReturn = 36,
};

// Bit 3 distinguishes RCpc2 ordered accesses in indexed and signed scalar packets.
inline constexpr std::uint32_t rcpc2Ordered = 8U;

enum class VectorMemoryMode : std::uint32_t {
  ScaledImmediate = 0,
  Unscaled = 1,
  PostIndex = 2,
  PreIndex = 3,
  RegisterOffset = 4,
  Literal = 5,
};

// AuthenticatedReturn uses `destination` as its sub-operation: 0 pushes the
// LR/SP shadow entry (PACIBSP), 1 validates and pops it (AUTIBSP), and 2
// validates it then returns (RETAB). Other operands are zero.
enum class AuthenticatedReturnStep : std::uint32_t {
  SignLinkRegister = 0,
  AuthenticateLinkRegister = 1,
  Return = 2,
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
// Scalar literal loads use destination=Rt, source=0, immediate=signed byte
// displacement from the instruction PC, shift=0 (LDR W), 1 (LDR X), or
// 2 (LDRSW X), and width=32 for W or 64 for X results.
// Single-vector memory uses destination=Vt, source=Rn (zero for literal),
// immediate=signed byte displacement (or zero for register offset), width=the
// transferred bits (8/16/32/64/128), and shift=VectorMemoryMode. Register
// offset additionally packs Rm at bits 3..7, option at 8..10, and scale at 11.
// Literal mode uses a signed displacement from the instruction PC.
// BranchCondition uses source for the condition and immediate for byte offset.
// TestBitBranch uses destination for bit index, source for Rt, immediate for
// TBNZ sense, shift for signed byte displacement, and width for W/X form.
// Its full-width target and fallthrough PCs are carried in Instruction::next.
// Conditional select uses destination=Rd, source=Rn, immediate=Rm,
// shift=(variant << 4) | condition, and width=32 or 64.
// Direct BL uses immediate=signed byte displacement, width=64, and resolved
// next edges {target, return PC}; LR receives the return PC.
// BR/BLR use source=Rn, immediate=1 for BLR and 0 for BR, width=64, and no
// static successors. Register 31 reads XZR; BLR snapshots its target before
// writing LR=PC+4.
// ADD/SUB shifted register uses destination=Rd, source=Rn, immediate=Rm,
// shift=(setFlags << 9) | (subtract << 8) | (shiftType << 6) | amount,
// and width=32 or 64. Shift types 0/1/2 are LSL/LSR/ASR; register 31 is ZR.
// ADD/SUB extended register uses destination=Rd, source=Rn, immediate=Rm,
// shift=(setFlags << 7) | (subtract << 6) | (option << 3) | amount,
// and width=32 or 64. Rn/Rd register 31 is SP; Rm register 31 is ZR.
// Logical shifted register uses destination=Rd, source=Rn, immediate=Rm,
// shift=(amount << 5) | (shiftType << 3) | (invert << 2) | opc,
// and width=32 or 64. Rn/Rm/Rd register 31 is ZR. Opc selects AND, ORR,
// EOR, or ANDS, with invert selecting BIC, ORN, EON, or BICS. Shift types
// 0/1/2/3 are LSL/LSR/ASR/ROR. ANDS/BICS update NZCV.
// Logical variable shifts use destination=Rd, source=Rn, immediate=Rm,
// shift=shiftType (0/1/2/3 for LSLV/LSRV/ASRV/RORV), and width=32 or 64.
// Rn/Rm/Rd register 31 is ZR. The shift amount uses the low 5 or 6 bits of Rm.
// Logical immediate uses destination=Rd, source=(opc << 5) | Rn,
// immediate=mask low word, shift=mask high word, width=32 or 64. Register 31
// is ZR. Opc selects AND, ORR, EOR, or ANDS; ANDS updates NZCV.
// ADR/ADRP uses destination=Rd, source=0 for ADR or 1 for ADRP,
// immediate=the unsigned encoded immhi:immlo 21-bit value, and width=64.
// The instruction PC comes from the packet and is passed to adr_apply.
// ADC/SBC uses destination=Rd, source=Rn, immediate=Rm, shift=(setFlags << 1)
// | subtract, and width=32 or 64. Register 31 is ZR for all operands.
// Integer multiply-accumulate uses destination=Rd, source=Rn, immediate=Rm,
// shift=(subtract << 5) | Ra, and width=32 or 64. Register 31 is ZR for all
// operands; Ra=31 gives MUL/MNEG aliases.
// UBFM/BFM/SBFM use destination=Rd, source=Rn, immediate=immr,
// shift=(variant << 6) | imms (0=unsigned, 1=merge, 2=signed), and width
// 32 or 64. Register 31 is ZR; BFM reads the prior destination as its merge
// value.
// Scalar FP uses destination=Fd, source=Rn/Fn, immediate=FMOV imm8,
// shift=(kind << 2) | (integer-source-is-X << 1) | signed-source, and
// width=32 or 64 for the FP format. Kinds are integer conversion, FMOV
// immediate, and FRINTP. Scalar FP registers use their low 64-bit lane; the
// upper lane is cleared. Integer source register 31 is ZR.
// RBIT/REV/CLZ/CLS use destination=Rd, source=Rn, immediate=(variant
// 0=RBIT, 1=REV, 2=CLZ, 3=CLS), shift=0, and width=32 or 64. Register 31
// reads as zero and discards writes.
// UDIV/SDIV use destination=Rd, source=Rn, immediate=Rm, shift=0 for UDIV
// or 1 for SDIV, and width=32 or 64. Register 31 reads as zero and discards
// writes.
// LDUMIN/LDUMAX/LDSMIN/LDSMAX use destination=Rt, source=Rn, immediate=Rs,
// shift=0/1 for unsigned min/max or 2/3 for signed min/max, and width=8, 16,
// 32 or 64 for the memory lane width. The old-value result is zero-extended.
// Register 31 reads as zero for
// the source and discards the old-value result. Only unordered variants are
// typed in this slice.
// Scalar FP uses destination=Fd, source=Rn/Fn, immediate=FMOV imm8 or FCMP Rm,
// shift=(kind << 2) | detail, and width=32 or 64 for the FP format. Kinds are
// integer conversion, FMOV immediate, FRINTP, FNEG, and FCMP/FCMPE. Kind 0
// details encode integer source width and signedness; kind 4 detail bit 0 marks
// FCMPE and bit 1 compares against +0. Scalar FP registers use their low 64-bit
// lane; the upper lane is cleared. Integer source register 31 is ZR.
// VectorCompareEqualZero uses destination=Vd, source=Vn, immediate=element
// width in bits, shift=0, and width=64 or 128 vector bits.
// VectorReduceExtreme uses destination=Vd, source=Vn, immediate=element width,
// shift=(signed << 1) | minimum, and width=64 or 128 vector bits.
// VectorReduceSum uses destination=Vd, source=Vn, immediate=element width,
// shift=0, and width=64 or 128 vector bits.
// VectorDot uses destination=Vd, source=Vn, immediate=Vm, shift=(signed |
// (indexed << 1) | (lane << 2)), and width=64 or 128 vector bits.
// VectorReverse uses destination=Vd, source=Vn, immediate=element width,
// shift=reverse group width (16, 32, or 64), and width=64 or 128 vector bits.
struct Operation {
  Opcode opcode;
  std::uint32_t destination;
  std::uint32_t source;
  std::uint32_t immediate;
  std::uint32_t shift;
  std::uint32_t width;
};

// FlagManipulation: width 64 selects RMIF (Rn, NZCV mask, rotation);
// widths 8 and 16 select SETF8 and SETF16 (Wn only). Register 31 reads as zero.
inline constexpr bool valid_flag_manipulation(std::uint32_t source, std::uint32_t mask,
                                              std::uint32_t shift, std::uint32_t width) {
  return source <= 31U && (width == 64U ? mask <= 15U && shift <= 63U :
      (width == 8U || width == 16U) && mask == 0U && shift == 0U);
}

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
// Process-only SIMD memory decoder; its forms mirror the process translator's
// single-vector literal, immediate, unscaled/writeback and register-offset forms.
std::optional<Operation> decode_vector_memory_operation(std::uint32_t bits);
// Process-only key-B authenticated return operations. Kept separate from
// decode_operation so standalone function translation retains its current
// unsupported-instruction policy.
std::optional<Operation> decode_authenticated_return_operation(std::uint32_t bits);
std::optional<std::uint64_t> decode_logical_immediate_mask(
    std::uint32_t n, std::uint32_t immr, std::uint32_t imms,
    std::uint32_t width);
Instruction make_operation_instruction(Operation operation,
                                       std::vector<std::int64_t> next,
                                       LoweringMode mode,
                                       std::uint64_t guest_code_limit = 0,
                                       std::uint64_t guest_pc = 0);

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
