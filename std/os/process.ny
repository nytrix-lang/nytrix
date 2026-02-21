;; Keywords: os process
;; Process module.

module std.os.process (
   run, popen, waitpid
)
use std.core *
use std.core as core
use std.os.sys *
use std.os *
use std.str *
use std.core.reflect *
use std.os.path as ospath

fn _is_windows(){
   "Internal helper."
   eq(__os_name(), "windows")
}

fn _is_macos(){
   "Internal helper."
   eq(__os_name(), "macos")
}

fn _resolve_cmd(cmd){
   "Internal helper."
   if(!is_str(cmd)){ return cmd }
   if(ospath.has_sep(cmd) || ospath.is_abs(cmd)){ return ospath.normalize(cmd) }
   def p = env("PATH")
   if(!is_str(p) || str_len(p) == 0){ return cmd }
   mut sep = ":"
   if(_is_windows()){ sep = ";" }
   def dirs = split(p, sep)
   def has_dot = find(cmd, ".") >= 0
   mut exts = list(0)
   if(_is_windows() && !has_dot){
      def pe = env("PATHEXT")
      if(is_str(pe) && str_len(pe) > 0){
         exts = split(pe, ";")
      } else {
         exts = [".exe", ".cmd", ".bat", ".com"]
      }
   }
   mut i = 0
   while(i < len(dirs)){
      def d = get(dirs, i, "")
      if(str_len(d) > 0){
         def c = ospath.join(d, cmd)
         if(file_exists(c)){ return ospath.normalize(c) }
         if(_is_windows() && !has_dot){
            mut j = 0
            while(j < len(exts)){
               def e = get(exts, j, "")
               if(str_len(e) > 0){
                  def c1 = c + e
                  if(file_exists(c1)){ return ospath.normalize(c1) }
               }
               j += 1
            }
         }
      }
      i += 1
   }
   ospath.normalize(cmd)
}

fn waitpid(pid, options){
   "Waits for `pid` and returns the **exit code** (0..255). Negative on failure."
   if(_is_windows()){
      if(pid <= 0){ return -1 }
      return __wait_process(pid)
   }
   def status_ptr = malloc(8)
   def res = __wait4(pid, status_ptr, options)
   if(res < 0){
       free(status_ptr)
       return res
   }
   def status = load32(status_ptr, 0)
   free(status_ptr)
   ;; Normalize to exit code
   if((status & 127) != 0){ return 128 + (status & 127) }
   (status / 256) % 256
}

fn run(path, args){
    "Forks and execs `path` with `args`, waits for completion, and returns the child exit code (0..255); returns `-1` if fork fails."
    mut rpath = _resolve_cmd(path)
    def n = core.len(args)
    if(_is_windows()){
        mut offset = 1
        if(n > 0 && (eq(get(args, 0, ""), path) || eq(get(args, 0, ""), rpath))){ offset = 0 }
        def argv = malloc((n + offset + 1) * 8)
        if(offset == 1){ store64(argv, rpath, 0) }
        mut i = 0
        while(i < n){
            store64(argv, get(args, i, 0), (i + offset) * 8)
            i += 1
        }
        store64(argv, 0, (n + offset) * 8)
        def code = __spawn_wait(rpath, argv)
        free(argv)
        return code
    }
    if(_is_macos()){
        mut offset = 1
        if(n > 0 && (eq(get(args, 0, ""), path) || eq(get(args, 0, ""), rpath))){ offset = 0 }
        def argv = malloc((n + offset + 1) * 8)
        if(offset == 1){ store64(argv, rpath, 0) }
        mut i = 0
        while(i < n){
            store64(argv, get(args, i, 0), (i + offset) * 8)
            i += 1
        }
        store64(argv, 0, (n + offset) * 8)
        def code = __spawn_wait(rpath, argv)
        free(argv)
        return code
    }
    mut pid = __fork()
    if(pid == 0){
        mut offset = 1
        if(n > 0 && (eq(get(args, 0, ""), path) || eq(get(args, 0, ""), rpath))){ offset = 0 }
        def argv = malloc((n + offset + 1) * 8)
        if(offset == 1){ store64(argv, rpath, 0) }
        mut i = 0
        while(i < n){
            store64(argv, get(args, i, 0), (i + offset) * 8)
            i += 1
        }
        store64(argv, 0, (n + offset) * 8)
        __execve(rpath, argv, 0)
        __exit(127)
    } else {
        if(pid < 0){ return -1 }
        def status_ptr = malloc(8)
        def wr = __wait4(pid, status_ptr, 0)
        if(wr < 0){
            free(status_ptr)
            return -1
        }
        def status = load32(status_ptr, 0)
        free(status_ptr)
        return (status / 256) % 256
    }
}

