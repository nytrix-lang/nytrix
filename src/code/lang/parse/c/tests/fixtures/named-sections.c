__attribute__((__section__(".probe.data"))) long nyc_section_tag = 7;
__attribute__((__section__(".probe.data"))) long nyc_section_bss;
int nyc_section_host = 11;
__attribute__((__section__(".probe.ptrs"))) int *nyc_section_ptr =
    &nyc_section_host;
extern char nyc_section_extern_msg[];
char nyc_section_msg[] __attribute__((__section__(".probe.data"))) =
    "ok";
long nyc_section_read(void) { return nyc_section_tag + nyc_section_bss +
    *nyc_section_ptr + (nyc_section_msg[0] - 'o'); }
