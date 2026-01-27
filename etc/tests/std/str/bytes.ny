use std.core *
use std.str.bytes *

;; std.str.bytes (Test)

def b = bytes(4)
assert(bytes_len(b) == 4, "bytes_len")
bytes_set(b, 0, 65)
bytes_set(b, 1, 66)
bytes_set(b, 2, 67)
bytes_set(b, 3, 68)
assert(bytes_get(b, 0) == 65, "bytes_get 0")
assert(bytes_get(b, 3) == 68, "bytes_get 3")

print("✓ std.str.bytes tests passed")
