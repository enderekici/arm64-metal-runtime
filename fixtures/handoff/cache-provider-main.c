#include <dlfcn.h>
#include <stdio.h>
typedef unsigned int (*crc_fn)(void);
int main(int argc, char **argv) {
  if (argc != 2) return 1;
  void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!handle) { puts(dlerror()); return 2; }
  crc_fn crc = (crc_fn)dlsym(handle, "guest_zlib_crc");
  if (!crc) return 3;
  unsigned int checksum = crc();
  printf("crc=%u\n", checksum);
  return checksum != 27515853U;
}
