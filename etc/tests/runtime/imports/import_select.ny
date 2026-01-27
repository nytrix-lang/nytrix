use std.core
use std.io
use test_module (local_add as add2, local_greet)

;; Import system – use module (list) (Test)

assert(add2(10, 5) == 15, "import list rename")

def g = local_greet("Tester")
assert(g != 0, "import list function")

print("✓ use module (list) tests passed")
