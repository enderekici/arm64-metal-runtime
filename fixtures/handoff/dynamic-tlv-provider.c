__thread int dynamic_tls_value = 9;

int dynamic_tls_get(void) { return dynamic_tls_value; }

void dynamic_tls_set(int value) { dynamic_tls_value = value; }
