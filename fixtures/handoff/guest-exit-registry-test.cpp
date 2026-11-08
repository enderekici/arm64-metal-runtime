#include "guest_exit.hpp"
#include "guest_dlopen.hpp"

#include <cstdio>
#include <unordered_map>

int main() {
  arm64metal::GuestExitRegistry registry;
  if (!registry.register_handler({11, 1, 101, 7,
          arm64metal::GuestExitHandlerKind::Cxa}) ||
      !registry.register_handler({22, 2, 202, 7,
          arm64metal::GuestExitHandlerKind::Cxa}) ||
      !registry.register_handler({33, 3, 303, 8,
          arm64metal::GuestExitHandlerKind::Cxa})) {
    return 1;
  }
  auto handler = registry.take_next_for_image(7);
  if (!handler || handler->target != 22 || handler->dso != 202 ||
      handler->owner_image != 7) return 2;
  if (!registry.register_handler({44, 4, 404, 7,
          arm64metal::GuestExitHandlerKind::Cxa})) return 3;
  handler = registry.take_next_for_image(7);
  if (!handler || handler->target != 44 || handler->dso != 404) return 4;
  handler = registry.take_next(101);
  if (!handler || handler->target != 11 || handler->owner_image != 7) return 5;
  handler = registry.take_next_for_image(7);
  if (handler) return 6;
  handler = registry.take_next_for_image(8);
  if (!handler || handler->target != 33 || registry.size() != 0) return 7;
  arm64metal::GuestImageHandles handles({{"/fixture.dylib", 0x1000}});
  const auto handle = handles.open_loaded("/fixture.dylib", RTLD_NOW | RTLD_NODELETE);
  if (!handle || !handles.nodelete(0x1000) ||
      !handles.has_open_reference_for_image(0x1000)) return 8;
  if (handles.close(*handle) != 0 || handles.has_open_reference_for_image(0x1000) ||
      !handles.nodelete(0x1000)) return 9;
  std::puts("cxa_owner_filter=lifo dso_filter=opaque reentrant_registration=once nodelete=pinned");
  return 0;
}
