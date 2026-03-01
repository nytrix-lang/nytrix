use std.core *
use std.core.error *
use std.ui.window *
use std.ui.gfx *
use std.ui.consts *
use std.os.time *
use std.math *
use std.text.io as tio
use std.os *

;; cache-bust-comment
if(env("CI") || env("NYTRIX_TEST_MODE") == "1"){
   tio.print("Skipping UI test in CI/Test Mode")
   __exit(0)
}

mut last_fps_time = 0
mut current_fps = 0
mut frames_this_second = 0
mut title_font = 0
mut ui_font = 0
mut dump_last_ms = 0
mut snapshot_done = false

fn _str_eq(a, b){
   _touch(a, b)
   if(!is_str(a) || !is_str(b)){ return false }
   if(str_len(a) != str_len(b)){ return false }
   mut i = 0
   while(i < str_len(a)){
      if(load8(a, i) != load8(b, i)){ return false }
      i += 1
   }
   true
}

fn _env_on(name){
   _touch(name)
   def v = env(name)
   if(!is_str(v)){ return false }
   if(str_len(v) == 1 && load8(v, 0) == 49){ return true } ;; "1"
   _str_eq(v, "true") || _str_eq(v, "TRUE") || _str_eq(v, "yes") || _str_eq(v, "on")
}

fn _to_i(v){
   _touch(v)
   if(is_int(v)){ return v }
   def s = to_str(v)
   if(!is_str(s) || str_len(s) == 0){ return 0 }
   mut i = 0
   mut neg = false
   if(load8(s, 0) == 45){ neg = true i = 1 } ;; '-'
   mut n = 0
   while(i < str_len(s)){
      def c = load8(s, i)
      if(c == 46){ break } ;; '.'
      if(c < 48 || c > 57){ break }
      n = (n * 10) + (c - 48)
      i += 1
   }
   if(neg){ return -n }
   n
}

fn _dump_layout(win, mode, header_h, footer_h,
                left_x, left_y, left_w, left_h,
                center_x, center_y, center_w, center_h,
                right_x, right_y, right_w, right_h){
   _touch(win, mode, header_h, footer_h, left_x, left_y, left_w, left_h, center_x, center_y, center_w, center_h, right_x, right_y, right_w, right_h)
   if(!_env_on("NY_UI_DUMP")){ return }
   def now_ms = ticks() / 1000000
   if(now_ms - dump_last_ms < 250){ return }
   dump_last_ms = now_ms
   def w = _win_w(win)
   def h = _win_h(win)
   def in_bounds = (left_x >= 0.0) && (left_y >= 0.0) && (center_x >= 0.0) && (center_y >= 0.0) &&
                    (right_x >= 0.0) && (right_y >= 0.0) &&
                    ((left_x + left_w) <= (w + 1.0)) && ((left_y + left_h) <= (h + 1.0)) &&
                    ((center_x + center_w) <= (w + 1.0)) && ((center_y + center_h) <= (h + 1.0)) &&
                    ((right_x + right_w) <= (w + 1.0)) && ((right_y + right_h) <= (h + 1.0))
   mut ok_v = 0
   if(in_bounds){ ok_v = 1 }
   mut s = "[ui] mode="
   s = cat(s, mode)
   s = cat(s, " win=")
   s = cat(s, to_str(_to_i(w)))
   s = cat(s, "x")
   s = cat(s, to_str(_to_i(h)))
   s = cat(s, " header=")
   s = cat(s, to_str(_to_i(header_h)))
   s = cat(s, " footer=")
   s = cat(s, to_str(_to_i(footer_h)))
   s = cat(s, " left=")
   s = cat(s, to_str(_to_i(left_x)))
   s = cat(s, ",")
   s = cat(s, to_str(_to_i(left_y)))
   s = cat(s, ",")
   s = cat(s, to_str(_to_i(left_w)))
   s = cat(s, "x")
   s = cat(s, to_str(_to_i(left_h)))
   s = cat(s, " center=")
   s = cat(s, to_str(_to_i(center_x)))
   s = cat(s, ",")
   s = cat(s, to_str(_to_i(center_y)))
   s = cat(s, ",")
   s = cat(s, to_str(_to_i(center_w)))
   s = cat(s, "x")
   s = cat(s, to_str(_to_i(center_h)))
   s = cat(s, " right=")
   s = cat(s, to_str(_to_i(right_x)))
   s = cat(s, ",")
   s = cat(s, to_str(_to_i(right_y)))
   s = cat(s, ",")
   s = cat(s, to_str(_to_i(right_w)))
   s = cat(s, "x")
   s = cat(s, to_str(_to_i(right_h)))
   s = cat(s, " ok=")
   s = cat(s, to_str(ok_v))
   tio.print(s)
}

