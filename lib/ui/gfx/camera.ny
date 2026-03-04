;; Keywords: ui gfx camera 3d
;; 3D Camera system for Nytrix.

module std.ui.gfx.camera (
   camera_init, camera_update, camera_free_look, camera_free_move,
   camera_vectors
)

use std.core *
use std.math *
use std.math.vector *
use std.math.matrix *
use std.ui.window *
use std.ui.window.native as native

fn camera_init(pos, yaw, pitch, fovy=45.0){
   "Initializes a 3D camera object. Returns a dictionary."
   mut c = dict(16)
   dict_set(c, "pos",   pos)
   dict_set(c, "yaw",   yaw)
   dict_set(c, "pitch", pitch)
   dict_set(c, "fovy",  fovy)
   dict_set(c, "target", add(pos, [0,0,-1]))
   dict_set(c, "up",     [0,1,0])
   dict_set(c, "sens",   0.15)
   dict_set(c, "speed",  400.0)
   camera_update(c)
}

fn camera_update(c){
   "Updates the camera target based on yaw/pitch (degrees)."
   def yaw = float(dict_get(c, "yaw", 0.0))
   def pitch = float(dict_get(c, "pitch", 0.0))
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0

   ;; Baseline math from commit 7e329de1f2
   def fwd = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))
   def pos = dict_get(c, "pos", [0,0,0])
   dict_set(c, "target", add(pos, fwd))
   c
}

fn camera_vectors(c){
   "Returns [fwd, rgt, up] vectors for the camera. Right vector is horizontal for FPS movement."
   def yaw = float(dict_get(c, "yaw", 0.0))
   def pitch = float(dict_get(c, "pitch", 0.0))
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0

   ;; 3D Forward (for looking)
   def fwd = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))

   ;; Horizontal Forward (for strafing)
   def fwd_h = vec3(sin(ryaw), 0.0, -cos(ryaw))
   def up_global = dict_get(c, "up", [0,1,0])

   ;; Right vector calculation must be horizontal
   def rgt = normalize(cross3(fwd_h, up_global))
   def up_local = cross3(rgt, fwd)

   [fwd, rgt, up_local]
}

fn camera_free_look(c, win, locked=true){
   "Handles mouse look for the camera."
   if(!locked){ return c }
   def handle = dict_get(win, "handle")
   if(!handle){ return c }

   def ws = size(win)
   def cx = float(get(ws, 0)) * 0.5
   def cy = float(get(ws, 1)) * 0.5

   def mpos = native.get_cursor_pos(handle)
   def mx = float(get(mpos, 0))
   def my = float(get(mpos, 1))
   native.set_cursor_pos(handle, cx, cy)

   def sens = float(dict_get(c, "sens", 0.15))
   mut yaw = float(dict_get(c, "yaw", 0.0))
   mut pitch = float(dict_get(c, "pitch", 0.0))

   yaw = yaw + (mx - cx) * sens
   pitch = pitch - (my - cy) * sens

   if(pitch > 89.0){ pitch = 89.0 }
   if(pitch < -89.0){ pitch = -89.0 }

   dict_set(c, "yaw", yaw)
   dict_set(c, "pitch", pitch)
   camera_update(c)
}

fn camera_free_move(c, win, dt){
   "Handles WASD keyboard movement for the camera."
   def yaw = float(dict_get(c, "yaw", 0.0))
   def ryaw = yaw * PI / 180.0

   ;; Horizontal vectors for FPS movement
   def fwd_h = vec3(sin(ryaw), 0.0, -cos(ryaw))
   def rgt_h = vec3(cos(ryaw), 0.0, sin(ryaw))
   def up_g  = vec3(0.0, 1.0, 0.0)

   mut move = vec3(0,0,0)
   if(key_down(win, 87)){ move = add(move, fwd_h) } ; W
   if(key_down(win, 83)){ move = sub(move, fwd_h) } ; S
   if(key_down(win, 65)){ move = sub(move, rgt_h) } ; A
   if(key_down(win, 68)){ move = add(move, rgt_h) } ; D
   if(key_down(win, 69)){ move = add(move, up_g) } ; E
   if(key_down(win, 81)){ move = sub(move, up_g) } ; Q

   if(len2(move) > 0.000001){
      def nmove = normalize(move)
      def speed = float(dict_get(c, "speed", 400.0))
      mut pos = dict_get(c, "pos")
      pos = add(pos, scale(nmove, speed * dt))
      dict_set(c, "pos", pos)
      camera_update(c)
   }
   c
}
