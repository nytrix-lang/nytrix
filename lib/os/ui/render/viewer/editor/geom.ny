;; Keywords: editor geometry layout hitbox text os ui render viewer
;; Geometry and hitbox helpers for editor layout and text interactions.
;; References:
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.editor.geom(
   LINE_H, STATUS_H, editor_layout, visible_rows,
   with_bottom_dock, pane_at, focus_next,
   row_at, col_at, buffer_at, divider_hit
)

use std.core
use std.core.str as str
use std.math (max, min)
use std.os.ui.render as gfx

def LINE_H = 22.0
def STATUS_H = 30.0
def MIN_RAIL_W = 48.0
def MIN_EDIT_W = 56.0
def MIN_BODY_H = 36.0
def MIN_DOCK_H = 40.0
def MIN_EDIT_H = 28.0

fn editor_layout(f64 sw, f64 sh, f64 rail_w=250.0, f64 top_h=32.0) dict {
   "Computes pane rectangles for the editor shell."
   def pad = 0.0
   def chrome_pad = 6.0
   def status_h = STATUS_H
   def rw = rail_w <= 1.0 ? 0.0 : min(max(rail_w, MIN_RAIL_W), max(MIN_RAIL_W, sw - MIN_EDIT_W))
   def body_y = top_h
   def body_h = max(MIN_BODY_H, sh - top_h - status_h)
   {
      "pad": pad, "chrome_pad": chrome_pad, "top": top_h,
      "rail_x": 0.0, "rail_y": body_y, "rail_w": rw, "rail_h": body_h,
      "edit_x": rw, "edit_y": body_y, "edit_w": max(MIN_EDIT_W, sw - rw), "edit_h": body_h,
      "divider_x": rw,
      "status_y": sh - status_h, "status_h": status_h,
   }
}

fn with_bottom_dock(dict lay, f64 sh, bool open, f64 want_h=0.0) dict {
   "Reserves a bottom dock above the status bar and shrinks the editor pane."
   lay["dock_open"] = open
   if !open { return lay }
   def gap = 0.0
   def ey = float(lay.get("edit_y", 0.0))
   def status_y = float(lay.get("status_y", sh - STATUS_H))
   def avail = max(MIN_DOCK_H + MIN_EDIT_H, status_y - ey - gap)
   def th = min(max(want_h > 0.0 ? want_h : sh * 0.30, MIN_DOCK_H), max(MIN_DOCK_H, avail - MIN_EDIT_H))
   def ty = status_y - gap - th
   lay["dock_x"] = float(lay.get("edit_x", 0.0))
   lay["dock_y"] = ty
   lay["dock_w"] = float(lay.get("edit_w", 0.0))
   lay["dock_h"] = th
   lay["edit_h"] = max(MIN_EDIT_H, ty - ey)
   lay
}

fn pane_at(dict lay, f64 x, f64 y) str {
   if divider_hit(lay, x, y) { return "divider" }
   if bool(lay.get("dock_open", false)) &&
   x >= float(lay.get("dock_x", 0.0)) && x <= float(lay.get("dock_x", 0.0)) + float(lay.get("dock_w", 0.0)) &&
   y >= float(lay.get("dock_y", 0.0)) && y <= float(lay.get("dock_y", 0.0)) + float(lay.get("dock_h", 0.0)){ return "terminal" }
   if x >= float(lay.get("edit_x", 0.0)) && x <= float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) &&
   y >= float(lay.get("edit_y", 0.0)) && y <= float(lay.get("edit_y", 0.0)) + float(lay.get("edit_h", 0.0)){ return "editor" }
   if float(lay.get("rail_w", 0.0)) > 1.0 &&
   x >= float(lay.get("rail_x", 0.0)) && x <= float(lay.get("rail_x", 0.0)) + float(lay.get("rail_w", 0.0)) &&
   y >= float(lay.get("rail_y", 0.0)) && y <= float(lay.get("rail_y", 0.0)) + float(lay.get("rail_h", 0.0)){ return "project" }
   if y >= float(lay.get("status_y", 0.0)) && y <= float(lay.get("status_y", 0.0)) + float(lay.get("status_h", STATUS_H)) { return "status" }
   "top"
}

fn focus_next(str current, bool project_on=true, bool dock_on=false) str {
   if current == "project" { return "editor" }
   if current == "editor" { return dock_on ? "terminal" : (project_on ? "project" : "editor") }
   if current == "terminal" { return project_on ? "project" : "editor" }
   project_on ? "project" : "editor"
}

fn visible_rows(f64 edit_h) int {
   max(1, int((edit_h - 18.0) / LINE_H))
}

fn row_at(dict lay, f64 y, int scroll) int {
   int((y - float(lay.get("edit_y", 0.0)) - 14.0) / LINE_H) + scroll
}

fn col_at(any font, str line, f64 x, f64 text_x) int {
   def target = max(0.0, x - text_x)
   mut lo = 0
   mut hi = line.len
   while lo < hi {
      def mid = (lo + hi) / 2
      def w = float(gfx.measure_text_fast(font, str.str_slice(line, 0, mid + 1)).get(0, 0.0))
      if w >= target { hi = mid }
      else { lo = mid + 1 }
   }
   lo
}

fn buffer_at(dict lay, f64 x, f64 y, int count) int {
   if x < float(lay.get("rail_x", 0.0)) + 10.0 || x > float(lay.get("rail_x", 0.0)) + float(lay.get("rail_w", 0.0)) - 10.0 { return -1 }
   def rel = y - float(lay.get("rail_y", 0.0)) - 44.0
   if rel < 0.0 { return -1 }
   def idx = int(rel / 42.0)
   if idx < 0 || idx >= count { return -1 }
   def in_row = rel - float(idx) * 42.0
   in_row <= 34.0 ? idx : -1
}

fn divider_hit(dict lay, f64 x, f64 y) bool {
   if float(lay.get("rail_w", 0.0)) <= 1.0 { return false }
   def dx = float(lay.get("divider_x", 0.0))
   x >= dx - 7.0 && x <= dx + 7.0 &&
   y >= float(lay.get("rail_y", 0.0)) && y <= float(lay.get("rail_y", 0.0)) + float(lay.get("rail_h", 0.0))
}
