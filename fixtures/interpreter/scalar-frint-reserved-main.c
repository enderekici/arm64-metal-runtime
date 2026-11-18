#ifndef RESERVED_FRINT_ENCODING
#error RESERVED_FRINT_ENCODING must be defined
#endif

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

int main(void) {
  __asm__ volatile(".word " STRINGIFY(RESERVED_FRINT_ENCODING));
  return 0;
}
