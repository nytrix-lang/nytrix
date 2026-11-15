/*
 * Negative fixture: reject a failed compile-time size assertion.
 */
_Static_assert(sizeof(long) == 1, "wrong long width");
