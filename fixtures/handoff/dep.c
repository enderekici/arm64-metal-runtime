#include <unistd.h>
__attribute__((constructor)) static void dep_init(void) { (void)write(1, "C_D\n", 4); }
int dyld_add_image_dependency(void) { return 17; }
