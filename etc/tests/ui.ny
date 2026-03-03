#!/bin/ny

use std.core *
use std.os *
use std.math *
use std.ui.gfx *
use std.ui.gfx.vk_renderer as vkr
use std.ui.window *
use std.ui.input *
use std.ui.glfw as glfw
use std.math.matrix *
use std.math.vector *

renderer_config(false, 0, "", "", 1)
if(!init_window(1280, 720, "Nytrix UI")){ exit(1) }

;; Pre-calculate packed colors for fixed-layout submission
def PK_WHITE  = vkr._pack_color(1, 1, 1, 1)
def PK_GREEN  = vkr._pack_color(0.2, 1.0, 0.2, 1.0)
def PK_YELLOW = vkr._pack_color(1.0, 0.9, 0.1, 1.0)
def PK_RED    = vkr._pack_color(1.0, 0.2, 0.2, 1.0)
def PK_CYAN   = vkr._pack_color(0.0, 0.9, 1.0, 1.0)
def PK_GRAY   = vkr._pack_color(0.5, 0.5, 0.55, 1.0)
def PK_PANEL  = vkr._pack_color(0.05, 0.05, 0.08, 0.92)

def font_path = "etc/assets/font/monocraft.ttf"
mut font_id = 0
if(file_exists(font_path)){ font_id = font_load(font_path, 16) }

def tex_id   = texture_load("etc/assets/images/test.png")
def mesh_mdl = mesh_load("etc/assets/models/teapot.obj", [1.0, 0.84, 0.0, 1.0])

def win      = get_active_window()
def glfw_win = dict_get(win, "handle", 0)
glfw._call("glfwSetInputMode", [glfw_win, 0x00033001, 0x00034003])
mut cursor_locked = true

def CLR_BG     = [0.015, 0.015, 0.03, 1.0]
def CLR_PANEL  = [0.05, 0.05, 0.08, 0.92]
def CLR_GRAY   = [0.5, 0.5, 0.55, 1.0]

def K_W = load8("W", 0)
def K_A = load8("A", 0)
def K_S = load8("S", 0)
def K_D = load8("D", 0)
def K_E = load8("E", 0)
def K_Q = load8("Q", 0)
def K_SHIFT = 16

mut pos_x = 0.0   mut pos_y = 12.0  mut pos_z = 35.0
mut vel_x = 0.0   mut vel_y = 0.0   mut vel_z = 0.0
mut yaw   = 3.14  mut pitch = -0.2
mut target_yaw = 3.14  mut target_pitch = -0.2

mut eye_arr  = [0.0, 0.0, 0.0]
mut look_arr = [0.0, 0.0, 0.0]
def UP_VEC   = [0.0, 1.0, 0.0]
def X_AXIS   = [1.0, 0.0, 0.0]
def Y_AXIS   = [0.0, 1.0, 0.0]

mut proj_m  = mat4_identity()
mut view_m  = mat4_identity()
mut world_m = mat4_identity()
mut T_mat   = mat4_identity()
mut R_mat   = mat4_identity()
mut vp_m    = mat4_identity()   ;; Pre-allocated, reused every frame
mut temp_m  = mat4_identity()   ;; Pre-allocated scratch

mut last_win_w = 0.0
mut last_win_h = 0.0
mut ww = 1280.0
mut wh = 720.0
mut last_mx = 0.0
mut last_my = 0.0
mut initialized_mouse = false

;; Pre-cache color arrays to avoid per-frame allocation
def GREEN  = [0.2, 1.0, 0.2, 1.0]
def YELLOW = [1.0, 0.9, 0.1, 1.0]
def RED    = [1.0, 0.2, 0.2, 1.0]
def CYAN   = [0.0, 0.9, 1.0, 1.0]

