;; Keywords: viewer window layout viewport frame os ui render
;; Viewer window lifecycle and viewport sizing helpers.
;; References:
;; - std.os.ui.window
;; - std.os.ui.render.viewer.runtime
module std.os.ui.render.viewer.window(
   FLAG_CELLS,
   rect_x, rect_y, rect_w, rect_h, monitor_name, monitor_row, desktop_bounds,
   extend_bounds, desktop_bounds_with_window, desktop_map, map_desktop_rect,
   metric, meter, cursor_scope, draw_window_flags, draw_dpi_info_text, draw_dpi_info, draw_monitor_info, draw_window_marker
)

use std.core
use std.core.str (to_hex)
use std.math (clamp, max, min)
use std.os.ui.render
use std.os.ui.window
use std.os.ui.render.viewer.widgets

def FLAG_CELLS = [["F fullscreen", "fullscreen"], ["B borderless", "borderless"], ["R fixed", "no_resize"], ["C no border", "no_border"], ["M maximize", "maximized"], ["T topmost", "floating"], ["H hide", "hidden"], ["N minimize", "minimized"], ["V vsync", "vsync"]]

fn rect_x(list r) int { int(r.get(0, 0)) }

fn rect_y(list r) int { int(r.get(1, 0)) }

fn rect_w(list r) int { max(1, int(r.get(2, 1))) }

fn rect_h(list r) int { max(1, int(r.get(3, 1))) }

fn monitor_name(any mon, int idx) str {
   "Returns a monitor name with a stable fallback label."
   def name = window.get_monitor_name(mon)
   if name.len > 0 { return name }
   "Monitor " + to_str(idx)
}

fn monitor_row(any mon, int idx) dict {
   "Builds display data for one monitor tile."
   def rect = window.get_monitor_rect(mon)
   def phys = window.get_monitor_physical_size(mon)
   def scale_xy = window.get_monitor_content_scale(mon)
   def res = window.get_monitor_resolution(mon)
   {
      "mon": mon,
      "rect": rect,
      "name": "[" + to_str(idx) + "] " + monitor_name(mon, idx),
      "info": to_str(int(res.get(0, rect_w(rect)))) + " x " + to_str(int(res.get(1, rect_h(rect)))) +
      "  " + to_str(int(res.get(2, 0))) + "hz",
      "info2": to_str(int(phys.get(0, mon.get("width_mm", 0)))) + "mm x " +
      to_str(int(phys.get(1, mon.get("height_mm", 0)))) + "mm",
      "info3": "pos " + to_str(rect_x(rect)) + ", " + to_str(rect_y(rect)) +
      "  scale " + to_str(scale_xy.get(0, 1.0)) + "x"
   }
}

fn desktop_bounds(list rows) list {
   "Returns [min_x, min_y, max_x, max_y] for monitor rows."
   if rows.len == 0 { return [0, 0, 1, 1] }
   def first = rows.get(0, {}).get("rect", [0, 0, 1, 1])
   mut min_x = rect_x(first)
   mut min_y = rect_y(first)
   mut max_x = min_x + rect_w(first)
   mut max_y = min_y + rect_h(first)
   mut i = 1
   while i < rows.len {
      def r = rows.get(i, {}).get("rect", [0, 0, 1, 1])
      def x = rect_x(r)
      def y = rect_y(r)
      def w = rect_w(r)
      def h = rect_h(r)
      if x < min_x { min_x = x }
      if y < min_y { min_y = y }
      if x + w > max_x { max_x = x + w }
      if y + h > max_y { max_y = y + h }
      i += 1
   }
   [min_x, min_y, max_x, max_y]
}

fn extend_bounds(list bounds, int x, int y, int w, int h) list {
   "Extends desktop bounds with one rectangle."
   def min_x = min(int(bounds.get(0, x)), x)
   def min_y = min(int(bounds.get(1, y)), y)
   def max_x = max(int(bounds.get(2, x + w)), x + max(1, w))
   def max_y = max(int(bounds.get(3, y + h)), y + max(1, h))
   [min_x, min_y, max_x, max_y]
}

