;; Keywords: camera view
;; Camera movement, projection, and view-state operations for UI scenes.
module std.os.ui.camera(Camera, init, update, apply, get_yaw, get_pitch, get_pos, get_rot, get_fov, set_fov, set_speed, set_sens, set_smoothing, reset_motion, set_pitch_limits, fit_camera_space_pose, fit_camera_space_pose_margin, mesh_fit_bounds, mesh_fit_center_span, mesh_set_target, mesh_set_camera)
use std.core
use std.math
use std.math.float (is_nan, is_inf)
use std.math.vector
use std.math.matrix
use std.os.ui.consts (KEY_W, KEY_S, KEY_A, KEY_D, KEY_SPACE, KEY_CTRL, KEY_SHIFT, MOD_CONTROL)
use std.os.ui.window.input
use std.os.ui.window
use std.os.ui.window as ui_window

fn _mesh_num3(any: v, int: idx, f64: fallback): f64 { float(v.get(idx, fallback)) }

fn mesh_fit_bounds(dict: mesh): list {
   "Returns [min_x, min_y, min_z, max_x, max_y, max_z] from fitted mesh bounds."
   def wb_min = mesh.get("fit_world_min", mesh.get("min", [0.0, 0.0, 0.0]))
   def wb_max = mesh.get("fit_world_max", mesh.get("max", [1.0, 1.0, 1.0]))
   def min_x = _mesh_num3(wb_min, 0, 0.0)
   def min_y = _mesh_num3(wb_min, 1, 0.0)
   def min_z = _mesh_num3(wb_min, 2, 0.0)
   def max_x = _mesh_num3(wb_max, 0, 1.0)
   def max_y = _mesh_num3(wb_max, 1, 1.0)
   def max_z = _mesh_num3(wb_max, 2, 1.0)
   [min_x, min_y, min_z, max_x, max_y, max_z]
}

fn mesh_fit_center_span(dict: mesh): list {
   "Returns bounds plus fitted center/span values for scene camera framing."
   def b = mesh_fit_bounds(mesh)
   def min_x = float(b.get(0, 0.0))
   def min_y = float(b.get(1, 0.0))
   def min_z = float(b.get(2, 0.0))
   def max_x = float(b.get(3, 1.0))
   def max_y = float(b.get(4, 1.0))
   def max_z = float(b.get(5, 1.0))
   def tcx = (min_x + max_x) * 0.5
   def tcy = (min_y + max_y) * 0.5
   def tcz = (min_z + max_z) * 0.5
   def wsx = max(0.001, max_x - min_x)
   def wsy = max(0.001, max_y - min_y)
   def wsz = max(0.001, max_z - min_z)
   [min_x, min_y, min_z, max_x, max_y, max_z, tcx, tcy, tcz, wsx, wsy, wsz]
}

fn mesh_set_target(dict: mesh, f64: tx, f64: ty, f64: tz): dict {
   "Stores fitted scene target coordinates on a mesh-like dictionary."
   mesh["fit_target_x"] = tx
   mesh["fit_target_y"] = ty
   mesh["fit_target_z"] = tz
   mesh
}

fn mesh_set_camera(dict: mesh, f64: cx, f64: cy, f64: cz, f64: yaw, f64: pitch): dict {
   "Stores fitted scene camera coordinates and orientation on a mesh-like dictionary."
   mesh["fit_cam_x"] = cx
   mesh["fit_cam_y"] = cy
   mesh["fit_cam_z"] = cz
   mesh["fit_cam_yaw"] = yaw
   mesh["fit_cam_pitch"] = pitch
   mesh
}

