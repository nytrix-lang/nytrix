/*
 * Host fixture: verify reads from local data stored in custom sections.
 */
int nyc_static_local_section(void);
int main(void) {
  return nyc_static_local_section() != 'b' || nyc_static_local_section() != 'c';
}