fn desktop_bounds_with_window(list rows, list pos, list size) list {
   "Returns desktop bounds including the current window rectangle."
   extend_bounds(desktop_bounds(rows),
      int(pos.get(0, 0)),
      int(pos.get(1, 0)),
      max(1, int(size.get(0, 1))),
   max(1, int(size.get(1, 1))))
}

fn desktop_map(list bounds, f64 x, f64 y, f64 w, f64 h, f64 margin=52.0) dict {
   "Builds a scale/offset map from desktop space to UI space."
   def min_x = int(bounds.get(0, 0))
   def min_y = int(bounds.get(1, 0))
   def desktop_w = max(1.0, float(int(bounds.get(2, 1)) - min_x))
   def desktop_h = max(1.0, float(int(bounds.get(3, 1)) - min_y))
   def inner_w = max(1.0, w - margin)
   def inner_h = max(1.0, h - margin)
   def scale = max(0.001, min(inner_w / desktop_w, inner_h / desktop_h))
   {
      "min_x": min_x, "min_y": min_y, "desktop_w": desktop_w, "desktop_h": desktop_h,
      "scale": scale,
      "x": x + (w - desktop_w * scale) * 0.5,
      "y": y + (h - desktop_h * scale) * 0.5,
   }
}

fn map_desktop_rect(dict map, list rect, f64 min_w=1.0, f64 min_h=1.0) list {
   "Maps one desktop-space rectangle through a desktop map."
   def scale = float(map.get("scale", 1.0))
   [
      float(map.get("x", 0.0)) + float(rect_x(rect) - int(map.get("min_x", 0))) * scale,
      float(map.get("y", 0.0)) + float(rect_y(rect) - int(map.get("min_y", 0))) * scale,
      max(min_w, float(rect_w(rect)) * scale),
      max(min_h, float(rect_h(rect)) * scale),
   ]
}

fn metric(any font_sm, str label, str value, f64 x, f64 y, f64 w) int {
   "Draws a label/value metric row."
   render.draw_text(font_sm, label, x, y, widgets.C_MUTED)
   def val_x = x + min(max(118.0, w * 0.55), max(118.0, w - 112.0))
   render.draw_text(font_sm, value, val_x, y, widgets.C_TEXT)
   0
}

fn meter(any font_sm, str label, any value, f64 x, f64 y, f64 w, any color) int {
   "Draws a clamped 0..1 meter row."
   def v = clamp(float(value), 0.0, 1.0)
   render.draw_text(font_sm, label, x, y, widgets.C_MUTED)
   def bar_x = x + 70.0
   def bar_w = max(24.0, w - 70.0)
   render.draw_rect(bar_x, y + 5.0, bar_w, 11.0, render.color_alpha(widgets.C_DIM, 0.45))
   render.draw_rect(bar_x, y + 5.0, bar_w * v, 11.0, color)
   render.draw_rectangle_lines(bar_x, y + 5.0, bar_w, 11.0, widgets.C_LINE, 1.0)
   0
}

