#include "guest_image_registry.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

int main() {
  arm64metal::GuestImageRegistry registry({
      {0x1000, "/guest/main"}, {0x2000, "/guest/dependency"},
      {0x5000, "/guest/no-slide"}});
  const std::unordered_map<std::uint64_t, std::uint64_t> slides{
      {0x1000, 0x5000},
      {0x2000, static_cast<std::uint64_t>(static_cast<std::int64_t>(-0x1000))}};
  if (registry.size() != 3 || registry.header_at(0) != 0x1000 ||
      registry.name_at(1) != "/guest/dependency" || registry.header_at(3) ||
      registry.name_at(3) ||
      arm64metal::guest_image_vmaddr_slide(registry, 0, slides) != 0x5000 ||
      arm64metal::guest_image_vmaddr_slide(registry, 1, slides) != slides.at(0x2000) ||
      arm64metal::guest_image_vmaddr_slide(registry, 3, slides) != 0) return 1;
  try {
    (void)arm64metal::guest_image_vmaddr_slide(registry, 2, slides);
    return 5;
  } catch (const std::logic_error&) {
  }

  {
    auto transaction = registry.begin_transaction();
    transaction.add(0x3000, "/guest/staged");
    transaction.add(0x1000, "/guest/duplicate");
  }
  if (registry.size() != 3 || registry.contains(0x3000)) return 2;

  auto transaction = registry.begin_transaction();
  transaction.add(0x3000, "/guest/staged");
  transaction.add(0x4000, "/guest/transitive");
  transaction.commit();
  if (registry.size() != 5 || registry.header_at(3) != 0x3000 ||
      registry.name_at(4) != "/guest/transitive" || !registry.contains(0x4000)) return 3;
  try {
    auto invalid = registry.begin_transaction();
    invalid.add(0, "/guest/invalid");
    return 4;
  } catch (const std::invalid_argument&) {
  }
  registry.remove({0x2000, 0x4000, 0xdead});
  if (registry.size() != 3 || registry.header_at(0) != 0x1000 ||
      registry.header_at(1) != 0x5000 || registry.name_at(1) != "/guest/no-slide" ||
      registry.header_at(2) != 0x3000 || registry.name_at(2) != "/guest/staged" ||
      registry.contains(0x2000) || registry.contains(0x4000)) return 6;
  return 0;
}
