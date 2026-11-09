#include <stdint.h>
#include <stdio.h>
#include <time.h>

static int64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long solve(long cols, long ld, long rd, int n) {
    long full = (1L << n) - 1;
    if (cols == full) return 1;
    long free = ~(cols | ld | rd) & full;
    long count = 0;
    while (free) {
        long pos = free & (-free);
        free -= pos;
        count += solve(cols | pos, (ld | pos) << 1, (rd | pos) >> 1, n);
    }
    return count;
}

int main(void) {
    int64_t start = now_ns();
    long r = 0;
    for (int col = 0; col < 13 / 2; col++) {
        long pos = 1L << col;
        r += 2 * solve(pos, pos << 1, pos >> 1, 13);
    }
    if (13 & 1) {
        long pos = 1L << (13 / 2);
        r += solve(pos, pos << 1, pos >> 1, 13);
    }
    int64_t end = now_ns();
    printf("checksum=%ld\n", r);
    if (end <= start) end = start + 1;
    printf("elapsed_ns=%lld\n", (long long)(end - start));
    return 0;
}
