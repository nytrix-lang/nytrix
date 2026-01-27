use std.core
use std.io
use std.core.error

;; Import system – explicit std usage (Test)

assert(print("Explicit std import works") == 0, "std.print callable")

print("✓ std explicit import tests passed")
