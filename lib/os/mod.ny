;; Keywords: os
;; Os module.

module std.os (
   pid, ppid, env, environ, getcwd, uid, gid, file_read, file_write, file_exists, file_append,
   file_remove, os, arch, temp_dir, home_dir, config_dir, data_dir, cache_dir,
   gpu_mode, gpu_backend, gpu_offload, gpu_min_work, gpu_async, gpu_fast_math,
   gpu_available, gpu_should_offload, gpu_offload_status,
   accel_target, accel_targets, accel_target_available, accel_target_triple, accel_binary_kind,
   accel_binary_ext, accel_target_status, accel_compile_plan,
   parallel_mode, parallel_threads, parallel_min_work, parallel_should_threads, parallel_status,
   OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM,
   GPU_MODE, GPU_BACKEND, GPU_OFFLOAD, GPU_MIN_WORK, GPU_ASYNC, GPU_FAST_MATH, GPU_AVAILABLE,
   ACCEL_TARGET, ACCEL_OBJECT,
   PARALLEL_MODE, PARALLEL_THREADS, PARALLEL_MIN_WORK,
   set_clipboard_text, get_clipboard_text, exit
)
use std.core *
use std.core.error *
use std.text *
use std.os.sys *
use std.text.io *
use std.os.path as ospath
use std.os.clipboard as cb
use std.os.platform as platform

fn set_clipboard_text(text){
   "Sets the system clipboard text."
   cb.set_text(text)
}

fn get_clipboard_text(){
   "Retrieves text from the system clipboard."
   cb.get_text()
}

use std.os.prim *
use std.os.dirs *
use std.os.gpu *
use std.os.parallel *

fn _is_transient_file_error(code){
   "Internal: returns true for transient Windows file operation errors."
   platform.is_windows() && (code == -22 || code == -13 || code == -5)
}

fn _open_with_retry(path, flags, mode) -> Result {
   "Internal: retries sys_open for transient file errors."
   mut tries = 0
   mut last = err(-1)
   while(tries < 5){
      last = sys_open(path, flags, mode)
      if(is_ok(last)){ return last }
      if(_is_transient_file_error(unwrap_err(last))){
         msleep(10)
         tries += 1
         continue
      }
      return last
   }
   last
}

fn _file_write_impl(path, content, flags) -> Result {
   "Internal: opens `path` with `flags` and writes the full contents of `content`."
   def p = ospath.normalize(path)
   def open_res = _open_with_retry(p, flags, 420)
   if(is_err(open_res)){ return open_res }
   def fd = unwrap(open_res)
   defer { unwrap(sys_close(fd)) }
   def n = str_len(content)
   return sys_write(fd, content, n)
}

fn getcwd(){
   "Returns the current working directory as a string; returns `\"\"` if `getcwd(2)` fails."
   mut buf = malloc(4096)
   mut clen = __getcwd(buf, 4096)
   if(clen <= 0){
      free(buf)
      ""
   } else {
      def s = cstr_to_str(buf)
      free(buf)
      s
   }
}

fn uid(){
   "Returns the **real user ID** of the calling process via **getuid(2)**."
   __getuid()
}

fn gid(){
   "Returns the **real group ID** of the calling process via **getgid(2)**."
   __getgid()
}

fn file_read(path) -> Result {
   "Reads the whole file at `path`; returns `ok(content_string)` or `err(errno_like_code)`."
   def p = ospath.normalize(path)
   match sys_open(p, 0, 0){ ; O_RDONLY
      ok(fd) -> {
         defer { unwrap(sys_close(fd)) }
         mut cap = 4096
         mut buf = malloc(cap + 16)
         def tmp = malloc(4096)
         defer { free(tmp) }
         mut tlen = 0
         while(true){
            match sys_read(fd, tmp, 4096){
               ok(r) -> {
                  if(r <= 0){ break }
                  if(tlen + r >= cap){
                     while(tlen + r >= cap){ cap = cap * 2 }
                     buf = realloc(buf, cap + 16)
                  }
                  __copy_mem(buf + 16 + tlen, tmp, r)
                  tlen = tlen + r
               }
               err(e) -> { return err(e) }
            }
         }
         store8(buf + 16, 0, tlen)
         return ok(init_str(buf + 16, tlen))
      }
      err(e) -> { return err(e) }
   }
}

fn file_write(path, content) -> Result {
   "Writes `content` to `path` (truncate/create); returns `ok(bytes_written)` or `err(errno_like_code)`."
   return _file_write_impl(path, content, 577) ; WRONLY|CREAT|TRUNC, 0644
}

fn file_exists(path){
   "Returns true when `path` exists (file or directory)."
   def p = ospath.normalize(path)
   mut res = __access(p, 0)
   res == 0
}

fn file_append(path, content) -> Result {
   "Appends `content` to `path` (create if missing); returns `ok(bytes_written)` or `err(errno_like_code)`."
   return _file_write_impl(path, content, 1089) ; WRONLY|CREAT|APPEND, 0644
}

fn file_remove(path) -> Result {
   "Removes file `path`; returns `ok(0)` on success or `err(errno_like_code)`."
   def p = ospath.normalize(path)
   mut tries = 0
   mut res = -1
   while(tries < 5){
      res = __unlink(p)
      if(res >= 0){ return ok(0) }
      if(_is_transient_file_error(res)){
         msleep(10)
         tries += 1
         continue
      }
      break
   }
   return err(res)
}

fn exit(code=0){
   "Terminates the calling process with the given status code."
   __exit(code)
}

if(comptime{__main()}){
    use std.core *
    use std.core.error *
    use std.core.reflect *
    use std.text *

    print("Testing OS Mod...")

    def p = __getpid()
    assert(p > 0, "pid > 0")

    def pp = __getppid()
    if(eq(os(), "windows")){
        print("Windows ppid:", pp)
        assert(pp >= 0, "Windows ppid should be non-negative")
    } else {
        assert(pp > 0, "ppid > 0")
    }

    def u = uid()
    assert(u >= 0, "uid >= 0")

    def g = gid()
    assert(g >= 0, "gid >= 0")

    def path = env("PATH")
    if(path != 0){
     assert(str_len(path) >= 0, "env PATH len")
    } else {
     assert(0, "env PATH missing")
    }

    def e = environ()
    assert(type(e) == "list", "environ list")
    assert(len(e) > 0, "environ len")

    ; Platform tests
    def o = os()
    assert(is_str(o), "os() is string")
    assert(len(o) > 0, "os() not empty")

    def a = arch()
    assert(is_str(a), "arch() is string")
    assert(len(a) > 0, "arch() not empty")

    print("Platform: " + o + " (" + a + ")")

    ;; File I/O tests skipped in bundled comptime (syscall restrictions)

    print("✓ std.os.mod tests passed")
}
