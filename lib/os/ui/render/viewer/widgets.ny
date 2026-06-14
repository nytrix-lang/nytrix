;; Keywords: viewer widgets ui controls panels os render
;; Small reusable UI widgets for viewer panels, controls, and editor chrome.
;; References:
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.widgets(
   C_BG, C_PANEL, C_PANEL_ALT, C_BOX, C_LINE, C_TEXT, C_MUTED, C_DIM, C_ACCENT, C_ACCENT_HI,
   C_KEY_IDLE, C_KEY_HOVER, C_KEY_DOWN,
   text_w, text_h, text_right, text_center, hit, fits, preview, limit_text,
   panel, button, text_box, keycap, signal_chip, chip_w, sphere, axis
)

use std.core
use std.core.str as str
use std.math (clamp, max, min)
use std.os.ui.render as gfx

def C_BG = gfx.color_hex("#000000")
def C_PANEL = gfx.color_hex("#080808")
def C_PANEL_ALT = gfx.color_hex("#131318")
def C_BOX = C_PANEL_ALT
def C_LINE = gfx.color_hex("#2b2634")
def C_TEXT = gfx.color_hex("#f5f5f6")
def C_MUTED = gfx.color_hex("#c6c6ca")
def C_DIM = gfx.color_hex("#15151b")
def C_ACCENT = gfx.color_hex("#9f86d9")
def C_ACCENT_HI = gfx.color_hex("#bda9ec")
def C_KEY_IDLE = gfx.color_hex("#121218")
def C_KEY_HOVER = gfx.color_hex("#1b1624")
def C_KEY_DOWN = gfx.color_hex("#332347")

fn text_w(any font, any label, any font_lg=0, any font_md=0, f64 px=9.5) f64 {
   "Estimates text width for fixed-size UI layout."
   def char_w = font == font_lg ? 21.0 : (font == font_md ? 12.5 : px)
   float(to_str(label).len) * char_w
}

fn text_h(any font, any font_lg=0, any font_md=0) f64 {
   "Returns the expected line height for a loaded UI font."
   font == font_lg ? 38.0 : (font == font_md ? 24.0 : 18.0)
}

fn text_right(any font, any label, f64 right_x, f64 y, any color, any font_lg=0, any font_md=0) int {
   "Draws a label aligned to a right edge."
   gfx.draw_text(font, label, right_x - text_w(font, label, font_lg, font_md), y, color)
   0
}

fn text_center(any font, any label, f64 cx, f64 y, any color, any font_lg=0, any font_md=0) int {
   "Draws a label centered around an x coordinate."
   gfx.draw_text(font, label, cx - text_w(font, label, font_lg, font_md) * 0.5, y, color)
   0
}

fn hit(f64 px, f64 py, f64 x, f64 y, f64 w, f64 h) bool {
   "Tests whether a point is inside a rectangle."
   px >= x && px <= x + w && py >= y && py <= y + h
}

fn fits(f64 y, f64 bottom, f64 h) bool {
   "Tests whether a block fits before a bottom edge."
   y + h <= bottom
}

fn preview(str s, int limit) str {
   "Returns an ASCII preview clipped with an ellipsis."
   if s.len <= limit { return s }
   if limit <= 3 { return str.str_slice(s, 0, limit) }
   str.str_slice(s, 0, limit - 3) + "..."
}

fn limit_text(str s, int limit) str {
   "Returns a UTF-8 safe prefix limited by codepoint count."
   if str.utf8_len(s) <= limit { return s }
   str.utf8_slice(s, 0, limit)
}

fn panel(any font_sm, f64 x, f64 y, f64 w, f64 h, str title, any accent=C_ACCENT) int {
   "Draws a compact titled panel frame."
   gfx.draw_rect(x, y, w, h, C_PANEL)
   gfx.draw_rect(x + 1.0, y + 1.0, w - 2.0, h - 2.0, C_PANEL_ALT)
   gfx.draw_rect(x, y, w, 2.0, accent)
   gfx.draw_rectangle_lines(x, y, w, h, C_LINE, 1.0)
   gfx.draw_text(font_sm, title, x + 10.0, y + 13.0, C_MUTED)
   0
}

