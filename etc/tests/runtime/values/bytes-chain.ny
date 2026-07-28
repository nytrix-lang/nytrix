use std.core

def b = bytes(3)
b.set(0, 65).set(1, 66).set(2, 67)
assert(b.get(0) == 65, "bytes chain index 0")
assert(b.get(1) == 66, "bytes chain index 1")
assert(b.get(2) == 67, "bytes chain index 2")
print("✓ bytes chain tests passed")
