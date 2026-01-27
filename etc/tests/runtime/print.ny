use std.core
use std.io
use std.core.error

;; std.io.print (Test)
;; Real assertions for print behavior.

assert(print() == 0, "print empty")

assert(print("Basic") == 0, "print basic")

assert(print("Vals:", 1, 2, 3) == 0, "print multi")

assert(print("A", "B", sep="-", end=".\n") == 0, "print kwargs")

assert(print(end="[END]\n") == 0, "print only kwarg")

print("✓ Print tests passed")
