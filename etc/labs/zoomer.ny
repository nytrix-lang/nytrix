#!/usr/bin/env ny

;; Keywords: ui zoom magnifier screen flashlight bubble color-picker wayland x11 example
;; Zoomer - a screen magnifier in one Nytrix file.
;;
;; Port of https://github.com/laluxx/zoomer (MIT), the C port of tsoding's
;; boomer. One file, no shaders, no extra assets: the desktop is captured once,
;; decoded by core.math.parse.img, then drawn back with a camera (zoom, pan,
;; inertia), a liquid-glass flashlight bubble and a gpick-style color picker.
;;
;;   ny etc/labs/zoomer.ny [options]
;;
;;   -w, --windowed        windowed instead of borderless fullscreen
;;   -p, --pick            start in color picker mode
;;   -c, --config PATH     config file (default $HOME/.config/zoomer/config)
;;   -d, --delay SECONDS   wait before the capture
;;       --new-config [PATH]  write the default config and exit
;;       --pattern         magnify the synthetic desktop instead of the screen
;;       --selftest        run the camera/flashlight/config checks and exit
;;   -h, --help            print the usage above
;;
;; Keys: q/Esc quit; 0 or middle click recenter; f flashlight; wheel or +/- zoom;
;; Ctrl+wheel or Ctrl+=/- bubble radius; left drag pan; h/j/k/l or arrows pan;
;; c or p color picker (loupe, arrows nudge by one pixel, left click prints
;; #RRGGBB and exits).
;;
;; Capture picks the best tool the session offers (grim on wlroots, spectacle on
;; KDE, gnome-screenshot on GNOME, scrot/maim on X11), so it works on Wayland and
;; on X11/XWayland alike; the PNG is decoded in-process. NYTRIX_ZOOMER_IMAGE=path
;; loads a screenshot you took yourself, and a synthetic desktop is magnified
;; when no tool is available. The view starts 1:1: at scale 1 one captured pixel
;; is one screen pixel, and only zoom and pan change the image.
;;
;; The C original passes camera/flashlight state to a GLSL fragment shader.
;; Here the same maths runs on the host: the magnified desktop is one textured
;; quad, and the bubble is a warped mesh of small textured cells shaded with the
;; original SDF, refraction and Fresnel terms, so no shader source is needed.

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

;; Texture format 37 is RGBA8 on both render backends; 33071 clamps to edge so
;; zooming out never tiles the screenshot.
def int FMT_RGBA = 37
def int WRAP_CLAMP = 33071
def int FILTER_NEAREST = 0
def int FILTER_LINEAR = 1

def f64 FL_INITIAL_DELTA_RADIUS = 250.0
def f64 FL_DELTA_RADIUS_DECELERATION = 10.0
def f64 FL_START_RADIUS = 200.0
def f64 VELOCITY_THRESHOLD = 1.0
def f64 TAU = 6.283185307179586
def f64 LENS_ZOOM = 1.75
def f64 LENS_UV_SCALE = 1048576.0
def f64 LENS_CELL_PX = 9.0
def int LENS_MAX_CELLS = 48
def int LENS_MIN_CELLS = 16
def str CONFIG_DEFAULT_PATH = "/.config/zoomer/config"

;; Screenshot tools in preference order; the output path is appended last.
;; Wayland first (grim covers wlroots compositors, spectacle KDE,
;; gnome-screenshot GNOME), then the X11 grabbers used under XWayland.
def list CAPTURE_TOOLS = [
   ["grim", "-t", "png"],
   ["spectacle", "-b", "-n", "-o"],
   ["gnome-screenshot", "-f"],
   ["scrot", "-o"],
   ["maim"]
]

;; ---------------------------------------------------------------------------
;; Config: the same keys, defaults and file syntax as the C original.
;; ---------------------------------------------------------------------------

mut dict cfg = dict(40)

fn cfg_defaults() any {
   cfg = dict(40)
   cfg["min_scale"] = 0.01
   cfg["scroll_speed"] = 1.5
   cfg["drag_friction"] = 6.0
   cfg["scale_friction"] = 4.0
   cfg["scale_lerp_speed"] = 3.0
   cfg["camera_pan_amount"] = 200.0
   cfg["camera_position_lerp_speed"] = 3.0
   cfg["flashlight_lerp_speed"] = 8.0
   cfg["flashlight_disable_radius_multiplier"] = 2.0
   cfg["lerp_camera_recenter"] = 1.0
   cfg["camera_recenter_lerp_speed"] = 3.0
   cfg["blur_background"] = 1.0
   cfg["background_blur_radius"] = 10.0
   cfg["blur_outside_flashlight"] = 1.0
   cfg["outside_flashlight_blur_radius"] = 10.0
   cfg["hide_cursor_on_flashlight"] = 1.0
   cfg["bubble_mass"] = 1.0
   cfg["bubble_spring_k"] = 80.0
   cfg["bubble_damping"] = 8.0
   cfg["bubble_stretch_factor"] = 0.0001
   cfg["bubble_squeeze_factor"] = 0.5
   cfg["bubble_deform_smoothing"] = 8.0
   cfg["vertex_shader_path"] = 0.0
   cfg["fragment_shader_path"] = 0.0
   0
}

fn cf(str k) f64 { float(cfg.get(k, 0.0)) }
fn cbool(str k) bool { cf(k) > 0.5 }

fn parse_bool(str v) bool {
   def str t = to_str(lower(strip(v)))
   t == "true" || t == "yes" || t == "on" || t == "1"
}

fn config_set(str key, str value) bool {
   if !cfg.contains(key) { return false }
   def str k = to_str(lower(key))
   if k == "vertex_shader_path" || k == "fragment_shader_path" { return false }
   if k == "lerp_camera_recenter" || k == "blur_background" ||
      k == "blur_outside_flashlight" || k == "hide_cursor_on_flashlight" {
      cfg[key] = parse_bool(value) ? 1.0 : 0.0
      return true
   }
   cfg[key] = atof(value)
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
            def str raw = to_str(lines.get(i, ""))
            def list body = split(raw, "#")
            def str line = strip(to_str(body.get(0, "")))
            if line.len > 0 {
               def list pair = split(line, "=")
               if pair.len >= 2 {
                  def str key = strip(to_str(pair.get(0, "")))
                  mut str value = ""
                  mut int j = 1
                  while j < pair.len {
                     if j > 1 { value = value + "=" }
                     value = value + strip(to_str(pair.get(j, "")))
                     j += 1
                  }
                  if config_set(key, value) { loaded = true }
               }
            }
            i += 1
         }
      }
      err(_) -> { eprint("Could not read config: " + path) }
   }
   loaded
}

