;; Keywords: viewer gamepad input controller os ui render
;; Gamepad input helpers for viewer navigation and editor control surfaces.
;; References:
;; - std.os.ui.window.input.gamepad
module std.os.ui.render.viewer.gamepad(
   axis_i100, axis_f, safe_axis_value, fixed2, axis_label, button_label, button_chip_label,
   button_side_label, is_sony_pad, joysticks, device_row, refresh, rows, best_jid, snapshot,
   release, pad_button, pad_axis, display_axis_slot, raw_axis_value, raw_button_down, axes_text,
   pad_text, button_text, raw_button_text, hats_text, raw_device_text, devices_text, signature
)

use std.core
use std.os (ticks)
use std.os.ui.window.input.gamepad as input_gamepad
use std.core.str as str

mut _device_rows = []
mut _best_jid_cached = -1
mut _last_device_scan_ticks = 0
def DEVICE_SCAN_INTERVAL_NS = 1000000000
def DEVICE_SCAN_EMPTY_INTERVAL_NS = 250000000
def _BTN_LABELS_GENERIC = ["A", "B", "X", "Y", "LB", "RB", "BACK", "START", "GUIDE", "L3", "R3", "UP", "RIGHT", "DOWN", "LEFT", "EX15", "EX16"]
def _BTN_LABELS_SONY = ["X", "O", "SQ", "TRI", "L1", "R1", "SHARE", "OPT", "PS", "L3", "R3", "UP", "RIGHT", "DOWN", "LEFT", "TOUCH", "MIC"]
def _BTN_CHIP_LABELS_GENERIC = ["A", "B", "X", "Y", "LB", "RB", "BK", "ST", "GD", "L3", "R3", "UP", "RGT", "DWN", "LFT", "15", "16"]
def _BTN_CHIP_LABELS_SONY = ["X", "O", "SQ", "TRI", "L1", "R1", "SHR", "OPT", "PS", "L3", "R3", "UP", "RGT", "DWN", "LFT", "TCH", "MIC"]
def _BTN_SIDE_LABELS_GENERIC = ["A", "B", "X", "Y", "LB", "RB", "BK", "ST", "GD", "L3", "R3", "U", "R", "D", "L", "15", "16"]
def _BTN_SIDE_LABELS_SONY = ["X", "O", "SQ", "TR", "L1", "R1", "SH", "OP", "PS", "L3", "R3", "U", "R", "D", "L", "TC", "MC"]

fn axis_i100(any v) int {
   mut n = int(v * 100.0)
   if(n > -4 && n < 4){ n = 0 }
   if(n < -100){ n = -100 }
   if(n > 100){ n = 100 }
   n
}

fn axis_f(int n) f64 { n * 0.01 }

fn safe_axis_value(any v) f64 { axis_f(axis_i100(v)) }

fn fixed2(any v) str { to_str(axis_f(axis_i100(v))) }

fn _name_has(any name, any terms) bool {
   def lname = str.lower(to_str(name))
   mut i = 0
   while(i < terms.len){
      if(str.find(lname, terms.get(i, "")) != -1){ return true }
      i += 1
   }
   false
}

fn is_sony_pad(any st) bool { _name_has(st.get("name", ""), ["sony", "dual", "playstation", "dualsense", "dualshock", "ps5", "ps4"]) }

fn _button_label_from(any st, any idx, any generic, any sony, any prefix) str {
   def fallback = prefix + to_str(idx)
   if(!st.get("mapped", false)){ return "B" + to_str(idx) }
   is_sony_pad(st) ? sony.get(idx, fallback) : generic.get(idx, fallback)
}

fn button_label(any st, any idx) str { _button_label_from(st, idx, _BTN_LABELS_GENERIC, _BTN_LABELS_SONY, "B") }

fn button_chip_label(any st, any idx) str { _button_label_from(st, idx, _BTN_CHIP_LABELS_GENERIC, _BTN_CHIP_LABELS_SONY, "B") }

fn button_side_label(any st, any idx) str { _button_label_from(st, idx, _BTN_SIDE_LABELS_GENERIC, _BTN_SIDE_LABELS_SONY, "B") }

