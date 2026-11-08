#include <unistd.h>
const char *volatile prefix = "guest library\n";
volatile int initialized;

__attribute__((constructor))
static void initialize(void) {
    write(STDOUT_FILENO, "support init\n", 13);
    initialized = 28;
}
int support(void) {
    return (int)write(STDOUT_FILENO, prefix, 14) + initialized;
}