fn config_text() str {
   cfg_defaults()
   " \n" +
   "# Boolean values can be: true/false, yes/no, on/off, 1/0\n" +
   "\n" +
   "# Camera Settings\n" +
   "min_scale = " + to_str(cf("min_scale")) + "\n" +
   "scroll_speed = " + to_str(cf("scroll_speed")) + "\n" +
   "drag_friction = " + to_str(cf("drag_friction")) + "\n" +
   "scale_friction = " + to_str(cf("scale_friction")) + "\n" +
   "scale_lerp_speed = " + to_str(cf("scale_lerp_speed")) + "\n" +
   "camera_pan_amount = " + to_str(cf("camera_pan_amount")) + "\n" +
   "camera_position_lerp_speed = " + to_str(cf("camera_position_lerp_speed")) + "\n" +
   "\n" +
   "# Recenter Settings (0 key or middle mouse button)\n" +
   "lerp_camera_recenter = true\n" +
   "camera_recenter_lerp_speed = " + to_str(cf("camera_recenter_lerp_speed")) + "\n" +
   "\n" +
   "# Flashlight Settings (f key to toggle)\n" +
   "flashlight_lerp_speed = " + to_str(cf("flashlight_lerp_speed")) + "\n" +
   "flashlight_disable_radius_multiplier = " + to_str(cf("flashlight_disable_radius_multiplier")) + "\n" +
   "hide_cursor_on_flashlight = true\n" +
   "\n" +
   "# Blur Settings\n" +
   "blur_background = true\n" +
   "background_blur_radius = " + to_str(cf("background_blur_radius")) + "\n" +
   "blur_outside_flashlight = true\n" +
   "outside_flashlight_blur_radius = " + to_str(cf("outside_flashlight_blur_radius")) + "\n" +
   "\n" +
   "# Bubble Settings\n" +
   "bubble_mass = " + to_str(cf("bubble_mass")) + "\n" +
   "bubble_spring_k = " + to_str(cf("bubble_spring_k")) + "\n" +
   "bubble_damping = " + to_str(cf("bubble_damping")) + "\n" +
   "bubble_stretch_factor = " + to_str(cf("bubble_stretch_factor")) + " # How much velocity causes stretch\n" +
   "bubble_squeeze_factor = " + to_str(cf("bubble_squeeze_factor")) + " # How much perpendicular squeeze\n" +
   "bubble_deform_smoothing = " + to_str(cf("bubble_deform_smoothing")) + " # Smoothing for deformation recovery\n" +
   "\n" +
   "# Shader paths are accepted for compatibility; this port has no shaders.\n" +
   "vertex_shader_path = /etc/zoomer/vert.glsl\n" +
   "fragment_shader_path = /etc/zoomer/frag.glsl\n"
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
   if !is_str(v) { return false }
   def str t = to_str(lower(strip(to_str(v))))
   t == "1" || t == "true" || t == "yes" || t == "on"
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

;; ---------------------------------------------------------------------------
;; Camera: a straight port of camera.c.
;; ---------------------------------------------------------------------------

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

fn camera_reset(bool smooth) any {
   if smooth && cbool("lerp_camera_recenter") {
      cam_tx = 0.0
      cam_ty = 0.0
      cam_target_scale = 1.0
      cam_vx = 0.0
      cam_vy = 0.0
      cam_delta_scale = 0.0
      return 0
   }
   cam_scale = 1.0
   cam_target_scale = 1.0
   cam_delta_scale = 0.0
   cam_x = 0.0
   cam_y = 0.0
   cam_tx = 0.0
   cam_ty = 0.0
   cam_vx = 0.0
   cam_vy = 0.0
   0
}

fn camera_zoom_pivot(f64 factor, f64 pivot_x, f64 pivot_y) any {
   cam_delta_scale = clamp(cam_delta_scale + factor * cf("scroll_speed"), -400.0, 400.0)
   cam_pivot_x = pivot_x
   cam_pivot_y = pivot_y
   0
}

fn camera_pan(f64 dx, f64 dy) any {
   cam_tx = cam_tx + dx * cf("camera_pan_amount")
   cam_ty = cam_ty + dy * cf("camera_pan_amount")
   0
}

;; Keeps the capture covering the window: panning stops at the edge of the
;; screenshot instead of showing black outside it. When the capture is smaller
;; than the window at the current scale the limit is zero and it stays centred.
fn clamp_camera(f64 win_w, f64 win_h) any {
   if shot_w <= 0 || shot_h <= 0 { return 0 }
   def f64 s = math.max(cam_scale, 0.0001)
   def f64 lim_x = math.max(0.0, float(shot_w) * 0.5 - win_w / (2.0 * s))
   def f64 lim_y = math.max(0.0, float(shot_h) * 0.5 - win_h / (2.0 * s))
   cam_x = clamp(cam_x, -lim_x, lim_x)
   cam_y = clamp(cam_y, -lim_y, lim_y)
   cam_tx = clamp(cam_tx, -lim_x, lim_x)
   cam_ty = clamp(cam_ty, -lim_y, lim_y)
   0
}

fn update_camera(f64 dt, bool dragging, f64 win_w, f64 win_h) any {
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
   ;; Drag inertia.
   if !dragging && math.sqrt(cam_vx * cam_vx + cam_vy * cam_vy) > VELOCITY_THRESHOLD {
      cam_x = cam_x + cam_vx * dt
      cam_y = cam_y + cam_vy * dt
      cam_tx = cam_x
      cam_ty = cam_y
      cam_vx = cam_vx - cam_vx * dt * cf("drag_friction")
      cam_vy = cam_vy - cam_vy * dt * cf("drag_friction")
   }
   ;; Keyboard pan and recenter animation.
   def f64 lerp = cf("camera_position_lerp_speed") * dt
   cam_x = cam_x + (cam_tx - cam_x) * lerp
   cam_y = cam_y + (cam_ty - cam_y) * lerp
   clamp_camera(win_w, win_h)
   0
}

;; ---------------------------------------------------------------------------
;; Flashlight bubble: port of update_flashlight() with the C bubble physics.
;; ---------------------------------------------------------------------------

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
      if vel > 0.1 {
         def f64 nx = fl_vx / vel
         def f64 ny = fl_vy / vel
         def f64 stretch = vel * cf("bubble_stretch_factor")
         fl_stretch_x = fl_stretch_x + (nx * stretch - fl_stretch_x) * smooth
         fl_stretch_y = fl_stretch_y + (ny * stretch - fl_stretch_y) * smooth
         def f64 squeeze = stretch * cf("bubble_squeeze_factor")
         fl_squeeze = fl_squeeze + (squeeze - fl_squeeze) * smooth
      } else {
         fl_stretch_x = fl_stretch_x - fl_stretch_x * smooth
         fl_stretch_y = fl_stretch_y - fl_stretch_y * smooth
         fl_squeeze = fl_squeeze - fl_squeeze * smooth
      }
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

;; The bubble SDF, normal, height and refraction from frag.glsl, evaluated on
;; the host instead of per fragment.
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

fn lens_height(f64 sd, f64 thickness) f64 {
   if sd >= 0.0 { return 0.0 }
   if sd < -thickness { return thickness }
   def f64 x = thickness + sd
   math.sqrt(math.max(0.0, thickness * thickness - x * x))
}

fn smoothstep(f64 e0, f64 e1, f64 x) f64 {
   if e1 <= e0 { return x < e0 ? 0.0 : 1.0 }
   def f64 t = clamp((x - e0) / (e1 - e0), 0.0, 1.0)
   t * t * (3.0 - 2.0 * t)
}

;; GLSL refract(I, N, eta) with I = (0, 0, -1); returns [x, y, z] or [0,0,0].
fn refract_dir(f64 nx, f64 ny, f64 nz, f64 eta) list {
   def f64 k = 1.0 - eta * eta * (1.0 - nz * nz)
   if k < 0.0 { return [0.0, 0.0, 0.0] }
   def f64 s = eta * -nz + float(math.sqrt(k))
   [-s * nx, -s * ny, -eta - s * nz]
}

;; ---------------------------------------------------------------------------
;; Lens warp tables
;;
;; The bubble magnifies: a point p inside the glass shows the capture at
;; centre + (p - centre) / zoom(p), where zoom peaks at the centre and falls to
;; 1 at the rim. That radial falloff is the lens curvature, and it is what the
;; shader in the C original gets from per-fragment ray maths. The warp is
;; evaluated once per grid *corner*, so neighbouring cells share identical UVs
;; on their shared edge and the mosaic is seamless; evaluating at cell centres
;; instead (as a naive port does) shifts every tile by a different amount and
;; reads as low-resolution blocks. The tables are cached and only rebuilt when
;; the glass actually moves.
;; ---------------------------------------------------------------------------

;; Three corner tables, one 4-byte slot per grid corner: u and v as u32
;; fixed point (2^20) and the coverage fade as a u8. The byte helpers used by
;; the capture path are offset-exact, unlike the f64 store/load builtins.
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
   lens_n = n
   lens_cell = cell
   lens_ox = ox
   lens_oy = oy
   if !lens_tables_dirty(win_w, win_h) { return true }
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
   lens_kw = win_w
   lens_kh = win_h
   true
}

