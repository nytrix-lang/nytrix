/*
 * C fixture exposing the current frame address and a local variable address.
 */
void *nyc_frame_address_matches(void **local_address) {
  long local;
  *local_address = &local;
  return __builtin_frame_address(0);
}
