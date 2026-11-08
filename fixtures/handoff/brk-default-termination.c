#include <unistd.h>

int main(void) {
  static const char before[] = "before\n";
  static const char after[] = "after\n";
  if (write(STDOUT_FILENO, before, sizeof(before) - 1) !=
      (ssize_t)(sizeof(before) - 1)) return 2;
  __asm__ volatile("brk #1" ::: "cc", "memory");
  (void)write(STDOUT_FILENO, after, sizeof(after) - 1);
  return 1;
}
