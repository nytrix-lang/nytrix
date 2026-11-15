/*
 * Runtime harness for reading data emitted into named object sections.
 */
char nyc_section_extern_msg[4] = {0};
long nyc_section_read(void);
int main(void) { return nyc_section_read() != 18; }
