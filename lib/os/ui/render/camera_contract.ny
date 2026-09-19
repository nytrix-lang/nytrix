;; Keywords: render camera contract matrix os ui
;; Lightweight positional-camera contract used by the rendering facade.
module std.os.ui.render.camera_contract(camera_init, camera_update, camera_set_look_at, __cam_compute_vectors)
use std.core
use std.math
use std.math.vector

fn camera_init(list pos, f64 yaw, f64 pitch, f64 aspect=1.33) list {
   "Initializes the facade's positional 3D camera representation."
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   def front = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))
   def target = [pos.get(0, 0.0) + at(front, 0, 0.0), pos.get(1, 0.0) + at(front, 1, 0.0), pos.get(2, 0.0) + at(front, 2, 0.0)]
   mut cam = list()
   cam = cam.append(pos)
   cam = cam.append(target)
   cam = cam.append(vec3(0.0, 1.0, 0.0))
   cam = cam.append(120.0)
   cam = cam.append(yaw)
   cam = cam.append(pitch)
   cam = cam.append(aspect)
   cam
}

fn camera_update(list camera) list {
   "Updates the facade camera target from its yaw/pitch orientation."
   def yaw = camera.get(4)
   def pitch = camera.get(5)
   def pos = camera.get(0)
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   mut target = camera.get(1)
   def fx, fy = sin(ryaw) * cos(rpitch), sin(rpitch)
   def fz = -cos(ryaw) * cos(rpitch)
   if is_list(target) {
      target[0] = pos.get(0) + fx
      target[1] = pos.get(1) + fy
      target[2] = pos.get(2) + fz
   } else {
      target = vec3(pos.get(0) + fx, pos.get(1) + fy, pos.get(2) + fz)
      camera[1] = target
   }
   camera
}

fn __cam_compute_vectors(f64 yaw, f64 pitch, any out) any {
   def y, p = float(yaw), float(pitch)
   def cp = cos(p)
   def fx = sin(y) * cp
   def fy = sin(p)
   def fz = -cos(y) * cp
   if is_list(out) || is_dict(out) {
      out[2] = fx
      out[3] = fy
      out[4] = fz
   }
   out
}

fn camera_set_look_at(any camera, list target) any {
   "Explicitly sets the facade camera look-at target."
   camera[1] = target
   camera
}
