#pragma once

#include <dlfcn.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace arm64metal {

// Resolves only images already present in the guest image graph. Handles are
// opaque guest values and are never passed to the host's dlopen implementation.
class GuestImageHandles {
 public:
  static constexpr std::uint64_t rtld_default_handle =
      std::numeric_limits<std::uint64_t>::max() - 1;
  static constexpr std::uint64_t rtld_main_only_handle =
      std::numeric_limits<std::uint64_t>::max() - 4;

  explicit GuestImageHandles(std::unordered_map<std::string, std::uint64_t> image_headers,
                             std::filesystem::path executable_path = {})
      : image_headers_(std::move(image_headers)) {
    if (executable_path.empty()) executable_path = std::filesystem::current_path();
    std::error_code error;
    executable_path = std::filesystem::absolute(executable_path, error);
    if (error) executable_path = executable_path.lexically_normal();
    executable_directory_ = executable_path.parent_path();
  }

  std::uint64_t open(const std::string& path, std::int32_t mode) {
    if (path.empty() || (mode & RTLD_NOLOAD) == 0) {
      throw std::invalid_argument("Guest dlopen requires a named image and RTLD_NOLOAD");
    }
    const auto header = lookup(path);
    if (!header) return 0;
    if (*header >= (std::uint64_t{1} << 63)) {
      throw std::overflow_error("Guest image header cannot be represented as a dlopen handle");
    }
    return retain(*header, mode);
  }

  std::optional<std::uint64_t> header_for_path(const std::string& path) const {
    return lookup(path);
  }

  // Opens an image already present in the process graph. It does not load or
  // initialize images; an empty result leaves that decision to the caller.
  std::optional<std::uint64_t> open_loaded(const std::string& path,
                                          std::int32_t mode) {
    if (path.empty() || (mode & RTLD_NOLOAD) != 0) {
      throw std::invalid_argument("Guest loaded-image open requires a named load mode");
    }
    const auto header = lookup(path);
    if (!header) return std::nullopt;
    return retain(*header, mode);
  }

  std::uint64_t retain(std::uint64_t header, std::int32_t mode) {
    if (header >= (std::uint64_t{1} << 63)) {
      throw std::overflow_error("Guest image header cannot be represented as a dlopen handle");
    }
    const std::uint64_t handle = (std::uint64_t{1} << 63) | header |
                                 ((mode & RTLD_FIRST) != 0 ? 1U : 0U);
    auto& references = references_[handle];
    if (references == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Guest dlopen reference count overflow");
    }
    ++references;
    closed_handles_.erase(handle);
    return handle;
  }

  std::uint64_t open_main(std::int32_t mode) const noexcept {
    return (mode & RTLD_FIRST) != 0 ? rtld_main_only_handle : rtld_default_handle;
  }

  void register_loaded(const std::string& path, std::uint64_t header) {
    image_headers_[path] = header;
    std::filesystem::path candidate(path);
    if (!candidate.is_absolute()) candidate = std::filesystem::absolute(candidate);
    candidate = candidate.lexically_normal();
    image_headers_[candidate.string()] = header;
  }

  std::uint64_t close(std::uint64_t handle) {
    if (is_rtld_default(handle) || is_rtld_main_only(handle)) return 0;
    const auto found = references_.find(handle);
    if (found == references_.end()) return std::numeric_limits<std::uint32_t>::max();
    if (--found->second == 0) {
      references_.erase(found);
      closed_handles_.insert(handle);
    }
    return 0;
  }

  std::uint64_t reference_count(std::uint64_t handle) const noexcept {
    const auto found = references_.find(handle);
    return found == references_.end() ? 0 : found->second;
  }

  std::optional<std::uint64_t> loaded_header(std::uint64_t handle) const noexcept {
    if (references_.find(handle) == references_.end()) return std::nullopt;
    constexpr std::uint64_t handleFlag = std::uint64_t{1} << 63U;
    return handle & ~handleFlag & ~std::uint64_t{1};
  }

  bool first_only(std::uint64_t handle) const noexcept {
    return references_.find(handle) != references_.end() && (handle & 1U) != 0;
  }

  bool is_rtld_default(std::uint64_t handle) const noexcept {
    return handle == rtld_default_handle;
  }

  bool is_rtld_main_only(std::uint64_t handle) const noexcept {
    return handle == rtld_main_only_handle;
  }

  bool is_closed_guest_handle(std::uint64_t handle) const noexcept {
    return closed_handles_.find(handle) != closed_handles_.end();
  }

 private:
  std::optional<std::uint64_t> lookup(const std::string& path) const {
    if (const auto exact = image_headers_.find(path); exact != image_headers_.end()) {
      return exact->second;
    }
    constexpr char executablePathPrefix[] = "@executable_path/";
    std::filesystem::path candidate;
    if (path == "@executable_path") {
      candidate = executable_directory_;
    } else if (path.rfind(executablePathPrefix, 0) == 0) {
      candidate = executable_directory_ / path.substr(sizeof(executablePathPrefix) - 1);
    } else {
      candidate = std::filesystem::path(path);
    }
    if (!candidate.is_absolute()) candidate = std::filesystem::absolute(candidate);
    candidate = candidate.lexically_normal();
    if (const auto direct = image_headers_.find(candidate.string()); direct != image_headers_.end()) {
      return direct->second;
    }

    for (unsigned depth = 0; depth < 16; ++depth) {
      std::error_code error;
      auto parent = candidate.parent_path();
      if (parent.empty()) parent = std::filesystem::current_path(error);
      if (error) return std::nullopt;
      auto resolved_parent = std::filesystem::weakly_canonical(parent, error);
      if (error) resolved_parent = parent.lexically_normal();
      candidate = (resolved_parent / candidate.filename()).lexically_normal();
      if (const auto found = image_headers_.find(candidate.string()); found != image_headers_.end()) {
        return found->second;
      }

      error.clear();
      if (!std::filesystem::is_symlink(candidate, error) || error) return std::nullopt;
      auto target = std::filesystem::read_symlink(candidate, error);
      if (error) return std::nullopt;
      candidate = (target.is_absolute() ? target : resolved_parent / target).lexically_normal();
      if (const auto found = image_headers_.find(candidate.string()); found != image_headers_.end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  std::unordered_map<std::string, std::uint64_t> image_headers_;
  std::filesystem::path executable_directory_;
  std::unordered_map<std::uint64_t, std::uint64_t> references_;
  std::unordered_set<std::uint64_t> closed_handles_;
};

}  // namespace arm64metal
