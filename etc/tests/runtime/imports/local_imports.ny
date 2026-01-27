use std.core
use std.io
use "./test_module.ny"

;; Import system – local module (Test)

def r = local_add(5, 3)
assert(r == 8, "local_add import")

def g = local_greet("Tester")
assert(g != 0, "local_greet import")

print("✓ local module import tests passed")
