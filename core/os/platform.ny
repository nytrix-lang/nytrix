;; Keywords: platform backend os
;; Shared host platform predicates.
;; References:
;; - core.os
module core.os.platform(is_windows, is_macos, is_linux, IS_WINDOWS, IS_MACOS,
   IS_LINUX)
use core

@inline
fn is_windows() bool {
   "Returns whether the host operating system is Windows."
   #windows {
      true
   } #else {
      false
   } #endif
}

@inline
fn is_macos() bool {
   "Returns whether the host operating system is macOS."
   #macos {
      true
   } #else {
      false
   } #endif
}

@inline
fn is_linux() bool {
   "Returns whether the host operating system is Linux."
   #linux {
      true
   } #else {
      false
   } #endif
}

;; Uppercase aliases keep platform predicates convenient in conditional
;; module code while the lowercase functions remain callable predicates.
def IS_WINDOWS = is_windows()
def IS_MACOS = is_macos()
def IS_LINUX = is_linux()
