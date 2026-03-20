use std

assert(is_str(OS), "use std exposes os globals")
assert(OS.len > 0, "OS string is populated")
assert(is_str(ARCH), "use std exposes os primitive globals")
assert(ARCH.len > 0, "ARCH string is populated")
assert((1 + 1) == 2, "use std keeps core arithmetic available")
