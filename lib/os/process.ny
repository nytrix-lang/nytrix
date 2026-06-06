;; Keywords: process spawn subprocess os
;; Process execution, spawning, waiting, capture, and pipe control.
;; References:
;; - std.os
module std.os.process(run, popen, waitpid)
use std.core
use std.core as core
use std.os.sys
use std.os.prim (env)
use std.core.str
use std.core.reflect
use std.os.path as ospath

fn _path_exists(any path) bool {
   if(!is_str(path) || path.len == 0){ return false }
   __access(path, 0) == 0
}

fn _resolve_cmd(any cmd) any {
   if(!is_str(cmd)){ return cmd }
   if(ospath.has_sep(cmd) || ospath.is_abs(cmd)){
      def norm = ospath.normalize(cmd)
      #windows {
         if(ospath.extname(norm) == ""){
            def exe = norm + ".exe"
            if(_path_exists(exe)){ return exe }
         }
      }
      #endif
      return norm
   }
   def p = env("PATH")
   if(!is_str(p) || p.len == 0){ return cmd }
   mut sep = ":"
   #windows { sep = ";" }
   #endif
   def dirs = split(p, sep)
   mut has_dot = false
   mut exts = list(0)
   #windows {
      has_dot = find(cmd, ".") >= 0
      if(!has_dot){
         def pe = env("PATHEXT")
         if(is_str(pe) && pe.len > 0){ exts = split(pe, ";") } else { exts = [".exe", ".cmd", ".bat", ".com"] }
      }
   }
   #endif
   mut i = 0
   while(i < dirs.len){
      def d = dirs.get(i, "")
      if(d.len > 0){
         def c = ospath.join(d, cmd)
         if(_path_exists(c)){ return ospath.normalize(c) }
         #windows {
            if(!has_dot){
               mut j = 0
               while(j < exts.len){
                  def e = exts.get(j, "")
                  if(e.len > 0){
                     def c1 = c + e
                     if(_path_exists(c1)){ return ospath.normalize(c1) }
                  }
                  j += 1
               }
            }
         }
         #endif
      }
      i += 1
   }
   ospath.normalize(cmd)
}

fn _argv_offset(str path, str rpath, list args, int n) int {
   if(n > 0 && (eq(args.get(0, ""), path) || eq(args.get(0, ""), rpath))){ return 0 }
   1
}

fn _build_argv(str path, str rpath, list args) any {
   def n = args.len
   def offset = _argv_offset(path, rpath, args, n)
   def argv = malloc((n + offset + 1) * 8)
   if(argv == 0){ return 0 }
   if(offset == 1){ store64(argv, rpath, 0) }
   mut i = 0
   while(i < n){
      store64(argv, args.get(i, 0), (i + offset) * 8)
      i += 1
   }
   store64(argv, 0, (n + offset) * 8)
   argv
}

fn waitpid(int pid, int options) int {
   "Waits for `pid` and returns the **exit code** (0..255). Negative on failure."
   #windows {
      if(pid <= 0){ return -1 }
      return __wait_process(pid)
   } #else {
      def status_ptr = malloc(8)
      if(status_ptr == 0){ return -1 }
      defer { free(status_ptr) }
      def res = __wait4(pid, status_ptr, options)
      if(res < 0){ return res }
      def status = load32(status_ptr, 0)
      if((status & 127) != 0){ return 128 + (status & 127) }
      return(status / 256) % 256
   } #endif
}

fn run(str path, list args) int {
   "Forks and execs `path` with `args`, waits for completion, and returns the child exit code(0..255); returns `-1` if fork fails."
   mut rpath = _resolve_cmd(path)
   #if(windows || macos){
      def argv = _build_argv(path, rpath, args)
      if(argv == 0){ return -1 }
      defer { free(argv) }
      def code = __spawn_wait(rpath, argv)
      return code
   } #else {
      mut pid = __fork()
      if(pid == 0){
         def argv = _build_argv(path, rpath, args)
         if(argv == 0){ __exit(127) }
         __execve(rpath, argv, __envp())
         __exit(127)
      } else {
         if(pid < 0){ return -1 }
         return waitpid(pid, 0)
      }
   } #endif
}

fn popen(str path, list args) any {
   "Starts `path` with stdin/stdout pipes(stderr merged) and returns `[pid, child_stdin_fd, child_stdout_fd]`; returns `0` on setup failure."
   mut rpath = _resolve_cmd(path)
   #windows {
      def argv = _build_argv(path, rpath, args)
      if(argv == 0){ return 0 }
      defer { free(argv) }
      def fds = malloc(8)
      if(fds == 0){ return 0 }
      defer { free(fds) }
      def pid = __spawn_pipe(rpath, argv, fds)
      if(pid <= 0){ return 0 }
      def stdin_write = load32(fds, 0)
      def stdout_read = load32(fds, 4)
      if(stdin_write < 0 || stdout_read < 0){
         sys_close_quiet(stdin_write)
         sys_close_quiet(stdout_read)
         return 0
      }
      return [pid, stdin_write, stdout_read]
   }
   #endif
   def n = args.len
   def p_stdin = malloc(8)
   def p_stdout = malloc(8)
   if(p_stdin == 0 || p_stdout == 0){
      if(p_stdin != 0){ free(p_stdin) }
      if(p_stdout != 0){ free(p_stdout) }
      return 0
   }
   defer { free(p_stdin) }
   defer { free(p_stdout) }
   if(__pipe(p_stdin) < 0){ return 0 }
   if(__pipe(p_stdout) < 0){
      def in_r, in_w = load32(p_stdin, 0), load32(p_stdin, 4)
      sys_close_quiet(in_r)
      sys_close_quiet(in_w)
      return 0
   }
   def stdin_read = load32(p_stdin, 0)
   def stdin_write = load32(p_stdin, 4)
   def stdout_read = load32(p_stdout, 0)
   def stdout_write = load32(p_stdout, 4)
   def pid = __fork()
   if(pid < 0){
      sys_close_quiet(stdin_read)
      sys_close_quiet(stdin_write)
      sys_close_quiet(stdout_read)
      sys_close_quiet(stdout_write)
      return 0
   }
   if(pid == 0){
      __dup2(stdin_read, 0)
      __dup2(stdout_write, 1)
      __dup2(stdout_write, 2)
      sys_close_quiet(stdin_write)
      sys_close_quiet(stdout_read)
      sys_close_quiet(stdin_read)
      sys_close_quiet(stdout_write)
      def argv = _build_argv(path, rpath, args)
      if(argv == 0){ __exit(127) }
      __execve(rpath, argv, __envp())
      __exit(127)
   }
   sys_close_quiet(stdin_read)
   sys_close_quiet(stdout_write)
   [pid, stdin_write, stdout_read]
}

#main {
   mut sh = "/bin/sh"
   mut ok_args = [sh, "-c", "exit 0"]
   mut exit_args = [sh, "-c", "exit 7"]
   #windows {
      sh = "cmd"
      ok_args = [sh, "/c", "exit", "0"]
      exit_args = [sh, "/c", "exit", "7"]
   } #endif
   assert(run(sh, ok_args) == 0, "process run zero")
   assert(run(sh, exit_args) == 7, "process run exit code")
   assert(run("nytrix-does-not-exist-nytrix", ["nytrix-does-not-exist-nytrix"]) != 0, "process run missing command")
   print("✓ std.os.process self-test passed")
}