fn _font_candidates(){
   def host = __os_name()
   if(_str_eq(host, "windows")){
      return [
         "C:/Windows/Fonts/segoeui.ttf",
         "C:/Windows/Fonts/arial.ttf",
         "C:/Windows/Fonts/calibri.ttf"
      ]
   }
   if(_str_eq(host, "macos")){
      return [
         "/System/Library/Fonts/Supplemental/Arial.ttf",
         "/System/Library/Fonts/Supplemental/Helvetica.ttf"
      ]
   }
   return [
      "etc/assets/font/Roboto-Regular.ttf",
      "etc/assets/font/jetbrains.ttf",
      "etc/assets/font/monocraft.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"
   ]
}

fn _load_font(size){
   _touch(size)
   def paths = _font_candidates()
   mut i = 0
   while(i < len(paths)){
      def id = font_load(get(paths, i), size)
      if(id){ return id }
      i += 1
   }
   0
}

fn _win_w(win){
   _touch(win)
   mut w = get(win, 5, 1280)
   if(!is_int(w) || w < 1){
      def sz = window_size(win)
      w = get(sz, 0, 1280)
      if(!is_int(w) || w < 1){ w = 1280 }
   }
   w
}

fn _win_h(win){
   _touch(win)
   mut h = get(win, 6, 720)
   if(!is_int(h) || h < 1){
      def sz = window_size(win)
      h = get(sz, 1, 720)
      if(!is_int(h) || h < 1){ h = 720 }
   }
   h
}

fn _clamp(v, lo, hi){
   _touch(v, lo, hi)
   if(v < lo){ return lo }
   if(v > hi){ return hi }
   v
}

fn _panel(x, y, w, h, bg, stroke){
   _touch(x, y, w, h, bg, stroke)
   if(w <= 2.0 || h <= 2.0){ return }
   def r = _clamp(min(w, h) * 0.06, 8.0, 16.0)
   draw_rounded_rectangle(x, y, w, h, r, bg)
   draw_rounded_rectangle(x + 1.0, y + 1.0, w - 2.0, h - 2.0, max(6.0, r - 1.0), color_rgba(1.0, 1.0, 1.0, 0.03))
   draw_rectangle_lines(x + 0.5, y + 0.5, w - 1.0, h - 1.0, stroke, 1.0)
}

fn _txt(s, x, y, color=WHITE){
   _touch(s, x, y, color)
   ;; Always route through draw_text so renderer fallback remains visible when a TTF load fails.
   mut font_id = 0
   if(ui_font){ font_id = ui_font }
   draw_text(font_id, s, x, y, color)
}

fn _metric_row(label, value, x, y){
   _touch(label, value, x, y)
   _txt(label, x, y, color_hex("#94a3b8"))
   _txt(value, x + 122.0, y, color_hex("#e2e8f0"))
}

