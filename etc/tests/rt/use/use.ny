use std.core
use std.core.reflect

print("Testing explicit std usage...")
print("Explicit std import works")
print("Testing relative file import...")

use "./local.ny" (helper_val, helper_add)

mut v = helper_val()
print(f"helper_val() returned: '{v}'")
assert(v == 123, f"file module call: {v}")
assert(helper_add(1, 2) == 3, "file module add")
print("Testing relative file import with rename...")

use "./local.ny" (helper_add as my_add)

assert(my_add(10, 10) == 20, "file module rename")
print("Testing module export profiles...")

use "./local.ny"

assert(helper_val() == 123, "core profile star import value")
assert(helper_add(4, 5) == 9, "core profile star import add")

use "./local.ny":debug

assert(helper_debug() == 9001, "debug profile star import")
print("✓ import system tests passed")
