;; Keywords: os primitives
;; Core OS primitives used by other os submodules.

module std.os.prim (
   pid, ppid, env, environ, os, arch,
   OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM
)

use std.core *

fn pid(){
   "Returns the process ID."
   __getpid()
}

fn ppid(){
   "Returns the parent process ID."
   __getppid()
}

fn env(key){
   "Returns the value of environment variable `key`."
   def ep = __envp()
   if(!ep){ return 0 }
   else {
      def key_len = str_len(key)
      mut i = 0
      mut res = 0
      while(load64(ep, i*8)){
         def env_entry = load64(ep, i*8)
         mut matches = 1
         mut j = 0
         while(j < key_len){
            if(load8(env_entry, j) != load8(key, j)){
               matches = 0
               break
            }
            j += 1
         }
         if(matches && load8(env_entry, key_len) == 61){
            res = cstr_to_str(env_entry, key_len + 1)
            break
         }
         i += 1
      }
      res
   }
}

fn environ(){
   "Returns a list of environment entries."
   def ep = __envp()
   if(!ep){ return list(8) }
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

fn os(){
   "Returns the operating system name."
   __os_name()
}

fn arch(){
   "Returns the architecture name."
   __arch_name()
}

def OS = os()
def ARCH = arch()
def IS_LINUX = (OS == "linux")
def IS_MACOS = (OS == "macos")
def IS_WINDOWS = (OS == "windows")
def IS_X86_64 = (ARCH == "x86_64")
def IS_AARCH64 = (ARCH == "aarch64" || ARCH == "arm64")
def IS_ARM = (ARCH == "arm")
