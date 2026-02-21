;; Keywords: os sys
;; Os Sys module.

module std.os.sys (
   syscall, sys_open, sys_read, sys_write, sys_close, sys_getdents64, sys_ioctl
)
use std.core *

fn syscall(num, a=0, b=0, c=0, d=0, e=0, f=0){
   "Performs a raw Linux syscall (`num`) with up to 6 arguments and returns the raw kernel result."
   if(__os_name() != "linux"){ return -1 }
   mut n = num
   ;; Map common x86_64 syscalls to ARM/AArch64 equivalents when needed.
   if(__arch_name() == "aarch64" || __arch_name() == "arm64"){
      if(num == 39){ n = 172 } ; getpid
      elif(num == 0){ n = 63 } ; read
      elif(num == 1){ n = 64 } ; write
      elif(num == 3){ n = 57 } ; close
      elif(num == 217){ n = 61 } ; getdents64
   } elif(__arch_name() == "arm"){
      if(num == 39){ n = 20 } ; getpid (arm32)
      elif(num == 0){ n = 3 } ; read
      elif(num == 1){ n = 4 } ; write
      elif(num == 3){ n = 6 } ; close
      elif(num == 217){ n = 217 } ; getdents64
   }
   return __syscall(n, a, b, c, d, e, f)
}

fn sys_open(path, flags, mode) -> Result {
   "Wrapper for `open(2)`; returns `ok(fd)` or `err(errno_like_code)`."
   def fd = __open(path, flags, mode)
   if(fd < 0){ return err(fd) }
   return ok(fd)
}

fn sys_read(fd, buf, n) -> Result {
   "Wrapper for `read(2)`; returns `ok(bytes_read)` or `err(errno_like_code)`."
   def res = __sys_read_off(fd, buf, n, 0)
   if(res < 0){ return err(res) }
   return ok(res)
}

fn sys_write(fd, buf, n) -> Result {
   "Wrapper for `write(2)`; returns `ok(bytes_written)` or `err(errno_like_code)`."
   def res = __sys_write_off(fd, buf, n, 0)
   if(res < 0){ return err(res) }
   return ok(res)
}

fn sys_close(fd) -> Result {
   "Wrapper for `close(2)`; returns `ok(0)` or `err(errno_like_code)`."
   def res = __close(fd)
   if(res < 0){ return err(res) }
   return ok(0)
}

fn sys_getdents64(fd, buf, n) -> Result {
   "Wrapper for `getdents64(2)`; returns `ok(bytes_filled)` or `err(errno_like_code)`."
   use std.os *
   if(__os_name() != "linux"){ return err(-1) }
   def res = syscall(217, fd, buf, n, 0, 0, 0)
   if(res < 0){ return err(res) }
   return ok(res)
}

fn sys_ioctl(fd, req, arg) -> Result {
   "Wrapper for `ioctl(2)`; returns `ok(0)` or `err(errno_like_code)`."
   def res = __ioctl(fd, req, arg)
   if(res < 0){ return err(res) }
   return ok(res)
}

if(comptime{__main()}){
    use std.os.sys *
    use std.core.error *
    use std.os.fs *
    use std.os.dirs *
    use std.os.path *
    use std.os *

    print("Testing sys...")

    def non_existent_file = normalize(temp_dir() + sep() + "non_existent_file_12345.tmp")
    def r = sys_open(non_existent_file, 0, 0)
    assert(is_err(r), "sys_open fails")
    def code = __unwrap(r)
    assert(code < 0, "errno set in Result")

    if(eq(os(), "linux")){
       def pid = syscall(39)
       assert(pid > 0, "syscall getpid")
    } else {
       print("Skipping raw syscall test: linux-only API")
    }

    print("✓ std.os.sys tests passed")
}
