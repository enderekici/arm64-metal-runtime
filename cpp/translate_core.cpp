#include "translate_core.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace arm64metal::translate_core {
namespace {

std::uint32_t field(std::uint32_t bits, unsigned start, unsigned count) {
  return (bits >> start) & ((std::uint32_t{1} << count) - 1);
}

std::int64_t signed_field(std::uint32_t value, unsigned width) {
  const auto sign = std::uint32_t{1} << (width - 1);
  return static_cast<std::int64_t>(value ^ sign) - static_cast<std::int64_t>(sign);
}

std::string reg(std::uint32_t index, bool wide) {
  if (index == 31) return wide ? "0ul" : "0u";
  return wide ? "x[" + std::to_string(index) + "]"
              : "uint(x[" + std::to_string(index) + "])";
}

std::string pc_literal(std::int64_t pc, const std::string& suffix) {
  return std::to_string(pc) + suffix;
}

}  // namespace

OperationPacket serialize_operation(const Operation& operation) {
  return {static_cast<std::uint32_t>(operation.opcode),
          operation.destination, operation.source, operation.immediate,
          operation.shift, operation.width};
}

bool Instruction::ends_block_at(std::int64_t pc) const {
  const auto straight_line = next.size() == 1 && next.front() == pc + 4;
  const auto ends_with = [](const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  const auto terminal = !code.empty() &&
                        (ends_with(code.back(), "break;") || ends_with(code.back(), "return;"));
  return !straight_line || terminal;
}

std::optional<Operation> decode_operation(std::uint32_t bits) {
  if (bits == 0xd65f03c0) {
    return Operation{Opcode::Return, 0, 30, 0, 0, 64};
  }

  if ((bits & 0x7e000000) == 0x34000000) {
    const auto width = field(bits, 31, 1) == 1 ? 64u : 32u;
    const auto displacement = signed_field(field(bits, 5, 19), 19) * 4;
    return Operation{Opcode::BranchZero, 0, field(bits, 0, 5), field(bits, 24, 1),
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     width};
  }

  if ((bits & 0xff000010U) == 0x54000000U) {
    const auto displacement = signed_field(field(bits, 5, 19), 19) * 4;
    return Operation{Opcode::BranchCondition, 0, field(bits, 0, 4),
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     0, 64};
  }

  const auto selectEncoding = bits & 0x7fe00c00U;
  std::uint32_t selectVariant;
  if (selectEncoding == 0x1a800000U) {
    selectVariant = 0;
  } else if (selectEncoding == 0x1a800400U) {
    selectVariant = 1;
  } else if (selectEncoding == 0x5a800000U) {
    selectVariant = 2;
  } else if (selectEncoding == 0x5a800400U) {
    selectVariant = 3;
  } else {
    selectVariant = 0;
  }
  if (selectEncoding == 0x1a800000U || selectEncoding == 0x1a800400U ||
      selectEncoding == 0x5a800000U || selectEncoding == 0x5a800400U) {
    return Operation{Opcode::ConditionalSelect, field(bits, 0, 5), field(bits, 5, 5),
                     field(bits, 16, 5), (selectVariant << 4U) | field(bits, 12, 4),
                     field(bits, 31, 1) == 1 ? 64U : 32U};
  }

  if ((bits & 0xfc000000) == 0x14000000) {
    const auto displacement = signed_field(field(bits, 0, 26), 26) * 4;
    return Operation{Opcode::Branch, 0, 0,
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     0, 64};
  }

  const auto unsignedMemory = bits & 0xffc00000U;
  const auto memoryWidths = std::array<std::pair<std::uint32_t, std::uint32_t>, 8>{
      std::pair{0x39400000U, 8U}, std::pair{0x39000000U, 8U},
      std::pair{0x79400000U, 16U}, std::pair{0x79000000U, 16U},
      std::pair{0xb9400000U, 32U}, std::pair{0xb9000000U, 32U},
      std::pair{0xf9400000U, 64U}, std::pair{0xf9000000U, 64U}};
  for (const auto& [encoding, width] : memoryWidths) {
    if (unsignedMemory == encoding) {
      const auto opcode = (encoding & 0x00400000U) != 0
          ? Opcode::LoadUnsignedImmediate : Opcode::StoreUnsignedImmediate;
      return Operation{opcode, field(bits, 0, 5), field(bits, 5, 5),
                       field(bits, 10, 12),
                       width == 64 ? 3U : (width == 32 ? 2U : (width == 16 ? 1U : 0U)),
                       width};
    }
  }
  const auto scalarIndexedMemory = bits & 0xffe00c00U;
  const auto indexedMemoryWidths =
      std::array<std::pair<std::uint32_t, std::uint32_t>, 8>{
          std::pair{0x38400000U, 8U}, std::pair{0x38000000U, 8U},
          std::pair{0x78400000U, 16U}, std::pair{0x78000000U, 16U},
          std::pair{0xb8400000U, 32U}, std::pair{0xb8000000U, 32U},
          std::pair{0xf8400000U, 64U}, std::pair{0xf8000000U, 64U}};
  for (const auto& [encoding, width] : indexedMemoryWidths) {
    for (const auto mode : {0U, 1U, 3U}) {
      if (scalarIndexedMemory == (encoding | (mode << 10U))) {
        const auto target = field(bits, 0, 5);
        const auto base = field(bits, 5, 5);
        if (mode != 0 && base != 31 && target == base) {
          return std::nullopt;
        }
        const bool load = (encoding & 0x00400000U) != 0;
        return Operation{load ? Opcode::LoadUnscaledWriteback
                              : Opcode::StoreUnscaledWriteback,
                         target, base,
                         static_cast<std::uint32_t>(
                             static_cast<std::int32_t>(
                                 signed_field(field(bits, 12, 9), 9))),
                         mode, width};
      }
    }
  }
  const auto pairEncoding = (bits & 0xffc00000U) & 0x7fffffffU;
  const bool integerPair =
      (pairEncoding == 0x29000000U || pairEncoding == 0x29400000U ||
       pairEncoding == 0x29800000U || pairEncoding == 0x29c00000U ||
       pairEncoding == 0x28800000U || pairEncoding == 0x28c00000U) &&
      field(bits, 30, 1) == 0;
  if (integerPair) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto elementBytes = width / 8U;
    const auto first = field(bits, 0, 5);
    const auto second = field(bits, 10, 5);
    const auto base = field(bits, 5, 5);
    const auto mode = field(bits, 23, 2);
    const bool load = field(bits, 22, 1) == 1;
    if ((load && first == second) ||
        (base != 31 && mode != 2 && (base == first || base == second))) {
      return std::nullopt;
    }
    const auto displacement = signed_field(field(bits, 15, 7), 7) * elementBytes;
    return Operation{load ? Opcode::LoadPair : Opcode::StorePair, first, base,
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     (mode << 5U) | second, width};
  }
  if ((bits & 0x1f000000) == 0x11000000) {
    const auto destination = field(bits, 0, 5);
    const auto source = field(bits, 5, 5);
    const auto setFlags = field(bits, 29, 1) != 0;
    const auto width = field(bits, 31, 1) == 1 ? 64u : 32u;
    const auto shiftField = field(bits, 22, 1);
    if (field(bits, 23, 1) != 0) {
      return std::nullopt;
    }
    const auto subtract = field(bits, 30, 1) != 0;
    const auto opcode = setFlags
        ? (subtract ? Opcode::SubImmediateSetFlags : Opcode::AddImmediateSetFlags)
        : (subtract ? Opcode::SubImmediate : Opcode::AddImmediate);
    return Operation{opcode,
                     destination, source, field(bits, 10, 12),
                     shiftField * 12u, width};
  }
  if ((bits & 0x1f200000U) == 0x0b000000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto shiftType = field(bits, 22, 2);
    const auto amount = field(bits, 10, 6);
    if (shiftType == 3U || amount >= width) return std::nullopt;
    const auto subtract = field(bits, 30, 1);
    const auto setFlags = field(bits, 29, 1);
    const auto packed = (setFlags << 9U) | (subtract << 8U) |
                        (shiftType << 6U) | amount;
    return Operation{Opcode::AddSubShiftedRegister, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5), packed, width};
  }
  const auto moveWide = bits & 0x7f800000;
  Opcode opcode;
  if (moveWide == 0x52800000) {
    opcode = Opcode::MoveWideZero;
  } else if (moveWide == 0x12800000) {
    opcode = Opcode::MoveWideNot;
  } else if (moveWide == 0x72800000) {
    opcode = Opcode::MoveWideKeep;
  } else {
    return std::nullopt;
  }

  const auto width = field(bits, 31, 1) == 1 ? 64u : 32u;
  const auto shift = field(bits, 21, 2) * 16u;
  if (shift >= width) {
    throw std::runtime_error("Invalid MOVZ shift for destination width");
  }
  return Operation{opcode, field(bits, 0, 5), 0,
                   field(bits, 5, 16), shift, width};
}

