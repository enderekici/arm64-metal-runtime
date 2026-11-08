#include <stdint.h>
#include <stdio.h>

int main(void) {
  const volatile uint8_t *user_page_shift_64 =
      (const volatile uint8_t *)(uintptr_t)0xfffff4025ULL;
  const volatile uint8_t *kernel_page_shift =
      (const volatile uint8_t *)(uintptr_t)0xfffff4037ULL;
  const uint8_t user_shift = *user_page_shift_64;
  const uint8_t kernel_shift = *kernel_page_shift;
  printf("user_page_shift_64=%u kernel_page_shift=%u\n", user_shift, kernel_shift);
  return user_shift >= 32 || kernel_shift >= 32;
}
