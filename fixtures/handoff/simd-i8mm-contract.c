#include <stdint.h>
#include <stdio.h>

typedef uint8_t Bytes __attribute__((vector_size(16)));
typedef uint32_t Words __attribute__((vector_size(16)));

static void report(const char *name, Words value) {
  printf("%s %08x %08x %08x %08x\n", name, value[0], value[1], value[2], value[3]);
}

int main(void) {
  const Bytes left = {128, 2, 253, 4, 5, 240, 7, 127, 129, 254, 0, 64, 162, 110, 11, 224};
  const Bytes right = {255, 32, 131, 2, 127, 128, 5, 3, 255, 128, 7, 2, 4, 1, 250, 191};
  const Words accumulator = {0xfffffff0, 0x80000002, 0x7fffff80, 0xffffffe0};

  Words value = accumulator;
  __asm__ volatile("smmla %0.4s, %1.16b, %2.16b" : "+&w"(value) : "w"(left), "w"(right));
  report("signed", value);
  value = accumulator;
  __asm__ volatile("ummla %0.4s, %1.16b, %2.16b" : "+&w"(value) : "w"(left), "w"(right));
  report("unsigned", value);
  value = accumulator;
  __asm__ volatile("usmmla %0.4s, %1.16b, %2.16b" : "+&w"(value) : "w"(left), "w"(right));
  report("mixed", value);

  value = (Words)left;
  __asm__ volatile("smmla %0.4s, %0.16b, %1.16b" : "+&w"(value) : "w"(right));
  report("dest-left", value);
  value = (Words)right;
  __asm__ volatile("usmmla %0.4s, %1.16b, %0.16b" : "+&w"(value) : "w"(left));
  report("dest-right", value);
  value = (Words)left;
  __asm__ volatile("ummla %0.4s, %0.16b, %0.16b" : "+w"(value));
  report("all-same", value);
  return 0;
}
