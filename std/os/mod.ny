;; Keywords: os
;; Os module.

use std.core *
use std.str *
use std.os.sys *
use std.str.io *
module std.os (
   pid, ppid, env, environ, getcwd, uid, gid, file_read, file_write, file_exists, file_append,
   file_remove, os, arch
)

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
   "Retrieves the value of an environment variable `key`. Returns `0` if not found."
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
   "Returns a [[std.core::list]] of all environment variables in `KEY=VALUE` format."
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
   "Returns the absolute path of the **current working directory**."
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

fn file_read(path){
   "Reads entire file at `path` into a string. Returns empty string on failure."
   def fd = syscall(2, path, 0, 0, 0, 0, 0)
   if(fd <= 0){ return "" }
   mut cap = 4096
   mut buf = malloc(cap)
   def tmp = malloc(4096)
   mut tlen = 0
   while(true){
      def r = sys_read(fd, tmp, 4096)
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
   syscall(3, fd, 0, 0, 0, 0, 0)
   free(tmp)
   store8(buf, 0, tlen)
   init_str(buf, tlen)
   buf
}

fn file_write(path, content){
   "Writes string `content` to file at `path`. Returns bytes written or 0 on failure."
   def fd = syscall(2, path, 577, 420, 0,0,0) ;; open(path, WRONLY|CREAT|TRUNC, 0644)
   if(fd < 0){ return 0 }
   def n = str_len(content)
   ;; TODO: handle bytes if needed
   
   mut res = syscall(1, fd, content, n, 0,0,0)
   syscall(3, fd, 0,0,0,0,0)
   res
}

fn file_exists(path){
   "Returns true if file at `path` exists."
   mut res = syscall(21, path, 0, 0, 0, 0, 0)
   res == 0
}

fn file_append(path, content){
   "Appends string `content` to file at `path`. Returns bytes written or 0 on failure."
   def fd = syscall(2, path, 1089, 420, 0,0,0) ;; open(path, WRONLY|CREAT|APPEND, 0644)
   if(fd < 0){ return 0 }
   def n = str_len(content)
   mut res = syscall(1, fd, content, n, 0,0,0)
   syscall(3, fd, 0,0,0,0,0)
   res
}

fn file_remove(path){
   "Removes the file at `path`. Returns true on success."
   mut res = syscall(87, path, 0, 0, 0, 0, 0)
   res == 0
}
