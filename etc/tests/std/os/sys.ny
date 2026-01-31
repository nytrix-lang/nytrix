use std.os.sys *
use std.core.error *
use std.os.fs *

;; std.os.sys (Test)
;; Tests errno handling and raw syscall.

print("Testing sys...")

def non_existent_file = "/tmp/non_existent_file_12345.tmp"
def r = sys_open(non_existent_file, 0, 0)
assert(is_err(r), "sys_open fails")
def code = __unwrap(r)
assert(code < 0, "errno set in Result")

def pid = syscall(39)
assert(pid > 0, "syscall getpid")

print("✓ std.os.sys tests passed")
