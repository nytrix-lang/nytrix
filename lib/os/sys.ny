;; Keywords: sys syscalls
;; Os Sys for Nytrix
module std.os.sys(syscall, sys_open, sys_read, sys_write, sys_close, sys_close_quiet, sys_getdents64, sys_ioctl, sys_openpty, STDIN_FD, STDOUT_FD, STDERR_FD)
use std.core
use std.core.error

def STDIN_FD = 0
def STDOUT_FD = 1
def STDERR_FD = 2

fn syscall(any: num, any: a=0, any: b=0, any: c=0, any: d=0, any: e=0, any: f=0): int {
   "Performs a raw Linux syscall(`num`) with up to 6 arguments and returns the raw kernel result."
   #linux {
      mut n = int(num)
      ; Map common x86_64 syscalls to ARM/AArch64 equivalents when needed.
      #aarch64 {
         n = case int(num){
            39 -> 172 ; getpid
            0 -> 63 ; read
            1 -> 64 ; write
            3 -> 57 ; close
            217 -> 61 ; getdents64
            _ -> int(num)
         }
      } #elif arm {
         n = case int(num){
            39 -> 20 ; getpid (arm32)
            0 -> 3 ; read
            1 -> 4 ; write
            3 -> 6 ; close
            217 -> 217 ; getdents64
            _ -> int(num)
         }
      } #endif
      return __syscall(n, a, b, c, d, e, f)
   } #else {
      return -1
   } #endif
}

fn sys_open(any: path, any: flags, any: mode): Result {
   "Wrapper for `open(2)`; returns `ok(fd)` or `err(errno_like_code)`."
   def fd = __open(path, flags, mode)
   if(fd < 0){ return err(fd) }
   return ok(fd)
}

fn _sys_io_result(any: res): Result {
   if(res < 0){ return err(res) }
   return ok(res)
}

fn sys_read(any: fd, any: buf, any: n): Result {
   "Wrapper for `read(2)`; returns `ok(bytes_read)` or `err(errno_like_code)`."
   return _sys_io_result(__read_off(fd, buf, n, 0))
}

fn sys_write(any: fd, any: buf, any: n): Result {
   "Wrapper for `write(2)`; returns `ok(bytes_written)` or `err(errno_like_code)`."
   return _sys_io_result(__write_off(fd, buf, n, 0))
}

fn sys_close(any: fd): Result {
   "Wrapper for `close(2)`; returns `ok(0)` or `err(errno_like_code)`."
   def res = __close(fd)
   if(res < 0){ return err(res) }
   return ok(0)
}

fn sys_close_quiet(any: fd): any {
   "Closes a file descriptor and ignores close errors."
   if(fd < 0){ return 0 }
   def ignored = __close(fd)
   ignored
}

fn sys_getdents64(any: fd, any: buf, any: n): Result {
   "Wrapper for `getdents64(2)`; returns `ok(bytes_filled)` or `err(errno_like_code)`."
   use std.os
   #linux {
      def res = syscall(217, fd, buf, n, 0, 0, 0)
      if(res < 0){ return err(res) }
      return ok(res)
   } #else {
      return err(-1)
   } #endif
}

fn sys_ioctl(any: fd, any: req, any: arg): Result {
   "Wrapper for `ioctl(2)`; returns `ok(0)` or `err(errno_like_code)`."
   def ureq = int(req) & 0xffffffff
   def res = __ioctl(fd, ureq, arg)
   if(res < 0){ return err(res) }
   return ok(res)
}

fn sys_openpty(any: fds_ptr): Result {
   "Wrapper for `openpty(3)`; returns `ok(0)` or `err(errno_like_code)`."
   def res = __openpty(fds_ptr)
   if(res < 0){ return err(res) }
   return ok(0)
}
