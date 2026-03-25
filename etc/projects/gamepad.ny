#!/usr/bin/env ny

;; Keywords: platform probe ui app example gamepad font
;; Nytrix Gamepad
use std.core
use std.os (ticks, msleep, exit)
use std.math.float as fmath
use std.os.ui.render
use std.os.ui.window as window
use std.os.ui.window.native as win_native
use std.os.ui.app as ui_app
use std.core.str as str
use std.util.common as common
use std.os.ui.consts
use std.os.ui.runtime as exutil
use std.demo as demo

mut font = 0
mut _device_rows = []
mut _best_jid_cached = -1
mut _last_device_scan_ticks = 0
mut _text_width_cache = dict(64)
def DEVICE_SCAN_INTERVAL_NS = 1000000000
def DEVICE_SCAN_EMPTY_INTERVAL_NS = 250000000
def C_BG = color_hex("#000000")
def C_PANEL = color_hex("#080808")
def C_PANEL_ALT = color_hex("#131318")
def C_IDLE = color_hex("#1d1d24")
def C_TEXT = color_hex("#f3f3f3")
def C_MUTED = color_hex("#c0c0c8")
def C_SUBTLE = color_hex("#7a7a84")
def C_ACCENT = color_hex("#bb86fc")
def C_ACCENT_SOFT = color_hex("#5f3b80")
def C_BLACK = color_hex("#000000")
def C_MID = color_hex("#4c4c52")
def C_STICK = color_hex("#121216")
def CONTROLLER_DRAW_SCALE = 0.86
def LEGEND_ACTIVE_RESERVE = "ACTIVE L1 R1 L2 R2 START"
def LEGEND_HAT_RESERVE = "HATS H0=15 H1=15"

fn _dbg(msg){
   if(common.env_enabled("NY_GAMEPAD_DEBUG")){ print("[gamepad] " + to_str(msg)) }
   0
}

fn _env_dim(name, fallback){
   def raw = common.env_trim(to_str(name))
   if(raw.len == 0){ return fallback }
   def parsed = int(str.atof(raw))
   if(parsed <= 0){ return fallback }
   parsed
}

fn _clampf(v, lo, hi){
   if(v < lo){ return lo }
   if(v > hi){ return hi }
   v
}

fn _absf(v){
   v < 0.0 ? (0.0 - v) : v
}

fn _safe_axis_value(v){
   def fv = fmath.float(v)
   if(fmath.is_nan(fv) || fmath.is_inf(fv)){ return 0.0 }
   if(fv < -1.0){ return -1.0 }
   if(fv > 1.0){ return 1.0 }
   fv
}

fn _fixed2(v){
   to_str(float(int(_safe_axis_value(v) * 100.0)) / 100.0)
}

fn _disc(cx, cy, radius, color){
   def d = radius * 2.0
   draw_rect_rounded(cx - radius, cy - radius, d, d, radius, color)
}

fn _text_w(label){
   def cached = _text_width_cache.get(label, -1.0)
   if(cached >= 0.0){ return cached }
   def tw = float(measure_text(font, label).get(0, 0.0))
   _text_width_cache[label] = tw
   tw
}

fn _fit_text(label, max_w){
   if(max_w <= 0.0){ return "" }
   def s = to_str(label)
   if(_text_w(s) <= max_w){ return s }
   def ell = "..."
   def ell_w = _text_w(ell)
   if(max_w <= ell_w){ return "" }
   mut hi = s.len
   while(hi > 0){
      def cand = str.str_slice(s, 0, hi) + ell
      if(_text_w(cand) <= max_w){ return cand }
      hi -= 1
   }
   ell
}

fn _draw_text_fit(label, x, y, max_w, color){
   def s = _fit_text(label, max_w)
   if(s.len > 0){ draw_text(font, s, x, y, color) }
   0
}

fn _draw_text_right_fit(label, right_x, y, max_w, color){
   def s = _fit_text(label, max_w)
   if(s.len > 0){ draw_text(font, s, right_x - _text_w(s), y, color) }
   0
}

def _BTN_LABELS_GENERIC = [
   "A", "B", "X", "Y", "LB", "RB", "BACK", "START", "GUIDE", "L3", "R3", "UP", "RIGHT", "DOWN", "LEFT", "EX15", "EX16"
]

def _BTN_LABELS_SONY = [
   "X", "O", "SQ", "TRI", "L1", "R1", "SHARE", "OPT", "PS", "L3", "R3", "UP", "RIGHT", "DOWN", "LEFT", "TOUCH", "MIC"
]

def _BTN_CHIP_LABELS_GENERIC = [
   "A", "B", "X", "Y", "LB", "RB", "BK", "ST", "GD", "L3", "R3", "UP", "RGT", "DWN", "LFT", "15", "16"
]

def _BTN_CHIP_LABELS_SONY = [
   "X", "O", "SQ", "TRI", "L1", "R1", "SHR", "OPT", "PS", "L3", "R3", "UP", "RGT", "DWN", "LFT", "TCH", "MIC"
]

