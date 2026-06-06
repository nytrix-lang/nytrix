;; Keywords: viewer clipboard text copy paste os ui render
;; Clipboard bridge helpers for text editing and viewer commands.
;; References:
;; - std.os.ui.window
module std.os.ui.render.viewer.clipboard(
   make, input_text, clipboard_text, editing, set_input, set_editing,
   update, write, cut, copy, paste, read, clear, process_events, draw_panel
)

use std.core
use std.core.str as str
use std.math (max)
use std.os.ui.window.consts as key
use std.os.ui.render as gfx
use std.os.ui.render.viewer.widgets
use std.os.ui.window

def MAX_TEXT = 256
def ACTIONS = ["CUT", "COPY", "PASTE", "CLEAR", "READ"]

fn _status(dict st, str label) dict {
   st["status"] = label
   st["status_timer"] = 1.25
   st
}

fn _delete_last(str s) str {
   def n = str.utf8_len(s)
   n <= 0 ? "" : str.utf8_slice(s, 0, n - 1)
}

fn make(any win, str text="") dict {
   "Creates clipboard UI state seeded with optional input text."
   {
      "input": text,
      "clipboard": window.get_clipboard(win),
      "editing": true,
      "status": "ready",
      "status_timer": 0.0
   }
}

fn input_text(dict st) str { to_str(st.get("input", "")) }

fn clipboard_text(dict st) str { to_str(st.get("clipboard", "")) }

fn editing(dict st) bool { st.get("editing", true) }

fn set_input(dict st, str text, str label="input") dict {
   "Replaces the editable input text and marks the text field active."
   st["input"] = widgets.limit_text(text, MAX_TEXT)
   st["editing"] = true
   _status(st, label)
}

fn set_editing(dict st, bool active) dict {
   "Sets whether typed character events edit the input field."
   st["editing"] = active
   st
}

fn update(dict st, f64 dt) dict {
   "Advances transient status timers."
   st["status_timer"] = max(0.0, float(st.get("status_timer", 0.0)) - dt)
   st
}

fn write(any win, dict st, str s, str label="copied") dict {
   "Writes text to the OS clipboard and mirrors the resulting clipboard text."
   window.set_clipboard(win, s)
   st["clipboard"] = window.get_clipboard(win)
   _status(st, label)
}

fn cut(any win, dict st) dict {
   "Copies the input text, clears it, and keeps editing active."
   st = write(win, st, input_text(st), "cut")
   st["input"] = ""
   st["editing"] = true
   st
}

fn copy(any win, dict st) dict {
   "Copies the input text to the OS clipboard."
   write(win, st, input_text(st), "copied")
}

fn paste(any win, dict st) dict {
   "Reads the OS clipboard into the input field."
   def clip = window.get_clipboard(win)
   st["clipboard"] = clip
   st["input"] = widgets.limit_text(clip, MAX_TEXT)
   st["editing"] = true
   _status(st, "pasted")
}

fn read(any win, dict st) dict {
   "Refreshes the mirrored clipboard text without editing input."
   st["clipboard"] = window.get_clipboard(win)
   _status(st, "read")
}

fn clear(dict st) dict {
   "Clears input text and keeps editing active."
   st["input"] = ""
   st["editing"] = true
   _status(st, "cleared")
}

fn _event_ctrl(any win, any data) bool {
   def mods = int(data.get("mods", data.get("mod", window.get_modifiers(win))))
   (mods & key.MOD_CONTROL) != 0
}

fn _handle_event(any win, dict st, any e) dict {
   def typ = window.event_type(e)
   def data = window.event_data(e)
   if(typ == key.EVENT_KEY_PRESSED && is_dict(data)){
      def ctrl = _event_ctrl(win, data)
      if(editing(st) && window.event_key_is(data, key.KEY_BACKSPACE)){
         st["input"] = _delete_last(input_text(st))
      }
      if(ctrl && window.event_key_is(data, key.KEY_C)){ st = copy(win, st) }
      if(editing(st) && ctrl && window.event_key_is(data, key.KEY_X)){ st = cut(win, st) }
      if(editing(st) && ctrl && window.event_key_is(data, key.KEY_V)){ st = paste(win, st) }
   } elif(typ == key.EVENT_KEY_CHAR && editing(st) && is_dict(data)){
      def cp = int(data.get("char", 0))
      def mods = int(data.get("mods", data.get("mod", 0)))
      if((mods & key.MOD_CONTROL) == 0 && cp >= 32 && cp != 127 && str.utf8_len(input_text(st)) < MAX_TEXT){
         st["input"] = input_text(st) + chr(cp)
      }
   }
   st
}

