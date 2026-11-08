#include <unistd.h>
extern int dyld_add_image_dependency(void);
__attribute__((constructor)) static void root_init(void) { (void)write(1, "C_R\n", 4); }
int dyld_add_image_value(void) { return dyld_add_image_dependency() + 25; }
