use std.core
use std.os.sys

; Runtime syscall denylist regression (B-1).
;
; The runtime's rt_syscall wrapper denies process-control / memory-protection
; syscalls so Nytrix programs (and FFI C code reaching __syscall) cannot spawn
; processes or remap executable memory. On linux/x86_64 the denied raw syscall
; numbers are: mprotect=10, clone=56, fork=57, vfork=58. Each must return
; -EPERM (-1) while an allowed syscall (getpid=39) succeeds with a positive pid.
;
; This guards against the regression where the inline-syscall path (and its
; denylist) was compiled out because the rt_x86_64__ / rt_asm__ guard macros
; were never defined, silently falling back to an unfiltered libc syscall().

#linux {
   #x86_64 {
      ; mprotect(addr=0, len=4096, prot=PROT_READ|PROT_WRITE|PROT_EXEC=7)
      assert(syscall(10, 0, 4096, 7, 0, 0, 0) == -1, "syscall mprotect denied")
      ; clone(0,0,0,0,0,0) — must not create a process
      assert(syscall(56, 0, 0, 0, 0, 0, 0) == -1, "syscall clone denied")
      ; fork()
      assert(syscall(57, 0, 0, 0, 0, 0, 0) == -1, "syscall fork denied")
      ; vfork()
      assert(syscall(58, 0, 0, 0, 0, 0, 0) == -1, "syscall vfork denied")
      ; getpid() stays allowed and returns a real positive pid
      assert(syscall(39, 0, 0, 0, 0, 0, 0) > 0, "syscall getpid allowed")
      print("✓ runtime syscall denylist (linux x86_64) passed")
   } #else {
      print("✓ runtime syscall denylist skipped (non-x86_64 linux)")
   } #endif
} #else {
   print("✓ runtime syscall denylist skipped (non-linux)")
} #endif
