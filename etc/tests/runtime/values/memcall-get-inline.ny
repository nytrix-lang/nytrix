use std.core

def a = [0, 99, 2]
assert(a.get(1, 0) == 99, "inline memcall get equals integer literal")
assert(a.get(1, 0) + 0 == 99, "inline memcall get in arithmetic")
assert(a.get(1, 0) < 100 && a.get(1, 0) > 0, "inline memcall get orderings")
def v = a.get(1, 0)
assert(v == 99 && v + 1 == 100, "stored memcall get keeps scalar value")

fn touch(m) {
   m[1] = 99
   m
}

mut b = [0, 1, 2]
touch(b)
assert(b.get(1, 0) == 99, "parameter mutation observed by inline memcall get")
print("memcall get inline ok")
