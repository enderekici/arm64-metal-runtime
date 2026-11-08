#include <stdint.h>

extern uint64_t bl_nested(void);

int main(void) {
  return bl_nested() == 42 ? 42 : 99;
}
