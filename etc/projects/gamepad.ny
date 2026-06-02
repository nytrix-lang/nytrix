#!/usr/bin/env ny

;; Keywords: platform probe ui app example gamepad font
;; Nytrix Gamepad
use std.core
use std.os (ticks, msleep, exit)
use std.os.ui.render
use std.os.ui.window as window
use std.os.ui.window.native as win_native
use std.os.ui.app as ui_app
use std.core.str as str
use std.util.common as common
use std.os.ui.consts
use std.os.ui.runtime as exutil

mut font = 0
mut _device_rows = []
mut _best_jid_cached = -1
mut _last_device_scan_ticks = 0
mut _text_width_cache = dict(64)
mut _profile_enabled = false
mut _profile_every_frames = 30
mut _profile_frame_index = 0
mut _probe_last_sig = ""
mut _probe_last_log_ticks = 0
mut _text_runs = []
mut _text_run_count = 0
mut _text_char_count = 0
def DEVICE_SCAN_INTERVAL_NS = 1000000000
def DEVICE_SCAN_EMPTY_INTERVAL_NS = 250000000
def INPUT_LOG_INTERVAL_NS = 250000000
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

fn _framebuffer_size_for_layout(any: win): list {
   #windows {
      return get_framebuffer_size()
   }
   #else {
      return win_native.get_framebuffer_size(window.id(win))
   }
   #endif
}

fn _axis_i100(v): int {
   mut n = int(v * 100.0)
   if(n > -4 && n < 4){ n = 0 }
   if(n < -100){ n = -100 }
   if(n > 100){ n = 100 }
   n
}

fn _axis_f(int: n): f64 {
   n * 0.01
}

fn _clampf(v, lo, hi){
   if(v < lo){ return lo }
   if(v > hi){ return hi }
   v
}

fn _safe_axis_value(v): f64 {
   _axis_f(_axis_i100(v))
}

fn _fixed2(v){
   to_str(_axis_f(_axis_i100(v)))
}

fn _packed_color(color): int {
   if(is_int(color)){ return int(color) }
   color_pack(
      float(color.get(0, 1.0)),
      float(color.get(1, 1.0)),
      float(color.get(2, 1.0)),
      float(color.get(3, 1.0)),
   )
}

fn _rect(x, y, w, h, color): bool {
   draw_rect_fast(float(x), float(y), float(w), float(h), _packed_color(color))
}

fn _round_rect(x, y, w, h, radius, color): bool {
   draw_rounded_rectangle_sdf(x, y, w, h, radius, color)
}

fn _disc(cx, cy, radius, color){
   draw_rounded_rectangle_sdf(cx - radius, cy - radius, radius * 2.0, radius * 2.0, radius, color)
}

fn _show_input_probe(): bool {
   common.env_enabled("NY_GAMEPAD_INPUT_PROBE")
}

fn _text_w(label){
   def cached = _text_width_cache.get(label, -1.0)
   if(cached >= 0.0){ return cached }
   def tw = float(measure_text(font, label).get(0, 0.0))
   _text_width_cache[label] = tw
   tw
}

fn _pack_text_color(color): int {
   _packed_color(color)
}

fn _queue_text(label, x, y, color): int {
   def s = to_str(label)
   if(s.len <= 0){ return 0 }
   _text_run_count += 1
   _text_char_count += s.len
   mut runs = is_list(_text_runs) ? _text_runs : []
   runs = runs.append(s)
   runs = runs.append(float(x))
   runs = runs.append(float(y))
   runs = runs.append(_pack_text_color(color))
   _text_runs = runs
   0
}

fn _flush_text_runs(): int {
   if(is_list(_text_runs) && _text_runs.len > 0){
      draw_text_runs_flat_colors(font, _text_runs)
      _text_runs = []
   }
   0
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
   if(s.len > 0){ _queue_text(s, x, y, color) }
   0
}

