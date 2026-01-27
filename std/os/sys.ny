;; Keywords: os sys
;; Os Sys module.

module std.os.sys (
   syscall, sys_open, sys_read, sys_write, sys_close, sys_getdents64
)

fn syscall(num, a=0, b=0, c=0, d=0, e=0, f=0){
   "Executes a raw Linux x86_64 system call with up to 6 arguments. `num` is the syscall number."
   return __syscall(num, a, b, c, d, e, f)
}

fn sys_open(path, flags, mode){
   "Opens a file using raw syscall."
   syscall(2, path, flags, mode)
}

fn sys_read(fd, buf, n){
   "Reads up to `n` bytes from file descriptor `fd` into `buf`."
   syscall(0, fd, buf, n, 0, 0, 0)
}

fn sys_write(fd, buf, n){
   "Writes `n` bytes from `buf` to file descriptor `fd`."
   syscall(1, fd, buf, n, 0, 0, 0)
}

fn sys_close(fd){
   "Closes file descriptor `fd`."
   syscall(3, fd, 0, 0, 0, 0, 0)
}

fn sys_getdents64(fd, buf, n){
   "Reads directory entries."
   syscall(217, fd, buf, n, 0, 0, 0)
}
