use std.io
use std.os.sys
use std.core.error
use std.io.fs

;; std.os.sys (Test)
;; Tests errno handling and raw syscall.

print("Testing sys...")

def non_existent_file = "/tmp/non_existent_file_12345.tmp"
def fd = sys_open(non_existent_file, 0, 0)
assert(fd < 0, "sys_open fails")
def err = errno()
assert(err != 0, "errno set")

def pid = syscall(39)
assert(pid > 0, "syscall getpid")

print("✓ std.os.sys tests passed")
