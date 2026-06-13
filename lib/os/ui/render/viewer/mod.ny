;; Keywords: viewer app runtime ui editor os render
;; Public viewer facade that wires runtime, window, input, and UI helpers together.
;; References:
;; - std.os.ui.render.viewer.runtime
;; - std.os.ui.render.viewer.loop
module std.os.ui.render.viewer(
   set_font, begin_text, flush_text, text_run_count, text_char_count,
   FONT_CANDIDATES, TERMINAL_FONT_DEFAULT, TERMINAL_FONT_CANDIDATES,
   default_override, default_font_size, font_filter_mode, default_font_filter,
   font_from_candidates, terminal_font_map, terminal_cell_size, normalize_mod,
   framebuffer_size, mouse_view, hit,
   text_w, queue_text, fit_text, draw_text_fit, draw_text_right_fit, line_h,
   rect, panel, draw_axis_meter, draw_button_chip, draw_stage_frame,
   controller_fit_scale, draw_controller,
   C_BG, C_PANEL, C_PANEL_ALT, C_IDLE, C_TEXT, C_MUTED, C_SUBTLE, C_ACCENT,
   C_ACCENT_SOFT, C_ACTIVE, C_ACTIVE_SOFT, C_BLACK, C_MID, C_STICK, CONTROLLER_DRAW_SCALE,
   term, vterm, gamepad, dock, keyboard, clipboard, widgets, assets, icons, gizmo, gui, editor,
   app, batch, bootstrap, idle, loop, runtime
)

use std.core
use std.core.common as common
use std.math (clamp, max)
use std.os.path as ospath
use std.os.ui.window.consts
use std.os.ui.render
use std.os.ui.window as ui_window
use std.os.ui.window.native as win_native
use std.os.ui.render.viewer.term
use std.os.ui.render.viewer.vterm
use std.os.ui.render.viewer.gamepad
use std.os.ui.render.viewer.dock
use std.os.ui.render.viewer.keyboard
use std.os.ui.render.viewer.clipboard
use std.os.ui.render.viewer.widgets
use std.os.ui.assets.viewer as assets
use std.os.ui.render.viewer.icons
use std.os.ui.render.viewer.gizmo
use std.os.ui.render.viewer.gui
use std.os.ui.render.viewer.editor
use std.os.ui.render.viewer.app
use std.os.ui.render.viewer.batch
use std.os.ui.render.viewer.bootstrap
use std.os.ui.render.viewer.idle
use std.os.ui.render.viewer.loop
use std.os.ui.render.viewer.runtime
use std.core.str as str

mut font = 0
mut _text_width_cache = dict(64)
mut _line_h_cache = -1.0
mut _text_runs = []
mut _text_run_count = 0
mut _text_char_count = 0
def C_BG = color_hex("#000000")
def C_PANEL = color_hex("#080808")
def C_PANEL_ALT = color_hex("#131318")
def C_IDLE = color_hex("#15151b")
def C_TEXT = color_hex("#f5f5f6")
def C_MUTED = color_hex("#c6c6ca")
def C_SUBTLE = color_hex("#808087")
def C_ACCENT = color_hex("#9f86d9")
def C_ACCENT_SOFT = color_hex("#181321")
def C_ACTIVE = color_hex("#563d7c")
def C_ACTIVE_SOFT = color_hex("#261b35")
def C_ACTIVE_HI = color_hex("#bda9ec")
def C_ACTIVE_RING = color_hex("#6e5a96")
def C_BLACK = color_hex("#000000")
def C_MID = color_hex("#282531")
def C_STICK = color_hex("#101014")
;; Controller-only palette. Keep this isolated from generic GUI button colors.
;; Important: face/menu/bumper/D-pad/stick/trigger all reuse this same palette.
;; There are no per-button colors, so A/B/X/Y, menu buttons, and bumpers cannot drift
;; into green/blue/red depending on the control identity or a shared GUI theme tweak.
def C_PAD_IDLE = color_hex("#141319")
def C_PAD_EDGE = color_hex("#272432")
def C_PAD_DOWN = color_hex("#5d3f8c")
def C_PAD_DOWN_EDGE = C_PAD_EDGE
def C_PAD_AXIS = C_PAD_DOWN
def C_PAD_MARK = C_PAD_DOWN
def CONTROLLER_DRAW_SCALE = 0.86
def FONT_CANDIDATES = assets.MONO_FONT_CANDIDATES
def TERMINAL_FONT_DEFAULT = assets.TERM_FONT_DEFAULT
def TERMINAL_FONT_CANDIDATES = assets.TERM_FONT_CANDIDATES

