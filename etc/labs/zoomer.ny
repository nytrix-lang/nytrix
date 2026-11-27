#!/usr/bin/env ny

;; Keywords: ui zoom magnifier screen flashlight bubble color-picker wayland x11 example
;; Zoomer — screen magnifier for boomers inspired by https://github.com/tsoding/boomer
;; Wayland/X11 capture, inertial camera, liquid-glass flashlight, pixel color picker.
;; Run: ny zoomer.ny --help
;; Q/Esc quit; wheel/+/- zoom; left drag or arrows/HJKL pan; 0/middle recenter;
;; F flashlight; Ctrl+wheel/+/- radius; T filter (nearest default); C/P picker.
;; NYTRIX_ZOOMER_IMAGE=path overrides capture. --pattern uses a synthetic desktop.

use core
use core.math as math
use core.os (exit, msleep, env, file_read, file_write, file_exists, file_remove,
   temp_dir, run_capture, ticks)
use core.os.args as process_args
use core.os.fs (make_dir)
use core.str (split, strip, lower, upper, atof, atoi, find_last, startswith, to_hex, chr)
use core.math.parse.img as image
use core.os.ui.render as gfx
use core.os.ui.window as window
use core.os.ui.window.consts as keyc
use core.os.ui.assets.viewer as assets

;; RGBA8, clamp-to-edge, nearest/linear samplers.
def int FMT_RGBA = 37
def int WRAP_CLAMP = 33071
def int FILTER_NEAREST = 0
def int FILTER_LINEAR = 1

def f64 FL_INITIAL_DELTA_RADIUS = 250.0
def f64 FL_DELTA_RADIUS_DECELERATION = 10.0
def f64 FL_START_RADIUS = 200.0
def f64 VELOCITY_THRESHOLD = 1.0
def f64 LENS_ZOOM = 1.75
def f64 LENS_UV_SCALE = 1048576.0
def f64 LENS_CELL_PX = 9.0
def int LENS_MAX_CELLS = 48
def int LENS_MIN_CELLS = 16
def str CONFIG_DEFAULT_PATH = "/.config/zoomer/config"

;; Prefer native Wayland capture, then X11 tools.
def list CAPTURE_TOOLS = [
   ["grim", "-t", "png"],
   ["spectacle", "-b", "-n", "-o"],
   ["gnome-screenshot", "-f"],
   ["scrot", "-o"],
   ["maim"]
]

;; Config: the same keys, defaults and file syntax as the C original.

def list CONFIG_DEFAULTS = [
   ["min_scale", "0.01"],
   ["scroll_speed", "1.5"],
   ["drag_friction", "6.0"],
   ["scale_friction", "4.0"],
   ["scale_lerp_speed", "3.0"],
   ["camera_pan_amount", "200.0"],
   ["camera_position_lerp_speed", "3.0"],
   ["flashlight_lerp_speed", "8.0"],
   ["flashlight_disable_radius_multiplier", "2.0"],
   ["lerp_camera_recenter", "true"],
   ["camera_recenter_lerp_speed", "3.0"],
   ["blur_background", "true"],
   ["background_blur_radius", "10.0"],
   ["blur_outside_flashlight", "true"],
   ["outside_flashlight_blur_radius", "10.0"],
   ["hide_cursor_on_flashlight", "true"],
   ["bubble_mass", "1.0"],
   ["bubble_spring_k", "80.0"],
   ["bubble_damping", "8.0"],
   ["bubble_stretch_factor", "0.0001"],
   ["bubble_squeeze_factor", "0.5"],
   ["bubble_deform_smoothing", "8.0"],
   ["vertex_shader_path", "/etc/zoomer/vert.glsl"],
   ["fragment_shader_path", "/etc/zoomer/frag.glsl"]
]

mut dict cfg = dict(40)

fn cfg_defaults() any {
   cfg = dict(40)
   mut int i = 0
   while i < CONFIG_DEFAULTS.len {
      def list entry = CONFIG_DEFAULTS.get(i, [])
      def str key = to_str(entry.get(0, ""))
      def str value = to_str(entry.get(1, ""))
      cfg[key] = 0.0
      config_set(key, value)
      i += 1
   }
   0
}

fn cf(str k) f64 { float(cfg.get(k, 0.0)) }
fn cbool(str k) bool { cf(k) > 0.5 }

fn parse_bool(str v) bool {
   def str t = to_str(lower(strip(v)))
   t == "true" || t == "yes" || t == "on" || t == "1"
}

fn config_set(str key, str value) bool {
   def str k = to_str(lower(key))
   if !cfg.contains(k) || k == "vertex_shader_path" || k == "fragment_shader_path" { return false }
   if k == "lerp_camera_recenter" || k == "blur_background" ||
      k == "blur_outside_flashlight" || k == "hide_cursor_on_flashlight" {
      cfg[k] = parse_bool(value) ? 1.0 : 0.0
   } else { cfg[k] = atof(value) }
   true
}

fn config_load(str path) bool {
   cfg_defaults()
   if !file_exists(path) {
      eprint("Config file not found: " + path + ". Using defaults.")
      return false
   }
   mut bool loaded = false
   match file_read(path) {
      ok(text) -> {
         def list lines = split(to_str(text), "\n")
         mut int i = 0
         while i < lines.len {
            def list body = split(to_str(lines.get(i, "")), "#")
            def list pair = split(to_str(body.get(0, "")), "=")
            if pair.len >= 2 {
               def str key = strip(to_str(pair.get(0, "")))
               def str value = strip(to_str(pair.get(1, "")))
               if config_set(key, value) { loaded = true }
            }
            i += 1
         }
      }
      err(_) -> { eprint("Could not read config: " + path) }
   }
   loaded
}

fn config_text() str {
   mut str out = "# Zoomer configuration\n# Booleans: true/false, yes/no, on/off, 1/0\n"
   mut int i = 0
   while i < CONFIG_DEFAULTS.len {
      def list entry = CONFIG_DEFAULTS.get(i, [])
      out = out + to_str(entry.get(0, "")) + " = " + to_str(entry.get(1, "")) + "\n"
      i += 1
   }
   out
}

fn config_write(str path) bool {
   def int slash = find_last(path, "/")
   if slash > 0 { make_dir(sx_prefix(path, slash)) }
   match file_write(path, config_text()) {
      ok(_) -> { true }
      err(_) -> {
         eprint("Failed to create config file: " + path)
         false
      }
   }
}

fn sx_prefix(str s, int stop) str {
   mut str out = ""
   mut int i = 0
   while i < stop && i < s.len {
      out = out + chr(load8(s, i))
      i += 1
   }
   out
}

fn env_flag(str name) bool {
   def v = env(name)
   is_str(v) && parse_bool(to_str(v))
}

fn env_int(str name, int fallback) int {
   def v = env(name)
   if !is_str(v) { return fallback }
   def int n = atoi(strip(to_str(v)))
   n > 0 ? n : fallback
}

fn trace(str msg) any {
   if env_flag("NYTRIX_ZOOMER_TRACE") { print("[zoomer] " + msg) }
   0
}

fn default_config_path() str {
   def home = env("HOME")
   if is_str(home) && to_str(home).len > 0 { return to_str(home) + CONFIG_DEFAULT_PATH }
   CONFIG_DEFAULT_PATH
}

;; Camera: a straight port of camera.c.

mut f64 cam_scale = 1.0
mut f64 cam_target_scale = 1.0
mut f64 cam_x = 0.0
mut f64 cam_y = 0.0
mut f64 cam_tx = 0.0
mut f64 cam_ty = 0.0
mut f64 cam_vx = 0.0
mut f64 cam_vy = 0.0
mut f64 cam_delta_scale = 0.0
mut f64 cam_pivot_x = 0.0
mut f64 cam_pivot_y = 0.0

fn clamp(f64 v, f64 lo, f64 hi) f64 { float(math.min(float(math.max(v, lo)), hi)) }
fn clamp_i(int v, int lo, int hi) int { v < lo ? lo : (v > hi ? hi : v) }

fn camera_reset(bool smooth) any {
   cam_tx = 0.0
   cam_ty = 0.0
   cam_target_scale = 1.0
   cam_vx = 0.0
   cam_vy = 0.0
   cam_delta_scale = 0.0
   if !smooth || !cbool("lerp_camera_recenter") {
      cam_scale = 1.0
      cam_x = 0.0
      cam_y = 0.0
   }
   0
}

fn camera_zoom_pivot(f64 factor, f64 pivot_x, f64 pivot_y) any {
   cam_delta_scale = clamp(cam_delta_scale + factor * cf("scroll_speed"), -400.0, 400.0)
   cam_pivot_x = pivot_x
   cam_pivot_y = pivot_y
   0
}

fn camera_grab() any {
   cam_target_scale = cam_scale
   cam_delta_scale = 0.0
   cam_tx = cam_x
   cam_ty = cam_y
   cam_vx = 0.0
   cam_vy = 0.0
   0
}

