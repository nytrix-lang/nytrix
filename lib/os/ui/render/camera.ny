;; Keywords: render camera view os ui
;; 3D Camera system for Nytrix.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.camera(Camera, init, update, apply, get_yaw, get_pitch, get_pos, get_rot, get_fov, set_fov, set_speed, set_sens, set_smoothing, reset_motion, set_pitch_limits, target_from_angles, fit_camera_space_pose, fit_camera_space_pose_margin, mesh_fit_bounds, mesh_fit_center_span, mesh_set_target, mesh_set_camera, env_float_range, fit_camera_sane, fit_bounds_pose, angle_state, fit_scene_state, simulate_frame_state, camera_init, camera_update, camera_free_look, camera_free_move, camera_vectors)
use std.core
use std.core.common as common
use std.core.str as str
use std.math
use std.math.float (is_nan, is_inf)
use std.math.vector
use std.math.matrix
use std.os.ui.window.consts (KEY_W, KEY_S, KEY_A, KEY_D, KEY_E, KEY_Q, KEY_SPACE, KEY_CTRL, KEY_SHIFT, MOD_CONTROL)
use std.os.ui.window as ui_window
use std.os.ui.window.native as native

fn _mesh_num3(any v, int idx, f64 fallback) f64 { float(v.get(idx, fallback)) }