fn button(any font, str label, f64 x, f64 y, f64 w, f64 h, f64 mx, f64 my, bool click, any color=C_ACCENT) bool {
   "Draws a button and returns true on a clicked hover."
   def hover = hit(mx, my, x, y, w, h)
   def down = hover && click
   def fill = down ? C_KEY_DOWN : (hover ? C_KEY_HOVER : C_KEY_IDLE)
   def border = down ? C_ACCENT_HI : (hover ? color : C_LINE)
   gfx.draw_rect(x, y, w, h, fill)
   gfx.draw_rectangle_lines(x, y, w, h, border, down ? 2.0 : (hover ? 1.5 : 1.0))
   gfx.draw_text(font, label, x + w * 0.5 - text_w(font, label) * 0.5, y + max(6.0, h * 0.26), C_TEXT)
   hover && click
}

fn text_box(any font_body, any font_small, str label, str value, f64 x, f64 y, f64 w, f64 h, bool active=false, bool caret=false, int max_chars=0) int {
   "Draws a single-line text box with optional active caret."
   def shown = preview(value, max_chars > 0 ? max_chars : int(max(8.0, (w - 28.0) / 9.8)))
   gfx.draw_text(font_small, label, x, y - 17.0, C_MUTED)
   gfx.draw_rect(x, y, w, h, C_BOX)
   gfx.draw_rectangle_lines(x, y, w, h, active ? C_ACCENT : C_LINE, active ? 1.5 : 1.0)
   gfx.draw_text(font_body, shown, x + 10.0, y + max(6.0, h * 0.24), value.len > 0 ? C_TEXT : C_MUTED)
   if active && caret {
      def cx = min(x + w - 14.0, x + 12.0 + text_w(font_body, shown))
      gfx.draw_rect(cx, y + 8.0, 2.0, max(8.0, h - 16.0), C_ACCENT_HI)
   }
   0
}

fn keycap(any font_md, f64 x, f64 y, f64 size, str label, bool pressed, any color=C_ACCENT) int {
   "Draws one fixed-size keyboard key."
   def fill = pressed ? C_KEY_DOWN : C_KEY_IDLE
   def border = pressed ? C_ACCENT_HI : C_LINE
   gfx.draw_rect(x, y, size, size, fill)
   gfx.draw_rectangle_lines(x, y, size, size, border, pressed ? 2.0 : 1.0)
   text_center(font_md, label, x + size * 0.5, y + size * 0.5 - text_h(font_md, 0, font_md) * 0.5, C_TEXT, 0, font_md)
   0
}

fn chip_w(any font_sm, any label) f64 {
   "Returns a compact status chip width."
   max(52.0, text_w(font_sm, label) + 18.0)
}

fn signal_chip(any font_sm, f64 x, f64 y, str label, bool active, any color=C_ACCENT) f64 {
   "Draws a status chip and returns its width."
   def w = chip_w(font_sm, label)
   gfx.draw_rect(x, y, w, 24.0, active ? C_KEY_DOWN : gfx.color_alpha(C_DIM, 0.35))
   gfx.draw_rectangle_lines(x, y, w, 24.0, active ? C_ACCENT_HI : C_LINE, 1.0)
   text_center(font_sm, label, x + w * 0.5, y + 6.0, active ? C_TEXT : C_MUTED)
   w
}

fn sphere(f64 cx, f64 cy, f64 r, any color, bool active=false) int {
   "Draws a small lit circular control marker."
   def halo = active ? 0.14 : 0.08
   gfx.draw_circle(cx, cy, r + 14.0, gfx.color_alpha(color, halo))
   gfx.draw_circle(cx, cy, r, color)
   gfx.draw_circle(cx - r * 0.28, cy - r * 0.32, max(3.0, r * 0.28), gfx.color_alpha(C_TEXT, active ? 0.20 : 0.12))
   gfx.draw_circle_lines(cx, cy, r + 14.0, gfx.color_alpha(color, active ? 0.30 : 0.20), 1.5)
   0
}

fn axis(bool neg, bool pos) f64 {
   "Converts negative/positive button states to an axis value."
   mut out = 0.0
   if neg { out -= 1.0 }
   if pos { out += 1.0 }
   out
}

#main {
   assert(hit(5.0, 5.0, 0.0, 0.0, 10.0, 10.0) && !hit(12.0, 5.0, 0.0, 0.0, 10.0, 10.0), "widget hit")
   assert(fits(2.0, 10.0, 8.0) && !fits(3.0, 10.0, 8.0), "widget fit")
   assert(preview("abcdef", 4) == "a..." && limit_text("abc", 8) == "abc", "widget text")
   assert(axis(true, false) == -1.0 && axis(false, true) == 1.0, "widget axis")
   print("✓ viewer widgets test passed")
}
