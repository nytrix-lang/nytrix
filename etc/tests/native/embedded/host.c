/*
 * Minimal C host for an emitted Nytrix object.
 *
 * Build flow:
 *   ny --native-backend x86_64 -emit-only -o app.o app.ny
 *   cc -O2 -o host host.c app.o -L<rt-lib-dir> -lnytrixrt
 *
 * rt_main is the entry the native emitter writes for top-level code; it
 * returns the program's exit value. Link against libnytrixrt for the
 * runtime helpers (rt_print_i64_raw, tbuf/dict, bigint, ...).
 */
#include <stdio.h>

extern long rt_main(void);

int main(void) {
    long rc = rt_main();
    if (rc != 0) {
        fprintf(stderr, "nytrix app failed: %ld\n", rc);
        return 1;
    }
    puts("host: nytrix app ok");
    return 0;
}