fn cursor_scope(any font_sm, str title, str idle_label, str down_label, f64 x, f64 y, f64 w, f64 h, f64 mx, f64 my, f64 sx, f64 sy, bool down) int {
   "Draws cursor position and scaled-position diagnostics."
   widgets.panel(font_sm, x, y, w, h, title, widgets.C_ACCENT)
   def cx = clamp(mx, x + 12.0, x + w - 12.0)
   def cy = clamp(my, y + 12.0, y + h - 12.0)
   def scx = clamp(sx, x + 12.0, x + w - 12.0)
   def scy = clamp(sy, y + 12.0, y + h - 12.0)
   render.draw_line_2d(x + 18.0, y + h * 0.5, x + w - 18.0, y + h * 0.5, render.color_alpha(widgets.C_LINE, 0.70), 1.0)
   render.draw_line_2d(x + w * 0.5, y + 18.0, x + w * 0.5, y + h - 18.0, render.color_alpha(widgets.C_LINE, 0.70), 1.0)
   render.draw_line_2d(scx, scy, cx, cy, render.color_alpha(widgets.C_ACCENT, 0.70), 2.0)
   widgets.sphere(scx, scy, down ? 13.0 : 9.0, down ? widgets.C_ACCENT_HI : widgets.C_ACCENT, down)
   render.draw_circle_lines(cx, cy, down ? 28.0 : 20.0, render.color_alpha(widgets.C_ACCENT_HI, down ? 0.90 : 0.45), 2.0)
   render.draw_text(font_sm, down ? down_label : idle_label, x + w - 122.0, y + 16.0, down ? widgets.C_ACCENT_HI : widgets.C_MUTED)
   0
}

fn _draw_flag_cell(any font_sm, f64 x, f64 y, f64 w, str label, bool active) int {
   def fill = active ? render.color_alpha(widgets.C_ACCENT, 0.55) : render.color_alpha(widgets.C_PANEL_ALT, 0.70)
   def border = active ? widgets.C_ACCENT_HI : widgets.C_LINE
   render.draw_rect(x, y, w, 24.0, fill)
   render.draw_rectangle_lines(x, y, w, 24.0, border, active ? 2.0 : 1.0)
   render.draw_text(font_sm, label, x + 8.0, y + 4.0, active ? widgets.C_TEXT : widgets.C_MUTED)
   widgets.text_right(font_sm, active ? "on" : "off", x + w - 8.0, y + 4.0, active ? widgets.C_ACCENT_HI : widgets.C_MUTED)
   0
}

fn draw_window_flags(any font_sm, f64 x, f64 y, f64 w, dict state) int {
   "Draws the compact window flag grid."
   render.draw_rect(x - 10.0, y - 10.0, w + 20.0, 176.0, render.color_alpha(widgets.C_PANEL, 0.88))
   render.draw_rectangle_lines(x - 10.0, y - 10.0, w + 20.0, 176.0, widgets.C_LINE, 1.0)
   render.draw_text(font_sm, "FLAGS  F/B/R/C/H/N/M/T/V", x, y, widgets.C_MUTED)
   def gap = 6.0
   def cw = (w - gap) * 0.5
   def cy = y + 22.0
   mut i = 0
   while i < FLAG_CELLS.len {
      def row = FLAG_CELLS.get(i)
      def col = i % 2
      _draw_flag_cell(font_sm, x + float(col) * (cw + gap), cy + float(int(i / 2)) * 29.0, cw, row.get(0, ""), state.get(row.get(1, ""), false))
      i += 1
   }
   render.draw_text(font_sm, "0x" + to_hex(int(state.get("flags", 0))), x + cw + gap + 8.0, cy + 120.0, widgets.C_MUTED)
   0
}

fn draw_dpi_info_text(any font_sm, f64 x, f64 y, f64 w, f64 h, str line1, str line2, dict state) int {
   "Draws a DPI/framebuffer information panel."
   render.draw_rect(x, y, w, h, render.color_alpha(widgets.C_PANEL, 0.88))
   render.draw_rectangle_lines(x, y, w, h, widgets.C_LINE, 1.0)
   render.draw_text(font_sm, "DPI / framebuffer", x + 12.0, y + 10.0, widgets.C_MUTED)
   render.draw_text(font_sm, line1, x + 12.0, y + 31.0, widgets.C_TEXT)
   render.draw_text(font_sm, line2, x + 12.0, y + 52.0, widgets.C_MUTED)
   widgets.text_right(font_sm, state.get("borderless", false) ? "borderless" : (state.get("fullscreen", false) ? "fullscreen" : "windowed"), x + w - 12.0, y + 10.0, widgets.C_ACCENT)
   0
}