fn _packed_color(color) int {
   if(is_int(color)){ return int(color) }
   color_pack(
      float(color.get(0, 1.0)),
      float(color.get(1, 1.0)),
      float(color.get(2, 1.0)),
      float(color.get(3, 1.0)),
   )
}

fn rect(any x, any y, any w, any h, any color) bool {
   draw_rect_fast(float(x), float(y), float(w), float(h), _packed_color(color))
}

fn _round_rect(any x, any y, any w, any h, any radius, any color) bool {
   draw_rounded_rectangle_sdf(x, y, w, h, radius, color)
}

fn _disc(any cx, any cy, any radius, any color) bool {
   draw_rounded_rectangle_sdf(cx - radius, cy - radius, radius * 2.0, radius * 2.0, radius, color)
}

fn text_w(any label) f64 {
   def cached = _text_width_cache.get(label, -1.0)
   if(cached >= 0.0){ return cached }
   def tw = float(measure_text_fast(font, label).get(0, 0.0))
   _text_width_cache[label] = tw
   tw
}

fn queue_text(any label, any x, any y, any color) int {
   def s = to_str(label)
   if(s.len <= 0){ return 0 }
   _text_run_count += 1
   _text_char_count += s.len
   mut runs = is_list(_text_runs) ? _text_runs : []
   runs = runs.append(s)
   runs = runs.append(float(x))
   runs = runs.append(float(y))
   runs = runs.append(_packed_color(color))
   _text_runs = runs
   0
}

fn flush_text() int {
   if(is_list(_text_runs) && _text_runs.len > 0){
      draw_text_runs_flat_colors(font, _text_runs)
      _text_runs = []
   }
   0
}

fn fit_text(any label, any max_w) str {
   if(max_w <= 0.0){ return "" }
   def s = to_str(label)
   if(text_w(s) <= max_w){ return s }
   def ell = "..."
   def ell_w = text_w(ell)
   if(max_w <= ell_w){ return "" }
   mut hi = s.len
   while(hi > 0){
      def cand = str.str_slice(s, 0, hi) + ell
      if(text_w(cand) <= max_w){ return cand }
      hi -= 1
   }
   ell
}

fn draw_text_fit(any label, any x, any y, any max_w, any color) int {
   def s = fit_text(label, max_w)
   if(s.len > 0){ queue_text(s, x, y, color) }
   0
}

fn draw_text_right_fit(any label, any right_x, any y, any max_w, any color) int {
   def s = fit_text(label, max_w)
   if(s.len > 0){ queue_text(s, right_x - text_w(s), y, color) }
   0
}

fn _pad_fill(bool down) any { down ? C_PAD_DOWN : C_PAD_IDLE }

fn _pad_edge(bool down) any { down ? C_PAD_DOWN_EDGE : C_PAD_EDGE }

fn _btn_color(any st, any button) any { _pad_fill(gamepad.pad_button(st, button)) }

fn _btn_ring_color(any st, any button) any { _pad_edge(gamepad.pad_button(st, button)) }

fn _trigger01(any value) f64 {
   ;; Backends differ: some expose triggers as -1..1, others as 0..1.
   ;; Treat exact/near zero as rest so a resting trigger does not draw a half-lit bar.
   def v = clamp(float(value), -1.0, 1.0)
   if(v <= -0.05){ return clamp((v + 1.0) * 0.5, 0.0, 1.0) }
   clamp(v, 0.0, 1.0)
}

fn panel(any x, any y, any w, any h, any title, any accent=0) int {
   rect(x, y, w, h, C_PANEL)
   rect(x + 1.0, y + 1.0, w - 2.0, h - 2.0, C_PANEL_ALT)
   rect(x, y, w, 3.0, accent ? accent : C_SUBTLE)
   draw_text_fit(title, x + 16.0, y + 26.0, max(24.0, w - 32.0), C_TEXT)
   0
}