fn fit_camera_space_pose_margin(f64: tcx, f64: tcy, f64: tcz, f64: span_x, f64: span_y, f64: span_z, f64: fit_fov_deg, f64: aspect, f64: yaw_deg, f64: pitch_deg, f64: margin_mul=1.08, f64: pad_diag_mul=0.06, f64: pad_max_mul=0.035, f64: min_diag_mul=1.18): dict {
   "Frames fitted bounds with a conservative sphere/axis solve. This path must
   never place the camera inside malformed or degenerate bounds."
   mut half_x, half_y = abs(float(span_x)) * 0.5, abs(float(span_y)) * 0.5
   mut half_z = abs(float(span_z)) * 0.5
   def half_max = max(half_x, max(half_y, half_z))
   def half_eps = max(0.00001, half_max * 0.001)
   if(half_x < half_eps){ half_x = half_eps }
   if(half_y < half_eps){ half_y = half_eps }
   if(half_z < half_eps){ half_z = half_eps }
   mut safe_aspect = float(aspect)
   if(safe_aspect < 0.1){ safe_aspect = 16.0 / 9.0 }
   mut safe_fov = float(fit_fov_deg)
   if(safe_fov <= 1.0 || safe_fov >= 179.0){ safe_fov = 120.0 }
   mut safe_yaw = float(yaw_deg)
   mut safe_pitch = float(pitch_deg)
   if(is_nan(safe_yaw) || is_inf(safe_yaw) || abs(safe_yaw) > 360000.0){ safe_yaw = 0.0 }
   if(is_nan(safe_pitch) || is_inf(safe_pitch) || abs(safe_pitch) > 360000.0){ safe_pitch = 0.0 }
   while(safe_yaw > 180.0){ safe_yaw = safe_yaw - 360.0 }
   while(safe_yaw < -180.0){ safe_yaw = safe_yaw + 360.0 }
   if(abs(safe_yaw) < 0.000001){ safe_yaw = 0.0 }
   if(abs(safe_pitch) < 0.000001){ safe_pitch = 0.0 }
   if(safe_pitch > 89.0){ safe_pitch = 89.0 }
   if(safe_pitch < -89.0){ safe_pitch = -89.0 }
   def ry, rp = safe_yaw * PI / 180.0, safe_pitch * PI / 180.0
   def cp = cos(rp)
   mut fx, fy = sin(ry) * cp, sin(rp)
   mut fz = 0.0 - cos(ry) * cp
   def fl = sqrt(fx * fx + fy * fy + fz * fz)
   if(fl < 0.000001){
      fx, fy = 0.0, 0.0
      fz = 0.0 - 1.0
   }
   mut rx = cos(ry)
   mut ryv = 0.0
   mut rz = sin(ry)
   def rl = sqrt(rx * rx + rz * rz)
   if(rl < 0.000001){
      rx = 1.0
      ryv = 0.0
      rz = 0.0
   }
   mut ux, uy = ryv * fz - rz * fy, rz * fx - rx * fz
   mut uz = rx * fy - ryv * fx
   def ul = sqrt(ux * ux + uy * uy + uz * uz)
   if(ul < 0.000001){
      ux, uy = 0.0, 1.0
      uz = 0.0
   }
   def fovy, fovx = safe_fov * PI / 180.0, 2.0 * atan(tan(fovy * 0.5) * safe_aspect)
   def tan_half_x, tan_half_y = max(tan(fovx * 0.5), 0.001), max(tan(fovy * 0.5), 0.001)
   def half_diag = sqrt(half_x * half_x + half_y * half_y + half_z * half_z)
   def support_x = half_x * abs(rx) + half_y * abs(ryv) + half_z * abs(rz)
   def support_y = half_x * abs(ux) + half_y * abs(uy) + half_z * abs(uz)
   def support_z = half_x * abs(fx) + half_y * abs(fy) + half_z * abs(fz)
   def axis_distance = max(support_x / tan_half_x, support_y / tan_half_y) + support_z
   def fit_pad = max(half_diag * pad_diag_mul, half_max * pad_max_mul)
   mut frame_distance = axis_distance * margin_mul + fit_pad
   def min_distance = half_diag * min_diag_mul + fit_pad
   if(frame_distance < min_distance){ frame_distance = min_distance }
   {
      "x": tcx - fx * frame_distance,
      "y": tcy - fy * frame_distance,
      "z": tcz - fz * frame_distance,
      "yaw": safe_yaw,
      "pitch": safe_pitch,
      "fov": safe_fov
   }
}

