/*
 * Regression: _Atomic(type-name) must retain the declared base type.
 */
#include <stdatomic.h>

typedef _Atomic(int) ny_atomic_int;

atomic_int ny_atomic_object;

_Static_assert(sizeof(ny_atomic_int) == sizeof(int),
               "_Atomic(int) must retain int representation");
int main(void) {
  if (sizeof(ny_atomic_int) != sizeof(int))
    return 1;
  atomic_store_explicit(&ny_atomic_object, 37, memory_order_relaxed);
  return atomic_load_explicit(&ny_atomic_object, memory_order_relaxed) != 37;
}
