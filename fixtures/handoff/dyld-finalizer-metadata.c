#include <unistd.h>

__attribute__((constructor)) static void image_initialize(void) {
    (void)write(STDOUT_FILENO, "native-init\n", 12);
}

__attribute__((destructor)) static void image_finalize(void) {
    (void)write(STDOUT_FILENO, "native-final\n", 13);
}

// The raw section tests Mach-O metadata parsing; the attribute above separately
// exercises native Cxa finalization during dlclose.
__attribute__((used, section("__DATA,__mod_term_func")))
static void (*const image_terminator)(void) = image_finalize;

int dyld_finalizer_fixture_value(void) {
    return 42;
}
