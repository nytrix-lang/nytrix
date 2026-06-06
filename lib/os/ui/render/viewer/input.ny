;; Keywords: viewer input mouse keyboard pointer os ui render
;; Pointer and keyboard input state helpers shared by viewer UI surfaces.
;; References:
;; - std.os.ui.window
;; - std.os.ui.render.viewer.keyboard
module std.os.ui.render.viewer.input(
   pad, pad_active, pad_info, draw_gamepad, draw_keyboard
)

use std.core
use std.math (clamp, max, min)
use std.os.ui.render
use std.os.ui.render.viewer
use std.os.ui.render.viewer.gamepad
use std.os.ui.render.viewer.keyboard
use std.os.ui.render.viewer.widgets
use std.os.ui.window

mut _fps_value = -1
mut _fps_text = "fps 0"
mut _pad_header_key = ""
mut _pad_header_detail = ""
mut _key_header_key = ""
mut _key_header_detail = ""
mut _mouse_key = ""
mut _mouse_detail = ""

fn pad(f64 sw, f64 sh) f64 {
   "Returns responsive outer padding for the input demo."
   clamp(min(sw, sh) * 0.05, 18.0, 42.0)
}

fn _cached_fps(int fps) str {
   if(fps != _fps_value){
      _fps_value = fps
      _fps_text = "fps " + to_str(fps)
   }
   _fps_text
}

fn _header(any font_title, any font_body, str title, str detail, int fps, f64 sw, f64 pad) int {
   render.draw_text(font_title, title, pad, pad + 4.0, widgets.C_TEXT)
   render.draw_text(font_body, detail, pad, pad + 42.0, widgets.C_MUTED)
   render.draw_text(font_body, _cached_fps(fps), max(pad, sw - 96.0), pad + 6.0, widgets.C_MUTED)
   0
}

fn pad_active(any st) bool {
   "Returns whether pad active."
   if(!st){ return false }
   if(st.get("pressed_labels", []).len > 0){ return true }
   mut i = 0
   while(i < st.get("axis_count", 0) && i < 4){
      def v = gamepad.display_axis_slot(st, i)
      if(v < -0.35 || v > 0.35){ return true }
      i += 1
   }
   false
}

fn pad_info(any pad_state, list rows, int jid) dict {
   "Builds formatted gamepad diagnostic text."
   if(!pad_state){ return dict(0) }
   {
      "pad": gamepad.pad_text(pad_state),
      "mapped_axes": gamepad.axes_text(pad_state, 6),
      "raw_axes": gamepad.axes_text(pad_state, 8, true),
      "buttons": gamepad.button_text(pad_state),
      "raw_buttons": gamepad.raw_button_text(pad_state, 18),
      "hats": gamepad.hats_text(pad_state, "", "HATS"),
      "devices": gamepad.devices_text(rows, jid),
   }
}

fn _draw_waiting(any font_body, f64 sw, f64 sh, f64 pad) int {
   def w = clamp(sw - pad * 2.0, 320.0, 680.0)
   def h = 150.0
   def x = sw * 0.5 - w * 0.5
   def y = sh * 0.5 - h * 0.5
   widgets.panel(font_body, x, y, w, h, "GAMEPAD", widgets.C_ACCENT)
   render.draw_text(font_body, "Move a stick or press a gamepad button to switch here.", x + 18.0, y + 70.0, widgets.C_TEXT)
   render.draw_text(font_body, "Tab switches source. Any normal key returns to keyboard.", x + 18.0, y + 100.0, widgets.C_MUTED)
   0
}

fn _info_line(any label, any text, f64 x, f64 y, f64 w) f64 {
   viewer.draw_text_fit(label, x, y, w, viewer.C_MUTED)
   viewer.draw_text_fit(text, x, y + viewer.line_h(), w, viewer.C_TEXT)
   y + viewer.line_h() * 2.35
}

fn _gamepad_header(any pad_state, list rows, int jid) str {
   def name = pad_state.get("name", "Controller")
   def key = name + "|" + to_str(jid) + "|" + to_str(rows.len)
   if(key != _pad_header_key){
      _pad_header_key = key
      _pad_header_detail = name + "  jid " + to_str(jid) + "  devices " + to_str(rows.len) + "  Tab: keyboard"
   }
   _pad_header_detail
}

