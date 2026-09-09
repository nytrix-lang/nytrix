use std.core

def payload = {"items": [10, 20, 30]}
assert(payload.get("items").get(1) == 20, "chained dot calls preserve receiver")
assert(payload.get("items").len == 3, "dot member access reads length")
assert("abc".len == 3, "dot member access works on strings")
print("dot operator fixture passed")
