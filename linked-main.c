#include <stdio.h>
#include <unistd.h>

int (*volatile output)(const char *) = puts;
extern int support(void);

__attribute__((constructor))
static void initialize(void) {
    write(STDOUT_FILENO, "main init\n", 10);
}
int main(int argc, char **argv) {
    if (argc > 2) return support();
    return argc > 1 ? output(argv[1]) : puts("hello");
}