fn fit_camera_space_pose(f64: tcx, f64: tcy, f64: tcz, f64: span_x, f64: span_y, f64: span_z, f64: fit_fov_deg, f64: aspect, f64: yaw_deg, f64: pitch_deg): dict {
   "Frames fitted bounds with the default camera-fit margin policy."
   fit_camera_space_pose_margin(
      tcx, tcy, tcz,
      span_x, span_y, span_z,
      fit_fov_deg, aspect,
      yaw_deg, pitch_deg,
   1.08, 0.06, 0.035, 1.18)
}

def Camera = "Camera"

fn init(list: pos=[0,0,0], f64: yaw=0.0, f64: pitch=0.0): list {
   "Initializes a new Camera object."
   [
      float(pos.get(0, 0.0)), float(pos.get(1, 0.0)), float(pos.get(2, 0.0)),
      0.0, 0.0, 0.0,
      yaw + 0.0, pitch + 0.0, yaw + 0.0, pitch + 0.0,
      0.08, 500.0, 10.0, 18.0, 12.0, 3.0, 45.0,
      0.0, 0.0, 0.0,
      1280.0, 720.0,
      -89.9, 89.9
   ]
}

@jit
fn update(list: cam, f64: dt, any: win, bool: skip_look=false, bool: skip_move=false): list {
   "Updates camera state based on input and physics."
   def _ww = cam.get(20) def _wh = cam.get(21)
   def _cx = _ww * 0.5 def _cy = _wh * 0.5
   mut target_yaw = cam.get(8)
   mut target_pitch = cam.get(9)
   if(!skip_look){
      def m_pos = ui_window.cursor_pos(win)
      def mx = m_pos.get(0, _cx)
      def my = m_pos.get(1, _cy)
      ui_window.set_cursor_pos(win, _cx, _cy)
      def dx, dy = mx - _cx, my - _cy
      def sens = cam.get(10)
      if(abs(dx) > 0.0001 || abs(dy) > 0.0001){
         target_yaw += dx * sens
         target_pitch -= dy * sens
         def p_min = cam.get(22, -89.9)
         def p_max = cam.get(23, 89.9)
         if(target_pitch < p_min){ target_pitch = p_min }
         if(target_pitch > p_max){ target_pitch = p_max }
         cam[8] = target_yaw
         cam[9] = target_pitch
      }
   }
   def rs = 1.0 - exp(0.0 - (cam.get(13) * dt))
   mut cyaw = cam.get(6)
   mut cpitch = cam.get(7)
   cyaw += (target_yaw - cyaw) * rs
   cpitch += (target_pitch - cpitch) * rs
   cam[6] = cyaw
   cam[7] = cpitch
   def ry = cyaw * PI / 180.0
   def sinr = sin(ry) def cosr = cos(ry)
   def fwd_x = sinr def fwd_z = 0.0 - cosr
   def rgt_x = cosr def rgt_z = sinr
   mut wx, wy, wz = 0.0, 0.0, 0.0
   if(!skip_move){
      if(ui_window.key_down(win, KEY_W)){ wx += fwd_x wz += fwd_z }
      if(ui_window.key_down(win, KEY_S)){ wx -= fwd_x wz -= fwd_z }
      if(ui_window.key_down(win, KEY_A)){ wx -= rgt_x wz -= rgt_z }
      if(ui_window.key_down(win, KEY_D)){ wx += rgt_x wz += rgt_z }
      if(ui_window.key_down(win, KEY_SPACE)){ wy += 1.0 }
      if(ui_window.key_down(win, KEY_CTRL) || ui_window.mod_down(win, MOD_CONTROL)){ wy -= 1.0 }
   }
   def wlen2 = wx*wx + wy*wy + wz*wz
   if(wlen2 > 0.0001){
      def inv = 1.0 / sqrt(wlen2)
      wx *= inv wy *= inv wz *= inv
   }
   def ms = 1.0 - exp(0.0 - (cam.get(14) * dt))
   mut cx2, cy2, cz = cam.get(17), cam.get(18), cam.get(19)
   cx2 += (wx - cx2) * ms
   cy2 += (wy - cy2) * ms
   cz  += (wz - cz)  * ms
   cam[17] = cx2
   cam[18] = cy2
   cam[19] = cz
   mut speed = cam.get(11)
   if(!skip_move && ui_window.key_down(win, KEY_SHIFT)){ speed *= cam.get(15) }
   def drag_f = 1.0 / (1.0 + cam.get(12) * dt)
   mut vx, vy, vz = cam.get(3), cam.get(4), cam.get(5)
   vx, vy = (vx + cx2 * speed * dt) * drag_f, (vy + cy2 * speed * dt) * drag_f
   vz = (vz + cz  * speed * dt) * drag_f
   cam[3] = vx
   cam[4] = vy
   cam[5] = vz
   def px = cam.get(0) def py = cam.get(1) def pz = cam.get(2)
   cam[0] = px + vx * dt
   cam[1] = py + vy * dt
   cam[2] = pz + vz * dt
   cam
}

