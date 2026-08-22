;; Keywords: zzprobe
use std.os.ui.render as gfx

def win = gfx.init_window(64, 64, "t", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
def x = 2

if x == 2 { print("PLAIN_TRUE") } else { print("PLAIN_FALSE") }
def loc = [2, 2]

if loc.get(0) == 2 { print("LOC_TRUE") } else { print("LOC_FALSE") }
def g0 = loc.get(0)

if g0 == 2 { print("CAP_TRUE") } else { print("CAP_FALSE") }
def sz = gfx.texture_size(gfx.texture_create_rgba(2, 2, [0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255], 37, 1, 10497, 10497, false))

if sz.get(0) == 2 { print("HOST_TRUE") } else { print("HOST_FALSE") }
gfx.end_frame()
