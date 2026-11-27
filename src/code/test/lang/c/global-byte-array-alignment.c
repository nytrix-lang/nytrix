/*
 * A file-scope byte array keeps its aligned attribute and does not inherit a neighbor's.
 */
typedef unsigned char uint8_t;
const uint8_t nyc_zero_page[4096] __attribute__((aligned(4096)));
uint8_t nyc_small[16] __attribute__((aligned(16)));
