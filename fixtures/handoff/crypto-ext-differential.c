// Native-vs-arm64-metal differential for the crypto-extension encodings the
// opcode sweep found in libcorecrypto (inventory/opcode-histogram.txt).
// Each op runs on edge-case and xorshift vectors; one "<op> <hash>" line per op.
// Build: xcrun clang -arch arm64 -O1 -fno-stack-protector crypto-ext-differential.c
#include <stdint.h>
#include <stdio.h>

typedef uint64_t V __attribute__((vector_size(16)));
static uint64_t st = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static uint64_t pick(void) {
  switch (rnd() % 8) {
    case 0: return 0;
    case 1: return ~0ull;
    case 2: return 0x8000000000000000ull;
    case 3: return 0x7fffffffffffffffull;
    case 4: return 0x0123456789abcdefull;
    default: return rnd();
  }
}
static uint64_t h;
static void mix(V v) {
  for (int i = 0; i < 2; ++i) { h ^= v[i]; h *= 1099511628211ull; }
}
typedef V (*Fn)(V, V, V, V);
// d = destination (also read by accumulating ops), n/m/a = sources.
#define OP(nm, ins)                                                          \
  static V f_##nm(V d, V n, V m, V a) {                                      \
    __asm__ volatile("mov v16.16b, %0.16b\n mov v17.16b, %1.16b\n"           \
                     "mov v18.16b, %2.16b\n mov v19.16b, %3.16b\n" ins       \
                     "\n mov %0.16b, v16.16b"                                \
                     : "+w"(d) : "w"(n), "w"(m), "w"(a)                      \
                     : "v16", "v17", "v18", "v19");                          \
    return d;                                                                \
  }
#define ARCH ".arch armv8.4-a+sha3+sha2\n"
OP(eor3, ARCH "eor3 v16.16b, v17.16b, v18.16b, v19.16b")
OP(eor3_alias, ARCH "eor3 v16.16b, v16.16b, v16.16b, v19.16b")
OP(bcax, ARCH "bcax v16.16b, v17.16b, v18.16b, v19.16b")
OP(bcax_alias, ARCH "bcax v16.16b, v17.16b, v16.16b, v16.16b")
OP(rax1, ARCH "rax1 v16.2d, v17.2d, v18.2d")
OP(rax1_alias, ARCH "rax1 v16.2d, v16.2d, v16.2d")
OP(xar0, ARCH "xar v16.2d, v17.2d, v18.2d, #0")
OP(xar1, ARCH "xar v16.2d, v17.2d, v18.2d, #1")
OP(xar8, ARCH "xar v16.2d, v17.2d, v18.2d, #8")
OP(xar63, ARCH "xar v16.2d, v17.2d, v18.2d, #63")
OP(sha512h, ARCH "sha512h q16, q17, v18.2d")
OP(sha512h_alias, ARCH "sha512h q16, q16, v16.2d")
OP(sha512h2, ARCH "sha512h2 q16, q17, v18.2d")
OP(sha512h2_alias, ARCH "sha512h2 q16, q17, v16.2d")
OP(sha512su0, ARCH "sha512su0 v16.2d, v17.2d")
OP(sha512su0_alias, ARCH "sha512su0 v16.2d, v16.2d")
OP(sha512su1, ARCH "sha512su1 v16.2d, v17.2d, v18.2d")
OP(sha512su1_alias, ARCH "sha512su1 v16.2d, v16.2d, v17.2d")

#define ENTRY(nm) {#nm, f_##nm}
static const struct { const char *name; Fn fn; } ops[] = {
  ENTRY(eor3), ENTRY(eor3_alias), ENTRY(bcax), ENTRY(bcax_alias), ENTRY(rax1),
  ENTRY(rax1_alias), ENTRY(xar0), ENTRY(xar1), ENTRY(xar8), ENTRY(xar63),
  ENTRY(sha512h), ENTRY(sha512h_alias), ENTRY(sha512h2), ENTRY(sha512h2_alias),
  ENTRY(sha512su0), ENTRY(sha512su0_alias), ENTRY(sha512su1), ENTRY(sha512su1_alias),
};

int main(void) {
  for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; ++i) {
    h = 1469598103934665603ull;
    st = 0x9e3779b97f4a7c15ull + i;
    for (int t = 0; t < 64; ++t) {
      V d = {pick(), pick()}, n = {pick(), pick()}, m = {pick(), pick()}, a = {pick(), pick()};
      mix(ops[i].fn(d, n, m, a));
    }
    printf("%s %016llx\n", ops[i].name, (unsigned long long)h);
  }
  fflush(stdout);
  return 0;
}