fn _draw_text_right_fit(label, right_x, y, max_w, color){
   def s = _fit_text(label, max_w)
   if(s.len > 0){ _queue_text(s, right_x - _text_w(s), y, color) }
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

fn _raw_counts(jid){
   def count_ptr = malloc(4)
   if(!count_ptr){ return [0, 0, 0] }
   win_native.get_joystick_axes(jid, count_ptr)
   def ac = load32(count_ptr, 0)
   win_native.get_joystick_buttons(jid, count_ptr)
   def bc = load32(count_ptr, 0)
   win_native.get_joystick_hats(jid, count_ptr)
   def hc = load32(count_ptr, 0)
   free(count_ptr)
   [ac, bc, hc]
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
   if(str.find(lname, "touchpad") != -1){ score -= 1000 }
   if(str.find(lname, "motion sensor") != -1){ score -= 1000 }
   if(str.find(lname, "motion sensors") != -1){ score -= 1000 }
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
   def counts = _raw_counts(jid)
   row["axis_count"] = counts.get(0, 0)
   row["button_count"] = counts.get(1, 0)
   row["hat_count"] = counts.get(2, 0)
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
   mut raw_axis_count = axis_count
   mut raw_button_count = button_count
   mut hat_count = 0
   mut axes_ptr = 0
   mut buttons_ptr = 0
   mut hats_ptr = 0
   def count_ptr = malloc(4)
   if(count_ptr){
      axes_ptr = win_native.get_joystick_axes(jid, count_ptr)
      raw_axis_count = load32(count_ptr, 0)
      if(!mapped){ axis_count = raw_axis_count }
      buttons_ptr = win_native.get_joystick_buttons(jid, count_ptr)
      raw_button_count = load32(count_ptr, 0)
      if(!mapped){ button_count = raw_button_count }
      hats_ptr = win_native.get_joystick_hats(jid, count_ptr)
      hat_count = load32(count_ptr, 0)
      free(count_ptr)
   }
   [axis_count, axes_ptr, button_count, buttons_ptr, hat_count, hats_ptr, raw_axis_count, raw_button_count]
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
   st["raw_axis_count"] = raw.get(6, st.get("axis_count", 0))
   st["raw_button_count"] = raw.get(7, st.get("button_count", 0))
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
   _rect(x, y, w, h, C_PANEL)
   _rect(x + 1.0, y + 1.0, w - 2.0, h - 2.0, C_PANEL_ALT)
   if(accent){
      _rect(x, y, w, 3.0, accent)
   } else {
      _rect(x, y, w, 3.0, C_SUBTLE)
   }
   _draw_text_fit(title, x + 16.0, y + 26.0, max(24.0, w - 32.0), C_TEXT)
}

fn _draw_axis_meter(label, value, x, y, w){
   def axis_i = _axis_i100(value)
   def mag_i = axis_i < 0 ? -axis_i : axis_i
   def fill_col = mag_i > 4 ? C_ACCENT : C_SUBTLE
   def value_s = _fixed2(value)
   def value_w = _text_w(value_s)
   def bar_x = x + 32.0
   def bar_w = max(24.0, w - 44.0 - value_w)
   def mid_x = bar_x + bar_w * 0.5
   _queue_text(label, x, y + 12.0, C_MUTED)
   _rect(bar_x, y + 5.0, bar_w, 8.0, C_IDLE)
   _rect(mid_x - 1.0, y + 4.0, 2.0, 10.0, C_SUBTLE)
   if(axis_i >= 0){
      _rect(mid_x, y + 5.0, (bar_w * 0.5) * _axis_f(axis_i), 8.0, fill_col)
   } else {
      def neg_w = (bar_w * 0.5) * _axis_f(-axis_i)
      _rect(mid_x - neg_w, y + 5.0, neg_w, 8.0, fill_col)
   }
   _queue_text(value_s, x + w - value_w, y + 12.0, C_TEXT)
}

fn _draw_button_chip(label, active, x, y, w){
   _rect(x, y, w, 22.0, active ? C_ACCENT_SOFT : C_IDLE)
   _rect(x, y, w, active ? 2.0 : 1.0, active ? C_ACCENT : C_SUBTLE)
   def shown = _fit_text(label, max(0.0, w - 8.0))
   def tw = _text_w(shown)
   def tx = x + ((w - tw) * 0.5)
   if(shown.len > 0){ _queue_text(shown, tx, y + 15.0, C_TEXT) }
}

fn _draw_stage_frame(x, y, w, h){
   _rect(x, y, w, h, C_PANEL)
   _rect(x + 1.0, y + 1.0, w - 2.0, h - 2.0, C_BG)
   _rect(x, y, w, 2.0, C_SUBTLE)
   _rect(x, y + h - 1.0, w, 1.0, C_SUBTLE)
   _rect(x, y, 1.0, h, C_SUBTLE)
   _rect(x + w - 1.0, y, 1.0, h, C_SUBTLE)
}

fn _gx(f64: off_x, f64: s, f64: v): f64 { off_x + v * s }
fn _gy(f64: off_y, f64: s, f64: v): f64 { off_y + v * s }
fn _gs(f64: s, f64: v): f64 { v * s }

fn _draw_pad_shell(f64: off_x, f64: off_y, f64: s, any: st, f64: lt, f64: rt): int {
   _round_rect(_gx(off_x, s, 175), _gy(off_y, s, 110), _gs(s, 460), _gs(s, 220), _gs(s, 33), C_PANEL)
   _round_rect(_gx(off_x, s, 215), _gy(off_y, s, 98), _gs(s, 100), _gs(s, 10), _gs(s, 5), _btn_color(st, win_native.GAMEPAD_BUTTON_LEFT_BUMPER))
   _round_rect(_gx(off_x, s, 495), _gy(off_y, s, 98), _gs(s, 100), _gs(s, 10), _gs(s, 5), _btn_color(st, win_native.GAMEPAD_BUTTON_RIGHT_BUMPER))
   _round_rect(_gx(off_x, s, 151), _gy(off_y, s, 110), _gs(s, 15), _gs(s, 70), _gs(s, 5), C_IDLE)
   _round_rect(_gx(off_x, s, 644), _gy(off_y, s, 110), _gs(s, 15), _gs(s, 70), _gs(s, 5), C_IDLE)
   def lt_h = ((1.0 + lt) * 0.5) * _gs(s, 70)
   def rt_h = ((1.0 + rt) * 0.5) * _gs(s, 70)
   if(lt_h > _gs(s, 10)){ _round_rect(_gx(off_x, s, 151), _gy(off_y, s, 110), _gs(s, 15), lt_h, _gs(s, 5), C_ACCENT) }
   if(rt_h > _gs(s, 10)){ _round_rect(_gx(off_x, s, 644), _gy(off_y, s, 110), _gs(s, 15), rt_h, _gs(s, 5), C_ACCENT) }
   return 0
}

fn _draw_menu_cluster(f64: off_x, f64: off_y, f64: s, any: st): int {
   _disc(_gx(off_x, s, 365), _gy(off_y, s, 170), _gs(s, 12), C_MID)
   _disc(_gx(off_x, s, 405), _gy(off_y, s, 170), _gs(s, 12), C_MID)
   _disc(_gx(off_x, s, 445), _gy(off_y, s, 170), _gs(s, 12), C_MID)
   _disc(_gx(off_x, s, 365), _gy(off_y, s, 170), _gs(s, 9), _btn_color(st, win_native.GAMEPAD_BUTTON_BACK))
   _disc(_gx(off_x, s, 405), _gy(off_y, s, 170), _gs(s, 9), _btn_color(st, win_native.GAMEPAD_BUTTON_GUIDE))
   _disc(_gx(off_x, s, 445), _gy(off_y, s, 170), _gs(s, 9), _btn_color(st, win_native.GAMEPAD_BUTTON_START))
   return 0
}

fn _draw_face_button(f64: off_x, f64: off_y, f64: s, any: st, int: button, f64: cx, f64: cy): int {
   _disc(_gx(off_x, s, cx), _gy(off_y, s, cy), _gs(s, 17), C_MID)
   _disc(_gx(off_x, s, cx), _gy(off_y, s, cy), _gs(s, 14), _btn_color(st, button))
   return 0
}

fn _draw_face_cluster(f64: off_x, f64: off_y, f64: s, any: st): int {
   _draw_face_button(off_x, off_y, s, st, win_native.GAMEPAD_BUTTON_SQUARE, 516, 191)
   _draw_face_button(off_x, off_y, s, st, win_native.GAMEPAD_BUTTON_CROSS, 551, 227)
   _draw_face_button(off_x, off_y, s, st, win_native.GAMEPAD_BUTTON_CIRCLE, 587, 191)
   _draw_face_button(off_x, off_y, s, st, win_native.GAMEPAD_BUTTON_TRIANGLE, 551, 155)
   return 0
}

fn _draw_dpad(f64: off_x, f64: off_y, f64: s, any: st): int {
   _round_rect(_gx(off_x, s, 245), _gy(off_y, s, 145), _gs(s, 28), _gs(s, 88), _gs(s, 4), C_MID)
   _round_rect(_gx(off_x, s, 215), _gy(off_y, s, 174), _gs(s, 88), _gs(s, 29), _gs(s, 4), C_MID)
   _round_rect(_gx(off_x, s, 247), _gy(off_y, s, 147), _gs(s, 24), _gs(s, 84), _gs(s, 4), C_IDLE)
   _round_rect(_gx(off_x, s, 217), _gy(off_y, s, 176), _gs(s, 84), _gs(s, 25), _gs(s, 4), C_IDLE)
   def dc_x = _gx(off_x, s, 259)
   def dc_y = _gy(off_y, s, 188.5)
   if(_pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_UP)){
      _round_rect(_gx(off_x, s, 247), _gy(off_y, s, 147), _gs(s, 24), _gs(s, 29), _gs(s, 4), C_ACCENT)
      _rect(_gx(off_x, s, 247), _gy(off_y, s, 158), _gs(s, 24), _gs(s, 18), C_ACCENT)
      draw_triangle([dc_x, dc_y, 0.0], [_gx(off_x, s, 247), _gy(off_y, s, 176), 0.0], [_gx(off_x, s, 271), _gy(off_y, s, 176), 0.0], C_ACCENT)
   }
   if(_pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_DOWN)){
      _round_rect(_gx(off_x, s, 247), _gy(off_y, s, 201), _gs(s, 24), _gs(s, 30), _gs(s, 4), C_ACCENT)
      _rect(_gx(off_x, s, 247), _gy(off_y, s, 201), _gs(s, 24), _gs(s, 16), C_ACCENT)
      draw_triangle([dc_x, dc_y, 0.0], [_gx(off_x, s, 271), _gy(off_y, s, 201), 0.0], [_gx(off_x, s, 247), _gy(off_y, s, 201), 0.0], C_ACCENT)
   }
   if(_pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_LEFT)){
      _round_rect(_gx(off_x, s, 217), _gy(off_y, s, 176), _gs(s, 30), _gs(s, 25), _gs(s, 4), C_ACCENT)
      _rect(_gx(off_x, s, 232), _gy(off_y, s, 176), _gs(s, 15), _gs(s, 25), C_ACCENT)
      draw_triangle([dc_x, dc_y, 0.0], [_gx(off_x, s, 247), _gy(off_y, s, 201), 0.0], [_gx(off_x, s, 247), _gy(off_y, s, 176), 0.0], C_ACCENT)
   }
   if(_pad_button(st, win_native.GAMEPAD_BUTTON_DPAD_RIGHT)){
      _round_rect(_gx(off_x, s, 271), _gy(off_y, s, 176), _gs(s, 30), _gs(s, 25), _gs(s, 4), C_ACCENT)
      _rect(_gx(off_x, s, 271), _gy(off_y, s, 176), _gs(s, 15), _gs(s, 25), C_ACCENT)
      draw_triangle([dc_x, dc_y, 0.0], [_gx(off_x, s, 271), _gy(off_y, s, 176), 0.0], [_gx(off_x, s, 271), _gy(off_y, s, 201), 0.0], C_ACCENT)
   }
   return 0
}

