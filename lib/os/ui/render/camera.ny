;; Keywords: render camera view
;; 3D Camera system for Nytrix.
module std.os.ui.render.camera(camera_init, camera_update, camera_free_look, camera_free_move, camera_vectors)
use std.core
use std.math
use std.math.vector
use std.math.matrix
use std.os.ui.window
use std.os.ui.window.native as native

fn camera_init(list: pos, any: yaw, any: pitch, any: fovy=120.0): any {
   "Initializes a 3D camera object. Returns a dictionary."
   def p = vec3(pos)
   mut c = {
      "pos": p,
      "yaw": yaw,
      "pitch": pitch,
      "fovy": fovy,
      "target": add(p, vec3(0, 0, -1)),
      "up": vec3(0, 1, 0),
      "sens": 0.15,
      "speed": 400.0
   }
   camera_update(c)
}

fn camera_update(any: c): any {
   "Updates the camera target based on yaw/pitch(degrees)."
   def yaw = float(c.get("yaw", 0.0))
   def pitch = float(c.get("pitch", 0.0))
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   def fwd = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))
   def pos = vec3(c.get("pos", [0,0,0]))
   c["pos"] = pos
   c["target"] = add(pos, fwd)
   c
}

fn camera_vectors(any: c): list {
   "Returns [fwd, rgt, up] vectors for the camera. Right vector is horizontal for FPS movement."
   def yaw = float(c.get("yaw", 0.0))
   def pitch = float(c.get("pitch", 0.0))
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   def fwd = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))
   def fwd_h = vec3(sin(ryaw), 0.0, -cos(ryaw))
   def up_global = vec3(c.get("up", [0,1,0]))
   def rgt = normalize(cross3(fwd_h, up_global))
   def up_local = cross3(rgt, fwd)
   [fwd, rgt, up_local]
}

fn camera_free_look(any: c, any: win, bool: locked=true): any {
   "Handles mouse look for the camera."
   if(!locked){ return c }
   def handle = win.get("handle")
   if(!handle){ return c }
   def ws = size(win)
   def cx = float(ws.get(0)) * 0.5
   def cy = float(ws.get(1)) * 0.5
   def mpos = native.get_cursor_pos(handle)
   def mx = float(mpos.get(0))
   def my = float(mpos.get(1))
   native.set_cursor_pos(handle, cx, cy)
   def sens = float(c.get("sens", 0.15))
   mut yaw = float(c.get("yaw", 0.0))
   mut pitch = float(c.get("pitch", 0.0))
   yaw = yaw + (mx - cx) * sens
   pitch = pitch - (my - cy) * sens
   if(pitch > 89.0){ pitch = 89.0 }
   if(pitch < -89.0){ pitch = -89.0 }
   c["yaw"] = yaw
   c["pitch"] = pitch
   camera_update(c)
}

fn camera_free_move(any: c, any: win, any: dt): any {
   "Handles WASD keyboard movement for the camera."
   def yaw = float(c.get("yaw", 0.0))
   def ryaw = yaw * PI / 180.0
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
      def speed = float(c.get("speed", 400.0))
      mut pos = vec3(c.get("pos", [0,0,0]))
      pos = add(pos, scale(nmove, speed * dt))
      c["pos"] = pos
      camera_update(c)
   }
   c
}