def _BTN_SIDE_LABELS_GENERIC = [
   "A", "B", "X", "Y", "LB", "RB", "BK", "ST", "GD", "L3", "R3", "U", "R", "D", "L", "15", "16"
]

def _BTN_SIDE_LABELS_SONY = [
   "X", "O", "SQ", "TR", "L1", "R1", "SH", "OP", "PS", "L3", "R3", "U", "R", "D", "L", "TC", "MC"
]

fn _is_sony_pad(st){
   def lname = str.lower(st.get("name", ""))
   str.find(lname, "sony") != -1 || str.find(lname, "dual") != -1 || str.find(lname, "playstation") != -1
}

fn _button_label(st, idx){
   if(!st.get("mapped", false)){ return "B" + to_str(idx) }
   if(_is_sony_pad(st)){ return _BTN_LABELS_SONY.get(idx, "B" + to_str(idx)) }
   _BTN_LABELS_GENERIC.get(idx, "B" + to_str(idx))
}

fn _button_chip_label(st, idx){
   if(!st.get("mapped", false)){ return "B" + to_str(idx) }
   if(_is_sony_pad(st)){ return _BTN_CHIP_LABELS_SONY.get(idx, "B" + to_str(idx)) }
   _BTN_CHIP_LABELS_GENERIC.get(idx, "B" + to_str(idx))
}

fn _button_side_label(st, idx){
   if(!st.get("mapped", false)){ return "B" + to_str(idx) }
   if(_is_sony_pad(st)){ return _BTN_SIDE_LABELS_SONY.get(idx, "B" + to_str(idx)) }
   _BTN_SIDE_LABELS_GENERIC.get(idx, "B" + to_str(idx))
}

fn _axis_label(idx){
   ["LX", "LY", "RX", "RY", "LT", "RT"].get(idx, "A" + to_str(idx))
}

fn _native_joysticks(){
   mut out = []
   mut jid = 0
   while(jid < 16){
      if(win_native.joystick_present(jid)){ out = out.append(jid) }
      jid += 1
   }
   out
}

fn _native_gamepad_name(jid){
   win_native.joystick_is_gamepad(jid) ? win_native.get_gamepad_name(jid) : win_native.get_joystick_name(jid)
}

fn _native_axis_count(jid, mapped){
   if(mapped){ return 6 }
   def count_ptr = malloc(4)
   if(!count_ptr){ return 0 }
   win_native.get_joystick_axes(jid, count_ptr)
   def n = load32(count_ptr, 0)
   free(count_ptr)
   n
}

fn _native_button_count(jid, mapped){
   if(mapped){ return 15 }
   def count_ptr = malloc(4)
   if(!count_ptr){ return 0 }
   win_native.get_joystick_buttons(jid, count_ptr)
   def n = load32(count_ptr, 0)
   free(count_ptr)
   n
}

fn _device_score(jid){
   def lname = str.lower(_native_gamepad_name(jid))
   mut score = 0
   if(win_native.joystick_is_gamepad(jid)){ score += 100 }
   if(str.find(lname, "controller") != -1){ score += 60 }
   if(str.find(lname, "gamepad") != -1){ score += 60 }
   if(str.find(lname, "xbox") != -1){ score += 60 }
   if(str.find(lname, "dual") != -1){ score += 70 }
   if(str.find(lname, "shock") != -1){ score += 50 }
   if(str.find(lname, "sony") != -1){ score += 60 }
   if(str.find(lname, "8bitdo") != -1){ score += 50 }
   if(str.find(lname, "logitech") != -1){ score += 40 }
   if(str.find(lname, "keyboard") != -1){ score -= 400 }
   if(str.find(lname, "mouse") != -1){ score -= 300 }
   if(str.find(lname, "touchpad") != -1){ score -= 250 }
   if(str.find(lname, "k400") != -1){ score -= 500 }
   score
}

fn _device_row(jid){
   def name = _native_gamepad_name(jid)
   def mapped = win_native.joystick_is_gamepad(jid)
   mut row = dict(4)
   row["jid"] = jid
   row["name"] = name
   row["mapped"] = mapped
   row["score"] = _device_score(jid)
   row
}

fn _pick_display_device(joysticks){
   mut best_jid = -1
   mut best_score = -100000
   mut fallback = -1
   def joysticks_n = joysticks.len
   mut i = 0
   while(i < joysticks_n){
      def jid = joysticks.get(i)
      if(fallback == -1){ fallback = jid }
      def score = _device_score(jid)
      if(score > best_score){
         best_score = score
         best_jid = jid
      }
      i += 1
   }
   if(best_jid != -1){ return best_jid }
   fallback
}

fn _best_jid_from_rows(rows){
   mut best_jid = -1
   mut best_score = -100000
   def rows_n = rows.len
   mut i = 0
   while(i < rows_n){
      def row = rows.get(i)
      def jid = row.get("jid", -1)
      def score = row.get("score", -100000)
      if(score > best_score){
         best_score = score
         best_jid = jid
      }
      i += 1
   }
   best_jid
}

