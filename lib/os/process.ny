;; Keywords: process spawn subprocess os
;; Process execution, spawning, waiting, capture, and pipe control.
;; References:
;; - core.os
module core.os.process(run, popen, waitpid)
use core
use core as core
use core.os.sys
use core.os.prim (env)
use core.str
use core.reflect
use core.os.path as ospath

fn _path_exists(any path) bool {
   if !is_str(path) || path.len == 0 { return false }
   __access(path, 0) == 0
}

fn _resolve_cmd(str cmd) str {
   if ospath.has_sep(cmd) || ospath.is_abs(cmd) {
      ;; Keep explicit relative paths intact: normalization removes a leading
      ;; "./", changing execvp("./tool") into PATH lookup for "tool".
      def norm = ospath.is_abs(cmd) ? ospath.normalize(cmd) : cmd
      #windows {
         if ospath.extname(norm) == "" {
            def exe = norm + ".exe"
            if _path_exists(exe) { return exe }
         }
      }
      #endif
      return norm
   }
   def p = env("PATH")
   if !is_str(p) || p.len == 0 { return cmd }
   mut sep = ":"
   #windows { sep = ";" }
   #endif
   def dirs = split(p, sep)
   mut has_dot = false
   mut exts = list(0)
   #windows {
      has_dot = find(cmd, ".") >= 0
      if !has_dot {
         def pe = env("PATHEXT")
         if is_str(pe) && pe.len > 0 { exts = split(pe, ";") } else { exts = [".exe", ".cmd", ".bat", ".com"] }
      }
   }
   #endif
   mut i = 0
   while i < dirs.len {
      def d = dirs.get(i, "")
      if d.len > 0 {
         def c = ospath.join(d, cmd)
         if _path_exists(c) { return ospath.normalize(c) }
         #windows {
            if !has_dot {
               mut j = 0
               while j < exts.len {
                  def e = exts.get(j, "")
                  if e.len > 0 {
                     def c1 = c + e
                     if _path_exists(c1) { return ospath.normalize(c1) }
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
   if n > 0 && (eq(args.get(0, ""), path) || eq(args.get(0, ""), rpath)) { return 0 }
   1
}

fn _build_argv(str path, str rpath, list args) list {
   def n = args.len
   def offset = _argv_offset(path, rpath, args, n)
   mut argv = list(n + offset)
   if offset == 1 { argv = argv.append(rpath) }
   mut i = 0
   while i < n {
      argv = argv.append(args.get(i, ""))
      i += 1
   }
   argv
}

fn waitpid(int pid, int options) int {
   "Waits for `pid` and returns the **exit code** (0..255). Negative on failure."
   #windows {
      if pid <= 0 { return -1 }
      return __wait_process(pid)
   } #else {
      def status_ptr = malloc(8)
      if status_ptr == 0 { return -1 }
      defer { free(status_ptr) }
      ; The rt_wait4 ABI consumes dynamic-value integers. Encode the typed
      ; machine pid/options explicitly so odd raw pids are not mistaken for
      ; pre-tagged immediates by the runtime bridge.
      def res = __wait4(__tag(pid), status_ptr, __tag(options))
      if res < 0 { return res }
      def int status = int(to_int(load32(status_ptr, 0)))
      if (status & 127) != 0 { return 128 + (status & 127) }
      return (status / 256) % 256
   } #endif
}

fn run(str path, list args) int {
   "Forks and execs `path` with `args` (with or without argv[0]), waits for completion, and returns the child exit code(0..255); returns `-1` if fork fails."
   mut rpath = _resolve_cmd(path)
   #if windows || macos {
      def argv = _build_argv(path, rpath, args)
      def code = __spawn_wait(rpath, argv)
      return code
   } #else {
      def argv = _build_argv(path, rpath, args)
      return __spawn_wait(rpath, argv)
   } #endif
}

fn popen(str path, list args) any {
   "Starts `path` with stdin/stdout pipes(stderr merged) and returns `[pid, child_stdin_fd, child_stdout_fd]`; returns `0` on setup failure."
   mut rpath = _resolve_cmd(path)
   #windows {
      def argv = _build_argv(path, rpath, args)
      def fds = malloc(8)
      if fds == 0 { return 0 }
      defer { free(fds) }
      def int pid = __spawn_pipe(rpath, argv, fds)
      if pid <= 0 { return 0 }
      ;; The spawn helper writes raw 32-bit fds into the buffer; no untag.
      def int stdin_write = load32(fds, 0)
      def int stdout_read = load32(fds, 4)
      if stdin_write < 0 || stdout_read < 0 {
         sys_close_quiet(stdin_write)
         sys_close_quiet(stdout_read)
         return 0
      }
      return [__tag(pid), __tag(stdin_write), __tag(stdout_read)]
   }
   #endif
   def fds = malloc(8)
   if fds == 0 { return 0 }
   defer { free(fds) }
   def int pid = __spawn_pipe(rpath, args, fds)
   if pid <= 0 { return 0 }
   def int stdin_write = load32(fds, 0)
   def int stdout_read = load32(fds, 4)
   if stdin_write < 0 || stdout_read < 0 {
      sys_close_quiet(stdin_write)
      sys_close_quiet(stdout_read)
      return 0
   }
   [__tag(pid), __tag(stdin_write), __tag(stdout_read)]
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
   #linux {
      assert(run(sh, ["-c", "exit 9"]) == 9, "process run options without argv[0]")
   }
   assert(run("nytrix-does-not-exist-nytrix", ["nytrix-does-not-exist-nytrix"]) != 0, "process run missing command")
   print("✓ core.os.process self-test passed")
}