fn camera_drag(f64 dx, f64 dy, f64 dt) any {
   def f64 scale = math.max(cam_scale, 0.0001)
   def f64 sx = dx / scale
   def f64 sy = dy / scale
   cam_x = cam_x - sx
   cam_y = cam_y - sy
   cam_tx = cam_x
   cam_ty = cam_y
   ;; Inertia is only for release, never for movement while held.
   def f64 vdt = math.max(dt, 0.004)
   def f64 blend = clamp(dt * 18.0, 0.15, 0.55)
   cam_vx = cam_vx + (clamp(-sx / vdt, -6000.0, 6000.0) - cam_vx) * blend
   cam_vy = cam_vy + (clamp(-sy / vdt, -6000.0, 6000.0) - cam_vy) * blend
   0
}

fn camera_pan(f64 dx, f64 dy) any {
   cam_tx = cam_tx + dx * cf("camera_pan_amount")
   cam_ty = cam_ty + dy * cf("camera_pan_amount")
   0
}

;; Keep the capture center reachable at every zoom.
fn clamp_camera() any {
   if shot_w <= 0 || shot_h <= 0 { return 0 }
   def f64 lim_x = float(shot_w) * 0.5
   def f64 lim_y = float(shot_h) * 0.5
   if (cam_x <= -lim_x && cam_vx < 0.0) || (cam_x >= lim_x && cam_vx > 0.0) { cam_vx = 0.0 }
   if (cam_y <= -lim_y && cam_vy < 0.0) || (cam_y >= lim_y && cam_vy > 0.0) { cam_vy = 0.0 }
   cam_x = clamp(cam_x, -lim_x, lim_x)
   cam_y = clamp(cam_y, -lim_y, lim_y)
   cam_tx = clamp(cam_tx, -lim_x, lim_x)
   cam_ty = clamp(cam_ty, -lim_y, lim_y)
   0
}

fn update_camera(f64 dt, bool dragging_now, f64 win_w, f64 win_h) any {
   ;; A held grab owns camera position and scale until release.
   if dragging_now {
      cam_target_scale = cam_scale
      cam_delta_scale = 0.0
      cam_tx = cam_x
      cam_ty = cam_y
      clamp_camera()
      return 0
   }
   ;; Lerp scale towards target (recenter animation).
   if float(math.abs(cam_target_scale - cam_scale)) > 0.001 {
      cam_scale = cam_scale + (cam_target_scale - cam_scale) * cf("scale_lerp_speed") * dt
   }
   ;; Manual zoom keeps the point under the wheel fixed.
   if float(math.abs(cam_delta_scale)) > 0.5 {
      def f64 hw = win_w * 0.5
      def f64 hh = win_h * 0.5
      def f64 p0x = (cam_pivot_x - hw) / cam_scale
      def f64 p0y = (cam_pivot_y - hh) / cam_scale
      cam_scale = math.max(cam_scale + cam_delta_scale * dt, cf("min_scale"))
      cam_target_scale = cam_scale
      def f64 p1x = (cam_pivot_x - hw) / cam_scale
      def f64 p1y = (cam_pivot_y - hh) / cam_scale
      cam_x = cam_x + (p0x - p1x)
      cam_y = cam_y + (p0y - p1y)
      cam_tx = cam_x
      cam_ty = cam_y
      cam_delta_scale = cam_delta_scale - cam_delta_scale * dt * cf("scale_friction")
   }
   if math.sqrt(cam_vx * cam_vx + cam_vy * cam_vy) > VELOCITY_THRESHOLD {
      cam_x = cam_x + cam_vx * dt
      cam_y = cam_y + cam_vy * dt
      cam_tx = cam_x
      cam_ty = cam_y
      def f64 decay = 1.0 - clamp(dt * cf("drag_friction"), 0.0, 1.0)
      cam_vx = cam_vx * decay
      cam_vy = cam_vy * decay
   } else {
      cam_vx = 0.0
      cam_vy = 0.0
   }
   def f64 lerp = clamp(cf("camera_position_lerp_speed") * dt, 0.0, 1.0)
   cam_x = cam_x + (cam_tx - cam_x) * lerp
   cam_y = cam_y + (cam_ty - cam_y) * lerp
   clamp_camera()
   0
}

;; Flashlight bubble: port of update_flashlight() with the C bubble physics.

mut bool fl_on = false
mut f64 fl_shadow = 0.0
mut f64 fl_radius = FL_START_RADIUS
mut f64 fl_target_radius = FL_START_RADIUS
mut f64 fl_delta_radius = 0.0
mut bool fl_animating = false
mut f64 fl_x = 0.0
mut f64 fl_y = 0.0
mut f64 fl_vx = 0.0
mut f64 fl_vy = 0.0
mut f64 fl_stretch_x = 0.0
mut f64 fl_stretch_y = 0.0
mut f64 fl_squeeze = 0.0

fn flashlight_reset(f64 cursor_x, f64 cursor_y) any {
   fl_on = false
   fl_shadow = 0.0
   fl_radius = FL_START_RADIUS
   fl_target_radius = FL_START_RADIUS
   fl_delta_radius = 0.0
   fl_animating = false
   fl_x = cursor_x
   fl_y = cursor_y
   fl_vx = 0.0
   fl_vy = 0.0
   fl_stretch_x = 0.0
   fl_stretch_y = 0.0
   fl_squeeze = 0.0
   0
}

fn flashlight_toggle(f64 win_w, f64 win_h) any {
   fl_on = !fl_on
   fl_animating = true
   if fl_on {
      fl_radius = float(math.max(win_w, win_h)) * 1.5
      fl_target_radius = FL_START_RADIUS
   } else {
      fl_target_radius = fl_target_radius * cf("flashlight_disable_radius_multiplier")
   }
   0
}

fn flashlight_nudge_radius(f64 amount) any {
   fl_delta_radius = fl_delta_radius + amount
   fl_animating = false
   0
}

fn update_flashlight(f64 dt, f64 cursor_x, f64 cursor_y) any {
   ;; Manual radius adjustment decelerates like the original.
   if fl_on && !fl_animating && float(math.abs(fl_delta_radius)) > 1.0 {
      fl_target_radius = float(math.max(50.0, fl_target_radius + fl_delta_radius * dt))
      fl_delta_radius = fl_delta_radius - fl_delta_radius * FL_DELTA_RADIUS_DECELERATION * dt
   }
   def f64 speed = cf("flashlight_lerp_speed")
   if fl_on {
      ;; Spring: F = -k*(p - target) - c*v, integrated with the frame dt.
      def f64 mass = math.max(0.05, cf("bubble_mass"))
      def f64 dx = fl_x - cursor_x
      def f64 dy = fl_y - cursor_y
      def f64 ax = (-dx * cf("bubble_spring_k") - fl_vx * cf("bubble_damping")) / mass
      def f64 ay = (-dy * cf("bubble_spring_k") - fl_vy * cf("bubble_damping")) / mass
      fl_vx = fl_vx + ax * dt
      fl_vy = fl_vy + ay * dt
      fl_x = fl_x + fl_vx * dt
      fl_y = fl_y + fl_vy * dt
      def f64 vel = math.sqrt(fl_vx * fl_vx + fl_vy * fl_vy)
      def f64 smooth = cf("bubble_deform_smoothing") * dt
      def f64 stretch = vel > 0.1 ? cf("bubble_stretch_factor") : 0.0
      fl_stretch_x = fl_stretch_x + (fl_vx * stretch - fl_stretch_x) * smooth
      fl_stretch_y = fl_stretch_y + (fl_vy * stretch - fl_stretch_y) * smooth
      fl_squeeze = fl_squeeze + (vel * stretch * cf("bubble_squeeze_factor") - fl_squeeze) * smooth
   } else {
      fl_x = cursor_x
      fl_y = cursor_y
      fl_vx = 0.0
      fl_vy = 0.0
      fl_stretch_x = 0.0
      fl_stretch_y = 0.0
      fl_squeeze = 0.0
   }
   fl_radius = fl_radius + (fl_target_radius - fl_radius) * speed * dt
   if fl_animating && float(math.abs(fl_target_radius - fl_radius)) < 1.0 {
      fl_radius = fl_target_radius
      fl_animating = false
   }
   def f64 target_shadow = fl_on ? 0.8 : 0.0
   fl_shadow = fl_shadow + (target_shadow - fl_shadow) * speed * dt
   0
}

;; CPU fallback: deformed bubble signed distance.
fn lens_sdf(f64 px, f64 py) f64 {
   def f64 radius = math.max(2.0, fl_radius * cam_scale)
   def f64 dx = px - fl_x
   def f64 dy = py - fl_y
   def f64 sl = math.sqrt(fl_stretch_x * fl_stretch_x + fl_stretch_y * fl_stretch_y)
   if sl < 0.01 { return math.sqrt(dx * dx + dy * dy) - radius }
   def f64 ux = fl_stretch_x / sl
   def f64 uy = fl_stretch_y / sl
   def f64 along = dx * ux + dy * uy
   def f64 perp = -dx * uy + dy * ux
   def f64 ra = clamp(radius * (1.0 + sl), radius * 0.5, radius * 2.0)
   def f64 rp = clamp(radius * (1.0 - fl_squeeze), radius * 0.3, radius * 1.5)
   def f64 m = math.min(ra, rp)
   def f64 qx = along / ra
   def f64 qy = perp / rp
   math.sqrt(qx * qx + qy * qy) * m - m
}

