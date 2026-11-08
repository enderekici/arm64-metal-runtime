#include <stdio.h>

extern unsigned long return_x42(void);
extern unsigned int return_w_beef(void);

int main(void) {
    const unsigned long wide = return_x42();
    const unsigned int narrow = return_w_beef();
    printf("wide=%lu narrow=%u\n", wide, narrow);
    return wide == 42 && narrow == 0xbeef0000u ? 0 : 1;
}
