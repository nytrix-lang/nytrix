extern int nyc_static_local_section_target;

int nyc_static_local_section_anchor(void) {
  static void * __attribute__((used))
      __attribute__((section(".discard.addressable"))) anchor =
          &nyc_static_local_section_target;
  return anchor != 0;
}
