#include "guest_image_registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

int main() {
  arm64metal::GuestImageRegistry registry({
      {0x1000, "/guest/main"}, {0x2000, "/guest/dependency"}});
  if (registry.size() != 2 || registry.header_at(0) != 0x1000 ||
      registry.name_at(1) != "/guest/dependency" || registry.header_at(2) ||
      registry.name_at(2)) return 1;

  {
    auto transaction = registry.begin_transaction();
    transaction.add(0x3000, "/guest/staged");
    transaction.add(0x1000, "/guest/duplicate");
  }
  if (registry.size() != 2 || registry.contains(0x3000)) return 2;

  auto transaction = registry.begin_transaction();
  transaction.add(0x3000, "/guest/staged");
  transaction.add(0x4000, "/guest/transitive");
  transaction.commit();
  if (registry.size() != 4 || registry.header_at(2) != 0x3000 ||
      registry.name_at(3) != "/guest/transitive" || !registry.contains(0x4000)) return 3;
  try {
    auto invalid = registry.begin_transaction();
    invalid.add(0, "/guest/invalid");
    return 4;
  } catch (const std::invalid_argument&) {
  }
  return 0;
}