fn draw_hud(win, t, fps){
   _touch(win, t, fps)
   def wi = max(260, _win_w(win))
   def hi = max(220, _win_h(win))
   def w = float(wi)
   def h = float(hi)
   def pad = _clamp(min(w, h) * 0.018, 10.0, 24.0)
   def header_h = _clamp(h * 0.11, 54.0, 92.0)
   def footer_h = _clamp(h * 0.075, 40.0, 64.0)
   def body_y = header_h + pad
   def body_h = max(96.0, h - body_y - footer_h - pad)

   draw_circle(w * 0.80, h * 0.20, max(120.0, w * 0.22), color_rgba(0.05, 0.16, 0.32, 0.26), 72)
   draw_circle(w * 0.15, h * 0.82, max(110.0, h * 0.22), color_rgba(0.16, 0.06, 0.24, 0.19), 72)

   _panel(0.0, 0.0, w, header_h, color_rgba(0.03, 0.06, 0.12, 0.92), color_hex("#1d4ed8"))
   draw_line_2d(0.0, header_h - 1.0, w, header_h - 1.0, color_hex("#38bdf8"), 1.0)
   if(title_font){
      draw_text(title_font, "Nytrix UI Reference", pad + 2.0, 12.0, color_hex("#e2e8f0"))
   }
   elif(ui_font){ draw_text(ui_font, "Nytrix UI Reference", pad + 2.0, 14.0, color_hex("#e2e8f0")) }
   else { draw_text(0, "Nytrix UI Reference", pad + 2.0, 14.0, color_hex("#e2e8f0")) }
   mut backend_name = get_active_backend_name()
   if(!is_str(backend_name) || str_len(backend_name) == 0){ backend_name = "none" }
   mut status_line = cat("Backend ", backend_name)
   status_line = cat(status_line, " | ")
   status_line = cat(status_line, to_str(wi))
   status_line = cat(status_line, "x")
   status_line = cat(status_line, to_str(hi))
   _txt(status_line, pad + 4.0, header_h - 24.0, color_hex("#93c5fd"))
   _txt("ESC close", w - pad - 86.0, header_h - 24.0, color_hex("#f8fafc"))

   mut mode = "wide"
   mut left_x = pad
   mut left_y = body_y
   mut left_w = 0.0
   mut left_h = 0.0
   mut center_x = pad
   mut center_y = body_y
   mut center_w = 0.0
   mut center_h = 0.0
   mut right_x = pad
   mut right_y = body_y
   mut right_w = 0.0
   mut right_h = 0.0

   if(w >= 940){
      left_w = _clamp(w * 0.20, 180.0, 280.0)
      right_w = _clamp(w * 0.24, 220.0, 340.0)
      def min_center = 300.0
      if(w - (pad * 4.0 + left_w + right_w) < min_center){
         right_w = max(180.0, w - (pad * 4.0 + left_w + min_center))
      }
      center_w = max(220.0, w - (pad * 4.0 + left_w + right_w))
      left_h = body_h
      center_h = body_h
      right_h = body_h
      center_x = left_x + left_w + pad
      center_y = body_y
      right_x = center_x + center_w + pad
      right_y = body_y
   } else {
      mode = "compact"
      left_w = w - pad * 2.0
      left_h = _clamp(body_h * 0.28, 96.0, 168.0)
      center_x = pad
      center_y = left_y + left_h + pad
      center_w = w - pad * 2.0
      right_h = _clamp(body_h * 0.24, 88.0, 150.0)
      center_h = body_h - left_h - right_h - pad * 2.0
      if(center_h < 90.0){ center_h = 90.0 }
      right_x = pad
      right_y = center_y + center_h + pad
      right_w = w - pad * 2.0
      if(right_y + right_h > h - footer_h - pad){
         right_h = max(72.0, (h - footer_h - pad) - right_y)
      }
   }

   _dump_layout(win, mode, header_h, footer_h,
                left_x, left_y, left_w, left_h,
                center_x, center_y, center_w, center_h,
                right_x, right_y, right_w, right_h)

   _panel(left_x, left_y, left_w, left_h, color_rgba(0.08, 0.12, 0.20, 0.92), color_hex("#334155"))
   _panel(center_x, center_y, center_w, center_h, color_rgba(0.06, 0.10, 0.16, 0.94), color_hex("#1f2937"))
   _panel(right_x, right_y, right_w, right_h, color_rgba(0.07, 0.11, 0.18, 0.92), color_hex("#334155"))

   _txt("SECTIONS", left_x + 14.0, left_y + 12.0, color_hex("#cbd5e1"))
   def items = ["Overview", "Renderer", "Typography", "Input", "Diagnostics"]
   def active_idx = int(t * 0.65) % len(items)
   mut i = 0
   while(i < len(items)){
      def iy = left_y + 36.0 + i * 24.0
      if(iy + 20.0 < left_y + left_h - 70.0){
         if(i == active_idx){
            draw_rounded_rectangle(left_x + 10.0, iy - 2.0, left_w - 20.0, 20.0, 6.0, color_rgba(0.12, 0.26, 0.46, 0.72))
            draw_rectangle(left_x + 10.0, iy - 2.0, 3.0, 20.0, color_hex("#22d3ee"))
         }
         _txt(get(items, i, ""), left_x + 18.0, iy, color_hex("#e2e8f0"))
      }
      i += 1
   }

   def mp = mouse_get_pos()
   _metric_row("Mouse", cat(to_str(_to_i(get(mp, 0, 0))), ", ", to_str(_to_i(get(mp, 1, 0)))), left_x + 14.0, left_y + left_h - 62.0)
   mut esc_state = "up"
   if(key_down(KEY_ESCAPE)){ esc_state = "down" }
   _metric_row("Escape", esc_state, left_x + 14.0, left_y + left_h - 42.0)
   _metric_row("FPS", str(fps), left_x + 14.0, left_y + left_h - 22.0)

   _txt("VIEWPORT", center_x + 14.0, center_y + 12.0, color_hex("#cbd5e1"))
   def viewport_x = center_x + 14.0
   def viewport_y = center_y + 34.0
   def viewport_w = max(60.0, center_w - 28.0)
   def viewport_h = max(60.0, center_h - 96.0)
   _panel(viewport_x, viewport_y, viewport_w, viewport_h, color_rgba(0.03, 0.07, 0.12, 0.94), color_hex("#0ea5e9"))

   mut gx = 0
   while(gx <= 6){
      def xx = viewport_x + (viewport_w * gx / 6.0)
      draw_line_2d(xx, viewport_y, xx, viewport_y + viewport_h, color_rgba(0.25, 0.32, 0.42, 0.35), 1.0)
      gx += 1
   }
   mut gy = 0
   while(gy <= 4){
      def yy = viewport_y + (viewport_h * gy / 4.0)
      draw_line_2d(viewport_x, yy, viewport_x + viewport_w, yy, color_rgba(0.25, 0.32, 0.42, 0.35), 1.0)
      gy += 1
   }

   def cx = viewport_x + viewport_w * 0.5 + sin(t * 0.9) * (viewport_w * 0.22)
   def cy = viewport_y + viewport_h * 0.5 + cos(t * 0.7) * (viewport_h * 0.26)
   draw_circle(cx, cy, 7.0, color_hex("#22d3ee"), 24)
   draw_line_2d(cx - 12.0, cy, cx + 12.0, cy, color_hex("#38bdf8"), 1.5)
   draw_line_2d(cx, cy - 12.0, cx, cy + 12.0, color_hex("#38bdf8"), 1.5)

   if(center_h > 130.0){
      _txt("TTF: Aa Bb Cc 0123456789", center_x + 14.0, viewport_y + viewport_h + 8.0, color_hex("#e5e7eb"))
      _txt("Kerning: AV WA To fi ffi", center_x + 14.0, viewport_y + viewport_h + 26.0, color_hex("#cbd5e1"))
      _txt("UTF-8: Nytrix ✓ ∆ 漢字", center_x + 14.0, viewport_y + viewport_h + 44.0, color_hex("#a5f3fc"))
   } elif(center_h > 106.0){
      _txt("UTF-8: Nytrix ✓ ∆ 漢字", center_x + 14.0, viewport_y + viewport_h + 10.0, color_hex("#a5f3fc"))
   }

   _txt("DIAGNOSTICS", right_x + 14.0, right_y + 12.0, color_hex("#cbd5e1"))
   _metric_row("Backend", get_active_backend_name(), right_x + 14.0, right_y + 36.0)
   _metric_row("Window", cat(to_str(_to_i(w)), "x", to_str(_to_i(h))), right_x + 14.0, right_y + 56.0)
   mut frame_ms = 0
   if(fps > 0){ frame_ms = 1000 / fps }
   _metric_row("Frame ms", to_str(frame_ms), right_x + 14.0, right_y + 76.0)
   _metric_row("Fonts", cat(to_str(title_font), " / ", to_str(ui_font)), right_x + 14.0, right_y + 96.0)

   if(right_h > 132.0){
      _txt("Legend", right_x + 14.0, right_y + 122.0, color_hex("#94a3b8"))
      draw_rectangle(right_x + 16.0, right_y + 144.0, 12.0, 12.0, color_hex("#22d3ee"))
      _txt("Active marker", right_x + 34.0, right_y + 142.0, color_hex("#cbd5e1"))
      draw_rectangle(right_x + 16.0, right_y + 164.0, 12.0, 12.0, color_hex("#1d4ed8"))
      _txt("Panel accent", right_x + 34.0, right_y + 162.0, color_hex("#cbd5e1"))
   }

   _panel(0.0, h - footer_h, w, footer_h, color_rgba(0.03, 0.06, 0.12, 0.92), color_hex("#1f2937"))
   _txt("Reference layout: header / navigation / viewport / diagnostics", pad + 4.0, h - footer_h + 10.0, color_hex("#cbd5e1"))
   _txt("Real Vulkan path, real TTF text, responsive placement", pad + 4.0, h - footer_h + 28.0, color_hex("#94a3b8"))
}

