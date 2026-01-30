use std.core *
use std.core.reflect *

print("Testing explicit std usage...")
print("Explicit std import works")

print("Testing relative file import...")
use "./local_helper.ny" as helper
mut v = helper.helper_val()
print(f"helper.helper_val() returned: '{v}'")
assert(v == 123, f"file module call: {v}")
assert(helper.helper_add(1, 2) == 3, "file module add")

print("Testing relative file import with rename...")
use "./local_helper.ny" (helper_add as my_add)
assert(my_add(10, 10) == 20, "file module rename")

print("✓ import system tests passed")