fn smoothstep(f64 e0, f64 e1, f64 x) f64 {
   if e1 <= e0 { return x < e0 ? 0.0 : 1.0 }
   def f64 t = clamp((x - e0) / (e1 - e0), 0.0, 1.0)
   t * t * (3.0 - 2.0 * t)
}

;; Lens warp tables

;; Fixed-point UVs and coverage, cached per mesh corner.
mut ptr lens_u = 0
mut ptr lens_v = 0
mut ptr lens_a = 0
mut int lens_n = 0
mut int lens_cap = 0
mut f64 lens_cell = 1.0
mut f64 lens_ox = 0.0
mut f64 lens_oy = 0.0
mut f64 lens_kx = 1.0e30
mut f64 lens_ky = 1.0e30
mut f64 lens_kr = -1.0
mut f64 lens_ksx = 1.0e30
mut f64 lens_ksy = 1.0e30
mut f64 lens_ksq = 1.0e30
mut f64 lens_kscale = -1.0
mut f64 lens_kcam_x = 1.0e30
mut f64 lens_kcam_y = 1.0e30
mut f64 lens_kw = -1.0
mut f64 lens_kh = -1.0

fn lens_zoom_at(f64 dist, f64 radius) f64 {
   def f64 t = clamp(dist / math.max(radius, 0.001), 0.0, 1.0)
   def f64 falloff = 1.0 - t * t
   1.0 + (LENS_ZOOM - 1.0) * falloff
}

fn lens_tables_dirty(f64 win_w, f64 win_h) bool {
   float(math.abs(fl_x - lens_kx)) > 0.01 || float(math.abs(fl_y - lens_ky)) > 0.01 ||
      float(math.abs(fl_radius - lens_kr)) > 0.05 ||
      float(math.abs(fl_stretch_x - lens_ksx)) > 0.0001 ||
      float(math.abs(fl_stretch_y - lens_ksy)) > 0.0001 ||
      float(math.abs(fl_squeeze - lens_ksq)) > 0.0001 ||
      float(math.abs(cam_scale - lens_kscale)) > 0.00001 ||
      float(math.abs(cam_x - lens_kcam_x)) > 0.00001 ||
      float(math.abs(cam_y - lens_kcam_y)) > 0.00001 ||
      float(math.abs(win_w - lens_kw)) > 0.01 || float(math.abs(win_h - lens_kh)) > 0.01
}

fn free_lens_tables() any {
   if lens_u != 0 { free(lens_u) lens_u = 0 }
   if lens_v != 0 { free(lens_v) lens_v = 0 }
   if lens_a != 0 { free(lens_a) lens_a = 0 }
   lens_cap = 0
   lens_n = 0
   lens_kx = 1.0e30
   0
}

fn lens_tables(f64 win_w, f64 win_h) bool {
   def f64 radius = math.max(2.0, fl_radius * cam_scale)
   def f64 ex = radius * (1.0 + float(math.abs(fl_stretch_x)))
   def f64 ey = radius * (1.0 + float(math.abs(fl_stretch_y)))
   def f64 half = math.max(ex, ey) * 1.15
   mut int n = int(half * 2.0 / LENS_CELL_PX)
   if n < LENS_MIN_CELLS { n = LENS_MIN_CELLS }
   if n > LENS_MAX_CELLS { n = LENS_MAX_CELLS }
   def f64 cell = half * 2.0 / float(n)
   def f64 ox = fl_x - half
   def f64 oy = fl_y - half
   def int count = (n + 1) * (n + 1)
   if count > lens_cap {
      free_lens_tables()
      def ptr p_u = malloc(count * 4)
      def ptr p_v = malloc(count * 4)
      def ptr p_a = malloc(count * 4)
      if p_u == 0 || p_v == 0 || p_a == 0 {
         if p_u != 0 { free(p_u) }
         if p_v != 0 { free(p_v) }
         if p_a != 0 { free(p_a) }
         return false
      }
      lens_u = p_u
      lens_v = p_v
      lens_a = p_a
      lens_cap = count
   }
   if lens_n == n && !lens_tables_dirty(win_w, win_h) { return true }
   lens_n = n
   lens_cell = cell
   lens_ox = ox
   lens_oy = oy
   def int stride = (n + 1) * 4
   mut int j = 0
   while j <= n {
      def f64 py = oy + float(j) * cell
      mut int i = 0
      while i <= n {
         def f64 px = ox + float(i) * cell
         def int idx = j * stride + i * 4
         def f64 sd = lens_sdf(px, py)
         def f64 cov = clamp(1.0 - smoothstep(-cell * 1.2, 0.0, sd), 0.0, 1.0)
         store8(lens_a, int(cov * 255.0), idx)
         def f64 dxs = px - fl_x
         def f64 dys = py - fl_y
         def f64 dist = math.sqrt(dxs * dxs + dys * dys)
         def f64 zoom = lens_zoom_at(dist, radius)
         store32(lens_u, int(clamp(uv_x(fl_x + dxs / zoom, win_w), 0.0, 1.0) * LENS_UV_SCALE), idx)
         store32(lens_v, int(clamp(uv_y(fl_y + dys / zoom, win_h), 0.0, 1.0) * LENS_UV_SCALE), idx)
         i += 1
      }
      j += 1
   }
   lens_kx = fl_x
   lens_ky = fl_y
   lens_kr = fl_radius
   lens_ksx = fl_stretch_x
   lens_ksy = fl_stretch_y
   lens_ksq = fl_squeeze
   lens_kscale = cam_scale
   lens_kcam_x = cam_x
   lens_kcam_y = cam_y
   lens_kw = win_w
   lens_kh = win_h
   true
}

;; Screen capture

mut ptr shot = 0
mut int shot_w = 0
mut int shot_h = 0
;; Keep both samplers resident: toggling never destroys an in-flight texture.
mut int shot_tex = 0
mut int shot_tex_nearest = 0
mut int shot_tex_linear = 0
mut int image_filter = FILTER_NEAREST
mut ptr shot_blur = 0
mut int blur_tex = 0
mut str shot_source = "none"

fn free_shot() any {
   if shot != 0 { free(shot) shot = 0 }
   if shot_blur != 0 { free(shot_blur) shot_blur = 0 }
   shot_w = 0
   shot_h = 0
   0
}

;; Normalize decoded RGB/RGBA bytes to owned RGBA8.
fn load_shot_file(str path) bool {
   def im = image.load(path)
   if !is_dict(im) { return false }
   def int iw = int(im.get("width", 0))
   def int ih = int(im.get("height", 0))
   if iw <= 0 || ih <= 0 { return false }
   def raw = im.get("data", 0)
   if !is_str(raw) { return false }
   def str bytes = to_str(raw)
   def int pixels = iw * ih
   if pixels <= 0 || bytes.len < pixels * 3 { return false }
   mut int bpp = 4
   if bytes.len < pixels * 4 { bpp = 3 }
   def ptr buf = malloc(pixels * 4)
   if buf == 0 { return false }
   mut int i = 0
   while i < pixels {
      def int si = i * bpp
      def int r = load8(bytes, si)
      def int g = load8(bytes, si + 1)
      def int b = load8(bytes, si + 2)
      def int a = bpp == 4 ? load8(bytes, si + 3) : 255
      store32(buf, r | (g << 8) | (b << 16) | (a << 24), i * 4)
      i += 1
   }
   free_shot()
   shot = buf
   shot_w = iw
   shot_h = ih
   true
}

fn capture_tool() bool {
   def str path = temp_dir() + "/nytrix-zoomer-shot.png"
   mut int i = 0
   while i < CAPTURE_TOOLS.len {
      def list tool = CAPTURE_TOOLS.get(i, [])
      def str name = to_str(tool.get(0, ""))
      mut list args = []
      mut int j = 1
      while j < tool.len {
         args = args.append(to_str(tool.get(j, "")))
         j += 1
      }
      args = args.append(path)
      if file_exists(path) { file_remove(path) }
      trace("capture tool: " + name)
      run_capture(name, args, nil, false)
      def bool captured = file_exists(path) && load_shot_file(path)
      if file_exists(path) { file_remove(path) }
      if captured {
         shot_source = name
         return true
      }
      i += 1
   }
   false
}

