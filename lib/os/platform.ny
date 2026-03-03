;; Keywords: os platform
;; Shared host platform predicates.

module std.os.platform (
   is_windows, is_macos, is_linux
)

use std.core *

fn is_windows(){
   "Returns whether the host operating system is Windows."
   __os_name() == "windows"
}

fn is_macos(){
   "Returns whether the host operating system is macOS."
   __os_name() == "macos"
}

fn is_linux(){
   "Returns whether the host operating system is Linux."
   __os_name() == "linux"
}
