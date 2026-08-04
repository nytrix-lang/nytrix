;; Keywords: browser input gamepad portability fixture
;; Browser gamepad mapping must be reachable from the page. The host self-test
;; (loaded with #gamepad-selftest) injects a standard-mapped synthetic Gamepad;
;; this fixture drives the test_report_gamepad bridge, which samples the mapped
;; gamepad (pad 0) and echoes button A and left-stick X into data-gamepad-*
;; attributes the harness greps out of --dump-dom. The browser cannot readily
;; round-trip window-module host-returned values into a further host call, so the
;; report host owns the read-and-mapping step (same split the touch self-test
;; uses to expose observed state).
use std.os.ui.render as gfx
use std.os.ui.window as window

def win = gfx.init_window(320, 180, "Nytrix browser gamepad", 0, true, false, 1)
mut n = 0
while !gfx.window_should_close() {
   window.test_report_gamepad(0, 0, 0.0)
   gfx.begin_frame_clear(gfx.BLACK)
   n += 1
   if n > 2 {
      window.set_should_close(win, true)
   }
   gfx.end_frame()
}