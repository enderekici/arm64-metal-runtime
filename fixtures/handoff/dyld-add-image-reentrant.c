#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static const char *nested_path;
static void *nested_handle;
static char events[64];
static size_t event_count;

static void record(char event) {
  if (event_count < sizeof(events) - 1) events[event_count++] = event;
  events[event_count] = 0;
}

static void added(const struct mach_header *header, intptr_t slide) {
  (void)slide;
  Dl_info info = {0};
  if (!dladdr((const void *)header, &info) || info.dli_fname == NULL) return;
  const char *leaf = strrchr(info.dli_fname, '/');
  leaf = leaf == NULL ? info.dli_fname : leaf + 1;
  if (strcmp(leaf, "libdyld-add-image-new.dylib") == 0) {
    record('R'); (void)write(1, "A_R\n", 4);
    nested_handle = dlopen(nested_path, RTLD_NOW | RTLD_LOCAL);
    record(nested_handle == NULL ? 'e' : 'r');
  } else if (strcmp(leaf, "libdyld-add-image-dep.dylib") == 0) {
    record('D'); (void)write(1, "A_D\n", 4);
  } else if (strcmp(leaf, "libdyld-add-image-nested.dylib") == 0) {
    record('N'); (void)write(1, "A_N\n", 4);
  }
}

int main(int argc, char **argv) {
  if (argc != 4) return 64;
  nested_path = argv[3];
  _dyld_register_func_for_add_image(added);
  const unsigned initial = _dyld_image_count();
  void *bad = dlopen(argv[2], RTLD_NOW);
  const unsigned failed = _dyld_image_count() - initial;
  if (bad != NULL || failed != 0 || event_count != 0) return 1;
  void *root = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  record(root == NULL ? 'e' : 'O');
  const unsigned added_count = _dyld_image_count() - initial;
  printf("initial=%u failed=%u added=%u events=%s nested=%d\n",
         initial, failed, added_count, events, nested_handle != NULL);
  if (root != NULL) dlclose(root);
  if (nested_handle != NULL) dlclose(nested_handle);
  return root == NULL || nested_handle == NULL || added_count != 3 ||
         strcmp(events, "RNrDO") != 0;
}
