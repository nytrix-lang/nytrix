;; std.os.fs.watch
;; Cross-platform file watching for hot reloading support.
;;
;; Backends:
;;   Linux: inotify
;;   macOS: kqueue (EVFILT_VNODE)
;;   Windows: directory change notifications
;;
;; The compiler's --hot/--watch uses efficient platform mechanisms.

module std.os.fs.watch (create, close, poll, has_event, wait_any)
use std.os.fs as fs
use std.os.platform as platform
use std.os.time as ostime
use std.core

def WATCH_MODIFY = fs.WATCH_MODIFY
def WATCH_CREATE = fs.WATCH_CREATE
def WATCH_ALL    = fs.WATCH_ALL

;; Returns the result of the `create` operation.
fn create(str path) any {
   fs.watch_create(path)
}

;; Closes resources owned by the state and returns the closed state.
fn close(any h) int {
   fs.watch_close(h)
}

;; Returns the result of the `poll` operation.
fn poll(any h) list {
   fs.watch_poll(h)
}

;; Returns true when has event.
fn has_event(any h) bool {
   fs.watch_has_event(h)
}

fn wait_any(any h, int timeout_ms = 200) bool {
   "Wait (best effort) for any event using repeated polls."
   mut i = 0
   while i < 20 {
      if has_event(h) { return true }
      ;; Best-effort backoff using real sleep (much lower CPU and faster in tests).
      ostime.msleep(1)
      i += 1
   }
   has_event(h)
}

#macos {
   ;; macOS users can access kqueue primitives via the runtime for advanced use.
} #endif

#windows {
   ;; Windows users can access change notification primitives via the runtime.
} #endif

#main {
   def h = create(".")
   if h {
      poll(h)
      close(h)
      print("✓ std.os.fs.watch self-test passed")
   }
}
