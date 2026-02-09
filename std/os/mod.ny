;; Keywords: os
;; Os module.

module std.os (
   pid, ppid, env, environ, getcwd, uid, gid, file_read, file_write, file_exists, file_append,
   file_remove, os, arch
)
use std.core *
use std.str *
use std.os.sys *
use std.str.io *

fn os() {
   "Returns the name of the operating system (e.g., 'linux', 'macos', 'windows')."
   __os_name()
}

fn arch() {
   "Returns the name of the system architecture (e.g., 'x86_64', 'aarch64')."
   __arch_name()
}

fn pid(){
   "Returns the **process ID** of the calling process via **getpid(2)**."
   syscall(39,0,0,0,0,0,0)
}

fn ppid(){
   "Returns the **parent process ID** of the calling process via **getppid(2)**."
   syscall(110,0,0,0,0,0,0)
}

fn env(key){
   "Returns the value of environment variable `key`, or `0` when no matching entry exists."
   def ep = envp()
   if(!ep){ 0 }
   else {
      def key_len = str_len(key)
      mut i = 0
      mut res = 0
      while(load64(ep, i*8)){
         def env_entry = load64(ep, i*8)
         ;; Check if this entry starts with our variable name
         mut matches = 1
         mut j = 0
         while (j < key_len) {
            if (load8(env_entry, j) != load8(key, j)) {
               matches = 0
               break
            }
            j += 1
         }
         ;; Check for '=' after the name (prevents partial matches)
         if (matches && load8(env_entry, key_len) == 61) {
            ;; Found it! Extract the value
            res = cstr_to_str(env_entry, key_len + 1)
            break
         }
         i += 1
      }
      res
   }
}

fn environ(){
   "Returns a list of environment entries in `KEY=VALUE` format."
   def ep = envp()
   if(!ep){ list(8) }
   else {
      def n = envc()
      if(n <= 0){ list(8) }
      else {
         mut xs = list(8)
         mut i = 0
         while(i < n && load64(ep, i*8)){
            def s_raw = load64(ep, i*8)
            xs = append(xs, cstr_to_str(s_raw))
            i += 1
         }
         xs
      }
   }
}

fn getcwd(){
   "Returns the current working directory as a string; returns `\"\"` if `getcwd(2)` fails."
   mut buf = malloc(4096)
   mut clen = syscall(79, buf, 4096, 0,0,0,0)
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
   syscall(102,0,0,0,0,0,0)
}

fn gid(){
   "Returns the **real group ID** of the calling process via **getgid(2)**."
   syscall(104,0,0,0,0,0,0)
}

fn file_read(path) -> Result {
   "Reads the whole file at `path`; returns `ok(content_string)` or `err(errno_like_code)`."
   match sys_open(path, 0, 0) { ; O_RDONLY
      ok(fd) -> {
         defer { unwrap(sys_close(fd)) }
         mut cap = 4096
         mut buf = malloc(cap)
         def tmp = malloc(4096)
         defer { free(tmp) }
         mut tlen = 0
         while(true){
            match sys_read(fd, tmp, 4096) {
               ok(r) -> {
                  if(r <= 0){ break }
                  if(tlen + r >= cap){
                     while(tlen + r >= cap){ cap = cap * 2 }
                     buf = realloc(buf, cap)
                  }
                  mut i = 0
                  while(i < r){
                     store8(buf, load8(tmp, i), tlen + i)
                     i = i + 1
                  }
                  tlen = tlen + r
               }
               err(e) -> { return err(e) }
            }
         }
         store8(buf, 0, tlen)
         return ok(init_str(buf, tlen))
      }
      err(e) -> { return err(e) }
   }
}

fn file_write(path, content) -> Result {
   "Writes `content` to `path` (truncate/create); returns `ok(bytes_written)` or `err(errno_like_code)`."
   match sys_open(path, 577, 420) { ;; open(path, WRONLY|CREAT|TRUNC, 0644)
      ok(fd) -> {
         defer { unwrap(sys_close(fd)) }
         def n = str_len(content)
         return sys_write(fd, content, n)
      }
      err(e) -> { return err(e) }
   }
}

fn file_exists(path){
   "Returns true when `path` exists (file or directory)."
   mut res = syscall(21, path, 0, 0, 0, 0, 0)
   res == 0
}

fn file_append(path, content) -> Result {
   "Appends `content` to `path` (create if missing); returns `ok(bytes_written)` or `err(errno_like_code)`."
   match sys_open(path, 1089, 420) { ;; open(path, WRONLY|CREAT|APPEND, 0644)
      ok(fd) -> {
         defer { unwrap(sys_close(fd)) }
         def n = str_len(content)
         return sys_write(fd, content, n)
      }
      err(e) -> { return err(e) }
   }
}

fn file_remove(path) -> Result {
   "Removes file `path`; returns `ok(0)` on success or `err(errno_like_code)`."
   mut res = syscall(87, path, 0, 0, 0, 0, 0)
   if(res < 0){ return err(res) }
   return ok(0)
}
