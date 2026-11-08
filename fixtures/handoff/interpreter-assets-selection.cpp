#include "../../cpp/interpreter_assets.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void touch(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Unable to create fixture file");
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() /
      ("arm64-interpreter-assets-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto executable = root / "bin/arm64-metal";
  const auto resources = root / "bin/resources/interpreter";
  const auto packagedLibrary = resources / "runtime-interpreter.metallib";
  const auto marker = resources / "AOT_REQUIRED";
  const auto customLibrary = root / "override.metallib";
  const auto customSource = root / "development.metal";

  try {
    auto selected = arm64metal::select_interpreter_assets(executable, nullptr,
                                                           nullptr, nullptr);
    require(!selected.enabled && selected.library.empty() && selected.source.empty(),
            "Source build unexpectedly enabled the interpreter");

    selected = arm64metal::select_interpreter_assets(executable, "1", nullptr,
                                                      customSource.c_str());
    require(selected.enabled && selected.library.empty() &&
                selected.source == customSource,
            "Explicit source-build opt-in was not preserved");

    touch(marker);
    touch(packagedLibrary);
    selected = arm64metal::select_interpreter_assets(executable, nullptr, nullptr,
                                                      nullptr);
    require(selected.enabled && selected.library == packagedLibrary &&
                selected.source.empty(),
            "AOT_REQUIRED did not select bundled AOT by default");

    selected = arm64metal::select_interpreter_assets(executable, "0", nullptr,
                                                      nullptr);
    require(!selected.enabled && selected.library.empty() && selected.source.empty(),
            "Explicit disable did not override the package marker");

    touch(customLibrary);
    selected = arm64metal::select_interpreter_assets(
        executable, nullptr, customLibrary.c_str(), nullptr);
    require(selected.enabled && selected.library == customLibrary,
            "Explicit AOT library did not override the bundled library");

    fs::remove(packagedLibrary);
    bool missingAotRejected = false;
    try {
      (void)arm64metal::select_interpreter_assets(executable, nullptr, nullptr,
                                                 nullptr);
    } catch (const std::runtime_error&) {
      missingAotRejected = true;
    }
    require(missingAotRejected, "AOT_REQUIRED accepted a missing bundled library");

    fs::remove(marker);
    touch(packagedLibrary);
    selected = arm64metal::select_interpreter_assets(executable, nullptr, nullptr,
                                                      nullptr);
    require(!selected.enabled,
            "An unmarked package changed the source-build default");
    selected = arm64metal::select_interpreter_assets(executable, "1", nullptr,
                                                      customSource.c_str());
    require(selected.enabled && selected.library == packagedLibrary &&
                selected.source.empty(),
            "An existing packaged library lost its opt-in precedence");

    fs::remove_all(root);
    std::cout << "Interpreter asset selection precedence passed\n";
    return 0;
  } catch (...) {
    fs::remove_all(root);
    throw;
  }
}
