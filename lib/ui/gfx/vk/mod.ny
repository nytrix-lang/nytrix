module std.ui.gfx.vk (
   init, shutdown,
   begin_frame, end_frame,
   clear, draw_rect, draw_rect_tex, draw_rect_tex_uv, draw_line,
   draw_triangle, draw_triangle_3d,
   create_texture, update_texture_rect,
   set_mvp, bind_texture, _update_default_mvp, snapshot
)

use std.ui.gfx.vk.state as vk_state
use std.ui.gfx.vk.setup as vk_setup
use std.ui.gfx.vk.pipeline as vk_pipeline
use std.ui.gfx.vk.buffers as vk_buffers
use std.ui.gfx.vk.texture as vk_texture
use std.ui.gfx.vk.draw as vk_draw
use std.ui.gfx.vk.utils as vk_utils

fn init(win){ vk_setup.init(win) }
fn shutdown(){ vk_setup.shutdown() }
fn begin_frame(){ vk_draw.begin_frame() }
fn end_frame(){ vk_draw.end_frame() }
fn clear(r, g=none, b=none, a=none){
   if(is_list(r)) {
      vk_draw.clear(get(r, 0), get(r, 1), get(r, 2), get(r, 3))
   } else {
      vk_draw.clear(r, g, b, a)
   }
}
fn draw_rect(x, y, w, h, r, g, b, a){ vk_draw.draw_rect(x, y, w, h, r, g, b, a) }
fn draw_rect_tex(x, y, w, h, id, r, g, b, a){ vk_draw.draw_rect_tex(x, y, w, h, id, r, g, b, a) }
fn draw_rect_tex_uv(x, y, w, h, u1, v1, u2, v2, id, r, g, b, a){ vk_draw.draw_rect_tex_uv(x, y, w, h, u1, v1, u2, v2, id, r, g, b, a) }
fn draw_triangle(x1, y1, x2, y2, x3, y3, r, g, b, a){ vk_draw.draw_triangle(x1, y1, x2, y2, x3, y3, r, g, b, a) }
fn draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a){ vk_draw.draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a) }
fn draw_line(x1, y1, x2, y2, thickness, r, g, b, a){ vk_draw.draw_line(x1, y1, x2, y2, thickness, r, g, b, a) }
fn bind_texture(id){ vk_draw.bind_texture(id) }
fn create_texture(w, h, pix){ vk_texture.create_texture(w, h, pix) }
fn update_texture_rect(id, x, y, w, h, pix){ vk_texture.update_texture_rect(id, x, y, w, h, pix) }
fn set_mvp(m){ vk_utils.set_mvp(m) }
fn _update_default_mvp(w){ vk_utils._update_default_mvp(w) }
fn snapshot(f){ vk_draw.snapshot(f) }
