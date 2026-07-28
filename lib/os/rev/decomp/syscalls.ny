;; Keywords: linux syscalls abi arguments pseudocode
;; Linux syscall number, ABI, and argument-name decoding for pseudocode.
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
fn _syscall_arg_label(int nr, str reg) str {
   if reg == "rax" { return "syscall_nr" }
   if nr == 0 || nr == 1 || nr < 0 {
      if reg == "rdi" { return "syscall_fd" }
      if reg == "rsi" { return "syscall_buf" }
      if reg == "rdx" { return "syscall_count" }
      return "syscall_" + reg
   }
   if nr == 2 {
      if reg == "rdi" { return "syscall_path" }
      if reg == "rsi" { return "syscall_flags" }
      if reg == "rdx" { return "syscall_mode" }
      return "syscall_" + reg
   }
   if nr == 8 {
      if reg == "rdi" { return "syscall_fd" }
      if reg == "rsi" { return "syscall_offset" }
      if reg == "rdx" { return "syscall_whence" }
      return "syscall_" + reg
   }
   if nr == 3 {
      if reg == "rdi" { return "syscall_fd" }
   }
   if nr == 4 || nr == 6 {
      if reg == "rdi" { return "syscall_path" }
      if reg == "rsi" { return "syscall_stat" }
      return "syscall_" + reg
   }
   if nr == 5 {
      if reg == "rdi" { return "syscall_fd" }
      if reg == "rsi" { return "syscall_stat" }
      return "syscall_" + reg
   }
   if nr == 9 {
      if reg == "rdi" { return "syscall_addr" }
      if reg == "rsi" { return "syscall_length" }
      if reg == "rdx" { return "syscall_prot" }
      if reg == "r10" { return "syscall_flags" }
      if reg == "r8" { return "syscall_fd" }
      if reg == "r9" { return "syscall_offset" }
      return "syscall_" + reg
   }
   if nr == 10 {
      if reg == "rdi" { return "syscall_addr" }
      if reg == "rsi" { return "syscall_length" }
      if reg == "rdx" { return "syscall_prot" }
      return "syscall_" + reg
   }
   if nr == 11 {
      if reg == "rdi" { return "syscall_addr" }
      if reg == "rsi" { return "syscall_length" }
      return "syscall_" + reg
   }
   if nr == 12 {
      if reg == "rdi" { return "syscall_brk" }
   }
   if nr == 16 {
      if reg == "rdi" { return "syscall_fd" }
      if reg == "rsi" { return "syscall_request" }
      if reg == "rdx" { return "syscall_arg" }
      return "syscall_" + reg
   }
   if nr == 19 || nr == 20 {
      if reg == "rdi" { return "syscall_fd" }
      if reg == "rsi" { return "syscall_iov" }
      if reg == "rdx" { return "syscall_iovcnt" }
      return "syscall_" + reg
   }
   if nr == 21 {
      if reg == "rdi" { return "syscall_path" }
      if reg == "rsi" { return "syscall_mode" }
      return "syscall_" + reg
   }
   if nr == 22 {
      if reg == "rdi" { return "syscall_pipefd" }
      return "syscall_" + reg
   }
   if nr == 24 {
      return "syscall_" + reg
   }
   if nr == 28 {
      if reg == "rdi" { return "syscall_addr" }
      if reg == "rsi" { return "syscall_length" }
      if reg == "rdx" { return "syscall_advice" }
      return "syscall_" + reg
   }
   if nr == 32 {
      if reg == "rdi" { return "syscall_oldfd" }
      return "syscall_" + reg
   }
   if nr == 33 {
      if reg == "rdi" { return "syscall_oldfd" }
      if reg == "rsi" { return "syscall_newfd" }
      return "syscall_" + reg
   }
   if nr == 35 {
      if reg == "rdi" { return "syscall_req" }
      if reg == "rsi" { return "syscall_rem" }
      return "syscall_" + reg
   }
   if nr == 39 || nr == 57 || nr == 58 || nr == 102 || nr == 104 || nr == 107 || nr == 108 || nr == 110 || nr == 186 {
      return "syscall_" + reg
   }
   if nr == 41 {
      if reg == "rdi" { return "syscall_domain" }
      if reg == "rsi" { return "syscall_type" }
      if reg == "rdx" { return "syscall_protocol" }
      return "syscall_" + reg
   }
   if nr == 42 {
      if reg == "rdi" { return "syscall_fd" }
      if reg == "rsi" { return "syscall_sockaddr" }
      if reg == "rdx" { return "syscall_addrlen" }
      return "syscall_" + reg
   }
   if nr == 44 || nr == 45 {
      if reg == "rdi" { return "syscall_fd" }
      if reg == "rsi" { return "syscall_buf" }
      if reg == "rdx" { return "syscall_count" }
      if reg == "r10" { return "syscall_flags" }
      if reg == "r8" { return nr == 44 ? "syscall_dest" : "syscall_src" }
      if reg == "r9" { return "syscall_addrlen" }
      return "syscall_" + reg
   }
   if nr == 56 {
      if reg == "rdi" { return "syscall_clone_flags" }
      if reg == "rsi" { return "syscall_stack" }
      if reg == "rdx" { return "syscall_parent_tid" }
      if reg == "r10" { return "syscall_child_tid" }
      if reg == "r8" { return "syscall_tls" }
      return "syscall_" + reg
   }
   if nr == 59 {
      if reg == "rdi" { return "syscall_path" }
      if reg == "rsi" { return "syscall_argv" }
      if reg == "rdx" { return "syscall_envp" }
      return "syscall_" + reg
   }
   if nr == 61 {
      if reg == "rdi" { return "syscall_wait_pid" }
      if reg == "rsi" { return "syscall_status" }
      if reg == "rdx" { return "syscall_options" }
      if reg == "r10" { return "syscall_rusage" }
      return "syscall_" + reg
   }
   if nr == 62 {
      if reg == "rdi" { return "syscall_signal_pid" }
      if reg == "rsi" { return "syscall_signal" }
      return "syscall_" + reg
   }
   if nr == 63 {
      if reg == "rdi" { return "syscall_utsname" }
      return "syscall_" + reg
   }
   if nr == 72 {
      if reg == "rdi" { return "syscall_fd" }
      if reg == "rsi" { return "syscall_cmd" }
      if reg == "rdx" { return "syscall_arg" }
      return "syscall_" + reg
   }
   if nr == 89 {
      if reg == "rdi" { return "syscall_path" }
      if reg == "rsi" { return "syscall_buf" }
      if reg == "rdx" { return "syscall_count" }
      return "syscall_" + reg
   }
   if nr == 96 {
      if reg == "rdi" { return "syscall_timeval" }
      if reg == "rsi" { return "syscall_timezone" }
      return "syscall_" + reg
   }
   if nr == 101 {
      if reg == "rdi" { return "syscall_ptrace_request" }
      if reg == "rsi" { return "syscall_ptrace_pid" }
      if reg == "rdx" { return "syscall_ptrace_addr" }
      if reg == "r10" { return "syscall_ptrace_data" }
      return "syscall_" + reg
   }
   if nr == 157 {
      if reg == "rdi" { return "syscall_prctl_option" }
      if reg == "rsi" { return "syscall_prctl_arg2" }
      if reg == "rdx" { return "syscall_prctl_arg3" }
      if reg == "r10" { return "syscall_prctl_arg4" }
      if reg == "r8" { return "syscall_prctl_arg5" }
      return "syscall_" + reg
   }
   if nr == 158 {
      if reg == "rdi" { return "syscall_arch_code" }
      if reg == "rsi" { return "syscall_arch_addr" }
      return "syscall_" + reg
   }
   if nr == 201 {
      if reg == "rdi" { return "syscall_time_ptr" }
      return "syscall_" + reg
   }
   if nr == 228 {
      if reg == "rdi" { return "syscall_clock_id" }
      if reg == "rsi" { return "syscall_timespec" }
      return "syscall_" + reg
   }
   if nr == 234 {
      if reg == "rdi" { return "syscall_tgid" }
      if reg == "rsi" { return "syscall_tid" }
      if reg == "rdx" { return "syscall_signal" }
      return "syscall_" + reg
   }
   if nr == 257 {
      if reg == "rdi" { return "syscall_dirfd" }
      if reg == "rsi" { return "syscall_path" }
      if reg == "rdx" { return "syscall_flags" }
      if reg == "r10" { return "syscall_mode" }
      return "syscall_" + reg
   }
   if nr == 262 {
      if reg == "rdi" { return "syscall_dirfd" }
      if reg == "rsi" { return "syscall_path" }
      if reg == "rdx" { return "syscall_stat" }
      if reg == "r10" { return "syscall_flags" }
      return "syscall_" + reg
   }
   if nr == 267 {
      if reg == "rdi" { return "syscall_dirfd" }
      if reg == "rsi" { return "syscall_path" }
      if reg == "rdx" { return "syscall_buf" }
      if reg == "r10" { return "syscall_count" }
      return "syscall_" + reg
   }
   if nr == 292 {
      if reg == "rdi" { return "syscall_oldfd" }
      if reg == "rsi" { return "syscall_newfd" }
      if reg == "rdx" { return "syscall_flags" }
      return "syscall_" + reg
   }
   if nr == 293 {
      if reg == "rdi" { return "syscall_pipefd" }
      if reg == "rsi" { return "syscall_flags" }
      return "syscall_" + reg
   }
   if nr == 318 {
      if reg == "rdi" { return "syscall_buf" }
      if reg == "rsi" { return "syscall_count" }
      if reg == "rdx" { return "syscall_flags" }
      return "syscall_" + reg
   }
   if nr == 332 {
      if reg == "rdi" { return "syscall_dirfd" }
      if reg == "rsi" { return "syscall_path" }
      if reg == "rdx" { return "syscall_flags" }
      if reg == "r10" { return "syscall_mask" }
      if reg == "r8" { return "syscall_statx" }
      return "syscall_" + reg
   }
   if nr == 60 || nr == 231 {
      if reg == "rdi" { return "syscall_code" }
   }
   "syscall_" + reg
}

