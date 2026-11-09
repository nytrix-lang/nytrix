#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(void) {
    const int N = 100000;
    unsigned char *codes = malloc((size_t)N);
    int seed = 42;
    for (int i = 0; i < N; i++) {
        seed = (seed * 3877 + 29573) % 139968;
        codes[i] = (unsigned char)(seed % 4);
    }
    int64_t cnt[16] = {0};
    int64_t start = now_ns();
    for (int i = 0; i < N - 1; i++) cnt[codes[i] * 4 + codes[i + 1]]++;
    int64_t ck = 0;
    for (int c = 0; c < 16; c++) ck += cnt[c] * (c + 1);
    int64_t end = now_ns();
    free(codes);
    printf("checksum=%lld\n", (long long)ck);
    if (end <= start) end = start + 1;
    printf("elapsed_ns=%lld\n", (long long)(end - start));
    return 0;
}
