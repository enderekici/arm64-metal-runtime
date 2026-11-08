#include "function_runner.hpp"
#include "gpu_interpreter.hpp"
#include "process_runner.hpp"
#include "shared_cache.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <mach-o/dyld.h>

namespace {

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

bool environment_enabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && std::string_view(value) == "1";
}

void print_help() {
  std::cout <<
      "Usage:\n"
      "  arm64-metal launch EXECUTABLE [ARG ...]\n"
      "    Set ARM64_METAL_INTERPRETER=1 to opt in for typed MOVZ/RET blocks.\n"
      "  arm64-metal run BINARY SYMBOL [INPUT.json|-] [BLOCK_BUDGET]\n"
      "  arm64-metal verify TRUSTED_DYLIB SYMBOL\n"
      "  arm64-metal translate BINARY SYMBOL\n"
      "  arm64-metal inspect-cache INSTALL_NAME SYMBOL\n"
      "  arm64-metal inspect-cache-rebases INSTALL_NAME\n"
      "\n"
      "Input is a JSON array of {\"words\":[1,2,3],\"seed\":7} invocations.\n"
      "Run reads stdin when INPUT is omitted or '-'.\n"
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

void configure_interpreter_assets(arm64metal::process_runner::LaunchOptions& options,
                                  const std::filesystem::path& executablePath) {
  const char* configuredLibrary = std::getenv("ARM64_METAL_INTERPRETER_LIBRARY");
  if (configuredLibrary != nullptr && configuredLibrary[0] != '\0') {
    options.interpreterLibrary = configuredLibrary;
    std::error_code error;
    if (!std::filesystem::is_regular_file(options.interpreterLibrary, error) || error) {
      throw std::runtime_error("Selected GPU interpreter library is missing or not a file: " +
                               options.interpreterLibrary.string());
    }
    return;
  }
  const auto interpreterResources = executablePath.parent_path() / "resources/interpreter";
  const auto packagedLibrary = interpreterResources / "runtime-interpreter.metallib";
  const auto packageMarker = interpreterResources / "AOT_REQUIRED";
  std::error_code error;
  const bool packageMode = std::filesystem::exists(packageMarker, error);
  if (error) {
    throw std::runtime_error("Unable to inspect GPU interpreter package marker: " +
                             packageMarker.string());
  }
  if (packageMode) {
    if (!std::filesystem::is_regular_file(packagedLibrary, error) || error) {
      throw std::runtime_error("Packaged GPU interpreter library is missing or not a file: " +
                               packagedLibrary.string());
    }
    options.interpreterLibrary = packagedLibrary;
    return;
  }
  const bool packagedLibraryExists = std::filesystem::exists(packagedLibrary, error);
  if (error) {
    throw std::runtime_error("Unable to inspect packaged GPU interpreter library: " +
                             packagedLibrary.string());
  }
  if (packagedLibraryExists && !std::filesystem::is_regular_file(packagedLibrary, error)) {
    throw std::runtime_error("Packaged GPU interpreter library is not a regular file: " +
                             packagedLibrary.string());
  }
  if (error) {
    throw std::runtime_error("Unable to inspect packaged GPU interpreter library: " +
                             packagedLibrary.string());
  }
  if (packagedLibraryExists) {
    options.interpreterLibrary = packagedLibrary;
    return;
  }
  const char* configuredSource = std::getenv("ARM64_METAL_INTERPRETER_SOURCE");
  if (configuredSource != nullptr && configuredSource[0] != '\0') {
    options.interpreterSource = configuredSource;
  } else {
    options.interpreterSource = executablePath.parent_path() /
        "../../cpp/gpu_interpreter.metal";
    options.interpreterSource = options.interpreterSource.lexically_normal();
  }
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
                           std::uint64_t successor0 = 0) {
      const auto operation = decode_operation(bits);
      if (!operation) throw std::runtime_error("Probe instruction has no typed operation");
      InstructionPacket result;
      result.operation = serialize_operation(*operation);
      result.pc = pc;
      result.flags = flags;
      result.successorCount = successorCount;
      result.successor0 = successor0;
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
    const std::vector<InstructionPacket> branchProgram{
        packet(0xd28000e0u, 0x5000, blockStart),
        packet(0x14000002u, 0x5004, blockEnd, 1, 0x500c),
        packet(0xd65f03c0u, 0x500c, blockStart | blockEnd)};

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

    const auto execute = [&backend, &interpreter](const auto& packets) {
      ProcessState initial{};
      initial[arm64metal::gpu_interpreter::returnRegisterIndex] =
          arm64metal::gpu_interpreter::noReturnAddress;
      initial[33] = packets.front().pc;
      initial[arm64metal::gpu_interpreter::blockBudgetIndex] = 1;
      return interpreter.run(backend, packets, initial);
    };
    const auto first = execute(firstProgram);
    const auto second = execute(secondProgram);
    std::vector<arm64metal::gpu_interpreter::Result> moveWideResults;
    moveWideResults.reserve(moveWidePrograms.size());
    for (const auto& program : moveWidePrograms) {
      moveWideResults.push_back(execute(program));
    }
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
    const auto dispatches = backend.completedDispatchCount() - initialDispatches;
    if (compiled != 1 || dispatches != 12 ||
        first.state[34] != 0 || first.state[0] != 42 ||
        second.state[34] != 0 || second.state[0] != 0xbeef0000u ||
        moveWideResults[0].state[34] != 0 ||
        moveWideResults[0].state[0] != 0xffffffffedcbffffull ||
        moveWideResults[1].state[34] != 0 ||
        moveWideResults[1].state[0] != 0xedcbffffu ||
        moveWideResults[2].state[34] != 0 ||
        moveWideResults[2].state[0] != 0xabcd00005678ull ||
        moveWideResults[3].state[34] != 0 ||
        moveWideResults[3].state[0] != 0xabcd0000u ||
        moveWideResults[4].state[34] != 0 ||
        moveWideResults[4].state[0] != 0x5678u ||
        moveWideResults[5].state[34] != 0 ||
        moveWideResults[5].state[0] != 0x5678u ||
        continuation.state[34] != 7 || continuation.state[33] != 0x3004 ||
        zeroBudget.state[34] != 1 || zeroBudget.state[0] != 99 ||
        branch.state[34] != 0 || branch.state[0] != 7 || branch.state[36] != 0 ||
        branchBudget.state[34] != 1 || branchBudget.state[33] != 0x500c ||
        branchBudget.state[0] != 7 || branchBudget.state[36] != 0) {
      throw std::runtime_error("Fixed GPU interpreter move-wide/block-flow probe mismatch");
    }
    std::cerr << "pipeline_compiles=" << compiled
              << " runtime_source_compiles=" << sourceCompiles
              << " completed_dispatches=" << dispatches << '\n';
    std::cout << "movn=" << std::hex << moveWideResults[0].state[0] << ','
              << moveWideResults[1].state[0] << " movk="
              << moveWideResults[2].state[0] << ',' << moveWideResults[3].state[0]
              << " discard=" << moveWideResults[4].state[0] << ','
              << moveWideResults[5].state[0] << '\n';
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
    options.interpreter = environment_enabled("ARM64_METAL_INTERPRETER");
    if (options.interpreter) configure_interpreter_assets(options, executablePath);
    return arm64metal::process_runner::launch_executable(args[1], arguments, options);
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
