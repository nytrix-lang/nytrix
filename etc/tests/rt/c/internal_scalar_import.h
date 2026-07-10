long labs(long value);

// inline function for seamless C interop test (captured + lowered)
static inline int nytrix_inline_add(int a, int b) {
  return a + b;
}

// function-like macro (name captured for full coverage; not value-expanded here)
#define NYTRIX_FL_ADD(x, y) ((x) + (y))

// object-like still works and is imported as const
#define NYTRIX_INLINE_MAGIC 0xCAFE

