/*
 * Runtime fixture for typedef scope and indexing within a nested block.
 */
typedef int nyc_scope_type;
int main(void) {
  if (sizeof(nyc_scope_type) != sizeof(int))
    return 1;
  {
    typedef long nyc_inner_scope_type;
    if (sizeof(nyc_inner_scope_type) != sizeof(long))
      return 2;
  }
  if (sizeof(nyc_scope_type) != sizeof(int))
    return 3;
  return sizeof(nyc_scope_type) != sizeof(int);
}