fn axis_label(any idx) str { ["LX", "LY", "RX", "RY", "LT", "RT"].get(idx, "A" + to_str(idx)) }

fn joysticks() list { input_gamepad.gamepads() }

fn _native_gamepad_name(any jid) str { input_gamepad.gamepad_name(jid) }

fn _score_name_terms(str lname, list terms, list scores) int {
   mut score = 0
   mut i = 0
   while(i < terms.len){
      if(str.find(lname, terms.get(i, "")) != -1){ score += int(scores.get(i, 0)) }
      i += 1
   }
   score
}

fn _device_score(any jid) int {
   def lname = str.lower(_native_gamepad_name(jid))
   (input_gamepad.gamepad_mapped(jid) ? 100 : 0) +
   _score_name_terms(lname,
      ["controller", "gamepad", "xbox", "dual", "shock", "sony", "8bitdo", "logitech", "keyboard", "mouse", "touchpad", "motion sensor", "motion sensors", "k400"],
   [60, 60, 60, 70, 50, 60, 50, 40, -400, -300, -1000, -1000, -1000, -500])
}

fn device_row(any jid) dict {
   def name = _native_gamepad_name(jid)
   def mapped = input_gamepad.gamepad_mapped(jid)
   def raw = input_gamepad.gamepad_raw_snapshot(jid)
   {"jid": jid, "name": name, "mapped": mapped, "score": _device_score(jid),
   "axis_count": raw.get("raw_axis_count", 0), "button_count": raw.get("raw_button_count", 0), "hat_count": raw.get("hat_count", 0)}
}