fn draw_gamepad(any font_title, any font_body, any pad_state, list rows, int jid, int fps, f64 sw, f64 sh, f64 pad, any info=0) int {
   "Draws the gamepad view with mapped and raw input state."
   if(!pad_state){
      _header(font_title, font_body, "Input: gamepad", "No active controller detected. Press Tab for keyboard.", fps, sw, pad)
      return _draw_waiting(font_body, sw, sh, pad)
   }
   _header(font_title, font_body, "Input: gamepad", _gamepad_header(pad_state, rows, jid), fps, sw, pad)
   def top = pad + 82.0
   def info_w = clamp(sw * 0.34, 260.0, 390.0)
   def stage_w = max(260.0, sw - pad * 3.0 - info_w)
   def stage_h = max(220.0, sh - top - pad)
   viewer.begin_text()
   viewer.draw_controller(pad_state, pad, top, stage_w, stage_h)
   def x = pad * 2.0 + stage_w
   mut y = top
   def lines = is_dict(info) ? info : pad_info(pad_state, rows, jid)
   y = _info_line("PAD", lines.get("pad", ""), x, y, info_w)
   y = _info_line("MAPPED AXES", lines.get("mapped_axes", ""), x, y, info_w)
   y = _info_line("RAW AXES", lines.get("raw_axes", ""), x, y, info_w)
   y = _info_line("BUTTONS", lines.get("buttons", ""), x, y, info_w)
   y = _info_line("RAW BUTTONS", lines.get("raw_buttons", ""), x, y, info_w)
   y = _info_line("HATS", lines.get("hats", ""), x, y, info_w)
   viewer.draw_text_fit("DEVICES", x, y, info_w, viewer.C_MUTED)
   viewer.draw_text_fit(lines.get("devices", ""), x, y + viewer.line_h(), info_w, viewer.C_SUBTLE)
   viewer.flush_text()
   0
}

fn _mouse_text(any win, list pos) str {
   def mx = int(pos.get(0, 0))
   def my = int(pos.get(1, 0))
   def l = window.mouse_down(win, 0) ? 1 : 0
   def r = window.mouse_down(win, 1) ? 1 : 0
   def m = window.mouse_down(win, 2) ? 1 : 0
   def key = to_str(mx) + "|" + to_str(my) + "|" + to_str(l) + "|" + to_str(r) + "|" + to_str(m)
   if(key != _mouse_key){
      _mouse_key = key
      _mouse_detail = "mouse " + to_str(mx) + "," + to_str(my) + "  L" + to_str(l) + " R" + to_str(r) + " M" + to_str(m)
   }
   _mouse_detail
}

fn _keyboard_header(str label, any code) str {
   def key = label + "|" + to_str(code)
   if(key != _key_header_key){
      _key_header_key = key
      _key_header_detail = "last key: " + label + " (" + to_str(code) + ")  Tab: gamepad"
   }
   _key_header_detail
}

fn draw_keyboard(any win, any font_title, any font_body, any font_key, str label, any code, f64 timer, int fps, f64 sw, f64 sh, f64 pad) int {
   "Draws the keyboard view with mouse and last-key diagnostics."
   _header(font_title, font_body, "Input: keyboard", _keyboard_header(label, code), fps, sw, pad)
   def pos = window.mouse_pos(win)
   keyboard.draw_board_at(win, font_key, sw, sh, pad, float(pos.get(0, 0.0)), float(pos.get(1, 0.0)))
   render.draw_text(font_body, _mouse_text(win, pos), pad, sh - pad - 52.0, widgets.C_MUTED)
   if(timer > 0.0){ render.draw_text(font_body, "pressed", pad, sh - pad - 24.0, widgets.C_KEY_DOWN) }
   0
}

#main {
   assert(!pad_active(0), "empty pad is inactive")
   assert(pad(1040.0, 620.0) > 0.0, "input pad")
   print("✓ std.os.ui.render.viewer.input self-test passed")
}
