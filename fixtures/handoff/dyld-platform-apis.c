#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint32_t (*base_platform_fn)(uint32_t platform);
typedef uint32_t (*active_platform_fn)(void);
typedef bool (*simulator_platform_fn)(uint32_t platform);

static uint32_t expected_base(uint32_t platform) {
  switch (platform) {
    case 6:
    case 7:
      return 2;
    case 8:
      return 3;
    case 9:
      return 4;
    case 12:
      return 11;
    default:
      return platform;
  }
}

static bool expected_simulator(uint32_t platform) {
  return platform == 7 || platform == 8 || platform == 9 || platform == 12;
}

int main(void) {
  const base_platform_fn base =
      (base_platform_fn)dlsym(RTLD_DEFAULT, "dyld_get_base_platform");
  const active_platform_fn active =
      (active_platform_fn)dlsym(RTLD_DEFAULT, "dyld_get_active_platform");
  const simulator_platform_fn simulator =
      (simulator_platform_fn)dlsym(RTLD_DEFAULT, "dyld_is_simulator_platform");
  if (!base || !active || !simulator) return 1;

  const uint32_t edge_platforms[] = {UINT32_MAX, 0x7fffffffU, 100, 255, 1024};
  for (uint32_t platform = 0; platform < 30; ++platform) {
    if (base(platform) != expected_base(platform) ||
        simulator(platform) != expected_simulator(platform)) {
      fprintf(stderr, "dyld platform mismatch for %u\n", platform);
      return 2;
    }
  }
  for (size_t index = 0; index < sizeof(edge_platforms) / sizeof(edge_platforms[0]);
       ++index) {
    const uint32_t platform = edge_platforms[index];
    if (base(platform) != expected_base(platform) || simulator(platform)) {
      fprintf(stderr, "dyld platform edge mismatch for %u\n", platform);
      return 3;
    }
  }
  if (active() != 1) {
    fprintf(stderr, "unexpected active platform %u\n", active());
    return 4;
  }
  puts("dyld-platform base=ok active=1 simulator=ok edges=ok");
  return 0;
}
