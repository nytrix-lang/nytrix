#!/bin/ny
;; Nytrix Engine - 3D renderer sample

use std.core *
use std.os *
use std.math *
use std.ui.gfx *
use std.ui.window *
use std.ui.input *
use std.math.matrix *
use std.math.vector *

;; Configuration: MSAA x8 for edge coverage
renderer_config(false, 0, "", "", 8)
if(!init_window(1280, 720, "Nytrix Engine")){ exit(1) }

;; Pre-calculate colors (Refined API)
def PK_WHITE  = color_pack(1, 1, 1, 1)
def PK_GREEN  = color_pack(0.2, 1.0, 0.2, 1.0)
def PK_YELLOW = color_pack(1.0, 0.9, 0.1, 1.0)
def PK_RED    = color_pack(1.0, 0.2, 0.2, 1.0)
def PK_CYAN   = color_pack(0.0, 0.9, 1.0, 1.0)
def PK_GRAY   = color_pack(0.5, 0.5, 0.55, 1.0)
def PK_PANEL  = color_pack(0.05, 0.05, 0.08, 0.95)

def font_path = "etc/assets/font/monocraft.ttf"
mut font_id = 0
if(file_exists(font_path)){ font_id = font_load(font_path, 16) }

def tex_id   = texture_load("etc/assets/images/test.png")
def mesh_mdl = mesh_load("etc/assets/models/teapot.obj", [1.0, 0.84, 0.0, 1.0])

def win      = get_active_window()

;; Always lock cursor for freecam mode (Refined API)
window_set_cursor_mode(win, CURSOR_LOCKED)

def CLR_BG     = [0.008, 0.008, 0.012, 1.0]

;; --- Camera ---
mut cam = camera_init([0.0, 5.0, 45.0], 0.0, 0.0)
mut cam_vel = vec3(0.0, 0.0, 0.0)
mut yaw = 0.0
mut pitch = 0.0
mut target_yaw = 0.0
mut target_pitch = 0.0

mut T_mat   = mat4_identity()
mut R_mat   = mat4_identity()
mut S_mat   = mat4_identity()
mut world_m = mat4_identity()

def stride = VERTEX_STRIDE

;; Pre-bake cube vertex buffer with normals
fn build_cube_buffer(_size, _col){
   def s = float(_size) * 0.5
   def buf = sys_malloc(36 * stride)
   def _v = fn(i, px, py, pz, u, v, nx, ny, nz){ 
      push_vertex(buf + i * stride, px, py, pz, u, v, _col, nx, ny, nz) 
   }
   ;; +Z
   _v(0,-s,-s, s, 0,0, 0,0,1) _v(1, s,-s, s, 1,0, 0,0,1) _v(2, s, s, s, 1,1, 0,0,1)
   _v(3,-s,-s, s, 0,0, 0,0,1) _v(4, s, s, s, 1,1, 0,0,1) _v(5,-s, s, s, 0,1, 0,0,1)
   ;; -Z
   _v(6, s,-s,-s, 0,0, 0,0,-1) _v(7,-s,-s,-s, 1,0, 0,0,-1) _v(8,-s, s,-s, 1,1, 0,0,-1)
   _v(9, s,-s,-s, 0,0, 0,0,-1) _v(10,-s, s,-s, 1,1, 0,0,-1) _v(11, s, s,-s, 0,1, 0,0,-1)
   ;; +Y
   _v(12,-s, s,-s, 0,0, 0,1,0) _v(13,-s, s, s, 0,1, 0,1,0) _v(14, s, s, s, 1,1, 0,1,0)
   _v(15,-s, s,-s, 0,0, 0,1,0) _v(16, s, s, s, 1,1, 0,1,0) _v(17, s, s,-s, 1,0, 0,1,0)
   ;; -Y
   _v(18,-s,-s, s, 0,0, 0,-1,0) _v(19, s,-s, s, 1,0, 0,-1,0) _v(20, s,-s,-s, 1,1, 0,-1,0)
   _v(21,-s,-s, s, 0,0, 0,-1,0) _v(22, s,-s,-s, 1,1, 0,-1,0) _v(23,-s,-s,-s, 0,1, 0,-1,0)
   ;; +X
   _v(24, s,-s,-s, 0,0, 1,0,0) _v(25, s, s,-s, 0,1, 1,0,0) _v(26, s, s, s, 1,1, 1,0,0)
   _v(27, s,-s,-s, 0,0, 1,0,0) _v(28, s, s, s, 1,1, 1,0,0) _v(29, s,-s, s, 1,0, 1,0,0)
   ;; -X
   _v(30,-s,-s, s, 0,0, -1,0,0) _v(31,-s,-s,-s, 1,0, -1,0,0) _v(32,-s, s,-s, 1,1, -1,0,0)
   _v(33,-s,-s, s, 0,0, -1,0,0) _v(34,-s, s,-s, 1,1, -1,0,0) _v(35,-s, s, s, 0,1, -1,0,0)
   [buf, 36]
}
def cube_vbuf = build_cube_buffer(6.0, PK_WHITE)
def cube_ptr  = get(cube_vbuf, 0)
def cube_cnt  = get(cube_vbuf, 1)

