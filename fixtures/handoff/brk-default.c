int main(void) {
  __asm__ volatile("brk #0x1234" ::: "memory");
  return 0;
}