fn draw_axis_meter(any label, any value, any x, any y, any w) int {
   def axis_i = gamepad.axis_i100(value)
   def mag_i = axis_i < 0 ? -axis_i : axis_i
   def fill_col = mag_i > 4 ? C_PAD_AXIS : C_SUBTLE
   def value_s = gamepad.fixed2(value)
   def value_w = text_w(value_s)
   def bar_x = x + 32.0
   def bar_w = max(24.0, w - 44.0 - value_w)
   def mid_x = bar_x + bar_w * 0.5
   queue_text(label, x, y + 12.0, C_MUTED)
   rect(bar_x, y + 5.0, bar_w, 8.0, C_IDLE)
   rect(mid_x - 1.0, y + 4.0, 2.0, 10.0, C_SUBTLE)
   if(axis_i >= 0){
      rect(mid_x, y + 5.0, (bar_w * 0.5) * gamepad.axis_f(axis_i), 8.0, fill_col)
   } else {
      def neg_w = (bar_w * 0.5) * gamepad.axis_f(-axis_i)
      rect(mid_x - neg_w, y + 5.0, neg_w, 8.0, fill_col)
   }
   queue_text(value_s, x + w - value_w, y + 12.0, C_TEXT)
   0
}

fn draw_button_chip(any label, any active, any x, any y, any w) int {
   rect(x, y, w, 22.0, active ? C_PAD_DOWN : C_IDLE)
   rect(x, y, w, active ? 2.0 : 1.0, active ? C_PAD_DOWN_EDGE : C_SUBTLE)
   def shown = fit_text(label, max(0.0, w - 8.0))
   def tw = text_w(shown)
   def tx = x + ((w - tw) * 0.5)
   if(shown.len > 0){ queue_text(shown, tx, y + 15.0, C_TEXT) }
   0
}

fn draw_stage_frame(any x, any y, any w, any h) int {
   rect(x, y, w, h, C_PANEL)
   rect(x + 1.0, y + 1.0, w - 2.0, h - 2.0, C_BG)
   rect(x, y, w, 2.0, C_SUBTLE)
   rect(x, y + h - 1.0, w, 1.0, C_SUBTLE)
   rect(x, y, 1.0, h, C_SUBTLE)
   rect(x + w - 1.0, y, 1.0, h, C_SUBTLE)
   0
}

fn _gx(f64 off_x, f64 s, f64 v) f64 { off_x + v * s }

fn _gy(f64 off_y, f64 s, f64 v) f64 { off_y + v * s }

fn _gs(f64 s, f64 v) f64 { v * s }

fn _draw_pad_shell(f64 off_x, f64 off_y, f64 s, any st, f64 lt, f64 rt) int {
   _round_rect(_gx(off_x, s, 175), _gy(off_y, s, 110), _gs(s, 460), _gs(s, 220), _gs(s, 33), C_PANEL)
   _round_rect(_gx(off_x, s, 215), _gy(off_y, s, 98), _gs(s, 100), _gs(s, 10), _gs(s, 5), _btn_color(st, win_native.GAMEPAD_BUTTON_LEFT_BUMPER))
   _round_rect(_gx(off_x, s, 495), _gy(off_y, s, 98), _gs(s, 100), _gs(s, 10), _gs(s, 5), _btn_color(st, win_native.GAMEPAD_BUTTON_RIGHT_BUMPER))
   def tx_l = _gx(off_x, s, 151)
   def tx_r = _gx(off_x, s, 644)
   def ty = _gy(off_y, s, 110)
   def tw = _gs(s, 15)
   def th = _gs(s, 70)
   _round_rect(tx_l, ty, tw, th, _gs(s, 5), C_IDLE)
   _round_rect(tx_r, ty, tw, th, _gs(s, 5), C_IDLE)
   def lt_t = _trigger01(lt)
   def rt_t = _trigger01(rt)
   def lt_h = th * lt_t
   def rt_h = th * rt_t
   if(lt_t > 0.01){
      def fill = lt_h < _gs(s, 4) ? _gs(s, 4) : lt_h
      _round_rect(tx_l, ty + th - fill, tw, fill, _gs(s, 5), C_PAD_DOWN)
   }
   if(rt_t > 0.01){
      def fill = rt_h < _gs(s, 4) ? _gs(s, 4) : rt_h
      _round_rect(tx_r, ty + th - fill, tw, fill, _gs(s, 5), C_PAD_DOWN)
   }
   rect(tx_l - _gs(s, 2), ty + th - lt_h, tw + _gs(s, 4), max(1.0, _gs(s, 2)), C_PAD_MARK)
   rect(tx_r - _gs(s, 2), ty + th - rt_h, tw + _gs(s, 4), max(1.0, _gs(s, 2)), C_PAD_MARK)
   return 0
}

