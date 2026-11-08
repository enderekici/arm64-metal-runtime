#include <unistd.h>

__attribute__((constructor)) static void image_initialize(void) {
    (void)write(STDOUT_FILENO, "native-init\n", 12);
}

__attribute__((destructor)) static void cxa_finalize(void) {
    (void)write(STDOUT_FILENO, "native-cxa-final\n", 17);
}

void dyld_raw_mod_term(void) {
    (void)write(STDOUT_FILENO, "native-mod-term\n", 16);
}

// This raw section is parsed separately from the native Cxa dtor. The test
// identifies which callback mechanism the system dyld runs on dlclose.
__attribute__((used, section("__DATA,__mod_term_func")))
static void (*const image_terminator)(void) = dyld_raw_mod_term;

int dyld_finalizer_fixture_value(void) {
    return 42;
}