;; Pre-bake cube vertex buffer
fn build_cube_buffer(_size, _col){
   def size = _size
   def col = _col
   def s = float(size) * 0.5
   def buf = sys_malloc(36 * 24)
   def _v = fn(i, px, py, pz, u, v){
      vkr.__vkr_push_vertex(buf + i * 24, px, py, pz, u, v, col)
   }
   _v(0,-s,-s,s,0,0) _v(1,s,-s,s,1,0) _v(2,s,s,s,1,1) _v(3,-s,-s,s,0,0) _v(4,s,s,s,1,1) _v(5,-s,s,s,0,1)
   _v(6,s,-s,-s,0,0) _v(7,-s,-s,-s,1,0) _v(8,-s,s,-s,1,1) _v(9,s,-s,-s,0,0) _v(10,-s,s,-s,1,1) _v(11,s,s,-s,0,1)
   _v(12,-s,s,-s,0,0) _v(13,-s,s,s,0,1) _v(14,s,s,s,1,1) _v(15,-s,s,-s,0,0) _v(16,s,s,s,1,1) _v(17,s,s,-s,1,0)
   _v(18,-s,-s,s,0,0) _v(19,s,-s,s,1,0) _v(20,s,-s,-s,1,1) _v(21,-s,-s,s,0,0) _v(22,s,-s,-s,1,1) _v(23,-s,-s,-s,0,1)
   _v(24,s,-s,-s,0,0) _v(25,s,s,-s,0,1) _v(26,s,s,s,1,1) _v(27,s,-s,-s,0,0) _v(28,s,s,s,1,1) _v(29,s,-s,s,1,0)
   _v(30,-s,-s,s,0,0) _v(31,-s,-s,-s,1,0) _v(32,-s,s,-s,1,1) _v(33,-s,-s,s,0,0) _v(34,-s,s,-s,1,1) _v(35,-s,s,s,0,1)
   [buf, 36]
}
def cube_vbuf = build_cube_buffer(6.0, vkr._pack_color(1,1,1,1))
def cube_ptr  = get(cube_vbuf, 0)
def cube_cnt  = get(cube_vbuf, 1)

mut tea_ptr = 0 mut tea_cnt = 0
if(mesh_mdl){ tea_ptr = dict_get(mesh_mdl, "ptr") tea_cnt = dict_get(mesh_mdl, "count") }

;; Pre-bake grid vertex buffer — real LINE_LIST pairs (2 verts per line)
def GRID_SLICES = 40
def GRID_SPACING = 2.5
def grid_extent = float(GRID_SLICES) * GRID_SPACING
def grid_line_count = (GRID_SLICES * 2 + 1) * 2   ;; X-lines + Z-lines
def grid_buf = sys_malloc(grid_line_count * 2 * 24) ;; 2 verts * 24 bytes each
def minor_c = vkr._pack_color(0.28, 0.32, 0.42, 0.55)
def major_c = vkr._pack_color(0.55, 0.65, 0.85, 1.0)
mut grid_off = 0
mut gi = 0 - GRID_SLICES
while(gi <= GRID_SLICES){
   def d = float(gi) * GRID_SPACING
   def c = (gi == 0) ? major_c : minor_c
   ;; Line along X at z=d: endpoints (-extent, 0, d) → (+extent, 0, d)
   vkr.__vkr_push_vertex(grid_buf + grid_off,      -grid_extent, 0.0, d, 0, 0, c)
   vkr.__vkr_push_vertex(grid_buf + grid_off + 24,  grid_extent, 0.0, d, 0, 0, c)
   grid_off += 48
   ;; Line along Z at x=d: endpoints (d, 0, -extent) → (d, 0, +extent)
   vkr.__vkr_push_vertex(grid_buf + grid_off,       d, 0.0, -grid_extent, 0, 0, c)
   vkr.__vkr_push_vertex(grid_buf + grid_off + 24, d, 0.0,  grid_extent, 0, 0, c)
   grid_off += 48
   gi += 1
}

;; Pre-bake axes vertex buffer — real LINE_LIST pairs (2 verts per line)
def axes_buf = sys_malloc(3 * 2 * 24)
def axes_len = 20.0
def rc = vkr._pack_color(1,0,0,1)
def gc = vkr._pack_color(0,1,0,1)
def bc = vkr._pack_color(0,0,1,1)

;; X axis
vkr.__vkr_push_vertex(axes_buf,      0.0, 0.01, 0.0, 0, 0, rc)
vkr.__vkr_push_vertex(axes_buf + 24, axes_len, 0.01, 0.0, 0, 0, rc)
;; Y axis
vkr.__vkr_push_vertex(axes_buf + 48, 0.0, 0.01, 0.0, 0, 0, gc)
vkr.__vkr_push_vertex(axes_buf + 72, 0.0, axes_len, 0.0, 0, 0, gc)
;; Z axis
vkr.__vkr_push_vertex(axes_buf + 96, 0.0, 0.01, 0.0, 0, 0, bc)
vkr.__vkr_push_vertex(axes_buf + 120, 0.0, 0.01, axes_len, 0, 0, bc)

