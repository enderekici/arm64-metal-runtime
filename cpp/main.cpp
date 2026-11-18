#include "function_runner.hpp"
#include "gpu_interpreter.hpp"
#include "interpreter_assets.hpp"
#include "process_runner.hpp"
#include "process_host.hpp"
#include "shared_cache.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <signal.h>

namespace {

constexpr std::uint32_t encode_add_sub_immediate(bool wide, bool subtract,
                                                 bool setFlags,
                                                 std::uint32_t destination,
                                                 std::uint32_t source,
                                                 std::uint32_t immediate,
                                                 bool shift12) {
  return 0x11000000u | (std::uint32_t(wide) << 31u) |
         (std::uint32_t(subtract) << 30u) | (std::uint32_t(setFlags) << 29u) |
         (std::uint32_t(shift12) << 22u) | (immediate << 10u) |
         (source << 5u) | destination;
}

constexpr auto addXShiftedEncoding =
    encode_add_sub_immediate(true, false, false, 0, 0, 0x123, true);
constexpr auto subXShiftedEncoding =
    encode_add_sub_immediate(true, true, false, 0, 0, 0x123, true);
constexpr auto addWWrapEncoding =
    encode_add_sub_immediate(false, false, false, 0, 0, 1, false);
constexpr auto subWShiftedEncoding =
    encode_add_sub_immediate(false, true, false, 0, 0, 0x123, true);
constexpr auto addsXOverflowEncoding =
    encode_add_sub_immediate(true, false, true, 31, 1, 1, false);
constexpr auto subsXUnderflowEncoding =
    encode_add_sub_immediate(true, true, true, 31, 1, 1, false);
constexpr auto cmpXShiftedEncoding =
    encode_add_sub_immediate(true, true, true, 31, 1, 1, true);
constexpr auto cmnXShiftedEncoding =
    encode_add_sub_immediate(true, false, true, 31, 1, 1, true);
constexpr auto addsWCarryEncoding =
    encode_add_sub_immediate(false, false, true, 31, 1, 1, false);
constexpr auto subsWUnderflowEncoding =
    encode_add_sub_immediate(false, true, true, 31, 1, 1, false);
constexpr auto addXFromSpEncoding =
    encode_add_sub_immediate(true, false, false, 0, 31, 2, true);
constexpr auto subSpShiftedEncoding =
    encode_add_sub_immediate(true, true, false, 31, 31, 1, true);
constexpr auto addWspFromW10Encoding =
    encode_add_sub_immediate(false, false, false, 31, 10, 0, false);
constexpr auto addXFromSpOneEncoding =
    encode_add_sub_immediate(true, false, false, 0, 31, 1, false);
constexpr auto addsWResultEncoding =
    encode_add_sub_immediate(false, false, true, 0, 1, 1, false);
constexpr auto cmpSpZeroEncoding =
    encode_add_sub_immediate(true, true, true, 31, 31, 0, false);

static_assert(addXShiftedEncoding == 0x91448c00u);
static_assert(subXShiftedEncoding == 0xd1448c00u);
static_assert(addWWrapEncoding == 0x11000400u);
static_assert(subWShiftedEncoding == 0x51448c00u);
static_assert(addsXOverflowEncoding == 0xb100043fu);
static_assert(subsXUnderflowEncoding == 0xf100043fu);
static_assert(cmpXShiftedEncoding == 0xf140043fu);
static_assert(cmnXShiftedEncoding == 0xb140043fu);
static_assert(addsWCarryEncoding == 0x3100043fu);
static_assert(subsWUnderflowEncoding == 0x7100043fu);
static_assert(addXFromSpEncoding == 0x91400be0u);
static_assert(subSpShiftedEncoding == 0xd14007ffu);
static_assert(addWspFromW10Encoding == 0x1100015fu);
static_assert(addXFromSpOneEncoding == 0x910007e0u);
static_assert(addsWResultEncoding == 0x31000420u);
static_assert(cmpSpZeroEncoding == 0xf10003ffu);

std::string read_input(const std::string& path) {
  if (path == "-") return std::string(std::istreambuf_iterator<char>(std::cin), {});
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to read input file: " + path);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

std::uint32_t parse_budget(const std::string& text) {
  std::uint32_t budget = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), budget);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || budget == 0 || budget > 1'000'000) {
    throw std::runtime_error("Invalid block budget");
  }
  return budget;
}

std::size_t parse_spawn_count(const std::string& text) {
  std::size_t count = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || count > 32768) {
    throw std::runtime_error("Invalid internal spawn vector size");
  }
  return count;
}