fn popen(path, args){
    "Starts `path` with stdin/stdout pipes and returns `[pid, child_stdin_fd, child_stdout_fd]`; returns `0` on setup failure."
    mut rpath = _resolve_cmd(path)
    def n = core.len(args)
    if(_is_windows() || _is_macos()){
        def fds = malloc(8)
        if(!fds){ return 0 }
        mut offset = 1
        if(n > 0 && (eq(get(args, 0, ""), path) || eq(get(args, 0, ""), rpath))){ offset = 0 }
        def argv = malloc((n + offset + 1) * 8)
        if(offset == 1){ store64(argv, rpath, 0) }
        mut i = 0
        while(i < n){
            store64(argv, get(args, i, 0), (i + offset) * 8)
            i += 1
        }
        store64(argv, 0, (n + offset) * 8)
        def pid = __spawn_pipe(rpath, argv, fds)
        free(argv)
        if(pid < 0){
            free(fds)
            return 0
        }
        def in_w = load32(fds, 0)
        def out_r = load32(fds, 4)
        free(fds)
        mut ret = list(3)
        ret = append(ret, pid)
        ret = append(ret, in_w)
        ret = append(ret, out_r)
        return ret
    }

    def p_stdin = malloc(8) ;; 2 ints = 8 bytes
    def p_stdout = malloc(8)

    if(__pipe(p_stdin) < 0){
        free(p_stdin)
        free(p_stdout)
        return 0
    } ;; pipe (syscall 22 for x86-64)
    if(__pipe(p_stdout) < 0){
        def in_r = load32(p_stdin, 0)
        def in_w = load32(p_stdin, 4)
        unwrap(sys_close(in_r))
        unwrap(sys_close(in_w))
        free(p_stdin)
        free(p_stdout)
        return 0
    }

    def stdin_read = load32(p_stdin, 0)
    def stdin_write = load32(p_stdin, 4)
    def stdout_read = load32(p_stdout, 0)
    def stdout_write = load32(p_stdout, 4)

    free(p_stdin)
    free(p_stdout)

    mut pid = __fork()
    if(pid < 0){
        unwrap(sys_close(stdin_read))
        unwrap(sys_close(stdin_write))
        unwrap(sys_close(stdout_read))
        unwrap(sys_close(stdout_write))
        return 0
    }
    if(pid == 0){
        ;; Child
        ;; dup2(stdin_read, 0)
        __dup2(stdin_read, 0)
        ;; dup2(stdout_write, 1)
        __dup2(stdout_write, 1)

        ;; Close unused
        unwrap(sys_close(stdin_write))
        unwrap(sys_close(stdout_read))
        unwrap(sys_close(stdin_read))
        unwrap(sys_close(stdout_write))

        ;; Exec
        mut offset = 1
        if(n > 0 && (eq(get(args, 0, ""), path) || eq(get(args, 0, ""), rpath))){ offset = 0 }
        def argv = malloc((n + offset + 1) * 8)
        if(offset == 1){ store64(argv, rpath, 0) }
        mut i = 0
        while(i < n){
            store64(argv, get(args, i, 0), (i + offset) * 8)
            i += 1
        }
        store64(argv, 0, (n + offset) * 8)

        __execve(rpath, argv, 0)
        __exit(127)
    }
 else {
        ;; Parent
        ;; Close child side
        unwrap(sys_close(stdin_read))
        unwrap(sys_close(stdout_write))

        mut ret = list(3)
        ret = append(ret, pid)
        ret = append(ret, stdin_write)
        ret = append(ret, stdout_read)
        ret
    }
}

