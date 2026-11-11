/*
 * Runtime harness checking builtin frame-address relationships.
 */
#include <stdint.h>
void *nyc_frame_address_matches(void **);
int main(void) {
  void *local;
  void *frame = nyc_frame_address_matches(&local);
  return (uintptr_t)frame <= (uintptr_t)local;
}
