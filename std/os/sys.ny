;; Keywords: os sys
;; Os Sys module.

module std.os.sys (
   syscall, errno
)

fn syscall(num, a=0, b=0, c=0, d=0, e=0, f=0){
   "Raw syscall (Linux x86_64): syscall(num, a=0,b=0,c=0,d=0,e=0,f=0)."
   return __syscall(num, a, b, c, d, e, f)
}

fn errno(){
   "Get last error code."
   return __errno() ; TODO use syscalls instead
}