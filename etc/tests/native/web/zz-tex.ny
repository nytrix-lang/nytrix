;; Keywords: zz
use std.os.ui.render as gfx
def win = gfx.init_window(64, 64, "t", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
mut pix = list()
mut i = 0
while i < 64 { pix = pix.append(255) if i % 4 != 0 pix = pix.append(128) if i % 4 == 1 pix = pix.append(0) if i % 4 == 2 i += 1 }
def tex = gfx.texture_create_rgba(4, 4, pix, 37, 1, 10497, 10497, false)
def sz = gfx.texture_size(tex)
print(tex, sz != nil)
def c = gfx.get_pixel(2, 2)
print(c == nil)
gfx.end_frame()