fn main() {
   if(_env_on("NY_UI_DUMP")){
      tio.print("[ui] start demo")
   }

   def is_headless = _env_on("NY_HEADLESS")
   if(is_headless){
      tio.print("NY_HEADLESS=1 detected, skipping visual demo.")
      return 0
   }

   if(!init_window(1280, 720, "Nytrix Vulkan UI Demo", true)) {
      tio.print("Failed to initialize UI window")
      __exit(1)
   }

   def win = get_active_window()
   window_set_exit_key(win, KEY_ESCAPE)
   tio.print(cat("Active Backend: ", get_active_backend_name()))
   if(_env_on("NY_UI_DUMP")){
      tio.print("[ui] geometry dump enabled (NY_UI_DUMP=1)")
   }

   title_font = _load_font(34)
   ui_font = _load_font(18)
   if(_env_on("NY_UI_DUMP")){
      mut fmsg = cat("[ui] fonts title=", to_str(title_font))
      fmsg = cat(fmsg, " ui=")
      fmsg = cat(fmsg, to_str(ui_font))
      tio.print(fmsg)
   }
   if(!title_font || !ui_font){
      tio.print("Warning: .ttf load failed, using fallback text renderer")
   }

   last_fps_time = ticks() / 1000000 ;; ms
   def start_t = get_time()

   while(true) {
      if(window_should_close(win)) { break }

      def now_ms = ticks() / 1000000
      frames_this_second += 1
      if(now_ms - last_fps_time >= 1000) {
         current_fps = frames_this_second
         frames_this_second = 0
         last_fps_time = now_ms
      }

      begin_frame_clear(color_hex("#0f172a"))

      def t = get_time() - start_t
      draw_hud(win, t, current_fps)

      end_frame()

      if(!snapshot_done){
         def snap_out = env("NY_UI_SNAPSHOT")
         if(is_str(snap_out) && str_len(snap_out) > 0){
            snapshot_done = true
            snapshot(snap_out)
            if(_env_on("NY_UI_SNAPSHOT_EXIT")){ break }
         }
      }

      if(key_pressed(KEY_ESCAPE) || key_down(KEY_ESCAPE) || window_key_down(win, 0xFF1B)){ break }
      msleep(1)
   }

   if(title_font){ font_destroy(title_font) }
   if(ui_font){ font_destroy(ui_font) }
   close_window()
   tio.print("Demo finished")
   0
}

main()