fn draw_dpi_info(any font_sm, f64 x, f64 y, f64 w, f64 h, list ws, list fb, list scale, list mouse, f64 mx, f64 my, dict state) int {
   "Formats and draws live DPI/window/mouse metrics."
   def line1 = "window " + to_str(int(ws.get(0, 0))) + "x" + to_str(int(ws.get(1, 0))) +
   "   fb " + to_str(int(fb.get(0, 0))) + "x" + to_str(int(fb.get(1, 0)))
   def line2 = "scale " + to_str(scale.get(0, 1.0)) + "x" + to_str(scale.get(1, 1.0)) +
   "   mouse " + to_str(int(mouse.get(0, 0))) + "," + to_str(int(mouse.get(1, 0))) +
   " -> " + to_str(int(mx)) + "," + to_str(int(my))
   draw_dpi_info_text(font_sm, x, y, w, h, line1, line2, state)
}

fn draw_monitor_info(any font_body, any font_sm, any row, f64 x, f64 y, f64 w, f64 h, bool active, f64 scale) int {
   "Draws one monitor tile in the desktop overview."
   def fill = active ? render.color_alpha(widgets.C_ACCENT, 0.24) : widgets.C_PANEL
   def border = active ? widgets.C_ACCENT : widgets.C_LINE
   render.draw_rect(x, y, w, h, fill)
   render.draw_rectangle_lines(x, y, w, h, border, active ? 4.0 : 2.0)
   def label_size = clamp(scale * 92.0, 14.0, 26.0)
   def info_size = clamp(scale * 62.0, 10.0, 16.0)
   render.draw_text(font_body, row.get("name", ""), x + 12.0, y + max(12.0, h * 0.18), active ? widgets.C_TEXT : render.color_alpha(widgets.C_TEXT, 0.82))
   if h > 92.0 && w > 180.0 {
      render.draw_text(font_sm, row.get("info", ""), x + 12.0, y + max(42.0, h * 0.18 + label_size), widgets.C_MUTED)
      render.draw_text(font_sm, row.get("info2", ""), x + 12.0, y + max(62.0, h * 0.18 + label_size + info_size), widgets.C_MUTED)
      render.draw_text(font_sm, row.get("info3", ""), x + 12.0, y + max(82.0, h * 0.18 + label_size + info_size * 2.0), widgets.C_MUTED)
   }
   0
}

fn draw_window_marker(any font_sm, list rect, str label="window") int {
   "Draws the current window rectangle over a monitor map."
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 3.0))
   def h = float(rect.get(3, 3.0))
   render.draw_rect(x, y, w, h, render.color_alpha(widgets.C_ACCENT_HI, 0.42))
   render.draw_rectangle_lines(x, y, w, h, widgets.C_ACCENT_HI, 2.0)
   if w > 54.0 && h > 24.0 { render.draw_text(font_sm, label, x + 8.0, y + 8.0, widgets.C_TEXT) }
   0
}

#main {
   assert(widgets.hit(5.0, 5.0, 0.0, 0.0, 10.0, 10.0), "window view uses widgets")
   assert(rect_w([0, 0, 3, 4]) == 3 && desktop_bounds([{"rect": [0, 0, 10, 10]}, {"rect": [-5, 2, 3, 4]}]).get(0) == -5, "window monitor helpers")
   def bounds = desktop_bounds_with_window([{"rect": [0, 0, 10, 10]}], [-20, 2], [4, 8])
   def mapped = map_desktop_rect(desktop_map(bounds, 0.0, 0.0, 100.0, 80.0), [-20, 2, 4, 8])
   assert(int(bounds.get(0, 0)) == -20 && float(mapped.get(2, 0.0)) >= 1.0, "window desktop map helpers")
   print("✓ std.os.ui.render.viewer.window self-test passed")
}