fn _draw_menu_cluster(f64 off_x, f64 off_y, f64 s, any st) int {
   _disc(_gx(off_x, s, 365), _gy(off_y, s, 170), _gs(s, 12), _btn_ring_color(st, win_native.GAMEPAD_BUTTON_BACK))
   _disc(_gx(off_x, s, 405), _gy(off_y, s, 170), _gs(s, 12), _btn_ring_color(st, win_native.GAMEPAD_BUTTON_GUIDE))
   _disc(_gx(off_x, s, 445), _gy(off_y, s, 170), _gs(s, 12), _btn_ring_color(st, win_native.GAMEPAD_BUTTON_START))
   _disc(_gx(off_x, s, 365), _gy(off_y, s, 170), _gs(s, 9), _btn_color(st, win_native.GAMEPAD_BUTTON_BACK))
   _disc(_gx(off_x, s, 405), _gy(off_y, s, 170), _gs(s, 9), _btn_color(st, win_native.GAMEPAD_BUTTON_GUIDE))
   _disc(_gx(off_x, s, 445), _gy(off_y, s, 170), _gs(s, 9), _btn_color(st, win_native.GAMEPAD_BUTTON_START))
   return 0
}

fn _draw_face_button(f64 off_x, f64 off_y, f64 s, any st, int button, f64 cx, f64 cy) int {
   def down = gamepad.pad_button(st, button)
   _disc(_gx(off_x, s, cx), _gy(off_y, s, cy), _gs(s, 17), down ? C_PAD_DOWN_EDGE : C_PAD_EDGE)
   _disc(_gx(off_x, s, cx), _gy(off_y, s, cy), _gs(s, 14), down ? C_PAD_DOWN : C_PAD_IDLE)
   return 0
}

fn _draw_face_cluster(f64 off_x, f64 off_y, f64 s, any st) int {
   _draw_face_button(off_x, off_y, s, st, win_native.GAMEPAD_BUTTON_SQUARE, 516, 191)
   _draw_face_button(off_x, off_y, s, st, win_native.GAMEPAD_BUTTON_CROSS, 551, 227)
   _draw_face_button(off_x, off_y, s, st, win_native.GAMEPAD_BUTTON_CIRCLE, 587, 191)
   _draw_face_button(off_x, off_y, s, st, win_native.GAMEPAD_BUTTON_TRIANGLE, 551, 155)
   return 0
}