fn process_events(any win, dict st) dict {
   "Consumes queued window events relevant to clipboard shortcuts and text input."
   mut e = window.check_event(win)
   while(e){
      st = _handle_event(win, st, e)
      e = window.check_event(win)
   }
   st
}

fn _action_color(str label) any {
   case label {
      "COPY" -> widgets.C_ACCENT_HI
      "CLEAR" -> widgets.C_LINE
      "READ" -> widgets.C_MUTED
      _ -> widgets.C_ACCENT
   }
}

fn _run_action(any win, dict st, str label) dict {
   case label {
      "CUT" -> cut(win, st)
      "COPY" -> copy(win, st)
      "PASTE" -> paste(win, st)
      "CLEAR" -> clear(st)
      "READ" -> read(win, st)
      _ -> st
   }
}

fn _action(any font, dict st, any win, int idx, int cols, str label, f64 x, f64 y, f64 bw, f64 bh, f64 gap, f64 mx, f64 my, bool click) dict {
   def col = idx % cols
   def row = int(idx / cols)
   if(!widgets.button(font, label, x + float(col) * (bw + gap), y + float(row) * (bh + gap), bw, bh, mx, my, click, _action_color(label))){ return st }
   _run_action(win, st, label)
}

fn draw_panel(any win, dict st, any font_body, any font_small, f64 x, f64 y, f64 w, f64 h, f64 mx, f64 my, bool click, bool caret) dict {
   "Draws the clipboard panel and applies clicked actions."
   widgets.panel(font_small, x, y, w, h, "CLIPBOARD", widgets.C_ACCENT)
   def status = float(st.get("status_timer", 0.0)) > 0.0 ? to_str(st.get("status", "ready")) : "ready"
   gfx.draw_text(font_small, status, x + w - 70.0, y + 16.0, float(st.get("status_timer", 0.0)) > 0.0 ? widgets.C_ACCENT_HI : widgets.C_MUTED)
   def inner = 14.0
   def field_y = y + 54.0
   def field_h = 44.0
   def content_w = max(90.0, w - inner * 2.0)
   if(click){ st = set_editing(st, widgets.hit(mx, my, x + inner, field_y, content_w, field_h)) }
   widgets.text_box(font_body, font_small, "Input", input_text(st), x + inner, field_y, content_w, field_h, editing(st), caret)
   def gap = 7.0
   def cols = w < 300.0 ? 2 : 3
   def bw = (content_w - gap * float(cols - 1)) / float(cols)
   def bh = 34.0
   def by = field_y + field_h + 20.0
   mut i = 0
   while(i < ACTIONS.len){
      st = _action(font_body, st, win, i, cols, ACTIONS.get(i, ""), x + inner, by, bw, bh, gap, mx, my, click)
      i += 1
   }
   def rows = int((5 + cols - 1) / cols)
   def clip_y = by + float(rows) * (bh + gap) + 12.0
   def clip_h = max(36.0, y + h - clip_y - inner)
   gfx.draw_rect(x + inner, clip_y, content_w, clip_h, gfx.color_alpha(widgets.C_DIM, 0.34))
   gfx.draw_rectangle_lines(x + inner, clip_y, content_w, clip_h, widgets.C_LINE, 1.0)
   gfx.draw_text(font_small, "Clipboard", x + inner + 10.0, clip_y + 12.0, widgets.C_MUTED)
   gfx.draw_text(font_body, widgets.preview(clipboard_text(st), int(max(10.0, (content_w - 20.0) / 9.8))), x + inner + 10.0, clip_y + 38.0, clipboard_text(st).len > 0 ? widgets.C_TEXT : widgets.C_MUTED)
   st
}

#main {
   mut st = make(0, "abc")
   assert(widgets.preview("abcdef", 4) == "a..." && widgets.limit_text("abc", 8) == "abc", "clipboard text helpers")
   st = update(st, 0.5)
   st = set_input(st, "abcd", "set")
   assert(input_text(st) == "abcd" && editing(st), "clipboard state")
   print("✓ std.os.ui.render.viewer.clipboard self-test passed")
}