fn _best_jid_from_rows(any rows) int {
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

fn refresh(any force=false) int {
   def now = ticks()
   def scan_interval = _device_rows.len == 0 ? DEVICE_SCAN_EMPTY_INTERVAL_NS : DEVICE_SCAN_INTERVAL_NS
   if(!force && _last_device_scan_ticks != 0 && (now - _last_device_scan_ticks) < scan_interval){
      return 0
   }
   def jids = joysticks()
   def jids_n = jids.len
   mut rows = []
   mut i = 0
   while(i < jids_n){
      rows = rows.append(device_row(jids.get(i)))
      i += 1
   }
   _device_rows = rows
   _best_jid_cached = _best_jid_from_rows(rows)
   _last_device_scan_ticks = now
   0
}

fn _cached_device_row(any jid) any {
   def rows_n = _device_rows.len
   mut i = 0
   while(i < rows_n){
      def row = _device_rows.get(i)
      if(row.get("jid", -1) == jid){ return row }
      i += 1
   }
   0
}

fn _read_raw_state(any jid, any mapped) dict {
   mut raw = input_gamepad.gamepad_raw_snapshot(jid)
   raw["axis_count"] = mapped ? input_gamepad.gamepad_axis_count(jid) : int(raw.get("raw_axis_count", 0))
   raw["button_count"] = mapped ? input_gamepad.gamepad_button_count(jid) : int(raw.get("raw_button_count", 0))
   raw
}

fn _collect_pressed_buttons(any st) list {
   mut last_btn = -1
   mut pressed_count = 0
   mut pressed_labels = []
   mut bi = 0
   def button_count = st.get("button_count", 0)
   while(bi < button_count){
      if(pad_button(st, bi)){
         last_btn = bi
         pressed_count += 1
         if(pressed_labels.len < 8){ pressed_labels = pressed_labels.append(button_chip_label(st, bi)) }
      }
      bi += 1
   }
   [last_btn, pressed_count, pressed_labels]
}

fn snapshot(any jid) any {
   def cached_row = _cached_device_row(jid)
   def cached_name = cached_row ? cached_row.get("name", "") : ""
   def mapped = cached_row ? bool(cached_row.get("mapped", false)) : input_gamepad.gamepad_mapped(jid)
   mut st = _read_raw_state(jid, mapped)
   st["jid"] = jid st["mapped"] = mapped st["name"] = cached_name.len > 0 ? cached_name : _native_gamepad_name(jid)
   def pressed = _collect_pressed_buttons(st)
   st["last_button"] = pressed.get(0, -1)
   st["pressed_count"] = pressed.get(1, 0)
   st["pressed_labels"] = pressed.get(2, [])
   st
}

fn release(any st) int { 0 }

fn pad_button(any st, any button) bool {
   def buttons = st.get("buttons", nil)
   if(is_list(buttons) && button >= 0 && button < buttons.len){ return bool(buttons.get(button, false)) }
   def jid = int(st.get("jid", -1))
   if(st.get("mapped", false) && jid >= 0 && button >= 0 && button < 15){ return input_gamepad.gamepad_button(jid, button) }
   input_gamepad.gamepad_raw_button(st, int(button))
}

fn pad_axis(any st, any axis) f64 {
   def axes = st.get("axes", nil)
   if(is_list(axes) && axis >= 0 && axis < axes.len){ return safe_axis_value(axes.get(axis, 0.0)) }
   def jid = int(st.get("jid", -1))
   if(st.get("mapped", false) && jid >= 0 && axis >= 0 && axis < 6){ return safe_axis_value(input_gamepad.gamepad_axis(jid, axis)) }
   input_gamepad.gamepad_raw_axis(st, int(axis))
}

fn raw_axis_value(any st, int idx) f64 {
   st ? input_gamepad.gamepad_raw_axis(st, idx) : 0.0
}

fn raw_button_down(any st, int idx) bool {
   st && input_gamepad.gamepad_raw_button(st, idx)
}

fn axes_text(any st, int max_axes, bool raw=false) str {
   if(!st){ return raw ? "raw axes -" : "axes -" }
   mut out = raw ? "raw axes" : (st.get("mapped", false) ? "mapped axes" : "axes")
   def axis_count = raw ? st.get("raw_axis_count", st.get("axis_count", 0)) : st.get("axis_count", 0)
   def shown = axis_count < max_axes ? axis_count : max_axes
   mut i = 0
   while(i < shown){
      def label = raw ? ("A" + to_str(i)) : axis_label(i)
      def value = raw ? raw_axis_value(st, i) : display_axis_slot(st, i)
      out = out + " " + label + "=" + fixed2(value)
      i += 1
   }
   if(axis_count > shown){ out = out + " +" + to_str(axis_count - shown) }
   out
}

fn pad_text(any st) str {
   if(!st){ return "pad none" }
   def last_btn = st.get("last_button", -1)
   "pad mapped " + (st.get("mapped", false) ? "1" : "0") +
   " axes " + to_str(st.get("axis_count", 0)) +
   "/" + to_str(st.get("raw_axis_count", st.get("axis_count", 0))) +
   " buttons " + to_str(st.get("button_count", 0)) +
   "/" + to_str(st.get("raw_button_count", st.get("button_count", 0))) +
   " hats " + to_str(st.get("hat_count", 0)) +
   " pressed " + to_str(st.get("pressed_count", 0)) +
   " last " + (last_btn >= 0 ? button_label(st, last_btn) : "-")
}

fn button_text(any st) str {
   if(!st){ return "buttons -" }
   def pressed = st.get("pressed_labels", [])
   if(pressed.len == 0){ return "buttons -" }
   mut out = "buttons"
   mut i = 0
   while(i < pressed.len){
      out = out + " " + pressed.get(i, "")
      i += 1
   }
   out
}

fn raw_button_text(any st, int max_buttons) str {
   if(!st){ return "raw buttons -" }
   def button_count = st.get("raw_button_count", st.get("button_count", 0))
   mut out = "raw buttons"
   mut shown = 0
   mut pressed = 0
   mut bi = 0
   while(bi < button_count){
      if(raw_button_down(st, bi)){
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

fn hats_text(any st, str empty="hats -", str prefix="hats") str {
   if(!st){ return empty }
   def hat_count = st.get("hat_count", 0)
   if(hat_count <= 0){ return empty }
   mut out = prefix
   mut hi = 0
   mut any_down = false
   while(hi < hat_count){
      def hv = input_gamepad.gamepad_raw_hat(st, hi)
      if(hv != 0){ any_down = true }
      out = out + " H" + to_str(hi) + "=" + to_str(hv)
      hi += 1
   }
   any_down ? out : empty
}

fn raw_device_text(any row) str {
   def jid = row.get("jid", -1)
   if(jid < 0){ return "" }
   def raw = input_gamepad.gamepad_raw_snapshot(jid)
   def ac, bc = int(raw.get("raw_axis_count", 0)), int(raw.get("raw_button_count", 0))
   mut pressed = 0
   mut bi = 0
   while(bi < bc){
      if(input_gamepad.gamepad_raw_button(raw, bi)){ pressed += 1 }
      bi += 1
   }
   def a0 = ac > 0 ? fixed2(input_gamepad.gamepad_raw_axis(raw, 0)) : "-"
   def a1 = ac > 1 ? fixed2(input_gamepad.gamepad_raw_axis(raw, 1)) : "-"
   "[" + to_str(jid) + "] a" + to_str(ac) + "(" + a0 + "," + a1 + ")" +
   " b" + to_str(bc) + " p" + to_str(pressed) +
   " h" + to_str(raw.get("hat_count", 0)) + " " + row.get("name", "")
}

fn devices_text(list rows, int best_jid) str {
   mut out = "raw"
   mut shown = 0
   mut remaining = 0
   mut i = 0
   while(i < rows.len){
      def row = rows.get(i)
      def jid = row.get("jid", -1)
      if(jid != best_jid){
         if(shown < 3){
            out = out + " " + raw_device_text(row)
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

fn signature(any st) str {
   if(!st){ return "none" }
   mut out = "jid=" + to_str(st.get("jid", -1)) +
   " name=" + st.get("name", "") +
   " mapped=" + (st.get("mapped", false) ? "1" : "0") +
   " mapped_axes"
   mut i = 0
   while(i < st.get("axis_count", 0) && i < 6){
      out = out + " " + axis_label(i) + "=" + to_str(axis_i100(display_axis_slot(st, i)))
      i += 1
   }
   out = out + " raw_axes"
   i = 0
   while(i < st.get("raw_axis_count", st.get("axis_count", 0)) && i < 8){
      out = out + " A" + to_str(i) + "=" + to_str(axis_i100(raw_axis_value(st, i)))
      i += 1
   }
   out = out + " mapped_buttons"
   i = 0
   while(i < st.get("button_count", 0)){
      if(pad_button(st, i)){ out = out + " " + button_label(st, i) + "#" + to_str(i) }
      i += 1
   }
   out = out + " raw_buttons"
   i = 0
   while(i < st.get("raw_button_count", st.get("button_count", 0))){
      if(raw_button_down(st, i)){ out = out + " B" + to_str(i) }
      i += 1
   }
   def hat_count = st.get("hat_count", 0)
   if(hat_count > 0){
      out = out + " hats"
      i = 0
      while(i < hat_count){
         out = out + " H" + to_str(i) + "=" + to_str(input_gamepad.gamepad_raw_hat(st, i))
         i += 1
      }
   }
   out
}

fn _axis_from_raw_map(any st, any axis, any lx, any ly, any rx, any ry, any lt, any rt) f64 {
   def slots = [lx, ly, rx, ry, lt, rt]
   def idx = int(axis)
   pad_axis(st, (idx >= 0 && idx < slots.len) ? slots.get(idx, axis) : axis)
}

fn display_axis_slot(any st, any axis) f64 {
   if(st.get("mapped", false)){ return pad_axis(st, axis) }
   #windows {
      if(is_sony_pad(st)){
         return _axis_from_raw_map(st, axis, 0, 1, 2, 5, 3, 4)
      }
      return _axis_from_raw_map(st, axis, 0, 1, 2, 3, 5, 4)
   }
   #else {
      return _axis_from_raw_map(st, axis, 0, 1, 3, 4, 2, 5)
   }
   #endif
}

fn rows() list { _device_rows }

fn best_jid() int { _best_jid_cached }

#main {
   def st = {"mapped": true, "name": "Demo DualSense Wireless Controller", "axes": [-0.62, 0.34], "buttons": [true]}
   assert(st.get("mapped", false) && axis_label(0) == "LX", "gamepad device demo state")
   assert(button_label(st, 0).len > 0 && fixed2(0.25).len > 0, "gamepad device labels")
   print("✓ std.os.ui.render.viewer.gamepad self-test passed")
}
