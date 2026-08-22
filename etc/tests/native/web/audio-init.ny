;; Keywords: browser web audio portability fixture
;; Exercises the standard audio facade through the browser target.
use std.os.sound as sound
use std.os.ui.render as gfx

def backend = sound.init(false)

if !backend { sound.shutdown() }
def win = gfx.init_window(320, 180, "Nytrix browser audio", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
gfx.draw_rect(24.0, 24.0, 272.0, 132.0, gfx.WHITE)
gfx.end_frame()
