;; Keywords: prim primitives syscalls os
;; Core OS primitives used by other os submodules.
;; References:
;; - core.os
module core.os.prim(pid, ppid, env, environ, os, arch, is_linux, is_macos, is_windows, OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM)
use core
use core.str as core_str

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
   if !is_str(key) { return nil }
   def int key_len = key.len
   if key_len == 0 { return nil }
   def int n = envc()
   mut i = 0
   while i < n {
      def ptr entry = envp(i)
      if entry {
         mut j = 0
         while j < key_len && load8(entry, j) == load8(key, j) { j += 1 }
         if j == key_len && load8(entry, key_len) == 61 {
            ;; Convert from the raw C pointer directly. The generic
            ;; cstr_to_str wrapper accepts a dynamic `any` value, while envp
            ;; supplies a native pointer word; crossing that boundary through
            ;; the wrapper loses the pointer class in NYIR.
            def ptr value_ptr = ptr_add(entry, key_len + 1)
            def str value = __cstr_to_str(value_ptr)
            return value
         }
      }
      i += 1
   }
   nil
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

@inline
fn is_linux() bool { IS_LINUX }

@inline
fn is_macos() bool { IS_MACOS }

@inline
fn is_windows() bool { IS_WINDOWS }
