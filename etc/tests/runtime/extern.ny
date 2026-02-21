use std.core *
use std.core.error *
use std.str.io *
use std.os.sys *

if(comptime{ __os_name() == "windows" }){
    extern fn c_getpid(): i32 as "GetCurrentProcessId"
    extern fn my_getpid(): i32 as "GetCurrentProcessId"
} else {
    extern fn c_getpid(): i32 as "getpid"
    extern fn my_getpid(): i32 as "getpid"
}

print("Testing runtime extern support via libc...")

def pid = c_getpid()
; print("c_getpid returned:", pid)

def my_pid = my_getpid()
assert(my_pid == pid, "my_getpid (aliased) works")

print("✓ runtime extern test passed")
