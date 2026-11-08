#include <stdio.h>
#include <stdint.h>

extern uint32_t score(const char *s);

int main(int argc, char **argv) {
    const char *text = argc > 1 ? argv[1] : "metal";
    uint32_t result = score(text);
    puts(text);
    return (int)(result & 127);
}
