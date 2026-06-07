use std.core

fn fails(any v) bool {
   v < 0
}

assert(fails(__socket(-1, -1, -1)), "socket rejects invalid domain/type/protocol")
assert(fails(__connect(-1, 0, 0)), "connect rejects invalid fd")
assert(fails(__bind(-1, 0, 0)), "bind rejects invalid fd")
assert(fails(__listen(-1, 1)), "listen rejects invalid fd")
assert(fails(__accept(-1, 0, 0)), "accept rejects invalid fd")
assert(fails(__sendto(-1, "x", 1, 0, 0, 0)), "sendto rejects invalid fd")
assert(fails(__recvfrom(-1, 0, 0, 0, 0, 0)), "recvfrom rejects invalid fd")
assert(fails(__setsockopt(-1, 0, 0, 0, 0)), "setsockopt rejects invalid fd")
assert(fails(__recv(-1, 0, 0, 0)), "recv rejects invalid fd")
assert(fails(__send(-1, "x", 1, 0)), "send rejects invalid fd")
assert(fails(__closesocket(-1)), "closesocket rejects invalid fd")

print("✓ runtime socket tests passed")
