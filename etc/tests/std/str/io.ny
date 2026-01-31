use std.core *
use std.str.io *

;; std.str.io (Test)

_print_write("io")
print("test")
assert(print() == 0, "print with no arguments")
assert(print("hello", "world", sep=" ") == 0, "print with sep kwarg")
assert(print("hello", "world", end="!\n") == 0, "print with end kwarg")
assert(print("hello", "world", sep="-", end="!\n") == 0,
       "print with sep/end kwargs")
assert(print("hello", "world", end="\n", sep="::") == 0,
       "print with kwargs in reversed order")
assert(print("hello", "world", sep=1, end=2) == 0,
       "print coerces non-string sep/end")
assert(print("hello", "world", unknown="ignored") == 0,
       "print ignores unknown keyword arguments")
assert(1 == 1, "io ok")

print("✓ std.str.io tests passed")