mut tea_ptr = 0
mut tea_cnt = 0
if(mesh_mdl){
   tea_ptr = dict_get(mesh_mdl, "ptr")
   tea_cnt = dict_get(mesh_mdl, "count")
}

;; Pre-bake grid and gizmo buffers
def GRID_SLICES = 40
def GRID_SPACING = 2.5
def grid_line_count = (GRID_SLICES * 2 + 1) * 2
def grid_buf = sys_malloc(grid_line_count * 2 * stride)
def gs = float(GRID_SLICES) * GRID_SPACING
mut grid_off = 0
mut gi = 0 - GRID_SLICES
while(gi <= GRID_SLICES){
    def d = float(gi) * GRID_SPACING
    def c = color_pack(0.3, 0.3, 0.4, 1)
    push_vertex(grid_buf + grid_off, -gs, 0, d, 0,0, c)
    grid_off += stride
    push_vertex(grid_buf + grid_off, gs, 0, d, 0,0, c)
    grid_off += stride
    push_vertex(grid_buf + grid_off, d, 0, -gs, 0,0, c)
    grid_off += stride
    push_vertex(grid_buf + grid_off, d, 0, gs, 0,0, c)
    grid_off += stride
    gi += 1
}

def axes_buf = sys_malloc(6 * stride)
push_vertex(axes_buf, 0,0,0, 0,0, color_pack(1,0,0,1))
push_vertex(axes_buf + stride, 20,0,0, 0,0, color_pack(1,0,0,1))
push_vertex(axes_buf + 2*stride, 0,0,0, 0,0, color_pack(0,1,0,1))
push_vertex(axes_buf + 3*stride, 0,20,0, 0,0, color_pack(0,1,0,1))
push_vertex(axes_buf + 4*stride, 0,0,0, 0,0, color_pack(0,0,1,1))
push_vertex(axes_buf + 5*stride, 0,0,20, 0,0, color_pack(0,0,1,1))


def start_ticks = ticks()
mut last_ticks = start_ticks
mut frame_num = 0
mut fps_val = 0
mut fps_last_report = start_ticks