std::vector<std::string> read_spawn_environment(int descriptor) {
  struct DescriptorGuard {
    int value;
    ~DescriptorGuard() { if (value >= 0) (void)::close(value); }
  } guard{descriptor};
  constexpr std::uint32_t magic = 0x4556414dU;
  constexpr std::size_t maximumEntries = 4096;
  constexpr std::size_t maximumBytes = 1U << 20;
  const auto read_exact = [descriptor](void* destination, std::size_t length) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    std::size_t received = 0;
    while (received < length) {
      const auto result = ::read(descriptor, bytes + received, length - received);
      if (result < 0 && errno == EINTR) continue;
      if (result <= 0) throw std::runtime_error("Truncated spawn environment channel");
      received += static_cast<std::size_t>(result);
    }
  };
  std::array<std::uint32_t, 2> header{};
  read_exact(header.data(), sizeof(header));
  if (header[0] != magic || header[1] > maximumEntries) {
    throw std::runtime_error("Invalid spawn environment channel header");
  }
  std::vector<std::string> environment;
  environment.reserve(header[1]);
  std::size_t totalBytes = 0;
  for (std::uint32_t index = 0; index < header[1]; ++index) {
    std::uint32_t length = 0;
    read_exact(&length, sizeof(length));
    if (length > maximumBytes - std::min(totalBytes, maximumBytes)) {
      throw std::runtime_error("Spawn environment channel exceeds its size limit");
    }
    std::string value(length, '\0');
    if (length != 0) read_exact(value.data(), length);
    totalBytes += length;
    environment.push_back(std::move(value));
  }
  std::uint8_t trailing = 0;
  for (;;) {
    const auto result = ::read(descriptor, &trailing, sizeof(trailing));
    if (result < 0 && errno == EINTR) continue;
    if (result != 0) throw std::runtime_error("Unexpected trailing spawn environment bytes");
    break;
  }
  return environment;
}

bool environment_enabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && std::string_view(value) == "1";
}

void print_help() {
  std::cout <<
      "Usage:\n"
      "  arm64-metal launch EXECUTABLE [ARG ...]\n"
      "    Packaged AOT_REQUIRED bundles enable typed AOT by default; set\n"
      "    ARM64_METAL_INTERPRETER=0 to disable or =1 to opt in on source builds.\n"
      "  arm64-metal run BINARY SYMBOL [INPUT.json|-] [BLOCK_BUDGET]\n"
      "  arm64-metal run-stream DYLIB SYMBOL\n"
      "  arm64-metal verify TRUSTED_DYLIB SYMBOL\n"
      "  arm64-metal translate BINARY SYMBOL\n"
      "  arm64-metal inspect-cache INSTALL_NAME SYMBOL\n"
      "  arm64-metal inspect-cache-rebases INSTALL_NAME\n"
      "\n"
      "Input is a JSON array of {\"words\":[1,2,3],\"seed\":7} invocations.\n"
      "Run reads stdin when INPUT is omitted or '-'.\n"
      "Run-stream reads one JSON batch per stdin line and keeps the Metal pipeline warm.\n"
      "Launch executes the guest process on Metal.\n";
}

std::filesystem::path host_executable_path() {
  std::uint32_t capacity = 0;
  (void)_NSGetExecutablePath(nullptr, &capacity);
  std::vector<char> path(capacity);
  if (_NSGetExecutablePath(path.data(), &capacity) != 0) {
    throw std::runtime_error("Unable to resolve arm64-metal executable path");
  }
  std::error_code error;
  auto resolved = std::filesystem::weakly_canonical(path.data(), error);
  if (error) throw std::runtime_error("Unable to canonicalize arm64-metal executable path");
  return resolved;
}

int re_terminate_with_guest_signal(std::int32_t signal, std::int32_t fallbackStatus) {
  struct sigaction action {};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  if (::sigaction(signal, &action, nullptr) != 0 && signal != SIGKILL) return fallbackStatus;
  sigset_t unblocked;
  sigemptyset(&unblocked);
  sigaddset(&unblocked, signal);
  if (::sigprocmask(SIG_UNBLOCK, &unblocked, nullptr) != 0) return fallbackStatus;
  ::raise(signal);
  return fallbackStatus;
}

void configure_interpreter_assets(arm64metal::process_runner::LaunchOptions& options,
                                  const std::filesystem::path& executablePath) {
  const auto selected = arm64metal::select_interpreter_assets(
      executablePath, std::getenv("ARM64_METAL_INTERPRETER"),
      std::getenv("ARM64_METAL_INTERPRETER_LIBRARY"),
      std::getenv("ARM64_METAL_INTERPRETER_SOURCE"));
  options.interpreter = selected.enabled;
  options.interpreterLibrary = selected.library;
  options.interpreterSource = selected.source;
}

std::int32_t launch_with_signal_propagation(
    const std::filesystem::path& guestExecutable,
    const std::vector<std::string>& arguments,
    const arm64metal::process_runner::LaunchOptions& options) {
  std::int32_t terminatingSignal = 0;
  const auto status = arm64metal::process_runner::launch_executable(
      guestExecutable, arguments, options, &terminatingSignal);
  if (terminatingSignal != 0) return re_terminate_with_guest_signal(terminatingSignal, status);
  return status;
}

