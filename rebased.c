#include <stdio.h>

extern int first(int);
int (*volatile operation)(int) = first;
int (*volatile output)(const char *) = puts;
const char *volatile message = "global pointer";

int main(int argc, char **argv) {
    output(message);
    return operation(argc);
}
