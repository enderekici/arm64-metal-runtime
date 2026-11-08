#include <stdint.h>

extern uint64_t guest_main(void);

int main(void) {
  return (int)guest_main();
}
