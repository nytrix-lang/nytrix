int nyc_static_local_section(void) {
  static char value[4] __attribute__((section(".nyc.local.data"))) = "a";
  static const char exact[16] = "0123456789ABCDEF";
  if (exact[15] != 'F')
    return -1;
  return ++value[0];
}
