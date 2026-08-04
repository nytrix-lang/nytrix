;; Keywords: browser input touch pointer portability fixture
;; Browser touch input must flow through the public input facade. The host
;; self-test (loaded with #touch-selftest) synthesizes a TouchEvent sequence;
;; this fixture loops, reads touch state through the public facade, and echoes
;; it back via test_report_touch so the harness can confirm delivery.
use std.os.ui.render as gfx
use std.os.ui.window.input as input
use std.os.ui.window as window

def win = gfx.init_window(320, 180, "Nytrix browser touch", 0, true, false, 1)
while !gfx.window_should_close() {
   def count = input.touch_count()
   def pos = input.touch_pos(0)
   window.test_report_touch(count, pos.get(0, 0.0), pos.get(1, 0.0))
   gfx.begin_frame_clear(gfx.BLACK)
   if count > 0 {
      gfx.draw_rect(pos.get(0, 0.0), pos.get(1, 0.0), 8.0, 8.0, gfx.WHITE)
      window.set_should_close(win, true)
   }
   gfx.end_frame()
}