fn _refresh_device_cache(force=false){
   def now = ticks()
   def scan_interval = _device_rows.len == 0 ? DEVICE_SCAN_EMPTY_INTERVAL_NS : DEVICE_SCAN_INTERVAL_NS
   if(!force && _last_device_scan_ticks != 0 && (now - _last_device_scan_ticks) < scan_interval){
      return 0
   }
   _dbg("refresh force=" + to_str(force) + " prev_rows=" + to_str(_device_rows.len))
   def joysticks = _native_joysticks()
   def joysticks_n = joysticks.len
   mut rows = []
   mut i = 0
   while(i < joysticks_n){
      rows = rows.append(_device_row(joysticks.get(i)))
      i += 1
   }
   _device_rows = rows
   _best_jid_cached = _best_jid_from_rows(rows)
   _last_device_scan_ticks = now
   _dbg("joysticks=" + to_str(joysticks) + " rows=" + to_str(_device_rows.len) + " best=" + to_str(_best_jid_cached))
   0
}

fn _cached_device_name(jid){
   def rows_n = _device_rows.len
   mut i = 0
   while(i < rows_n){
      def row = _device_rows.get(i)
      if(row.get("jid", -1) == jid){ return row.get("name", "") }
      i += 1
   }
   ""
}

fn _cached_device_mapped(jid){
   def rows_n = _device_rows.len
   mut i = 0
   while(i < rows_n){
      def row = _device_rows.get(i)
      if(row.get("jid", -1) == jid){ return row.get("mapped", false) }
      i += 1
   }
   false
}

fn _read_raw_state(jid, mapped){
   mut axis_count = _native_axis_count(jid, mapped)
   mut button_count = _native_button_count(jid, mapped)
   mut hat_count = 0
   mut axes_ptr = 0
   mut buttons_ptr = 0
   mut hats_ptr = 0
   def count_ptr = malloc(4)
   if(count_ptr){
      axes_ptr = win_native.get_joystick_axes(jid, count_ptr)
      if(!mapped){ axis_count = load32(count_ptr, 0) }
      buttons_ptr = win_native.get_joystick_buttons(jid, count_ptr)
      if(!mapped){ button_count = load32(count_ptr, 0) }
      hats_ptr = win_native.get_joystick_hats(jid, count_ptr)
      hat_count = load32(count_ptr, 0)
      free(count_ptr)
   }
   [axis_count, axes_ptr, button_count, buttons_ptr, hat_count, hats_ptr]
}

fn _read_mapped_state_ptr(jid, mapped): any {
   if(!mapped){ return 0 }
   def state_ptr = malloc(64)
   if(!state_ptr){ return 0 }
   if(win_native.get_gamepad_state(jid, state_ptr)){ return state_ptr }
   free(state_ptr)
   0
}

fn _collect_pressed_buttons(st){
   mut last_btn = -1
   mut pressed_count = 0
   mut pressed_labels = []
   mut bi = 0
   def button_count = st.get("button_count", 0)
   while(bi < button_count){
      if(_pad_button(st, bi)){
         last_btn = bi
         pressed_count += 1
         if(pressed_labels.len < 8){ pressed_labels = pressed_labels.append(_button_chip_label(st, bi)) }
      }
      bi += 1
   }
   [last_btn, pressed_count, pressed_labels]
}

fn _snapshot_pad_state(jid){
   _dbg("snapshot begin")
   mut st = dict(12)
   def cached_name = _cached_device_name(jid)
   def mapped = cached_name.len > 0 ? _cached_device_mapped(jid) : win_native.joystick_is_gamepad(jid)
   st["jid"] = jid
   st["mapped"] = mapped
   st["name"] = cached_name.len > 0 ? cached_name : _native_gamepad_name(jid)
   def raw = _read_raw_state(jid, mapped)
   st["axis_count"] = raw.get(0, 0)
   st["axes_ptr"] = raw.get(1, 0)
   st["button_count"] = raw.get(2, 0)
   st["buttons_ptr"] = raw.get(3, 0)
   st["hat_count"] = raw.get(4, 0)
   st["hats_ptr"] = raw.get(5, 0)
   _dbg("snapshot raw done")
   st["state_ptr"] = _read_mapped_state_ptr(jid, mapped)
   _dbg("snapshot mapped done")
   def pressed = _collect_pressed_buttons(st)
   st["last_button"] = pressed.get(0, -1)
   st["pressed_count"] = pressed.get(1, 0)
   st["pressed_labels"] = pressed.get(2, [])
   _dbg("snapshot done")
   st
}

fn _release_pad_state(st){
   def state_ptr = st.get("state_ptr", 0)
   if(state_ptr){ free(state_ptr) }
   0
}

