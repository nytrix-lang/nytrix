#!/usr/bin/env ny

;; Keywords: ui window monitor display desktop example
;; Live monitor detector with scaled desktop layout and window placement.
use core
use core.os (exit)
use core.os.fixture (ready, start_ui, tick_ui, quit_key, next_ui_key, exit_status)
use core.os (ticks)
use core.os.ui.window.consts (
   WINDOW_SCALE_TO_MONITOR, WINDOW_CENTER,
   KEY_F, WINDOW_FULLSCREEN, KEY_B, KEY_SPACE,
   KEY_R, WINDOW_NO_RESIZE, KEY_C, WINDOW_NO_BORDER,
   KEY_M, WINDOW_MAXIMIZE, KEY_T, WINDOW_FLOATING,
   KEY_V, KEY_H, WINDOW_HIDE, KEY_N, WINDOW_MINIMIZE,
   KEY_ENTER
)
use core.os.ui.render as gfx
use core.os.ui.render.viewer.runtime as ui_runtime
use core.os.ui.render.dump (auto_dump_delay_frames, auto_dump_enabled, auto_dump_pre_frame, auto_dump_post_frame)
use core.os.ui.render.viewer.widgets (C_TEXT, C_MUTED, text_right, C_PANEL_ALT, C_LINE, C_ACCENT_HI)
use core.os.ui.render.viewer.window as view_window
use core.os.ui.window

gfx.apply_backend_env()
gfx.apply_backend_argv()
mut win = gfx.init_window(960, 540, "Nytrix Monitor Detector", WINDOW_SCALE_TO_MONITOR | WINDOW_CENTER)

if !win { panic("window init failed") }
if is_dict(win) && !!win.get("_mock", false) {
   if gfx.begin_frame_clear(gfx.color_rgb(0.015, 0.015, 0.015)) {
      gfx.end_frame()
   }
   gfx.close_window()
   exit(0)
}
ready("monitor")
print("[monitor:trace] backend=" + to_str(gfx.get_active_backend()))
def UI_FONTS = own(["etc/assets/fonts/monocraft.ttf", "etc/assets/fonts/jetbrains.ttf"])
def font_title = gfx.font_load_first(UI_FONTS, 28)
def font_body = gfx.font_load_first(UI_FONTS, 18)
def font_small = gfx.font_load_first(UI_FONTS, 14)
def bg = gfx.color_rgb(0.015, 0.015, 0.015)
fn max_f64(f64 a, f64 b) f64 { a > b ? a : b }
fn min_f64(f64 a, f64 b) f64 { a < b ? a : b }
fn clamp_f64(f64 value, f64 lower, f64 upper) f64 {
   min_f64(max_f64(value, lower), upper)
}
fn max_int(int a, int b) int { a > b ? a : b }
def auto_dump_delay = auto_dump_delay_frames(8)
def timeout_limit = ui_runtime.timeout_ns(0)
def mock_backend = is_dict(win) && !!win.get("_mock", false)
def fx = start_ui(auto_dump_enabled() ? auto_dump_delay + 2 : 0, 0, mock_backend)
mut frame_count = 0
mut fps_state = ui_runtime.fps_begin()
mut auto_dump_done = false
mut hidden_timer = 0.0
mut minimized_timer = 0.0
def MONITOR_REFRESH_NS = 500000000
def STATE_REFRESH_NS = 100000000
mut monitor_rows = own([])
mut current_monitor = 0
mut last_monitor_refresh = 0
mut state_cache = own({})
mut last_state_refresh = 0
mut hud_win_w = -1
mut hud_win_h = -1
mut hud_sw = -1
mut hud_sh = -1
mut hud_raw_mx = -1
mut hud_raw_my = -1
mut hud_mx = -1
mut hud_my = -1
mut hud_fps = -1
mut hud_monitors = -1
mut hud_dpi_x = -1.0
mut hud_dpi_y = -1.0
mut hud_info = ""
mut hud_right = ""
mut hud_dpi_line1 = ""
mut hud_dpi_line2 = ""

