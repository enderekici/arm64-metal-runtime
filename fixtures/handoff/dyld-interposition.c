#include <stdbool.h>
#include <stdio.h>
extern bool dyld_has_inserted_or_interposing_libraries(void);
int main(void) {
  bool active = dyld_has_inserted_or_interposing_libraries();
  printf("interposition=%d\n", active);
  return active;
}
