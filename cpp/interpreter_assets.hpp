#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace arm64metal {

struct InterpreterAssets {
  bool enabled = false;
  std::filesystem::path library;
  std::filesystem::path source;
};

inline InterpreterAssets select_interpreter_assets(
    const std::filesystem::path& executablePath,
    const char* interpreterSetting,
    const char* configuredLibrary,
    const char* configuredSource) {
  InterpreterAssets selected;
  const auto interpreterResources =
      executablePath.parent_path() / "resources/interpreter";
  const auto packagedLibrary =
      interpreterResources / "runtime-interpreter.metallib";
  const auto packageMarker = interpreterResources / "AOT_REQUIRED";
  std::error_code error;
  const bool packageMode = std::filesystem::exists(packageMarker, error);
  if (error) {
    throw std::runtime_error("Unable to inspect GPU interpreter package marker: " +
                             packageMarker.string());
  }

  // An explicit setting wins; packaged AOT is automatic only when unset.
  selected.enabled = interpreterSetting != nullptr
      ? std::string_view(interpreterSetting) == "1"
      : packageMode;
  if (!selected.enabled) return selected;

  if (configuredLibrary != nullptr && configuredLibrary[0] != '\0') {
    selected.library = configuredLibrary;
    if (!std::filesystem::is_regular_file(selected.library, error) || error) {
      throw std::runtime_error(
          "Selected GPU interpreter library is missing or not a file: " +
          selected.library.string());
    }
    return selected;
  }
  if (packageMode) {
    if (!std::filesystem::is_regular_file(packagedLibrary, error) || error) {
      throw std::runtime_error(
          "Packaged GPU interpreter library is missing or not a file: " +
          packagedLibrary.string());
    }
    selected.library = packagedLibrary;
    return selected;
  }

  const bool packagedLibraryExists =
      std::filesystem::exists(packagedLibrary, error);
  if (error) {
    throw std::runtime_error("Unable to inspect packaged GPU interpreter library: " +
                             packagedLibrary.string());
  }
  if (packagedLibraryExists) {
    if (!std::filesystem::is_regular_file(packagedLibrary, error) || error) {
      throw std::runtime_error(
          "Packaged GPU interpreter library is not a regular file: " +
          packagedLibrary.string());
    }
    selected.library = packagedLibrary;
    return selected;
  }
  if (configuredSource != nullptr && configuredSource[0] != '\0') {
    selected.source = configuredSource;
  } else {
    selected.source = (executablePath.parent_path() /
        "../../cpp/gpu_interpreter.metal").lexically_normal();
  }
  return selected;
}

}  // namespace arm64metal