fn mesh_fit_bounds(dict mesh) list {
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

fn mesh_fit_center_span(dict mesh) list {
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

fn mesh_set_target(dict mesh, f64 tx, f64 ty, f64 tz) dict {
   "Stores fitted scene target coordinates on a mesh-like dictionary."
   mesh["fit_target_x"] = tx
   mesh["fit_target_y"] = ty
   mesh["fit_target_z"] = tz
   mesh
}

fn mesh_set_camera(dict mesh, f64 cx, f64 cy, f64 cz, f64 yaw, f64 pitch) dict {
   "Stores fitted scene camera coordinates and orientation on a mesh-like dictionary."
   mesh["fit_cam_x"] = cx
   mesh["fit_cam_y"] = cy
   mesh["fit_cam_z"] = cz
   mesh["fit_cam_yaw"] = yaw
   mesh["fit_cam_pitch"] = pitch
   mesh
}

fn target_from_angles(f64 px, f64 py, f64 pz, f64 yaw_deg, f64 pitch_deg) list {
   "Returns the look target for a camera position plus yaw/pitch angles in degrees."
   def ry = yaw_deg * (PI / 180.0)
   def rp = pitch_deg * (PI / 180.0)
   def cp = cos(rp)
   [
      px + sin(ry) * cp,
      py + sin(rp),
      pz - cos(ry) * cp,
   ]
}

fn fit_camera_space_pose_margin(f64 tcx, f64 tcy, f64 tcz, f64 span_x, f64 span_y, f64 span_z, f64 fit_fov_deg, f64 aspect, f64 yaw_deg, f64 pitch_deg, f64 margin_mul=1.08, f64 pad_diag_mul=0.06, f64 pad_max_mul=0.035, f64 min_diag_mul=1.18) dict {
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

fn fit_camera_space_pose(f64 tcx, f64 tcy, f64 tcz, f64 span_x, f64 span_y, f64 span_z, f64 fit_fov_deg, f64 aspect, f64 yaw_deg, f64 pitch_deg) dict {
   "Frames fitted bounds with the default camera-fit margin policy."
   fit_camera_space_pose_margin(
      tcx, tcy, tcz,
      span_x, span_y, span_z,
      fit_fov_deg, aspect,
      yaw_deg, pitch_deg,
   1.08, 0.06, 0.035, 1.18)
}

fn env_float_range(str name, f64 lo, f64 hi, f64 fallback) f64 {
   "Reads a bounded floating-point environment override."
   def raw = common.env_trim(name)
   if(raw.len <= 0){ return fallback }
   def v = str.atof(raw)
   (v >= lo && v <= hi) ? v : fallback
}

fn _mesh_num(any mesh, str key, f64 fallback=0.0) f64 {
   is_dict(mesh) ? float(mesh.get(key, fallback)) : fallback
}

fn fit_camera_sane(any mesh) bool {
   "Validates that cached mesh fit-camera values are finite and usable."
   if(!is_dict(mesh)){ return false }
   def fit_scale = _mesh_num(mesh, "fit_scale", 0.0)
   if(is_nan(fit_scale) || is_inf(fit_scale) || fit_scale <= 0.000001 || abs(fit_scale) > 1000000.0){ return false }
   def cam_x, cam_y = _mesh_num(mesh, "fit_cam_x", 0.0), _mesh_num(mesh, "fit_cam_y", 0.0)
   def cam_z = _mesh_num(mesh, "fit_cam_z", 0.0)
   def target_x, target_y = _mesh_num(mesh, "fit_target_x", cam_x), _mesh_num(mesh, "fit_target_y", cam_y)
   def target_z = _mesh_num(mesh, "fit_target_z", cam_z)
   if(is_nan(cam_x) || is_inf(cam_x) || abs(cam_x) > 100000000.0 ||
      is_nan(cam_y) || is_inf(cam_y) || abs(cam_y) > 100000000.0 ||
      is_nan(cam_z) || is_inf(cam_z) || abs(cam_z) > 100000000.0 ||
      is_nan(target_x) || is_inf(target_x) || abs(target_x) > 100000000.0 ||
      is_nan(target_y) || is_inf(target_y) || abs(target_y) > 100000000.0 ||
      is_nan(target_z) || is_inf(target_z) || abs(target_z) > 100000000.0){
      return false
   }
   def dx, dy = target_x - cam_x, target_y - cam_y
   def dz = target_z - cam_z
   dx * dx + dy * dy + dz * dz > 0.000001
}

fn fit_bounds_pose(f64 tcx, f64 tcy, f64 tcz, f64 span_x, f64 span_y, f64 span_z, f64 fov_deg, f64 aspect, f64 yaw_deg, f64 pitch_deg, f64 margin_mul=1.12) dict {
   "Computes a camera pose that frames raw bounds."
   fit_camera_space_pose_margin(float(tcx),
      float(tcy), float(tcz),
      float(span_x), float(span_y), float(span_z),
      float(fov_deg), float(aspect),
      float(yaw_deg), float(pitch_deg),
   float(margin_mul), 0.08, 0.05, 1.20)
}

fn angle_state(f64 sane_cam_x, f64 sane_cam_y, f64 sane_cam_z, f64 sane_target_x, f64 sane_target_y, f64 sane_target_z, f64 yaw, f64 pitch) list {
   "Computes yaw/pitch from a camera-target pair, with fallback angles."
   def fx, fy = sane_target_x - sane_cam_x, sane_target_y - sane_cam_y
   def fz, fh = sane_target_z - sane_cam_z, sqrt(fx * fx + fz * fz)
   mut fit_cam_yaw, fit_cam_pitch = yaw, pitch
   if(fh > 0.000001 || abs(fy) > 0.000001){
      fit_cam_yaw = atan2(fx, -fz) * 180.0 / PI
      fit_cam_pitch = atan2(fy, max(0.000001, fh)) * 180.0 / PI
   }
   [fit_cam_yaw, fit_cam_pitch]
}

fn _auto_bounds_shape(f64 span_x, f64 span_y, f64 span_z) dict {
   def max_span = max(0.001, max(span_x, max(span_y, span_z)))
   def thin_x = span_x <= max(0.01, max(span_y, span_z) * 0.04) && min(span_y, span_z) > max(0.01, span_x * 12.0)
   def thin_y = span_y <= max(0.01, max(span_x, span_z) * 0.04) && min(span_x, span_z) > max(0.01, span_y * 12.0)
   def thin_z = span_z <= max(0.01, max(span_x, span_y) * 0.04) && min(span_x, span_y) > max(0.01, span_z * 12.0)
   def tall = span_y > max(span_x, span_z) * 1.65
   mut yaw, pitch = -35.0, tall ? -4.0 : -10.0
   mut margin, fov_mul = 1.16, 1.0
   mut branch = tall ? "auto-tall" : "auto-bounds"
   mut sx, sy = span_x, span_y
   mut sz = span_z
   if(thin_y){
      sy = max(0.001, max_span * 0.015)
      yaw = 0.0
      pitch = -88.0
      margin = 1.55
      fov_mul = 1.12
      branch = "auto-flat-xz"
   } elif(thin_x){
      sx = max(0.001, max_span * 0.015)
      yaw = -90.0
      pitch = tall ? -4.0 : 0.0
      margin = 1.40
      branch = "auto-flat-yz"
   } elif(thin_z){
      sz = max(0.001, max_span * 0.015)
      yaw = 0.0
      pitch = tall ? -4.0 : 0.0
      margin = 1.40
      branch = "auto-flat-xy"
   } elif(tall){
      margin = 1.22
   } elif(max(span_x, span_z) > span_y * 2.4){
      yaw = -28.0
      pitch = -8.0
      margin = 1.24
      branch = "auto-wide"
   }
   {"yaw": yaw, "pitch": pitch, "margin": margin, "fov_mul": fov_mul, "span_x": sx, "span_y": sy, "span_z": sz, "branch": branch}
}

fn _auto_fit_state(f64 min_x, f64 min_y, f64 min_z, f64 max_x, f64 max_y, f64 max_z,
   f64 span_x, f64 span_y, f64 span_z, f64 safe_fov, f64 safe_aspect, bool dump_pose=false, bool wide_mode=false) list {
   def tcx, tcy = (min_x + max_x) * 0.5, (min_y + max_y) * 0.5
   def tcz = (min_z + max_z) * 0.5
   def shape = _auto_bounds_shape(span_x, span_y, span_z)
   mut yaw, pitch = float(shape.get("yaw", -35.0)), float(shape.get("pitch", -10.0))
   mut margin = float(shape.get("margin", 1.16))
   if(wide_mode){ margin = max(1.08, margin * 0.94) }
   if(dump_pose){
      safe_fov = env_float_range("NY_UI_DUMP_FOV", 15.0, 120.0, safe_fov)
      def fill = env_float_range("NY_UI_DUMP_FIT_FILL", 0.20, 0.98, 0.0)
      if(fill > 0.0){ margin = clamp(1.0 / fill, 1.02, 4.0) }
      yaw = env_float_range("NY_UI_DUMP_YAW", -360.0, 360.0, yaw)
      pitch = env_float_range("NY_UI_DUMP_PITCH", -89.0, 89.0, pitch)
   }
   safe_fov = clamp(safe_fov * float(shape.get("fov_mul", 1.0)), 15.0, 120.0)
   def pose = fit_bounds_pose(tcx, tcy, tcz,
      float(shape.get("span_x", span_x)), float(shape.get("span_y", span_y)), float(shape.get("span_z", span_z)),
   safe_fov, safe_aspect, yaw, pitch, margin)
   mut cam_x, cam_y = float(pose.get("x", tcx)), float(pose.get("y", tcy))
   mut cam_z = float(pose.get("z", tcz + max(span_x, max(span_y, span_z))))
   if(dump_pose){
      def dist_scale = env_float_range("NY_UI_DUMP_FIT_DIST_SCALE", 0.05, 4.0, 1.0)
      if(abs(dist_scale - 1.0) > 0.000001){
         cam_x = tcx + (cam_x - tcx) * dist_scale
         cam_y = tcy + (cam_y - tcy) * dist_scale
         cam_z = tcz + (cam_z - tcz) * dist_scale
      }
   }
   [cam_x, cam_y, cam_z, tcx, tcy, tcz, float(pose.get("yaw", yaw)), float(pose.get("pitch", pitch)), safe_fov, safe_fov, to_str(shape.get("branch", "auto-bounds"))]
}

fn fit_scene_state(any mesh, any opts=0) dict {
   "Builds a model-agnostic camera fit state for a scene mesh."
   if(!is_dict(mesh)){ return dict(0) }
   def cfg = is_dict(opts) ? opts : dict(0)
   def dump_pose = bool(cfg.get("dump_pose", false))
   def wide_mode = bool(cfg.get("wide_mode", false))
   def win_w = float(cfg.get("win_w", 1280.0))
   def win_h = float(cfg.get("win_h", 720.0))
   def fit_cam_x = _mesh_num(mesh, "fit_cam_x", float(cfg.get("cam_x", 0.0)))
   def fit_cam_y = _mesh_num(mesh, "fit_cam_y", float(cfg.get("cam_y", 0.0)))
   def fit_cam_z = _mesh_num(mesh, "fit_cam_z", float(cfg.get("cam_z", 0.0)))
   mut fit_cam_yaw = _mesh_num(mesh, "fit_cam_yaw", float(cfg.get("cam_yaw", 0.0)))
   mut fit_cam_pitch = _mesh_num(mesh, "fit_cam_pitch", float(cfg.get("cam_pitch", 0.0)))
   def fit_cam_fov = _mesh_num(mesh, "fit_cam_fov", float(cfg.get("cam_fov", 60.0)))
   def bounds = mesh_fit_center_span(mesh)
   def min_x, min_y = float(bounds.get(0, 0.0)), float(bounds.get(1, 0.0))
   def min_z = float(bounds.get(2, 0.0))
   def max_x, max_y = float(bounds.get(3, 1.0)), float(bounds.get(4, 1.0))
   def max_z = float(bounds.get(5, 1.0))
   def span_x, span_y = float(bounds.get(9, 0.001)), float(bounds.get(10, 0.001))
   def span_z = float(bounds.get(11, 0.001))
   mut safe_aspect = (win_h > 1.0) ? (win_w / win_h) : (16.0 / 9.0)
   if(safe_aspect < 0.1){ safe_aspect = 16.0 / 9.0 }
   mut safe_fov = fit_cam_fov
   if(safe_fov < 15.0 || safe_fov > 120.0){ safe_fov = 60.0 }
   mut fit_branch = "auto-bounds"
   def fit_state = _auto_fit_state(min_x, min_y, min_z, max_x, max_y, max_z, span_x, span_y, span_z, safe_fov, safe_aspect, dump_pose, wide_mode)
   mut sane_cam_x, sane_cam_y = 0.0, 0.0
   mut sane_cam_z = 0.0
   mut sane_target_x, sane_target_y = 0.0, 0.0
   mut sane_target_z = 0.0
   mut applied_fov = safe_fov
   sane_cam_x, sane_cam_y, sane_cam_z = float(fit_state.get(0, sane_cam_x)), float(fit_state.get(1, sane_cam_y)), float(fit_state.get(2, sane_cam_z))
   sane_target_x, sane_target_y, sane_target_z = float(fit_state.get(3, sane_target_x)), float(fit_state.get(4, sane_target_y)), float(fit_state.get(5, sane_target_z))
   fit_cam_yaw, fit_cam_pitch = float(fit_state.get(6, fit_cam_yaw)), float(fit_state.get(7, fit_cam_pitch))
   safe_fov, applied_fov = float(fit_state.get(8, safe_fov)), float(fit_state.get(9, applied_fov))
   fit_branch = to_str(fit_state.get(10, fit_branch))
   def angles = angle_state(sane_cam_x, sane_cam_y, sane_cam_z, sane_target_x, sane_target_y, sane_target_z, fit_cam_yaw, fit_cam_pitch)
   fit_cam_yaw, fit_cam_pitch = float(angles.get(0, fit_cam_yaw)), float(angles.get(1, fit_cam_pitch))
   {
      "fit_cam_x": fit_cam_x, "fit_cam_y": fit_cam_y, "fit_cam_z": fit_cam_z,
      "cam_x": sane_cam_x, "cam_y": sane_cam_y, "cam_z": sane_cam_z,
      "target_x": sane_target_x, "target_y": sane_target_y, "target_z": sane_target_z,
      "min_x": min_x, "min_y": min_y, "min_z": min_z,
      "max_x": max_x, "max_y": max_y, "max_z": max_z,
      "force_dump_pose": false, "branch": fit_branch,
      "yaw": fit_cam_yaw, "pitch": fit_cam_pitch, "fov": applied_fov
   }
}

fn _zero_small(f64 v) f64 { abs(v) < 1e-6 ? 0.0 : v }

fn simulate_frame_state(dict state) dict {
   "Integrates one free-camera frame and returns updated camera/input state."
   mut yaw, pitch = float(state.get("yaw", 0.0)), float(state.get("pitch", 0.0))
   mut target_yaw, target_pitch = float(state.get("target_yaw", yaw)), float(state.get("target_pitch", pitch))
   mut rmb_dx_smooth, rmb_dy_smooth = float(state.get("rmb_dx_smooth", 0.0)), float(state.get("rmb_dy_smooth", 0.0))
   def dt = float(state.get("dt", 0.0))
   def dx, dy = float(state.get("dx", 0.0)), float(state.get("dy", 0.0))
   def fps_mouse_look = bool(state.get("rmb_look_active", false)) || bool(state.get("cursor_lock", false)) || bool(state.get("focus_mouse_look", false))
   mut look_applied = false
   mut look_dx, look_dy = 0.0, 0.0
   mut h_sinr = sin(yaw * (PI / 180.0))
   mut h_cosr = cos(yaw * (PI / 180.0))
   if(!bool(state.get("gui_mouse", false)) && !bool(state.get("skip_look", false)) && (abs(dx) > 0.0001 || abs(dy) > 0.0001)){
      look_dx, look_dy = dx, dy
      if(fps_mouse_look){
         look_dx, look_dy = clamp(look_dx, -48.0, 48.0), clamp(look_dy, -48.0, 48.0)
         ;; Raw/captured mouse should be direct by default.  The old 0.58
         ;; smoothing was frame-rate dependent and made Vulkan camera look jitter
         ;; and laggy, especially when swap/present timing varied.
         mut smooth_alpha = clamp(float(state.get("look_smooth_alpha", 1.0)), 0.0, 1.0)
         ;; Make smoothing stable across 30/60/144Hz.  A fixed per-frame alpha
         ;; changes feel with present timing and can show as jitter when Vulkan
         ;; pacing varies.  Treat the configured alpha as the 60Hz response.
         if(smooth_alpha > 0.0 && smooth_alpha < 0.999){
            def frame_scale = clamp(dt * 60.0, 0.25, 4.0)
            smooth_alpha = 1.0 - pow(1.0 - smooth_alpha, frame_scale)
         }
         if(smooth_alpha >= 0.999){
            rmb_dx_smooth, rmb_dy_smooth = look_dx, look_dy
         } else {
            rmb_dx_smooth = rmb_dx_smooth + (look_dx - rmb_dx_smooth) * smooth_alpha
            rmb_dy_smooth = rmb_dy_smooth + (look_dy - rmb_dy_smooth) * smooth_alpha
         }
         look_dx, look_dy = rmb_dx_smooth, rmb_dy_smooth
      } elif(!bool(state.get("prep_gui", false))){
         look_dx, look_dy = clamp(look_dx, -60.0, 60.0), clamp(look_dy, -60.0, 60.0)
      }
      def look_sens = float(state.get("sens", 0.08)) * (fps_mouse_look ? float(state.get("rmb_sens_mul", 1.0)) : 1.0)
      yaw += look_dx * look_sens
      pitch -= look_dy * look_sens
      pitch = clamp(pitch, float(state.get("pitch_min", -89.9)), float(state.get("pitch_max", 89.9)))
      target_yaw, target_pitch = yaw, pitch
      h_sinr = sin(yaw * (PI / 180.0))
      h_cosr = cos(yaw * (PI / 180.0))
      look_applied = true
   } elif(bool(state.get("rmb_look_active", false))){
      rmb_dx_smooth, rmb_dy_smooth = 0.0, 0.0
   }
   def fwd_x, fwd_z = h_sinr, 0.0 - h_cosr
   def rgt_x, rgt_z = h_cosr, h_sinr
   mut wx, wy, wz = 0.0, 0.0, 0.0
   if(bool(state.get("key_w", false))){ wx += fwd_x wz += fwd_z }
   if(bool(state.get("key_s", false))){ wx -= fwd_x wz -= fwd_z }
   if(bool(state.get("key_a", false))){ wx -= rgt_x wz -= rgt_z }
   if(bool(state.get("key_d", false))){ wx += rgt_x wz += rgt_z }
   if(bool(state.get("key_space", false))){ wy += 1.0 }
   if(bool(state.get("key_ctrl", false))){ wy -= 1.0 }
   def wlen2 = wx * wx + wy * wy + wz * wz
   if(wlen2 > 0.0001){
      def inv = 1.0 / sqrt(wlen2)
      wx *= inv
      wy *= inv
      wz *= inv
   }
   def ms = 1.0 - exp(0.0 - (float(state.get("damp", 18.0)) * dt))
   mut spdx = float(state.get("spdx", 0.0)) + (wx - float(state.get("spdx", 0.0))) * ms
   mut spdy = float(state.get("spdy", 0.0)) + (wy - float(state.get("spdy", 0.0))) * ms
   mut spdz = float(state.get("spdz", 0.0)) + (wz - float(state.get("spdz", 0.0))) * ms
   mut speed = float(state.get("speed", 500.0))
   if(bool(state.get("key_shift", false))){ speed *= float(state.get("speed_mul", 3.0)) }
   def drag_f = 1.0 / (1.0 + float(state.get("drag", 10.0)) * dt)
   mut vx = (float(state.get("vx", 0.0)) + spdx * speed * dt) * drag_f
   mut vy = (float(state.get("vy", 0.0)) + spdy * speed * dt) * drag_f
   mut vz = (float(state.get("vz", 0.0)) + spdz * speed * dt) * drag_f
   {
      "yaw": yaw, "pitch": pitch, "target_yaw": target_yaw, "target_pitch": target_pitch,
      "rmb_dx_smooth": rmb_dx_smooth, "rmb_dy_smooth": rmb_dy_smooth,
      "spdx": _zero_small(spdx), "spdy": _zero_small(spdy), "spdz": _zero_small(spdz),
      "vx": _zero_small(vx), "vy": _zero_small(vy), "vz": _zero_small(vz),
      "cam_x": float(state.get("cam_x", 0.0)) + vx * dt,
      "cam_y": float(state.get("cam_y", 0.0)) + vy * dt,
      "cam_z": float(state.get("cam_z", 0.0)) + vz * dt,
      "look_applied": look_applied, "look_dx": look_dx, "look_dy": look_dy,
      "focus_mouse_look": bool(state.get("focus_mouse_look", false)),
      "fps_mouse_look": fps_mouse_look
   }
}

def Camera = "Camera"

fn init(list pos=[0,0,0], f64 yaw=0.0, f64 pitch=0.0) list {
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
fn update(list cam, f64 dt, any win, bool skip_look=false, bool skip_move=false) list {
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
fn update_win_size(list cam, f64 w, f64 h) list {
   "Cache window size into camera for fast access in update()."
   cam[20] = float(w)
   cam[21] = float(h)
   cam
}

@jit
fn apply(list cam, list gfx_cam) list {
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
fn get_yaw(list cam) f64 {
   "Returns the current yaw(horizontal rotation) in degrees."
   cam.get(6, 0.0)
}

@jit
fn get_pitch(list cam) f64 {
   "Returns the current pitch(vertical rotation) in degrees."
   cam.get(7, 0.0)
}

@jit
fn get_pos(list cam) list {
   "Returns the current camera position as [x, y, z]."
   [cam.get(0, 0.0), cam.get(1, 0.0), cam.get(2, 0.0)]
}

@jit
fn get_rot(list cam) list {
   "Returns the current camera rotation as [yaw, pitch]."
   [get_yaw(cam), get_pitch(cam)]
}

@jit
fn get_fov(list cam) f64 {
   "Returns the current camera field of view in degrees."
   cam.get(16, 45.0)
}

fn set_fov(list cam, f64 fov) list {
   "Sets the field of view for the camera(degrees)."
   mut v = fov + 0.0
   if(v < 15.0){ v = 15.0 }
   if(v > 120.0){ v = 120.0 }
   cam[16] = v
   cam
}

fn set_speed(list cam, f64 speed) list {
   "Sets the movement speed(acceleration) of the camera."
   cam[11] = speed
   cam
}

fn set_sens(list cam, f64 sens) list {
   "Sets the mouse look sensitivity."
   cam[10] = sens
   cam
}

fn set_smoothing(list cam, f64 rot_response=10.0, f64 move_smooth=10.0) list {
   "Sets camera rotation and movement smoothing values."
   cam[13] = rot_response
   cam[14] = move_smooth
   cam
}

fn set_pitch_limits(list cam, f64 min_v, f64 max_v) list {
   "Sets the vertical rotation limits for the camera."
   cam[22] = min_v + 0.0
   cam[23] = max_v + 0.0
   cam
}

fn reset_motion(list cam) list {
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

fn camera_init(list pos, f64 yaw, f64 pitch, f64 fovy=120.0) dict {
   "Initializes a dictionary camera for lightweight render demos."
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

fn camera_update(dict c) dict {
   "Updates a dictionary camera target from yaw/pitch degrees."
   def yaw = float(c.get("yaw", 0.0))
   def pitch = float(c.get("pitch", 0.0))
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   def fwd = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))
   def pos = vec3(c.get("pos", [0, 0, 0]))
   c["pos"] = pos
   c["target"] = add(pos, fwd)
   c
}

fn camera_vectors(dict c) list {
   "Returns [forward, right, up] vectors for a dictionary camera."
   def yaw = float(c.get("yaw", 0.0))
   def pitch = float(c.get("pitch", 0.0))
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   def fwd = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))
   def fwd_h = vec3(sin(ryaw), 0.0, -cos(ryaw))
   def up_global = vec3(c.get("up", [0, 1, 0]))
   def rgt = normalize(cross3(fwd_h, up_global))
   def up_local = cross3(rgt, fwd)
   [fwd, rgt, up_local]
}

fn camera_free_look(dict c, any win, bool locked=true) dict {
   "Handles centered mouse-look for a dictionary camera."
   if(!locked){ return c }
   def handle = win.get("handle")
   if(!handle){ return c }
   def ws = ui_window.size(win)
   def cx = float(ws.get(0)) * 0.5
   def cy = float(ws.get(1)) * 0.5
   def mpos = native.get_cursor_pos(handle)
   def mx = float(mpos.get(0))
   def my = float(mpos.get(1))
   native.set_cursor_pos(handle, cx, cy)
   def sens = float(c.get("sens", 0.15))
   mut yaw = float(c.get("yaw", 0.0))
   mut pitch = float(c.get("pitch", 0.0))
   yaw += (mx - cx) * sens
   pitch -= (my - cy) * sens
   if(pitch > 89.0){ pitch = 89.0 }
   if(pitch < -89.0){ pitch = -89.0 }
   c["yaw"] = yaw
   c["pitch"] = pitch
   camera_update(c)
}

fn camera_free_move(dict c, any win, f64 dt) dict {
   "Handles WASD/E/Q movement for a dictionary camera."
   def yaw = float(c.get("yaw", 0.0))
   def ryaw = yaw * PI / 180.0
   def fwd_h = vec3(sin(ryaw), 0.0, -cos(ryaw))
   def rgt_h = vec3(cos(ryaw), 0.0, sin(ryaw))
   def up_g = vec3(0.0, 1.0, 0.0)
   mut move = vec3(0, 0, 0)
   if(ui_window.key_down(win, KEY_W)){ move = add(move, fwd_h) }
   if(ui_window.key_down(win, KEY_S)){ move = sub(move, fwd_h) }
   if(ui_window.key_down(win, KEY_A)){ move = sub(move, rgt_h) }
   if(ui_window.key_down(win, KEY_D)){ move = add(move, rgt_h) }
   if(ui_window.key_down(win, KEY_E)){ move = add(move, up_g) }
   if(ui_window.key_down(win, KEY_Q)){ move = sub(move, up_g) }
   if(len2(move) > 0.000001){
      def nmove = normalize(move)
      def speed = float(c.get("speed", 400.0))
      mut pos = vec3(c.get("pos", [0, 0, 0]))
      pos = add(pos, scale(nmove, speed * dt))
      c["pos"] = pos
      camera_update(c)
   }
   c
}

#main {
   assert(env_float_range("NY_UI_CAMERA_MISSING", 0.0, 1.0, 0.5) == 0.5, "camera env range")
   def pose = fit_bounds_pose(0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 60.0, 1.0, 0.0, 0.0)
   assert(is_dict(pose) && pose.contains("x"), "camera fit pose")
   def fitted = {"fit_scale": 1.0, "fit_cam_x": 0.0, "fit_cam_y": 0.0, "fit_cam_z": 2.0, "fit_target_x": 0.0, "fit_target_y": 0.0, "fit_target_z": 0.0}
   assert(fit_camera_sane(fitted), "camera sane fit")
   def fit_st = fit_scene_state(fitted, {"win_w": 800.0, "win_h": 600.0})
   assert(is_dict(fit_st) && fit_st.contains("yaw") && to_str(fit_st.get("branch", "")).len > 0, "camera scene state")
   def sim = simulate_frame_state({"dt": 0.016, "key_w": true, "speed": 100.0})
   assert(float(sim.get("cam_z", 0.0)) < 0.0, "camera simulate")
   def c0 = camera_init([0.0, 0.0, 0.0], 0.0, 0.0, 90.0)
   assert(is_dict(c0) && camera_vectors(c0).len == 3, "camera helper")
   print("✓ std.os.ui.render.camera self-test passed")
}