@jit
fn update_win_size(list: cam, f64: w, f64: h): list {
   "Cache window size into camera for fast access in update()."
   cam[20] = float(w)
   cam[21] = float(h)
   cam
}

@jit
fn apply(list: cam, list: gfx_cam): list {
   "Applies camera state to a renderer-style camera object(Target/Up vectors)."
   def px = cam.get(0) def py = cam.get(1) def pz = cam.get(2)
   def yaw = cam.get(6) def pitch = cam.get(7)
   def fov = cam.get(16)
   def ry = yaw * PI / 180.0
   def rp = pitch * PI / 180.0
   def cp = cos(rp)
   def tx = px + sin(ry) * cp
   def ty = py + sin(rp)
   def tz = pz - cos(ry) * cp
   gfx_cam[0] = [px, py, pz]
   gfx_cam[1] = [tx, ty, tz]
   gfx_cam[2] = [0.0, 1.0, 0.0]
   gfx_cam[3] = float(fov)
   gfx_cam
}

@jit
fn get_yaw(list: cam): f64 {
   "Returns the current yaw(horizontal rotation) in degrees."
   cam.get(6, 0.0)
}

@jit
fn get_pitch(list: cam): f64 {
   "Returns the current pitch(vertical rotation) in degrees."
   cam.get(7, 0.0)
}

@jit
fn get_pos(list: cam): list {
   "Returns the current camera position as [x, y, z]."
   [cam.get(0, 0.0), cam.get(1, 0.0), cam.get(2, 0.0)]
}

@jit
fn get_rot(list: cam): list {
   "Returns the current camera rotation as [yaw, pitch]."
   [get_yaw(cam), get_pitch(cam)]
}

@jit
fn get_fov(list: cam): f64 {
   "Returns the current camera field of view in degrees."
   cam.get(16, 45.0)
}

fn set_fov(list: cam, f64: fov): list {
   "Sets the field of view for the camera(degrees)."
   mut v = fov + 0.0
   if(v < 15.0){ v = 15.0 }
   if(v > 120.0){ v = 120.0 }
   cam[16] = v
   cam
}

fn set_speed(list: cam, f64: speed): list {
   "Sets the movement speed(acceleration) of the camera."
   cam[11] = speed
   cam
}

fn set_sens(list: cam, f64: sens): list {
   "Sets the mouse look sensitivity."
   cam[10] = sens
   cam
}

fn set_smoothing(list: cam, f64: rot_response=10.0, f64: move_smooth=10.0): list {
   "Sets camera rotation and movement smoothing values."
   cam[13] = rot_response
   cam[14] = move_smooth
   cam
}

fn set_pitch_limits(list: cam, f64: min_v, f64: max_v): list {
   "Sets the vertical rotation limits for the camera."
   cam[22] = min_v + 0.0
   cam[23] = max_v + 0.0
   cam
}

fn reset_motion(list: cam): list {
   "Clears current velocity and smoothed movement intent."
   if(!cam || !is_list(cam) || cam.len < 20){ return cam }
   cam[3] = 0.0
   cam[4] = 0.0
   cam[5] = 0.0
   cam[17] = 0.0
   cam[18] = 0.0
   cam[19] = 0.0
   cam
}