fn capture_screen() bool {
   def img_env = env("NYTRIX_ZOOMER_IMAGE")
   if is_str(img_env) {
      def str p = to_str(img_env)
      if p.len > 0 {
         if load_shot_file(p) {
            shot_source = p
            return true
         }
         eprint("zoomer: could not decode " + p + "; trying a screenshot tool")
      }
   }
   if capture_tool() { return true }
   eprint("zoomer: no screenshot tool worked (grim, spectacle, gnome-screenshot, scrot, maim); using the synthetic desktop")
   false
}

fn put_px(ptr buf, int w, int x, int y, int r, int g, int b) any {
   store32(buf, r | (g << 8) | (b << 16) | (255 << 24), (y * w + x) * 4)
}

fn lerp_i(int a, int b, f64 t) int { int(float(a) + (float(b) - float(a)) * t) }

;; Synthetic desktop for testing without screen capture.
fn capture_pattern() bool {
   def int w = 1280
   def int h = 720
   def ptr buf = malloc(w * h * 4)
   if buf == 0 { return false }
   mut int y = 0
   while y < h {
      def f64 v = float(y) / float(h)
      def int rr = lerp_i(24, 10, v)
      def int gg = lerp_i(30, 16, v)
      def int bb = lerp_i(52, 34, v)
      mut int x = 0
      while x < w {
         def f64 u = float(x) / float(w)
         def f64 glow = math.max(0.0, 1.0 - math.sqrt((u - 0.72) * (u - 0.72) + (v - 0.18) * (v - 0.18)) * 1.7)
         mut int r = int(float(rr) + glow * 120.0)
         mut int g = int(float(gg) + glow * 84.0)
         mut int b = int(float(bb) + glow * 40.0)
         if x % 80 == 0 || y % 80 == 0 {
            r = math.min(255, r + 26)
            g = math.min(255, g + 26)
            b = math.min(255, b + 26)
         }
         put_px(buf, w, x, y, r, g, b)
         x += 1
      }
      y += 1
   }
   ;; Two windows with title bars and body lines.
   draw_panel(buf, w, h, 90, 120, 420, 260, 28, 34, 46, 58, 66, 82)
   draw_panel(buf, w, h, 560, 300, 560, 320, 30, 28, 40, 240, 168, 96)
   ;; Dock swatches.
   mut int i = 0
   while i < 8 {
      def int dx = 40 + i * 64
      draw_rect_px(buf, w, h, dx, 640, 48, 48, 60 + i * 22, 200 - i * 16, 120 + i * 10)
      i += 1
   }
   free_shot()
   shot = buf
   shot_w = w
   shot_h = h
   shot_source = "pattern"
   true
}

fn draw_rect_px(ptr buf, int w, int h, int x0, int y0, int rw, int rh,
   int r, int g, int b) any {
   mut int y = y0
   while y < y0 + rh && y < h {
      if y >= 0 {
         mut int x = x0
         while x < x0 + rw && x < w {
            if x >= 0 { put_px(buf, w, x, y, r, g, b) }
            x += 1
         }
      }
      y += 1
   }
   0
}

fn draw_panel(ptr buf, int w, int h, int x0, int y0, int rw, int rh,
   int br, int bg, int bb, int ar, int ag, int ab) any {
   draw_rect_px(buf, w, h, x0, y0, rw, rh, br, bg, bb)
   draw_rect_px(buf, w, h, x0, y0, rw, 26, ar, ag, ab)
   draw_rect_px(buf, w, h, x0 + 12, y0 + 10, 6, 6, 250, 250, 250)
   draw_rect_px(buf, w, h, x0 + 24, y0 + 10, 6, 6, 250, 250, 250)
   mut int line = 0
   while line < 6 {
      def int ly = y0 + 52 + line * 30
      def int lw = 90 + (line * 53) % (rw - 140)
      draw_rect_px(buf, w, h, x0 + 24, ly, lw, 8, 96, 104, 120)
      line += 1
   }
   0
}

;; Precompute the static flashlight backdrop.
fn blur_pass(ptr src, ptr dst, int radius, int length, int count, int stride, int pitch) any {
   def int samples = radius * 2 + 1
   mut int row = 0
   while row < count {
      def int base = row * pitch
      mut int ar, ag, ab = 0, 0, 0
      mut int k = -radius
      while k <= radius {
         def int pos = base + clamp_i(k, 0, length - 1) * stride
         ar += load8(src, pos)
         ag += load8(src, pos + 1)
         ab += load8(src, pos + 2)
         k += 1
      }
      mut int x = 0
      while x < length {
         store32(dst, (ar / samples) | ((ag / samples) << 8) |
            ((ab / samples) << 16) | (255 << 24), base + x * stride)
         def int old = base + clamp_i(x - radius, 0, length - 1) * stride
         def int next = base + clamp_i(x + radius + 1, 0, length - 1) * stride
         ar += load8(src, next) - load8(src, old)
         ag += load8(src, next + 1) - load8(src, old + 1)
         ab += load8(src, next + 2) - load8(src, old + 2)
         x += 1
      }
      row += 1
   }
   0
}

fn build_blur(int radius) bool {
   if shot_blur != 0 { free(shot_blur) shot_blur = 0 }
   def int r = math.max(1, radius)
   def int n = shot_w * shot_h
   if n <= 0 || shot == 0 { return false }
   def ptr tmp = malloc(n * 4)
   def ptr out = malloc(n * 4)
   if tmp == 0 || out == 0 {
      if tmp != 0 { free(tmp) }
      if out != 0 { free(out) }
      return false
   }
   blur_pass(shot, tmp, r, shot_w, shot_h, 4, shot_w * 4)
   blur_pass(tmp, out, r, shot_h, shot_w, shot_w * 4, 4)
   free(tmp)
   shot_blur = out
   true
}

fn free_textures() any {
   shot_tex = 0
   if shot_tex_nearest > 0 { gfx.texture_destroy(shot_tex_nearest) shot_tex_nearest = 0 }
   if shot_tex_linear > 0 { gfx.texture_destroy(shot_tex_linear) shot_tex_linear = 0 }
   if blur_tex > 0 { gfx.texture_destroy(blur_tex) blur_tex = 0 }
   0
}

fn upload_texture(ptr pixels, int filter) int {
   if pixels == 0 || shot_w <= 0 || shot_h <= 0 { return 0 }
   gfx.texture_create_rgba(shot_w, shot_h, pixels, FMT_RGBA, filter, WRAP_CLAMP, WRAP_CLAMP, true)
}

fn upload_textures() any {
   free_textures()
   shot_tex_nearest = upload_texture(shot, FILTER_NEAREST)
   shot_tex_linear = upload_texture(shot, FILTER_LINEAR)
   blur_tex = upload_texture(shot_blur, FILTER_LINEAR)
   shot_tex = shot_tex_nearest
   image_filter = FILTER_NEAREST
   0
}

fn toggle_image_filter() any {
   def int next_filter = image_filter == FILTER_NEAREST ? FILTER_LINEAR : FILTER_NEAREST
   def int next_tex = next_filter == FILTER_NEAREST ? shot_tex_nearest : shot_tex_linear
   if next_tex <= 0 {
      eprint("zoomer: requested image filter is unavailable")
      return 0
   }
   image_filter = next_filter
   shot_tex = next_tex
   trace(image_filter == FILTER_NEAREST ? "filter nearest" : "filter linear")
   0
}

fn shot_pixel(int x, int y) int {
   if shot == 0 || shot_w <= 0 || shot_h <= 0 { return 0 }
   def int cx = clamp_i(x, 0, shot_w - 1)
   def int cy = clamp_i(y, 0, shot_h - 1)
   load32(shot, (cy * shot_w + cx) * 4) & 0xffffff
}

fn rgb_hex(int c) str {
   "#" + to_str(upper(to_hex((c >> 16) & 255, 2))) +
   to_str(upper(to_hex((c >> 8) & 255, 2))) +
   to_str(upper(to_hex(c & 255, 2)))
}

;; Drawing

mut int font_big = 0
mut int font_small = 0

fn uv_x(f64 px, f64 win_w) f64 {
   if shot_w <= 0 { return 0.0 }
   0.5 + ((px - win_w * 0.5) / cam_scale + cam_x) / float(shot_w)
}

fn uv_y(f64 py, f64 win_h) f64 {
   if shot_h <= 0 { return 0.0 }
   0.5 + ((py - win_h * 0.5) / cam_scale + cam_y) / float(shot_h)
}

;; GPU focus / liquid-glass shader (Vulkan)

def str FOCUS_VS = """#version 450
layout(location=1) in vec2 uv;
layout(location=0) out vec2 vUv;
void main() {
   vUv = uv;
   gl_Position = vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, 0.0, 1.0);
}
"""