fn refresh_hud_text(list ws, f64 sw, f64 sh, list dpi, list mouse, f64 mx, f64 my, int fps, int monitor_count) int {
   def iww = int(ws.get(0, sw))
   def iwh = int(ws.get(1, sh))
   def isw = int(sw)
   def ish = int(sh)
   def rmx = int(mouse.get(0, 0))
   def rmy = int(mouse.get(1, 0))
   def imx = int(mx)
   def imy = int(my)
   def dx = float(dpi.get(0, 1.0))
   def dy = float(dpi.get(1, 1.0))
   if (iww == hud_win_w && iwh == hud_win_h && isw == hud_sw && ish == hud_sh &&
      rmx == hud_raw_mx && rmy == hud_raw_my && imx == hud_mx && imy == hud_my &&
   fps == hud_fps && monitor_count == hud_monitors && dx == hud_dpi_x && dy == hud_dpi_y){ return 0 }
   hud_win_w = iww
   hud_win_h = iwh
   hud_sw = isw
   hud_sh = ish
   hud_raw_mx = rmx
   hud_raw_my = rmy
   hud_mx = imx
   hud_my = imy
   hud_fps = fps
   hud_monitors = monitor_count
   hud_dpi_x = dx
   hud_dpi_y = dy
   hud_info = "fb " + to_str(isw) + "x" + to_str(ish) +
   "   scale " + to_str(dx) + "x" + to_str(dy) +
   "   mouse " + to_str(imx) + "," + to_str(imy)
   hud_right = "fps " + to_str(fps) + "   " + to_str(monitor_count) + " display(s)"
   hud_dpi_line1 = "window " + to_str(iww) + "x" + to_str(iwh) + "   fb " + to_str(isw) + "x" + to_str(ish)
   hud_dpi_line2 = "scale " + to_str(dx) + "x" + to_str(dy) +
   "   mouse " + to_str(rmx) + "," + to_str(rmy) + " -> " + to_str(imx) + "," + to_str(imy)
   0
}

fn refresh_monitor_cache(bool force) int {
   def now = ticks()
   if !force && last_monitor_refresh != 0 && now - last_monitor_refresh < MONITOR_REFRESH_NS { return 0 }
   if win.get("_mock", false) {
      monitor_rows = own([])
      current_monitor = 0
      last_monitor_refresh = now
      return 0
   }
   def monitors = window.get_monitors()
   if force || last_monitor_refresh == 0 { print("[monitor:trace] monitors.count=" + to_str(monitors.len)) }
   mut rows = []
   mut i = 0
   while i < monitors.len {
      def mon = monitors.get(i)
      def mode = window.get_video_mode(mon)
      def rect = window.get_monitor_rect(mon)
      rows = rows.append(view_window.monitor_row(mon, i))
      if force || last_monitor_refresh == 0 { print("[monitor:trace] dict=" + to_str(is_dict(mon)) + " size=" + to_str(mon.get("width", -1)) + "x" + to_str(mon.get("height", -1)) + " mode=" + to_str(mode.get("width", -1)) + "x" + to_str(mode.get("height", -1)) + " pos=" + to_str(window.get_monitor_pos(mon)) + " rect=" + to_str(rect.get(0, -1)) + "," + to_str(rect.get(1, -1)) + " " + to_str(rect.get(2, -1)) + "x" + to_str(rect.get(3, -1)) + " ws=" + to_str(window.size(win)) + " wp=" + to_str(window.pos(win))) }
      i += 1
   }
   def rows_len = rows.len
   monitor_rows = own(rows)
   if rows_len > 0 {
      current_monitor = window.get_current_monitor_index(win, monitors)
      if current_monitor < 0 { current_monitor = 0 }
      elif current_monitor >= rows_len { current_monitor = rows_len - 1 }
   } else {
      current_monitor = 0
   }
   last_monitor_refresh = now
   0
}

fn refresh_window_state(bool force) dict {
   def now = ticks()
   if !force && last_state_refresh != 0 && now - last_state_refresh < STATE_REFRESH_NS && is_dict(state_cache) { return state_cache }
   state_cache = window.window_state(win)
   last_state_refresh = now
   state_cache
}

fn toggle_flag_key(any code, any flag) bool { if !window.key_pressed(win, code) { return false } window.toggle_window_flag(win, flag) true }

