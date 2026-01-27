;; Keywords: os sys
;; Os Sys module.

module std.os.sys (
   syscall, errno
)

fn syscall(num, a=0, b=0, c=0, d=0, e=0, f=0){
   "Executes a raw Linux x86_64 system call with up to 6 arguments. `num` is the syscall number."
   return __syscall(num, a, b, c, d, e, f)
}

fn errno(){
   "Returns the value of the thread-local `errno` variable, indicating the last error encountered by a system call."
   return __errno() ; TODO use syscalls instead
}
