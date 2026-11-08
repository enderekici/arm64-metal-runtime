#include "image_loader.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    constexpr std::size_t placement = 0x100000000ULL;
    arm64metal::MachImage image(argv[1], false, placement);
    if (image.initializers.size() != 2 || image.finalizers.size() != 1) return 3;
    for (const auto target : image.finalizers) {
      bool executable = false;
      for (const auto& segment : image.mapped_segments) {
        if (segment.executable && target >= segment.offset &&
            target < segment.offset + segment.size) {
          executable = true;
          break;
        }
      }
      if (!executable) return 4;
    }

    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) return 5;
    auto value = reinterpret_cast<int (*)()>(dlsym(handle, "dyld_finalizer_fixture_value"));
    if (value == nullptr || value() != 42) return 6;
    if (dlclose(handle) != 0) return 7;
    std::puts("metadata initializers=2 finalizers=1 finalizer_text=1 native_value=42");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 8;
  }
  return 0;
}