if(comptime{__main()}){
    use std.os.process *
    use std.os *
    use std.os.fs *
    use std.os.sys *
    use std.core *
    use std.str *
    use std.str.io *
    use std.core.error *

    fn _pick_win_shell(comspec){
       mut sh = "cmd"
       if(file_exists("C:\\Windows\\System32\\cmd.exe")){ return "C:\\Windows\\System32\\cmd.exe" }
       if(file_exists("C:\\WINNT\\System32\\cmd.exe")){ return "C:\\WINNT\\System32\\cmd.exe" }

       mut root = env("SystemRoot")
       if(!is_str(root)){ root = env("SYSTEMROOT") }
       if(!is_str(root)){ root = env("windir") }
       if(!is_str(root)){ root = env("WINDIR") }
       if(is_str(root)){
          def rr = strip(root)
          if(str_len(rr) > 0){
             def c = rr + "\\System32\\cmd.exe"
             if(file_exists(c)){ return c }
          }
       }

       if(is_str(comspec)){
          mut clean = replace(strip(comspec), "\"", "")
          if(is_str(root) && str_len(strip(root)) > 0){
             def rr = strip(root)
             clean = replace(clean, "%SystemRoot%", rr)
             clean = replace(clean, "%SYSTEMROOT%", rr)
             clean = replace(clean, "%WINDIR%", rr)
             clean = replace(clean, "%windir%", rr)
          }
          if(str_len(clean) > 0){
             if(file_exists(clean)){ return clean }
             if(find(clean, "%") < 0){ sh = clean }
          }
       }
       sh
    }

    fn _pick_posix_shell(){
       if(file_exists("/bin/sh")){ return "/bin/sh" }
       "sh"
    }

    fn _pick_cat_cmd(){
       if(file_exists("/bin/cat")){ return "/bin/cat" }
       "cat"
    }

    print("Testing Process...")

    def comspec = env("COMSPEC")
    mut is_win = false
    mut sh = _pick_win_shell(comspec)
    mut ok_cmd = sh
    mut ok_args = [sh, "/c", "exit", "0"]
    mut exit_cmd = sh
    mut exit_args = [sh, "/c", "exit", "7"]
    mut cat_cmd = sh
    mut cat_args = [sh, "/c", "more"]

    mut res = run(ok_cmd, ok_args)
    if(res == 0){
       is_win = true
    } else {
       def ps = _pick_posix_shell()
       ok_cmd = ps
       ok_args = [ps, "-c", "exit 0"]
       exit_cmd = ps
       exit_args = [ps, "-c", "exit 7"]
       cat_cmd = _pick_cat_cmd()
       cat_args = [cat_cmd]
       res = run(ok_cmd, ok_args)
    }

    if(res != 0){
       print("run zero exit=" + to_str(res) + " cmd=" + ok_cmd)
       assert(0, "run zero exit")
    }

    assert(run(exit_cmd, exit_args) == 7, "run returns exit code")
    def missing = run("nytrix-does-not-exist-nytrix", ["nytrix-does-not-exist-nytrix"])
    assert(missing != 0, "run returns non-zero when exec fails")

    print("Testing popen...")

    def p = popen(cat_cmd, cat_args)
    if(p == 0){
       print("popen failed, returned 0")
       assert(0, "popen returned 0")
    }
    def pid = get(p, 0, 0)
    def stdin = get(p, 1, 0)
    def stdout = get(p, 2, 0)

    def msg = "hello pipe"
    def nw = sys_write(stdin, msg, str_len(msg))
    assert(!is_err(nw), "pipe write")
    assert(unwrap(nw) == str_len(msg), "pipe write bytes")
    assert(!is_err(sys_close(stdin)), "close child stdin")

    def buf = malloc(100)
    def nr_res = sys_read(stdout, buf, 100)
    mut nr = 0
    if(!is_err(nr_res)){ nr = unwrap(nr_res) }

    init_str(buf, nr)

    assert(str_contains(buf, msg), "pipe echo match")

    def status = waitpid(pid, 0)
    assert(status >= 0, "waitpid returns status")
    assert(!is_err(sys_close(stdout)), "close child stdout")

    print("✓ std.process.mod tests passed")
}
