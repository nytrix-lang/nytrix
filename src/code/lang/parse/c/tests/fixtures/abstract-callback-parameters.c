#include <stddef.h>

static long node = 41;
static int released;

static void *allocate(size_t bytes) { return bytes == 96 ? &node : NULL; }

static void release(void *pointer, size_t bytes) {
  released = pointer == &node && bytes == 96;
}

/*
 * The abstract pointer-to-callback parameters must agree with the named
 * definition, including the pointer return and both callback signatures.
 */
static void get_callbacks(void *(**)(size_t), void (**)(void *, size_t));

static void get_callbacks(void *(**allocator)(size_t),
                          void (**deallocator)(void *, size_t)) {
  *allocator = allocate;
  *deallocator = release;
}

int main(void) {
  void *(*allocator)(size_t) = NULL;
  void (*deallocator)(void *, size_t) = NULL;
  get_callbacks(&allocator, &deallocator);
  void *pointer = allocator(96);
  if (!pointer || *(long *)pointer != 41)
    return 1;
  deallocator(pointer, 96);
  return !released;
}
