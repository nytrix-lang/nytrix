/*
 * Linkage fixture defining distinct private helpers and a second entry point.
 */
static int shared_private_helper(void) { return 41; }
static int shared_private_data = 3;
int linkage_b(void) {
  return shared_private_helper() + "shared"[0] + shared_private_data;
}
