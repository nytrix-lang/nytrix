;; Keywords: engine overlay profile console info os ui render viewer scene
;; Viewport overlay helpers for profile, console, selection, and status information.
;; References:
;; - std.os.ui.render.viewer.engine.state
;; - std.os.ui.render.dump
module std.os.ui.render.viewer.engine.overlay(init, prepare_pass, fps_skip_reuse, draw_fps, draw_backdrop, draw_crosshair_pixels, draw_crosshair)
use std.core
use std.os.ui.render as gfx
use std.os.ui.render.matrix as rmat
use std.os.ui.render.vk as vkr

mut _cached_fps = -1
mut _cached_fps_str = ""
mut _refresh_frames = 0
mut _color_diag_bg = 0
mut _color_diag_bar = 0
mut _color_fps_g = 0
mut _color_fps_w = 0
mut _color_fps_b = 0
mut _color_cross_c = 0
mut _color_cross_o = 0

fn init() bool {
   "Initializes cached overlay colors and FPS text state."
   _cached_fps = -1
   _cached_fps_str = ""
   _refresh_frames = 0
   _color_diag_bg = gfx.color_pack(0.0, 0.0, 0.0, 1.0)
   _color_diag_bar = gfx.color_pack(0.64, 0.86, 0.84, 0.48)
   _color_fps_g = gfx.color_pack(0.1, 1.0, 0.4, 0.9)
   _color_fps_w = gfx.color_pack(1.0, 0.7, 0.0, 0.9)
   _color_fps_b = gfx.color_pack(1.0, 0.2, 0.1, 1.0)
   _color_cross_c = gfx.color_pack(1.0, 1.0, 1.0, 0.7)
   _color_cross_o = gfx.color_pack(1.0, 1.0, 1.0, 0.4)
   true
}

fn prepare_pass(f64 win_w, f64 win_h, any model_matrix) bool {
   "Prepares renderer state for 2D overlay drawing."
   gfx.set_ortho_2d(0, win_w, win_h, 0)
   vkr.use_custom_push_constants(false)
   vkr.bind_pipeline(0)
   gfx.set_unlit(true)
   gfx.set_model_matrix(model_matrix)
   vkr.set_material_packed(0xffffffff, 0, 0, -1, 0, -1, 0)
   vkr.set_mask(0)
   vkr.reset_scissor_rect()
   true
}

fn fps_skip_reuse(bool reuse_color, int fps_value) bool {
   "Returns whether cached FPS text/color can be reused."
   reuse_color && fps_value == _cached_fps && _refresh_frames <= 0
}

fn draw_fps(int font, int fps_value, bool reuse_color=false) bool {
   "Draws the compact FPS badge."
   if(!font){ return false }
   def fv = int(fps_value)
   if(fv != _cached_fps){
      _cached_fps = fv
      _cached_fps_str = f"FPS {fv:04}"
      _refresh_frames = 8
   }
   if(reuse_color && _refresh_frames <= 0){ return true }
   def col = (fv >= 100) ? _color_fps_g : ((fv >= 50) ? _color_fps_w : _color_fps_b)
   vkr.draw_rect_fast(3.0, 3.0, 112.0, 21.0, _color_diag_bg)
   vkr.draw_rect_fast(3.0, 3.0, 2.0, 21.0, _color_diag_bar)
   gfx.draw_text(font, _cached_fps_str, 5.0, 5.0, col)
   if(reuse_color && _refresh_frames > 0){ _refresh_frames -= 1 }
   true
}

fn draw_backdrop(f64 ww, f64 wh, f64 alpha=0.86) bool {
   "Draws a full-window translucent backdrop."
   gfx.draw_rect_fast(0.0, 0.0, float(ww), float(wh), gfx.color_pack(0.0, 0.0, 0.0, alpha))
   true
}

fn draw_crosshair_pixels(f64 ww, f64 wh) bool {
   "Draws the pixel fallback crosshair."
   mut ix = int(float(ww) * 0.5)
   mut iy = int(float(wh) * 0.5)
   ix -= 1
   iy -= 1
   gfx.draw_rect_fast(ix, iy, 2, 2, _color_cross_c)
   gfx.draw_rect_fast(ix, iy - 8, 2, 4, _color_cross_o)
   gfx.draw_rect_fast(ix, iy + 6, 2, 4, _color_cross_o)
   gfx.draw_rect_fast(ix - 8, iy, 4, 2, _color_cross_o)
   gfx.draw_rect_fast(ix + 6, iy, 4, 2, _color_cross_o)
   true
}

fn draw_crosshair(f64 ww, f64 wh, any mesh=0, any model_matrix=0, f64 last_x=-9e9, f64 last_y=-9e9) list {
   "Draws the cached mesh crosshair when available, otherwise draws the pixel fallback. Returns [center_x, center_y]."
   def cx = float(int(ww * 0.5))
   def cy = float(int(wh * 0.5))
   if(mesh != 0 && model_matrix != 0){
      if(cx != last_x || cy != last_y){
         rmat.mat4_translate_into(cx - 1.0, cy - 1.0, -1.0, model_matrix)
      }
      gfx.set_ortho_2d(0, ww, 0, wh)
      gfx.set_unlit(true)
      gfx.set_model_matrix(model_matrix)
      gfx.draw_mesh(mesh, false)
   } else { draw_crosshair_pixels(ww, wh) }
   [cx, cy]
}

#main {
   assert(init(), "overlay init")
   assert(fps_skip_reuse(false, 60) == false, "overlay fps reuse")
   print("✓ viewer overlay self-test passed")
}
