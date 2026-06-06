;; Keywords: platform backend os
;; Shared host platform predicates.
;; References:
;; - std.os
module std.os.platform(is_windows, is_macos, is_linux)
use std.core

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
