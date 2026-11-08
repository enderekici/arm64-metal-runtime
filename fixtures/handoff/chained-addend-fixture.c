extern int chained_addend_target;

int *const chained_addend_reference = &chained_addend_target + 3;

int chained_addend_read(void) { return *chained_addend_reference; }
