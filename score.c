#include <stdint.h>
uint32_t score(const char *s) {
    uint32_t sum = 0;
    while (*s) {
        sum += (unsigned char)*s++;
    }
    return sum;
}