Instruction make_operation_instruction(Operation operation,
                                       std::vector<std::int64_t> next,
                                       LoweringMode mode,
                                       std::uint64_t guest_code_limit) {
  std::vector<std::string> code;
  if (operation.opcode == Opcode::MoveWideZero ||
      operation.opcode == Opcode::MoveWideNot ||
      operation.opcode == Opcode::MoveWideKeep) {
    if (operation.destination != 31) {
      const auto destination = "x[" + std::to_string(operation.destination) + "]";
      code.push_back(destination + " = move_wide_apply(" +
                     std::to_string(static_cast<std::uint32_t>(operation.opcode)) +
                     "u, " + destination + ", " +
                     std::to_string(operation.immediate) + "u, " +
                     std::to_string(operation.shift) + "u, " +
                     std::to_string(operation.width) + "u);");
    }
  } else if (operation.opcode == Opcode::AddImmediate ||
             operation.opcode == Opcode::SubImmediate ||
             operation.opcode == Opcode::AddImmediateSetFlags ||
             operation.opcode == Opcode::SubImmediateSetFlags) {
    const bool setFlags = operation.opcode == Opcode::AddImmediateSetFlags ||
                          operation.opcode == Opcode::SubImmediateSetFlags;
    const auto subtract = operation.opcode == Opcode::SubImmediate ||
                          operation.opcode == Opcode::SubImmediateSetFlags;
    const auto lhs = "x[" + std::to_string(operation.source) + "]";
    code.push_back("{");
    code.push_back("const uint previous_flags = uint(z) | (uint(n) << 1u) | "
                   "(uint(v) << 2u) | (uint(c) << 3u);");
    code.push_back("const AddSubImmediateResult add_sub_result = "
                   "add_sub_immediate_apply(" + std::string(subtract ? "1u" : "0u") +
                   ", " + lhs + ", " + std::to_string(operation.immediate) +
                   "u, " + std::to_string(operation.shift) + "u, " +
                   std::to_string(operation.width) + "u, " +
                   std::string(setFlags ? "1u" : "0u") + ", previous_flags);");
    if (!setFlags || operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = add_sub_result.value;");
    }
    code.push_back("z = (add_sub_result.flags & 1u) != 0u;");
    code.push_back("n = (add_sub_result.flags & 2u) != 0u;");
    code.push_back("v = (add_sub_result.flags & 4u) != 0u;");
    code.push_back("c = (add_sub_result.flags & 8u) != 0u;");
    if (mode == LoweringMode::Process) {
      code.push_back("x[32] = add_sub_result.flags;");
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::AddSubShiftedRegister) {
    const bool subtract = (operation.shift & 0x100U) != 0;
    const bool setFlags = (operation.shift & 0x200U) != 0;
    const auto shiftType = (operation.shift >> 6U) & 3U;
    const auto amount = operation.shift & 63U;
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || shiftType > 2 || amount >= operation.width ||
        (operation.width != 32 && operation.width != 64) ||
        (operation.shift & ~0x3ffU) != 0) {
      throw std::invalid_argument("Typed ADD/SUB shifted-register operands are invalid");
    }
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("{");
    code.push_back("const AddSubImmediateResult add_sub_result = "
                   "add_sub_shifted_apply(" + std::string(subtract ? "1u" : "0u") +
                   ", " + reg(operation.source, operation.width == 64) + ", " +
                   reg(operation.immediate, operation.width == 64) + ", " +
                   std::to_string(shiftType) + "u, " + std::to_string(amount) +
                   "u, " + std::to_string(operation.width) + "u, " +
                   std::string(setFlags ? "1u" : "0u") + ", " + flags + ");");
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = add_sub_result.value;");
    }
    code.push_back("z = (add_sub_result.flags & 1u) != 0u;");
    code.push_back("n = (add_sub_result.flags & 2u) != 0u;");
    code.push_back("v = (add_sub_result.flags & 4u) != 0u;");
    code.push_back("c = (add_sub_result.flags & 8u) != 0u;");
    if (mode == LoweringMode::Process) {
      code.push_back("x[32] = add_sub_result.flags;");
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::Return) {
    if (mode == LoweringMode::Function) {
      code = {"returns[id] = uint(x[0]); status[id] = 0; return;"};
    } else {
      if (guest_code_limit == 0) {
        throw std::invalid_argument("Process return lowering needs a code limit");
      }
      code = {
          "if (x[30] == ~0ul) { x[34] = 0; return; }",
          "if (x[30] >= " + std::to_string(guest_code_limit) +
              "ul) { x[34] = 3; return; }",
          "x[33] = x[30];",
          "x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);",
          "x[34] = 7; return;"};
    }
  } else if (operation.opcode == Opcode::Branch) {
    if (next.size() != 1) {
      throw std::invalid_argument("Typed B needs one resolved guest target");
    }
    code.push_back("pc = " + std::to_string(next.front()) + "ul; break;");
  } else if (operation.opcode == Opcode::BranchZero) {
    if (operation.source > 31 || operation.immediate > 1 ||
        (operation.width != 32 && operation.width != 64) || next.size() != 2) {
      throw std::invalid_argument("Typed CBZ/CBNZ needs two resolved guest edges");
    }
    const auto condition = reg(operation.source, operation.width == 64) +
                           (operation.immediate == 0 ? " == 0" : " != 0");
    code.push_back("pc = (" + condition + ") ? " +
                   pc_literal(next[0], "ul") + " : " +
                   pc_literal(next[1], "ul") + "; break;");
  } else if (operation.opcode == Opcode::BranchCondition) {
    if (operation.source > 15 || next.size() != 2 || operation.width != 64) {
      throw std::invalid_argument("Typed B.cond needs a condition and two resolved guest edges");
    }
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("pc = arm64_condition_holds(" +
                   std::to_string(operation.source) + "u, " + flags + ") ? " +
                   pc_literal(next[0], "ul") + " : " +
                   pc_literal(next[1], "ul") + "; break;");
  } else if (operation.opcode == Opcode::ConditionalSelect) {
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || operation.shift > 63 ||
        (operation.width != 32 && operation.width != 64)) {
      throw std::invalid_argument("Typed conditional select operands are invalid");
    }
    if (operation.destination != 31) {
      const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                         "(uint(c) << 3u))";
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = conditional_select_apply(" +
                     std::to_string(operation.shift >> 4U) + "u, " +
                     reg(operation.source, operation.width == 64) + ", " +
                     reg(operation.immediate, operation.width == 64) + ", " +
                     std::to_string(operation.shift & 15U) + "u, " + flags + ", " +
                     std::to_string(operation.width) + "u);");
    }
  } else {
    throw std::invalid_argument("Unknown typed ARM64 operation");
  }
  return Instruction{std::move(code), std::move(next), operation};
}