fn _pad_button(st, button){
   def buttons = st.get("buttons", nil)
   if(is_list(buttons) && button >= 0 && button < buttons.len){ return bool(buttons.get(button, false)) }
   def state_ptr = st.get("state_ptr", 0)
   if(state_ptr && button >= 0 && button < 15){ return load8(state_ptr, button) != 0 }
   def buttons_ptr = st.get("buttons_ptr", 0)
   def button_count = st.get("button_count", 0)
   buttons_ptr && button >= 0 && button < button_count && load8(buttons_ptr, button) != 0
}

fn _pad_axis(st, axis){
   def axes = st.get("axes", nil)
   if(is_list(axes) && axis >= 0 && axis < axes.len){ return _safe_axis_value(axes.get(axis, 0.0)) }
   def state_ptr = st.get("state_ptr", 0)
   if(state_ptr && axis >= 0 && axis < 6){ return _safe_axis_value(load32_f32(state_ptr, 16 + axis * 4)) }
   def axes_ptr = st.get("axes_ptr", 0)
   def axis_count = st.get("axis_count", 0)
   (axes_ptr && axis >= 0 && axis < axis_count) ? _safe_axis_value(load32_f32(axes_ptr, axis * 4)) : 0.0
}

fn _btn_color(st, button){ _pad_button(st, button) ? C_ACCENT : C_IDLE }

fn _trigger_axis(st, mapped_axis, raw_axis){
   st.get("mapped", false) ? _pad_axis(st, mapped_axis) : _pad_axis(st, raw_axis)
}

fn _demo_rows(){
   [
      {"jid": 0, "name": "Demo DualSense Wireless Controller", "mapped": true, "score": 1000},
      {"jid": 1, "name": "Arcade Stick Raw HID", "mapped": false, "score": 10},
      {"jid": 2, "name": "8BitDo Lite 2", "mapped": true, "score": 800},
   ]
}

fn _demo_pad_state(){
   {
      "jid": 0,
      "mapped": true,
      "name": "Demo DualSense Wireless Controller",
      "axis_count": 6,
      "button_count": 17,
      "hat_count": 0,
      "axes": [-0.62, 0.34, 0.45, -0.28, -0.10, 0.72],
      "buttons": [true, false, true, false, true, false, false, true, false, false, true, true, false, false, false, false, false],
      "last_button": 10,
      "pressed_count": 5,
      "pressed_labels": ["X", "SQ", "L1", "OPT", "R3"],
   }
}

fn _panel(x, y, w, h, title, accent=0){
   draw_rect(x, y, w, h, C_PANEL)
   draw_rect(x + 1.0, y + 1.0, w - 2.0, h - 2.0, C_PANEL_ALT)
   if(accent){
      draw_rect(x, y, w, 3.0, accent)
   } else {
      draw_rect(x, y, w, 3.0, C_SUBTLE)
   }
   _draw_text_fit(title, x + 16.0, y + 26.0, max(24.0, w - 32.0), C_TEXT)
}

fn _draw_axis_meter(label, value, x, y, w){
   def mag = _absf(value)
   def fill_col = mag > 0.04 ? C_ACCENT : C_SUBTLE
   def value_s = _fixed2(value)
   def value_w = _text_w(value_s)
   def bar_x = x + 32.0
   def bar_w = max(24.0, w - 44.0 - value_w)
   def mid_x = bar_x + bar_w * 0.5
   draw_text(font, label, x, y + 12.0, C_MUTED)
   draw_rect(bar_x, y + 5.0, bar_w, 8.0, C_IDLE)
   draw_rect(mid_x - 1.0, y + 4.0, 2.0, 10.0, C_SUBTLE)
   if(value >= 0.0){
      draw_rect(mid_x, y + 5.0, (bar_w * 0.5) * _clampf(value, 0.0, 1.0), 8.0, fill_col)
   } else {
      def neg_w = (bar_w * 0.5) * _clampf(0.0 - value, 0.0, 1.0)
      draw_rect(mid_x - neg_w, y + 5.0, neg_w, 8.0, fill_col)
   }
   draw_text(font, value_s, x + w - value_w, y + 12.0, C_TEXT)
}

fn _draw_button_chip(label, active, x, y, w){
   draw_rect(x, y, w, 22.0, active ? C_ACCENT_SOFT : C_IDLE)
   draw_rect(x, y, w, active ? 2.0 : 1.0, active ? C_ACCENT : C_SUBTLE)
   def shown = _fit_text(label, max(0.0, w - 8.0))
   def tw = _text_w(shown)
   def tx = x + ((w - tw) * 0.5)
   if(shown.len > 0){ draw_text(font, shown, tx, y + 15.0, C_TEXT) }
}

fn _draw_stage_frame(x, y, w, h){
   draw_rect(x, y, w, h, C_PANEL)
   draw_rect(x + 1.0, y + 1.0, w - 2.0, h - 2.0, C_BG)
   draw_rect(x, y, w, 2.0, C_SUBTLE)
   draw_rect(x, y + h - 1.0, w, 1.0, C_SUBTLE)
   draw_rect(x, y, 1.0, h, C_SUBTLE)
   draw_rect(x + w - 1.0, y, 1.0, h, C_SUBTLE)
}

