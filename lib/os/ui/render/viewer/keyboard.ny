;; Keywords: viewer keyboard shortcut chord input os ui render
;; Keyboard shortcut normalization and chord state helpers for viewer tools.
;; References:
;; - std.os.ui.window.consts
;; - std.os.ui.render.viewer.input
module std.os.ui.render.viewer.keyboard(
   KEY_GAP, ROWS,
   row_width, keyboard_width, keyboard_height, keyboard_layout,
   is_viewer_menu_hotkey, draw_status, draw_keycap, draw_board_at, draw_board, scan_pressed
)

use std.core
use std.math (clamp, max, min)
use std.os.ui.window.consts as key
use std.os.ui.render as gfx
use std.os.ui.render.viewer.widgets
use std.os.ui.window

def KEY_GAP = 4.0
def KEYBOARD_BASE_W = 748.0
def KEYBOARD_BASE_H = 240.0
def ROWS = [
   [30.0, [
         [key.KEY_ESCAPE, 45.0, "ESC"], [key.KEY_F1, 45.0, "F1"], [key.KEY_F2, 45.0, "F2"],
         [key.KEY_F3, 45.0, "F3"], [key.KEY_F4, 45.0, "F4"], [key.KEY_F5, 45.0, "F5"],
         [key.KEY_F6, 45.0, "F6"], [key.KEY_F7, 45.0, "F7"], [key.KEY_F8, 45.0, "F8"],
         [key.KEY_F9, 45.0, "F9"], [key.KEY_F10, 45.0, "F10"], [key.KEY_F11, 45.0, "F11"],
         [key.KEY_F12, 45.0, "F12"], [key.KEY_PRINT_SCREEN, 62.0, "PRINT"], [key.KEY_PAUSE, 45.0, "PAUSE"]
   ]],
   [38.0, [
         [key.KEY_GRAVE, 25.0, "`"], [key.KEY_1, 45.0, "1"], [key.KEY_2, 45.0, "2"],
         [key.KEY_3, 45.0, "3"], [key.KEY_4, 45.0, "4"], [key.KEY_5, 45.0, "5"],
         [key.KEY_6, 45.0, "6"], [key.KEY_7, 45.0, "7"], [key.KEY_8, 45.0, "8"],
         [key.KEY_9, 45.0, "9"], [key.KEY_0, 45.0, "0"], [key.KEY_MINUS, 45.0, "-"],
         [key.KEY_EQUAL, 45.0, "="], [key.KEY_BACKSPACE, 82.0, "BACK"], [key.KEY_DELETE, 45.0, "DEL"]
   ]],
   [38.0, [
         [key.KEY_TAB, 50.0, "TAB"], [key.KEY_Q, 45.0, "Q"], [key.KEY_W, 45.0, "W"],
         [key.KEY_E, 45.0, "E"], [key.KEY_R, 45.0, "R"], [key.KEY_T, 45.0, "T"],
         [key.KEY_Y, 45.0, "Y"], [key.KEY_U, 45.0, "U"], [key.KEY_I, 45.0, "I"],
         [key.KEY_O, 45.0, "O"], [key.KEY_P, 45.0, "P"], [key.KEY_LEFT_BRACKET, 45.0, "["],
         [key.KEY_RIGHT_BRACKET, 45.0, "]"], [key.KEY_BACKSLASH, 57.0, "\\"], [key.KEY_INSERT, 45.0, "INS"]
   ]],
   [38.0, [
         [key.KEY_CAPS_LOCK, 68.0, "CAPS"], [key.KEY_A, 45.0, "A"], [key.KEY_S, 45.0, "S"],
         [key.KEY_D, 45.0, "D"], [key.KEY_F, 45.0, "F"], [key.KEY_G, 45.0, "G"],
         [key.KEY_H, 45.0, "H"], [key.KEY_J, 45.0, "J"], [key.KEY_K, 45.0, "K"],
         [key.KEY_L, 45.0, "L"], [key.KEY_SEMICOLON, 45.0, ";"], [key.KEY_APOSTROPHE, 45.0, "'"],
         [key.KEY_ENTER, 88.0, "ENTER"], [key.KEY_PAGE_UP, 45.0, "PGUP"]
   ]],
   [38.0, [
         [key.KEY_LEFT_SHIFT, 80.0, "LSHIFT"], [key.KEY_Z, 45.0, "Z"], [key.KEY_X, 45.0, "X"],
         [key.KEY_C, 45.0, "C"], [key.KEY_V, 45.0, "V"], [key.KEY_B, 45.0, "B"],
         [key.KEY_N, 45.0, "N"], [key.KEY_M, 45.0, "M"], [key.KEY_COMMA, 45.0, ","],
         [key.KEY_PERIOD, 45.0, "."], [key.KEY_SLASH, 45.0, "/"], [key.KEY_RIGHT_SHIFT, 76.0, "RSHIFT"],
         [key.KEY_UP, 45.0, "UP"], [key.KEY_PAGE_DOWN, 45.0, "PGDN"]
   ]],
   [38.0, [
         [key.KEY_LEFT_CONTROL, 80.0, "LCTRL"], [key.KEY_LEFT_SUPER, 45.0, "WIN"], [key.KEY_LEFT_ALT, 45.0, "LALT"],
         [key.KEY_SPACE, 208.0, "SPACE"], [key.KEY_RIGHT_ALT, 45.0, "ALTGR"], [key.KEY_MENU, 45.0, "MENU"],
         [key.KEY_NULL, 45.0, "FN"], [key.KEY_RIGHT_CONTROL, 60.0, "RCTRL"], [key.KEY_LEFT, 45.0, "LEFT"],
         [key.KEY_DOWN, 45.0, "DOWN"], [key.KEY_RIGHT, 45.0, "RIGHT"]
   ]]
]