fn _draw_stick(f64: off_x, f64: off_y, f64: s, any: st, f64: cx, f64: cy, f64: ax, f64: ay, int: button): int {
   def knob = _pad_button(st, button) ? C_ACCENT : C_STICK
   _disc(_gx(off_x, s, cx), _gy(off_y, s, cy), _gs(s, 40), C_BLACK)
   _disc(_gx(off_x, s, cx), _gy(off_y, s, cy), _gs(s, 35), C_IDLE)
   _disc(_gx(off_x, s, cx) + (ax * _gs(s, 20)), _gy(off_y, s, cy) + (ay * _gs(s, 20)), _gs(s, 25), knob)
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
   def lx = _pad_axis(st, win_native.GAMEPAD_AXIS_LEFT_X)
   def ly = _pad_axis(st, win_native.GAMEPAD_AXIS_LEFT_Y)
   def rx = _pad_axis(st, win_native.GAMEPAD_AXIS_RIGHT_X)
   def ry = _pad_axis(st, win_native.GAMEPAD_AXIS_RIGHT_Y)
   def lt = _trigger_axis(st, win_native.GAMEPAD_AXIS_LEFT_TRIGGER, 2)
   def rt = _trigger_axis(st, win_native.GAMEPAD_AXIS_RIGHT_TRIGGER, 5)
   _draw_pad_shell(off_x, off_y, s, st, lt, rt)
   _draw_menu_cluster(off_x, off_y, s, st)
   _draw_face_cluster(off_x, off_y, s, st)
   _draw_dpad(off_x, off_y, s, st)
   _draw_stick(off_x, off_y, s, st, 345, 260, lx, ly, win_native.GAMEPAD_BUTTON_LEFT_THUMB)
   _draw_stick(off_x, off_y, s, st, 465, 260, rx, ry, win_native.GAMEPAD_BUTTON_RIGHT_THUMB)
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

fn _probe_bit(bool: v): str { v ? "1" : "0" }

fn _raw_axis_value(any: st, int: idx): f64 {
   if(!st){ return 0.0 }
   def axes_ptr = st.get("axes_ptr", 0)
   def axis_count = st.get("raw_axis_count", st.get("axis_count", 0))
   if(!axes_ptr || idx < 0 || idx >= axis_count){ return 0.0 }
   _safe_axis_value(load32_f32(axes_ptr, idx * 4))
}

fn _raw_button_down(any: st, int: idx): bool {
   if(!st){ return false }
   def buttons_ptr = st.get("buttons_ptr", 0)
   def button_count = st.get("raw_button_count", st.get("button_count", 0))
   buttons_ptr && idx >= 0 && idx < button_count && load8(buttons_ptr, idx) != 0
}

fn _probe_axes_text(any: st, int: max_axes): str {
   if(!st){ return "axes -" }
   mut out = st.get("mapped", false) ? "mapped axes" : "axes"
   def axis_count = st.get("axis_count", 0)
   def shown = axis_count < max_axes ? axis_count : max_axes
   mut i = 0
   while(i < shown){
      out = out + " " + _axis_label(i) + "=" + _fixed2(_pad_axis(st, i))
      i += 1
   }
   if(axis_count > shown){ out = out + " +" + to_str(axis_count - shown) }
   out
}

fn _probe_raw_axes_text(any: st, int: max_axes): str {
   if(!st){ return "raw axes -" }
   mut out = "raw axes"
   def axis_count = st.get("raw_axis_count", st.get("axis_count", 0))
   def shown = axis_count < max_axes ? axis_count : max_axes
   mut i = 0
   while(i < shown){
      out = out + " A" + to_str(i) + "=" + _fixed2(_raw_axis_value(st, i))
      i += 1
   }
   if(axis_count > shown){ out = out + " +" + to_str(axis_count - shown) }
   out
}

fn _probe_pad_text(any: st): str {
   if(!st){ return "pad none" }
   def mapped = st.get("mapped", false)
   def last_btn = st.get("last_button", -1)
   def last_s = last_btn >= 0 ? _button_label(st, last_btn) : "-"
   "pad mapped " + _probe_bit(mapped) +
      " axes " + to_str(st.get("axis_count", 0)) +
      "/" + to_str(st.get("raw_axis_count", st.get("axis_count", 0))) +
      " buttons " + to_str(st.get("button_count", 0)) +
      "/" + to_str(st.get("raw_button_count", st.get("button_count", 0))) +
      " hats " + to_str(st.get("hat_count", 0)) +
      " pressed " + to_str(st.get("pressed_count", 0)) +
      " last " + last_s
}

fn _probe_button_text(any: st): str {
   if(!st){ return "buttons -" }
   def pressed = st.get("pressed_labels", [])
   if(pressed.len == 0){ return "buttons -" }
   mut out = "buttons"
   mut i = 0
   def pressed_n = pressed.len
   while(i < pressed_n){
      out = out + " " + pressed.get(i, "")
      i += 1
   }
   out
}

fn _probe_raw_button_text(any: st, int: max_buttons): str {
   if(!st){ return "raw buttons -" }
   def button_count = st.get("raw_button_count", st.get("button_count", 0))
   mut out = "raw buttons"
   mut shown = 0
   mut pressed = 0
   mut bi = 0
   while(bi < button_count){
      if(_raw_button_down(st, bi)){
         pressed += 1
         if(shown < max_buttons){
            out = out + " B" + to_str(bi)
            shown += 1
         }
      }
      bi += 1
   }
   if(pressed == 0){ return "raw buttons -" }
   if(pressed > shown){ out = out + " +" + to_str(pressed - shown) }
   out
}

fn _probe_hats_text(any: st): str {
   if(!st){ return "hats -" }
   def hats_ptr = st.get("hats_ptr", 0)
   def hat_count = st.get("hat_count", 0)
   if(hat_count <= 0){ return "hats -" }
   mut out = "hats"
   mut hi = 0
   mut any_down = false
   while(hi < hat_count){
      def hv = hats_ptr ? load8(hats_ptr, hi) : 0
      if(hv != 0){ any_down = true }
      out = out + " H" + to_str(hi) + "=" + to_str(hv)
      hi += 1
   }
   any_down ? out : "hats -"
}

fn _probe_raw_device_text(any: row): str {
   def jid = row.get("jid", -1)
   if(jid < 0){ return "" }
   def count_ptr = malloc(4)
   if(!count_ptr){ return "[" + to_str(jid) + "] " + row.get("name", "") }
   def axes = win_native.get_joystick_axes(jid, count_ptr)
   def ac = load32(count_ptr, 0)
   def buttons = win_native.get_joystick_buttons(jid, count_ptr)
   def bc = load32(count_ptr, 0)
   win_native.get_joystick_hats(jid, count_ptr)
   def hc = load32(count_ptr, 0)
   mut pressed = 0
   mut bi = 0
   while(bi < bc){
      if(buttons && load8(buttons, bi) != 0){ pressed += 1 }
      bi += 1
   }
   def a0 = (axes && ac > 0) ? _fixed2(load32_f32(axes, 0)) : "-"
   def a1 = (axes && ac > 1) ? _fixed2(load32_f32(axes, 4)) : "-"
   free(count_ptr)
   "[" + to_str(jid) + "] a" + to_str(ac) + "(" + a0 + "," + a1 + ")" +
      " b" + to_str(bc) + " p" + to_str(pressed) +
      " h" + to_str(hc) + " " + row.get("name", "")
}

fn _probe_devices_text(list: rows, int: best_jid): str {
   mut out = "raw"
   mut shown = 0
   mut remaining = 0
   def rows_n = rows.len
   mut i = 0
   while(i < rows_n){
      def row = rows.get(i)
      def jid = row.get("jid", -1)
      if(jid != best_jid){
         if(shown < 3){
            out = out + " " + _probe_raw_device_text(row)
            shown += 1
         } else {
            remaining += 1
         }
      }
      i += 1
   }
   if(shown == 0){ return "raw aux -" }
   if(remaining > 0){ out = out + " +" + to_str(remaining) }
   out
}

fn _draw_input_probe(any: win, list: rows, int: best_jid, any: pad_state, f64: x, f64: y, f64: w, f64: line_h): int {
   def mp = window.mouse_pos(win)
   def mx = int(mp.get(0, 0))
   def my = int(mp.get(1, 0))
   _draw_text_fit("INPUT PROBE", x, y, w, C_MUTED)
   _draw_text_fit("backend " + window.backend() + " devices " + to_str(rows.len) + " active " + to_str(best_jid), x, y + line_h, w, C_TEXT)
   _draw_text_fit("mouse " + to_str(mx) + "," + to_str(my) + " L" + _probe_bit(window.mouse_down(win, 0)) + " R" + _probe_bit(window.mouse_down(win, 1)) + " M" + _probe_bit(window.mouse_down(win, 2)) + " B4" + _probe_bit(window.mouse_down(win, 3)) + " B5" + _probe_bit(window.mouse_down(win, 4)), x, y + line_h * 2.0, w, C_SUBTLE)
   _draw_text_fit("keys WASD " + _probe_bit(window.key_down(win, KEY_W)) + _probe_bit(window.key_down(win, KEY_A)) + _probe_bit(window.key_down(win, KEY_S)) + _probe_bit(window.key_down(win, KEY_D)) + " arrows " + _probe_bit(window.key_down(win, KEY_UP)) + _probe_bit(window.key_down(win, KEY_LEFT)) + _probe_bit(window.key_down(win, KEY_DOWN)) + _probe_bit(window.key_down(win, KEY_RIGHT)) + " ESC " + _probe_bit(window.key_down(win, KEY_ESCAPE)) + " SPC " + _probe_bit(window.key_down(win, KEY_SPACE)) + " ENT " + _probe_bit(window.key_down(win, KEY_ENTER)), x, y + line_h * 3.0, w, C_SUBTLE)
   _draw_text_fit(_probe_pad_text(pad_state), x, y + line_h * 4.0, w, C_SUBTLE)
   _draw_text_fit(_probe_axes_text(pad_state, 6), x, y + line_h * 5.0, w, C_SUBTLE)
   _draw_text_fit(_probe_raw_axes_text(pad_state, 8), x, y + line_h * 6.0, w, C_SUBTLE)
   _draw_text_fit(_probe_button_text(pad_state), x, y + line_h * 7.0, w, C_SUBTLE)
   _draw_text_fit(_probe_raw_button_text(pad_state, 18), x, y + line_h * 8.0, w, C_SUBTLE)
   _draw_text_fit(_probe_hats_text(pad_state), x, y + line_h * 9.0, w, C_SUBTLE)
   _draw_text_fit(_probe_devices_text(rows, best_jid), x, y + line_h * 10.0, w, C_SUBTLE)
   return 0
}

fn _probe_signature(any: st): str {
   if(!st){ return "none" }
   mut out = "jid=" + to_str(st.get("jid", -1)) +
      " name=" + st.get("name", "") +
      " mapped=" + _probe_bit(st.get("mapped", false))
   out = out + " mapped_axes"
   mut i = 0
   while(i < st.get("axis_count", 0) && i < 6){
      out = out + " " + _axis_label(i) + "=" + to_str(_axis_i100(_pad_axis(st, i)))
      i += 1
   }
   out = out + " raw_axes"
   i = 0
   while(i < st.get("raw_axis_count", st.get("axis_count", 0)) && i < 8){
      out = out + " A" + to_str(i) + "=" + to_str(_axis_i100(_raw_axis_value(st, i)))
      i += 1
   }
   out = out + " mapped_buttons"
   i = 0
   while(i < st.get("button_count", 0)){
      if(_pad_button(st, i)){ out = out + " " + _button_label(st, i) + "#" + to_str(i) }
      i += 1
   }
   out = out + " raw_buttons"
   i = 0
   while(i < st.get("raw_button_count", st.get("button_count", 0))){
      if(_raw_button_down(st, i)){ out = out + " B" + to_str(i) }
      i += 1
   }
   def hats_ptr = st.get("hats_ptr", 0)
   def hat_count = st.get("hat_count", 0)
   if(hat_count > 0){
      out = out + " hats"
      i = 0
      while(i < hat_count){
         out = out + " H" + to_str(i) + "=" + to_str(hats_ptr ? load8(hats_ptr, i) : 0)
         i += 1
      }
   }
   out
}

fn _probe_log_state(any: st): int {
   if(!st || !common.env_enabled("NY_GAMEPAD_INPUT_LOG")){ return 0 }
   def now = ticks()
   def sig = _probe_signature(st)
   if(sig != _probe_last_sig || _probe_last_log_ticks == 0 || (now - _probe_last_log_ticks) >= INPUT_LOG_INTERVAL_NS){
      print("[gamepad:input] " + sig)
      _probe_last_sig = sig
      _probe_last_log_ticks = now
   }
   0
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
   _queue_text("AXES", x, y, C_MUTED)
   mut row_y = y + line_h
   mut i = 0
   while(i < axis_count){
      def label = _axis_label(i)
      def val_s = _fixed2(_pad_axis(st, i))
      _queue_text(label, x, row_y, C_TEXT)
      _queue_text(val_s, x + w - _text_w(val_s), row_y, C_MUTED)
      row_y += line_h
      i += 1
   }
   def last_btn = st.get("last_button", -1)
   def last_s = "LAST"
   def pressed_s = "PRESSED"
   _queue_text(last_s, x, row_y, last_btn >= 0 ? C_ACCENT : C_MUTED)
   _queue_text(pressed_s, x + w - _text_w(pressed_s), row_y, C_TEXT)
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
   _queue_text("OTHER DEVICES", x, y, C_MUTED)
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
      _queue_text("+" + to_str(remaining) + " more", x, row_y, C_SUBTLE)
   }
   return 0
}

