#include <stdio.h>

extern int first(int);
extern int second(int);

int main(int argc, char **argv) {
    int (*operation)(int) = argc > 1 ? first : second;
    int result = operation(argc);
    puts(argc > 1 ? "first" : "second");
    return result;
}
