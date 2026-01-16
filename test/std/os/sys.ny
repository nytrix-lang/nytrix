use std.os.sys
use std.core
use std.core.test
use std.io.fs

print("Testing sys...")

fn test_errno(){
	def non_existent_file = "/tmp/non_existent_file_12345.tmp"
	def fd = file_open(non_existent_file, 0, 0) ; O_RDONLY
	assert(fd < 0, "file_open non-existent fails")
	def err = errno()
	assert(err != 0, "errno is set after failed syscall")
}

fn test_syscall_getpid(){
	def pid = syscall(39) ; SYS_getpid
	assert(pid > 0, "syscall(SYS_getpid) returns valid pid")
}

test_errno()
test_syscall_getpid()

print("✓ std.os.sys tests passed")
