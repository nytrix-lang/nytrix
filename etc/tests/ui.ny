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

;; Pre-calculate colors
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
glfw.set_input_mode(glfw_win, 0x00033001, 0x00034003)
mut cursor_locked = true

def CLR_BG     = [0.015, 0.015, 0.03, 1.0]

;; --- Camera ---
mut cam = camera_init([0.0, 5.0, 45.0], 0.0, 0.0)
mut cam_vel = vec3(0.0, 0.0, 0.0)
mut yaw = 0.0
mut pitch = 0.0
mut target_yaw = 0.0
mut target_pitch = 0.0

mut T_mat   = mat4_identity()
mut R_mat   = mat4_identity()
mut world_m = mat4_identity()
mut temp_m  = mat4_identity()

mut last_mx = 0.0
mut last_my = 0.0
mut initialized_mouse = false

;; Pre-bake cube vertex buffer
fn build_cube_buffer(_size, _col){
   def s = float(_size) * 0.5
   def buf = sys_malloc(36 * 24)
   def _v = fn(i, px, py, pz, u, v){ vkr.__vkr_push_vertex(buf + i * 24, px, py, pz, u, v, _col) }
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

mut tea_ptr = 0
mut tea_cnt = 0
if(mesh_mdl){
   tea_ptr = dict_get(mesh_mdl, "ptr")
   tea_cnt = dict_get(mesh_mdl, "count")
}

def GRID_SLICES = 40
def GRID_SPACING = 2.5
def grid_extent = float(GRID_SLICES) * GRID_SPACING
def grid_line_count = (GRID_SLICES * 2 + 1) * 2
def grid_buf = sys_malloc(grid_line_count * 2 * 24)
def minor_c = vkr._pack_color(0.28, 0.32, 0.42, 0.55)
def major_c = vkr._pack_color(0.55, 0.65, 0.85, 1.0)
mut grid_off = 0
mut gi = 0 - GRID_SLICES
while(gi <= GRID_SLICES){
   def d = float(gi) * GRID_SPACING
   mut c = minor_c
   if(gi == 0){ c = major_c }
   vkr.__vkr_push_vertex(grid_buf + grid_off, -grid_extent, 0.0, d, 0, 0, c)
   vkr.__vkr_push_vertex(grid_buf + grid_off + 24, grid_extent, 0.0, d, 0, 0, c)
   grid_off = grid_off + 48
   vkr.__vkr_push_vertex(grid_buf + grid_off, d, 0.0, -grid_extent, 0, 0, c)
   vkr.__vkr_push_vertex(grid_buf + grid_off + 24, d, 0.0, grid_extent, 0, 0, c)
   grid_off = grid_off + 48
   gi = gi + 1
}
def axes_buf = sys_malloc(3 * 2 * 24)
def axes_len = 20.0
def rc = vkr._pack_color(1,0,0,1)
def gc = vkr._pack_color(0,1,0,1)
def bc = vkr._pack_color(0,0,1,1)
vkr.__vkr_push_vertex(axes_buf, 0.0, 0.01, 0.0, 0, 0, rc)
vkr.__vkr_push_vertex(axes_buf + 24, axes_len, 0.01, 0.0, 0, 0, rc)
vkr.__vkr_push_vertex(axes_buf + 48, 0.0, 0.01, 0.0, 0, 0, gc)
vkr.__vkr_push_vertex(axes_buf + 72, 0.0, axes_len, 0.0, 0, 0, gc)
vkr.__vkr_push_vertex(axes_buf + 96, 0.0, 0.01, 0.0, 0, 0, bc)
vkr.__vkr_push_vertex(axes_buf + 120, 0.0, 0.01, axes_len, 0, 0, bc)

def start_ticks = ticks()
mut last_ticks = start_ticks
mut frame_num = 0
mut fps_val = 0
mut fps_last_report = start_ticks

while(!window_should_close()){
   if(frame_num == 0){ print("DEBUG: Renderer loop started.") }
   begin_frame()
   clear_background(CLR_BG)

   def now_t = ticks()
   mut clamped_dt = float(now_t - last_ticks) / 1000000000.0
   if(clamped_dt > 0.1){ clamped_dt = 0.016 }
   last_ticks = now_t
   def anim_phase = float(now_t - start_ticks) / 1000000000.0

   def ws = window_size(win)
   def ww = float(get(ws, 0))
   def wh = float(get(ws, 1))

   ;; Mouse Look
   if(cursor_locked){
      def mpos = glfw.get_cursor_pos(glfw_win)
      def mx = float(get(mpos, 0))
      def my = float(get(mpos, 1))
      def cx = ww * 0.5
      def cy = wh * 0.5
      glfw.set_cursor_pos(glfw_win, cx, cy)
      
      def sens = 0.12
      target_yaw = target_yaw + (mx - cx) * sens
      target_pitch = target_pitch - (my - cy) * sens
      if(target_pitch > 89.0){ target_pitch = 89.0 }
      if(target_pitch < -89.0){ target_pitch = -89.0 }
   }
   
   def look_smooth = 1.0 - 1.0 / (1.0 + 40.0 * clamped_dt)
   yaw = yaw + (target_yaw - yaw) * look_smooth
   pitch = pitch + (target_pitch - pitch) * look_smooth
   set_idx(cam, 4, yaw)
   set_idx(cam, 5, pitch)
   cam = camera_update(cam)

   ;; Camera Vectors (for movement)
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   ;; Forward vector: yaw=0 => [0,0,-1]
   def fwd = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))
   def rgt = normalize(cross3(fwd, [0.0, 1.0, 0.0]))

   ;; Keyboard Movement
   mut accel_v = 800.0
   if(window_key_down(win, 16)){ accel_v = 2400.0 } ;; SHIFT
   mut want_move = vec3(0.0, 0.0, 0.0)
   if(window_key_down(win, 87)){ want_move = add(want_move, fwd) } ;; W
   if(window_key_down(win, 83)){ want_move = sub(want_move, fwd) } ;; S
   if(window_key_down(win, 65)){ want_move = sub(want_move, rgt) } ;; A
   if(window_key_down(win, 68)){ want_move = add(want_move, rgt) } ;; D
   if(window_key_down(win, 69)){ want_move = add(want_move, vec3(0.0, 1.0, 0.0)) } ;; E
   if(window_key_down(win, 81)){ want_move = sub(want_move, vec3(0.0, 1.0, 0.0)) } ;; Q

   if(len2(want_move) > 0.0){ want_move = normalize(want_move) }

   cam_vel = add(cam_vel, scale(want_move, accel_v * clamped_dt))
   def drag = 1.0 / (1.0 + 15.0 * clamped_dt)
   cam_vel = scale(cam_vel, drag)
   
   def c_pos = get(cam, 0)
   set_idx(cam, 0, add(c_pos, scale(cam_vel, clamped_dt)))

   if(window_key_pressed(win, 9)){ ;; TAB
      cursor_locked = !cursor_locked
      if(cursor_locked){
         glfw.set_input_mode(glfw_win, 0x00033001, 0x00034003)
      } else {
         glfw.set_input_mode(glfw_win, 0x00033001, 0x00034001)
      }
   }

   ;; --- 3D Rendering ---
   vkr.clear_depth()
   begin_mode_3d(cam)
   
   vkr.draw_lines_raw(grid_buf, grid_line_count, 1.0)
   vkr.draw_lines_raw(axes_buf, 3, 3.0)

   def base_mvp = vkr._mvp_matrix()
   
   ;; Spinning Cube
   mat4_rotate_x_into(float(anim_phase), R_mat)
   mat4_mul_into(base_mvp, R_mat, temp_m)
   vkr.set_mvp(temp_m)
   vkr.draw_vertices(cube_ptr, cube_cnt, tex_id)

   ;; Golden Teapot
   if(tea_ptr){
      mat4_rotate_y_into(float(anim_phase) * 1.25, R_mat)
      mat4_translate_into(20.0, -5.0, 0.0, T_mat)
      mat4_mul_into(T_mat, R_mat, world_m)
      mat4_mul_into(base_mvp, world_m, temp_m)
      vkr.set_mvp(temp_m)
      vkr.draw_vertices(tea_ptr, tea_cnt, -1)
   }

   ;; --- UI Overlay ---
   vkr.clear_depth()
   vkr.set_ortho(0.0, ww, 0.0, wh, -1.0, 1.0)
   vkr.draw_rectangle_fast(0.0, 0.0, ww, 24.0, PK_PANEL)
   draw_text_fast(font_id, " NYTRIX ENGINE | 3D RENDERER ", 10.0, 6.0, PK_CYAN)
   
   vkr.draw_rectangle_fast(0.0, 24.0, 260.0, 110.0, PK_PANEL)
   mut fpk = PK_YELLOW
   if(fps_val > 100){ fpk = PK_GREEN } elif(fps_val < 30) { fpk = PK_RED }
   draw_text_fast(font_id, f"FPS: {fps_val}", 10.0, 32.0, fpk)
   
   def final_pos = get(cam, 0)
   draw_text_fast(font_id, f"POS: {int(get(final_pos,0))} {int(get(final_pos,1))} {int(get(final_pos,2))}", 10.0, 50.0, PK_GRAY)
   draw_text_fast(font_id, f"ROT: {int(yaw)} YAW | {int(pitch)} PCH", 10.0, 68.0, PK_GRAY)
   
   if(cursor_locked){
      draw_text_fast(font_id, "MODE: FREECAM (TAB to free)", 10.0, 86.0, PK_CYAN)
   } else {
      draw_text_fast(font_id, "MODE: UI (TAB to lock)", 10.0, 86.0, PK_GRAY)
   }

   ;; Crosshair
   def cx = ww * 0.5
   def cy = wh * 0.5
   vkr.draw_line(cx - 10.0, cy, cx + 10.0, cy, 1.5, 0, 1, 1, 0.6)
   vkr.draw_line(cx, cy - 10.0, cx, cy + 10.0, 1.5, 0, 1, 1, 0.6)

   frame_num = frame_num + 1
   if(now_t - fps_last_report >= 1000000000){
      fps_val = frame_num
      frame_num = 0
      fps_last_report = now_t
   }

   if(window_key_down(win, 27)){ break } ;; ESC
   end_frame()
   poll_events()
}

glfw.set_input_mode(glfw_win, 0x00033001, 0x00034001)
exit(0)