Instruction decode(std::uint32_t bits, std::int64_t pc,
                   const std::string& address_suffix) {
  const auto rd = field(bits, 0, 5);
  const auto rn = field(bits, 5, 5);
  const auto rm = field(bits, 16, 5);
  const bool wide = field(bits, 31, 1) == 1;
  const std::string cast = wide ? "ulong" : "uint";
  const auto following = pc + 4;

  const auto assign = [&](const std::string& value) {
    return rd == 31 ? std::string{} :
                      "x[" + std::to_string(rd) + "] = " + cast + "(" + value + ");";
  };
  const auto advance = [following](std::vector<std::string> code) {
    return Instruction{std::move(code), {following}};
  };

  if (const auto operation = decode_operation(bits);
      operation && operation->opcode != Opcode::LoadUnsignedImmediate &&
      operation->opcode != Opcode::StoreUnsignedImmediate &&
      operation->opcode != Opcode::LoadPair &&
      operation->opcode != Opcode::StorePair &&
      operation->opcode != Opcode::LoadUnscaledWriteback &&
      operation->opcode != Opcode::StoreUnscaledWriteback) {
    std::vector<std::int64_t> successors;
    if (operation->opcode == Opcode::Branch) {
      successors.push_back(pc + static_cast<std::int32_t>(operation->immediate));
    } else if (operation->opcode == Opcode::BranchZero) {
      successors.push_back(pc + static_cast<std::int32_t>(operation->shift));
      successors.push_back(following);
    } else if (operation->opcode == Opcode::BranchCondition) {
      successors.push_back(pc + static_cast<std::int32_t>(operation->immediate));
      successors.push_back(following);
    } else if (operation->opcode != Opcode::Return) {
      successors.push_back(following);
    }
    return make_operation_instruction(*operation, std::move(successors),
                                      LoweringMode::Function);
  }
  const auto branch = [&](const std::string& condition, std::int64_t target) {
    return Instruction{{"pc = " + condition + " ? " + pc_literal(target, address_suffix) +
                        " : " + pc_literal(following, address_suffix) + "; break;"},
                       {target, following}};
  };

  if ((bits & 0xff00001e) == 0x54000000) {
    return branch(field(bits, 0, 1) == 0 ? "z" : "!z",
                  pc + signed_field(field(bits, 5, 19), 19) * 4);
  }
  if ((bits & 0x7f200000) == 0x0b000000 ||
      (bits & 0x7f200000) == 0x4a000000) {
    const auto shift = field(bits, 10, 6);
    if (field(bits, 22, 2) == 0 && shift < (wide ? 64u : 32u)) {
      const auto op = (bits & 0x7f000000) == 0x0b000000 ? "+" : "^";
      return advance({assign(reg(rn, wide) + " " + op + " (" + reg(rm, wide) +
                            " << " + std::to_string(shift) + ")")});
    }
  }
  if ((bits & 0x7f800000) == 0x71000000 && rn != 31) {
    const auto immediate = field(bits, 10, 12) << (field(bits, 22, 1) * 12);
    return advance({"z = (" + reg(rn, wide) + " == " + cast + "(" +
                        std::to_string(immediate) + "));",
                    assign(reg(rn, wide) + " - " + cast + "(" +
                           std::to_string(immediate) + ")")});
  }
  if ((bits & 0x7fe0ffe0) == 0x2a0003e0) return advance({assign(reg(rm, wide))});
  if ((bits & 0xffc0fc00) == 0x53007c00 && field(bits, 16, 6) < 32) {
    return advance({assign(reg(rn, false) + " >> " + std::to_string(field(bits, 16, 6)))});
  }
  // ANDS to WZR with the single-bit immediate 1.
  if ((bits & 0xfffffc1f) == 0x7200001f) {
    return advance({"z = ((" + reg(rn, false) + " & 1u) == 0);"});
  }
  if ((bits & 0x7fe0ec00) == 0x1a800400) {
    const auto condition = field(bits, 12, 1) == 0 ? "z" : "!z";
    return advance({assign(std::string(condition) + " ? " + reg(rn, wide) + " : " +
                           reg(rm, wide) + " + " + cast + "(1)")});
  }

  const auto word_memory = bits & 0xffc00000;
  const auto post_memory = bits & 0xffe00c00;
  const bool supported_memory = rn != 31 &&
      (word_memory == 0xb9400000 || word_memory == 0xb9000000 ||
       ((post_memory == 0xb8400400 || post_memory == 0xb8000400) && rn != rd));
  if (supported_memory) {
    const bool post = post_memory == 0xb8400400 || post_memory == 0xb8000400;
    const bool load = word_memory == 0xb9400000 || post_memory == 0xb8400400;
    const auto displacement = post ? 0u : field(bits, 10, 12) * 4;
    const auto base = reg(rn, true);
    const auto address = "(" + base + " + " + std::to_string(displacement) + "ul)";
    const auto index = "(" + address + " >> 2)";
    const auto bounds = "if ((" + address + " & 3ul) || " + index +
                        " >= count) { status[id] = 2; return; }";
    const auto access = load ? assign("words[" + index + "]")
                             : "words[" + index + "] = " + reg(rd, false) + ";";
    if (post) {
      return advance({bounds, access,
                      base + " += ulong(" +
                          std::to_string(signed_field(field(bits, 12, 9), 9)) + ");"});
    }
    return advance({bounds, access});
  }

  std::ostringstream message;
  message << "Unsupported ARM64 instruction 0x" << std::hex << std::nouppercase
          << std::setw(8) << std::setfill('0') << bits << " at __text+0x"
          << pc;
  throw std::runtime_error(message.str());
}

