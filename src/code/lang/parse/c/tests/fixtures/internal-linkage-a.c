static int shared_private_helper(void) { return 40; }
static int shared_private_data = 2;
int linkage_a(void) { return shared_private_helper() + "shared"[0] + shared_private_data; }
