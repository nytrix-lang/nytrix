;; Keywords: sys syscalls os
;; Os Sys for Nytrix
;; References:
;; - core.os
module core.os.sys(syscall, sys_open, sys_read, sys_write, sys_close, sys_close_quiet, sys_getdents64, sys_ioctl, sys_openpty, STDIN_FD, STDOUT_FD, STDERR_FD)
use core
use core.error

def STDIN_FD = 0
def STDOUT_FD = 1
def STDERR_FD = 2

fn syscall(any num, any a=0, any b=0, any c=0, any d=0, any e=0, any f=0) int {
   "Performs a raw Linux syscall(`num`) with up to 6 arguments and returns the raw kernel result."
   #linux {
      mut n = int(num)
      #aarch64 {
         n = case int(num){
            39 -> 172
            0 -> 63
            1 -> 64
            3 -> 57
            217 -> 61
            _ -> int(num)
         }
      } #elif arm {
         n = case int(num){
            39 -> 20
            0 -> 3
            1 -> 4
            3 -> 6
            217 -> 217
            _ -> int(num)
         }
      } #endif
      ; `int(num)` already carries the language integer ABI at this legacy
      ; runtime boundary. Re-tagging it here double-encodes the syscall
      ; number (39 becomes 79), causing getpid to dispatch as syscall 79.
      return __syscall(n, a, b, c, d, e, f)
   } #else {
      return -1
   } #endif
}

fn sys_open(str path, int flags, int mode) Result<int, int> {
   "Wrapper for `open(2)`; returns `ok(fd)` or `err(errno_like_code)`."
   def int fd = __open_raw(path, flags, mode)
   if fd < 0 { return err(__tag(fd)) }
   return ok(__tag(fd))
}

fn _sys_io_result(int res) Result<int, int> {
   if res < 0 { return err(res) }
   return ok(res)
}

fn sys_read(int fd, ptr buf, int n) Result<int, int> {
   "Wrapper for `read(2)`; returns `ok(bytes_read)` or `err(errno_like_code)`."
   return _sys_io_result(__read_fd_raw(fd, buf, n))
}

fn sys_write(int fd, ptr buf, int n) Result<int, int> {
   "Wrapper for `write(2)`; returns `ok(bytes_written)` or `err(errno_like_code)`."
   return _sys_io_result(__write_fd_raw(fd, buf, n))
}

fn sys_close(int fd) Result<int, int> {
   "Wrapper for `close(2)`; returns `ok(0)` or `err(errno_like_code)`."
   def res = __close(fd)
   if res < 0 { return err(res) }
   return ok(0)
}

fn sys_close_quiet(int fd) any {
   "Closes a file descriptor and ignores close errors."
   if fd < 0 { return 0 }
   def ignored = __close(fd)
   ignored
}

fn sys_getdents64(any fd, any buf, any n) Result<int, int> {
   "Wrapper for `getdents64(2)`; returns `ok(bytes_filled)` or `err(errno_like_code)`."
   use core.os
   #linux {
      def res = syscall(217, fd, buf, n, 0, 0, 0)
      if res < 0 { return err(res) }
      return ok(res)
   } #else {
      return err(-1)
   } #endif
}

fn sys_ioctl(any fd, any req, any arg) Result<int, int> {
   "Wrapper for `ioctl(2)`; returns `ok(0)` or `err(errno_like_code)`."
   def ureq = int(req) & 0xffffffff
   def res = __ioctl(fd, ureq, arg)
   if res < 0 { return err(res) }
   return ok(res)
}

fn sys_openpty(any fds_ptr) Result<int, int> {
   "Wrapper for `openpty(3)`; returns `ok(0)` or `err(errno_like_code)`."
   def res = __openpty(fds_ptr)
   if res < 0 { return err(res) }
   return ok(0)
}

#main {
   assert(STDIN_FD == 0 && STDOUT_FD == 1 && STDERR_FD == 2, "sys std fds")
   def r = sys_open("/tmp/nytrix_missing_sys_selftest.tmp", 0, 0)
   assert(is_err(r) && __unwrap(r) < 0 && sys_close_quiet(-1) == 0, "sys error paths")
   #linux {
      assert(syscall(39) > 0, "syscall getpid")
   } #else {
      assert(syscall(39) == -1, "non-linux syscall fallback")
   } #endif
   print("✓ core.os.sys self-test passed")
}