;; ---------------------------------------------------------------------------
;; Screen capture
;;
;; Wayland gives no portable framebuffer read (that needs the ScreenCast portal
;; and PipeWire), so the supported route is the session's own screenshot tool:
;; grim on wlroots compositors, spectacle on KDE, gnome-screenshot on GNOME, and
;; scrot/maim under X11 or XWayland. The tool writes a PNG, core.math.parse.img
;; decodes it in-process, and the RGBA8 result becomes the texture. The grab
;; happens once, like the C original.
;; ---------------------------------------------------------------------------

mut ptr shot = 0
mut int shot_w = 0
mut int shot_h = 0
mut int shot_tex = 0
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

;; Decodes any image core.math.parse.img understands into the RGBA8 capture.
;; The decoder reports RGBA8 in a byte string and leaves `bpp` at 0, so the
;; channel count comes from the buffer length (4 when alpha is present).
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
   mut int y = 0
   while y < ih {
      def int row = y * iw * bpp
      def int out_row = y * iw * 4
      mut int x = 0
      while x < iw {
         def int si = row + x * bpp
         def int r = load8(bytes, si)
         def int g = load8(bytes, si + 1)
         def int b = load8(bytes, si + 2)
         def int a = bpp >= 4 ? load8(bytes, si + 3) : 255
         store32(buf, r | (g << 8) | (b << 16) | (a << 24), out_row + x * 4)
         x += 1
      }
      y += 1
   }
   free_shot()
   shot = buf
   shot_w = iw
   shot_h = ih
   true
}

