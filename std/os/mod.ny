;; Keywords: os mod
;; Os Mod module.

use std.core
use std.strings.str
use std.collections
use std.io
module std.os (
   pid, ppid, env, environ, getcwd, uid, gid
)

fn pid(){
   "Returns the **process ID** of the calling process via **getpid(2)**."
   __syscall(39,0,0,0,0,0,0)
}

fn ppid(){
   "Returns the **parent process ID** of the calling process via **getppid(2)**."
   __syscall(110,0,0,0,0,0,0)
}

fn env(name){
   "Retrieves the value of an environment variable `name`. Returns `0` if not found."
   def envp = __envp()
   if(!envp){ 0 }
   else {
      def name_len = str_len(name)
      def i = 0
      def res = 0
      while(load64(envp, i*8)){
         def env_entry = load64(envp, i*8)
         ;; Check if this entry starts with our variable name
         def matches = 1
         def j = 0
         while (j < name_len) {
            if (load8(env_entry, j) != load8(name, j)) {
               matches = 0
               break
            }
            j += 1
         }
         ;; Check for '=' after the name (prevents partial matches)
         if (matches && load8(env_entry, name_len) == 61) {
            ;; Found it! Extract the value
            res = cstr_to_str(env_entry, name_len + 1)
            break
         }
         i += 1
      }
      res
   }
}

fn environ(){
   "Returns a [[std.core::list]] of all environment variables in `KEY=VALUE` format."
   def envp = __envp()
   if(!envp){ list(8) }
   else {
      def n = __envc()
      if(n <= 0){ list(8) }
      else {
         def xs = list(8)
         def i = 0
         while(i < n && load64(envp, i*8)){
            def s_raw = load64(envp, i*8)
            xs = append(xs, s_raw)
            i += 1
         }
         xs
      }
   }
}

fn getcwd(){
   "Returns the absolute path of the **current working directory**."
   def buf = __malloc(4096)
   def len = __syscall(79, buf, 4096, 0,0,0,0)
   if(len <= 0){
      __free(buf)
      ""
   } else {
      def s = cstr_to_str(buf)
      __free(buf)
      s
   }
}

fn uid(){
   "Returns the **real user ID** of the calling process via **getuid(2)**."
   __syscall(102,0,0,0,0,0,0)
}

fn gid(){
   "Returns the **real group ID** of the calling process via **getgid(2)**."
   __syscall(104,0,0,0,0,0,0)
}