fn _draw_pad_shell(any: p, any: q, any: r, any: st, f64: lt, f64: rt): int {
   draw_rect_rounded(p(175), q(110), r(460), r(220), r(33), C_PANEL)
   draw_rect_rounded(p(215), q(98), r(100), r(10), r(5), _btn_color(st, win_native.GAMEPAD_BUTTON_LEFT_BUMPER))
   draw_rect_rounded(p(495), q(98), r(100), r(10), r(5), _btn_color(st, win_native.GAMEPAD_BUTTON_RIGHT_BUMPER))
   draw_rect_rounded(p(151), q(110), r(15), r(70), r(5), C_IDLE)
   draw_rect_rounded(p(644), q(110), r(15), r(70), r(5), C_IDLE)
   def lt_h = ((1.0 + lt) / 2.0) * r(70)
   def rt_h = ((1.0 + rt) / 2.0) * r(70)
   if(lt_h > r(10)){ draw_rect_rounded(p(151), q(110), r(15), lt_h, r(5), C_ACCENT) }
   if(rt_h > r(10)){ draw_rect_rounded(p(644), q(110), r(15), rt_h, r(5), C_ACCENT) }
   return 0
}

fn _draw_menu_cluster(any: p, any: q, any: r, any: st): int {
   _disc(p(365), q(170), r(12), C_MID)
   _disc(p(405), q(170), r(12), C_MID)
   _disc(p(445), q(170), r(12), C_MID)
   _disc(p(365), q(170), r(9), _btn_color(st, win_native.GAMEPAD_BUTTON_BACK))
   _disc(p(405), q(170), r(9), _btn_color(st, win_native.GAMEPAD_BUTTON_GUIDE))
   _disc(p(445), q(170), r(9), _btn_color(st, win_native.GAMEPAD_BUTTON_START))
   return 0
}

fn _draw_face_button(any: p, any: q, any: r, any: st, int: button, f64: cx, f64: cy): int {
   _disc(p(cx), q(cy), r(17), C_MID)
   _disc(p(cx), q(cy), r(14), _btn_color(st, button))
   return 0
}

fn _draw_face_cluster(any: p, any: q, any: r, any: st): int {
   _draw_face_button(p, q, r, st, win_native.GAMEPAD_BUTTON_SQUARE, 516, 191)
   _draw_face_button(p, q, r, st, win_native.GAMEPAD_BUTTON_CROSS, 551, 227)
   _draw_face_button(p, q, r, st, win_native.GAMEPAD_BUTTON_CIRCLE, 587, 191)
   _draw_face_button(p, q, r, st, win_native.GAMEPAD_BUTTON_TRIANGLE, 551, 155)
   return 0
}

fn _draw_dpad(any: p, any: q, any: r, any: st): int {
   draw_rect_rounded(p(245), q(145), r(28), r(88), r(4), C_MID)
   draw_rect_rounded(p(215), q(174), r(88), r(29), r(4), C_MID)
   draw_rect_rounded(p(247), q(147), r(24), r(84), r(4), C_IDLE)
   draw_rect_rounded(p(217), q(176), r(84), r(25), r(4), C_IDLE)
   def dc_x = p(259)
   def dc_y = q(188.5)
   if(_pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_UP)){
      draw_rect_rounded(p(247), q(147), r(24), r(29), r(4), C_ACCENT)
      draw_rect(p(247), q(158), r(24), r(18), C_ACCENT)
      draw_triangle([dc_x, dc_y, 0.0], [p(247), q(176), 0.0], [p(271), q(176), 0.0], C_ACCENT)
   }
   if(_pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_DOWN)){
      draw_rect_rounded(p(247), q(201), r(24), r(30), r(4), C_ACCENT)
      draw_rect(p(247), q(201), r(24), r(16), C_ACCENT)
      draw_triangle([dc_x, dc_y, 0.0], [p(271), q(201), 0.0], [p(247), q(201), 0.0], C_ACCENT)
   }
   if(_pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_LEFT)){
      draw_rect_rounded(p(217), q(176), r(30), r(25), r(4), C_ACCENT)
      draw_rect(p(232), q(176), r(15), r(25), C_ACCENT)
      draw_triangle([dc_x, dc_y, 0.0], [p(247), q(201), 0.0], [p(247), q(176), 0.0], C_ACCENT)
   }
   if(_pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_RIGHT)){
      draw_rect_rounded(p(271), q(176), r(30), r(25), r(4), C_ACCENT)
      draw_rect(p(271), q(176), r(15), r(25), C_ACCENT)
      draw_triangle([dc_x, dc_y, 0.0], [p(271), q(176), 0.0], [p(271), q(201), 0.0], C_ACCENT)
   }
   return 0
}

