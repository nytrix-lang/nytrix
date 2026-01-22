;; Keywords: process mod
;; Process Mod module.

use std.collections
use std.os.sys
module std.process (
   fork, waitpid, pack_argv, execve, spawn, sys_pipe, dup2, kill, exec, run, popen, exit
)

fn fork(){
   "Forks the current process via **fork(2)**.
   - Returns the child **PID** to the parent.
   - Returns **0** to the child process."
   __syscall(57, 0,0,0,0,0,0)
}

fn waitpid(pid, options){
   "Waits for a process `pid` to change state via **waitpid(2)**. Returns a [[std.core::list]] `[pid, status]` or `-1` on error."
   def st = __malloc(8)
   def r = __syscall(61, pid, st, options, 0,0,0)
   if(r < 0){ __free(st)  -1 }
   else {
      def s = load32(st)
      __free(st)
      return [r, s]
   }
}

fn pack_argv(args){
   "Internal: Packs a [[std.core::list]] of strings into a C-style `argv` array (null-terminated)."
   def n = list_len(args)
   def arr = __malloc((n + 1) * 8)
   def i = 0
   while(i < n){
      store64(arr + i*8, get(args, i))
      i += 1
   }
   store64(arr + n*8, to_int(0))
   arr
}

fn execve(path, args){
   "Standard **execve(2)** replacement of the current process image with program at `path` and arguments `args`."
   def argvp = pack_argv(args)
   def envp = __envp()
   __syscall(59, path, argvp, envp, 0,0,0)
}

fn spawn(path, args){
   "Non-blocking process start. Spawns `path` with `args`. Returns the **PID** of the new process. See [[std.process::run]] for blocking execution."
   def pid = fork()
   if(pid==0){
      execve(path, args)
      __syscall(60, 1, 0,0,0,0,0)
   }
   pid
}

fn sys_pipe(){
   "Create a unidirectional data channel via **pipe(2)**. Returns a [[std.core::list]] `[read_fd, write_fd]`."
   def fds = __malloc(16)
   def r = __syscall(22, fds, 0,0,0,0,0)
   if(r < 0){ return [0,0] }
   else { return [load32(fds), load32(fds+4)] }
}

fn dup2(oldfd, newfd){
   "Duplicates file descriptor `oldfd` to `newfd` via **dup2(2)**."
   __syscall(33, oldfd, newfd, 0,0,0,0)
}

fn kill(pid, sig){
   "Sends signal `sig` to process `pid` via **kill(2)**."
   __syscall(62, pid, sig, 0,0,0,0)
}

fn exec(path, args){
   "Convenience wrapper for [[std.process::execve]]. Automatically prepends `path` to the argument list."
   def full_args = list(8)
   full_args = append(full_args, path)
   def i = 0
   while(i < list_len(args)){
      full_args = append(full_args, get(args, i))
      i += 1
   }
   execve(path, full_args)
}

fn run(path, args){
   "Blocking command execution. Spawns the process and waits for termination. Returns the **exit status**. See [[std.process::spawn]]."
   def pid = spawn(path, args)
   if(pid < 0){ -1 }
   else {
      def status = waitpid(pid, 0)
      if(status == -1){ -1 }
      else { (get(status, 1) >> 8) & 255 }
   }
}

fn popen(path, args){
   "Spawns a process with piped stdin/stdout. Returns a [[std.core::list]] `[pid, stdin_write_fd, stdout_read_fd]`."
   def in_pipe = sys_pipe()
   def out_pipe = sys_pipe()
   def pid = fork()
   if(pid == 0){
      ; Child
      dup2(get(in_pipe, 0), 0)
      dup2(get(out_pipe, 1), 1)
      dup2(get(out_pipe, 1), 2)
      ; Close all pipe fds in child after dup2
      __syscall(3, get(in_pipe, 0), 0,0,0,0,0)
      __syscall(3, get(in_pipe, 1), 0,0,0,0,0)
      __syscall(3, get(out_pipe, 0), 0,0,0,0,0)
      __syscall(3, get(out_pipe, 1), 0,0,0,0,0)
      def argvp = pack_argv(args)
      def child_envp = __envp()
      __syscall(59, path, argvp, child_envp, 0,0,0)
      __syscall(60, 1, 0,0,0,0,0)
   }
   ; Parent: close child ends
   __syscall(3, get(in_pipe, 0), 0,0,0,0,0)
   __syscall(3, get(out_pipe, 1), 0,0,0,0,0)
   return [pid, get(in_pipe, 1), get(out_pipe, 0)]
}

fn exit(code){
   "Terminates the current process with status `code` via **exit(2)**."
   __syscall(60, code, 0,0,0,0,0)
}