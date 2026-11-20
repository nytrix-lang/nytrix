/*
 * C frontend test fixture: inline aggregate return capture.
 */
struct argument {
  _Bool compat;
  union {
    const char *native;
    unsigned long address;
  } pointer;
};

static inline struct argument native_arg(const char *p) {
  return (struct argument){.pointer.native = p};
}

__attribute__((noinline)) static int check(struct argument a, struct argument b,
                                           const char *first,
                                           const char *second) {
  return a.compat || b.compat || a.pointer.native != first ||
         b.pointer.native != second;
}

int main(int argc, char **argv) {
  const char *first = argv[0];
  const char *second = argc > 1 ? argv[1] : "second";
  return check(native_arg(first), native_arg(second), first, second);
}