fn _draw_bottom_layout(list: rows, int: best_jid, any: pad_state, f64: ww, f64: wh, f64: pad, f64: gap, f64: header_h, f64: line_h, f64: axis_w, f64: footer_h, f64: devices_h, int: max_device_rows): int {
   def footer_y = wh - pad - footer_h
   def stage_y = pad + header_h + gap
   def stage_h = footer_y - gap - stage_y
   _draw_simple_dashboard(pad_state, pad, stage_y, ww - pad * 2.0, stage_h)
   if(devices_h > 0.0){
      _draw_device_block(rows, best_jid, pad, footer_y, max(48.0, ww - pad * 3.0 - axis_w - gap), max_device_rows, line_h)
   }
   _draw_axis_block(pad_state, ww - pad - axis_w, footer_y, axis_w, line_h)
   return 0
}

fn _draw_side_layout(list: rows, int: best_jid, any: pad_state, f64: ww, f64: wh, f64: pad, f64: gap, f64: header_h, f64: line_h, f64: axis_w, f64: devices_h, int: max_device_rows): int {
   def footer_y = wh - pad - devices_h
   def stage_y = pad + header_h + gap
   def stage_w = ww - pad * 3.0 - axis_w
   def stage_h = footer_y - gap - stage_y
   _draw_simple_dashboard(pad_state, pad, stage_y, stage_w, stage_h)
   _draw_axis_block(pad_state, ww - pad - axis_w, stage_y, axis_w, line_h)
   if(devices_h > 0.0){
      _draw_device_block(rows, best_jid, pad, footer_y, max(48.0, ww - pad * 2.0), max_device_rows, line_h)
   }
   return 0
}

