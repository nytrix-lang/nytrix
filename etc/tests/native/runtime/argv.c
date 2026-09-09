/*
 * Standalone runtime regression (includes the private argv decoder).
 * cc -std=gnu11 -O1 -D_GNU_SOURCE -Isrc -ffunction-sections -fdata-sections \
 *   etc/tests/native/runtime/argv.c -Wl,--gc-sections -lm -ldl -lpthread \
 *   -lffi -o /tmp/nytrix-argv-test && /tmp/nytrix-argv-test
 */
#include "code/runtime/init.c"
#include <assert.h>

int main(void) {
  bool owned = true;
  assert(ny_native_argv(0, &owned) == NULL && !owned);

  int64_t empty = rt_tbuf_new_raw(0, 24);
  assert(empty);
  char **argv = ny_native_argv(empty, &owned);
  assert(argv && owned && argv[0] == NULL);
  free(argv);

  _Alignas(8) char odd_storage[] = "!argument";
  char *odd = odd_storage + 1;
  assert(((uintptr_t)odd & 1) != 0);
  int64_t args = rt_tbuf_new_raw(2, 24);
  assert(args);
  int64_t *slots = (int64_t *)(uintptr_t)args;
  slots[0] = (int64_t)(uintptr_t)odd;
  slots[1] = 8;
  slots[2] = TAG_STR;
  slots[3] = (int64_t)(uintptr_t)"second";
  slots[4] = 6;
  slots[5] = TAG_STR;
  argv = ny_native_argv(args, &owned);
  assert(argv && owned && argv[0] == odd);
  assert(strcmp(argv[0], "argument") == 0);
  assert(strcmp(argv[1], "second") == 0 && argv[2] == NULL);
  free(argv);

  int64_t raw = rt_tbuf_new_raw(1, 8);
  assert(raw);
  *(int64_t *)(uintptr_t)raw = (int64_t)(uintptr_t)odd;
  argv = ny_native_argv(raw, &owned);
  assert(argv && owned && argv[0] == odd && argv[1] == NULL);
  free(argv);

  /*
   * Legacy tagged pointer slots still use their original decoding.
   */
  int64_t legacy[8] = {0};
  legacy[3] = TAG_LIST;
  legacy[4] = rt_tag_v(1);
  legacy[6] = rt_tag_v((int64_t)(uintptr_t)odd);
  argv = ny_native_argv((intptr_t)&legacy[4], &owned);
  assert(argv && owned && argv[0] == odd && argv[1] == NULL);
  free(argv);

  int64_t *hdr = (int64_t *)((uintptr_t)raw - RT_NATIVE_TBUF_HEADER);
  hdr[1] = hdr[3] + 1;
  assert(ny_native_argv(raw, &owned) == NULL && !owned);
  free(hdr);
  free((void *)((uintptr_t)args - RT_NATIVE_TBUF_HEADER));
  free((void *)((uintptr_t)empty - RT_NATIVE_TBUF_HEADER));
  puts("argv layouts: ok");
  return 0;
}