def str FOCUS_FS = """#version 450
#extension GL_EXT_nonuniform_qualifier : enable
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D texSamplers[1024];
layout(push_constant) uniform ZoomerPC {
   layout(offset=64) vec4 win_shot;
   layout(offset=80) vec4 cam;
   layout(offset=96) vec4 fl_pos;
   layout(offset=112) vec4 fl_def;
} pc;

vec2 screenToUv(vec2 sp) {
   return vec2(0.5) + ((sp - pc.win_shot.xy * 0.5) / max(pc.cam.z, 0.0001) + pc.cam.xy) / max(pc.win_shot.zw, vec2(1.0));
}

vec3 sampleCapture(int tid, vec2 uv) {
   if (tid < 0 || tid >= 1024) { return vec3(0.043, 0.047, 0.063); }
   if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      vec2 pxOut = max(vec2(0.0), max(-uv, uv - vec2(1.0))) * pc.win_shot.zw * max(pc.cam.z, 0.0001);
      float edge = max(pxOut.x, pxOut.y);
      if (edge <= 1.25) { return vec3(0.18, 0.20, 0.25); }
      return vec3(0.043, 0.047, 0.063);
   }
   return texture(texSamplers[nonuniformEXT(uint(tid))], uv).rgb;
}

float bubbleSdf(vec2 sp, out vec2 localNorm) {
   float radius = max(2.0, pc.fl_pos.z);
   vec2 d = sp - pc.fl_pos.xy;
   vec2 st = pc.fl_def.xy;
   float sl = length(st);
   if (sl < 0.01) {
      float len = length(d);
      localNorm = len > 1e-4 ? d / len : vec2(0.0, -1.0);
      return len - radius;
   }
   vec2 u = st / sl;
   float along = dot(d, u);
   float perp = -d.x * u.y + d.y * u.x;
   float ra = clamp(radius * (1.0 + sl), radius * 0.5, radius * 2.0);
   float rp = clamp(radius * (1.0 - pc.fl_pos.w), radius * 0.3, radius * 1.5);
   float m = min(ra, rp);
   vec2 q = vec2(along / ra, perp / rp);
   float ql = length(q);
   vec2 gLocal = ql > 1e-4 ? vec2(q.x / ra, q.y / rp) / ql : vec2(0.0, -1.0);
   vec2 gWorld = u * gLocal.x + vec2(-u.y, u.x) * gLocal.y;
   float gl = length(gWorld);
   localNorm = gl > 1e-4 ? gWorld / gl : vec2(0.0, -1.0);
   return ql * m - m;
}

void main() {
   vec2 sp = vUv * pc.win_shot.xy;
   int shotTid = int(pc.fl_def.z + 0.5);
   int blurTid = int(pc.fl_def.w);
   float flShadow = pc.cam.w;

   vec2 baseUv = screenToUv(sp);
   if (flShadow <= 0.001) {
      outColor = vec4(sampleCapture(shotTid, baseUv), 1.0);
      return;
   }

   float dim = clamp(flShadow / 0.8, 0.0, 1.0);
   vec2 grad;
   float sd = bubbleSdf(sp, grad);
   float radius = max(2.0, pc.fl_pos.z);

   // Smooth blurred + deeply vignetted backdrop outside the glass.
   vec3 sharpBg = sampleCapture(shotTid, baseUv);
   vec3 blurBg = (blurTid >= 0 && blurTid < 1024) ? sampleCapture(blurTid, baseUv) : sharpBg;
   vec3 bgBase = mix(sharpBg, blurBg, smoothstep(0.05, 0.45, dim));
   float outerT = clamp(max(sd, 0.0) / max(radius * 1.15, 24.0), 0.0, 1.0);
   float vig = mix(0.28, 0.0, smoothstep(0.0, 1.0, outerT));
   float dropShadow = 1.0 - 0.55 * dim * exp(-max(sd, 0.0) / max(radius * 0.12, 8.0));
   vec3 outsideCol = bgBase * mix(1.0, vig, dim) * dropShadow;

   // Inside the optical liquid-glass lens.
   float mask = 1.0 - smoothstep(-1.1, 1.1, sd);
   if (mask <= 0.001) {
      outColor = vec4(outsideCol, 1.0);
      return;
   }

   // Bevel dome profile: flat/crystal-clear core with curved refractive rim.
   float rimWidth = clamp(radius * 0.24, 14.0, 64.0);
   float rimT = clamp(1.0 + sd / rimWidth, 0.0, 1.0); // 0 in core, 1 at rim
   float bevel = rimT * rimT;
   float domeSlope = bevel * 0.72;
   vec3 N = normalize(vec3(grad * domeSlope, sqrt(max(0.04, 1.0 - domeSlope * domeSlope))));

   // Radial magnification (1.75x center -> 1.0x rim) + physical Snell rim refraction.
   vec2 d = sp - pc.fl_pos.xy;
   float distNorm = clamp(length(d) / radius, 0.0, 1.0);
   float zoom = mix(1.0, 1.0 + 0.75 * (1.0 - distNorm * distNorm), dim);
   vec3 refr = refract(vec3(0.0, 0.0, -1.0), N, 1.0 / 1.45);
   vec2 refrPx = refr.xy * rimWidth * 0.65 * dim;
   vec2 glassSp = pc.fl_pos.xy + d / zoom + refrPx;

   // Subtle chromatic dispersion only along the curved glass bevel.
   float dispPx = bevel * 2.2 * dim;
   vec2 uvR = screenToUv(glassSp + grad * dispPx);
   vec2 uvG = screenToUv(glassSp);
   vec2 uvB = screenToUv(glassSp - grad * dispPx);
   vec3 glassCol = vec3(
      sampleCapture(shotTid, uvR).r,
      sampleCapture(shotTid, uvG).g,
      sampleCapture(shotTid, uvB).b
   );

   // Studio Fresnel rim highlight + soft top-left specular arc + crisp edge.
   float fresnel = pow(clamp(1.0 - N.z, 0.0, 1.0), 2.6);
   vec3 lightDir = normalize(vec3(-0.55, -0.72, 0.42));
   vec3 halfVec = normalize(lightDir + vec3(0.0, 0.0, 1.0));
   float spec = pow(max(dot(N, halfVec), 0.0), 28.0) * bevel;
   float rimLine = exp(-pow(abs(sd + 1.0) / 1.6, 2.0));
   float topArc = clamp(dot(-grad, normalize(vec2(0.55, 0.80))) * 0.5 + 0.5, 0.0, 1.0);

   glassCol += vec3(0.75, 0.86, 1.0) * fresnel * 0.14 * dim;
   glassCol += vec3(1.0, 1.0, 1.0) * spec * 0.48 * dim;
   glassCol += vec3(0.92, 0.96, 1.0) * rimLine * (0.14 + 0.26 * topArc * topArc) * dim;

   outColor = vec4(mix(outsideCol, clamp(glassCol, 0.0, 1.0), mask), 1.0);
}
"""

mut any focus_pipe = 0
mut ptr focus_pc = 0

fn init_focus_shader() bool {
   if bool(focus_pipe) && focus_pc != 0 { return true }
   if !gfx.vk_available() { return false }
   def v = gfx.compile_shader(FOCUS_VS, "vert")
   if !v { return false }
   def f = gfx.compile_shader(FOCUS_FS, "frag")
   if !f { return false }
   focus_pipe = gfx.create_pipeline(v, f, 3, 0, 0, 0)
   if !focus_pipe { return false }
   focus_pc = malloc(64)
   if !focus_pc { return false }
   true
}

fn free_focus_shader() any {
   if focus_pc != 0 { free(focus_pc) focus_pc = 0 }
   focus_pipe = 0
   0
}

fn draw_focus_gpu(f64 win_w, f64 win_h) bool {
   if !focus_pipe || !focus_pc || shot_tex <= 0 { return false }
   store32_f32(focus_pc, win_w, 0)
   store32_f32(focus_pc, win_h, 4)
   store32_f32(focus_pc, float(shot_w), 8)
   store32_f32(focus_pc, float(shot_h), 12)
   store32_f32(focus_pc, cam_x, 16)
   store32_f32(focus_pc, cam_y, 20)
   store32_f32(focus_pc, cam_scale, 24)
   store32_f32(focus_pc, fl_shadow, 28)
   store32_f32(focus_pc, fl_x, 32)
   store32_f32(focus_pc, fl_y, 36)
   store32_f32(focus_pc, math.max(2.0, fl_radius * cam_scale), 40)
   store32_f32(focus_pc, fl_squeeze, 44)
   store32_f32(focus_pc, fl_stretch_x, 48)
   store32_f32(focus_pc, fl_stretch_y, 52)
   store32_f32(focus_pc, float(shot_tex), 56)
   store32_f32(focus_pc, float(blur_tex > 0 ? blur_tex : -1), 60)
   gfx.draw_shader_rect(0.0, 0.0, win_w, win_h, focus_pipe, focus_pc, 64, 64)
}

fn draw_desktop(f64 win_w, f64 win_h) any {
   ;; Avoid flashing a blurred frame on a quick toggle.
   def int tex = (fl_on && fl_shadow > 0.15 && blur_tex > 0) ? blur_tex : shot_tex
   if tex <= 0 { return 0 }
   ;; Draw only the actual capture rectangle; outside it is the clear color.
   def f64 rw = float(shot_w) * cam_scale
   def f64 rh = float(shot_h) * cam_scale
   def f64 x = win_w * 0.5 - rw * 0.5 - cam_x * cam_scale
   def f64 y = win_h * 0.5 - rh * 0.5 - cam_y * cam_scale
   gfx.draw_rect_tex_uv(x, y, rw, rh, tex,
      0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0)
   0
}