fn _draw_stick(any: p, any: q, any: r, any: st, f64: cx, f64: cy, f64: ax, f64: ay, int: button): int {
   def knob = _pad_button(st, button) ? C_ACCENT : C_STICK
   _disc(p(cx), q(cy), r(40), C_BLACK)
   _disc(p(cx), q(cy), r(35), C_IDLE)
   _disc(p(cx) + (ax * r(20)), q(cy) + (ay * r(20)), r(25), knob)
   return 0
}

fn _draw_simple_dashboard(any: st, f64: x, f64: y, f64: w, f64: h): int {
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
   def p = fn(v){ off_x + v * s }
   def q = fn(v){ off_y + v * s }
   def r = fn(v){ v * s }
   def lx = _pad_axis(st, win_native.GAMEPAD_AXIS_LEFT_X)
   def ly = _pad_axis(st, win_native.GAMEPAD_AXIS_LEFT_Y)
   def rx = _pad_axis(st, win_native.GAMEPAD_AXIS_RIGHT_X)
   def ry = _pad_axis(st, win_native.GAMEPAD_AXIS_RIGHT_Y)
   def lt = _trigger_axis(st, win_native.GAMEPAD_AXIS_LEFT_TRIGGER, 2)
   def rt = _trigger_axis(st, win_native.GAMEPAD_AXIS_RIGHT_TRIGGER, 5)
   _draw_pad_shell(p, q, r, st, lt, rt)
   _draw_menu_cluster(p, q, r, st)
   _draw_face_cluster(p, q, r, st)
   _draw_dpad(p, q, r, st)
   _draw_stick(p, q, r, st, 345, 260, lx, ly, win_native.GAMEPAD_BUTTON_LEFT_THUMB)
   _draw_stick(p, q, r, st, 465, 260, rx, ry, win_native.GAMEPAD_BUTTON_RIGHT_THUMB)
   return 0
}

fn _draw_empty_state(ww, wh){
   def box_w = _clampf(ww - 48.0, 360.0, 760.0)
   def box_h = 186.0
   def x = ww * 0.5 - box_w * 0.5
   def y = wh * 0.5 - box_h * 0.5
   _panel(x, y, box_w, box_h, "WAITING FOR A GAMEPAD", C_ACCENT)
   def text_w = box_w - 36.0
   _draw_text_fit("Connect a controller, then move a stick or press a button.", x + 18.0, y + 76.0, text_w, C_TEXT)
   _draw_text_fit("The list rescans every second; no restart is needed.", x + 18.0, y + 104.0, text_w, C_MUTED)
   _draw_text_fit("Set NY_GAMEPAD_DEMO=1 to preview the layout without hardware.", x + 18.0, y + 132.0, text_w, C_SUBTLE)
}

fn _line_h(){
   def sz = measure_text(font, "Mg")
   def h = float(sz.get(1, 18.0))
   h > 0.0 ? h : 18.0
}

fn _controller_fit_scale(w, h){
   if(w <= 0.0 || h <= 0.0){ return 0.0 }
   def sx = w / 508.0
   def sy = h / 232.0
   (sx < sy ? sx : sy) * CONTROLLER_DRAW_SCALE
}

fn _active_pressed_text(st){
   def pressed = st.get("pressed_labels", [])
   if(pressed.len == 0){ return "" }
   def pressed_n = pressed.len
   mut out = "ACTIVE"
   mut i = 0
   while(i < pressed_n){
      out = out + " " + pressed.get(i, "")
      i += 1
   }
   out
}

fn _show_hats(st){
   def hats_ptr = st.get("hats_ptr", 0)
   def hat_count = st.get("hat_count", 0)
   mut hi = 0
   while(hi < hat_count){
      if(hats_ptr && load8(hats_ptr, hi) != 0){ return true }
      hi += 1
   }
   false
}

fn _hat_text(st){
   if(!_show_hats(st)){ return "" }
   def hats_ptr = st.get("hats_ptr", 0)
   def hat_count = st.get("hat_count", 0)
   mut out = "HATS"
   mut hi = 0
   while(hi < hat_count){
      out = out + " H" + to_str(hi) + "=" + to_str(hats_ptr ? load8(hats_ptr, hi) : 0)
      hi += 1
   }
   out
}

fn _axis_block_width(st, gap){
   def axis_count = st.get("axis_count", 0)
   mut w = _text_w("AXES")
   def axis_value_w = _text_w("-1.00")
   mut i = 0
   while(i < axis_count){
      def row_w = _text_w(_axis_label(i)) + gap + axis_value_w
      if(row_w > w){ w = row_w }
      i += 1
   }
   def last_s = "LAST"
   def pressed_s = "PRESSED"
   def summary_w = _text_w(last_s) + gap + _text_w(pressed_s)
   if(summary_w > w){ w = summary_w }
   def active_w = _text_w(LEGEND_ACTIVE_RESERVE)
   if(active_w > w){ w = active_w }
   if(st.get("hat_count", 0) > 0){
      def hat_w = _text_w(LEGEND_HAT_RESERVE)
      if(hat_w > w){ w = hat_w }
   }
   w
}

