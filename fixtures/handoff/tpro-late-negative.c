#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

extern void _dyld_call_with_writable_tpro_memory(void (*callback)(void *), void *context);

struct CallbackState {
  uint64_t value;
  int called;
};

static void callback(void *opaque) {
  static const char marker[] = "CALLBACK_ENTERED\n";
  (void)write(STDERR_FILENO, marker, sizeof(marker) - 1);
  struct CallbackState *state = opaque;
  ++state->called;
  state->value ^= UINT64_C(0x5a5a1234);
}

int main(void) {
  struct CallbackState state = {UINT64_C(0x12345678), 0};
  fprintf(stderr, "BEFORE called=%d value=%llx\n", state.called,
          (unsigned long long)state.value);
  _dyld_call_with_writable_tpro_memory(callback, &state);
  fprintf(stderr, "AFTER called=%d value=%llx\n", state.called,
          (unsigned long long)state.value);
  return state.called != 1 ||
      state.value != (UINT64_C(0x12345678) ^ UINT64_C(0x5a5a1234));
}
