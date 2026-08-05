;; Keywords: browser webgl texture probe portability fixture
;; Texture probe: the browser renderer must create a 2x2 RGBA texture from
;; Ny-side pixel data, report its live size through the public facade, draw it,
;; and read a pixel back through the host. Loop-free on purpose: a `while`
;; loop before the texture calls is DCE'd by the wasm backend.
use std.os.ui.render as gfx
def win = gfx.init_window(320, 180, "Nytrix browser texture", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
def pix = [255, 0, 0, 255,  255, 0, 0, 255,  0, 0, 0, 255,  0, 0, 0, 255]
def tex = gfx.texture_create_rgba(2, 2, pix, 37, 1, 10497, 10497, false)
def sz = gfx.texture_size(tex)
print(int(sz.get(0)), int(sz.get(1)))
if int(sz.get(0)) == 2 && int(sz.get(1)) == 2 {
   gfx.draw_texture(tex, 0.0, 0.0, 1.0, gfx.WHITE)
   def tl = gfx.get_pixel(0, 0)
   print(int(tl.get(0) * 255), int(tl.get(1) * 255), int(tl.get(2) * 255))
   def br = gfx.get_pixel(1, 1)
   print(int(br.get(0) * 255), int(br.get(1) * 255), int(br.get(2) * 255))
   gfx.end_frame()
}
gfx.end_frame()