fn row_width(any row) f64 {
   "Returns total width for one keyboard row."
   def keys = row.get(1)
   mut out = 0.0
   mut i = 0
   while i < keys.len {
      if i > 0 { out += KEY_GAP }
      out += float(keys.get(i).get(1))
      i += 1
   }
   out
}

fn keyboard_width() f64 {
   "Returns the reference keyboard layout width."
   KEYBOARD_BASE_W
}

fn keyboard_height() f64 {
   "Returns the reference keyboard layout height."
   KEYBOARD_BASE_H
}

fn is_viewer_menu_hotkey(int code) bool {
   "Returns whether a key is reserved for viewer menus."
   case code {
      key.KEY_F1, key.KEY_F3, key.KEY_F4, key.KEY_F5, key.KEY_F6, key.KEY_F7 -> true
      _ -> false
   }
}

fn keyboard_layout(f64 sw, f64 sh, f64 pad, f64 header_h=86.0) dict {
   "Computes a fitted keyboard board rectangle."
   def avail_w = max(100.0, sw - pad * 2.0)
   def avail_h = max(100.0, sh - pad * 2.0 - header_h)
   def scale = min(avail_w / KEYBOARD_BASE_W, avail_h / KEYBOARD_BASE_H)
   {
      "board_w": KEYBOARD_BASE_W, "board_h": KEYBOARD_BASE_H, "avail_w": avail_w, "avail_h": avail_h, "scale": scale,
      "x": pad + (avail_w - KEYBOARD_BASE_W * scale) * 0.5,
      "y": pad + header_h + (avail_h - KEYBOARD_BASE_H * scale) * 0.5
   }
}

