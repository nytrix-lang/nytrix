/*
 * Host caller for the NYC static wide-character array fixture.
 */
unsigned long nyc_static_wide_array_value(void);

int main(void) { return nyc_static_wide_array_value() != 223; }
