/*
 * Fixture: GNU leading-zero builtins have int result types in typeof.
 */
_Static_assert(__builtin_types_compatible_p(typeof(__builtin_clz(1U)), int),
               "clz result type");
_Static_assert(__builtin_types_compatible_p(typeof(__builtin_clzl(1UL)), int),
               "clzl result type");
_Static_assert(__builtin_types_compatible_p(typeof(__builtin_clzll(1ULL)), int),
               "clzll result type");

int main(void) { return 0; }