fn capture_tool() bool {
   def str path = temp_dir() + "/nytrix-zoomer-shot.png"
   def list tools = CAPTURE_TOOLS
   mut int i = 0
   while i < tools.len {
      def list tool = tools.get(i, 0)
      if is_list(tool) && tool.len > 0 {
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
         if file_exists(path) && load_shot_file(path) {
            shot_source = name
            file_remove(path)
            return true
         }
         if file_exists(path) { file_remove(path) }
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

;; A synthetic desktop: gradient wallpaper, a grid, a few "windows" with title
;; bars and a dock of saturated swatches. Enough colour and fine detail to judge
;; magnification when no screen can be captured.
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

;; Two box passes over the capture, used as the blurred background while the
;; flashlight is on (frag.glsl's gaussianBlur, precomputed because the capture
;; is static).
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
   mut int y = 0
   while y < shot_h {
      def int row = y * shot_w * 4
      mut int x = 0
      while x < shot_w {
         mut int ar, ag, ab = 0, 0, 0
         mut int count = 0
         mut int k = x - r
         while k <= x + r {
            mut int cx = k
            if cx < 0 { cx = 0 }
            if cx >= shot_w { cx = shot_w - 1 }
            def int o = row + cx * 4
            ar += load8(shot, o)
            ag += load8(shot, o + 1)
            ab += load8(shot, o + 2)
            count += 1
            k += 1
         }
         def int o2 = row + x * 4
         store32(tmp, (ar / count) | ((ag / count) << 8) | ((ab / count) << 16) | (255 << 24), o2)
         x += 1
      }
      y += 1
   }
   mut int xx = 0
   while xx < shot_w {
      mut int yy = 0
      while yy < shot_h {
         mut int ar, ag, ab = 0, 0, 0
         mut int count = 0
         mut int k = yy - r
         while k <= yy + r {
            mut int cy = k
            if cy < 0 { cy = 0 }
            if cy >= shot_h { cy = shot_h - 1 }
            def int o = (cy * shot_w + xx) * 4
            ar += load8(tmp, o)
            ag += load8(tmp, o + 1)
            ab += load8(tmp, o + 2)
            count += 1
            k += 1
         }
         def int o2 = (yy * shot_w + xx) * 4
         store32(out, (ar / count) | ((ag / count) << 8) | ((ab / count) << 16) | (255 << 24), o2)
         yy += 1
      }
      xx += 1
   }
   free(tmp)
   shot_blur = out
   true
}

fn upload_textures() any {
   if shot_tex > 0 { gfx.texture_destroy(shot_tex) shot_tex = 0 }
   if blur_tex > 0 { gfx.texture_destroy(blur_tex) blur_tex = 0 }
   if shot != 0 && shot_w > 0 && shot_h > 0 {
      shot_tex = gfx.texture_create_rgba(shot_w, shot_h, shot, FMT_RGBA,
         FILTER_LINEAR, WRAP_CLAMP, WRAP_CLAMP, true)
   }
   if shot_blur != 0 && shot_w > 0 && shot_h > 0 {
      blur_tex = gfx.texture_create_rgba(shot_w, shot_h, shot_blur, FMT_RGBA,
         FILTER_LINEAR, WRAP_CLAMP, WRAP_CLAMP, true)
   }
   0
}

fn shot_pixel(int x, int y) int {
   if shot == 0 || shot_w <= 0 { return 0 }
   mut int cx = x
   mut int cy = y
   if cx < 0 { cx = 0 }
   if cy < 0 { cy = 0 }
   if cx >= shot_w { cx = shot_w - 1 }
   if cy >= shot_h { cy = shot_h - 1 }
   def int px = load32(shot, (cy * shot_w + cx) * 4)
   (px & 255) | ((px >> 8) & 255) << 8 | ((px >> 16) & 255) << 16
}

fn rgb_hex(int c) str {
   "#" + to_str(upper(to_hex((c >> 16) & 255, 2))) +
   to_str(upper(to_hex((c >> 8) & 255, 2))) +
   to_str(upper(to_hex(c & 255, 2)))
}

;; ---------------------------------------------------------------------------
;; Drawing
;; ---------------------------------------------------------------------------

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

fn draw_desktop(f64 win_w, f64 win_h) any {
   ;; Only swap in the blurred capture once the bubble has actually faded in, so
   ;; a quick toggle never drops a blurry frame over the desktop.
   def int tex = (fl_on && fl_shadow > 0.15 && blur_tex > 0) ? blur_tex : shot_tex
   if tex <= 0 { return 0 }
   gfx.draw_rect_tex_uv(0.0, 0.0, win_w, win_h, tex,
      uv_x(0.0, win_w), uv_y(0.0, win_h),
      uv_x(win_w, win_w), uv_y(win_h, win_h),
      1.0, 1.0, 1.0, 1.0)
   0
}

fn draw_flashlight(f64 win_w, f64 win_h) any {
   if fl_shadow <= 0.01 { return 0 }
   def f64 radius = math.max(2.0, fl_radius * cam_scale)
   def f64 reach = float(math.max(win_w, win_h)) * 2.0
   ;; Everything outside the bubble fades to *pure* black (AMOLED), not to dark
   ;; grey. fl_shadow only ever reaches 0.8, so scaling by it would leave 20% of
   ;; the blurred capture visible; `dim` normalises it to a true 0..1 fade and
   ;; the far field is fully opaque, so the corners are #000000.
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
   ;; Vector glass shading, independent of the grid resolution: a cool tint, a
   ;; bright rim, a dark edge just outside it, and a soft specular blob.
   ;; Nothing is drawn over the magnified capture itself: any tint or specular
   ;; blob inside the glass adds white to the image and reads as a washed-out
   ;; screenshot. The glass is only shaded on and just outside the rim.
   gfx.draw_ring(fl_x, fl_y, radius * 0.992, radius, [1.0, 1.0, 1.0, 0.10 * dim], 160)
   gfx.draw_ring(fl_x, fl_y, radius, radius * 1.02, [0.0, 0.0, 0.0, 0.24 * dim], 160)
   if !lens_traced { trace("lens shaded") lens_traced = true }
   0
}

;; ---------------------------------------------------------------------------
;; Color picker: a gpick-style loupe with a hex readout.
;; ---------------------------------------------------------------------------

mut bool lens_traced = false
mut bool picker_on = false
mut int pick_r = 0
mut int pick_g = 0
mut int pick_b = 0
;; The picked point lives in capture pixels; the mouse sets it, the arrow keys
;; nudge it by one pixel so a colour can be picked without pixel-perfect aim.
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
   if pick_cx < 0.0 { pick_cx = 0.0 }
   if pick_cy < 0.0 { pick_cy = 0.0 }
   if pick_cx > float(shot_w - 1) { pick_cx = float(shot_w - 1) }
   if pick_cy > float(shot_h - 1) { pick_cy = float(shot_h - 1) }
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

   ;; Crosshair through the picked pixel, not through the pointer, so the
   ;; keyboard nudge stays visible.
   gfx.draw_rect(0.0, cy, win_w, 1.0, [1.0, 1.0, 1.0, 0.60])
   gfx.draw_rect(cx, 0.0, 1.0, win_h, [1.0, 1.0, 1.0, 0.60])

   ;; Panel: loupe on the left, readout on the right.
   def f64 pw = 306.0
   def f64 ph = 116.0
   def f64 x0 = clamp(cx + 26.0, 10.0, math.max(10.0, win_w - pw - 10.0))
   def f64 y0 = clamp(cy + 26.0, 10.0, math.max(10.0, win_h - ph - 10.0))
   gfx.draw_rect(x0, y0, pw, ph, [0.05, 0.05, 0.06, 0.94])
   gfx.draw_rect_lines(x0, y0, pw, ph, [1.0, 1.0, 1.0, 0.16], 1.0)

   ;; Loupe: magnification of the 15x15 pixels around the picked one, sampling
   ;; the capture texture (FILTER_LINEAR) with a single textured quad.
   def int span = 15
   def int half = 7
   def f64 lx = x0 + 8.0
   def f64 ly = y0 + 10.0
   def f64 ls = 96.0
   if shot_tex > 0 && shot_w > 0 && shot_h > 0 {
      def f64 u0 = (pick_cx - 0.5) / float(shot_w) - float(half) / float(shot_w)
      def f64 v0 = (pick_cy - 0.5) / float(shot_h) - float(half) / float(shot_h)
      def f64 u1 = u0 + float(span) / float(shot_w)
      def f64 v1 = v0 + float(span) / float(shot_h)
      gfx.draw_rect_tex_uv(lx, ly, ls, ls, shot_tex, u0, v0, u1, v1, 1.0, 1.0, 1.0, 1.0)
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

;; ---------------------------------------------------------------------------
;; Startup hint
;;
;; The controls are listed for the first few seconds and then fade, or fade as
;; soon as the user does anything, so the screen comes back to a clean 1:1 view.
;; ---------------------------------------------------------------------------

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
   def f64 ph = 106.0
   def f64 x0 = float(math.floor((win_w - pw) * 0.5))
   def f64 y0 = float(math.max(12.0, win_h - ph - 34.0))
   gfx.draw_rect(x0, y0, pw, ph, [0.04, 0.04, 0.05, 0.74 * alpha])
   gfx.draw_rect_lines(x0, y0, pw, ph, [1.0, 1.0, 1.0, 0.14 * alpha], 1.0)
   if font_small > 0 {
      def f64 a = 0.88 * alpha
      gfx.draw_text(font_small, "wheel / + -   zoom in and out", x0 + 22.0, y0 + 14.0,
         [1.0, 1.0, 1.0, a])
      gfx.draw_text(font_small, "drag   pan      0   recenter", x0 + 22.0, y0 + 38.0,
         [1.0, 1.0, 1.0, a])
      gfx.draw_text(font_small, "f   flashlight bubble", x0 + 22.0, y0 + 62.0,
         [1.0, 1.0, 1.0, a])
      gfx.draw_text(font_small, "c   color picker      q   quit", x0 + 22.0, y0 + 84.0,
         [1.0, 1.0, 1.0, a])
   }
   0
}

;; ---------------------------------------------------------------------------
;; Input
;; ---------------------------------------------------------------------------

mut bool dragging = false
mut f64 drag_prev_x = 0.0
mut f64 drag_prev_y = 0.0
mut bool running = true
mut bool quit_with_color = false
mut int last_frame_ticks = 0

fn picker_toggle(f64 mx, f64 my, f64 win_w, f64 win_h) any {
   picker_on = !picker_on
   if picker_on {
      if fl_on {
         fl_on = false
         fl_animating = true
         fl_target_radius = fl_target_radius * cf("flashlight_disable_radius_multiplier")
      }
      pick_point_from_mouse(mx, my, win_w, win_h)
      last_mouse_x = mx
      last_mouse_y = my
   }
   0
}

;; `fresh` is false for backend key repeats (action == 2). Toggles must only
;; fire on a real press: a held f used to flip the flashlight dozens of times a
;; second, so the dim never settled (wallpaper stayed visible) and the desktop
;; kept switching to the blurred capture.
fn handle_key(int key, int mod, bool fresh, f64 mx, f64 my, f64 win_w, f64 win_h) any {
   hint_dismiss()
   if key == keyc.KEY_Q || key == keyc.KEY_ESCAPE {
      running = false
      return 0
   }
   if key == keyc.KEY_0 {
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
   ;; In picker mode the arrows nudge the picked pixel instead of panning.
   if picker_on {
      if key == keyc.KEY_LEFT || key == keyc.KEY_H { pick_cx = pick_cx - 1.0 }
      if key == keyc.KEY_RIGHT || key == keyc.KEY_L { pick_cx = pick_cx + 1.0 }
      if key == keyc.KEY_UP || key == keyc.KEY_K { pick_cy = pick_cy - 1.0 }
      if key == keyc.KEY_DOWN || key == keyc.KEY_J { pick_cy = pick_cy + 1.0 }
      if key == keyc.KEY_LEFT || key == keyc.KEY_RIGHT || key == keyc.KEY_UP ||
         key == keyc.KEY_DOWN || key == keyc.KEY_H || key == keyc.KEY_J ||
         key == keyc.KEY_K || key == keyc.KEY_L {
         return 0
      }
   }
   def bool ctrl = (mod & keyc.MOD_CONTROL) != 0
   if key == keyc.KEY_EQUAL || key == keyc.KEY_KP_ADD {
      if ctrl && fl_on { flashlight_nudge_radius(FL_INITIAL_DELTA_RADIUS) }
      else { camera_zoom_pivot(1.0, mx, my) }
      return 0
   }
   if key == keyc.KEY_MINUS || key == keyc.KEY_KP_SUBTRACT {
      if ctrl && fl_on { flashlight_nudge_radius(-FL_INITIAL_DELTA_RADIUS) }
      else { camera_zoom_pivot(-1.0, mx, my) }
      return 0
   }
   if key == keyc.KEY_H || key == keyc.KEY_LEFT { camera_pan(-1.0, 0.0) }
   if key == keyc.KEY_J || key == keyc.KEY_DOWN { camera_pan(0.0, 1.0) }
   if key == keyc.KEY_K || key == keyc.KEY_UP { camera_pan(0.0, -1.0) }
   if key == keyc.KEY_L || key == keyc.KEY_RIGHT { camera_pan(1.0, 0.0) }
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
         print(rgb_hex(pick_r | (pick_g << 8) | (pick_b << 16)))
         quit_with_color = true
         running = false
      } elif button == 2 {
         camera_reset(true)
      } elif button == 0 && !picker_on {
         dragging = true
         drag_prev_x = mx
         drag_prev_y = my
         cam_vx = 0.0
         cam_vy = 0.0
      }
      return 0
   }
   if typ == keyc.EVENT_MOUSE_BUTTON_RELEASED {
      if int(data.get("button", -1)) == 0 { dragging = false }
      return 0
   }
   if typ == keyc.EVENT_QUIT { running = false }
   0
}

;; ---------------------------------------------------------------------------
;; Selftest: the ported maths, config parser and capture helpers.
;; ---------------------------------------------------------------------------

fn check(bool ok, str label) any {
   if !ok {
      eprint("ZOOMER_FAIL " + label)
      exit(1)
   }
   0
}

fn near(f64 a, f64 b, f64 eps) bool { float(math.abs(a - b)) <= eps }

fn selftest() any {
   cfg_defaults()
   check(near(cf("min_scale"), 0.01, 0.0001), "default min_scale")
   check(near(cf("bubble_spring_k"), 80.0, 0.0001), "default spring")
   check(cbool("lerp_camera_recenter"), "default recenter")
   check(config_set("scroll_speed", "2.5"), "set scroll_speed")
   check(near(cf("scroll_speed"), 2.5, 0.0001), "parsed scroll_speed")
   check(config_set("blur_outside_flashlight", "off"), "set bool")
   check(!cbool("blur_outside_flashlight"), "parsed bool off")
   check(!config_set("not_a_key", "1"), "unknown key rejected")

   ;; Camera: zooming at the window centre keeps the centre fixed.
   cfg_defaults()
   camera_reset(false)
   cam_pivot_x = 100.0
   cam_pivot_y = 50.0
   cam_delta_scale = 0.0
   def f64 before_x = cam_x
   camera_zoom_pivot(1.0, 100.0, 50.0)
   mut int i = 0
   while i < 120 { update_camera(1.0 / 60.0, false, 200.0, 100.0) i += 1 }
   check(cam_scale > 1.05, "wheel zoom grows the scale")
   check(near(cam_x, before_x, 0.0001), "centre pivot keeps the camera x")
   check(near(cam_y, 0.0, 0.0001), "centre pivot keeps the camera y")

   ;; Camera: zooming at a corner moves the camera so that corner stays put.
   camera_reset(false)
   camera_zoom_pivot(1.0, 0.0, 0.0)
   i = 0
   while i < 60 { update_camera(1.0 / 60.0, false, 200.0, 100.0) i += 1 }
   check(cam_scale > 1.0, "corner zoom grows the scale")
   check(cam_x < -0.001 && cam_y < -0.001, "corner zoom shifts the camera")

   ;; Camera: drag inertia decays and stops.
   camera_reset(false)
   cam_vx = 600.0
   cam_vy = -300.0
   i = 0
   while i < 240 { update_camera(1.0 / 60.0, false, 200.0, 100.0) i += 1 }
   check(float(math.abs(cam_vx)) < 1.0 && float(math.abs(cam_vy)) < 1.0, "drag inertia decays")
   check(cam_x > 1.0 && cam_y < -1.0, "drag inertia moves the camera")

   ;; Camera: recenter lerps back to the origin.
   camera_reset(false)
   cam_x = 400.0
   cam_y = -200.0
   cam_tx = 400.0
   cam_ty = -200.0
   camera_reset(true)
   i = 0
   while i < 600 { update_camera(1.0 / 60.0, false, 200.0, 100.0) i += 1 }
   check(near(cam_x, 0.0, 0.5) && near(cam_y, 0.0, 0.5), "recenter returns to the origin")
   check(near(cam_scale, 1.0, 0.01), "recenter restores the scale")

   ;; Flashlight: the spring settles on the cursor and the shadow fades in.
   flashlight_reset(0.0, 0.0)
   fl_on = true
   i = 0
   while i < 600 { update_flashlight(1.0 / 60.0, 320.0, 180.0) i += 1 }
   check(near(fl_x, 320.0, 0.5) && near(fl_y, 180.0, 0.5), "bubble follows the cursor")
   check(near(fl_shadow, 0.8, 0.02), "bubble shadow fades in")
   check(near(fl_radius, FL_START_RADIUS, 1.0), "bubble radius settles")
   fl_on = false
   i = 0
   while i < 600 { update_flashlight(1.0 / 60.0, 320.0, 180.0) i += 1 }
   check(near(fl_shadow, 0.0, 0.01), "bubble shadow fades out")
   check(near(fl_x, 320.0, 0.001), "disabled bubble snaps to the cursor")

   ;; Flashlight: a moving cursor stretches the bubble along its velocity.
   flashlight_reset(0.0, 0.0)
   fl_on = true
   i = 0
   while i < 8 {
      update_flashlight(1.0 / 60.0, 0.0, 0.0)
      i += 1
   }
   i = 0
   while i < 4 {
      update_flashlight(1.0 / 60.0, 300.0, 0.0)
      i += 1
   }
   check(float(math.abs(fl_stretch_x)) > 0.0, "bubble stretches while moving")

   ;; Lens SDF: zero on the rim, negative at the centre.
   flashlight_reset(0.0, 0.0)
   cam_scale = 1.0
   fl_radius = 100.0
   fl_target_radius = 100.0
   check(near(lens_sdf(0.0, 0.0), -100.0, 0.001), "lens sdf at the centre")
   check(near(lens_sdf(100.0, 0.0), 0.0, 0.001), "lens sdf on the rim")
   check(lens_sdf(160.0, 0.0) > 0.0, "lens sdf outside")
   fl_stretch_x = 0.2
   fl_squeeze = 0.4
   check(lens_sdf(0.0, 0.0) < 0.0, "stretched lens still covers the centre")

   ;; Refraction: straight through at the centre, bent at the rim.
   def list straight = refract_dir(0.0, 0.0, 1.0, 1.0 / 1.45)
   check(near(float(straight.get(0, 1.0)), 0.0, 0.0001), "refraction stays on axis")
   check(float(straight.get(2, 0.0)) < 0.0, "refraction keeps going into the glass")
   def list bent = refract_dir(0.6, 0.0, 0.8, 1.0 / 1.45)
   check(float(bent.get(0, 0.0)) < 0.0, "refraction bends at the rim")

   ;; Height profile is monotonic from the rim inwards.
   check(near(lens_height(0.0, 12.0), 0.0, 0.0001), "height at the rim")
   check(near(lens_height(-12.0, 12.0), 12.0, 0.0001), "height at the core")
   check(lens_height(-6.0, 12.0) > lens_height(-3.0, 12.0), "height grows inwards")

   ;; Lens magnification profile and the warp tables the bubble draws from.
   check(near(lens_zoom_at(0.0, 100.0), LENS_ZOOM, 0.0001), "lens zoom at the centre")
   check(near(lens_zoom_at(100.0, 100.0), 1.0, 0.0001), "lens zoom at the rim")
   def f64 zmid = lens_zoom_at(50.0, 100.0)
   check(zmid < LENS_ZOOM && zmid > 1.0, "lens zoom falls off")
   check(near(lens_zoom_at(300.0, 100.0), 1.0, 0.0001), "lens zoom clamps outside")
   shot_w = 800
   shot_h = 600
   camera_reset(false)
   flashlight_reset(400.0, 300.0)
   fl_on = true
   fl_radius = 100.0
   fl_target_radius = 100.0
   fl_stretch_x = 0.0
   fl_stretch_y = 0.0
   fl_squeeze = 0.0
   check(lens_tables(800.0, 600.0), "lens tables build")
   check(lens_n >= LENS_MIN_CELLS && lens_n <= LENS_MAX_CELLS, "lens grid resolution")
   def int mid = ((lens_n / 2) * (lens_n + 1) + lens_n / 2) * 4
   check(near(float(load32(lens_u, mid)) / LENS_UV_SCALE, 0.5, 0.03), "lens centre u")
   check(near(float(load32(lens_v, mid)) / LENS_UV_SCALE, 0.5, 0.03), "lens centre v")
   check(load8(lens_a, mid) > 200, "lens centre covered")
   check(lens_tables(800.0, 600.0), "lens tables cached when still")
   free_lens_tables()
   shot_w = 0
   shot_h = 0

   ;; Colour helpers and the synthetic capture.
   check(rgb_hex(0x0a14ff) == "#0A14FF", "hex formatting")
   check(capture_pattern(), "pattern capture")
   check(shot_w == 1280 && shot_h == 720, "pattern size")
   check(shot_pixel(0, 0) != 0, "pattern pixel")
   check(shot_pixel(-5, -5) == shot_pixel(0, 0), "pattern pixel clamps")
   check(build_blur(4), "blur pass")
   check(shot_blur != 0, "blur buffer")
   check(load8(shot_blur, 3) == 255, "blur keeps alpha")
   free_shot()

   ;; UV mapping: the window centre maps to the screenshot centre, and at scale
   ;; 1 one screen pixel is exactly one captured pixel (1:1 with the real screen).
   shot_w = 800
   shot_h = 600
   cam_scale = 1.0
   cam_x = 0.0
   cam_y = 0.0
   check(near(uv_x(400.0, 800.0), 0.5, 0.0001), "centre u")
   check(near(uv_y(300.0, 600.0), 0.5, 0.0001), "centre v")
   check(uv_x(800.0, 800.0) > 0.5 && uv_y(600.0, 600.0) > 0.5, "u and v grow with the screen")
   check(near(uv_x(101.0, 800.0) - uv_x(100.0, 800.0), 1.0 / 800.0, 0.000000001), "1:1 mapping in x")
   check(near(uv_y(101.0, 600.0) - uv_y(100.0, 600.0), 1.0 / 600.0, 0.000000001), "1:1 mapping in y")
   cam_scale = 2.0
   check(near(uv_x(101.0, 800.0) - uv_x(100.0, 800.0), 0.5 / 800.0, 0.000000001), "2x zoom halves the step")
   check(near(uv_x(800.0, 800.0), 0.75, 0.0001), "zoom halves the sampled span")

   ;; Camera clamping: the capture keeps covering the window.
   shot_w = 1920
   shot_h = 1080
   cam_scale = 1.0
   cam_x = 5000.0
   cam_y = -5000.0
   cam_tx = cam_x
   cam_ty = cam_y
   clamp_camera(1280.0, 720.0)
   check(near(cam_x, 320.0, 0.001), "camera x clamps to the capture edge")
   check(near(cam_y, -180.0, 0.001), "camera y clamps to the capture edge")
   check(near(cam_tx, 320.0, 0.001), "camera target clamps too")

   ;; Picker: the point under the pointer round-trips through capture space.
   shot_w = 800
   shot_h = 600
   cam_x = 0.0
   cam_y = 0.0
   cam_scale = 1.0
   pick_point_from_mouse(400.0, 300.0, 800.0, 600.0)
   check(near(pick_cx, 400.0, 0.0001) && near(pick_cy, 300.0, 0.0001), "picker centre")
   check(near(pick_screen_x(800.0), 400.0, 0.0001), "picker maps back in x")
   check(near(pick_screen_y(600.0), 300.0, 0.0001), "picker maps back in y")
   cam_scale = 2.0
   pick_point_from_mouse(400.0, 300.0, 800.0, 600.0)
   check(near(pick_cx, 400.0, 0.0001), "picker centre at 2x")
   pick_point_from_mouse(500.0, 300.0, 800.0, 600.0)
   check(near(pick_cx, 450.0, 0.0001), "2x picker moves half a screen pixel per pixel")

   ;; Key repeats must not re-toggle the flashlight or the picker.
   cam_scale = 1.0
   cam_x = 0.0
   cam_y = 0.0
   flashlight_reset(400.0, 300.0)
   picker_on = false
   handle_key(keyc.KEY_F, 0, false, 400.0, 300.0, 800.0, 600.0)
   check(!fl_on, "held f does not toggle")
   handle_key(keyc.KEY_F, 0, true, 400.0, 300.0, 800.0, 600.0)
   check(fl_on, "f press toggles the flashlight")
   handle_key(keyc.KEY_C, 0, false, 400.0, 300.0, 800.0, 600.0)
   check(!picker_on, "held c does not toggle")
   handle_key(keyc.KEY_F, 0, true, 400.0, 300.0, 800.0, 600.0)
   check(!fl_on, "second f press toggles back")
   flashlight_reset(0.0, 0.0)

   ;; The startup hint holds briefly, fades out, and any input starts the fade.
   check(near(hint_alpha(0.0), 1.0, 0.0001), "hint visible at start")
   check(near(hint_alpha(HINT_HOLD), 1.0, 0.0001), "hint holds while the screen settles")
   def f64 hmid = hint_alpha(HINT_HOLD + HINT_FADE * 0.5)
   check(hmid > 0.0 && hmid < 1.0, "hint fades out")
   check(near(hint_alpha(HINT_HOLD + HINT_FADE), 0.0, 0.0001), "hint gone after the fade")
   check(near(hint_alpha(600.0), 0.0, 0.0001), "hint stays hidden")
   hint_age = 0.0
   hint_dismiss()
   check(hint_age >= HINT_HOLD, "input starts the fade")

   shot_w = 0
   shot_h = 0

   print("ZOOMER_OK")
   exit(0)
}

;; ---------------------------------------------------------------------------
;; Entry point
;; ---------------------------------------------------------------------------

def list argv = process_args.args()
def bool want_help = process_args.flag_from(argv, "-h") || process_args.flag_from(argv, "--help")
def bool windowed = process_args.flag_from(argv, "-w") || process_args.flag_from(argv, "--windowed")
def bool start_pick = process_args.flag_from(argv, "-p") || process_args.flag_from(argv, "--pick")
def bool force_pattern = process_args.flag_from(argv, "--pattern")
def bool want_selftest = process_args.flag_from(argv, "--selftest")
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
   print("      --selftest          run the built-in checks")
   print("")
   print("Capture uses grim, spectacle, gnome-screenshot, scrot or maim.")
   print("NYTRIX_ZOOMER_IMAGE=path loads a screenshot you took yourself.")
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

if want_selftest { selftest() }

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

;; The window starts at the capture size so a fullscreen surface shows the
;; screen 1:1 at scale 1; only zoom and pan change what is on screen.
def int capture_w = shot_w > 0 ? shot_w : 1600
def int capture_h = shot_h > 0 ? shot_h : 900
mut int init_w = capture_w
mut int init_h = capture_h
if windowed {
   init_w = int(math.min(float(capture_w), 1280.0))
   init_h = int(math.min(float(capture_h), 720.0))
}
;; A magnifier is an overlay: fullscreen borderless and always on top, the way
;; the C original uses override_redirect + save_under.
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
trace("textures " + to_str(shot_tex) + " " + to_str(blur_tex))
def bool headless = gfx.get_active_backend_name() == "mock"

if shot_tex <= 0 {
   eprint("zoomer: backend '" + gfx.get_active_backend_name() +
      "' cannot sample textures; the " + shot_source + " capture needs a GPU backend")
   free_shot()
   gfx.close_window()
   exit(headless ? 0 : 1)
}

;; One line that says everything needed to diagnose a bad first frame: which
;; capture tool produced the image, its size, the size of the texture the
;; backend actually bound, and which backend drew it.
def list diag_tex = gfx.texture_size(shot_tex)
print("zoomer: " + shot_source + " " + to_str(shot_w) + "x" + to_str(shot_h) +
   " -> texture " + to_str(int(diag_tex.get(0, 0))) + "x" + to_str(int(diag_tex.get(1, 0))) +
   " on " + gfx.get_active_backend_name() + " (scale 1 is 1:1, wheel or +/- to zoom)")
font_big = gfx.font_load_first(assets.MONO_FONT_CANDIDATES, 22)
font_small = gfx.font_load_first(assets.MONO_FONT_CANDIDATES, 14)

if font_big > 0 { gfx.font_prepare(font_big, "0123456789ABCDEF#") }
if font_small > 0 { gfx.font_prepare(font_small, "abcdefghijklmnopqrstuvwxyz0123456789 ,-/+") }
flashlight_reset(0.0, 0.0)
picker_on = start_pick

;; NYTRIX_ZOOMER_FLASHLIGHT=1 starts with the bubble on (handy for scripted
;; captures); NYTRIX_ZOOMER_DUMP=path saves the first frame.
if env_flag("NYTRIX_ZOOMER_FLASHLIGHT") {
   fl_on = true
   fl_shadow = 0.8
   fl_radius = FL_START_RADIUS
   fl_target_radius = FL_START_RADIUS
}
def str dump_path = is_str(env("NYTRIX_ZOOMER_DUMP")) ? to_str(env("NYTRIX_ZOOMER_DUMP")) : ""
def int max_frames = env_int("NYTRIX_ZOOMER_MAX_FRAMES", 0)
;; The dump needs one frame of notice: the backends record a readback copy only
;; when the capture was requested before that frame is submitted.
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
mut bool prev_drag = false

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

   def bool drag_now = window.mouse_down(win, 0) && !picker_on
   if drag_now && prev_drag {
      def f64 dx = (mouse_x - drag_prev_x) / cam_scale
      def f64 dy = (mouse_y - drag_prev_y) / cam_scale
      cam_x = cam_x - dx
      cam_y = cam_y - dy
      cam_tx = cam_x
      cam_ty = cam_y
      cam_vx = -dx / math.max(dt, 0.0001)
      cam_vy = -dy / math.max(dt, 0.0001)
   }
   if drag_now {
      drag_prev_x = mouse_x
      drag_prev_y = mouse_y
   }
   prev_drag = drag_now

   hint_age = hint_age + dt
   update_camera(dt, drag_now, w, h)
   update_flashlight(dt, mouse_x, mouse_y)
   update_picker(mouse_x, mouse_y, w, h)

   if !gfx.begin_frame_clear([0.10, 0.10, 0.10, 1.0]) {
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
   draw_desktop(w, h)
   draw_flashlight(w, h)
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
      ;; Wayland never asks the compositor for the fullscreen in the window
      ;; flags, so a tiling compositor hands back a half tile and the capture is
      ;; scaled. Maximizing fills the monitor, keeping the screen 1:1 at scale 1.
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
free_shot()
free_lens_tables()
if shot_tex > 0 { gfx.texture_destroy(shot_tex) }
if blur_tex > 0 { gfx.texture_destroy(blur_tex) }
gfx.close_window()
exit(quit_with_color ? 0 : (surface_failures >= 120 ? 1 : 0))