fn draw_flashlight(f64 win_w, f64 win_h) any {
   if fl_shadow <= 0.01 { return 0 }
   def f64 radius = math.max(2.0, fl_radius * cam_scale)
   def f64 reach = float(math.max(win_w, win_h)) * 2.0
   ;; Fade to opaque black outside the fallback lens.
   def f64 dim = clamp(fl_shadow / 0.8, 0.0, 1.0)
   def int steps = 8
   def f64 band = 0.20
   mut int step = 0
   while step < steps {
      def f64 t0 = float(step) * band
      def f64 t1 = float(step + 1) * band
      def f64 c = float(step + 1) / float(steps)
      def f64 a = clamp(dim * (0.10 + 0.90 * c * c), 0.0, 1.0)
      gfx.draw_ring(fl_x, fl_y, radius * (1.0 + t0), radius * (1.0 + t1),
         [0.0, 0.0, 0.0, a], 128)
      step += 1
   }
   gfx.draw_ring(fl_x, fl_y, radius * (1.0 + float(steps) * band), radius + reach,
      [0.0, 0.0, 0.0, dim], 128)
   if !fl_on || shot_tex <= 0 { return 0 }

   ;; The glass: a seamless grid of cells sampling the magnified capture.
   if !lens_tables(win_w, win_h) { return 0 }
   if !lens_traced { trace("lens tables n=" + to_str(lens_n)) }
   def int n = lens_n
   def f64 cell = lens_cell
   def int stride = (n + 1) * 4
   mut int gy = 0
   while gy < n {
      def f64 y = lens_oy + float(gy) * cell
      mut int gx = 0
      while gx < n {
         def int i0 = gy * stride + gx * 4
         def int i1 = i0 + stride + 4
         def f64 alpha = (float(load8(lens_a, i0)) + float(load8(lens_a, i0 + 4)) +
            float(load8(lens_a, i0 + stride)) + float(load8(lens_a, i1))) / 1020.0
         if alpha > 0.02 {
            gfx.draw_rect_tex_uv(lens_ox + float(gx) * cell, y, cell + 0.75, cell + 0.75,
               shot_tex,
               float(load32(lens_u, i0)) / LENS_UV_SCALE,
               float(load32(lens_v, i0)) / LENS_UV_SCALE,
               float(load32(lens_u, i1)) / LENS_UV_SCALE,
               float(load32(lens_v, i1)) / LENS_UV_SCALE,
               1.0, 1.0, 1.0, alpha)
         }
         gx += 1
      }
      gy += 1
   }

   if !lens_traced { trace("lens grid drawn") }
   ;; Keep the magnified pixels clear; shade only the rim.
   gfx.draw_ring(fl_x, fl_y, radius * 0.992, radius, [1.0, 1.0, 1.0, 0.10 * dim], 160)
   gfx.draw_ring(fl_x, fl_y, radius, radius * 1.02, [0.0, 0.0, 0.0, 0.24 * dim], 160)
   if !lens_traced { trace("lens shaded") lens_traced = true }
   0
}

;; Color picker: a gpick-style loupe with a hex readout.

mut bool lens_traced = false
mut bool picker_on = false
mut int pick_r = 0
mut int pick_g = 0
mut int pick_b = 0
;; Picker coordinates are capture pixels, independent of camera zoom.
mut f64 pick_cx = 0.0
mut f64 pick_cy = 0.0
mut bool pick_has_point = false
mut f64 last_mouse_x = -1.0
mut f64 last_mouse_y = -1.0

fn pick_point_from_mouse(f64 cursor_x, f64 cursor_y, f64 win_w, f64 win_h) any {
   pick_cx = (cursor_x - win_w * 0.5) / cam_scale + cam_x + float(shot_w) * 0.5
   pick_cy = (cursor_y - win_h * 0.5) / cam_scale + cam_y + float(shot_h) * 0.5
   pick_has_point = true
   0
}

fn pick_screen_x(f64 win_w) f64 {
   (pick_cx - float(shot_w) * 0.5 - cam_x) * cam_scale + win_w * 0.5
}

fn pick_screen_y(f64 win_h) f64 {
   (pick_cy - float(shot_h) * 0.5 - cam_y) * cam_scale + win_h * 0.5
}

fn update_picker(f64 cursor_x, f64 cursor_y, f64 win_w, f64 win_h) any {
   if !picker_on || shot == 0 { return 0 }
   def bool moved = float(math.abs(cursor_x - last_mouse_x)) > 0.01 ||
      float(math.abs(cursor_y - last_mouse_y)) > 0.01
   last_mouse_x = cursor_x
   last_mouse_y = cursor_y
   if moved || !pick_has_point {
      pick_point_from_mouse(cursor_x, cursor_y, win_w, win_h)
   }
   pick_cx = clamp(pick_cx, 0.0, float(shot_w - 1))
   pick_cy = clamp(pick_cy, 0.0, float(shot_h - 1))
   def int c = shot_pixel(int(math.floor(pick_cx)), int(math.floor(pick_cy)))
   pick_r = c & 255
   pick_g = (c >> 8) & 255
   pick_b = (c >> 16) & 255
   0
}

fn draw_picker(f64 win_w, f64 win_h) any {
   if !picker_on { return 0 }
   def f64 cx = pick_screen_x(win_w)
   def f64 cy = pick_screen_y(win_h)
   def f64 cr = float(pick_r) / 255.0
   def f64 cg = float(pick_g) / 255.0
   def f64 cb = float(pick_b) / 255.0
   def int packed = pick_r | (pick_g << 8) | (pick_b << 16)

   ;; Crosshair follows keyboard nudges as well as mouse movement.
   gfx.draw_rect(0.0, cy, win_w, 1.0, [1.0, 1.0, 1.0, 0.60])
   gfx.draw_rect(cx, 0.0, 1.0, win_h, [1.0, 1.0, 1.0, 0.60])

   ;; Panel: loupe on the left, readout on the right.
   def f64 pw = 306.0
   def f64 ph = 116.0
   def f64 x0 = clamp(cx + 26.0, 10.0, math.max(10.0, win_w - pw - 10.0))
   def f64 y0 = clamp(cy + 26.0, 10.0, math.max(10.0, win_h - ph - 10.0))
   gfx.draw_rect(x0, y0, pw, ph, [0.05, 0.05, 0.06, 0.94])
   gfx.draw_rect_lines(x0, y0, pw, ph, [1.0, 1.0, 1.0, 0.16], 1.0)

   ;; Always sample the nearest texture for the loupe.
   def int span = 15
   def int half = 7
   def f64 lx = x0 + 8.0
   def f64 ly = y0 + 10.0
   def f64 ls = 96.0
   if shot_tex_nearest > 0 && shot_w > 0 && shot_h > 0 {
      def f64 u0 = (float(math.floor(pick_cx)) - float(half)) / float(shot_w)
      def f64 v0 = (float(math.floor(pick_cy)) - float(half)) / float(shot_h)
      def f64 u1 = u0 + float(span) / float(shot_w)
      def f64 v1 = v0 + float(span) / float(shot_h)
      gfx.draw_rect_tex_uv(lx, ly, ls, ls, shot_tex_nearest, u0, v0, u1, v1, 1.0, 1.0, 1.0, 1.0)
   }
   def f64 cell = ls / float(span)
   gfx.draw_rect_lines(lx + float(half) * cell, ly + float(half) * cell, cell, cell,
      [1.0, 1.0, 1.0, 0.95], 1.0)
   gfx.draw_rect_lines(lx, ly, ls, ls, [1.0, 1.0, 1.0, 0.30], 1.0)

   ;; Swatch and values.
   def f64 sx = x0 + 116.0
   gfx.draw_rect(sx, y0 + 10.0, 44.0, 44.0, [cr, cg, cb, 1.0])
   gfx.draw_rect_lines(sx, y0 + 10.0, 44.0, 44.0, [1.0, 1.0, 1.0, 0.35], 1.0)
   if font_big > 0 {
      gfx.draw_text(font_big, rgb_hex(packed), sx + 56.0, y0 + 14.0, [1.0, 1.0, 1.0, 1.0])
   }
   if font_small > 0 {
      gfx.draw_text(font_small, to_str(pick_r) + " " + to_str(pick_g) + " " + to_str(pick_b),
         sx + 56.0, y0 + 44.0, [1.0, 1.0, 1.0, 0.80])
      gfx.draw_text(font_small, to_str(int(pick_cx)) + "," + to_str(int(pick_cy)),
         sx + 56.0, y0 + 62.0, [1.0, 1.0, 1.0, 0.45])
      gfx.draw_text(font_small, "click to copy  arrows nudge", sx, y0 + 92.0,
         [1.0, 1.0, 1.0, 0.55])
   }
   0
}

