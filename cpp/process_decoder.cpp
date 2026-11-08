#include "process_decoder.hpp"
#include "host_service_registry.hpp"
#include "guest_strings.hpp"
#include "guest_vector_crypto.hpp"
#include "process_host.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <type_traits>
namespace arm64metal {
namespace {
using Strings = std::vector<std::string>;
using PCs = std::vector<int64_t>;
using GuestError = std::runtime_error;
template <class T> struct Opt {
  T value{};
  bool present = false;
  Opt() = default;
  Opt(std::nullptr_t) {}
  Opt(T v) : value(v), present(true) {}
  explicit operator bool() const { return present; }
  operator T() const { return value; }
  bool operator==(std::nullptr_t) const { return !present; }
  bool operator!=(std::nullptr_t) const { return present; }
};
template <class T> struct Map : std::map<uint32_t, T> {
  using std::map<uint32_t, T>::map;
  Opt<T> operator[](uint32_t k) const {
    auto p = this->find(k);
    return p == this->end() ? Opt<T>{} : Opt<T>{p->second};
  }
};
inline std::string str(const std::string &s) { return s; }
inline std::string str(bool b) { return b ? "true" : "false"; }
template <class T> std::string str(T n) { return std::to_string(n); }
template <class T> std::string str(Opt<T> n) { return str(n.value); }
template <class T> Opt<T> first(Opt<T> v) { return v; }
template <class T, class... A> Opt<T> first(Opt<T> v, A... a) {
  return v.present ? v : first(a...);
}
inline bool contains(std::initializer_list<uint32_t> vals, uint32_t x) {
  return std::find(vals.begin(), vals.end(), x) != vals.end();
}
inline int ctz(uint64_t v) { return v ? __builtin_ctzll(v) : 64; }
inline Strings operator+(Strings a, const Strings &b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
inline Instruction make_instruction(Strings code, PCs next) {
  return {std::move(code), std::move(next)};
}
inline void replace_all(std::string &s, const std::string &a,
                        const std::string &b) {
  size_t p = 0;
  while ((p = s.find(a, p)) != std::string::npos) {
    s.replace(p, a.size(), b);
    p += b.size();
  }
}
std::string mappedOffset(const std::string &address, const std::string &count,
                         bool write) {
  return "guest_offset(" + address + ", " + count + ", size, writable, " +
         (write ? "true" : "false") + ", x, resident, config)";
}
} // namespace

Instruction hostCall(const std::string &name,
                     std::optional<int64_t> returnAddress, bool cached) {
  if (auto lowered = guest_string_call(
          name,
          returnAddress ? std::optional<uint64_t>(*returnAddress)
                        : std::nullopt,
          cached, [](std::string_view a, std::string_view n, bool w) {
            return mappedOffset(std::string(a), std::string(n), w);
          })) {
    return {lowered->code, PCs(lowered->next.begin(), lowered->next.end())};
  }
  const std::map<std::string, int> patternWidths{{"_memset_pattern4", 4},
                                                 {"_memset_pattern8", 8},
                                                 {"_memset_pattern16", 16}};
  bool checkedCopy = name == "___memcpy_chk";
  bool copy = name == "_memcpy" || name == "_memmove" || checkedCopy;
  bool zero = name == "_bzero";
  auto pattern = patternWidths.find(name);
  if (copy || zero || name == "_memset" || pattern != patternWidths.end()) {
    std::string count = zero ? "x[1]" : "x[2]";
    std::string destination =
        cached ? mappedOffset("x[0]", count, true) : "x[0]";
    std::string sourceWidth =
        pattern != patternWidths.end() ? str(pattern->second) + "ul" : count;
    std::string source =
        cached ? mappedOffset("x[1]", sourceWidth, false) : "x[1]";
    bool hasSource = copy || pattern != patternWidths.end();
    std::string sourceBounds =
        !hasSource ? ""
        : cached   ? " || " + source + " == ~0ul"
                   : " || x[1] > size || " + sourceWidth + " > size - x[1]";
    std::string destinationBounds =
        cached ? destination + " == ~0ul"
               : "x[0] > size || " + count +
                     " > size - x[0] || !guest_write(x[0], " + count +
                     ", writable)";
    std::string operation;
    if (pattern != patternWidths.end()) {
      operation = "guest_pattern(memory + " + destination + ", memory + " +
                  source + ", " + count + ", " + str(pattern->second) + ");";
    } else if (copy) {
      const auto helper = name == "_memmove" ? "guest_move" : "guest_copy";
      operation = std::string(helper) + "(memory + " + destination + ", memory + " +
                  source + ", " + count + ");";
    } else {
      operation = "guest_fill(memory + " + destination + ", " +
                  (zero ? "0" : "uchar(x[1])") + ", " + count + ");";
    }
    Strings code{returnAddress ? "x[30] = " + str(*returnAddress) + "ul;" : ""};
    if (checkedCopy) {
      code.push_back("if (x[2] > x[3]) {");
      code.push_back("x[33] = x[30];");
      code.push_back("x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);");
      code.push_back("x[35] = " + str(kMemcpyChkFailureService) + "ul; x[34] = 4; return;");
      code.push_back("}");
    }
    const std::string failureAddress = zero
        ? "x[" + str(guestFaultAddress) + "] = x[0]; "
        : "";
    code.insert(code.end(), {"if (" + count + " != 0) {",
         "if (" + destinationBounds + sourceBounds +
             ") { " + failureAddress + "x[33] = pc; x[34] = 2; return; }",
         operation, "}", "if (x[30] == ~0ul) { x[34] = 0; return; }",
         "if (x[30] >= " + str(guestCodeLimit) + "ul) { x[874] = x[30]; x[33] = pc; x[34] = 3; return; }",
         "pc = x[30]; break;"});
    return {std::move(code), returnAddress ? PCs{*returnAddress} : PCs{}};
  }
  const auto& services = decoder_host_service_ids();
  auto service = services.find(name);
  if (service == services.end())
    throw GuestError("Unsupported executable import " + name);
  return {
      {returnAddress ? "x[30] = " + str(*returnAddress) + "ul;" : "",
       "x[33] = x[30];",
       "x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);",
       "x[35] = " + str(service->second) + "ul; x[34] = 4; return;"},
      service->second == 3 || service->second == 169 || service->second == 72 || service->second == 150 ||
              !returnAddress
          ? PCs{}
          : PCs{*returnAddress}};
}

Instruction decode_process(uint32_t bits, int64_t pc,
                           const std::map<int64_t, std::string> &imports,
                           const std::map<int64_t, int64_t> &guestStubs,
                           bool cached) {
  auto physical = [&](std::string address, int64_t width,
                      bool load) -> std::string {
    return cached
               ? mappedOffset(address, (std::string(str(width)) + "ul"), !load)
               : address;
  };
  auto outside = [&](std::string address, int64_t width,
                     bool load) -> std::string {
    if (cached) {
      return (std::string(str(physical(address, width, load))) + " == ~0ul");
    }
    return (std::string(str(address)) + " < " + str(16384) + "ul || " +
            str(address) + " > size - " + str(width) + "ul") +
           (load ? (std::string(""))
                 : (std::string(" || !guest_write(") + str(address) + ", " +
                    str(width) + "ul, writable)"));
  };
  auto field = [&](int64_t start, int64_t width) -> uint32_t {
    return (bits >> start) & ((1 << width) - 1);
  };
  auto sign_extend = [&](uint32_t raw, int64_t width) -> int64_t {
    auto sign = 1 << (width - 1);
    return (int64_t(raw) ^ sign) - sign;
  };
  auto reg = [&](uint32_t index, bool sp = false) -> std::string {
    return index == 31 && !sp ? (std::string("0ul"))
                              : (std::string("x[") + str(index) + "]");
  };
  auto next = pc + 4;
  auto rd = field(0, 5), rn = field(5, 5);
  const auto decodedOperation = translate_core::decode_operation(bits);
  const bool typedBranch = decodedOperation &&
                           decodedOperation->opcode == translate_core::Opcode::Branch;
  const bool typedBranchLink = decodedOperation &&
                               decodedOperation->opcode == translate_core::Opcode::BranchLink;
  const bool typedBranchRegister = decodedOperation &&
      decodedOperation->opcode == translate_core::Opcode::BranchRegister;
  const bool typedBranchZero = decodedOperation &&
                               decodedOperation->opcode == translate_core::Opcode::BranchZero;
  const bool typedBranchCondition = decodedOperation &&
      decodedOperation->opcode == translate_core::Opcode::BranchCondition;
  const bool typedTestBitBranch = decodedOperation &&
      decodedOperation->opcode == translate_core::Opcode::TestBitBranch;
  if (typedBranch) {
    const auto target = pc + static_cast<std::int32_t>(decodedOperation->immediate);
    if (!imports.count(target) && !guestStubs.count(target)) {
      return translate_core::make_operation_instruction(
          *decodedOperation, {target}, translate_core::LoweringMode::Process,
          guestCodeLimit, static_cast<std::uint64_t>(pc));
    }
  }
  if (typedBranchLink) {
    const auto target = pc + static_cast<std::int32_t>(decodedOperation->immediate);
    if (target >= 0 && static_cast<std::uint64_t>(target) < guestCodeLimit &&
        !imports.count(target) && !guestStubs.count(target)) {
      return translate_core::make_operation_instruction(
          *decodedOperation, {target, next}, translate_core::LoweringMode::Process,
          guestCodeLimit, static_cast<std::uint64_t>(pc));
    }
  }
  if (typedBranchRegister) {
    return translate_core::make_operation_instruction(
        *decodedOperation, {}, translate_core::LoweringMode::Process,
        guestCodeLimit, static_cast<std::uint64_t>(pc));
  }
  if (typedBranchZero) {
    const auto target = pc + static_cast<std::int32_t>(decodedOperation->shift);
    return translate_core::make_operation_instruction(
        *decodedOperation, {target, next}, translate_core::LoweringMode::Process,
        guestCodeLimit, static_cast<std::uint64_t>(pc));
  }
  if (typedBranchCondition) {
    const auto target = pc + static_cast<std::int32_t>(decodedOperation->immediate);
    return translate_core::make_operation_instruction(
        *decodedOperation, {target, next}, translate_core::LoweringMode::Process,
        guestCodeLimit, static_cast<std::uint64_t>(pc));
  }
  if (typedTestBitBranch) {
    const auto target = pc + static_cast<std::int32_t>(decodedOperation->shift);
    return translate_core::make_operation_instruction(
        *decodedOperation, {target, next}, translate_core::LoweringMode::Process,
        guestCodeLimit, static_cast<std::uint64_t>(pc));
  }
  if (decodedOperation && !typedBranch && !typedBranchLink &&
      !typedBranchRegister && !typedBranchZero &&
      !typedBranchCondition &&
      !typedTestBitBranch &&
      decodedOperation->opcode != translate_core::Opcode::LoadUnsignedImmediate &&
      decodedOperation->opcode != translate_core::Opcode::StoreUnsignedImmediate &&
      decodedOperation->opcode != translate_core::Opcode::LoadPair &&
      decodedOperation->opcode != translate_core::Opcode::StorePair &&
      decodedOperation->opcode != translate_core::Opcode::LoadUnscaledWriteback &&
      decodedOperation->opcode != translate_core::Opcode::StoreUnscaledWriteback &&
      decodedOperation->opcode != translate_core::Opcode::LoadSignedScalar) {
    const auto successors =
        decodedOperation->opcode == translate_core::Opcode::Return
            ? PCs{}
            : PCs{next};
    return translate_core::make_operation_instruction(
        *decodedOperation, successors, translate_core::LoweringMode::Process,
        guestCodeLimit, static_cast<std::uint64_t>(pc));
  }
  auto emit = [&](auto... lines) -> Instruction {
    std::vector<std::string> code{lines...};
    for (auto &line : code)
      replace_all(line, "x[34] = 2;", "x[33] = " + str(pc) + "ul; x[34] = 2;");
    Instruction instruction{code, {next}};
    if (decodedOperation &&
        (decodedOperation->opcode == translate_core::Opcode::LoadUnsignedImmediate ||
         decodedOperation->opcode == translate_core::Opcode::StoreUnsignedImmediate ||
         decodedOperation->opcode == translate_core::Opcode::LoadPair ||
         decodedOperation->opcode == translate_core::Opcode::StorePair ||
         decodedOperation->opcode == translate_core::Opcode::LoadUnscaledWriteback ||
         decodedOperation->opcode == translate_core::Opcode::StoreUnscaledWriteback ||
         decodedOperation->opcode == translate_core::Opcode::LoadSignedScalar)) {
      instruction.operation = *decodedOperation;
    }
    return instruction;
  };
  if (bits == 0xd503201f || (bits & 0xffffff3f) == 0xd503241f) {
    return emit();
  }
  if (bits == 0xd4001001) {
    return make_instruction(
        Strings{
            (std::string("x[33] = ") + str(next) + "ul;"),
            (std::string("x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | "
                         "(uint(c) << 3);")),
            (std::string("x[34] = 13; return;")),
        },
        PCs{});
  }
  auto dataBarrier = bits & 0xfffff0ff;
  if (bits == 0xd5033fdf ||
      (contains({0xd503309f, 0xd50330bf}, dataBarrier) && field(8, 2) != 0)) {
    return emit((std::string("threadgroup_barrier(mem_flags::mem_device);")));
  }
  if ((bits & 0xffffffe0) == 0xd53bd060) {
    return rd == 31 ? emit()
                    : emit((std::string(str(reg(rd))) + " = x[" +
                            str(guestThreadPointer) + "];"));
  }
  if ((bits & 0xffffffe0) == 0xd53bd040) {
    // Darwin puts the CPU and cluster indices here. The guest has one of each.
    return rd == 31 ? emit() : emit((std::string(str(reg(rd))) + " = 0ul;"));
  }
  auto conditions = Map<std::string>{{0, (std::string("z"))},
                                     {1, (std::string("!z"))},
                                     {2, (std::string("c"))},
                                     {3, (std::string("!c"))},
                                     {4, (std::string("n"))},
                                     {5, (std::string("!n"))},
                                     {6, (std::string("v"))},
                                     {7, (std::string("!v"))},
                                     {8, (std::string("c && !z"))},
                                     {9, (std::string("!c || z"))},
                                     {10, (std::string("n == v"))},
                                     {11, (std::string("n != v"))},
                                     {12, (std::string("!z && n == v"))},
                                     {13, (std::string("z || n != v"))}};
  auto arithmeticFlags = [&](std::string lhs, std::string rhs, std::string type,
                             bool subtract) -> Strings {
    auto sign = type == (std::string("ulong"))
                    ? (std::string("0x8000000000000000ul"))
                    : (std::string("0x80000000u"));
    auto op = subtract ? (std::string("-")) : (std::string("+"));
    auto result = (std::string(str(type)) + "(" + str(lhs) + " " + str(op) +
                   " " + str(rhs) + ")");
    auto carry = subtract ? (std::string(str(lhs)) + " >= " + str(rhs))
                          : (std::string(str(result)) + " < " + str(lhs));
    auto overflow =
        subtract ? (std::string("(") + str(lhs) + " ^ " + str(rhs) + ")")
                 : (std::string("~(") + str(lhs) + " ^ " + str(rhs) + ")");
    return Strings{(std::string("z = (") + str(result) + " == 0); n = ((" +
                    str(result) + " & " + str(sign) + ") != 0); c = (" +
                    str(carry) + ");"),
                   (std::string("v = ((") + str(overflow) + " & (" + str(lhs) +
                    " ^ " + str(result) + ")) & " + str(sign) + ") != 0;")};
  };
  if (bits == 0xdac1a7fe || bits == 0xd503237f) {
    // Model authenticated returns with a private shadow stack of LR, SP, and
    // signer PC.
    return emit(
        (std::string("if (x[37] >= 256ul) { x[34] = 6; return; }")),
        (std::string("{ ulong slot = 38ul + 3ul * x[37]++;")),
        (std::string("x[slot] = x[30]; x[slot + 1] = x[31]; x[slot + 2] = ") +
         str(pc) + "ul; }"));
  }
  auto checkReturn = [&](std::optional<int64_t> signer) -> Strings {
    auto check = signer ? " || x[slot + 2] != ulong(" + str(*signer) + ")" : "";
    return Strings{
        (std::string("if (x[37] == 0) { x[34] = 6; return; }")),
        (std::string("{ ulong slot = 38ul + 3ul * --x[37];")),
        (std::string("if (x[30] != x[slot] || x[31] != x[slot + 1]") +
         str(check) + ") { x[34] = 6; return; } }"),
    };
  };
  if (bits == 0xd50323ff || (bits & 0xffe0001f) == 0xf3a0001f) {
    std::optional<int64_t> signer =
        bits == 0xd50323ff
            ? std::nullopt
            : std::optional<int64_t>(pc - int64_t(field(5, 16)) * 4);
    return make_instruction(checkReturn(signer), PCs{next});
  }
  auto authenticated = (bits & 0xffe0001f) == 0x5520001f || bits == 0xd65f0fff;
  if (authenticated) {
    std::optional<int64_t> signer =
        bits == 0xd65f0fff
            ? std::nullopt
            : std::optional<int64_t>(pc - int64_t(field(5, 16)) * 4);
    auto authentication = authenticated ? checkReturn(signer) : Strings{};
    return make_instruction(
        authentication +
            Strings{
                (std::string("if (x[30] == ~0ul) { x[34] = 0; return; }")),
                (std::string("if (x[30] >= ") + str(guestCodeLimit) +
                 "ul) { x[874] = x[30]; x[33] = pc; x[34] = 3; return; }"),
                (std::string("x[33] = x[30];")),
                (std::string("x[32] = uint(z) | (uint(n) << 1) | (uint(v) << "
                             "2) | (uint(c) << 3);")),
                (std::string("x[34] = 7; return;")),
            },
        PCs{});
  }
  auto immediateBranch = bits & 0xfc000000;
  if (immediateBranch == 0x94000000 || typedBranch) {
    auto linked = immediateBranch == 0x94000000;
    auto target = pc + (typedBranch
                            ? static_cast<std::int32_t>(decodedOperation->immediate)
                            : sign_extend(field(0, 26), 26) * 4);
    auto usesGuestAllocator =
        (imports.count(target) &&
         imports.at(target) == (std::string("_malloc_size"))) &&
        (target >= (1LL << 32) && target < (1LL << 44));
    if (imports.count(target) && !usesGuestAllocator) {
      auto name = imports.at(target);
      return hostCall(
          name, linked ? std::optional<int64_t>(next) : std::nullopt, cached);
    }
    auto destination =
        (guestStubs.count(target) ? guestStubs.at(target) : target);
    return make_instruction(
        Strings{
            linked ? (std::string("x[30] = ") + str(next) + "ul;")
                   : (std::string("")),
            (std::string("pc = ") + str(destination) + "ul; break;"),
        },
        linked ? PCs{destination, next} : PCs{destination});
  }
  if ((bits & 0xffe0fc00) == 0x9ac03000) {
    return rd == 31 ? emit()
                    : emit((std::string(str(reg(rd))) + " = guest_pacga(x, " +
                            str(reg(rn)) + ", " + str(reg(field(16, 5), true)) +
                            ");"));
  }
  if ((bits & 0xfffffbe0) == 0xdac143e0) {
    return rd == 31 ? emit()
                    : emit((std::string("x[") + str(rd) +
                            "] = strip_guest_pointer(x, x[" + str(rd) + "]);"));
  }
  auto authIP = (bits & 0xffffff3f) == 0xd503211f;
  auto authZeroModifier = (bits & 0xffffe3e0) == 0xdac123e0;
  if (authIP ||
      ((authZeroModifier || (bits & 0xffffe000) == 0xdac10000) && rd != 31)) {
    auto destination = authIP ? 17 : rd;
    auto authenticate = authIP ? field(7, 1) == 1 : field(12, 1) == 1;
    auto operation = authenticate ? (std::string("authenticate_guest_pointer"))
                                  : (std::string("sign_guest_pointer"));
    auto key = authIP ? field(6, 1) : field(10, 2);
    auto failure = key >= 2 ? 9 : 12;
    auto result = authenticate ? (std::string("~0ul")) : (std::string("0ul"));
    auto modifier =
        authIP ? (std::string("x[16]"))
               : (authZeroModifier ? (std::string("0ul")) : reg(rn, true));
    return emit(
        (std::string("{ ulong result = ") + str(operation) + "(x, x[" +
         str(destination) + "], " + str(modifier) + ", " + str(key) + "ul);"),
        (std::string("if (result == ") + str(result) + ") { x[" +
         str(guestFaultAddress) + "] = x[" + str(destination) + "]; x[" +
         str(guestCacheFaultAddress) + "] = " + str(modifier) + "; x[33] = " +
         str(pc) + "ul; x[34] = " + str(failure) + "; return; }"),
        (std::string("x[") + str(destination) + "] = result; }"));
  }
  auto authZero = bits & 0xfffff81f;
  auto authRegister = bits & 0xfffff800;
  if (contains({0xd61f081f, 0xd63f081f}, authZero) ||
      contains({0xd71f0800, 0xd73f0800}, authRegister)) {
    auto zero = contains({0xd61f081f, 0xd63f081f}, authZero);
    auto linked = field(21, 1) == 1;
    auto modifier = zero ? (std::string("0ul")) : reg(rd, true);
    auto key = field(10, 1);
    return make_instruction(
        Strings{
            (std::string("{ ulong target = authenticate_guest_pointer(x, ") +
             str(reg(rn)) + ", " + str(modifier) + ", " + str(key) + "ul);"),
            (std::string("if (target >= ") + str(guestCodeLimit) + "ul) { x[" +
             str(guestFaultAddress) + "] = " + str(reg(rn)) + "; x[" +
             str(guestCacheFaultAddress) + "] = " + str(modifier) +
             "; x[33] = " + str(pc) + "ul; x[34] = 10; return; }"),
            linked ? (std::string("x[30] = ") + str(next) + "ul;")
                   : (std::string("")),
            (std::string("pc = target; } break;")),
        },
        linked ? PCs{next} : PCs{});
  }
  if ((bits & 0x7ffffc00) == 0x5ac00000) {
    auto source = reg(rn);
    auto low = (std::string("reverse_bits(uint(") + str(source) + "))");
    auto result = field(31, 1) == 0 ? low
                                    : (std::string("(ulong(") + str(low) +
                                       ") << 32) | ulong(reverse_bits(uint(" +
                                       str(source) + " >> 32)))");
    return rd == 31
               ? emit()
               : emit((std::string(str(reg(rd))) + " = " + str(result) + ";"));
  }
  if ((bits & 0xfffffc00) == 0x5ac00800) {
    return emit(rd == 31
                    ? (std::string(""))
                    : (std::string("\n            { uint value = uint(") +
                       str(reg(rn)) +
                       ");\n            value = ((value & 0x00ff00ffu) << 8) | "
                       "((value >> 8) & 0x00ff00ffu);\n            " +
                       str(reg(rd)) +
                       " = (value << 16) | (value >> 16); }\n            "));
  }
  if ((bits & 0xfffffc00) == 0xdac00c00) {
    return emit(rd == 31
                    ? (std::string(""))
                    : (std::string("\n            { ulong value = ") +
                       str(reg(rn)) +
                       ";\n            value = ((value & 0x00ff00ff00ff00fful) "
                       "<< 8) | ((value >> 8) & 0x00ff00ff00ff00fful);\n       "
                       "     value = ((value & 0x0000ffff0000fffful) << 16) | "
                       "((value >> 16) & 0x0000ffff0000fffful);\n            " +
                       str(reg(rd)) +
                       " = (value << 32) | (value >> 32); }\n            "));
  }
  auto leadingCount = bits & 0x7ffff800;
  if (leadingCount == 0x5ac01000) {
    auto wide = field(31, 1) != 0;
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    auto width = wide ? 64 : 32;
    auto signCount = field(10, 1) != 0;
    auto normalize = signCount ? (std::string("value ^= ") + str(type) +
                                  "(0) - (value >> " + str(width - 1) + ");")
                               : (std::string(""));
    return emit(rd == 31
                    ? (std::string(""))
                    : (std::string("{ ") + str(type) + " value = " + str(type) +
                       "(" + str(reg(rn)) + "); " + str(normalize) + " " +
                       str(reg(rd)) + " = (value == 0 ? " + str(width) +
                       "u : clz(value)) - " + str(signCount ? 1 : 0) + "u; }"));
  }
  auto select = bits & 0x7fe00c00;
  if (contains({0x1a800000, 0x1a800400, 0x5a800000, 0x5a800400}, select) &&
      (bool(conditions[field(12, 4)]))) {
    auto condition = conditions[field(12, 4)];
    auto type =
        field(31, 1) == 0 ? (std::string("uint")) : (std::string("ulong"));
    auto right = reg(field(16, 5));
    std::string alternative;
    switch (select) {
    case 0x1a800400:
      alternative = (std::string(str(right)) + " + 1ul");
      break;
    case 0x5a800000:
      alternative = (std::string("~") + str(right));
      break;
    case 0x5a800400:
      alternative = (std::string("0ul - ") + str(right));
      break;
    default:
      alternative = right;
      break;
    }
    return emit(rd == 31 ? (std::string(""))
                         : (std::string(str(reg(rd))) + " = " + str(type) +
                            "((" + str(condition) + ") ? " + str(reg(rn)) +
                            " : " + str(alternative) + ");"));
  }
  if ((bits & 0xff000010) == 0x54000000) {
    if (auto condition = conditions[field(0, 4)]; condition) {
      auto target = pc + sign_extend(field(5, 19), 19) * 4;
      return make_instruction(
          Strings{(std::string("pc = (") + str(condition) + ") ? " +
                   str(target) + "ul : " + str(next) + "ul; break;")},
          PCs{target, next});
    }
  }
  auto flagsImmediate = (bits & 0x3f800000) == 0x31000000;
  auto flagsShifted = (bits & 0x3f200000) == 0x2b000000;
  auto flagsExtended = (bits & 0x3fe00000) == 0x2b200000;
  if (flagsImmediate || flagsShifted || flagsExtended) {
    auto subtract = field(30, 1) == 1;
    auto wide = field(31, 1) == 1;
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    auto lhs = (std::string(str(type)) + "(" +
                str(reg(rn, flagsImmediate || flagsExtended)) + ")");
    std::string rhs;
    if (flagsImmediate) {
      rhs = (std::string(str(type)) + "(" +
             str(field(10, 12) << (field(22, 1) * 12)) + "u)");
    } else {
      auto source = reg(field(16, 5));
      if (flagsExtended) {
        auto distance = field(10, 3);
        if (!(distance <= 4)) {
          throw GuestError((std::string("Invalid extended arithmetic shift")));
        }
        auto conversion = Strings{(std::string("uchar")),
                                  (std::string("ushort")),
                                  (std::string("uint")),
                                  (std::string("ulong")),
                                  (std::string("char")),
                                  (std::string("short")),
                                  (std::string("int")),
                                  (std::string("long"))}[int64_t(field(13, 3))];
        rhs = (std::string("(") + str(type) + "(" + str(conversion) + "(" +
               str(source) + ")) << " + str(distance) + ")");
      } else {
        auto distance = field(10, 6);
        auto shift = field(22, 2);
        if (!(distance < (wide ? 64 : 32) && shift < 3)) {
          throw GuestError((std::string("Invalid arithmetic shift")));
        }
        if (shift == 2) {
          rhs = (std::string(str(type)) + "(" +
                 str(wide ? (std::string("long")) : (std::string("int"))) +
                 "(" + str(source) + ") >> " + str(distance) + ")");
        } else {
          rhs = (std::string("(") + str(type) + "(" + str(source) + ") " +
                 str(shift == 0 ? (std::string("<<")) : (std::string(">>"))) +
                 " " + str(distance) + ")");
        }
      }
    }
    auto op = subtract ? (std::string("-")) : (std::string("+"));
    auto assign = rd == 31
                      ? (std::string(""))
                      : (std::string(str(reg(rd))) + " = " + str(type) + "(" +
                         str(lhs) + " " + str(op) + " " + str(rhs) + ");");
    return make_instruction(
        arithmeticFlags(lhs, rhs, type, subtract) + Strings{assign}, PCs{next});
  }
  if ((bits & 0x3fe00410) == 0x3a400000 && (bool(conditions[field(12, 4)]))) {
    auto condition = conditions[field(12, 4)];
    auto type =
        field(31, 1) == 1 ? (std::string("ulong")) : (std::string("uint"));
    auto flags = field(0, 4);
    auto lhs = (std::string(str(type)) + "(" + str(reg(rn)) + ")");
    auto rhs =
        field(11, 1) == 1
            ? (std::string(str(type)) + "(" + str(field(16, 5)) + "u)")
            : (std::string(str(type)) + "(" + str(reg(field(16, 5))) + ")");
    return make_instruction(
        Strings{(std::string("if (") + str(condition) + ") {")} +
            arithmeticFlags(lhs, rhs, type, field(30, 1) == 1) +
            Strings{(std::string("} else { n = ") + str(flags & 8) +
                     "; z = " + str(flags & 4) + "; c = " + str(flags & 2) +
                     "; v = " + str(flags & 1) + "; }")},
        PCs{next});
  }
  if ((bits & 0x7f800000) == 0x11000000 || (bits & 0x7f800000) == 0x51000000) {
    auto immediate = field(10, 12) << (field(22, 1) * 12);
    auto operation =
        field(30, 1) == 0 ? (std::string("+")) : (std::string("-"));
    auto type =
        field(31, 1) == 0 ? (std::string("uint")) : (std::string("ulong"));
    return emit((std::string(str(reg(rd, true))) + " = " + str(type) + "(" +
                 str(reg(rn, true)) + " " + str(operation) + " " +
                 str(immediate) + "ul);"));
  }
  if (((bits & 0x7fa00000) == 0x13800000) && (field(22, 1) == field(31, 1)) &&
      (rd != 31)) {
    auto width = field(31, 1) == 1 ? 64 : 32;
    auto type = width == 64 ? (std::string("ulong")) : (std::string("uint"));
    auto shift = int64_t(field(10, 6));
    if (!(shift < width)) {
      throw GuestError((std::string("Invalid EXTR shift")));
    }
    auto lower = (std::string(str(type)) + "(" + str(reg(field(16, 5))) + ")");
    if (shift == 0) {
      return emit((std::string(str(reg(rd))) + " = " + str(lower) + ";"));
    }
    auto upper = (std::string(str(type)) + "(" + str(reg(rn)) + ")");
    return emit((std::string(str(reg(rd))) + " = (" + str(lower) + " >> " +
                 str(shift) + ") | (" + str(upper) + " << " +
                 str(width - shift) + ");"));
  }
  auto division = bits & 0x7fe0fc00;
  if ((contains({0x1ac00800, 0x1ac00c00}, division)) && (rd != 31)) {
    auto wide = field(31, 1) == 1;
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    auto left = (std::string(str(type)) + "(" + str(reg(rn)) + ")"),
         right = (std::string(str(type)) + "(" + str(reg(field(16, 5))) + ")");
    if (division == 0x1ac00800) {
      return emit((std::string(str(reg(rd))) + " = " + str(right) +
                   " == 0 ? 0 : " + str(left) + " / " + str(right) + ";"));
    }
    auto signedType = wide ? (std::string("long")) : (std::string("int"));
    auto minimum = wide ? (std::string("0x8000000000000000ul"))
                        : (std::string("0x80000000u"));
    return emit((std::string(str(reg(rd))) + " = " + str(right) +
                 " == 0 ? 0 : (" + str(left) + " == " + str(minimum) + " && " +
                 str(right) + " == ~" + str(type) + "(0)) ? " + str(left) +
                 " : " + str(type) + "(" + str(signedType) + "(" + str(left) +
                 ") / " + str(signedType) + "(" + str(right) + "));"));
  }
  auto variableShift = bits & 0x7fe0fc00;
  if ((contains({0x1ac02000, 0x1ac02400, 0x1ac02800, 0x1ac02c00},
                variableShift)) &&
      (rd != 31)) {
    auto wide = field(31, 1) == 1;
    auto width = wide ? 64 : 32;
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    auto source = (std::string(str(type)) + "(" + str(reg(rn)) + ")");
    auto shift = (std::string("(uint(") + str(reg(field(16, 5))) + ") & " +
                  str(width - 1) + "u)");
    std::string value;
    switch (variableShift) {
    case 0x1ac02000:
      value = (std::string(str(source)) + " << " + str(shift));
      break;
    case 0x1ac02400:
      value = (std::string(str(source)) + " >> " + str(shift));
      break;
    case 0x1ac02800:
      value = (std::string(
                   str(wide ? (std::string("long")) : (std::string("int")))) +
               "(" + str(source) + ") >> " + str(shift));
      break;
    default:
      value = (std::string("(") + str(source) + " >> " + str(shift) + ") | (" +
               str(source) + " << ((0u - " + str(shift) + ") & " +
               str(width - 1) + "u))");
      break;
    }
    return emit((std::string(str(reg(rd))) + " = " + str(type) + "(" +
                 str(value) + ");"));
  }
  auto logicalRegister = bits & 0x7f000000;
  if ((contains({0x0a000000, 0x2a000000, 0x4a000000, 0x6a000000},
                logicalRegister)) &&
      (field(10, 6) < (field(31, 1) == 0 ? 32 : 64))) {
    auto wide = field(31, 1) == 1;
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    auto width = wide ? 64 : 32;
    auto operation =
        Strings{(std::string("&")), (std::string("|")), (std::string("^")),
                (std::string("&"))}[int64_t(field(29, 2))];
    auto left = (std::string(str(type)) + "(" + str(reg(rn)) + ")");
    auto source = (std::string(str(type)) + "(" + str(reg(field(16, 5))) + ")");
    auto distance = field(10, 6);
    std::string shifted;
    switch (field(22, 2)) {
    case 0:
      shifted = (std::string("(") + str(source) + " << " + str(distance) + ")");
      break;
    case 1:
      shifted = (std::string("(") + str(source) + " >> " + str(distance) + ")");
      break;
    case 2:
      shifted = (std::string(str(type)) + "(" +
                 str(wide ? (std::string("long")) : (std::string("int"))) +
                 "(" + str(source) + ") >> " + str(distance) + ")");
      break;
    default:
      shifted = distance == 0
                    ? source
                    : (std::string("((") + str(source) + " >> " +
                       str(distance) + ") | (" + str(source) + " << " +
                       str(width - int64_t(distance)) + "))");
      break;
    }
    auto right =
        field(21, 1) == 0 ? shifted : (std::string("~") + str(shifted));
    auto result = (std::string(str(type)) + "(" + str(left) + " " +
                   str(operation) + " " + str(right) + ")");
    auto assign = rd == 31
                      ? (std::string(""))
                      : (std::string(str(reg(rd))) + " = " + str(result) + ";");
    if (logicalRegister == 0x6a000000) {
      return emit((std::string("z = (") + str(result) + " == 0); n = ((" +
                   str(result) + " >> " + str(width - 1) +
                   ") != 0); c = false; v = false;"),
                  assign);
    }
    return emit(assign);
  }
  if (((bits & 0x7f800000) == 0x72800000) && (rd != 31) &&
      (field(31, 1) == 1 || field(21, 2) < 2)) {
    auto shift = 16 * field(21, 2);
    auto mask = ~(uint64_t(0xffff) << shift);
    auto immediate = uint64_t(field(5, 16)) << shift;
    auto type =
        field(31, 1) == 0 ? (std::string("uint")) : (std::string("ulong"));
    return emit((std::string(str(reg(rd))) + " = " + str(type) + "((" +
                 str(reg(rd)) + " & " + str(mask) + "ul) | " + str(immediate) +
                 "ul);"));
  }
  if (((bits & 0xffe0f000) == 0x9a002000) && (rd != 31) && (rn != 31)) {
    // Guest pointers have no top-byte tags.
    return emit((std::string(str(reg(rd))) + " = " + str(reg(rn)) + " + (" +
                 str(reg(field(16, 5))) + " << " + str(field(10, 2)) + ");"));
  }
  if (((bits & 0x7fe0e000) == 0x1ac04000) && (rd != 31)) {
    auto operation = field(10, 3);
    auto width = 1 << (operation & 3);
    if (!((width == 8) == (field(31, 1) == 1))) {
      throw GuestError((std::string("Invalid CRC32 width")));
    }
    auto polynomial = operation < 4 ? (std::string("0xedb88320u"))
                                    : (std::string("0x82f63b78u"));
    return emit((std::string(str(reg(rd))) + " = guest_crc32(uint(" +
                 str(reg(rn)) + "), " + str(reg(field(16, 5))) + ", " +
                 str(width) + ", " + str(polynomial) + ");"));
  }
  auto wideningMultiply = bits & 0xffe00000;
  if ((contains({0x9b200000, 0x9ba00000}, wideningMultiply)) && (rd != 31)) {
    auto sign = wideningMultiply == 0x9b200000;
    auto operation =
        field(15, 1) == 0 ? (std::string("+")) : (std::string("-"));
    auto accumulator = reg(field(10, 5));
    auto lhs = (std::string(str(sign ? (std::string("long(int"))
                                     : (std::string("ulong(uint")))) +
                "(" + str(reg(rn)) + "))");
    auto rhs = (std::string(str(sign ? (std::string("long(int"))
                                     : (std::string("ulong(uint")))) +
                "(" + str(reg(field(16, 5))) + "))");
    return emit((std::string(str(reg(rd))) + " = " + str(accumulator) + " " +
                 str(operation) + " ulong(" + str(lhs) + " * " + str(rhs) +
                 ");"));
  }
  auto multiplyHigh = bits & 0xffe0fc00;
  if ((contains({0x9b407c00, 0x9bc07c00}, multiplyHigh)) && (rd != 31)) {
    auto lhs = reg(rn), rhs = reg(field(16, 5));
    auto high =
        (std::string("multiply_high(") + str(lhs) + ", " + str(rhs) + ")");
    auto result =
        multiplyHigh == 0x9b407c00
            ? (std::string(str(high)) + " - (" + str(lhs) + " >> 63) * " +
               str(rhs) + " - (" + str(rhs) + " >> 63) * " + str(lhs))
            : high;
    return emit((std::string(str(reg(rd))) + " = " + str(result) + ";"));
  }
  if (((bits & 0x7fe00000) == 0x1b000000) && (rd != 31)) {
    auto type =
        field(31, 1) == 1 ? (std::string("ulong")) : (std::string("uint"));
    auto operation =
        field(15, 1) == 0 ? (std::string("+")) : (std::string("-"));
    auto accumulator =
        (std::string(str(type)) + "(" + str(reg(field(10, 5))) + ")");
    auto left = (std::string(str(type)) + "(" + str(reg(rn)) + ")");
    auto right = (std::string(str(type)) + "(" + str(reg(field(16, 5))) + ")");
    return emit((std::string(str(reg(rd))) + " = " + str(type) + "(" +
                 str(accumulator) + " " + str(operation) + " " + str(left) +
                 " * " + str(right) + ");"));
  }
  auto shiftedArithmetic = bits & 0x7f200000;
  if ((contains({0x0b000000, 0x4b000000}, shiftedArithmetic)) &&
      (field(22, 2) < 3) && (field(10, 6) < (field(31, 1) == 0 ? 32 : 64))) {
    auto type =
        field(31, 1) == 0 ? (std::string("uint")) : (std::string("ulong"));
    auto signedType =
        field(31, 1) == 0 ? (std::string("int")) : (std::string("long"));
    auto operation = shiftedArithmetic == 0x0b000000 ? (std::string("+"))
                                                     : (std::string("-"));
    auto shift = field(22, 2) == 0 ? (std::string("<<")) : (std::string(">>"));
    auto operandType = field(22, 2) == 2 ? signedType : type;
    auto right = (std::string(str(type)) + "(" + str(operandType) + "(" +
                  str(reg(field(16, 5))) + ") " + str(shift) + " " +
                  str(field(10, 6)) + ")");
    return emit(rd == 31 ? (std::string(""))
                         : (std::string(str(reg(rd))) + " = " + str(type) +
                            "(" + str(type) + "(" + str(reg(rn)) + ") " +
                            str(operation) + " " + str(right) + ");"));
  }
  auto minmax = bits & 0x7fe0f000;
  if (minmax == 0x1ac06000) {
    auto wide = field(31, 1) != 0;
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    auto operand = field(10, 1) == 0
                       ? (wide ? (std::string("long")) : (std::string("int")))
                       : type;
    auto function =
        field(11, 1) == 0 ? (std::string("max")) : (std::string("min"));
    return emit(rd == 31 ? (std::string(""))
                         : (std::string(str(reg(rd))) + " = " + str(type) +
                            "(" + str(function) + "(" + str(operand) + "(" +
                            str(reg(rn)) + "), " + str(operand) + "(" +
                            str(reg(field(16, 5))) + ")));"));
  }
  if (((bits & 0xfffffc00) == 0x93407c00) && (rd != 31)) {
    return emit((std::string(str(reg(rd))) + " = ulong(long(int(" +
                 str(reg(rn)) + ")));"));
  }
  auto extendedArithmetic = bits & 0x7fe00000;
  if ((contains({0x0b200000, 0x4b200000}, extendedArithmetic)) &&
      (field(10, 3) <= 4)) {
    auto type =
        field(31, 1) == 0 ? (std::string("uint")) : (std::string("ulong"));
    auto extensionType = Strings{
        (std::string("uchar")), (std::string("ushort")), (std::string("uint")),
        (std::string("ulong")), (std::string("char")),   (std::string("short")),
        (std::string("int")),   (std::string("long"))}[int64_t(field(13, 3))];
    auto rhs = (std::string(str(type)) + "(" + str(extensionType) + "(" +
                str(reg(field(16, 5))) + "))");
    auto operation = extendedArithmetic == 0x0b200000 ? (std::string("+"))
                                                      : (std::string("-"));
    return emit((std::string(str(reg(rd, true))) + " = " + str(type) + "(" +
                 str(reg(rn, true)) + " " + str(operation) + " (" + str(rhs) +
                 " << " + str(field(10, 3)) + "));"));
  }
  if ((bits & 0x1f800000) == 0x12000000) {
    auto width = field(31, 1) == 1 ? 64 : 32;
    const auto mask = translate_core::decode_logical_immediate_mask(
        field(22, 1), field(16, 6), field(10, 6), width);
    if (!mask) {
      throw GuestError((std::string("Reserved logical immediate")));
    }
    auto op =
        Strings{(std::string("&")), (std::string("|")), (std::string("^")),
                (std::string("&"))}[int64_t(field(29, 2))];
    auto source =
        width == 32 ? (std::string("uint(") + str(reg(rn)) + ")") : reg(rn);
    auto result = (std::string("(") + str(source) + " " + str(op) + " " +
                   str(*mask) + "ul)");
    auto assign = rd == 31
                      ? (std::string(""))
                      : (std::string(str(reg(rd))) + " = " + str(result) + ";");
    if ((bits & 0x60000000) == 0x60000000) {
      return emit((std::string("z = (") + str(result) + " == 0); n = ((" +
                   str(result) + " >> " + str(width - 1) +
                   ") != 0); c = false; v = false;"),
                  assign);
    }
    return emit(assign);
  }
  if ((bits & 0x7f800000) == 0x53000000) {
    auto width = field(31, 1) == 1 ? 64 : 32;
    auto rotation = int64_t(field(16, 6)), end = int64_t(field(10, 6));
    if (!(rotation < width && end < width && field(22, 1) == field(31, 1))) {
      throw GuestError((std::string("Reserved bitfield encoding")));
    }
    auto count = end >= rotation ? end - rotation + 1 : end + 1;
    auto mask = count == 64 ? UINT64_MAX : (uint64_t(1) << count) - 1;
    auto source = reg(rn);
    auto result = end >= rotation
                      ? (std::string("((") + str(source) + " >> " +
                         str(rotation) + ") & " + str(mask) + "ul)")
                      : (std::string("((") + str(source) + " & " + str(mask) +
                         "ul) << " + str(width - rotation) + ")");
    return emit(rd == 31
                    ? (std::string(""))
                    : (std::string(str(reg(rd))) + " = " + str(result) + ";"));
  }
  if (((bits & 0x7f800000) == 0x33000000) && (field(22, 1) == field(31, 1)) &&
      (rd != 31)) {
    auto wide = field(31, 1) == 1;
    auto width = wide ? 64 : 32;
    auto rotation = int64_t(field(16, 6)), end = int64_t(field(10, 6));
    if (!(rotation < width && end < width)) {
      throw GuestError((std::string("Reserved bitfield merge encoding")));
    }
    auto count = end >= rotation ? end - rotation + 1 : end + 1;
    auto mask = count == 64 ? UINT64_MAX : (uint64_t(1) << count) - 1;
    auto position = end >= rotation ? 0 : width - rotation;
    auto source = reg(rn);
    auto extracted = end >= rotation
                         ? (std::string("((") + str(source) + " >> " +
                            str(rotation) + ") & " + str(mask) + "ul)")
                         : (std::string("((") + str(source) + " & " +
                            str(mask) + "ul) << " + str(position) + ")");
    return emit((std::string(str(reg(rd))) + " = " +
                 str(wide ? (std::string("ulong")) : (std::string("uint"))) +
                 "((" + str(reg(rd)) + " & ~" + str(mask << position) +
                 "ul) | " + str(extracted) + ");"));
  }
  if (((bits & 0x7f800000) == 0x13000000) && (field(22, 1) == field(31, 1)) &&
      (rd != 31)) {
    auto wide = field(31, 1) == 1;
    auto width = wide ? 64 : 32;
    auto rotation = int64_t(field(16, 6)), end = int64_t(field(10, 6));
    if (!(rotation < width && end < width)) {
      throw GuestError((std::string("Reserved signed bitfield encoding")));
    }
    auto count = end >= rotation ? end - rotation + 1 : end + 1;
    auto mask = count == 64 ? UINT64_MAX : (uint64_t(1) << count) - 1;
    auto position = end >= rotation ? 0 : width - rotation;
    auto source = reg(rn);
    auto extracted = end >= rotation
                         ? (std::string("((") + str(source) + " >> " +
                            str(rotation) + ") & " + str(mask) + "ul)")
                         : (std::string("((") + str(source) + " & " +
                            str(mask) + "ul) << " + str(position) + ")");
    auto fill = width - (count + position);
    auto type = wide ? (std::string("ulong")) : (std::string("uint")),
         signedType = wide ? (std::string("long")) : (std::string("int"));
    return emit((std::string(str(reg(rd))) + " = " + str(type) + "(" +
                 str(signedType) + "(" + str(type) + "(" + str(extracted) +
                 ") << " + str(fill) + ") >> " + str(fill) + ");"));
  }
  auto vector = [&](uint32_t index) -> std::string {
    return (std::string("ulong2(x[") + str(806 + 2 * index) + "], x[" +
            str(807 + 2 * index) + "])");
  };
  auto assignVector = [&](std::string expression) -> Instruction {
    return emit((std::string("{ ulong2 value = ") + str(expression) + "; x[" +
                 str(806 + 2 * rd) + "] = value[0]; x[" + str(807 + 2 * rd) +
                 "] = value[1]; }"));
  };
  if (auto lowered = lower_vector_crypto(bits, pc)) {
    return *lowered;
  }
  if ((bits & 0xbfe08400) == 0x2e000000) {
    auto lanes = field(30, 1) == 0 ? 8 : 16;
    auto start = int64_t(field(11, 4));
    if (!(start < lanes)) {
      throw GuestError((std::string("Invalid vector extract offset")));
    }
    return emit((
        std::string("\n            { ulong2 first = ") + str(vector(rn)) +
        ", second = " + str(vector(field(16, 5))) +
        ", value = ulong2(0);\n              for (uint lane = 0; lane < " +
        str(lanes) + "; ++lane) {\n                  uint index = lane + " +
        str(start) + ";\n                  ulong byte = index < " + str(lanes) +
        " ? lane_read(first, index, 8)\n                                       "
        "          : lane_read(second, index - " +
        str(lanes) +
        ", 8);\n                  lane_add(value, lane, 8, byte);\n            "
        "  }\n              x[" +
        str(806 + 2 * rd) + "] = value[0]; x[" + str(807 + 2 * rd) +
        "] = value[1]; }\n            "));
  }
  if ((bits & 0xbfe08c00) == 0x0e000000) {
    auto count = int64_t(field(13, 2)) + 1;
    std::string table;
    for (int i = 0; i < count; ++i) {
      if (i)
        table += ", ";
      table += vector((rn + i) & 31);
    }
    auto indices = vector(field(16, 5));
    auto original = field(12, 1) == 0 ? (std::string("ulong2(0)")) : vector(rd);
    auto lanes = field(30, 1) == 0 ? 8 : 16;
    return emit(
        (std::string("\n            { ulong2 table[") + str(count) + "] = { " +
         str(table) + " };\n              ulong2 indices = " + str(indices) +
         ", original = " + str(original) +
         ", value = ulong2(0);\n              for (uint lane = 0; lane < " +
         str(lanes) +
         "; ++lane) {\n                  uint index = uint(lane_read(indices, "
         "lane, 8));\n                  ulong byte = index < " +
         str(count * 16) +
         " ? lane_read(table[index / 16], index % 16, 8)\n                     "
         "                                 : lane_read(original, lane, 8);\n   "
         "               lane_add(value, lane, 8, byte);\n              }\n    "
         "          x[" +
         str(806 + 2 * rd) + "] = value[0]; x[" + str(807 + 2 * rd) +
         "] = value[1]; }\n            "));
  }
  if ((bits & 0xbf3ffc00) == 0x0e200800) {
    auto width = 8 << field(22, 2);
    if (!(width < 64)) {
      throw GuestError((std::string("Invalid REV64 arrangement")));
    }
    auto lanes = (field(30, 1) == 0 ? 64 : 128) / int64_t(width);
    return emit(
        (std::string("\n            { ulong2 source = ") + str(vector(rn)) +
         ", value = ulong2(0);\n              for (uint lane = 0; lane < " +
         str(lanes) + "; ++lane)\n                  lane_add(value, lane, " +
         str(width) + ", lane_read(source, lane ^ " + str(64 / width - 1) +
         ", " + str(width) + "));\n              x[" + str(806 + 2 * rd) +
         "] = value[0]; x[" + str(807 + 2 * rd) +
         "] = value[1]; }\n            "));
  }
  if ((bits & 0xfffffc00) == 0x6e200800) {
    auto source = vector(rn);
    return assignVector(
        (std::string("\n            ((") + str(source) +
         " & 0x000000ff000000fful) << 24) |\n            ((" + str(source) +
         " & 0x0000ff000000ff00ul) << 8) |\n            ((" + str(source) +
         " >> 8) & 0x0000ff000000ff00ul) |\n            ((" + str(source) +
         " >> 24) & 0x000000ff000000fful)\n            "));
  }
  if ((bits & 0xffe0fc00) == 0x4e001c00) {
    auto immediate = field(16, 5);
    if (!(immediate != 0 && ctz(immediate) < 4)) {
      throw GuestError((std::string("Invalid vector INS element size")));
    }
    auto width = 8 << ctz(immediate);
    auto lane = int64_t(immediate) >> (ctz(immediate) + 1);
    auto bit = width * lane;
    auto slot = int64_t(806 + 2 * rd) + bit / 64;
    auto shift = bit % 64;
    auto mask = width == 64 ? UINT64_MAX : (uint64_t(1) << width) - 1;
    return emit((std::string("x[") + str(slot) + "] = (x[" + str(slot) +
                 "] & ~" + str(mask << shift) + "ul) | ((" + str(reg(rn)) +
                 " & " + str(mask) + "ul) << " + str(shift) + ");"));
  }
  auto vectorBits = field(30, 1) == 1 ? 128 : 64;
  auto floatingVectorImmediate = bits & 0xbf20fc00;
  if (floatingVectorImmediate == 0x0f00f400 ||
      floatingVectorImmediate == 0x2f00f400) {
    auto wideElement = floatingVectorImmediate == 0x2f00f400;
    if (wideElement && vectorBits != 128) {
      throw GuestError("Invalid 64-bit vector floating-point immediate arrangement");
    }
    auto immediate = (field(16, 3) << 5) | field(5, 5);
    auto fraction = wideElement ? 52u : 23u;
    auto exponentBits = wideElement ? 11u : 8u;
    auto repeated = (immediate >> 6) & 1;
    auto exponent = ((repeated ^ 1) << (exponentBits - 1)) |
                    (repeated ? ((uint64_t(1) << (exponentBits - 1)) - 4) : 0) |
                    ((immediate >> 4) & 3);
    auto value = (uint64_t(immediate >> 7) << (fraction + exponentBits)) |
                 (exponent << fraction) |
                 (uint64_t(immediate & 15) << (fraction - 4));
    return assignVector((std::string("vector_dup(") + str(value) + ", " +
                         str(wideElement ? 64 : 32) + ", " + str(vectorBits) +
                         ")"));
  }
  if ((bits & 0x9f80fc00) == 0x0f00a400) {
    auto immediate = int64_t(field(16, 7));
    if (!((immediate >= 8 && immediate < 64))) {
      throw GuestError((std::string("Invalid widening shift immediate")));
    }
    auto width = 1 << (63 - __builtin_clzll(immediate));
    auto shift = immediate - width;
    auto offset = vectorBits == 128 ? 64 / width : 0;
    auto sign = field(29, 1) == 0 ? uint64_t(1) << (width - 1) : 0;
    return emit(
        (std::string("\n            { ulong2 source = ") + str(vector(rn)) +
         ", result = ulong2(0);\n              for (uint lane = 0; lane < " +
         str(64 / width) +
         "; ++lane) {\n                  ulong value = lane_read(source, lane "
         "+ " +
         str(offset) + ", " + str(width) +
         ");\n                  ulong extended = (value ^ " + str(sign) +
         "ul) - " + str(sign) +
         "ul;\n                  lane_add(result, lane, " + str(2 * width) +
         ", extended << " + str(shift) +
         ");\n              }\n              x[" + str(806 + 2 * rd) +
         "] = result[0]; x[" + str(807 + 2 * rd) +
         "] = result[1]; }\n            "));
  }
  if ((bits & 0xbf20fc00) == 0x2e20c000) {
    auto width = 8 << field(22, 2);
    return assignVector((std::string("vector_long(ulong2(0), ") +
                         str(vector(rn)) + ", " + str(vector(field(16, 5))) +
                         ", " + str(width) + ", " + str(vectorBits) +
                         ", false, false, false)"));
  }
  if ((bits & 0xbf20fc00) == 0x2e208000) {
    auto width = 8 << field(22, 2);
    auto top = field(30, 1) == 1;
    return assignVector(
        (std::string("vector_long(") + str(vector(rd)) + ", " +
         str(vector(rn)) + ", " + str(vector(field(16, 5))) + ", " +
         str(width) + ", " + str(vectorBits) + ", true, " +
         str(top ? (std::string("true")) : (std::string("false"))) +
         ", false)"));
  }
  if ((bits & 0xbf3ffc00) == 0x2e303800) {
    auto sourceWidth = 8 << field(22, 2);
    auto destinationWidth = sourceWidth * 2;
    return assignVector((std::string("vector_reduce_long(") + str(vector(rn)) +
                         ", " + str(sourceWidth) + ", " +
                         str(destinationWidth) + ", " + str(vectorBits) + ")"));
  }
  if ((bits & 0xbf20fc00) == 0x2e200000) {
    auto width = 8 << field(22, 2);
    return assignVector((std::string("vector_add_long(") + str(vector(rn)) +
                         ", " + str(vector(field(16, 5))) + ", " + str(width) +
                         ", " + str(vectorBits) + ", false)"));
  }
  if ((bits & 0xbf20fc00) == 0x0e003800) {
    auto width = 8 << field(22, 2);
    return assignVector((std::string("vector_zip1(") + str(vector(rn)) + ", " +
                         str(vector(field(16, 5))) + ", " + str(width) + ", " +
                         str(vectorBits) + ")"));
  }
  if ((bits & 0xff20fc00) == 0x2f20a000) {
    auto width = 8 << field(22, 2);
    auto lane = (field(11, 1) << 1) | field(21, 1);
    return assignVector((std::string("vector_long(ulong2(0), ") +
                         str(vector(rn)) + ", vector_dup(lane_read(" +
                         str(vector(field(16, 5))) + ", " + str(lane) + ", " +
                         str(width) + "), " + str(width) + ", " +
                         str(vectorBits) + "), " + str(width) + ", " +
                         str(vectorBits) + ", false, false, false)"));
  }
  if ((bits & 0xff20fc00) == 0x2f004000) {
    auto width = 8 << field(22, 2);
    auto lane = field(11, 1) | (field(21, 1) << 1);
    return assignVector((std::string("vector_mla(") + str(vector(rd)) + ", " +
                         str(vector(rn)) + ", vector_dup(lane_read(" +
                         str(vector(field(16, 5))) + ", " + str(lane) + ", " +
                         str(width) + "), " + str(width) + ", " +
                         str(vectorBits) + "), " + str(width) + ", " +
                         str(vectorBits) + ", true)"));
  }
  if ((bits & 0xffe08400) == 0x6e000400) {
    auto immediate = field(16, 5);
    auto sourceImmediate = field(11, 4);
    if (!(immediate != 0 && ctz(immediate) < 4)) {
      throw GuestError((std::string("Invalid SIMD INS lane encoding")));
    }
    auto width = 8 << ctz(immediate);
    auto destinationLane = int64_t(immediate >> (ctz(immediate) + 1));
    auto sourceLane = int64_t(sourceImmediate >> ctz(immediate));
    auto destinationBit = destinationLane * width,
         sourceBit = sourceLane * width;
    auto dslot = int64_t(806 + 2 * rd) + destinationBit / 64;
    auto sslot = int64_t(806 + 2 * rn) + sourceBit / 64;
    auto mask = width == 64 ? UINT64_MAX : (uint64_t(1) << width) - 1;
    return emit((std::string("x[") + str(dslot) + "] = (x[" + str(dslot) +
                 "] & ~(" + str(mask) + "ul << " + str(destinationBit % 64) +
                 ")) | ((x[" + str(sslot) + "] >> " + str(sourceBit % 64) +
                 " & " + str(mask) + "ul) << " + str(destinationBit % 64) +
                 ");"));
  }
  if ((bits & 0xbfe0fc00) == 0x0e002c00) {
    auto immediate = field(16, 5);
    auto wide = field(30, 1) != 0;
    if (!(immediate != 0 && ctz(immediate) < (wide ? 3 : 2))) {
      throw GuestError((std::string("Invalid SMOV lane encoding")));
    }
    auto width = 8 << ctz(immediate);
    auto lane = immediate >> (ctz(immediate) + 1);
    auto sign = uint64_t(1) << (width - 1);
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    return rd == 31 ? emit()
                    : emit((std::string(str(reg(rd))) + " = " + str(type) +
                            "((lane_read(" + str(vector(rn)) + ", " +
                            str(lane) + ", " + str(width) + ") ^ " + str(sign) +
                            "ul) - " + str(sign) + "ul);"));
  }
  if ((bits & 0xbf20fc00) == 0x0e003c00) {
    auto immediate = field(16, 5);
    if (!immediate || ctz(immediate) >= 4)
      throw GuestError("Invalid UMOV lane encoding");
    auto width = 8 << ctz(immediate);
    auto lane = immediate >> (ctz(immediate) + 1);
    auto sourceBit = int64_t(lane) * width;
    auto value =
        (std::string("((x[") + str(806 + 2 * rn + uint32_t(sourceBit / 64)) +
         "] >> " + str(sourceBit % 64) + ") & " +
         str(width == 64
                 ? (std::string("~0ul"))
                 : (std::string(str((uint64_t(1) << width) - 1)) + "ul")) +
         ")");
    return emit(rd == 31 ? (std::string(""))
                         : (std::string(str(reg(rd))) + " = " +
                            str(width == 64 ? (std::string("ulong"))
                                            : (std::string("uint"))) +
                            str(value) + ";"));
  }
  auto scalarCopy = (bits & 0xffe0fc00) == 0x5e000400;
  if (scalarCopy || (bits & 0xbfe0fc00) == 0x0e000400) {
    auto immediate = field(16, 5);
    if (!(immediate != 0 && ctz(immediate) < 4)) {
      throw GuestError((std::string("Invalid SIMD lane copy")));
    }
    auto width = 8 << ctz(immediate);
    if (!(scalarCopy || width < 64 || vectorBits == 128)) {
      throw GuestError((std::string("Invalid vector DUP arrangement")));
    }
    auto lane = immediate >> (ctz(immediate) + 1);
    auto value = (std::string("lane_read(") + str(vector(rn)) + ", " +
                  str(lane) + ", " + str(width) + ")");
    return assignVector(scalarCopy
                            ? (std::string("ulong2(") + str(value) + ", 0ul)")
                            : (std::string("vector_dup(") + str(value) + ", " +
                               str(width) + ", " + str(vectorBits) + ")"));
  }
  if ((bits & 0xbfe0fc00) == 0x0e000c00) {
    auto immediate = field(16, 5);
    if (!(immediate != 0 && ctz(immediate) < 4)) {
      throw GuestError((std::string("Invalid vector DUP element size")));
    }
    auto width = 8 << ctz(immediate);
    if (!(width < 64 || vectorBits == 128)) {
      throw GuestError((std::string("Invalid vector DUP arrangement")));
    }
    return assignVector((std::string("vector_dup(") + str(reg(rn)) + ", " +
                         str(width) + ", " + str(vectorBits) + ")"));
  }
  switch (bits & 0xffffffe0) {
  case 0xd53b4200:
    return emit(rd == 31 ? "" : reg(rd) + " = (ulong(n) << 31) | (ulong(z) << 30) | (ulong(c) << 29) | (ulong(v) << 28);");
  case 0xd51b4200:
    return emit("n = (" + reg(rd) + " & (1ul << 31)) != 0; z = (" +
                reg(rd) + " & (1ul << 30)) != 0; c = (" + reg(rd) +
                " & (1ul << 29)) != 0; v = (" + reg(rd) +
                " & (1ul << 28)) != 0;");
  case 0xd53b4400:
  case 0xd53b4420: {
    auto slot = (bits & 32) == 0 ? guestFPCR : guestFPSR;
    return emit(rd == 31 ? "" : reg(rd) + " = x[" + str(slot) + "];");
  }
  case 0xd51b4400:
  case 0xd51b4420: {
    bool fpcr = (bits & 32) == 0;
    return emit("x[" + str(fpcr ? guestFPCR : guestFPSR) + "] = " + reg(rd) +
                " & " + (fpcr ? "0x03c00000ul" : "0xf800009ful") + ";");
  }
  default:
    break;
  }
  if ((bits & 0x7f3efc00) == 0x1e220000) {
    auto wide = field(31, 1) == 1;
    auto unsignedValue = field(16, 1) == 1;
    auto doubleValue = field(22, 1) == 1;
    auto source =
        wide ? reg(rn)
             : (unsignedValue
                    ? (std::string("ulong(uint(") + str(reg(rn)) + "))")
                    : (std::string("ulong(long(int(") + str(reg(rn)) + ")))"));
    auto negative = unsignedValue
                        ? (std::string("false"))
                        : (std::string("(long(") + str(source) + ") < 0)");
    return assignVector((std::string("ulong2(integer_to_float(") + str(source) +
                         ", " + str(negative) + ", " +
                         str(doubleValue ? 52 : 23) + "u, x), 0ul)"));
  }
  if ((bits & 0xff3ffc00) == 0x1e24c000) {
    const auto format = field(22, 2);
    if (format > 1) {
      throw GuestError("Unsupported scalar FRINTP format");
    }
    const auto source = "x[" + str(806 + 2 * rn) + "]";
    return assignVector("ulong2(fp_round_positive(" + source + ", " +
                         (format == 1 ? "true" : "false") + ", x), 0ul)");
  }
  if ((bits & 0xff20fc00) == 0x1e202000 && (bits & 7) == 0) {
    const auto format = field(22, 2);
    const bool zero = field(3, 1) != 0;
    if (format > 1 || (zero && field(16, 5) != 0)) {
      throw GuestError("Unsupported scalar floating-point compare encoding");
    }
    const auto left = "x[" + str(806 + 2 * rn) + "]";
    const auto right = zero ? "0ul" : "x[" + str(806 + 2 * field(16, 5)) + "]";
    const auto flags = "fp_compare(" + left + ", " + right + ", " +
                       (format == 1 ? "true" : "false") + ", " +
                       (field(4, 1) != 0 ? "true" : "false") + ", x)";
    return emit("{ uint fp_flags = " + flags + "; n = (fp_flags & 8u) != 0; " +
                 "z = (fp_flags & 4u) != 0; c = (fp_flags & 2u) != 0; " +
                 "v = (fp_flags & 1u) != 0; }");
  }
  if ((bits & 0xff20fc00) == 0x1e201800) {
    auto format = field(22, 2);
    if (format > 1) {
      throw GuestError("Unsupported scalar floating-point divide format");
    }
    auto left = "x[" + str(806 + 2 * rn) + "]";
    auto right = "x[" + str(806 + 2 * field(16, 5)) + "]";
    return assignVector("ulong2(fp_divide(" + left + ", " + right + ", " +
                         (format == 1 ? "true" : "false") + ", x), 0ul)");
  }
  if ((bits & 0xff201fe0) == 0x1e201000) {
    const auto format = field(22, 2);
    if (format > 1)
      throw GuestError("Unsupported floating-point immediate format");
    const uint64_t immediate = field(13, 8);
    const unsigned fraction = format == 1 ? 52 : 23;
    const unsigned exponentBits = format == 1 ? 11 : 8;
    const uint64_t repeated = (immediate >> 6) & 1;
    // VFPExpandImm: inverted exponent top bit, repeated bit 6, then bits 5:4.
    const uint64_t exponent =
        ((repeated ^ 1) << (exponentBits - 1)) |
        (repeated ? ((uint64_t(1) << (exponentBits - 1)) - 4) : 0) |
        ((immediate >> 4) & 3);
    const uint64_t value = ((immediate >> 7) << (fraction + exponentBits)) |
                           (exponent << fraction) |
                           ((immediate & 15) << (fraction - 4));
    return assignVector("ulong2(" + str(value) + "ul, 0ul)");
  }
  if ((bits & 0xff3ffc00) == 0x1e214000) {
    const auto format = field(22, 2);
    if (format > 1)
      throw GuestError("Unsupported scalar FNEG format");
    const auto source = "x[" + str(806 + 2 * rn) + "]";
    const auto value = format == 1
                           ? "(" + source + " ^ 0x8000000000000000ul)"
                           : "ulong(uint(" + source + ") ^ 0x80000000u)";
    return assignVector("ulong2(" + value + ", 0ul)");
  }
  auto vectorMove = bits & 0xfffffc00;
  if (contains({0x1e260000, 0x1e270000, 0x9e660000, 0x9e670000}, vectorMove)) {
    auto wide = field(31, 1) == 1;
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    if (field(16, 1) == 0) {
      return emit(rd == 31 ? (std::string(""))
                           : (std::string(str(reg(rd))) + " = " + str(type) +
                              "(x[" + str(806 + 2 * rn) + "]);"));
    }
    return assignVector(
        (std::string("ulong2(") + str(type) + "(" + str(reg(rn)) + "), 0ul)"));
  }
  if ((bits & 0xbf3ffc00) == 0x0e31b800) {
    auto width = 8 << field(22, 2);
    if (!(width < 64 && (width != 32 || vectorBits == 128))) {
      throw GuestError((std::string("Invalid vector reduction arrangement")));
    }
    return assignVector((std::string("vector_sum(") + str(vector(rn)) + ", " +
                         str(width) + ", " + str(vectorBits) + ")"));
  }
  if ((bits & 0x9f3efc00) == 0x0e30a800) {
    auto width = 8 << field(22, 2);
    if (!(width < 64 && (width != 32 || vectorBits == 128))) {
      throw GuestError((std::string("Invalid vector min/max arrangement")));
    }
    auto minimum = field(16, 1) == 1;
    auto signedLanes = field(29, 1) == 0;
    return assignVector((std::string("vector_extreme(") + str(vector(rn)) +
                         ", " + str(width) + ", " + str(vectorBits) + ", " +
                         str(minimum) + ", " + str(signedLanes) + ")"));
  }
  const bool vectorEqualZero = (bits & 0xbf3ffc00) == 0x0e209800;
  const bool scalarEqualZero = (bits & 0xff3ffc00) == 0x5e209800;
  if (vectorEqualZero || scalarEqualZero) {
    const auto width = 8 << field(22, 2);
    if ((scalarEqualZero && width != 64) ||
        (vectorEqualZero && width == 64 && vectorBits != 128))
      throw GuestError("Invalid CMEQ zero arrangement");
    const auto count = scalarEqualZero ? 64 : vectorBits;
    return assignVector("vector_compare(" + vector(rn) + ", ulong2(0), " +
                        str(width) + ", " + str(count) +
                        ", false, false, true)");
  }
  auto equalLanes = (bits & 0xbf20fc00) == 0x2e208c00;
  if (equalLanes || (bits & 0x9f20f400) == 0x0e203400) {
    auto width = 8 << field(22, 2);
    if (!(width != 64 || vectorBits == 128)) {
      throw GuestError((std::string("Invalid vector comparison arrangement")));
    }
    auto inclusive = field(11, 1) == 1;
    auto signedLanes = field(29, 1) == 0;
    return assignVector((std::string("vector_compare(") + str(vector(rn)) +
                         ", " + str(vector(field(16, 5))) + ", " + str(width) +
                         ", " + str(vectorBits) + ", " + str(inclusive) + ", " +
                         str(signedLanes) + ", " + str(equalLanes) + ")"));
  }
  auto vectorLogical = bits & 0xbfe0fc00;
  auto operation = Map<std::string>{{0x0e201c00, "&"},
                                    {0x0e601c00, "&"},
                                    {0x0ea01c00, "|"},
                                    {0x0ee01c00, "|"},
                                    {0x2e201c00, "^"}}[vectorLogical];
  if (operation) {
    auto right = vector(field(16, 5));
    auto inverted =
        contains({0x0e601c00, 0x0ee01c00}, vectorLogical) ? "~" : "";
    auto mask = vectorBits == 128 ? "" : " & ulong2(~0ul, 0ul)";
    return assignVector("(" + vector(rn) + " " + str(operation) + " " +
                        inverted + right + ")" + mask);
  }
  auto vectorOperation = bits & 0xbf20fc00;
  if (contains({0x2e204400, 0x0e001800, 0x0e208400, 0x1e208400, 0x2e208400,
                0x0e209400, 0x2e209400, 0x0e209c00},
               vectorOperation)) {
    auto width = 8 << field(22, 2);
    if (!(width < 64 || vectorBits == 128 || vectorOperation == 0x1e208400)) {
      throw GuestError((std::string("Invalid vector arrangement")));
    }
    auto arguments =
        (std::string(str(vector(rn))) + ", " + str(vector(field(16, 5))) +
         ", " + str(width) + ", " + str(vectorBits));
    if (vectorOperation == 0x0e208400 || vectorOperation == 0x1e208400 ||
        vectorOperation == 0x2e208400) {
      return assignVector(
          (std::string("vector_add(") + str(arguments) + ", " +
           str(vectorOperation == 0x2e208400 ? (std::string("true"))
                                             : (std::string("false"))) +
           ")"));
    }
    if (contains({0x0e209400, 0x2e209400, 0x0e209c00}, vectorOperation)) {
      if (!(width < 64)) {
        throw GuestError(
            (std::string("Invalid vector multiply-accumulate arrangement")));
      }
      auto accumulator = vectorOperation == 0x0e209c00
                             ? (std::string("ulong2(0)"))
                             : vector(rd);
      return assignVector(
          (std::string("vector_mla(") + str(accumulator) + ", " +
           str(arguments) + ", " +
           str(vectorOperation == 0x2e209400 ? (std::string("true"))
                                             : (std::string("false"))) +
           ")"));
    }
    auto helper = vectorOperation == 0x2e204400 ? (std::string("vector_ushl"))
                                                : (std::string("vector_uzp1"));
    return assignVector(
        (std::string(str(helper)) + "(" + str(arguments) + ")"));
  }
  auto immediateShift = bits & 0xbf80fc00;
  if ((contains({0x0f005400, 0x1f005400, 0x2f000400, 0x2f001400},
                immediateShift)) &&
      (field(16, 7) >= 8)) {
    auto encoded = int64_t(field(16, 7));
    auto width = 1 << (63 - __builtin_clzll(encoded));
    if (!(width < 64 || vectorBits == 128 || immediateShift == 0x1f005400)) {
      throw GuestError((std::string("Invalid vector shift arrangement")));
    }
    auto shift = immediateShift == 0x0f005400 || immediateShift == 0x1f005400
                     ? encoded - width
                     : 2 * width - encoded;
    auto accumulator =
        immediateShift == 0x2f001400 ? vector(rd) : (std::string("ulong2(0)"));
    return assignVector(
        (std::string("vector_shift(") + str(accumulator) + ", " +
         str(vector(rn)) + ", " + str(width) + ", " + str(vectorBits) + ", " +
         str(shift) + ", " +
         str(immediateShift == 0x0f005400 || immediateShift == 0x1f005400
                 ? (std::string("true"))
                 : (std::string("false"))) +
         ")"));
  }
  if ((bits & 0xbf800c00) == 0x2f000400) {
    auto mode = field(12, 4);
    if (contains({1, 3, 5, 7, 9, 11}, mode)) {
      auto width = mode < 8 ? 32 : 16;
      auto shift = int64_t((mode & 7) >> 1) * 8;
      auto immediate = (field(16, 3) << 5) | field(5, 5);
      auto mask = vectorBits == 128 ? (std::string("ulong2(~0ul)"))
                                    : (std::string("ulong2(~0ul, 0ul)"));
      return assignVector(
          (std::string("(") + str(vector(rd)) + " & ~vector_dup(" +
           str(uint64_t(immediate) << shift) + "ul, " + str(width) + ", " +
           str(vectorBits) + ")) & " + str(mask)));
    }
  }
  if ((bits & 0xbf800c00) == 0x0f000400) {
    auto mode = field(12, 4);
    if (contains({0, 2, 4, 6, 8, 10, 14}, mode)) {
      auto width = mode < 8 ? 32 : (mode == 14 ? 8 : 16);
      auto shift = mode == 14 ? 0 : int64_t((mode & 7) >> 1) * 8;
      auto immediate = uint64_t((field(16, 3) << 5) | field(5, 5)) << shift;
      return assignVector((std::string("vector_dup(") + str(immediate) +
                           "ul, " + str(width) + ", " + str(vectorBits) + ")"));
    }
  }
  if ((bits & 0xbff8fc00) == 0x2f00e400) {
    auto immediate = (field(16, 3) << 5) | field(5, 5);
    uint64_t value = 0;
    for (int byte = 0; byte < 8; ++byte)
      if ((immediate & (1 << byte)) != 0) {
        value |= uint64_t(255) << (byte * 8);
      }
    return assignVector((std::string("ulong2(") + str(value) + "ul, " +
                         str(vectorBits == 128 ? value : 0) + "ul)"));
  }
  // Each dispatch has one active guest thread. These operations are indivisible
  // at the scheduler boundary and shared buffers are synchronized on dispatch
  // completion.
  auto atomicOperation =
      Map<std::string>{{0x38200000, (std::string("+"))},
                       {0x38201000, (std::string("& ~"))},
                       {0x38202000, (std::string("^"))},
                       {0x38203000, (std::string("|"))}}[bits & 0x3f20fc00];
  auto atomicSwap = (bits & 0x3f20fc00) == 0x38208000;
  auto atomicCompare = (bits & 0x3fa07c00) == 0x08a07c00;
  if ((bits & 0xbfa07c00) == 0x08207c00) {
    auto compare = field(16, 5);
    if (!((compare % 2 == 0) && (rd % 2 == 0))) {
      throw GuestError((std::string("Invalid CASP regIndex pair")));
    }
    auto width = field(30, 1) == 0 ? 4 : 8;
    auto count = 2 * width;
    auto type = width == 4 ? (std::string("uint")) : (std::string("ulong"));
    auto pointer = physical((std::string("address")), count, false);
    auto secondResult =
        compare == 30 ? (std::string(""))
                      : (std::string(str(reg(compare + 1))) + " = oldHigh;");
    return emit((
        std::string("\n            { ulong address = ") + str(reg(rn, true)) +
        ";\n              if ((address & " + str(count - 1) + "ul) || " +
        str(outside((std::string("address")), count, false)) +
        ") {\n                  x[" + str(guestFaultAddress) +
        "] = address; x[34] = 2; return;\n              }\n              "
        "device uchar *pair = memory + " +
        str(pointer) +
        ";\n              ulong oldLow = load_guest_word(pair, " + str(width) +
        ");\n              ulong oldHigh = load_guest_word(pair + " +
        str(width) + ", " + str(width) + ");\n              if (oldLow == " +
        str(type) + "(" + str(reg(compare)) + ") && oldHigh == " + str(type) +
        "(" + str(reg(compare + 1)) +
        ")) {\n                  store_guest_word(pair, " + str(reg(rd)) +
        ", " + str(width) + ");\n                  store_guest_word(pair + " +
        str(width) + ", " + str(reg(rd + 1)) + ", " + str(width) +
        ");\n              }\n              " + str(reg(compare)) +
        " = oldLow;\n              " + str(secondResult) + " }\n            "));
  }
  auto ordered = (bits & 0x3fbffc00) == 0x089ffc00;
  auto rcpcAcquire = (bits & 0x3ffffc00) == 0x38bfc000;
  if (atomicOperation != nullptr || atomicSwap || atomicCompare || ordered ||
      rcpcAcquire) {
    auto width = 1 << field(30, 2);
    auto load = rcpcAcquire || (ordered && field(22, 1) == 1);
    auto address = reg(rn, true);
    auto source = reg(field(16, 5));
    auto mask = width == 8 ? UINT64_MAX : (uint64_t(1) << (width * 8)) - 1;
    auto pointer = physical((std::string("address")), width, load);
    auto bound =
        (std::string("if ((address & ") + str(width - 1) + "ul) || " +
         str(outside((std::string("address")), width, load)) + ") { x[" +
         str(guestFaultAddress) + "] = address; x[33] = " + str(pc) +
         "ul; x[34] = 2; return; }");
    auto get = (std::string("ulong old = load_guest_word(memory + ") +
                str(pointer) + ", " + str(width) + ");");
    auto store = [&](std::string value) -> std::string {
      return (std::string("store_guest_word(memory + ") + str(pointer) + ", " +
              str(value) + ", " + str(width) + ");");
    };
    auto save = [&](uint32_t regIndex) -> std::string {
      return regIndex == 31 ? (std::string(""))
                            : (std::string(str(reg(regIndex))) + " = old;");
    };
    Strings operation;
    if (atomicOperation) {
      operation = Strings{get,
                          store((std::string("old ") + str(atomicOperation) +
                                 " " + str(source))),
                          save(rd)};
    } else if (atomicSwap) {
      operation = Strings{get, store(source), save(rd)};
    } else if (atomicCompare) {
      operation = Strings{get,
                          (std::string("if (old == (") + str(source) + " & " +
                           str(mask) + "ul)) { " + str(store(reg(rd))) + " }"),
                          save(field(16, 5))};
    } else if (load) {
      operation = Strings{get, save(rd)};
    } else {
      operation = Strings{store(reg(rd))};
    }
    return make_instruction(
        Strings{(std::string("{ ulong address = ") + str(address) + ";"),
                bound} +
            operation + Strings{(std::string("}"))},
        PCs{next});
  }
  auto registers =
      Map<int>{{0x7, 1}, {0xa, 2}, {0x6, 3}, {0x2, 4}}[field(12, 4)];
  if ((bits & 0xbf200000) == 0x0c000000 && registers) {
    auto post = field(23, 1) != 0;
    auto index = field(16, 5);
    if (!(post || index == 0)) {
      throw GuestError((std::string("Invalid SIMD structure address")));
    }
    auto width = field(30, 1) == 0 ? 8 : 16;
    auto bytes = registers * width;
    auto load = field(22, 1) != 0;
    auto base = reg(rn, true);
    auto pointer = physical((std::string("address")), bytes, load);
    auto code =
        Strings{(std::string("{ ulong address = ") + str(base) + ";"),
                (std::string("if (") +
                 str(outside((std::string("address")), bytes, load)) +
                 ") { x[33] = " + str(pc) + "ul; x[34] = 2; return; }")};
    for (int index = 0; index < registers; ++index) {
      auto slot = 806 + 2 * ((int64_t(rd) + index) & 31);
      auto start = (std::string("memory + ") + str(pointer) + " + " +
                    str(index * width));
      if (load) {
        code.push_back((std::string("x[") + str(slot) + "] = load_guest_word(" +
                        str(start) + ", 8);"));
        auto high =
            width == 16
                ? (std::string("load_guest_word(") + str(start) + " + 8, 8)")
                : (std::string("0"));
        code.push_back(
            (std::string("x[") + str(slot + 1) + "] = " + str(high) + ";"));
      } else {
        code.push_back((std::string("store_guest_word(") + str(start) + ", x[" +
                        str(slot) + "], 8);"));
        if (width == 16) {
          code.push_back((std::string("store_guest_word(") + str(start) +
                          " + 8, x[" + str(slot + 1) + "], 8);"));
        }
      }
    }
    if (post) {
      code.push_back(
          (std::string(str(base)) + " += " +
           str(index == 31 ? (std::string(str(bytes)) + "ul") : reg(index)) +
           ";"));
    }
    return make_instruction(code + Strings{(std::string("}"))}, PCs{next});
  }
  auto vectorPair = bits & 0x3fc00000;
  if ((contains({0x2d000000, 0x2d400000, 0x2d800000, 0x2dc00000, 0x2c800000,
                 0x2cc00000},
                vectorPair)) &&
      (field(30, 2) < 3)) {
    auto width = 4 << field(30, 2);
    auto base = reg(rn, true);
    auto offset = sign_extend(field(15, 7), 7) * width;
    auto second = field(10, 5);
    auto mode = field(23, 2);
    auto load = field(22, 1) == 1;
    if (!(!load || rd != second)) {
      throw GuestError((std::string("Unpredictable vector pair overlap")));
    }
    auto address =
        mode == 1 ? base
                  : (std::string(str(base)) + " + ulong(" + str(offset) + ")");
    auto transfer = [&](uint32_t regIndex, int64_t displacement) -> Strings {
      auto first = 806 + 2 * regIndex;
      auto pointer = (std::string("memory + ") +
                      str(physical((std::string("address")), 2 * width, load)) +
                      " + " + str(displacement));
      auto count = std::min<int64_t>(8, width);
      if (load) {
        auto upper =
            width == 16
                ? (std::string("load_guest_word(") + str(pointer) + " + 8, 8)")
                : (std::string("0"));
        return Strings{
            (std::string("x[") + str(first) + "] = load_guest_word(" +
             str(pointer) + ", " + str(count) + ");"),
            (std::string("x[") + str(first + 1) + "] = " + str(upper) + ";")};
      }
      auto lines = Strings{(std::string("store_guest_word(") + str(pointer) +
                            ", x[" + str(first) + "], " + str(count) + ");")};
      if (width == 16) {
        lines.push_back((std::string("store_guest_word(") + str(pointer) +
                         " + 8, x[" + str(first + 1) + "], 8);"));
      }
      return lines;
    };
    return make_instruction(
        Strings{(std::string("{ ulong address = ") + str(address) + ";"),
                (std::string("if (") +
                 str(outside((std::string("address")), 2 * width, load)) +
                 ") { x[33] = " + str(pc) + "ul; x[34] = 2; return; }")} +
            transfer(rd, 0) + transfer(second, width) +
            Strings{mode == 2 ? (std::string(""))
                              : (std::string(str(base)) + " += ulong(" +
                                 str(offset) + ");"),
                    (std::string("}"))},
        PCs{next});
  }
  auto indexedOffset = [&](int64_t width) -> std::string {
    auto source = reg(field(16, 5));
    std::string offset;
    switch (field(13, 3)) {
    case 2:
      offset = (std::string("ulong(uint(") + str(source) + "))");
      break;
    case 6:
      offset = (std::string("ulong(long(int(") + str(source) + ")))");
      break;
    default:
      offset = source;
      break;
    }
    auto scale = field(12, 1) == 1 ? ctz(width) : 0;
    return (std::string("(") + str(offset) + " << " + str(scale) + ")");
  };
  if ((bits & 0x3f000000) == 0x18000000 && field(30, 2) != 3) {
    const auto operation = field(30, 2);
    const int width = operation == 1 ? 8 : 4;
    const int64_t address = pc + sign_extend(field(5, 19), 19) * 4;
    if (address < 0)
      throw GuestError("Invalid scalar literal load address");
    const auto logical = str(address) + "ul";
    const auto pointer = physical(logical, width, true);
    const auto loaded =
        "load_guest_word(memory + " + pointer + ", " + str(width) + ")";
    const auto value =
        operation == 2 ? "ulong(long(int(" + loaded + ")))" : loaded;
    return emit("if (" + outside(logical, width, true) + ") { x[" +
                    str(guestFaultAddress) + "] = " + logical +
                    "; x[34] = 2; return; }",
                rd == 31 ? "" : reg(rd) + " = " + value + ";");
  }
  if ((bits & 0x3f000000) == 0x1c000000) {
    auto width = 4 << field(30, 2);
    auto address = pc + sign_extend(field(5, 19), 19) * 4;
    if (!(width <= 16 && address >= 0)) {
      throw GuestError((std::string("Invalid SIMD literal load")));
    }
    auto logical = (std::string(str(address)) + "ul");
    auto pointer = physical(logical, width, true);
    auto upper = width == 16
                     ? (std::string("load_guest_word(memory + offset + 8, 8)"))
                     : (std::string("0ul"));
    auto slot = 806 + 2 * int64_t(rd);
    return emit(
        (std::string("{ ulong offset = ") + str(pointer) + ";"),
        (std::string("if (") + str(outside(logical, width, true)) +
         ") { x[34] = 2; return; }"),
        (std::string("x[") + str(slot) +
         "] = load_guest_word(memory + offset, " +
         str(std::min<int64_t>(width, 8)) + ");"),
        (std::string("x[") + str(slot + 1) + "] = " + str(upper) + "; }"));
  }
  auto vectorMemory = bits & 0xff800000;
  auto vectorWidth =
      Map<int>{{0x3d800000, 16}, {0xfd000000, 8}, {0xbd000000, 4}};
  auto unscaledWidths =
      Map<int>{{0x3c800000, 16}, {0xfc000000, 8}, {0xbc000000, 4}};
  auto unscaledWidth =
      field(10, 2) != 2 ? unscaledWidths[bits & 0xffa00000] : nullptr;
  auto indexedVectorWidths = Map<int>{{0x3ca00800, 16},
                                      {0xfc200800, 8},
                                      {0xbc200800, 4},
                                      {0x7c200800, 2},
                                      {0x3c200800, 1}};
  auto indexedVectorWidth = contains({2, 3, 6, 7}, field(13, 3))
                                ? indexedVectorWidths[bits & 0xffa00c00]
                                : nullptr;
  auto vectorUnscaled = unscaledWidth != nullptr;
  if (auto width =
          first(indexedVectorWidth, unscaledWidth, vectorWidth[vectorMemory]);
      width) {
    auto load = field(22, 1) == 1;
    auto base = reg(rn, true);
    auto mode = vectorUnscaled ? field(10, 2) : 0;
    auto displacement = vectorUnscaled ? sign_extend(field(12, 9), 9)
                                       : int64_t(field(10, 12)) * width;
    auto offset = indexedVectorWidth == nullptr
                      ? (std::string("ulong(") + str(displacement) + ")")
                      : indexedOffset(width);
    auto address =
        mode == 1 ? base : (std::string(str(base)) + " + " + str(offset));
    auto first = 806 + 2 * rd;
    auto count = std::min<int64_t>(width, 8);
    auto pointer = physical((std::string("address")), width, load);
    auto upper = width == 16 ? (std::string("load_guest_word(memory + ") +
                                str(pointer) + " + 8, 8)")
                             : (std::string("0"));
    auto operation =
        load
            ? (std::string("x[") + str(first) +
               "] = load_guest_word(memory + " + str(pointer) + ", " +
               str(count) + "); x[" + str(first + 1) + "] = " + str(upper) +
               ";")
            : (std::string("store_guest_word(memory + ") + str(pointer) +
               ", x[" + str(first) + "], " + str(count) + ");") +
                  (width == 16
                       ? (std::string(" store_guest_word(memory + ") +
                          str(pointer) + " + 8, x[" + str(first + 1) + "], 8);")
                       : (std::string("")));
    return emit((std::string("{ ulong address = ") + str(address) + ";"),
                (std::string("if (") +
                 str(outside((std::string("address")), width, load)) +
                 ") { x[34] = 2; return; }"),
                operation,
                mode == 0 ? (std::string(""))
                          : (std::string(str(base)) + " += ulong(" +
                             str(displacement) + ");"),
                (std::string("}")));
  }
  auto pair = bits & 0xffc00000;
  auto pairEncoding = pair & 0x7fffffff;
  const bool signedPair = contains({0x69400000, 0x69c00000, 0x68c00000}, pair);
  if (signedPair || (contains({0x29000000, 0x29400000, 0x29800000, 0x29c00000,
                               0x28800000, 0x28c00000},
                              pairEncoding) &&
                     field(30, 1) == 0)) {
    auto wide = field(31, 1) == 1;
    auto width = wide ? 8 : 4;
    auto type = wide ? (std::string("ulong")) : (std::string("uint"));
    auto base = reg(rn, true);
    auto second = field(10, 5);
    auto load = field(22, 1) == 1;
    auto mode = field(23, 2);
    if (!((!load || rd != second) &&
          (rn == 31 || mode == 2 || (rn != rd && rn != second)))) {
      throw GuestError((std::string("Unpredictable pair register overlap")));
    }
    auto offset = sign_extend(field(15, 7), 7) * width;
    auto address =
        mode == 1
            ? base
            : (std::string("(") + str(base) + " + ulong(" + str(offset) + "))");
    auto bounds =
        (std::string("if (") +
         str(outside((std::string("address")), 2 * width, load)) + ") { x[" +
         str(guestFaultAddress) + "] = address; x[34] = 2; return; }");
    auto transfer = [&](uint32_t regIndex, int64_t offset) -> std::string {
      auto pointer = (std::string("memory + ") +
                      str(physical((std::string("address")), 2 * width, load)) +
                      " + " + str(offset));
      if (load) {
        const std::string value =
            "load_guest_word(" + pointer + ", " + str(width) + ")";
        return regIndex == 31
                   ? ""
                   : reg(regIndex) + " = " +
                         (signedPair ? "ulong(long(int(" + value + ")))"
                                     : value) +
                         ";";
      }
      return (std::string("store_guest_word(") + str(pointer) + ", " +
              str(type) + "(" + str(reg(regIndex)) + "), " + str(width) + ");");
    };
    auto firstAccess = transfer(rd, 0);
    auto secondAccess = transfer(second, width);
    return emit((std::string("{ ulong address = ") + str(address) + ";"),
                bounds, firstAccess, secondAccess,
                mode == 2 ? (std::string(""))
                          : (std::string(str(base)) + " += ulong(" +
                             str(offset) + ");"),
                (std::string("}")));
  }
  auto indexed = bits & 0xffe00c00;
  auto indexedWidths = Map<int>{
      {0x38600800, 1}, {0x78600800, 2}, {0xb8600800, 4}, {0xf8600800, 8},
      {0x38200800, 1}, {0x78200800, 2}, {0xb8200800, 4}, {0xf8200800, 8},
      {0x38a00800, 1}, {0x38e00800, 1}, {0x78a00800, 2}, {0x78e00800, 2},
      {0xb8a00800, 4}};
  if (auto width = indexedWidths[indexed];
      width && contains({2, 3, 6, 7}, field(13, 3))) {
    auto signExtend = field(23, 1) == 1;
    auto load = signExtend || field(22, 1) == 1;
    auto address = (std::string("(") + str(reg(rn, true)) + " + " +
                    str(indexedOffset(width)) + ")");
    auto pointer = physical(address, width, load);
    auto value = (std::string("load_guest_word(memory + ") + str(pointer) +
                  ", " + str(width) + ")");
    auto signedType = width == 1 ? (std::string("char"))
                                 : (width == 2 ? (std::string("short"))
                                               : (std::string("int")));
    auto destination = field(22, 1) == 1 ? (std::string("uint(int"))
                                         : (std::string("ulong(long"));
    auto result = signExtend ? (std::string(str(destination)) + "(" +
                                str(signedType) + "(" + str(value) + ")))")
                             : value;
    auto operation =
        load ? (rd == 31
                    ? (std::string(""))
                    : (std::string(str(reg(rd))) + " = " + str(result) + ";"))
             : (std::string("store_guest_word(memory + ") + str(pointer) +
                ", " + str(reg(rd)) + ", " + str(width) + ");");
    return emit((std::string("if (") + str(outside(address, width, load)) +
                 ") { x[" + str(guestFaultAddress) + "] = " + str(address) +
                 "; x[34] = 2; return; }"),
                operation);
  }
  auto unsignedValue = bits & 0xffc00000;
  auto post = bits & 0xffe00c00;
  auto loads = Map<int>{
      {0x39400000, 1}, {0x79400000, 2}, {0xb9400000, 4}, {0xf9400000, 8}};
  auto stores = Map<int>{
      {0x39000000, 1}, {0x79000000, 2}, {0xb9000000, 4}, {0xf9000000, 8}};
  auto postLoads = Map<int>{{0x38400400, 1}, {0x38400c00, 1}, {0x78400400, 2},
                            {0x78400c00, 2}, {0xb8400400, 4}, {0xb8400c00, 4},
                            {0xf8400400, 8}, {0xf8400c00, 8}};
  auto postStores = Map<int>{{0x38000400, 1}, {0x38000c00, 1}, {0x78000400, 2},
                             {0x78000c00, 2}, {0xb8000400, 4}, {0xb8000c00, 4},
                             {0xf8000400, 8}, {0xf8000c00, 8}};
  auto unscaledLoads = Map<int>{
      {0x38400000, 1}, {0x78400000, 2}, {0xb8400000, 4}, {0xf8400000, 8}};
  auto unscaledStores = Map<int>{
      {0x38000000, 1}, {0x78000000, 2}, {0xb8000000, 4}, {0xf8000000, 8}};
  auto signedLoads = Map<int>{{0x39800000, 1},
                              {0x39c00000, 1},
                              {0x79800000, 2},
                              {0x79c00000, 2},
                              {0xb9800000, 4}};
  auto signedIndexed =
      field(24, 1) == 0 && field(21, 1) == 0 && field(10, 2) != 2
          ? signedLoads[unsignedValue | 0x01000000]
          : nullptr;
  auto signedWidth = first(signedLoads[unsignedValue], signedIndexed);
  if (auto width =
          first(signedWidth, loads[unsignedValue], stores[unsignedValue],
                postLoads[post], postStores[post], unscaledLoads[post],
                unscaledStores[post])) {
    auto load = signedWidth != nullptr || loads[unsignedValue] != nullptr ||
                postLoads[post] != nullptr || unscaledLoads[post] != nullptr;
    auto update = (signedIndexed != nullptr && field(10, 2) != 0) ||
                  postLoads[post] != nullptr || postStores[post] != nullptr;
    auto unscaled = signedIndexed != nullptr ||
                    unscaledLoads[post] != nullptr ||
                    unscaledStores[post] != nullptr;
    if (!(!update || rn == 31 || rn != rd)) {
      throw GuestError(
          (std::string("Unpredictable load/store register overlap")));
    }
    auto base = reg(rn, true);
    auto displacement = sign_extend(field(12, 9), 9);
    auto address =
        update ? (field(11, 1) == 1 ? (std::string("(") + str(base) +
                                       " + ulong(" + str(displacement) + "))")
                                    : base)
               : (unscaled ? (std::string("(") + str(base) + " + ulong(" +
                              str(displacement) + "))")
                           : (std::string("(") + str(base) + " + " +
                              str(field(10, 12) * width) + "ul)"));
    auto pointer = physical(address, int64_t(width), load);
    auto access = (std::string("load_guest_word(memory + ") + str(pointer) +
                   ", " + str(width) + ")");
    auto loaded = access;
    if (signedWidth != nullptr) {
      auto type = width == 1 ? (std::string("char"))
                             : (width == 2 ? (std::string("short"))
                                           : (std::string("int")));
      loaded = field(22, 1) == 1 ? (std::string("uint(int(") + str(type) + "(" +
                                    str(access) + ")))")
                                 : (std::string("ulong(long(") + str(type) +
                                    "(" + str(access) + ")))");
    }
    auto operation =
        load ? (rd == 31
                    ? (std::string(""))
                    : (std::string(str(reg(rd))) + " = " + str(loaded) + ";"))
             : (std::string("store_guest_word(memory + ") + str(pointer) +
                ", " + str(reg(rd)) + ", " + str(width) + ");");
    return emit((std::string("if (") +
                 str(outside(address, int64_t(width), load)) + ") { x[" +
                 str(guestFaultAddress) + "] = " + str(address) +
                 "; x[34] = 2; return; }"),
                operation,
                update ? (std::string(str(base)) + " += ulong(" +
                          str(sign_extend(field(12, 9), 9)) + ");")
                       : (std::string("")));
  }
  return translate_core::decode(bits, pc, (std::string("ul")));
}
} // namespace arm64metal