fn _draw_dpad(f64 off_x, f64 off_y, f64 s, any st) int {
   _round_rect(_gx(off_x, s, 245), _gy(off_y, s, 145), _gs(s, 28), _gs(s, 88), _gs(s, 4), C_PAD_EDGE)
   _round_rect(_gx(off_x, s, 215), _gy(off_y, s, 174), _gs(s, 88), _gs(s, 29), _gs(s, 4), C_PAD_EDGE)
   _round_rect(_gx(off_x, s, 247), _gy(off_y, s, 147), _gs(s, 24), _gs(s, 84), _gs(s, 4), C_PAD_IDLE)
   _round_rect(_gx(off_x, s, 217), _gy(off_y, s, 176), _gs(s, 84), _gs(s, 25), _gs(s, 4), C_PAD_IDLE)
   def dc_x = _gx(off_x, s, 259)
   def dc_y = _gy(off_y, s, 188.5)
   if(gamepad.pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_UP)){
      _round_rect(_gx(off_x, s, 247), _gy(off_y, s, 147), _gs(s, 24), _gs(s, 29), _gs(s, 4), C_PAD_DOWN)
      rect(_gx(off_x, s, 247), _gy(off_y, s, 158), _gs(s, 24), _gs(s, 18), C_PAD_DOWN)
      draw_triangle([dc_x, dc_y, 0.0], [_gx(off_x, s, 247), _gy(off_y, s, 176), 0.0], [_gx(off_x, s, 271), _gy(off_y, s, 176), 0.0], C_PAD_DOWN)
   }
   if(gamepad.pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_DOWN)){
      _round_rect(_gx(off_x, s, 247), _gy(off_y, s, 201), _gs(s, 24), _gs(s, 30), _gs(s, 4), C_PAD_DOWN)
      rect(_gx(off_x, s, 247), _gy(off_y, s, 201), _gs(s, 24), _gs(s, 16), C_PAD_DOWN)
      draw_triangle([dc_x, dc_y, 0.0], [_gx(off_x, s, 271), _gy(off_y, s, 201), 0.0], [_gx(off_x, s, 247), _gy(off_y, s, 201), 0.0], C_PAD_DOWN)
   }
   if(gamepad.pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_LEFT)){
      _round_rect(_gx(off_x, s, 217), _gy(off_y, s, 176), _gs(s, 30), _gs(s, 25), _gs(s, 4), C_PAD_DOWN)
      rect(_gx(off_x, s, 232), _gy(off_y, s, 176), _gs(s, 15), _gs(s, 25), C_PAD_DOWN)
      draw_triangle([dc_x, dc_y, 0.0], [_gx(off_x, s, 247), _gy(off_y, s, 201), 0.0], [_gx(off_x, s, 247), _gy(off_y, s, 176), 0.0], C_PAD_DOWN)
   }
   if(gamepad.pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_RIGHT)){
      _round_rect(_gx(off_x, s, 271), _gy(off_y, s, 176), _gs(s, 30), _gs(s, 25), _gs(s, 4), C_PAD_DOWN)
      rect(_gx(off_x, s, 271), _gy(off_y, s, 176), _gs(s, 15), _gs(s, 25), C_PAD_DOWN)
      draw_triangle([dc_x, dc_y, 0.0], [_gx(off_x, s, 271), _gy(off_y, s, 176), 0.0], [_gx(off_x, s, 271), _gy(off_y, s, 201), 0.0], C_PAD_DOWN)
   }
   return 0
}

fn _draw_stick(f64 off_x, f64 off_y, f64 s, any st, f64 cx, f64 cy, f64 ax, f64 ay, int button) int {
   def sx = clamp(ax, -1.0, 1.0)
   def sy = clamp(ay, -1.0, 1.0)
   def center_x = _gx(off_x, s, cx)
   def center_y = _gy(off_y, s, cy)
   def travel = _gs(s, 30)
   def knob_x = center_x + sx * travel
   def knob_y = center_y + sy * travel
   def knob = gamepad.pad_button(st, button) ? C_PAD_DOWN : C_STICK
   _disc(center_x, center_y, _gs(s, 42), C_BLACK)
   _disc(center_x, center_y, _gs(s, 36), C_PAD_IDLE)
   rect(center_x - travel, center_y - max(1.0, _gs(s, 1)), travel * 2.0, max(1.0, _gs(s, 2)), C_PAD_EDGE)
   rect(center_x - max(1.0, _gs(s, 1)), center_y - travel, max(1.0, _gs(s, 2)), travel * 2.0, C_PAD_EDGE)
   if(gamepad.axis_i100(sx) != 0 || gamepad.axis_i100(sy) != 0){
      def shaft_r = max(2.0, _gs(s, 4))
      draw_line_2d(center_x, center_y, knob_x, knob_y, C_PAD_EDGE, shaft_r * 2.0)
      _disc(center_x, center_y, shaft_r, C_PAD_EDGE)
   }
   _disc(knob_x, knob_y, _gs(s, 25), knob)
   return 0
}