while(!window_should_close(win)){
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
   def mpos = window_get_cursor_pos(win)
   def mx = float(get(mpos, 0))
   def my = float(get(mpos, 1))
   def cx = ww * 0.5
   def cy = wh * 0.5
   
   window_set_cursor_pos(win, float(cx), float(cy))
   
   def dx = mx - cx
   def dy = my - cy
   if(abs(dx) > 0.0001 || abs(dy) > 0.0001){
      def sens = 0.12
      target_yaw = target_yaw + dx * sens
      target_pitch = target_pitch - dy * sens
      target_pitch = clamp(target_pitch, -89.0, 89.0)
   }
   
   def look_smooth = 1.0 - 1.0 / (1.0 + 40.0 * clamped_dt)
   yaw = yaw + (target_yaw - yaw) * look_smooth
   pitch = pitch + (target_pitch - pitch) * look_smooth
   set_idx(cam, 4, yaw)
   set_idx(cam, 5, pitch)

   ;; Camera Vectors (for movement)
   def ryaw = yaw * PI / 180.0
   def fwd_h = vec3(sin(ryaw), 0.0, -cos(ryaw))
   def rgt_h = vec3(cos(ryaw), 0.0, sin(ryaw))

   ;; Keyboard Movement
   mut accel_v = 800.0
   if(window_key_down(win, 16)){ accel_v = 2400.0 } ;; SHIFT
   mut want_move = vec3(0.0, 0.0, 0.0)
   if(window_key_down(win, 87)){ want_move = add(want_move, fwd_h) } ;; W
   if(window_key_down(win, 83)){ want_move = sub(want_move, fwd_h) } ;; S
   if(window_key_down(win, 65)){ want_move = sub(want_move, rgt_h) } ;; A
   if(window_key_down(win, 68)){ want_move = add(want_move, rgt_h) } ;; D
   if(window_key_down(win, 69)){ want_move = add(want_move, [0.0, 1.0, 0.0]) } ;; E
   if(window_key_down(win, 81)){ want_move = sub(want_move, [0.0, 1.0, 0.0]) } ;; Q

   if(len2(want_move) > 0.0){ want_move = normalize(want_move) }

   cam_vel = add(cam_vel, scale(want_move, accel_v * clamped_dt))
   def drag = 1.0 / (1.0 + 15.0 * clamped_dt)
   cam_vel = scale(cam_vel, drag)
   
   def c_pos = get(cam, 0)
   set_idx(cam, 0, add(c_pos, scale(cam_vel, clamped_dt)))
   cam = camera_update(cam)

   ;; --- 3D Rendering ---
   clear_depth()
   begin_mode_3d(cam)
   set_unlit(true)
   set_model_matrix(mat4_identity())
   
   draw_lines_raw(grid_buf, grid_line_count, 0.015)
   draw_lines_raw(axes_buf, 3, 0.08)

   ;; Spinning Cube (UNLIT)
   set_unlit(true)
   mat4_rotate_x_into(float(anim_phase), R_mat)
   set_model_matrix(R_mat)
   draw_vertices(cube_ptr, cube_cnt, tex_id)

   ;; Golden Teapot (LIT)
   if(tea_ptr){
      set_unlit(false)
      mat4_rotate_y_into(float(anim_phase) * 1.25, R_mat)
      mat4_translate_into(0.0, 5.0, 0.0, T_mat)
      mat4_scale_into(5.0, 5.0, 5.0, S_mat)
      mat4_mul_into(T_mat, S_mat, world_m)
      mat4_mul_into(world_m, R_mat, T_mat) ;; Re-use T_mat for result
      set_model_matrix(T_mat)
      draw_vertices(tea_ptr, tea_cnt, -1)
   }

   ;; --- UI Overlay ---
   clear_depth()
   set_ortho(0.0, ww, 0.0, wh, -1.0, 1.0)
   set_unlit(true)
   set_model_matrix(mat4_identity())
   
   def title_text = " NYTRIX ENGINE | 3D RENDERER "
   def fps_text   = f"FPS: {fps_val}"
   def pos_text   = f"POS: {int(get(c_pos,0))} {int(get(c_pos,1))} {int(get(c_pos,2))}"
   def rot_text   = f"ROT: {int(yaw)} YAW | {int(pitch)} PCH"
   def help_text  = "WASD/EQ: Move | ESC: Quit"
   
   def sz_title = measure_text(font_id, title_text)
   def sz_fps   = measure_text(font_id, fps_text)
   def sz_pos   = measure_text(font_id, pos_text)
   def sz_rot   = measure_text(font_id, rot_text)
   def sz_help  = measure_text(font_id, help_text)
   
   mut hud_w = get(sz_title, 0)
   if(get(sz_fps, 0) > hud_w){ hud_w = get(sz_fps, 0) }
   if(get(sz_pos, 0) > hud_w){ hud_w = get(sz_pos, 0) }
   if(get(sz_rot, 0) > hud_w){ hud_w = get(sz_rot, 0) }
   if(get(sz_help, 0) > hud_w){ hud_w = get(sz_help, 0) }
   hud_w = hud_w + 20.0
   
   draw_rectangle_fast(0.0, 0.0, ww, 24.0, PK_PANEL)
   draw_text_fast(font_id, title_text, 10.0, 6.0, PK_CYAN)
   
   draw_rectangle_fast(0.0, 24.0, hud_w, 110.0, PK_PANEL)
   mut fpk = YELLOW
   if(fps_val > 100){ fpk = GREEN } elif(fps_val < 30) { fpk = RED }
   draw_text_fast(font_id, fps_text, 10.0, 32.0, color_pack(get(fpk,0), get(fpk,1), get(fpk,2)))
   draw_text_fast(font_id, pos_text, 10.0, 50.0, PK_GRAY)
   draw_text_fast(font_id, rot_text, 10.0, 68.0, PK_GRAY)
   draw_text_fast(font_id, help_text, 10.0, 86.0, color_pack(1,1,1,1))

   ;; Crosshair
   def ccx = ww * 0.5
   def ccy = wh * 0.5
   draw_line_2d(ccx - 10.0, ccy, ccx + 10.0, ccy, WHITE, 1.0)
   draw_line_2d(ccx, ccy - 10.0, ccx, ccy + 10.0, WHITE, 1.0)

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

window_set_cursor_mode(win, CURSOR_NORMAL)
exit(0)
