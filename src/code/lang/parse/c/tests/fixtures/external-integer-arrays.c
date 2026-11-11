/*
 * Fixture: compare external array symbols and preserve section attributes.
 */
typedef int nyc_external_array_entry_t;
extern nyc_external_array_entry_t nyc_external_array_start[],
    nyc_external_array_end[];
extern unsigned char nyc_read_mostly_key[256]
    __attribute__((section(".data..read_mostly")));
int nyc_external_arrays_equal(void) {
  return nyc_external_array_start == nyc_external_array_end;
}