int run_command(const std::vector<std::string>& args,
                const std::filesystem::path& executablePath) {
  if (args.empty() || (args.size() == 1 && args[0] == "--help")) {
    print_help();
    return 0;
  }

  if (args[0] == "interpreter-probe" && args.size() == 2) {
    using arm64metal::gpu_interpreter::InstructionPacket;
    using arm64metal::gpu_interpreter::Interpreter;
    using arm64metal::gpu_interpreter::ProcessState;
    using arm64metal::translate_core::decode_operation;
    using arm64metal::translate_core::serialize_operation;

    const auto packet = [](std::uint32_t bits, std::uint64_t pc,
                           std::uint32_t flags = 0,
                           std::uint32_t successorCount = 0,
                           std::uint64_t successor0 = 0,
                           std::uint64_t successor1 = 0) {
      const auto operation = decode_operation(bits);
      if (!operation) throw std::runtime_error("Probe instruction has no typed operation");
      InstructionPacket result;
      result.operation = serialize_operation(*operation);
      result.pc = pc;
      result.flags = flags;
      result.successorCount = successorCount;
      result.successor0 = successor0;
      result.successor1 = successor1;
      return result;
    };
    constexpr auto blockStart = arm64metal::gpu_interpreter::packetBlockStart;
    constexpr auto blockEnd = arm64metal::gpu_interpreter::packetBlockEnd;
    const std::vector<InstructionPacket> firstProgram{
        packet(0xd2800540u, 0x1000, blockStart),
        packet(0xd65f03c0u, 0x1004, blockEnd)};
    const std::vector<InstructionPacket> secondProgram{
        packet(0x52b7dde0u, 0x2000, blockStart),
        packet(0xd65f03c0u, 0x2004, blockEnd)};
    const std::vector<std::vector<InstructionPacket>> moveWidePrograms{
        {packet(0x92a24680u, 0x5000, blockStart), packet(0xd65f03c0u, 0x5004, blockEnd)},
        {packet(0x12a24680u, 0x6000, blockStart), packet(0xd65f03c0u, 0x6004, blockEnd)},
        {packet(0xd28acf00u, 0x7000, blockStart), packet(0xf2d579a0u, 0x7004),
         packet(0xd65f03c0u, 0x7008, blockEnd)},
        {packet(0xd2eeeee0u, 0x8000, blockStart), packet(0x72b579a0u, 0x8004),
         packet(0xd65f03c0u, 0x8008, blockEnd)},
        {packet(0xd28acf00u, 0x9000, blockStart), packet(0xf2d579bfu, 0x9004),
         packet(0xd65f03c0u, 0x9008, blockEnd)},
        {packet(0xd28acf00u, 0xa000, blockStart), packet(0x72b579bfu, 0xa004),
         packet(0xd65f03c0u, 0xa008, blockEnd)}};
    const std::vector<std::vector<InstructionPacket>> addSubImmediatePrograms{
        {packet(0xd28000a0u, 0xb000, blockStart), packet(addXShiftedEncoding, 0xb004),
         packet(0xd65f03c0u, 0xb008, blockEnd)},
        {packet(0xd2a468a0u, 0xc000, blockStart), packet(subXShiftedEncoding, 0xc004),
         packet(0xd65f03c0u, 0xc008, blockEnd)},
        {packet(0x529fffe0u, 0xd000, blockStart), packet(addWWrapEncoding, 0xd004),
         packet(0xd65f03c0u, 0xd008, blockEnd)},
        {packet(0x52a24680u, 0xe000, blockStart), packet(subWShiftedEncoding, 0xe004),
         packet(0xd65f03c0u, 0xe008, blockEnd)}};
    const std::vector<std::vector<InstructionPacket>> addSubFlagPrograms{
        {packet(addsXOverflowEncoding, 0xf000, blockStart),
         packet(0xd65f03c0u, 0xf004, blockEnd)},
        {packet(subsXUnderflowEncoding, 0x10000, blockStart),
         packet(0xd65f03c0u, 0x10004, blockEnd)},
        {packet(cmpXShiftedEncoding, 0x11000, blockStart),
         packet(0xd65f03c0u, 0x11004, blockEnd)},
        {packet(cmnXShiftedEncoding, 0x12000, blockStart),
         packet(0xd65f03c0u, 0x12004, blockEnd)},
        {packet(addsWCarryEncoding, 0x13000, blockStart),
         packet(0xd65f03c0u, 0x13004, blockEnd)},
        {packet(subsWUnderflowEncoding, 0x14000, blockStart),
         packet(0xd65f03c0u, 0x14004, blockEnd)}};
    const std::vector<InstructionPacket> stackPointerProgram{
        packet(addXFromSpEncoding, 0x15000, blockStart),
        packet(subSpShiftedEncoding, 0x15004),
        packet(0xd65f03c0u, 0x15008, blockEnd)};
    const std::vector<InstructionPacket> wordStackPointerProgram{
        packet(0x52b0000au, 0x16000, blockStart),
        packet(addWspFromW10Encoding, 0x16004),
        packet(addXFromSpOneEncoding, 0x16008),
        packet(0xd65f03c0u, 0x1600c, blockEnd)};
    const std::vector<InstructionPacket> addWordFlagsProgram{
        packet(addsWResultEncoding, 0x17000, blockStart),
        packet(0xd65f03c0u, 0x17004, blockEnd)};
    const std::vector<InstructionPacket> compareStackPointerProgram{
        packet(cmpSpZeroEncoding, 0x18000, blockStart),
        packet(0xd65f03c0u, 0x18004, blockEnd)};
    const std::vector<InstructionPacket> branchProgram{
        packet(0xd28000e0u, 0x5000, blockStart),
        packet(0x14000002u, 0x5004, blockEnd, 1, 0x500c),
        packet(0xd65f03c0u, 0x500c, blockStart | blockEnd)};
    constexpr std::uint64_t conditionalPc = 0x100004000;
    const auto conditionalProgram = [&packet](std::uint32_t bits) {
      constexpr std::uint64_t pc = 0x100004000;
      constexpr std::uint64_t fallthrough = pc + 4;
      constexpr std::uint64_t target = pc + 16;
      return std::vector<InstructionPacket>{
          packet(bits, pc, blockStart | blockEnd, 2, target, fallthrough),
          packet(0xd28002e0u, fallthrough, blockStart),
          packet(0xd65f03c0u, fallthrough + 4, blockEnd),
          packet(0xd2800220u, target, blockStart),
          packet(0xd65f03c0u, target + 4, blockEnd)};
    };
    struct ConditionalCase {
      std::uint32_t bits;
      std::uint64_t input;
      std::uint64_t expected;
    };
    const std::vector<ConditionalCase> conditionalCases{
        {0x34000080u, 0, 17},
        {0x34000080u, 1, 23},
        {0x34000080u, 0x100000000ull, 17},
        {0x35000080u, 0, 23},
        {0x35000080u, 1, 17},
        {0x35000080u, 0x100000000ull, 23},
        {0xb4000080u, 0, 17},
        {0xb4000080u, 0x100000000ull, 23},
        {0xb5000080u, 0, 23},
        {0xb5000080u, 0x100000000ull, 17},
        {0xb400009fu, 0xabc, 17},
        {0x3500009fu, 0, 23}};
    const std::vector<InstructionPacket> missingTargetProgram{
        packet(0x14000002u, 0x6000, blockStart | blockEnd, 1, 0x6008)};
    const std::vector<InstructionPacket> missingFallthroughProgram{
        packet(0xd2800540u, 0x6100, blockStart | blockEnd, 1, 0x6104)};
    const std::vector<InstructionPacket> nonEntryTargetProgram{
        packet(0xd2800540u, 0x6200, blockStart | blockEnd, 1, 0x6204),
        packet(0xd2800020u, 0x6204)};
    constexpr std::size_t interpreterBlockCap = 8192;
    constexpr std::uint64_t cappedProgramEntry = 0x800000;
    std::vector<InstructionPacket> cappedMissingTargetProgram;
    cappedMissingTargetProgram.reserve(interpreterBlockCap);
    for (std::size_t index = 0; index < interpreterBlockCap; ++index) {
      const auto pc = cappedProgramEntry + static_cast<std::uint64_t>(index * 4);
      cappedMissingTargetProgram.push_back(packet(0xd2800000u, pc,
          blockStart | blockEnd, 1, pc + 4));
    }

    auto backend = arm64metal::Backend::createDefault();
    const auto initialCompiles = backend.compiledPipelineCount();
    const auto initialSourceCompiles = backend.sourceLibraryCompileCount();
    const auto initialDispatches = backend.completedDispatchCount();
    const Interpreter interpreter(backend, args[1]);
    const auto compiled = backend.compiledPipelineCount() - initialCompiles;
    const auto sourceCompiles = backend.sourceLibraryCompileCount() - initialSourceCompiles;
    if (compiled != 1 || sourceCompiles != 0) {
      throw std::runtime_error("AOT interpreter load counters mismatch");
    }

    const auto runFrom = [&backend, &interpreter](
                             const auto& packets, ProcessState initial) {
      initial[arm64metal::gpu_interpreter::returnRegisterIndex] =
          arm64metal::gpu_interpreter::noReturnAddress;
      initial[33] = packets.front().pc;
      initial[arm64metal::gpu_interpreter::blockBudgetIndex] = 1;
      return interpreter.run(backend, packets, initial);
    };
    const auto execute = [&runFrom](const auto& packets,
                                    std::uint64_t flags = 0) {
      ProcessState initial{};
      initial[32] = flags;
      return runFrom(packets, initial);
    };
    const auto first = execute(firstProgram);
    const auto second = execute(secondProgram);
    std::vector<arm64metal::gpu_interpreter::Result> moveWideResults;
    moveWideResults.reserve(moveWidePrograms.size());
    for (const auto& program : moveWidePrograms) {
      moveWideResults.push_back(execute(program));
    }
    std::vector<arm64metal::gpu_interpreter::Result> addSubImmediateResults;
    addSubImmediateResults.reserve(addSubImmediatePrograms.size());
    for (const auto& program : addSubImmediatePrograms) {
      addSubImmediateResults.push_back(execute(program, 8));
    }
    std::vector<arm64metal::gpu_interpreter::Result> addSubFlagResults;
    addSubFlagResults.reserve(addSubFlagPrograms.size());
    const std::array<std::uint64_t, 6> flagInputs{
        0x7fffffffffffffffull, 0, 0x1000, 0xfffffffffffff000ull,
        0xffffffffull, 0};
    for (std::size_t index = 0; index < addSubFlagPrograms.size(); ++index) {
      ProcessState initial{};
      initial[1] = flagInputs[index];
      addSubFlagResults.push_back(runFrom(addSubFlagPrograms[index], initial));
    }
    ProcessState stackPointerInitial{};
    stackPointerInitial[31] = 0x100000;
    const auto stackPointerResult = runFrom(
        stackPointerProgram, stackPointerInitial);
    ProcessState wordStackPointerInitial{};
    wordStackPointerInitial[31] = 0x100000000ull;
    const auto wordStackPointerResult = runFrom(
        wordStackPointerProgram, wordStackPointerInitial);
    ProcessState addWordFlagsInitial{};
    addWordFlagsInitial[1] = 0xffffffffull;
    const auto addWordFlagsResult = runFrom(
        addWordFlagsProgram, addWordFlagsInitial);
    ProcessState compareStackPointerInitial{};
    compareStackPointerInitial[31] = 0x100000;
    const auto compareStackPointerResult = runFrom(
        compareStackPointerProgram, compareStackPointerInitial);
    ProcessState continuationState{};
    continuationState[arm64metal::gpu_interpreter::returnRegisterIndex] = 0x3004;
    continuationState[33] = 0x3000;
    continuationState[arm64metal::gpu_interpreter::blockBudgetIndex] = 1;
    const auto continuation = interpreter.run(
        backend, {packet(0xd65f03c0u, 0x3000, blockStart | blockEnd)}, continuationState);
    ProcessState zeroBudgetState{};
    zeroBudgetState[arm64metal::gpu_interpreter::resultRegisterIndex] = 99;
    zeroBudgetState[arm64metal::gpu_interpreter::returnRegisterIndex] =
        arm64metal::gpu_interpreter::noReturnAddress;
    zeroBudgetState[33] = 0x4000;
    zeroBudgetState[arm64metal::gpu_interpreter::blockBudgetIndex] = 0;
    const auto zeroBudget = interpreter.run(
        backend, {packet(0xd2800540u, 0x4000, blockStart),
                  packet(0xd65f03c0u, 0x4004, blockEnd)},
        zeroBudgetState);
    ProcessState branchState{};
    branchState[arm64metal::gpu_interpreter::returnRegisterIndex] =
        arm64metal::gpu_interpreter::noReturnAddress;
    branchState[33] = 0x5000;
    branchState[arm64metal::gpu_interpreter::blockBudgetIndex] = 2;
    const auto branch = interpreter.run(backend, branchProgram, branchState);
    branchState[arm64metal::gpu_interpreter::blockBudgetIndex] = 1;
    const auto branchBudget = interpreter.run(backend, branchProgram, branchState);
    ProcessState missingTargetState{};
    missingTargetState[arm64metal::gpu_interpreter::resultRegisterIndex] = 99;
    missingTargetState[arm64metal::gpu_interpreter::returnRegisterIndex] =
        arm64metal::gpu_interpreter::noReturnAddress;
    missingTargetState[33] = 0x6000;
    missingTargetState[arm64metal::gpu_interpreter::blockBudgetIndex] = 2;
    const auto missingTarget = interpreter.run(
        backend, missingTargetProgram, missingTargetState);
    auto missingTargetLastBudgetState = missingTargetState;
    missingTargetLastBudgetState[arm64metal::gpu_interpreter::blockBudgetIndex] = 1;
    const auto missingTargetLastBudget = interpreter.run(
        backend, missingTargetProgram, missingTargetLastBudgetState);
    ProcessState missingFallthroughState{};
    missingFallthroughState[33] = 0x6100;
    missingFallthroughState[arm64metal::gpu_interpreter::blockBudgetIndex] = 1;
    const auto missingFallthrough = interpreter.run(
        backend, missingFallthroughProgram, missingFallthroughState);
    ProcessState nonEntryTargetState{};
    nonEntryTargetState[33] = 0x6200;
    nonEntryTargetState[arm64metal::gpu_interpreter::blockBudgetIndex] = 1;
    const auto nonEntryTarget = interpreter.run(
        backend, nonEntryTargetProgram, nonEntryTargetState);
    ProcessState cappedMissingTargetState{};
    cappedMissingTargetState[33] = cappedProgramEntry;
    cappedMissingTargetState[arm64metal::gpu_interpreter::blockBudgetIndex] =
        interpreterBlockCap + 1;
    const auto cappedMissingTarget = interpreter.run(
        backend, cappedMissingTargetProgram, cappedMissingTargetState);
    std::vector<arm64metal::gpu_interpreter::Result> conditionalResults;
    conditionalResults.reserve(conditionalCases.size());
    for (const auto& test : conditionalCases) {
      ProcessState initial{};
      initial[0] = test.input;
      initial[31] = 0xffff;
      initial[32] = 0xa;
      initial[arm64metal::gpu_interpreter::returnRegisterIndex] =
          arm64metal::gpu_interpreter::noReturnAddress;
      initial[33] = conditionalPc;
      initial[arm64metal::gpu_interpreter::blockBudgetIndex] = 2;
      conditionalResults.push_back(interpreter.run(
          backend, conditionalProgram(test.bits), initial));
    }
    const auto dispatches = backend.completedDispatchCount() - initialDispatches;
    const auto requireProbe = [](bool matches, const char* name) {
      if (!matches) {
        throw std::runtime_error(std::string("Fixed GPU interpreter probe mismatch: ") +
                                 name);
      }
    };
    requireProbe(compiled == 1 && sourceCompiles == 0, "AOT pipeline counters");
    requireProbe(dispatches == 43, "completed dispatch count");
    requireProbe(first.state[34] == 0 && first.state[0] == 42 &&
                     second.state[34] == 0 && second.state[0] == 0xbeef0000u,
                 "independent MOVZ programs");
    requireProbe(moveWideResults[0].state[34] == 0 &&
                     moveWideResults[0].state[0] == 0xffffffffedcbffffull &&
                     moveWideResults[1].state[34] == 0 &&
                     moveWideResults[1].state[0] == 0xedcbffffu &&
                     moveWideResults[2].state[34] == 0 &&
                     moveWideResults[2].state[0] == 0xabcd00005678ull &&
                     moveWideResults[3].state[34] == 0 &&
                     moveWideResults[3].state[0] == 0xabcd0000u &&
                     moveWideResults[4].state[34] == 0 &&
                     moveWideResults[4].state[0] == 0x5678u &&
                     moveWideResults[5].state[34] == 0 &&
                     moveWideResults[5].state[0] == 0x5678u,
                 "MOVN/MOVK width and Rd31 cases");
    requireProbe(addSubImmediateResults[0].state[34] == 0 &&
                     addSubImmediateResults[0].state[0] == 0x123005u &&
                     addSubImmediateResults[1].state[34] == 0 &&
                     addSubImmediateResults[1].state[0] == 0x2332d000u &&
                     addSubImmediateResults[2].state[34] == 0 &&
                     addSubImmediateResults[2].state[0] == 0x10000u &&
                     addSubImmediateResults[3].state[34] == 0 &&
                     addSubImmediateResults[3].state[0] == 0x1221d000u &&
                     addSubImmediateResults[0].state[32] == 8 &&
                     addSubImmediateResults[1].state[32] == 8 &&
                     addSubImmediateResults[2].state[32] == 8 &&
                     addSubImmediateResults[3].state[32] == 8,
                 "non-flag-setting W/X arithmetic and NZCV preservation");
    requireProbe(addSubFlagResults[0].state[32] == 6 &&
                     addSubFlagResults[0].state[34] == 0,
                 "ADDS X overflow expected packed N|V (6)");
    requireProbe(addSubFlagResults[1].state[32] == 2 &&
                     addSubFlagResults[1].state[34] == 0,
                 "SUBS X borrow expected packed N (2)");
    requireProbe(addSubFlagResults[2].state[32] == 9 &&
                     addSubFlagResults[2].state[34] == 0,
                 "CMP X shifted equality expected packed Z|C (9)");
    requireProbe(addSubFlagResults[3].state[32] == 9 &&
                     addSubFlagResults[3].state[34] == 0,
                 "CMN X shifted carry expected packed Z|C (9)");
    requireProbe(addSubFlagResults[4].state[32] == 9 &&
                     addSubFlagResults[4].state[34] == 0,
                 "ADDS W carry expected packed Z|C (9)");
    requireProbe(addSubFlagResults[5].state[32] == 2 &&
                     addSubFlagResults[5].state[34] == 0,
                 "SUBS W borrow expected packed N (2)");
    requireProbe(stackPointerResult.state[0] == 0x102000 &&
                     stackPointerResult.state[31] == 0xff000 &&
                     stackPointerResult.state[34] == 0,
                 "X SP add/sub update");
    requireProbe(wordStackPointerResult.state[31] == 0x80000000ull &&
                     wordStackPointerResult.state[0] == 0x80000001ull &&
                     wordStackPointerResult.state[34] == 0,
                 "WSP zero extension");
    requireProbe(addWordFlagsResult.state[0] == 0 &&
                     addWordFlagsResult.state[32] == 9 &&
                     addWordFlagsResult.state[34] == 0,
                 "ADDS W result and NZCV");
    requireProbe(compareStackPointerResult.state[32] == 8 &&
                     compareStackPointerResult.state[34] == 0,
                 "CMP SP expected NZCV");
    requireProbe(continuation.state[34] == 7 && continuation.state[33] == 0x3004,
                 "RET continuation address");
    requireProbe(zeroBudget.state[34] == 1 && zeroBudget.state[0] == 99,
                 "zero block budget");
    requireProbe(branch.state[34] == 0 && branch.state[0] == 7 &&
                     branch.state[36] == 0,
                 "B target execution");
    requireProbe(branchBudget.state[34] == 1 && branchBudget.state[33] == 0x500c &&
                     branchBudget.state[0] == 7 && branchBudget.state[36] == 0,
                 "B block budget");
    requireProbe(missingTarget.state[34] == 7 && missingTarget.state[33] == 0x6008 &&
                     missingTarget.state[0] == 99 && missingTarget.state[36] == 1,
                 "missing branch target");
    requireProbe(missingTargetLastBudget.state[34] == 7 &&
                     missingTargetLastBudget.state[33] == 0x6008 &&
                     missingTargetLastBudget.state[0] == 99 &&
                     missingTargetLastBudget.state[36] == 0,
                 "missing branch target at final budget");
    requireProbe(missingFallthrough.state[34] == 7 &&
                     missingFallthrough.state[33] == 0x6104 &&
                     missingFallthrough.state[36] == 0,
                 "missing fallthrough at final budget");
    requireProbe(nonEntryTarget.state[34] == 7 &&
                     nonEntryTarget.state[33] == 0x6204 &&
                     nonEntryTarget.state[36] == 0,
                 "non-entry successor at final budget");
    requireProbe(cappedMissingTarget.state[34] == 7 &&
                     cappedMissingTarget.state[33] ==
                         cappedProgramEntry + interpreterBlockCap * 4 &&
                     cappedMissingTarget.state[36] == 1,
                 "missing successor at interpreter hard cap");
    for (std::size_t index = 0; index < conditionalCases.size(); ++index) {
      const auto& result = conditionalResults[index].state;
      if (result[34] != 0 || result[0] != conditionalCases[index].expected ||
          result[32] != 0xa || result[36] != 0) {
        throw std::runtime_error("Fixed GPU interpreter CBZ/CBNZ probe mismatch");
      }
    }
    std::cerr << "pipeline_compiles=" << compiled
              << " runtime_source_compiles=" << sourceCompiles
              << " completed_dispatches=" << dispatches << '\n';
    std::cout << "movn=" << std::hex << moveWideResults[0].state[0] << ','
              << moveWideResults[1].state[0] << " movk="
              << moveWideResults[2].state[0] << ',' << moveWideResults[3].state[0]
              << " discard=" << moveWideResults[4].state[0] << ','
              << moveWideResults[5].state[0] << '\n';
    const auto architecturalNzcv = [](std::uint64_t flags) {
      return ((flags & 2u) << 30) | ((flags & 1u) << 30) |
             ((flags & 8u) << 26) | ((flags & 4u) << 26);
    };
    std::cout << "add=" << std::hex << addSubImmediateResults[0].state[0] << ','
              << addSubImmediateResults[2].state[0] << " sub="
              << addSubImmediateResults[1].state[0] << ','
              << addSubImmediateResults[3].state[0] << " nzcv="
              << architecturalNzcv(addSubImmediateResults[0].state[32]) << '\n';
    std::cout << "adds=" << std::hex
              << architecturalNzcv(addSubFlagResults[0].state[32]) << ','
              << architecturalNzcv(addWordFlagsResult.state[32]) << " subs="
              << architecturalNzcv(addSubFlagResults[1].state[32]) << ','
              << architecturalNzcv(addSubFlagResults[5].state[32]) << " cmp="
              << architecturalNzcv(addSubFlagResults[2].state[32]) << " cmn="
              << architecturalNzcv(addSubFlagResults[3].state[32]) << " cmpsp="
              << architecturalNzcv(compareStackPointerResult.state[32]) << " wz="
              << addWordFlagsResult.state[0] << " sp="
              << (stackPointerResult.state[0] - 0x100000ull) << ','
              << (stackPointerResult.state[31] - 0x100000ull) << ','
              << wordStackPointerResult.state[0] << '\n';
    return 0;
  }

  if (args[0] == "launch") {
    if (args.size() < 2) throw std::runtime_error("launch requires an executable path");
    std::vector<std::string> arguments(args.begin() + 2, args.end());
    arm64metal::process_runner::LaunchOptions options;
    options.sharedCache = environment_enabled("ARM64_METAL_SHARED_CACHE");
    options.demandCache = environment_enabled("ARM64_METAL_CACHE_DEMAND");
    options.cacheBootstrap = environment_enabled("ARM64_METAL_CACHE_BOOTSTRAP");
    options.cachedInitializers = environment_enabled("ARM64_METAL_CACHE_INITIALIZERS");
    options.runtimeExecutablePath = executablePath;
    configure_interpreter_assets(options, executablePath);
    return launch_with_signal_propagation(args[1], arguments, options);
  }

  if (args[0] == "__spawn_launch") {
    if (args.size() < 4) throw std::runtime_error("Malformed internal spawn launch");
    const auto argumentCount = parse_spawn_count(args[2]);
    if (argumentCount == 0 || argumentCount > args.size() - 4) {
      throw std::runtime_error("Malformed internal spawn argument vector");
    }
    std::size_t cursor = 3;
    const auto guestArgv0 = args[cursor++];
    std::vector<std::string> arguments;
    arguments.reserve(argumentCount - 1);
    for (std::size_t index = 1; index < argumentCount; ++index) {
      arguments.push_back(args[cursor++]);
    }
    if (cursor + 1 != args.size()) {
      throw std::runtime_error("Malformed internal spawn environment channel");
    }
    const auto environmentDescriptor = parse_spawn_count(args[cursor]);
    if (environmentDescriptor < static_cast<std::size_t>(
                                    arm64metal::kGuestSpawnEnvironmentDescriptorFloor) ||
        environmentDescriptor > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("Invalid internal spawn environment descriptor");
    }
    auto environment = read_spawn_environment(
        static_cast<int>(environmentDescriptor));

    arm64metal::process_runner::LaunchOptions options;
    options.sharedCache = environment_enabled("ARM64_METAL_SHARED_CACHE");
    options.demandCache = environment_enabled("ARM64_METAL_CACHE_DEMAND");
    options.cacheBootstrap = environment_enabled("ARM64_METAL_CACHE_BOOTSTRAP");
    options.cachedInitializers = environment_enabled("ARM64_METAL_CACHE_INITIALIZERS");
    options.runtimeExecutablePath = executablePath;
    options.guestArgv0 = guestArgv0;
    options.guestEnvironment = std::move(environment);
    configure_interpreter_assets(options, executablePath);
    return launch_with_signal_propagation(args[1], arguments, options);
  }

  if (args[0] == "__exec_launch") {
    if (args.size() < 6) throw std::runtime_error("Malformed internal exec launch");
    const auto argumentCount = parse_spawn_count(args[2]);
    if (argumentCount == 0 || argumentCount > args.size() - 5) {
      throw std::runtime_error("Malformed internal exec argument vector");
    }
    std::size_t cursor = 3;
    const auto guestArgv0 = args[cursor++];
    std::vector<std::string> arguments;
    arguments.reserve(argumentCount - 1);
    for (std::size_t index = 1; index < argumentCount; ++index) {
      arguments.push_back(args[cursor++]);
    }
    const auto environmentDescriptor = parse_spawn_count(args[cursor++]);
    if (environmentDescriptor < static_cast<std::size_t>(
                                    arm64metal::kGuestSpawnEnvironmentDescriptorFloor) ||
        environmentDescriptor > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("Invalid internal exec environment descriptor");
    }
    const auto inheritedCount = parse_spawn_count(args[cursor++]);
    if (inheritedCount > args.size() - cursor) {
      throw std::runtime_error("Malformed internal exec descriptor vector");
    }
    std::vector<std::int32_t> inheritedDescriptors;
    inheritedDescriptors.reserve(inheritedCount);
    std::set<std::int32_t> uniqueDescriptors;
    for (std::size_t index = 0; index < inheritedCount; ++index, ++cursor) {
      const auto descriptor = parse_spawn_count(args[cursor]);
      if (descriptor < 3 || descriptor > static_cast<std::size_t>(
                                              std::numeric_limits<std::int32_t>::max()) ||
          !uniqueDescriptors.insert(static_cast<std::int32_t>(descriptor)).second) {
        throw std::runtime_error("Invalid internal exec inherited descriptor");
      }
      inheritedDescriptors.push_back(static_cast<std::int32_t>(descriptor));
    }
    if (cursor == args.size()) {
      throw std::runtime_error("Malformed internal exec writable descriptor count");
    }
    const auto writableCount = parse_spawn_count(args[cursor++]);
    if (writableCount != args.size() - cursor) {
      throw std::runtime_error("Malformed internal exec writable descriptor vector");
    }
    std::vector<std::int32_t> writableDescriptors;
    writableDescriptors.reserve(writableCount);
    std::set<std::int32_t> uniqueWritableDescriptors;
    for (; cursor < args.size(); ++cursor) {
      const auto descriptor = parse_spawn_count(args[cursor]);
      if (descriptor < 3 || descriptor > static_cast<std::size_t>(
                                              std::numeric_limits<std::int32_t>::max()) ||
          !uniqueDescriptors.count(static_cast<std::int32_t>(descriptor)) ||
          !uniqueWritableDescriptors.insert(static_cast<std::int32_t>(descriptor)).second) {
        throw std::runtime_error("Invalid internal exec writable descriptor");
      }
      writableDescriptors.push_back(static_cast<std::int32_t>(descriptor));
    }
    auto environment = read_spawn_environment(static_cast<int>(environmentDescriptor));

    arm64metal::process_runner::LaunchOptions options;
    options.sharedCache = environment_enabled("ARM64_METAL_SHARED_CACHE");
    options.demandCache = environment_enabled("ARM64_METAL_CACHE_DEMAND");
    options.cacheBootstrap = environment_enabled("ARM64_METAL_CACHE_BOOTSTRAP");
    options.cachedInitializers = environment_enabled("ARM64_METAL_CACHE_INITIALIZERS");
    options.runtimeExecutablePath = executablePath;
    options.guestArgv0 = guestArgv0;
    options.guestEnvironment = std::move(environment);
    options.inheritedGuestDescriptors = std::move(inheritedDescriptors);
    options.inheritedWritableGuestDescriptors = std::move(writableDescriptors);
    configure_interpreter_assets(options, executablePath);
    return launch_with_signal_propagation(args[1], arguments, options);
  }

  if (args[0] == "inspect-cache" && args.size() == 3) {
    arm64metal::SharedCache cache;
    const auto symbol = cache.exportSymbol(args[1], args[2]);
    if (!symbol) throw std::runtime_error("Direct export is absent from the cached image: " + args[2]);
    std::cout << args[2] << " offset 0x" << std::hex << symbol->offset
              << " first word 0x" << std::setw(8) << std::setfill('0') << symbol->firstWord << '\n';
    return 0;
  }

  if (args[0] == "inspect-cache-rebases" && args.size() == 2) {
    arm64metal::SharedCache cache;
    for (const auto& rebase : cache.rebases(args[1])) {
      std::cout << "0x" << std::hex << rebase.location << " 0x" << rebase.target << std::dec << ' ';
      if (rebase.authentication) {
        const auto& auth = *rebase.authentication;
        std::cout << "key=" << (auth.dataKey ? "DA" : "IA")
                  << " diversity=" << auth.diversity
                  << " address-diversity=" << (auth.addressDiversity ? "true" : "false");
      } else {
        std::cout << "high-byte=" << static_cast<unsigned>(rebase.highByte);
      }
      std::cout << '\n';
    }
    return 0;
  }

  if (args[0] == "translate" && args.size() == 3) {
    std::cout << arm64metal::function_runner::translate(args[1], args[2]);
    return 0;
  }
  if (args[0] == "verify" && args.size() == 3) {
    arm64metal::function_runner::verify(args[1], args[2]);
    return 0;
  }
  if (args[0] == "run" && args.size() >= 3 && args.size() <= 5) {
    const std::string input_path = args.size() >= 4 ? args[3] : "-";
    const std::uint32_t budget = args.size() == 5 ? parse_budget(args[4]) : 65'536;
    const std::string output = arm64metal::function_runner::run(
        args[1], args[2], read_input(input_path), budget);
    std::cout << output << '\n';
    return 0;
  }
  if (args[0] == "run-stream" && args.size() == 3) {
    arm64metal::function_runner::run_stream(args[1], args[2], std::cin, std::cout);
    return 0;
  }
  throw std::runtime_error("Unknown command or wrong arguments. See --help.");
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);
  try {
    const auto executablePath = host_executable_path();
    return run_command(args, executablePath);
  } catch (const std::exception& error) {
    std::cerr << "arm64-metal: " << error.what() << '\n';
    return 1;
  }
}