fn _syscall_arg_label_generic(str family, int nr, str reg) str {
   def prof = _syscall_profile_for_family(family)
   if reg == prof.get("nr", "") { return "syscall_nr" }
   def args = prof.get("args", [])
   mut pos = -1
   mut i = 0
   while i < args.len {
      if reg == args[i] { pos = i }
      i += 1
   }
   if pos < 0 { return "syscall_" + reg }
   def name = _syscall_name_for (family, nr)
   if name == "read" || name == "write" {
      if pos == 0 { return "syscall_fd" }
      if pos == 1 { return "syscall_buf" }
      if pos == 2 { return "syscall_count" }
   }
   if name == "readv" || name == "writev" {
      if pos == 0 { return "syscall_fd" }
      if pos == 1 { return "syscall_iov" }
      if pos == 2 { return "syscall_iovcnt" }
   }
   if name == "open" {
      if pos == 0 { return "syscall_path" }
      if pos == 1 { return "syscall_flags" }
      if pos == 2 { return "syscall_mode" }
   }
   if name == "openat" {
      if pos == 0 { return "syscall_dirfd" }
      if pos == 1 { return "syscall_path" }
      if pos == 2 { return "syscall_flags" }
      if pos == 3 { return "syscall_mode" }
   }
   if name == "close" {
      if pos == 0 { return "syscall_fd" }
   }
   if name == "lseek" {
      if pos == 0 { return "syscall_fd" }
      if pos == 1 { return "syscall_offset" }
      if pos == 2 { return "syscall_whence" }
   }
   if name == "mmap" || name == "mmap2" {
      if pos == 0 { return "syscall_addr" }
      if pos == 1 { return "syscall_length" }
      if pos == 2 { return "syscall_prot" }
      if pos == 3 { return "syscall_flags" }
      if pos == 4 { return "syscall_fd" }
      if pos == 5 { return "syscall_offset" }
   }
   if name == "mprotect" {
      if pos == 0 { return "syscall_addr" }
      if pos == 1 { return "syscall_length" }
      if pos == 2 { return "syscall_prot" }
   }
   if name == "munmap" {
      if pos == 0 { return "syscall_addr" }
      if pos == 1 { return "syscall_length" }
   }
   if name == "brk" {
      if pos == 0 { return "syscall_brk" }
   }
   if name == "ioctl" {
      if pos == 0 { return "syscall_fd" }
      if pos == 1 { return "syscall_request" }
      if pos == 2 { return "syscall_arg" }
   }
   if name == "exit" || name == "exit_group" {
      if pos == 0 { return "syscall_code" }
   }
   if name == "getrandom" {
      if pos == 0 { return "syscall_buf" }
      if pos == 1 { return "syscall_count" }
      if pos == 2 { return "syscall_flags" }
   }
   if name == "statx" {
      if pos == 0 { return "syscall_dirfd" }
      if pos == 1 { return "syscall_path" }
      if pos == 2 { return "syscall_flags" }
      if pos == 3 { return "syscall_mask" }
      if pos == 4 { return "syscall_statx" }
   }
   "syscall_arg" + to_str(pos)
}

fn _syscall_arg_label_for (str family, int nr, str reg) str {
   if family == "x86" || family == "x86_64" { return _syscall_arg_label(nr, reg) }
   _syscall_arg_label_generic(family, nr, reg)
}

fn _syscall_profile_for_family(str family) dict {
   if family == "aarch64" { return {"abi": "linux_aarch64", "nr": "x8", "ret": "x0", "args": ["x0", "x1", "x2", "x3", "x4", "x5"]} }
   if family == "arm" { return {"abi": "linux_arm", "nr": "r7", "ret": "r0", "args": ["r0", "r1", "r2", "r3", "r4", "r5"]} }
   if family == "riscv" { return {"abi": "linux_riscv", "nr": "a7", "ret": "a0", "args": ["a0", "a1", "a2", "a3", "a4", "a5"]} }
   {"abi": "linux_x86_64", "nr": "rax", "ret": "rax", "args": ["rdi", "rsi", "rdx", "r10", "r8", "r9"]}
}
