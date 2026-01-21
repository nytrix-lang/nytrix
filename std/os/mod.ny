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
   "Return current process id."
   return rt_syscall(39,0,0,0,0,0,0)
}

fn ppid(){
   "Return parent process id."
   return rt_syscall(110,0,0,0,0,0,0)
}

fn env(name){
   "Get environment variable."
   def envp = rt_envp()
   if(!envp){ return 0 }
   def name_len = str_len(name)
   def i = 0
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
         j = j + 1
      }
      ;; Check for '=' after the name (prevents partial matches)
      if (matches && load8(env_entry, name_len) == 61) {
         ;; Found it! Extract the value
         return cstr_to_str(env_entry, name_len + 1)
      }
      i = i + 1
   }
   return 0
}

fn environ(){
   "All environment variables as a list of strings."
   def envp = rt_envp()
   if(!envp){ return list(8) }
   def n = rt_envc()
   if(n <= 0){ return list(8) }
   def xs = list(8)
   def i = 0
   while(i < n && load64(envp, i*8)){
      def s_raw = load64(envp, i*8)
      xs = append(xs, s_raw)
      i = i + 1
   }
   return xs
}

fn getcwd(){
   "Current working directory."
   def buf = rt_malloc(4096)
   def len = rt_syscall(79, buf, 4096, 0,0,0,0)
   if(len <= 0){
      rt_free(buf)
      return ""
   }
   def s = cstr_to_str(buf)
   rt_free(buf)
   return s
}

fn uid(){
   "Return user id."
   return rt_syscall(102,0,0,0,0,0,0)
}

fn gid(){
   "Return group id."
   return rt_syscall(104,0,0,0,0,0,0)
}
