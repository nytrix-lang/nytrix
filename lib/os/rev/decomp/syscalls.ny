;; Linux syscall-number decoders used by pseudocode rendering.
module std.os.rev.decomp.syscalls *

use std.core

fn _syscall_name_x86_64(int nr) str {
   case nr {
      0 -> "read"
      1 -> "write"
      2 -> "open"
      3 -> "close"
      4 -> "stat"
      5 -> "fstat"
      6 -> "lstat"
      8 -> "lseek"
      9 -> "mmap"
      10 -> "mprotect"
      11 -> "munmap"
      12 -> "brk"
      16 -> "ioctl"
      19 -> "readv"
      20 -> "writev"
      21 -> "access"
      22 -> "pipe"
      24 -> "sched_yield"
      28 -> "madvise"
      32 -> "dup"
      33 -> "dup2"
      35 -> "nanosleep"
      39 -> "getpid"
      41 -> "socket"
      42 -> "connect"
      44 -> "sendto"
      45 -> "recvfrom"
      56 -> "clone"
      57 -> "fork"
      58 -> "vfork"
      59 -> "execve"
      61 -> "wait4"
      62 -> "kill"
      63 -> "uname"
      72 -> "fcntl"
      89 -> "readlink"
      96 -> "gettimeofday"
      101 -> "ptrace"
      102 -> "getuid"
      104 -> "getgid"
      107 -> "geteuid"
      108 -> "getegid"
      110 -> "getppid"
      157 -> "prctl"
      158 -> "arch_prctl"
      186 -> "gettid"
      201 -> "time"
      228 -> "clock_gettime"
      234 -> "tgkill"
      257 -> "openat"
      262 -> "newfstatat"
      267 -> "readlinkat"
      292 -> "dup3"
      293 -> "pipe2"
      318 -> "getrandom"
      332 -> "statx"
      60 -> "exit"
      231 -> "exit_group"
      _ -> "syscall_" + to_str(nr)
   }
}

fn _syscall_name_linux_generic(int nr) str {
   case nr {
      17 -> "getcwd"
      23 -> "dup"
      24 -> "dup3"
      25 -> "fcntl"
      29 -> "ioctl"
      34 -> "mknodat"
      35 -> "mkdirat"
      37 -> "linkat"
      38 -> "renameat"
      46 -> "ftruncate"
      48 -> "faccessat"
      49 -> "chdir"
      56 -> "openat"
      57 -> "close"
      62 -> "lseek"
      63 -> "read"
      64 -> "write"
      65 -> "readv"
      66 -> "writev"
      78 -> "readlinkat"
      80 -> "fstat"
      93 -> "exit"
      94 -> "exit_group"
      129 -> "kill"
      160 -> "uname"
      169 -> "gettimeofday"
      172 -> "getpid"
      173 -> "getppid"
      174 -> "getuid"
      175 -> "geteuid"
      176 -> "getgid"
      177 -> "getegid"
      214 -> "brk"
      222 -> "mmap"
      226 -> "mprotect"
      227 -> "munmap"
      278 -> "getrandom"
      291 -> "statx"
      _ -> "syscall_" + to_str(nr)
   }
}

fn _syscall_name_linux_arm(int nr) str {
   case nr {
      1 -> "exit"
      3 -> "read"
      4 -> "write"
      5 -> "open"
      6 -> "close"
      19 -> "lseek"
      20 -> "getpid"
      33 -> "access"
      37 -> "kill"
      45 -> "brk"
      54 -> "ioctl"
      78 -> "gettimeofday"
      90 -> "mmap"
      91 -> "munmap"
      122 -> "uname"
      146 -> "writev"
      163 -> "mremap"
      192 -> "mmap2"
      195 -> "stat64"
      197 -> "fstat64"
      221 -> "fcntl64"
      224 -> "gettid"
      248 -> "exit_group"
      295 -> "openat"
      322 -> "dup3"
      327 -> "readlinkat"
      345 -> "getrandom"
      397 -> "statx"
      _ -> "syscall_" + to_str(nr)
   }
}

fn _syscall_name_for (str family, int nr) str {
   if nr < 0 { return "syscall_unknown" }
   if family == "aarch64" || family == "riscv" { return _syscall_name_linux_generic(nr) }
   if family == "arm" { return _syscall_name_linux_arm(nr) }
   _syscall_name_x86_64(nr)
}

fn _syscall_name(int nr) str {
   _syscall_name_x86_64(nr)
}