std::set<std::int64_t> block_entries(
    const std::map<std::int64_t, Instruction>& instructions,
    std::int64_t entry) {
  std::set<std::int64_t> leaders{entry};
  for (const auto& [pc, instruction] : instructions) {
    if (!instruction.ends_block_at(pc)) continue;
    leaders.insert(instruction.next.begin(), instruction.next.end());
  }
  for (auto it = leaders.begin(); it != leaders.end();) {
    if (instructions.find(*it) == instructions.end()) it = leaders.erase(it);
    else ++it;
  }
  return leaders;
}

std::string render_blocks(
    const std::map<std::int64_t, Instruction>& instructions,
    std::int64_t entry, const std::string& address_suffix) {
  const auto leaders = block_entries(instructions, entry);
  std::vector<std::string> blocks;
  for (const auto block_entry : leaders) {
    std::vector<std::string> lines{"case " + pc_literal(block_entry, address_suffix) + ":"};
    auto pc = block_entry;
    while (true) {
      const auto found = instructions.find(pc);
      if (found == instructions.end()) break;
      const auto& instruction = found->second;
      for (const auto& statement : instruction.code) {
        if (!statement.empty()) lines.push_back("    " + statement);
      }
      if (instruction.ends_block_at(pc)) break;
      pc += 4;
      if (leaders.find(pc) != leaders.end() || instructions.find(pc) == instructions.end()) {
        lines.push_back("    pc = " + pc_literal(pc, address_suffix) + "; break;");
        break;
      }
    }
    std::ostringstream block;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i != 0) block << '\n';
      block << lines[i];
    }
    blocks.push_back(block.str());
  }
  std::ostringstream rendered;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (i != 0) rendered << '\n';
    rendered << blocks[i];
  }
  return rendered.str();
}

}  // namespace arm64metal::translate_core