fn draw_status(any font_title, any font_body, f64 sw, f64 sh, f64 pad, any last_label, any last_code, f64 last_timer, int fps) int {
   "Draws keyboard testbed title and current key status."
   def title_y = pad + 4.0
   def hint_y = pad + 42.0
   gfx.draw_text(font_title, "Keyboard Testbed", pad, title_y, widgets.C_TEXT)
   gfx.draw_text(font_body, "US layout key-state map. Escape is not the exit key.", pad, hint_y, widgets.C_MUTED)
   def last_col = last_timer > 0.0 ? widgets.C_KEY_DOWN : widgets.C_MUTED
   gfx.draw_text(font_body, "last: " + to_str(last_label) + " (" + to_str(last_code) + ")", max(pad, sw - 230.0), hint_y, last_col)
   gfx.draw_text(font_body, "fps " + to_str(fps), max(pad, sw - 96.0), title_y, widgets.C_MUTED)
   0
}

fn draw_keycap(any win, any font_key, any item, f64 x, f64 y, f64 w, f64 h, f64 mx, f64 my) int {
   "Draws one keycap with hover and pressed states."
   def code = int(item.get(0))
   def label = to_str(item.get(2))
   def enabled = code != key.KEY_NULL
   def down = enabled && window.key_down(win, code)
   def hover = enabled && widgets.hit(mx, my, x, y, w, h)
   def fill = down ? widgets.C_KEY_DOWN : (hover ? widgets.C_KEY_HOVER : widgets.C_KEY_IDLE)
   def border = down ? widgets.C_KEY_DOWN : (hover ? widgets.C_KEY_DOWN : widgets.C_LINE)
   gfx.draw_rect(x, y, w, h, fill)
   gfx.draw_rect_lines(x, y, w, h, border, hover ? 3.0 : 2.0)
   gfx.draw_text(font_key, label, x + 6.0, y + 6.0, down ? gfx.WHITE : (enabled ? widgets.C_TEXT : widgets.C_MUTED))
   0
}

fn draw_board_at(any win, any font_key, f64 sw, f64 sh, f64 pad, f64 mx, f64 my) int {
   "Draws the fitted keyboard board at a supplied mouse position."
   def geom = keyboard_layout(sw, sh, pad)
   def scale = float(geom.get("scale", 1.0))
   def start_x = float(geom.get("x", pad))
   mut y = float(geom.get("y", pad + 86.0))
   mut r = 0
   while r < ROWS.len {
      def row = ROWS.get(r)
      def row_h = float(row.get(0)) * scale
      def keys = row.get(1)
      mut x = start_x
      mut i = 0
      while i < keys.len {
         def item = keys.get(i)
         def kw = float(item.get(1)) * scale
         draw_keycap(win, font_key, item, x, y, kw, row_h, mx, my)
         x += kw + KEY_GAP * scale
         i += 1
      }
      y += row_h + KEY_GAP * scale
      r += 1
   }
   0
}

fn draw_board(any win, any font_key, f64 sw, f64 sh, f64 pad) int {
   "Draws the keyboard board using the live mouse position."
   def mouse = window.mouse_pos(win)
   draw_board_at(win, font_key, sw, sh, pad, float(mouse.get(0, 0.0)), float(mouse.get(1, 0.0)))
}

fn scan_pressed(any win) any {
   "Returns the first newly pressed key in the keyboard layout."
   mut r = 0
   while r < ROWS.len {
      def row = ROWS.get(r)
      def keys = row.get(1)
      mut i = 0
      while i < keys.len {
         def item = keys.get(i)
         def code = int(item.get(0))
         if code != key.KEY_NULL && window.key_pressed(win, code) {
            return {"code": code, "label": item.get(2)}
         }
         i += 1
      }
      r += 1
   }
   0
}

#main {
   assert(ROWS.len == 6 && row_width(ROWS.get(0)) == KEYBOARD_BASE_W && keyboard_height() == KEYBOARD_BASE_H, "keyboard layout")
   def l = keyboard_layout(960.0, 540.0, 24.0)
   assert(float(l.get("scale", 0.0)) > 0.0, "keyboard fit")
   print("✓ std.os.ui.render.viewer.keyboard self-test passed")
}
