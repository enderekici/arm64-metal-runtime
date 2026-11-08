#include <dlfcn.h>
#include <stdio.h>
#include <zlib.h>
typedef unsigned long (*crc_fn)(unsigned long, const unsigned char *, unsigned int);
int main(void) {
  const char *path = "/usr/lib/libz.1.dylib";
  void *before = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
  void *first = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!first) { puts(dlerror()); return 1; }
  crc_fn crc = (crc_fn)dlsym(first, "crc32");
  void *after = dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
  const unsigned char input[] = {'M', 'e', 't', 'a', 'l'};
  unsigned long value = crc ? crc(0, input, sizeof(input)) : 0;
  printf("before=%d after_same=%d crc=%lu\n", before != NULL, after == first, value);
  return before || after != first || value != 27515853UL;
}
