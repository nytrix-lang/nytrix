;; Keywords: browser time clock portability fixture
;; Browser time imports must return live wall-clock and monotonic values.
use std.os.time as time
use std.os.ui.render as gfx
use std.os.ui.window as window

def wall = time.now_ms()
def mono = time.monotonic_ns()
window.test_report_touch(wall, wall, mono)
def win = gfx.init_window(320, 180, "Nytrix browser clock", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
gfx.draw_rect(24.0, 24.0, 272.0, 132.0, gfx.WHITE)
gfx.end_frame()
