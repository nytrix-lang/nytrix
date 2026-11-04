#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char key[16];
  int value;
} Slot;

static size_t widths(Slot *slots, int *effects) {
  size_t result = sizeof(slots[(*effects)++].key);
  result += sizeof slots[(*effects)++].value;
  result += _Generic(slots[(*effects)++].value, int: 7, default: 1000);
  char extent[sizeof(*(slots + 0))];
  return result + sizeof(extent);
}

int main(void) {
  struct {
    int matrix[2][3];
  } arrays[1];
  if (sizeof(arrays[0].matrix) != 24 || sizeof(arrays[0].matrix[0]) != 12 ||
      sizeof(arrays[0].matrix[0][0]) != 4)
    return 2;
  Slot *slots = calloc(2, sizeof(Slot));
  if (!slots)
    return 1;
  slots[0].value = 17;
  slots[1].value = 23;
  char key[16];
  int total = 0;
  for (int i = 0; i < 2; ++i) {
    snprintf(key, sizeof(key), "%d", i);
    strncpy(slots[i].key, key, sizeof(slots[i].key) - 1);
    total += slots[i].value;
  }
  int effects = 0;
  int failed = widths(slots, &effects) != 47 || effects != 0 || total != 40 ||
               strcmp(slots[0].key, "0") != 0 || strcmp(slots[1].key, "1") != 0;
  free(slots);
  return failed;
}
