;; Keywords: browser web filesystem asset read portability fixture
;; Packaged browser assets are readable through std.os.file_read without
;; exposing a general host filesystem or write API.
use std.os
use std.os.ui.render as gfx
use std.os.ui.window as window

def win = gfx.init_window(320, 180, "Nytrix browser asset read", 0, true, false, 1)
match file_read("beep.wav") {
   ok(data) -> window.test_report_touch(data.len, data.len, 0)
   err(code) -> window.test_report_touch(0, code, 0)
}
gfx.begin_frame_clear(gfx.BLACK)
gfx.draw_rect(24.0, 24.0, 272.0, 132.0, gfx.WHITE)
gfx.end_frame()
window.set_should_close(win, true)
