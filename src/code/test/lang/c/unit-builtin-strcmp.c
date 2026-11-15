/*
 * Fixture: fold literal __builtin_strcmp calls used by kernel compile-time
 * guards.
 */
extern void __BUILD_BUG_vruntime_cmp(void);
extern void __BUILD_BUG_vruntime_op(void);

#define NYC_VRUNTIME_CMP(a, cmp, b)        \
  ({                                       \
    int result = 0;                        \
    if (!__builtin_strcmp(cmp, "<"))       \
      result = (long)((a) - (b)) < 0;      \
    else if (!__builtin_strcmp(cmp, "<=")) \
      result = (long)((a) - (b)) <= 0;     \
    else if (!__builtin_strcmp(cmp, ">"))  \
      result = (long)((a) - (b)) > 0;      \
    else if (!__builtin_strcmp(cmp, ">=")) \
      result = (long)((a) - (b)) >= 0;     \
    else                                   \
      __BUILD_BUG_vruntime_cmp();          \
    result;                                \
  })

#define NYC_VRUNTIME_OP(a, op, b)   \
  ({                                \
    long result = 0;                \
    if (!__builtin_strcmp(op, "-")) \
      result = (long)((a) - (b));   \
    else                            \
      __BUILD_BUG_vruntime_op();    \
    result;                         \
  })

int main(void) {
  if (0)
    __BUILD_BUG_vruntime_cmp();
  return NYC_VRUNTIME_CMP(2, "<", 3) != 1 ||
         NYC_VRUNTIME_CMP(3, "<=", 3) != 1 ||
         NYC_VRUNTIME_CMP(4, ">", 3) != 1 ||
         NYC_VRUNTIME_CMP(3,
                          ">"
                          "=",
                          3) != 1 ||
         NYC_VRUNTIME_OP(9, "-", 4) != 5;
}
