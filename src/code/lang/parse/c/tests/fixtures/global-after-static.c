static int local_static_predecessor(void) {
  static int count;
  return ++count;
}
int nyc_external_after_local_static = 37;
int nyc_external_after_local_static_read(void) {
  return nyc_external_after_local_static + (local_static_predecessor() == 1 ? 0 : 100);
}
