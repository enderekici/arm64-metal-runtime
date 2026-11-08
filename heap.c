#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    volatile unsigned char *value = malloc(16);
    if (!value) return 1;
    value[0] = 42;
    value[15] = 9;
    const size_t extent = 65 * 1024 * 1024;
    volatile unsigned char *wide = malloc(extent);
    if (!wide || value[0] != 42 || value[15] != 9) return 6;
    wide[extent - 1] = 71;
    volatile unsigned char *larger = realloc((void *)value, 64);
    if (!larger || larger[0] != 42 || larger[15] != 9 || wide[extent - 1] != 71) return 2;
    free((void *)wide);
    free((void *)larger);
    volatile unsigned char *zero = calloc(64, 1);
    if (!zero) return 3;
    for (int i = 0; i < 64; ++i) if (zero[i] != 0) return 4;
    free((void *)zero);
    void *aligned = NULL;
    if (posix_memalign(&aligned, 4096, 65) || !aligned || ((uintptr_t)aligned & 4095)) return 8;
    volatile unsigned char *bytes = aligned;
    bytes[64] = 55;
    void *same = aligned;
    if (posix_memalign(&same, 24, 64) != EINVAL || same != aligned) return 9;
    volatile size_t too_large = SIZE_MAX;
    if (posix_memalign(&same, 64, too_large) != ENOMEM || same != aligned) return 10;
    volatile unsigned char *resized = realloc(aligned, 129);
    if (!resized || resized[64] != 55) return 11;
    free((void *)resized);
    volatile int letter = 'A';
    if (!isalpha(letter) || isdigit(letter)) return 7;
    return write(STDOUT_FILENO, "heap\n", 5) == 5 ? 0 : 5;
}