fn draw_controller(any st, f64 x, f64 y, f64 w, f64 h) int {
   def ref_min_x = 151.0
   def ref_min_y = 98.0
   def ref_w = 508.0
   def ref_h = 232.0
   def zone_x = x
   def zone_y = y
   def zone_w = max(220.0, w)
   def zone_h = max(140.0, h)
   def sx = zone_w / ref_w
   def sy = zone_h / ref_h
   def fit_s = (sx < sy) ? sx : sy
   def s = fit_s * CONTROLLER_DRAW_SCALE
   def off_x = zone_x + (zone_w - ref_w * s) * 0.5 - ref_min_x * s
   def off_y = zone_y + (zone_h - ref_h * s) * 0.5 - ref_min_y * s
   def lx = gamepad.display_axis_slot(st, win_native.GAMEPAD_AXIS_LEFT_X)
   def ly = gamepad.display_axis_slot(st, win_native.GAMEPAD_AXIS_LEFT_Y)
   def rx = gamepad.display_axis_slot(st, win_native.GAMEPAD_AXIS_RIGHT_X)
   def ry = gamepad.display_axis_slot(st, win_native.GAMEPAD_AXIS_RIGHT_Y)
   def lt = gamepad.display_axis_slot(st, win_native.GAMEPAD_AXIS_LEFT_TRIGGER)
   def rt = gamepad.display_axis_slot(st, win_native.GAMEPAD_AXIS_RIGHT_TRIGGER)
   _draw_pad_shell(off_x, off_y, s, st, lt, rt)
   _draw_menu_cluster(off_x, off_y, s, st)
   _draw_face_cluster(off_x, off_y, s, st)
   _draw_dpad(off_x, off_y, s, st)
   _draw_stick(off_x, off_y, s, st, 345, 260, lx, ly, win_native.GAMEPAD_BUTTON_LEFT_THUMB)
   _draw_stick(off_x, off_y, s, st, 465, 260, rx, ry, win_native.GAMEPAD_BUTTON_RIGHT_THUMB)
   return 0
}

fn line_h() f64 {
   if(_line_h_cache > 0.0){ return _line_h_cache }
   def sz = measure_text_fast(font, "Mg")
   def h = float(sz.get(1, 18.0))
   _line_h_cache = h > 0.0 ? h : 18.0
   _line_h_cache
}

fn controller_fit_scale(any w, any h) f64 {
   if(w <= 0.0 || h <= 0.0){ return 0.0 }
   min(w / 508.0, h / 232.0) * CONTROLLER_DRAW_SCALE
}

fn framebuffer_size(any win, any fallback_w=1280, any fallback_h=720) list {
   def fb = ui_window.get_framebuffer_size(win)
   [max(1.0, float(fb.get(0, fallback_w))), max(1.0, float(fb.get(1, fallback_h)))]
}

fn mouse_view(any win, any fallback_w=1280, any fallback_h=720) list {
   def fb = framebuffer_size(win, fallback_w, fallback_h)
   def ws = ui_window.size(win)
   def mouse = ui_window.mouse_pos(win)
   def sw = float(fb.get(0, fallback_w))
   def sh = float(fb.get(1, fallback_h))
   def ww = max(1.0, float(ws.get(0, fallback_w)))
   def wh = max(1.0, float(ws.get(1, fallback_h)))
   [sw, sh, float(mouse.get(0, 0.0)) * sw / ww, float(mouse.get(1, 0.0)) * sh / wh]
}

fn hit(f64 x, f64 y, f64 w, f64 h, f64 mx, f64 my) bool {
   mx >= x && mx <= x + w && my >= y && my <= y + h
}

fn normalize_mod(any mods) int {
   int(mods) & (MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META)
}

fn default_override(any tag, any suffix) str {
   def key_tag = "NY_" + str.upper(to_str(tag)) + "_" + str.upper(to_str(suffix))
   def key_shared = "NY_UI_" + str.upper(to_str(suffix))
   def tag_value = common.env_trim(key_tag)
   if(tag_value.len > 0){ return tag_value }
   common.env_trim(key_shared)
}

fn default_font_size(any tag, f64 fallback, str suffix="FONT_SIZE") any {
   def raw = default_override(tag, suffix)
   if(raw.len == 0){ return fallback }
   min(max(str.atof(raw), 8.0), 96.0)
}