;; Startup hint

def f64 HINT_HOLD = 3.0
def f64 HINT_FADE = 1.5
mut f64 hint_age = 0.0

fn hint_alpha(f64 age) f64 {
   if age <= HINT_HOLD { return 1.0 }
   1.0 - clamp((age - HINT_HOLD) / HINT_FADE, 0.0, 1.0)
}

fn hint_dismiss() any {
   if hint_age < HINT_HOLD { hint_age = HINT_HOLD }
   0
}

fn draw_hint(f64 win_w, f64 win_h, f64 alpha) any {
   if alpha <= 0.01 || picker_on { return 0 }
   def f64 pw = 432.0
   def f64 ph = 130.0
   def f64 x0 = float(math.floor((win_w - pw) * 0.5))
   def f64 y0 = float(math.max(12.0, win_h - ph - 34.0))
   gfx.draw_rect(x0, y0, pw, ph, [0.04, 0.04, 0.05, 0.74 * alpha])
   gfx.draw_rect_lines(x0, y0, pw, ph, [1.0, 1.0, 1.0, 0.14 * alpha], 1.0)
   if font_small > 0 {
      def list labels = ["wheel / + -   zoom in and out", "drag   pan      0   recenter",
         "f   flashlight bubble", "c   color picker      q   quit", "t   nearest / linear filter"]
      def list offsets = [14.0, 38.0, 62.0, 84.0, 106.0]
      mut int i = 0
      while i < labels.len {
         gfx.draw_text(font_small, to_str(labels.get(i, "")), x0 + 22.0,
            y0 + float(offsets.get(i, 0.0)), [1.0, 1.0, 1.0, 0.88 * alpha])
         i += 1
      }
   }
   0
}

;; Input

mut bool dragging = false
mut bool drag_poll_seen = false
mut bool drag_wait_up = false
mut f64 drag_prev_x = 0.0
mut f64 drag_prev_y = 0.0
mut bool running = true
mut bool quit_with_color = false
mut int last_frame_ticks = 0

fn drag_start(f64 mx, f64 my) any {
   dragging = true
   drag_poll_seen = false
   drag_wait_up = false
   drag_prev_x = mx
   drag_prev_y = my
   camera_grab()
   0
}

fn drag_stop() any {
   dragging = false
   drag_poll_seen = false
   drag_wait_up = true
   0
}

fn update_drag(any win, f64 mouse_x, f64 mouse_y, f64 dt) bool {
   def bool btn_down = window.mouse_down(win, 0)
   ;; Button events remain authoritative when polling is unavailable.
   if !btn_down { drag_wait_up = false }
   if picker_on {
      if dragging { drag_stop() }
   } elif dragging {
      if btn_down { drag_poll_seen = true }
      elif drag_poll_seen { drag_stop() }
   } elif btn_down && !drag_wait_up {
      drag_start(mouse_x, mouse_y)
      drag_poll_seen = true
   }
   def bool drag_now = dragging && !picker_on
   if drag_now {
      camera_drag(mouse_x - drag_prev_x, mouse_y - drag_prev_y, dt)
      drag_prev_x = mouse_x
      drag_prev_y = mouse_y
   }
   drag_now
}

fn picker_toggle(f64 mx, f64 my, f64 win_w, f64 win_h) any {
   picker_on = !picker_on
   drag_stop()
   camera_grab()
   if picker_on {
      if fl_on { flashlight_toggle(win_w, win_h) }
      pick_point_from_mouse(mx, my, win_w, win_h)
      last_mouse_x = mx
      last_mouse_y = my
   }
   0
}

;; Only fresh key presses toggle modes; repeats still pan/zoom.
fn handle_key(int key, int mod, bool fresh, f64 mx, f64 my, f64 win_w, f64 win_h) any {
   hint_dismiss()
   if key == keyc.KEY_Q || key == keyc.KEY_ESCAPE {
      running = false
      return 0
   }
   if key == keyc.KEY_T {
      if fresh { toggle_image_filter() }
      return 0
   }
   if key == keyc.KEY_0 {
      drag_stop()
      camera_reset(true)
      return 0
   }
   if key == keyc.KEY_F && !picker_on {
      if fresh { flashlight_toggle(win_w, win_h) }
      return 0
   }
   if key == keyc.KEY_C || key == keyc.KEY_P {
      if fresh { picker_toggle(mx, my, win_w, win_h) }
      return 0
   }
   def f64 dx = (key == keyc.KEY_H || key == keyc.KEY_LEFT) ? -1.0 : ((key == keyc.KEY_L || key == keyc.KEY_RIGHT) ? 1.0 : 0.0)
   def f64 dy = (key == keyc.KEY_K || key == keyc.KEY_UP) ? -1.0 : ((key == keyc.KEY_J || key == keyc.KEY_DOWN) ? 1.0 : 0.0)
   if dx != 0.0 || dy != 0.0 {
      if picker_on {
         pick_cx = pick_cx + dx
         pick_cy = pick_cy + dy
      } else { camera_pan(dx, dy) }
      return 0
   }
   def bool ctrl = (mod & keyc.MOD_CONTROL) != 0
   def f64 zoom = (key == keyc.KEY_EQUAL || key == keyc.KEY_KP_ADD) ? 1.0 : ((key == keyc.KEY_MINUS || key == keyc.KEY_KP_SUBTRACT) ? -1.0 : 0.0)
   if zoom != 0.0 {
      if ctrl && fl_on { flashlight_nudge_radius(zoom * FL_INITIAL_DELTA_RADIUS) }
      else { camera_zoom_pivot(zoom, mx, my) }
   }
   0
}

fn handle_event(any e, f64 mx, f64 my, f64 win_w, f64 win_h) any {
   def int typ = window.event_type(e)
   def any data = window.event_data(e)
   if typ == keyc.EVENT_KEY_PRESSED {
      handle_key(int(data.get("key", 0)), int(data.get("mod", 0)),
         int(data.get("action", 1)) != 2, mx, my, win_w, win_h)
      return 0
   }
   if typ == keyc.EVENT_MOUSE_SCROLL {
      hint_dismiss()
      def f64 dy = float(data.get("dy", 0.0))
      def bool ctrl = (int(data.get("mod", 0)) & keyc.MOD_CONTROL) != 0
      if dy != 0.0 {
         if ctrl && fl_on { flashlight_nudge_radius(dy * FL_INITIAL_DELTA_RADIUS * 0.5) }
         else { camera_zoom_pivot(dy > 0.0 ? 1.0 : -1.0, mx, my) }
      }
      return 0
   }
   if typ == keyc.EVENT_MOUSE_BUTTON_PRESSED {
      hint_dismiss()
      def int button = int(data.get("button", -1))
      if picker_on && button == 0 {
         update_picker(mx, my, win_w, win_h)
         print(rgb_hex(pick_r | (pick_g << 8) | (pick_b << 16)))
         quit_with_color = true
         running = false
      } elif button == 2 {
         drag_stop()
         camera_reset(true)
      } elif button == 0 && !picker_on {
         drag_start(mx, my)
      }
      return 0
   }
   if typ == keyc.EVENT_MOUSE_BUTTON_RELEASED {
      if int(data.get("button", -1)) == 0 { drag_stop() }
      return 0
   }
   if typ == keyc.EVENT_QUIT { running = false }
   0
}

;; Entry point

def list argv = process_args.args()
def bool want_help = process_args.flag_from(argv, "-h") || process_args.flag_from(argv, "--help")
def bool windowed = process_args.flag_from(argv, "-w") || process_args.flag_from(argv, "--windowed")
def bool start_pick = process_args.flag_from(argv, "-p") || process_args.flag_from(argv, "--pick")
def bool force_pattern = process_args.flag_from(argv, "--pattern")
mut str config_path_opt = process_args.value_from(argv, "--config", "")
if config_path_opt.len == 0 { config_path_opt = process_args.value_from(argv, "-c", "") }
mut f64 delay_sec = process_args.float_value_from(argv, "--delay", -1.0)
if delay_sec < 0.0 { delay_sec = process_args.float_value_from(argv, "-d", 0.0) }

if want_help {
   print("Usage: zoomer [OPTIONS]")
   print("  -d, --delay <seconds>   delay execution by <seconds>")
   print("  -h, --help              show this help")
   print("  -c, --config <filepath> use config at <filepath>")
   print("  -w, --windowed          windowed mode")
   print("  -p, --pick              start in color picker mode")
   print("      --new-config [path] generate default config")
   print("      --pattern           magnify a synthetic desktop")
   print("")
   print("Capture uses grim, spectacle, gnome-screenshot, scrot or maim.")
   print("NYTRIX_ZOOMER_IMAGE=path loads a screenshot you took yourself.")
   print("Left mouse drag pans; T switches nearest/linear (default nearest).")
   exit(0)
}