fn handle_window_flag_keys() bool {
   mut changed = false
   if toggle_flag_key(KEY_F, WINDOW_FULLSCREEN) { changed = true }
   if window.key_pressed(win, KEY_B) || window.key_pressed(win, KEY_SPACE) { window.toggle_window_borderless(win) changed = true }
   if toggle_flag_key(KEY_R, WINDOW_NO_RESIZE) { changed = true }
   if toggle_flag_key(KEY_C, WINDOW_NO_BORDER) { changed = true }
   if toggle_flag_key(KEY_M, WINDOW_MAXIMIZE) { changed = true }
   if toggle_flag_key(KEY_T, WINDOW_FLOATING) { changed = true }
   if window.key_pressed(win, KEY_V) { window.toggle_window_vsync() changed = true }
   if window.key_pressed(win, KEY_H) {
      window.toggle_window_flag(win, WINDOW_HIDE)
      hidden_timer = window.has_window_flag(win, WINDOW_HIDE) ? 3.0 : 0.0
      changed = true
   }
   if window.key_pressed(win, KEY_N) {
      window.set_window_flag(win, WINDOW_MINIMIZE, true)
      minimized_timer = 3.0
      changed = true
   }
   changed
}

fn update_window_flag_timers(f64 dt) bool {
   mut changed = false
   if hidden_timer > 0.0 {
      hidden_timer -= dt
      if hidden_timer <= 0.0 { window.set_window_flag(win, WINDOW_HIDE, false) changed = true }
   }
   if minimized_timer > 0.0 {
      minimized_timer -= dt
      if minimized_timer <= 0.0 { window.set_window_flag(win, WINDOW_MINIMIZE, false) changed = true }
   }
   changed
}