def start_ticks = ticks()
mut last_ticks = start_ticks
mut frame_num = 0
mut fps_val = 0
mut fps_last_report = start_ticks

while(!window_should_close()){
   begin_frame()
   clear_background(CLR_BG)

   def now_t = ticks()
   def dt_ns = now_t - last_ticks
   last_ticks = now_t
   mut clamped_dt = float(dt_ns) / 1000000000.0
   if(clamped_dt > 0.1){ clamped_dt = 0.016 }
   def anim_phase = float(now_t - start_ticks) / 1000000000.0

   ;; Window size (only call FFI if size might have changed)
   def ws = window_size(win)
   ww = float(get(ws, 0))
   wh = float(get(ws, 1))

   ;; Mouse delta via raw GLFW (single FFI call returning a list)
   def mpos = glfw.get_cursor_pos(glfw_win)
   def mx = float(get(mpos, 0))
   def my = float(get(mpos, 1))
   mut dmx = 0.0 mut dmy = 0.0
   if(initialized_mouse){ dmx = mx - last_mx dmy = my - last_my }
   last_mx = mx last_my = my initialized_mouse = true

   if(cursor_locked){
      def sens = 0.0016
      target_yaw   += dmx * sens
      target_pitch -= dmy * sens
      if(target_pitch > 1.48){ target_pitch = 1.48 }
      if(target_pitch < -1.48){ target_pitch = -1.48 }
   }

   ;; Frame-rate independent smoothing
   def look_smooth = 1.0 - 1.0 / (1.0 + 40.0 * clamped_dt)
   yaw   += (target_yaw - yaw) * look_smooth
   pitch += (target_pitch - pitch) * look_smooth

   def cos_p = cos(pitch)
   def fwd_x = cos(yaw) * cos_p
   def fwd_y = sin(pitch)
   def fwd_z = sin(yaw) * cos_p
   def rgt_x = sin(yaw)
   def rgt_z = -cos(yaw)

   mut accel_v = 800.0
   if(window_key_down(win, K_SHIFT)){ accel_v = 2400.0 }
   mut want_x = 0.0 mut want_y = 0.0 mut want_z = 0.0
   if(window_key_down(win, K_W)){ want_x += fwd_x want_y += fwd_y want_z += fwd_z }
   if(window_key_down(win, K_S)){ want_x -= fwd_x want_y -= fwd_y want_z -= fwd_z }
   if(window_key_down(win, K_A)){ want_x += rgt_x want_z += rgt_z }
   if(window_key_down(win, K_D)){ want_x -= rgt_x want_z -= rgt_z }
   if(window_key_down(win, K_E)){ want_y += 1.0 }
   if(window_key_down(win, K_Q)){ want_y -= 1.0 }

   ;; Normalize
   def len_sq = want_x * want_x + want_y * want_y + want_z * want_z
   if(len_sq > 0.0){
       def inv_len = 1.0 / sqrt(len_sq)
       want_x *= inv_len want_y *= inv_len want_z *= inv_len
   }

   vel_x += want_x * accel_v * clamped_dt
   vel_y += want_y * accel_v * clamped_dt
   vel_z += want_z * accel_v * clamped_dt
   def drag = 1.0 / (1.0 + 15.0 * clamped_dt)
   vel_x *= drag vel_y *= drag vel_z *= drag
   pos_x += vel_x * clamped_dt
   pos_y += vel_y * clamped_dt
   pos_z += vel_z * clamped_dt

   if(window_key_pressed(win, KEY_TAB)){
      cursor_locked = !cursor_locked
      if(cursor_locked){
         glfw._call("glfwSetInputMode", [glfw_win, 0x00033001, 0x00034003])
      } else {
         glfw._call("glfwSetInputMode", [glfw_win, 0x00033001, 0x00034001])
      }
   }

   store_item(eye_arr, 0, float(pos_x))
   store_item(eye_arr, 1, float(pos_y))
   store_item(eye_arr, 2, float(pos_z))
   store_item(look_arr, 0, float(pos_x + fwd_x))
   store_item(look_arr, 1, float(pos_y + fwd_y))
   store_item(look_arr, 2, float(pos_z + fwd_z))
   mat4_look_at_into(eye_arr, look_arr, UP_VEC, view_m)

   if(ww != last_win_w || wh != last_win_h){
      mat4_perspective_into(45.0 * PI / 180.0, ww / wh, 0.1, 5000.0, proj_m)
      last_win_w = ww last_win_h = wh
   }

   ;; 3D Rendering
   vkr.clear_depth()
   
   ;; VP Matrix (reuse pre-allocated vp_m)
   mat4_mul_into(proj_m, view_m, vp_m)
   vkr.set_mvp(vp_m)

    ;; Grid + Axes
    vkr.draw_lines_raw(grid_buf, grid_line_count, 1.0)
    vkr.draw_lines_raw(axes_buf, 3, 3.0)

   ;; Spinning Cube
   mat4_rotate_into(float(anim_phase), X_AXIS, R_mat)
   mat4_mul_into(vp_m, R_mat, temp_m) ; cube at origin, T=identity, skip mat_mul
   vkr.set_mvp(temp_m)
   vkr.draw_vertices(cube_ptr, cube_cnt, tex_id)

   ;; Golden Teapot
   if(tea_ptr){
      mat4_rotate_into(float(anim_phase) * 1.25, Y_AXIS, R_mat)
      mat4_translate_into(20.0, -5.0, 0.0, T_mat)
      mat4_mul_into(T_mat, R_mat, world_m)
      mat4_mul_into(vp_m, world_m, temp_m)
      vkr.set_mvp(temp_m)
      vkr.draw_vertices(tea_ptr, tea_cnt, -1)
   }

   ;; UI Overlay
   vkr.clear_depth()
   vkr.set_ortho(0.0, ww, 0.0, wh, -1.0, 1.0)

   ;; Top panel
   vkr.draw_rectangle_fast(0.0, 0.0, ww, 24.0, PK_PANEL)
   draw_text_fast(font_id, " NYTRIX ENGINE | 3D RENDERER ", 10.0, 6.0, PK_CYAN)
   
   ;; Readouts panel
   vkr.draw_rectangle_fast(0.0, 24.0, 260.0, 110.0, PK_PANEL)

   def yaw_deg = int(yaw * 180.0 / PI)
   def pitch_deg = int(pitch * 180.0 / PI)
   
   mut fpk = PK_YELLOW
   if(fps_val > 100){ fpk = PK_GREEN } elif(fps_val < 30) { fpk = PK_RED }
   
   draw_text_fast(font_id, f"FPS: {fps_val}", 10.0, 32.0, fpk)
   draw_text_fast(font_id, f"POS: {int(pos_x)} {int(pos_y)} {int(pos_z)}", 10.0, 50.0, PK_GRAY)
   draw_text_fast(font_id, f"ROT: {yaw_deg} YAW | {pitch_deg} PCH", 10.0, 68.0, PK_GRAY)
   
   if(cursor_locked){
      draw_text_fast(font_id, "MODE: FREECAM (TAB to free)", 10.0, 86.0, PK_CYAN)
   } else {
      draw_text_fast(font_id, "MODE: UI (TAB to lock)", 10.0, 86.0, PK_GRAY)
   }

   ;; Crosshair
   def cx = ww * 0.5 mut cy = wh * 0.5
   vkr.draw_line(cx - 10.0, cy, cx + 10.0, cy, 1.5, 0, 1, 1, 0.6)
   vkr.draw_line(cx, cy - 10.0, cx, cy + 10.0, 1.5, 0, 1, 1, 0.6)

   frame_num += 1
   def now_report = ticks()
   if(now_report - fps_last_report >= 1000000000){
      fps_val = frame_num
      frame_num = 0
      fps_last_report = now_report
      print(f"FPS: {fps_val}")
   }

   if(window_key_down(win, KEY_ESCAPE)){ break }
   end_frame()
   poll_events()
}

glfw._call("glfwSetInputMode", [glfw_win, 0x00033001, 0x00034001])
exit(0)
