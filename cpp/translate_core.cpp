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
  const auto branch = [&](const std::string& condition, std::int64_t target) {
    return Instruction{{"pc = " + condition + " ? " + pc_literal(target, address_suffix) +
                        " : " + pc_literal(following, address_suffix) + "; break;"},
                       {target, following}};
  };

  if (bits == 0xd65f03c0) {
    return {{"returns[id] = uint(x[0]); status[id] = 0; return;"}, {}};
  }
  if ((bits & 0x7e000000) == 0x34000000) {
    const auto op = field(bits, 24, 1) == 0 ? "==" : "!=";
    return branch(reg(rd, wide) + " " + op + " 0",
                  pc + signed_field(field(bits, 5, 19), 19) * 4);
  }
  if ((bits & 0xff00001e) == 0x54000000) {
    return branch(field(bits, 0, 1) == 0 ? "z" : "!z",
                  pc + signed_field(field(bits, 5, 19), 19) * 4);
  }
  if ((bits & 0xfc000000) == 0x14000000) {
    const auto target = pc + signed_field(field(bits, 0, 26), 26) * 4;
    return {{"pc = " + pc_literal(target, address_suffix) + "; break;"}, {target}};
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