while !gfx.window_should_close(win) && tick_ui(fx) {
   if frame_count < 4 { print("[monitor:trace] frame.begin " + to_str(frame_count)) }
   if quit_key(next_ui_key(fx)) { break }
   if mock_backend {
      if gfx.begin_frame_clear(bg) {
         gfx.draw_text(font_title, "Nytrix Monitor Detector", 24.0, 24.0, C_TEXT)
         gfx.end_frame()
      }
      continue
   }
   def dt = gfx.get_delta_time()
   def state_changed = handle_window_flag_keys()
   def timer_changed = update_window_flag_timers(dt)
   if !gfx.begin_frame_clear(bg) {
      if frame_count < 4 { print("[monitor:trace] begin_frame_clear=false") }
      continue
   }
   fps_state = ui_runtime.fps_tick(fps_state, dt)
   def fps = ui_runtime.fps_current(fps_state, dt)
   def fb = window.get_framebuffer_size(win)
   def ws = window.size(win)
   if frame_count < 4 { print("[monitor:trace] frame.open fb=" + to_str(fb.get(0, -1)) + "x" + to_str(fb.get(1, -1)) + " ws=" + to_str(ws.get(0, -1)) + "x" + to_str(ws.get(1, -1))) }
   def state = refresh_window_state(state_changed || timer_changed)
   def dpi = window.get_window_scale_dpi(win)
   def mouse = window.mouse_pos(win)
   def f64 sw = max_f64(1.0, float(fb.get(0, 960.0)))
   def f64 sh = max_f64(1.0, float(fb.get(1, 540.0)))
   def f64 sx = sw / max_f64(1.0, float(ws.get(0, 960)))
   def f64 sy = sh / max_f64(1.0, float(ws.get(1, 540)))
   def f64 mx = clamp_f64(float(mouse.get(0, 0)) * sx, 0.0, sw)
   def f64 my = clamp_f64(float(mouse.get(1, 0)) * sy, 0.0, sh)
   gfx.set_ortho_2d(0.0, sw, 0.0, sh)
   refresh_monitor_cache(false)
   def monitor_count = monitor_rows.len
   mut current = current_monitor
   if window.key_pressed(win, KEY_ENTER) && monitor_count > 1 {
      current = (current + 1) % max_int(1, monitor_count)
      window.move_to_monitor(win, monitor_rows.get(current, {}).get("mon", 0))
      refresh_monitor_cache(true)
      current = current_monitor
   }
   refresh_hud_text(ws, sw, sh, dpi, mouse, mx, my, fps, monitor_count)
   if frame_count < 4 { print("[monitor:trace] hud.ready monitors=" + to_str(monitor_count)) }
   def f64 pad = clamp_f64(min_f64(sw, sh) * 0.045, 18.0, 38.0)
   def header_h = sw < 720.0 ? 104.0 : 96.0
   def area_x = pad
   def area_y = pad + header_h
   def f64 area_w = max_f64(120.0, sw - pad * 2.0)
   def f64 area_h = max_f64(120.0, sh - area_y - pad)
   gfx.draw_text(font_title, "Monitor Detector", pad, pad + 4.0, C_TEXT)
   gfx.draw_text(font_small, "Enter: next monitor   flags: F B R C H N M T V", pad, pad + 40.0, C_MUTED)
   gfx.draw_text(font_small, hud_info, pad, pad + 62.0, C_MUTED)
   text_right(font_small, hud_right, sw - pad, pad + 44.0, C_MUTED)
   gfx.draw_rect(area_x, area_y, area_w, area_h, gfx.color_alpha(C_PANEL_ALT, 0.85))
   gfx.draw_rect_lines(area_x, area_y, area_w, area_h, C_LINE, 1.5)
   if frame_count < 4 { print("[monitor:trace] chrome.drawn") }
   if monitor_count == 0 {
      gfx.draw_text(font_body, "No monitors reported by the active backend.", area_x + 28.0, area_y + 34.0, C_MUTED)
   } else {
      def wp = window.pos(win)
      def bounds = view_window.desktop_bounds_with_window(monitor_rows, wp, ws)
      if frame_count < 4 { print("[monitor:trace] wp=" + to_str(wp.get(0, -1)) + "," + to_str(wp.get(1, -1)) + " ws=" + to_str(ws.get(0, -1)) + "x" + to_str(ws.get(1, -1)) + " bounds=" + to_str(bounds)) }
      def map = view_window.desktop_map(bounds, area_x, area_y, area_w, area_h)
      if frame_count < 4 { print("[monitor:trace] map=" + to_str(map)) }
      def scale = float(map.get("scale", 1.0))
      mut i = 0
      while i < monitor_count {
         def row = monitor_rows.get(i, {})
         def mon = view_window.map_desktop_rect(map, row.get("rect", [0, 0, 1, 1]))
         view_window.draw_monitor_info(font_body, font_small, row,
            float(mon.get(0, 0.0)), float(mon.get(1, 0.0)),
            float(mon.get(2, 1.0)), float(mon.get(3, 1.0)),
         i == current, scale)
         i += 1
      }
      if frame_count < 4 { print("[monitor:trace] monitors.drawn") }
      def wr = view_window.map_desktop_rect(map, [
            int(wp.get(0, 0)), int(wp.get(1, 0)),
            max_int(1, int(ws.get(0, 1))), max_int(1, int(ws.get(1, 1)))
      ], 3.0, 3.0)
      view_window.draw_window_marker(font_small, wr)
      if frame_count < 4 { print("[monitor:trace] window.marker.drawn") }
   }
   def f64 info_w = min_f64(430.0, max_f64(240.0, area_w - 24.0))
   def info_h = 80.0
   view_window.draw_dpi_info_text(font_small, area_x + 12.0, area_y + area_h - info_h - 12.0, info_w, info_h, hud_dpi_line1, hud_dpi_line2, state)
   if frame_count < 4 { print("[monitor:trace] dpi.drawn") }
   def f64 flags_w = min_f64(360.0, max_f64(250.0, area_w - 24.0))
   if area_w >= 680.0 {
      view_window.draw_window_flags(font_small, area_x + area_w - flags_w - 22.0, area_y + area_h - 166.0, flags_w, state)
   } elif area_h >= 340.0 {
      view_window.draw_window_flags(font_small, area_x + 22.0, area_y + area_h - 258.0, flags_w, state)
   }
   if frame_count < 4 { print("[monitor:trace] flags.drawn") }
   gfx.draw_circle(mx, my, 8.0, C_ACCENT_HI)
   gfx.draw_line_2d(mx - 18.0, my, mx + 18.0, my, gfx.color_alpha(C_ACCENT_HI, 0.70), 1.0)
   gfx.draw_line_2d(mx, my - 18.0, mx, my + 18.0, gfx.color_alpha(C_ACCENT_HI, 0.70), 1.0)
   auto_dump_pre_frame(auto_dump_done, frame_count, auto_dump_delay)
   gfx.end_frame()
   if frame_count < 4 { print("[monitor:trace] frame.present") }
   frame_count += 1
   auto_dump_done = auto_dump_post_frame(win, auto_dump_done, frame_count, auto_dump_delay, "build/cache/fb/ui/monitor.png")
   ui_runtime.close_on_timeout(win, int(fps_state.get("start", 0)), timeout_limit)
}

ui_runtime.fps_finish("monitor", fps_state)
gfx.close_window()
exit(exit_status(fx))
