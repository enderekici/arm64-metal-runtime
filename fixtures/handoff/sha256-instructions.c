#include <arm_neon.h>
#include <stdint.h>
#include <unistd.h>

static void print_vector(const char *name, uint32x4_t value) {
  static const char hex[] = "0123456789abcdef";
  char line[128];
  size_t length = 0;
  while (name[length] != '\0') {
    line[length] = name[length];
    ++length;
  }
  line[length++] = '=';
  uint32_t lanes[4];
  vst1q_u32(lanes, value);
  for (size_t lane = 0; lane < 4; ++lane) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      line[length++] = hex[(lanes[lane] >> shift) & 0xfu];
    }
    line[length++] = lane == 3 ? '\n' : ',';
  }
  (void)write(STDOUT_FILENO, line, length);
}

static uint32x4_t sha256h_destination_source1_alias(uint32x4_t abcd,
                                                   uint32x4_t schedule) {
  uint32x4_t result;
  __asm__ volatile("mov v0.16b, %1.16b\n"
                   "mov v1.16b, %2.16b\n"
                   "sha256h q0, q0, v1.4s\n"
                   "mov %0.16b, v0.16b"
                   : "=w"(result) : "w"(abcd), "w"(schedule) : "v0", "v1");
  return result;
}

static uint32x4_t sha256h2_destination_source2_alias(uint32x4_t destination,
                                                    uint32x4_t abcd) {
  uint32x4_t result;
  __asm__ volatile("mov v0.16b, %1.16b\n"
                   "mov v1.16b, %2.16b\n"
                   "sha256h2 q0, q1, v0.4s\n"
                   "mov %0.16b, v0.16b"
                   : "=w"(result) : "w"(destination), "w"(abcd) : "v0", "v1");
  return result;
}

int main(void) {
  const uint32x4_t schedule0 = {0x01020304u, 0x11223344u, 0x55667788u, 0x99aabbccu};
  const uint32x4_t schedule1 = {0x0f1e2d3cu, 0x4b5a6978u, 0x8796a5b4u, 0xc3d2e1f0u};
  const uint32x4_t schedule2 = {0x01234567u, 0x89abcdefu, 0xfedcba98u, 0x76543210u};
  const uint32x4_t schedule3 = {0xdeadbeefu, 0x10293847u, 0x56473829u, 0xabcdef01u};

  print_vector("su0-distinct", vsha256su0q_u32(schedule0, schedule1));
  uint32x4_t su0_alias = schedule0;
  __asm__ volatile("sha256su0 %0.4s, %0.4s" : "+w"(su0_alias));
  print_vector("su0-alias", su0_alias);

  print_vector("su1-distinct", vsha256su1q_u32(schedule0, schedule1, schedule2));
  uint32x4_t su1_n_alias = schedule0;
  __asm__ volatile("sha256su1 %0.4s, %0.4s, %1.4s"
                   : "+w"(su1_n_alias) : "w"(schedule2));
  print_vector("su1-destination-source1-alias", su1_n_alias);
  uint32x4_t su1_m_alias = schedule0;
  __asm__ volatile("sha256su1 %0.4s, %1.4s, %0.4s"
                   : "+w"(su1_m_alias) : "w"(schedule1));
  print_vector("su1-destination-source2-alias", su1_m_alias);

  uint32x4_t abcd = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au};
  const uint32x4_t abcd_initial = abcd;
  uint32x4_t efgh = {0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  uint32x4_t round_words = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u};
  abcd = vsha256hq_u32(abcd, efgh, round_words);
  print_vector("sha256h-alias", abcd);
  efgh = vsha256h2q_u32(efgh, abcd_initial, round_words);
  print_vector("sha256h2-alias", efgh);

  print_vector("sha256h-destination-source1-alias",
               sha256h_destination_source1_alias(abcd_initial, round_words));
  print_vector("sha256h2-destination-source2-alias",
               sha256h2_destination_source2_alias(round_words, abcd_initial));

  uint32x4_t hash_abcd = {0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u};
  uint32x4_t hash_efgh = {0xa4093822u, 0x299f31d0u, 0x082efa98u, 0xec4e6c89u};
  print_vector("sha256h-distinct", vsha256hq_u32(hash_abcd, hash_efgh, schedule3));
  print_vector("sha256h2-distinct", vsha256h2q_u32(hash_efgh, hash_abcd, schedule3));
  return 0;
}
