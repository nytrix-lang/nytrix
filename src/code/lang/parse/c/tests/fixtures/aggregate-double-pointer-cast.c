/*
 * Fixture: cast and dereference pointers to aggregate-backed interface data.
 */
struct nyc_vtab_base {
  const void *module;
  int refcount;
  char *error;
};
typedef struct nyc_vtab_base nyc_vtab_base;
typedef struct nyc_echo_vtab nyc_echo_vtab;
struct nyc_echo_vtab {
  nyc_vtab_base base;
  void *interp;
  int in_transaction;
};
static struct nyc_echo_vtab state = {.in_transaction = 1};
static int read_transaction(nyc_vtab_base const *vtab) {
  return (*(nyc_echo_vtab **)&vtab)->in_transaction;
}
int main(void) { return read_transaction(&state.base) != 1; }
