;; Keywords: browser web filesystem persistence portability fixture
;; Browser file writes use the scoped virtual filesystem and survive a runner
;; reload through localStorage. The test removes its entry before finishing.
use std.os as os
use std.os.ui.render as gfx
use std.os.ui.window as window

def path = "nytrix-web-persistence.txt"
mut written = 0
match os.file_write(path, "browser persistence") {
   ok(n) -> { written = n }
   err(_) -> { written = 0 }
}

mut content = ""
match os.file_read(path) {
   ok(value) -> { content = value }
   err(_) -> { content = "" }
}

def present = os.file_exists(path)
window.test_report_touch(content.len, written, present ? content.len : 0)
match os.file_remove(path) {
   ok(_) -> { }
   err(_) -> { }
}

def win = gfx.init_window(320, 180, "Nytrix browser persistence", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
gfx.draw_rect(24.0, 24.0, 272.0, 132.0, gfx.WHITE)
gfx.end_frame()