fn _axis_block_height(st, line_h){
   mut lines = 1 + st.get("axis_count", 0) + 2
   if(st.get("hat_count", 0) > 0){ lines += 1 }
   line_h * float(lines)
}

fn _draw_axis_block(st, x, y, w, line_h){
   def axis_count = st.get("axis_count", 0)
   draw_text(font, "AXES", x, y, C_MUTED)
   mut row_y = y + line_h
   mut i = 0
   while(i < axis_count){
      def label = _axis_label(i)
      def val_s = _fixed2(_pad_axis(st, i))
      draw_text(font, label, x, row_y, C_TEXT)
      draw_text(font, val_s, x + w - _text_w(val_s), row_y, C_MUTED)
      row_y += line_h
      i += 1
   }
   def last_btn = st.get("last_button", -1)
   def last_s = "LAST"
   def pressed_s = "PRESSED"
   draw_text(font, last_s, x, row_y, last_btn >= 0 ? C_ACCENT : C_MUTED)
   draw_text(font, pressed_s, x + w - _text_w(pressed_s), row_y, C_TEXT)
   row_y += line_h
   def active_s = _active_pressed_text(st)
   _draw_text_fit(active_s.len > 0 ? active_s : "ACTIVE -", x, row_y, w, active_s.len > 0 ? C_SUBTLE : C_MUTED)
   row_y += line_h
   if(st.get("hat_count", 0) > 0){
      def hat_s = _hat_text(st)
      _draw_text_fit(hat_s.len > 0 ? hat_s : "HATS -", x, row_y, w, hat_s.len > 0 ? C_SUBTLE : C_MUTED)
   }
}

fn _other_device_count(rows, best_jid){
   mut n = 0
   def rows_n = rows.len
   mut i = 0
   while(i < rows_n){
      if(rows.get(i).get("jid", -1) != best_jid){ n += 1 }
      i += 1
   }
   n
}

fn _device_block_height(rows, best_jid, max_rows, line_h){
   def count = _other_device_count(rows, best_jid)
   if(count == 0){ return 0.0 }
   def shown = count < max_rows ? count : max_rows
   def extra = count > shown ? 1 : 0
   line_h * float(1 + shown + extra)
}

fn _draw_device_block(list: rows, int: best_jid, f64: x, f64: y, f64: max_w, int: max_rows, f64: line_h): int {
   if(_other_device_count(rows, best_jid) == 0){ return 0 }
   draw_text(font, "OTHER DEVICES", x, y, C_MUTED)
   mut row_y = y + line_h
   mut shown = 0
   mut remaining = 0
   def rows_n = rows.len
   mut i = 0
   while(i < rows_n){
      def row = rows.get(i)
      def jid = row.get("jid", -1)
      if(jid != best_jid){
         if(shown < max_rows){
            _draw_text_fit("[" + to_str(jid) + "] " + row.get("name", ""), x, row_y, max(48.0, max_w), C_SUBTLE)
            row_y += line_h
            shown += 1
         } else {
            remaining += 1
         }
      }
      i += 1
   }
   if(remaining > 0){
      draw_text(font, "+" + to_str(remaining) + " more", x, row_y, C_SUBTLE)
   }
   return 0
}

fn _draw_bottom_layout(list: rows, int: best_jid, any: pad_state, f64: ww, f64: wh, f64: pad, f64: gap, f64: line_h, f64: axis_w, f64: footer_h, f64: devices_h, int: max_device_rows): int {
   def footer_y = wh - pad - footer_h
   def stage_y = pad + line_h + gap
   def stage_h = footer_y - gap - stage_y
   _draw_simple_dashboard(pad_state, pad, stage_y, ww - pad * 2.0, stage_h)
   if(devices_h > 0.0){
      _draw_device_block(rows, best_jid, pad, footer_y, max(48.0, ww - pad * 3.0 - axis_w - gap), max_device_rows, line_h)
   }
   _draw_axis_block(pad_state, ww - pad - axis_w, footer_y, axis_w, line_h)
   return 0
}

fn _draw_side_layout(list: rows, int: best_jid, any: pad_state, f64: ww, f64: wh, f64: pad, f64: gap, f64: line_h, f64: axis_w, f64: devices_h, int: max_device_rows): int {
   def footer_y = wh - pad - devices_h
   def stage_y = pad + line_h + gap
   def stage_w = ww - pad * 3.0 - axis_w
   def stage_h = footer_y - gap - stage_y
   _draw_simple_dashboard(pad_state, pad, stage_y, stage_w, stage_h)
   _draw_axis_block(pad_state, ww - pad - axis_w, stage_y, axis_w, line_h)
   if(devices_h > 0.0){
      _draw_device_block(rows, best_jid, pad, footer_y, max(48.0, ww - pad * 2.0), max_device_rows, line_h)
   }
   return 0
}

