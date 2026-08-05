;; Keywords: browser web fullscreen pointer-lock portability fixture
;; Browser window requests must remain asynchronous and permission-aware.
use std.os.ui.render as gfx
use std.os.ui.window as window

def win = gfx.init_window(320, 180, "Nytrix browser requests", 0, true, false, 1)
window.set_window_fullscreen(win, false)
window.set_input_exclusive(win, false)
gfx.begin_frame_clear(gfx.BLACK)
gfx.draw_rect(24.0, 24.0, 272.0, 132.0, gfx.WHITE)
gfx.end_frame()