if process_args.flag_from(argv, "--new-config") {
   mut str out_path = default_config_path()
   mut int idx = 0
   mut int i = 0
   while i < argv.len {
      if to_str(argv.get(i, "")) == "--new-config" {
         idx = i + 1
      }
      i += 1
   }
   if idx > 0 && idx < argv.len {
      def str next = to_str(argv.get(idx, ""))
      if next.len > 0 && !startswith(next, "-") { out_path = next }
   }
   if config_write(out_path) { print("Generated config at " + out_path) }
   exit(0)
}

def str config_path = config_path_opt.len > 0 ? config_path_opt : default_config_path()
config_load(config_path)

if delay_sec > 0.0 {
   def int wait_ms = int(delay_sec * 1000.0)
   if wait_ms > 0 { msleep(wait_ms) }
}

;; Capture before the window is mapped, so the magnifier never captures itself.
trace("capture start")
mut bool captured = false
if !force_pattern { captured = capture_screen() }
if !captured { captured = capture_pattern() }
if !captured { panic("zoomer: could not obtain a screen image") }
trace("capture " + shot_source + " " + to_str(shot_w) + "x" + to_str(shot_h))

if cbool("blur_outside_flashlight") {
   build_blur(math.max(1, int(cf("outside_flashlight_blur_radius"))))
   trace("blur ready")
}

if env_flag("NY_UI_HEADLESS") { gfx.set_backend_type(gfx.BACKEND_MOCK) }
else {
   gfx.apply_backend_env()
   gfx.apply_backend_argv()
}

;; Start at capture size for a 1:1 fullscreen view.
def int capture_w = shot_w > 0 ? shot_w : 1600
def int capture_h = shot_h > 0 ? shot_h : 900
mut int init_w = capture_w
mut int init_h = capture_h
if windowed {
   init_w = int(math.min(float(capture_w), 1280.0))
   init_h = int(math.min(float(capture_h), 720.0))
}
;; Keep the overlay focused and above other windows.
def int win_flags = windowed ? (keyc.WINDOW_CENTER | keyc.WINDOW_FOCUS_ON_SHOW | keyc.WINDOW_FLOATING) : (keyc.WINDOW_FULLSCREEN | keyc.WINDOW_NO_BORDER | keyc.WINDOW_FOCUS_ON_SHOW | keyc.WINDOW_FLOATING)
trace("opening window " + to_str(init_w) + "x" + to_str(init_h))
mut any win = gfx.init_window(init_w, init_h, "zoomer", win_flags, true, false, 1)

if !win {
   eprint("zoomer: no window: " + gfx.window_error())
   free_shot()
   exit(1)
}

window.set_exit_key(win, keyc.KEY_NULL)

if cbool("hide_cursor_on_flashlight") || start_pick {
   window.set_cursor_mode(win, window.CURSOR_NORMAL)
}

trace("window ready on " + gfx.get_active_backend_name())
upload_textures()
def bool use_focus_gpu = init_focus_shader()
trace("textures " + to_str(shot_tex) + " " + to_str(blur_tex) + " gpu_focus=" + to_str(use_focus_gpu))
def bool headless = gfx.get_active_backend_name() == "mock"

if shot_tex <= 0 {
   eprint("zoomer: backend '" + gfx.get_active_backend_name() +
      "' cannot sample textures; the " + shot_source + " capture needs a GPU backend")
   free_focus_shader()
   free_textures()
   free_shot()
   gfx.close_window()
   exit(headless ? 0 : 1)
}

;; Log capture and actual texture size for diagnostics.
def list diag_tex = gfx.texture_size(shot_tex)
print("zoomer: " + shot_source + " " + to_str(shot_w) + "x" + to_str(shot_h) +
   " -> texture " + to_str(int(diag_tex.get(0, 0))) + "x" + to_str(int(diag_tex.get(1, 0))) +
   " on " + gfx.get_active_backend_name() + " (scale 1 is 1:1, wheel or +/- to zoom)")
font_big = gfx.font_load_first(assets.MONO_FONT_CANDIDATES, 22)
font_small = gfx.font_load_first(assets.MONO_FONT_CANDIDATES, 14)

if font_big > 0 { gfx.font_prepare(font_big, "0123456789ABCDEF#") }
if font_small > 0 { gfx.font_prepare(font_small, "abcdefghijklmnopqrstuvwxyz0123456789 ,-/+") }
flashlight_reset(float(init_w) * 0.5, float(init_h) * 0.5)
picker_on = start_pick

;; Optional scripted flashlight and frame dump.
if env_flag("NYTRIX_ZOOMER_FLASHLIGHT") {
   fl_on = true
   fl_shadow = 0.8
   fl_radius = FL_START_RADIUS
   fl_target_radius = FL_START_RADIUS
}
def str dump_path = is_str(env("NYTRIX_ZOOMER_DUMP")) ? to_str(env("NYTRIX_ZOOMER_DUMP")) : ""
def int max_frames = env_int("NYTRIX_ZOOMER_MAX_FRAMES", 0)
;; Request readback before submitting the frame.
mut bool dump_pending = dump_path.len > 0
def int startup = ticks()
last_frame_ticks = startup
mut int frames = 0
mut int surface_failures = 0
mut bool first_frame = true
mut f64 mouse_x = 0.0
mut f64 mouse_y = 0.0
mut f64 frame_w = float(init_w)
mut f64 frame_h = float(init_h)

while running && !gfx.window_should_close(win) {
   window.poll_events()
   def list mp = window.mouse_pos(win)
   mouse_x = float(mp.get(0, 0.0))
   mouse_y = float(mp.get(1, 0.0))
   mut any e = window.check_event(win)
   while !is_nil(e) && !(is_int(e) && int(e) == 0) {
      handle_event(e, mouse_x, mouse_y, frame_w, frame_h)
      e = window.check_event(win)
   }
   if !running { break }
   def list fb = gfx.framebuffer_size_f64(float(init_w), float(init_h))
   def f64 w = float(math.max(1.0, float(fb.get(0, init_w))))
   def f64 h = float(math.max(1.0, float(fb.get(1, init_h))))
   frame_w = w
   frame_h = h
   def int now = ticks()
   def f64 dt = clamp(float(now - last_frame_ticks) / 1000000000.0, 0.0, 0.05)
   last_frame_ticks = now

   def bool drag_now = update_drag(win, mouse_x, mouse_y, dt)

   hint_age = hint_age + dt
   update_camera(dt, drag_now, w, h)
   def f64 fl_target_x = (mouse_x <= 0.5 && mouse_y <= 0.5 && frames < 10) ? (w * 0.5) : mouse_x
   def f64 fl_target_y = (mouse_x <= 0.5 && mouse_y <= 0.5 && frames < 10) ? (h * 0.5) : mouse_y
   update_flashlight(dt, fl_target_x, fl_target_y)
   update_picker(mouse_x, mouse_y, w, h)

   if !gfx.begin_frame_clear([0.043, 0.047, 0.063, 1.0]) {
      surface_failures += 1
      if surface_failures >= 120 {
         eprint("zoomer: the render surface is unavailable")
         break
      }
      msleep(16)
      continue
   }
   surface_failures = 0
   gfx.set_ortho_2d(0.0, w, h, 0.0)
   if !use_focus_gpu || !draw_focus_gpu(w, h) {
      draw_desktop(w, h)
      draw_flashlight(w, h)
   }
   draw_picker(w, h)
   draw_hint(w, h, hint_alpha(hint_age))
   if dump_pending { gfx.request_frame_capture() }
   if !gfx.end_frame() {
      eprint("zoomer: presentation failed")
      surface_failures = 120
      break
   }
   if dump_pending && frames >= 3 {
      if gfx.snapshot(dump_path) { dump_pending = false }
      else if frames >= 8 { eprint("zoomer: could not write " + dump_path)  dump_pending = false }
   }
   if first_frame {
      if !window.show(win) { eprint("zoomer: window could not be shown") }
      ;; Maximize after mapping to fill tiling Wayland surfaces.
      if !windowed { _ = window.maximize(win) }
      def list wsz = window.size(win)
      def list wpos = window.pos(win)
      trace("shown visible=" + to_str(window.is_window_visible(win)) +
         " size=" + to_str(int(wsz.get(0, 0))) + "x" + to_str(int(wsz.get(1, 0))) +
         " pos=" + to_str(int(wpos.get(0, 0))) + "," + to_str(int(wpos.get(1, 0))))
      first_frame = false
   }
   frames += 1
   if max_frames > 0 && frames >= max_frames { break }
   if !windowed {
      def int budget = 16666666 - (ticks() - now)
      if budget > 0 { msleep((budget + 999999) / 1000000) }
   }
}

if font_big > 0 { gfx.font_destroy(font_big) }
if font_small > 0 { gfx.font_destroy(font_small) }
free_focus_shader()
free_shot()
free_lens_tables()
free_textures()
gfx.close_window()
exit(quit_with_color ? 0 : (surface_failures >= 120 ? 1 : 0))
