#include "function_runner.hpp"
#include "process_runner.hpp"
#include "shared_cache.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
      "  arm64-metal run BINARY SYMBOL [INPUT.json|-] [BLOCK_BUDGET]\n"
      "  arm64-metal verify TRUSTED_DYLIB SYMBOL\n"
      "  arm64-metal translate BINARY SYMBOL\n"
      "  arm64-metal inspect-cache INSTALL_NAME SYMBOL\n"
      "  arm64-metal inspect-cache-rebases INSTALL_NAME\n\n"
      "Input is a JSON array of {\"words\":[1,2,3],\"seed\":7} invocations.\n"
      "Run reads stdin when INPUT is omitted or '-'.\n"
      "Launch executes the guest process on Metal.\n";
}

int run_command(const std::vector<std::string>& args) {
  if (args.empty() || (args.size() == 1 && args[0] == "--help")) {
    print_help();
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
    return run_command(args);
  } catch (const std::exception& error) {
    std::cerr << "arm64-metal: " << error.what() << '\n';
    return 1;
  }
}