fn _draw_scene(list: rows, int: best_jid, any: pad_state, f64: ww, f64: wh): int {
   _dbg("draw scene begin")
   clear_background(C_BG)
   if(best_jid == -1){
      _draw_empty_state(ww, wh)
      return 0
   }
   def line_h = _line_h()
   def pad = max(14.0, line_h * 0.55)
   def gap = max(10.0, line_h * 0.45)
   def max_device_rows = max(1, int((wh * 0.18) / line_h))
   def title = pad_state.get("name", "Unknown")
   def header_h = line_h
   _dbg("draw metrics begin")
   def axis_w = _axis_block_width(pad_state, pad)
   def axis_h = _axis_block_height(pad_state, line_h)
   def devices_h = _device_block_height(rows, best_jid, max_device_rows, line_h)
   _dbg("draw metrics done")
   def footer_h = axis_h > devices_h ? axis_h : devices_h
   def side_stage_w = ww - pad * 3.0 - axis_w
   def side_stage_h = wh - pad * 3.0 - header_h - devices_h
   def side_scale = _controller_fit_scale(side_stage_w, side_stage_h)
   def bottom_stage_w = ww - pad * 2.0
   def bottom_stage_h = wh - pad * 3.0 - header_h - footer_h
   def bottom_scale = _controller_fit_scale(bottom_stage_w, bottom_stage_h)
   def use_bottom = bottom_scale >= side_scale
   _dbg("draw title")
   _draw_text_fit(title, pad, pad, max(80.0, ww - pad * 2.0), pad_state.get("mapped", false) ? C_ACCENT : C_TEXT)
   if(use_bottom){
      _draw_bottom_layout(rows, best_jid, pad_state, ww, wh, pad, gap, line_h, axis_w, footer_h, devices_h, max_device_rows)
   } else {
      _draw_side_layout(rows, best_jid, pad_state, ww, wh, pad, gap, line_h, axis_w, devices_h, max_device_rows)
   }
   _dbg("draw scene done")
   return 0
}

def mode = window.primary_mode()
def screen_w = mode.get(0, 1280)
def screen_h = mode.get(1, 720)
def default_sz = window.default_window_size(screen_w, screen_h)
def start_w = _env_dim("NY_GAMEPAD_WIDTH", default_sz.get(0, 1280))
def start_h = _env_dim("NY_GAMEPAD_HEIGHT", default_sz.get(1, 720))
def render_cfg = ui_app.app_startup_render_config(4, true, true)
def flags = WINDOW_CENTER | WINDOW_FOCUS_ON_SHOW
def win = exutil.open_windowed(
   "Gamepad",
   start_w,
   start_h,
   flags,
   bool(render_cfg.get("vsync", true)),
   bool(render_cfg.get("filter_linear", true)),
   int(render_cfg.get("msaa", 4)),
)

if(!win){
   print("[gamepad] failed to create window")
   exit(1)
}

font = demo.mono_font(exutil.demo_font_size("gamepad", 18.0), 0, exutil.demo_font_filter("gamepad", FONT_FILTER_LINEAR))
def auto_dump = common.env_enabled("NYTRIX_AUTO_DUMP")
def auto_dump_exit = common.env_enabled("NYTRIX_AUTO_DUMP_EXIT")
def auto_dump_path = common.env_trim("NYTRIX_AUTO_DUMP_PATH")
def auto_dump_delay = _env_dim("NYTRIX_AUTO_DUMP_DELAY_FRAMES", 8)
def demo_mode = common.env_enabled("NY_GAMEPAD_DEMO")
mut auto_dump_done = false
mut frame_count = 0
mut startup_ticks = ticks()
while(!window.should_close(win)){
   if(exutil.step(win, startup_ticks, 0, true)){ break }
   if(!demo_mode){ _refresh_device_cache(false) }
   if(!begin_frame()){
      msleep(1)
      continue
   }
   def fb = win_native.get_framebuffer_size(window.id(win))
   def ww_i = int(fb.get(0, 1280))
   def wh_i = int(fb.get(1, 720))
   def ww = float(ww_i)
   def wh = float(wh_i)
   set_win_size(ww_i, wh_i)
   set_ortho_2d(0.0, ww, 0.0, wh)
   def joysticks = demo_mode ? _demo_rows() : _device_rows
   def best_jid = demo_mode ? 0 : _best_jid_cached
   def pad_state = demo_mode ? _demo_pad_state() : ((best_jid != -1) ? _snapshot_pad_state(best_jid) : 0)
   _draw_scene(joysticks, best_jid, pad_state, ww, wh)
   if(pad_state){ _release_pad_state(pad_state) }
   if(auto_dump && !auto_dump_done && (frame_count + 1) >= auto_dump_delay){
      request_frame_capture()
   }
   end_frame()
   frame_count += 1
   if(auto_dump && !auto_dump_done && frame_count >= auto_dump_delay){
      def dump_path = auto_dump_path.len > 0 ? auto_dump_path : "build/cache/fb/gamepad_ui/gamepad_dump.png"
      snapshot(dump_path)
      auto_dump_done = true
      if(auto_dump_exit){
         window.set_should_close(win, true)
      }
   }
}

window.close(win)
