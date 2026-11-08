extern int dyld_image_dependency_value(void);
int root_value(void) { return dyld_image_dependency_value() + 1; }
