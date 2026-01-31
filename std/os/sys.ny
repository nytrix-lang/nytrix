;; Keywords: os sys
;; Os Sys module.

module std.os.sys (
   syscall, sys_open, sys_read, sys_write, sys_close, sys_getdents64
)

fn syscall(num, a=0, b=0, c=0, d=0, e=0, f=0){
   "Performs a raw Linux x86_64 syscall (`num`) with up to 6 arguments and returns the raw kernel result."
   return __syscall(num, a, b, c, d, e, f)
}

fn sys_open(path, flags, mode) -> Result {
   "Wrapper for `open(2)`; returns `ok(fd)` or `err(errno_like_code)`."
   def fd = syscall(2, path, flags, mode)
   if (fd < 0) { return err(fd) }
   return ok(fd)
}

fn sys_read(fd, buf, n) -> Result {
   "Wrapper for `read(2)`; returns `ok(bytes_read)` or `err(errno_like_code)`."
   def res = syscall(0, fd, buf, n, 0, 0, 0)
   if (res < 0) { return err(res) }
   return ok(res)
}

fn sys_write(fd, buf, n) -> Result {
   "Wrapper for `write(2)`; returns `ok(bytes_written)` or `err(errno_like_code)`."
   def res = syscall(1, fd, buf, n, 0, 0, 0)
   if (res < 0) { return err(res) }
   return ok(res)
}

fn sys_close(fd) -> Result {
   "Wrapper for `close(2)`; returns `ok(0)` or `err(errno_like_code)`."
   def res = syscall(3, fd, 0, 0, 0, 0, 0)
   if (res < 0) { return err(res) }
   return ok(res)
}

fn sys_getdents64(fd, buf, n) -> Result {
   "Wrapper for `getdents64(2)`; returns `ok(bytes_filled)` or `err(errno_like_code)`."
   def res = syscall(217, fd, buf, n, 0, 0, 0)
   if (res < 0) { return err(res) }
   return ok(res)
}
