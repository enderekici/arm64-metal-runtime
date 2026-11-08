#define _DARWIN_C_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*root_value_fn)(void);

static int check_snapshot(const char *tag, uint32_t expectedMinimum) {
  const uint32_t count = _dyld_image_count();
  if (count < expectedMinimum || _dyld_get_image_header(count) != NULL ||
      _dyld_get_image_name(count) != NULL) {
    fprintf(stderr, "%s: invalid count or out-of-range result\n", tag);
    return 1;
  }
  for (uint32_t index = 0; index < count; ++index) {
    const struct mach_header *header = _dyld_get_image_header(index);
    const char *name = _dyld_get_image_name(index);
    if (header == NULL || name == NULL || header->magic != MH_MAGIC_64) {
      fprintf(stderr, "%s: invalid image entry %u\n", tag, index);
      return 1;
    }
  }
  printf("%s count=%u first=%s\n", tag, count, _dyld_get_image_name(0));
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 4) return 64;
  const uint32_t before = _dyld_image_count();
  if (check_snapshot("before", before) != 0) return 1;

  if (dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) != NULL || dlerror() == NULL) {
    fprintf(stderr, "missing image unexpectedly loaded\n");
    return 2;
  }
  if (_dyld_image_count() != before || check_snapshot("after-failed-load", before) != 0) {
    fprintf(stderr, "failed dlopen changed the image snapshot\n");
    return 3;
  }

  void *handle = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 4;
  }
  dlerror();
  root_value_fn root_value = (root_value_fn)dlsym(handle, "root_value");
  if (root_value == NULL || root_value() != 8) return 5;
  const uint32_t after = _dyld_image_count();
  if (after != before + 2 || check_snapshot("after-successful-load", after) != 0) {
    fprintf(stderr, "successful dlopen did not publish root and dependency\n");
    return 6;
  }
  char expectedRoot[PATH_MAX];
  char expectedDependency[PATH_MAX];
  if (realpath(argv[2], expectedRoot) == NULL ||
      realpath(argv[3], expectedDependency) == NULL) return 7;
  const char *rootName = _dyld_get_image_name(before);
  const char *dependencyName = _dyld_get_image_name(before + 1);
  if (rootName == NULL || dependencyName == NULL ||
      strcmp(rootName, expectedRoot) != 0 ||
      strcmp(dependencyName, expectedDependency) != 0) {
    fprintf(stderr, "unexpected dynamic image order: %s, %s\n",
            rootName ? rootName : "<null>", dependencyName ? dependencyName : "<null>");
    return 8;
  }
  printf("dynamic-order root=%s dependency=%s\n", rootName, dependencyName);
  return 0;
}
