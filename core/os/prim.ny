;; Keywords: prim primitives syscalls os
;; Core OS primitives used by other os submodules.
;; References:
;; - core.os
module core.os.prim(pid, ppid, env, environ, os, arch, is_linux, is_macos, is_windows, OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM)
use core
use core.str as core_str

;; Returns the process ID.
fn pid() int {
   __getpid()
}

;; Returns the parent process ID.
fn ppid() int {
   __getppid()
}

;; Returns the value of environment variable `key`.
fn env(str key) any {
   if !is_str(key) { return nil }
   def value = __env_get(key)
   if value == 0 { return nil }
   value
}

;; Returns a list of environment entries.
fn environ() list {
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

;; Returns the operating system name.
fn os() str {
   __os_name()
}

;; Returns the architecture name.
fn arch() str {
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

@inline

;; Returns whether the target OS is Linux.
fn is_linux() bool { IS_LINUX }

@inline

;; Returns whether the target OS is macOS.
fn is_macos() bool { IS_MACOS }

@inline

;; Returns whether the target OS is Windows.
fn is_windows() bool { IS_WINDOWS }
