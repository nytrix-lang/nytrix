;; Keywords: prim primitives syscalls os
;; Core OS primitives used by other os submodules.
;; References:
;; - std.os
module std.os.prim(pid, ppid, env, environ, os, arch, OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM)
use std.core
use std.core.str as core_str

fn pid() int {
   "Returns the process ID."
   __getpid()
}

fn ppid() int {
   "Returns the parent process ID."
   __getppid()
}

fn env(str key) any {
   "Returns the value of environment variable `key`."
   __env_get(key)
}

fn environ() list {
   "Returns a list of environment entries."
   def ep = __envp()
   if !ep { return list(8) }
   else {
      def n = envc()
      if n <= 0 { list(8) }
      else {
         mut xs = list(8)
         mut i = 0
         while i < n && load64(ep, i*8) {
            def s_raw = load64(ep, i*8)
            xs = xs.append(core_str.cstr_to_str(s_raw))
            i += 1
         }
         xs
      }
   }
}

fn os() str {
   "Returns the operating system name."
   __os_name()
}

fn arch() str {
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