fn _draw_scene(any: win, list: rows, int: best_jid, any: pad_state, f64: ww, f64: wh): int {
   def pf_scene0 = ticks()
   _text_runs = []
   _text_run_count = 0
   _text_char_count = 0
   _dbg("draw scene begin")
   clear_background(C_BG)
   if(best_jid == -1){
      _draw_empty_state(ww, wh)
      def empty_line_h = _line_h()
      if(_show_input_probe()){
         _draw_input_probe(win, rows, best_jid, 0, 24.0, wh - 24.0 - empty_line_h * 11.0, max(120.0, ww - 48.0), empty_line_h)
      }
      _flush_text_runs()
      if(_profile_enabled && (_profile_frame_index < 5 || (_profile_frame_index % _profile_every_frames) == 0)){
         def pf_scene1 = ticks()
         print("[gamepad:draw] frame=" + to_str(_profile_frame_index + 1) +
            " empty_ms=" + to_str(int((pf_scene1 - pf_scene0) / 1000000)) +
            " text_runs=" + to_str(_text_run_count) +
            " text_chars=" + to_str(_text_char_count))
      }
      return 0
   }
   def pf_scene1 = ticks()
   def line_h = _line_h()
   def pad = max(14.0, line_h * 0.55)
   def gap = max(10.0, line_h * 0.45)
   def max_device_rows = max(1, int((wh * 0.18) / line_h))
   def title = pad_state.get("name", "Unknown")
   def show_probe = _show_input_probe()
   def header_h = show_probe ? line_h * 12.0 : line_h * 2.0
   _dbg("draw metrics begin")
   def axis_w = _axis_block_width(pad_state, pad)
   def axis_h = _axis_block_height(pad_state, line_h)
   def devices_h = _device_block_height(rows, best_jid, max_device_rows, line_h)
   _dbg("draw metrics done")
   def pf_scene2 = ticks()
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
   def pf_scene3 = ticks()
   if(show_probe){
      _draw_input_probe(win, rows, best_jid, pad_state, pad, pad + line_h, max(80.0, ww - pad * 2.0), line_h)
   }
   def pf_scene4 = ticks()
   if(use_bottom){
      _draw_bottom_layout(rows, best_jid, pad_state, ww, wh, pad, gap, header_h, line_h, axis_w, footer_h, devices_h, max_device_rows)
   } else {
      _draw_side_layout(rows, best_jid, pad_state, ww, wh, pad, gap, header_h, line_h, axis_w, devices_h, max_device_rows)
   }
   def pf_scene_layout_done = ticks()
   _flush_text_runs()
   def pf_scene5 = ticks()
   if(_profile_enabled && (_profile_frame_index < 5 || (_profile_frame_index % _profile_every_frames) == 0)){
      print("[gamepad:draw] frame=" + to_str(_profile_frame_index + 1) +
         " clear_ms=" + to_str(int((pf_scene1 - pf_scene0) / 1000000)) +
         " metrics_ms=" + to_str(int((pf_scene2 - pf_scene1) / 1000000)) +
         " title_ms=" + to_str(int((pf_scene3 - pf_scene2) / 1000000)) +
         " probe_ms=" + to_str(int((pf_scene4 - pf_scene3) / 1000000)) +
         " layout_ms=" + to_str(int((pf_scene_layout_done - pf_scene4) / 1000000)) +
         " text_flush_ms=" + to_str(int((pf_scene5 - pf_scene_layout_done) / 1000000)) +
         " total_ms=" + to_str(int((pf_scene5 - pf_scene0) / 1000000)) +
         " text_runs=" + to_str(_text_run_count) +
         " text_chars=" + to_str(_text_char_count))
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

font = exutil.mono_font(
   exutil.demo_font_size("gamepad", 18.0),
   ["etc/assets/fonts/jetbrains.ttf", "etc/assets/fonts/maplemono.ttf", "etc/assets/fonts/monocraft.ttf"],
   exutil.demo_font_filter("gamepad", FONT_FILTER_LINEAR),
)
def auto_dump = common.env_enabled("NYTRIX_AUTO_DUMP")
def auto_dump_exit = common.env_enabled("NYTRIX_AUTO_DUMP_EXIT")
def auto_dump_path = common.env_trim("NYTRIX_AUTO_DUMP_PATH")
def auto_dump_delay = _env_dim("NYTRIX_AUTO_DUMP_DELAY_FRAMES", 8)
def demo_mode = common.env_enabled("NY_GAMEPAD_DEMO")
def profile_mode = common.env_enabled("NY_GAMEPAD_PROFILE")
def profile_every = max(1, _env_dim("NY_GAMEPAD_PROFILE_EVERY", 30))
_profile_enabled = profile_mode
_profile_every_frames = profile_every
mut auto_dump_done = false
mut frame_count = 0
mut startup_ticks = ticks()
while(!window.should_close(win)){
   def pf0 = ticks()
   if(exutil.step(win, startup_ticks, 0, true)){ break }
   def pf1 = ticks()
   if(!demo_mode){ _refresh_device_cache(false) }
   def pf2 = ticks()
   if(!begin_frame()){
      msleep(1)
      continue
   }
   def pf3 = ticks()
   def fb = _framebuffer_size_for_layout(win)
   def ww_i = int(fb.get(0, 1280))
   def wh_i = int(fb.get(1, 720))
   def ww = float(ww_i)
   def wh = float(wh_i)
   set_win_size(ww_i, wh_i)
   set_ortho_2d(0.0, ww, 0.0, wh)
   def joysticks = demo_mode ? _demo_rows() : _device_rows
   def best_jid = demo_mode ? 0 : _best_jid_cached
   def pad_state = demo_mode ? _demo_pad_state() : ((best_jid != -1) ? _snapshot_pad_state(best_jid) : 0)
   _probe_log_state(pad_state)
   def pf4 = ticks()
   _profile_frame_index = frame_count
   _draw_scene(win, joysticks, best_jid, pad_state, ww, wh)
   def pf5 = ticks()
   if(pad_state){ _release_pad_state(pad_state) }
   if(auto_dump && !auto_dump_done && (frame_count + 1) >= auto_dump_delay){
      request_frame_capture()
   }
   end_frame()
   def pf6 = ticks()
   if(profile_mode && (frame_count < 5 || (frame_count % profile_every) == 0)){
      print("[gamepad:prof] frame=" + to_str(frame_count + 1) +
         " step_ms=" + to_str(int((pf1 - pf0) / 1000000)) +
         " refresh_ms=" + to_str(int((pf2 - pf1) / 1000000)) +
         " begin_ms=" + to_str(int((pf3 - pf2) / 1000000)) +
         " snapshot_ms=" + to_str(int((pf4 - pf3) / 1000000)) +
         " draw_ms=" + to_str(int((pf5 - pf4) / 1000000)) +
         " end_ms=" + to_str(int((pf6 - pf5) / 1000000)) +
         " total_ms=" + to_str(int((pf6 - pf0) / 1000000)))
   }
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
