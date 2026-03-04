;; Keywords: ui camera 3d navigation

module std.ui.camera (
   Camera, init, update, apply,
   get_yaw, get_pitch, get_pos, get_rot,
   set_fov, set_speed, set_sens
)

use std.core *
use std.math *
use std.math.vector *
use std.math.matrix *
use std.ui.input as input
use std.ui.window as window
use std.util.common as common

def Camera = dict_type("Camera")

;; Flat list layout for camera state (avoid string hash table lookups on hot path)
;; Offsets: 0:pos(v3) 3:vel(v3) 6:yaw 7:pitch 8:target_yaw 9:target_pitch
;;          10:sens 11:accel 12:drag 13:rot_smooth 14:move_smooth 15:boost_mult 16:fov
;;          17:cmd(v3) 20:_win_w 21:_win_h

fn init(pos=[0,0,0], yaw=0.0, pitch=0.0){
   "Initializes a new Camera object."
   mut c = list(24)
   ;; pos (3)
   append(c, get(pos,0)) append(c, get(pos,1)) append(c, get(pos,2))
   ;; vel (3)
   append(c, 0.0) append(c, 0.0) append(c, 0.0)
   ;; yaw pitch target_yaw target_pitch (4)
   append(c, yaw + 0.0) append(c, pitch + 0.0) append(c, yaw + 0.0) append(c, pitch + 0.0)
   ;; sens accel drag rot_smooth move_smooth boost_mult fov (7)
   append(c, 0.08) append(c, 500.0) append(c, 10.0) append(c, 18.0) append(c, 12.0) append(c, 3.0) append(c, 45.0)
   ;; cur_move_dir (3)
   append(c, 0.0) append(c, 0.0) append(c, 0.0)
   ;; cached win size (2)
   append(c, 1280.0) append(c, 720.0)
   c
}

@jit
fn update(cam, dt, win, skip_look=false, skip_move=false){
   "Updates camera state based on input and physics."
   common.touch(cam) common.touch(dt) common.touch(win) common.touch(skip_look) common.touch(skip_move)

   def ww = get(cam, 20) def wh = get(cam, 21)
   def cx = ww * 0.5 def cy = wh * 0.5

   ;; Mouse Look (fast dictionary access to avoid list allocation)
   if(!skip_look){
      def mx = get(win, "mouse_x", cx)
      def my = get(win, "mouse_y", cy)
      window.set_cursor_pos(win, cx, cy)
      def dx = mx - cx
      def dy = my - cy

      def sens = get(cam, 10)
      if(abs(dx) > 0.0001 || abs(dy) > 0.0001){
         def ty = get(cam, 8) + dx * sens
         mut tp = get(cam, 9) - dy * sens
         if(tp < -89.9){ tp = -89.9 }
         if(tp > 89.9){ tp = 89.9 }
         set_idx(cam, 8, ty)
         set_idx(cam, 9, tp)
      }
   }

   ;; Rotation smoothing
   def rs = 1.0 - exp(0.0 - (get(cam, 13) * dt))
   mut cyaw = get(cam, 6)
   mut cpitch = get(cam, 7)
   cyaw += (get(cam, 8) - cyaw) * rs
   cpitch += (get(cam, 9) - cpitch) * rs
   set_idx(cam, 6, cyaw)
   set_idx(cam, 7, cpitch)

   ;; Movement direction
   def ry = cyaw * PI / 180.0
   def sinr = sin(ry) def cosr = cos(ry)
   def fwd_x = sinr def fwd_z = 0.0 - cosr
   def rgt_x = cosr def rgt_z = sinr

   mut wx = 0.0 mut wy = 0.0 mut wz = 0.0
   if(!skip_move){
      if(window.key_down(win, input.KEY_W)){ wx += fwd_x wz += fwd_z }
      if(window.key_down(win, input.KEY_S)){ wx -= fwd_x wz -= fwd_z }
      if(window.key_down(win, input.KEY_A)){ wx -= rgt_x wz -= rgt_z }
      if(window.key_down(win, input.KEY_D)){ wx += rgt_x wz += rgt_z }
      if(window.key_down(win, input.KEY_SPACE)){ wy += 1.0 }
      if(window.key_down(win, input.KEY_CTRL) || window.mod_down(win, input.MOD_CONTROL)){ wy -= 1.0 }
   }

   def wlen2 = wx*wx + wy*wy + wz*wz
   if(wlen2 > 0.0001){
      def inv = 1.0 / sqrt(wlen2)
      wx *= inv wy *= inv wz *= inv
   }

   ;; Movement smoothing
   def ms = 1.0 - exp(0.0 - (get(cam, 14) * dt))
   mut cx2 = get(cam, 17) mut cy2 = get(cam, 18) mut cz = get(cam, 19)
   cx2 += (wx - cx2) * ms
   cy2 += (wy - cy2) * ms
   cz  += (wz - cz)  * ms
   set_idx(cam, 17, cx2)
   set_idx(cam, 18, cy2)
   set_idx(cam, 19, cz)

   mut speed = get(cam, 11)
   if(!skip_move && window.key_down(win, input.KEY_SHIFT)){ speed *= get(cam, 15) }

   def drag_f = 1.0 / (1.0 + get(cam, 12) * dt)
   mut vx = get(cam, 3) mut vy = get(cam, 4) mut vz = get(cam, 5)
   vx = (vx + cx2 * speed * dt) * drag_f
   vy = (vy + cy2 * speed * dt) * drag_f
   vz = (vz + cz  * speed * dt) * drag_f
   set_idx(cam, 3, vx) set_idx(cam, 4, vy) set_idx(cam, 5, vz)

   set_idx(cam, 0, get(cam, 0) + vx * dt)
   set_idx(cam, 1, get(cam, 1) + vy * dt)
   set_idx(cam, 2, get(cam, 2) + vz * dt)
   cam
}