fn font_filter_mode(any mode, int fallback) int {
   case mode {
      "nearest", "point", "pixel" -> { return FONT_FILTER_NEAREST }
      "linear", "bilinear", "smooth" -> { return FONT_FILTER_LINEAR }
      _ -> fallback
   }
}

fn default_font_filter(any tag, int fallback=FONT_FILTER_DEFAULT, str suffix="FONT_FILTER") int {
   def raw = default_override(tag, suffix)
   raw.len == 0 ? fallback : font_filter_mode(str.lower(str.strip(raw)), fallback)
}

fn font_from_candidates(int size, list candidates=FONT_CANDIDATES, int font_filter=-1) int {
   mut i = 0
   while(i < candidates.len){
      def raw = candidates.get(i, "")
      def resolved = ospath.resolve_repo_asset(raw)
      def path = resolved.len > 0 ? resolved : raw
      def filter = str.find(str.lower(raw), "monocraft") >= 0 ? FONT_FILTER_NEAREST : font_filter
      def f = font_load(path, size, filter)
      if(f){ return f }
      i += 1
   }
   0
}

fn terminal_font_map(any size, int font_filter=FONT_FILTER_NEAREST, int emoji_filter=FONT_FILTER_LINEAR, str reg_path="", str bold_path="", str ital_path="", str emoji_path="", bool emoji_on=true) dict {
   mut f = 0
   def font_size = int(size)
   if(reg_path.len > 0){
      f = font_load(reg_path, font_size, font_filter)
   } else {
      f = font_from_candidates(font_size, TERMINAL_FONT_CANDIDATES, font_filter)
   }
   if(!f){ f = font_load(TERMINAL_FONT_DEFAULT, font_size, font_filter) }
   mut bold = f
   mut italic = f
   mut emoji = f
   if(bold_path.len > 0){ bold = common.value_or(font_load(bold_path, font_size, font_filter), f) }
   if(ital_path.len > 0){ italic = common.value_or(font_load(ital_path, font_size, font_filter), f) }
   if(emoji_on){
      def ep = (emoji_path.len > 0) ? emoji_path : "/usr/share/fonts/noto/NotoColorEmoji.ttf"
      emoji = common.value_or(font_load(ep, font_size, emoji_filter), f)
   }
   {"regular": f, "bold": bold, "italic": italic, "emoji": emoji}
}

fn terminal_cell_size(any font_id, any font_size) list {
   def fs = float(font_size)
   if(!font_id){ return [max(1.0, fs * 0.6), max(1.0, max(fs, 20.0))] }
   def probe = measure_text_fast(font_id, "M")
   mut cw, ch = float(probe.get(0, 0.0)), float(probe.get(1, 0.0))
   if(cw <= 1.0){ cw = max(cw, float(measure_text_fast(font_id, "A").get(0, 0.0))) }
   if(cw <= 1.0){ cw = max(cw, float(measure_text_fast(font_id, "i").get(0, 0.0))) }
   if(cw <= 1.0){ cw = fs * 0.6 }
   if(ch <= 1.0){ ch = fs }
   [max(1.0, float(int(cw + 0.5))), max(1.0, float(int(ch + 0.5)))]
}

fn set_font(any f) int {
   "Sets the font used by viewer text batching."
   font = f
   _text_width_cache = dict(64)
   _line_h_cache = -1.0
   0
}

fn begin_text() int {
   "Clears queued text and frame counters."
   _text_runs = []
   _text_run_count = 0
   _text_char_count = 0
   0
}

fn text_run_count() int {
   "Returns queued text run count for the current frame."
   _text_run_count
}

fn text_char_count() int {
   "Returns queued text character count for the current frame."
   _text_char_count
}

#main {
   assert(controller_fit_scale(508.0, 232.0) > 0.0, "gamepad viewer fit scale")
   assert(hit(0.0, 0.0, 4.0, 4.0, 2.0, 2.0) && !hit(0.0, 0.0, 4.0, 4.0, 5.0, 2.0), "viewer hit")
   assert(normalize_mod(MOD_CONTROL | 0x100000) == MOD_CONTROL, "viewer mod mask")
   assert(terminal_cell_size(0, 16.0).get(1) >= 16.0, "viewer terminal cell size")
   print("✓ std.os.ui.render.viewer self-test passed")
}

