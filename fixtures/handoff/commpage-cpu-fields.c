#include <stdint.h>
#include <stdio.h>

int main(void) {
  const volatile uint8_t *cpu_clusters =
      (const volatile uint8_t *)(uintptr_t)0xfffffc02fULL;
  const volatile uint8_t *physical_cpus =
      (const volatile uint8_t *)(uintptr_t)0xfffffc035ULL;
  const volatile uint8_t *logical_cpus =
      (const volatile uint8_t *)(uintptr_t)0xfffffc036ULL;
  printf("cpu_clusters=%u physical_cpus=%u logical_cpus=%u\n",
      *cpu_clusters, *physical_cpus, *logical_cpus);
  return 0;
}