@jit
fn update_win_size(cam, w, h){
   "Cache window size into camera for fast access in update()."
   set_idx(cam, 20, float(w))
   set_idx(cam, 21, float(h))
}

@jit
fn apply(cam, gfx_cam){
   "Applies camera state to a gfx camera object."
   common.touch(cam) common.touch(gfx_cam)
   ;; Mutate existing pos vec3 in gfx_cam[0] to avoid allocation
   mut pos = get(gfx_cam, 0)
   if(is_list(pos) || is_tuple(pos)){
      set_idx(pos, 0, get(cam, 0))
      set_idx(pos, 1, get(cam, 1))
      set_idx(pos, 2, get(cam, 2))
   } else {
      set_idx(gfx_cam, 0, [get(cam,0), get(cam,1), get(cam,2)])
   }
   set_idx(gfx_cam, 4, get(cam, 6))
   set_idx(gfx_cam, 5, get(cam, 7))
   mut fov = get(cam, 16)
   if(fov > 0){ set_idx(gfx_cam, 3, float(fov)) }
}

@jit
fn get_yaw(cam){
   "Returns the current yaw (horizontal rotation) in degrees."
   get(cam, 6, 0.0)
}
@jit
fn get_pitch(cam){
   "Returns the current pitch (vertical rotation) in degrees."
   get(cam, 7, 0.0)
}
@jit
fn get_pos(cam){
   "Returns the current camera position as [x, y, z]."
   [get(cam, 0, 0.0), get(cam, 1, 0.0), get(cam, 2, 0.0)]
}
@jit
fn get_rot(cam){
   "Returns the current camera rotation as [yaw, pitch]."
   [get_yaw(cam), get_pitch(cam)]
}

fn set_fov(cam, fov){
   "Sets the field of view for the camera (degrees)."
   set_idx(cam, 16, fov)
}

fn set_speed(cam, speed){
   "Sets the movement speed (acceleration) of the camera."
   set_idx(cam, 11, speed)
}

fn set_sens(cam, sens){
   "Sets the mouse look sensitivity."
   set_idx(cam, 10, sens)
}
