;; Keywords: browser webgl framebuffer probe portability fixture
;; Framebuffer probe: the browser renderer must report the live backbuffer
;; dimensions through the public facade, not an unhosted stub.
use std.os.ui.render as gfx
def win = gfx.init_window(320, 180, "Nytrix browser framebuffer", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
def sz = gfx.get_framebuffer_size()
print(int(sz.get(0)), int(sz.get(1)))
gfx.end_frame()
