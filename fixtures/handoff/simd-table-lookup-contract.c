#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char v16u8 __attribute__((vector_size(16)));
__attribute__((noinline)) static v16u8 tbl(v16u8 a, v16u8 b, v16u8 ix) {
  v16u8 r;
  __asm__ volatile("tbl %0.16b, {%1.16b, %2.16b}, %3.16b"
                   : "=w"(r)
                   : "w"(a), "w"(b), "w"(ix));
  return r;
}
__attribute__((noinline)) static v16u8 tbx(v16u8 d, v16u8 a, v16u8 b,
                                           v16u8 ix) {
  __asm__ volatile("tbx %0.16b, {%1.16b, %2.16b}, %3.16b"
                   : "+w"(d)
                   : "w"(a), "w"(b), "w"(ix));
  return d;
}
static void out(const char *tag, v16u8 v) {
  unsigned char b[16];
  memcpy(b, &v, 16);
  printf("%s", tag);
  for (int i = 0; i < 16; i++)
    printf(" %02x", b[i]);
  putchar('\n');
}
int main(void) {
  v16u8 a, b, ix, d;
  for (int i = 0; i < 16; i++) {
    a[i] = i;
    b[i] = i + 16;
    ix[i] = (unsigned char[]){0,  15, 16, 31, 32, 255, 7,   24,
                              30, 1,  33, 14, 17, 2,   254, 8}[i];
    d[i] = 0x80 + i;
  }
  out("tbl", tbl(a, b, ix));
  out("tbx", tbx(d, a, b, ix));
  return 0;
}
