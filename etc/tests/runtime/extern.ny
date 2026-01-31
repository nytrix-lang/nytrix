use std.core *
use std.core.error *
use std.str.io *

extern fn c_getpid() as "getpid"
extern fn my_getpid() as "getpid"

print("Testing runtime extern support via libc...")

def pid = c_getpid()
; print("c_getpid returned:", pid)

def my_pid = my_getpid()
assert(my_pid == pid, "my_getpid (aliased) works")

print("✓ runtime extern test passed")
