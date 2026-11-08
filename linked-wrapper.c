#include <unistd.h>
extern int support(void);
__attribute__((constructor))
static void initialize(void) {
    write(STDOUT_FILENO, "bridge init\n", 12);
}
int puts(const char *unused) {
    return support();
}
