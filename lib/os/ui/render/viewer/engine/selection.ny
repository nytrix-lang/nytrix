;; Keywords: engine selection gizmo picking transform model os ui render viewer scene
;; Selection and transform target helpers for engine gizmos and model editing.
;; References:
;; - std.os.ui.render.viewer.engine.state
;; - std.os.ui.render.viewer.gizmo
module std.os.ui.render.viewer.engine.selection(selection_bounds_valid, selection_bounds_from_part_bounds, selection_bounds_from_parts, selection_bounds_from_gpu_parts, selection_bounds_from_scene, selection_gizmo_label, selection_gizmo_mode, selection_rect, selection_rect_contains, selection_overlay_rects, selection_overlay_hit_test)
use std.core
use std.math
use std.core.str as str

fn selection_bounds_valid(any v) bool {
   "Returns whether a value can be used as a 3D bounds vector."
   is_list(v) && v.len >= 3
}

fn selection_gizmo_label(any mode) str {
   "Returns the canonical label for a transform gizmo mode."
   case int(mode){
      1 -> "rotate"
      2 -> "scale"
      _ -> "move"
   }
}

fn selection_gizmo_mode(any mode, any fallback=0) int {
   "Normalizes transform gizmo mode text or numeric aliases to move=0, rotate=1, scale=2."
   if is_int(mode) {
      def m = int(mode)
      return(m >= 0 && m <= 2) ? m : int(fallback)
   }
   if !is_str(mode) { return int(fallback) }
   def s = str.lower(str.strip(mode))
   case s {
      "" -> int(fallback)
      "1", "rotate", "rot", "r" -> 1
      "2", "scale", "s" -> 2
      "0", "move", "translate", "g", "w" -> 0
      _ -> int(fallback)
   }
}

fn selection_rect(any x=0.0, any y=0.0, any w=0.0, any h=0.0) list {
   "Returns a UI rectangle as `[x, y, w, h]`."
   [float(x), float(y), float(w), float(h)]
}

fn selection_rect_contains(any rect, any x, any y) bool {
   "Returns true when point `(x, y)` lies inside a `[x, y, w, h]` UI rect."
   if !is_list(rect) || rect.len < 4 { return false }
   def rx, ry = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def rw, rh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   if rw <= 0.0 || rh <= 0.0 { return false }
   def px, py = float(x), float(y)
   px >= rx && py >= ry && px <= rx + rw && py <= ry + rh
}

fn selection_overlay_rects() list {
   "Returns empty overlay rectangles: panel, toolbar, move, rotate, scale."
   [selection_rect(), selection_rect(), selection_rect(), selection_rect(), selection_rect()]
}

fn selection_overlay_hit_test(any x, any y, any panel_rect, any toolbar_rect, any move_rect, any rotate_rect, any scale_rect) int {
   "Hit-tests selection overlay controls. Returns -1 none, 0 chrome, 1 move, 2 rotate, 3 scale."
   if selection_rect_contains(move_rect, x, y) { return 1 }
   if selection_rect_contains(rotate_rect, x, y) { return 2 }
   if selection_rect_contains(scale_rect, x, y) { return 3 }
   if selection_rect_contains(toolbar_rect, x, y) || selection_rect_contains(panel_rect, x, y) { return 0 }
   -1
}

fn _selection_bounds_ready(any bounds) bool {
   is_list(bounds) && bounds.len >= 6 && float(bounds.get(0, 0.0)) <= float(bounds.get(3, 0.0))
}

fn _selection_bounds_span(any bounds) f64 {
   if !is_list(bounds) || bounds.len < 6 { return 0.0 }
   def sx = abs(float(bounds.get(3, 0.0)) - float(bounds.get(0, 0.0)))
   def sy = abs(float(bounds.get(4, 0.0)) - float(bounds.get(1, 0.0)))
   def sz = abs(float(bounds.get(5, 0.0)) - float(bounds.get(2, 0.0)))
   max(sx, max(sy, sz))
}

fn _selection_choose_tighter_bounds(any a, any b) list {
   if !_selection_bounds_ready(a) { return _selection_bounds_ready(b) ? b : [] }
   if !_selection_bounds_ready(b) { return a }
   def aspan = _selection_bounds_span(a)
   def bspan = _selection_bounds_span(b)
   if bspan > 0.000001 && (aspan <= 0.000001 || bspan < aspan) { return b }
   a
}

fn _selection_bounds_from_minmax(any bmin, any bmax) list {
   if !selection_bounds_valid(bmin) || !selection_bounds_valid(bmax) { return [] }
   [
      float(bmin.get(0, 0.0)), float(bmin.get(1, 0.0)), float(bmin.get(2, 0.0)),
      float(bmax.get(0, 0.0)), float(bmax.get(1, 0.0)), float(bmax.get(2, 0.0))
   ]
}

fn _selection_bounds_fit(any bounds, any scale, any x, any y, any z) list {
   if !is_list(bounds) || bounds.len < 6 { return [] }
   def sc = float(scale)
   def tx, ty = float(x), float(y)
   def tz = float(z)
   [
      float(bounds.get(0, 0.0)) * sc + tx, float(bounds.get(1, 0.0)) * sc + ty, float(bounds.get(2, 0.0)) * sc + tz,
      float(bounds.get(3, 0.0)) * sc + tx, float(bounds.get(4, 0.0)) * sc + ty, float(bounds.get(5, 0.0)) * sc + tz
   ]
}

fn _model_data_off(any m) int {
   if !is_list(m) { return -1 }
   def n = m.len
   if n >= 18 && int(m.get(0, 0)) == 4 && int(m.get(1, 0)) == 4 { return 2 }
   if n >= 16 { return 0 }
   -1
}

fn _transform_bound_point(any m, any x, any y, any z) list {
   def off = _model_data_off(m)
   if off < 0 { return [float(x), float(y), float(z)] }
   [
      float(x) * float(m.get(off + 0, 1.0)) + float(y) * float(m.get(off + 4, 0.0)) + float(z) * float(m.get(off + 8, 0.0)) + float(m.get(off + 12, 0.0)),
      float(x) * float(m.get(off + 1, 0.0)) + float(y) * float(m.get(off + 5, 1.0)) + float(z) * float(m.get(off + 9, 0.0)) + float(m.get(off + 13, 0.0)),
      float(x) * float(m.get(off + 2, 0.0)) + float(y) * float(m.get(off + 6, 0.0)) + float(z) * float(m.get(off + 10, 1.0)) + float(m.get(off + 14, 0.0))
   ]
}

fn selection_bounds_from_part_bounds(any pmin, any pmax, any model) list {
   "Transforms one part min/max pair into world-space bounds."
   if !selection_bounds_valid(pmin) || !selection_bounds_valid(pmax) { return [] }
   def x1 = float(pmin.get(0, 0.0)) def y1 = float(pmin.get(1, 0.0)) def z1 = float(pmin.get(2, 0.0))
   def x2 = float(pmax.get(0, 0.0)) def y2 = float(pmax.get(1, 0.0)) def z2 = float(pmax.get(2, 0.0))
   mut mnx, mny, mnz = 1e9, 1e9, 1e9
   mut mxx, mxy, mxz = -1e9, -1e9, -1e9
   mut i = 0
   while i < 8 {
      def px, py = (i & 1) ? x2 : x1, (i & 2) ? y2 : y1
      def pz = (i & 4) ? z2 : z1
      def p = _transform_bound_point(model, px, py, pz)
      def wx = float(p.get(0, 0.0))
      def wy = float(p.get(1, 0.0))
      def wz = float(p.get(2, 0.0))
      if wx < mnx { mnx = wx } if wx > mxx { mxx = wx }
      if wy < mny { mny = wy } if wy > mxy { mxy = wy }
      if wz < mnz { mnz = wz } if wz > mxz { mxz = wz }
      i += 1
   }
   [mnx, mny, mnz, mxx, mxy, mxz]
}

fn _accum_bounds(any cur, any part_bounds) list {
   if !is_list(part_bounds) || part_bounds.len < 6 { return cur }
   mut out = cur
   if !is_list(out) || out.len < 6 { out = [1e9, 1e9, 1e9, -1e9, -1e9, -1e9] }
   def mnx, mny = float(part_bounds.get(0, 0.0)), float(part_bounds.get(1, 0.0))
   def mnz = float(part_bounds.get(2, 0.0))
   def mxx = float(part_bounds.get(3, 0.0))
   def mxy = float(part_bounds.get(4, 0.0))
   def mxz = float(part_bounds.get(5, 0.0))
   if mnx < float(out.get(0, 0.0)) { out[0] = mnx }
   if mny < float(out.get(1, 0.0)) { out[1] = mny }
   if mnz < float(out.get(2, 0.0)) { out[2] = mnz }
   if mxx > float(out.get(3, 0.0)) { out[3] = mxx }
   if mxy > float(out.get(4, 0.0)) { out[4] = mxy }
   if mxz > float(out.get(5, 0.0)) { out[5] = mxz }
   out
}

fn selection_bounds_from_parts(any parts) list {
   "Combines visible CPU part bounds into one world-space selection box."
   if !is_list(parts) || parts.len == 0 { return [] }
   mut out = []
   mut i = 0
   while i < parts.len {
      def part = parts.get(i, 0)
      if is_dict(part) && part.get("visible", true) {
         mut pmin = part.get("min", 0)
         mut pmax = part.get("max", 0)
         if !selection_bounds_valid(pmin) || !selection_bounds_valid(pmax) {
            def mesh = part.get("mesh", 0)
            if is_dict(mesh) { pmin, pmax = mesh.get("min", pmin), mesh.get("max", pmax) }
         }
         out = _accum_bounds(out, selection_bounds_from_part_bounds(pmin, pmax, part.get("model", 0)))
      }
      i += 1
   }
   if !_selection_bounds_ready(out) { return [] }
   out
}

fn selection_bounds_from_gpu_parts(any parts) list {
   "Combines visible GPU part bounds into one world-space selection box."
   if !is_list(parts) || parts.len == 0 { return [] }
   mut out = []
   mut i = 0
   while i < parts.len {
      def rec = parts.get(i, 0)
      if is_list(rec) && rec.len >= 40 && int(rec.get(37, 1)) != 0 { out = _accum_bounds(out, selection_bounds_from_part_bounds(rec.get(38, 0), rec.get(39, 0), rec.get(10, 0))) }
      i += 1
   }
   if !_selection_bounds_ready(out) { return [] }
   out
}

fn _selection_bounds_add_offset(any bounds, any x, any y, any z) list {
   _selection_bounds_fit(bounds, 1.0, x, y, z)
}

fn _selection_scene_model_baked_for_draw(any scene_obj) bool {
   if !is_dict(scene_obj) { return false }
   if bool(scene_obj.get("parts_model_baked", false)) { return true }
   def gpu_state = scene_obj.get("gpu_draw_state", 0)
   if is_list(gpu_state) && gpu_state.len >= 2 && to_int(gpu_state.get(0, 0)) != 0 && int(gpu_state.get(1, 0)) > 0 {
      if gpu_state.len >= 9 { return int(gpu_state.get(5, 0)) != 0 }
      if gpu_state.len >= 7 { return int(gpu_state.get(4, 0)) != 0 }
   }
   if to_int(scene_obj.get("gpu_parts_slab", 0)) != 0 || int(scene_obj.get("gpu_parts_count", 0)) > 0 {
      return bool(scene_obj.get("gpu_model_baked", false))
   }
   bool(scene_obj.get("gpu_model_baked", false))
}

fn _selection_bounds_apply_scene_transform(any bounds, any scene_obj) list {
   if !is_list(bounds) || bounds.len < 6 { return [] }
   if !is_dict(scene_obj) { return bounds }
   if bool(scene_obj.get("fit_applied", false)) && _selection_scene_model_baked_for_draw(scene_obj) {
      return _selection_bounds_apply_edit(bounds, scene_obj)
   }
   def fit_bounds = _selection_bounds_fit(bounds,
      scene_obj.get("fit_scale", 1.0),
      scene_obj.get("fit_tx", 0.0),
      scene_obj.get("fit_ty", 0.0),
   scene_obj.get("fit_tz", 0.0))
   _selection_bounds_apply_edit(fit_bounds, scene_obj)
}

fn _selection_axis_scale(any scene_obj, str key, f64 fallback) f64 {
   if is_dict(scene_obj) && scene_obj.contains(key) { return max(0.02, float(scene_obj.get(key, fallback))) }
   max(0.02, fallback)
}

fn _selection_transform_edit_point(any x, any y, any z, any sx0, any sy0, any sz0, any rx, any ry, any rz, any tx, any ty, any tz) list {
   mut px = float(x) * float(sx0)
   mut py = float(y) * float(sy0)
   mut pz = float(z) * float(sz0)
   def sx, cx = sin(float(rx)), cos(float(rx))
   def yx = py * cx - pz * sx
   def zx = py * sx + pz * cx
   py, pz = yx, zx
   def sy, cy = sin(float(ry)), cos(float(ry))
   def xy = px * cy + pz * sy
   def zy = (0.0 - px) * sy + pz * cy
   px, pz = xy, zy
   def sz, cz = sin(float(rz)), cos(float(rz))
   def xz = px * cz - py * sz
   def yz = px * sz + py * cz
   [xz + float(tx), yz + float(ty), pz + float(tz)]
}

fn _selection_bounds_apply_edit(any bounds, any scene_obj) list {
   if !is_list(bounds) || bounds.len < 6 { return [] }
   if !is_dict(scene_obj) { return bounds }
   def tx = float(scene_obj.get("edit_tx", 0.0))
   def ty = float(scene_obj.get("edit_ty", 0.0))
   def tz = float(scene_obj.get("edit_tz", 0.0))
   def rx = float(scene_obj.get("edit_rx", 0.0))
   def ry = float(scene_obj.get("edit_ry", 0.0))
   def rz = float(scene_obj.get("edit_rz", 0.0))
   def sc = max(0.02, float(scene_obj.get("edit_scale", 1.0)))
   def sx0 = _selection_axis_scale(scene_obj, "edit_sx", sc)
   def sy0 = _selection_axis_scale(scene_obj, "edit_sy", sc)
   def sz0 = _selection_axis_scale(scene_obj, "edit_sz", sc)
   if abs(rx) <= 0.000001 && abs(ry) <= 0.000001 && abs(rz) <= 0.000001 &&
   abs(sx0 - 1.0) <= 0.000001 && abs(sy0 - 1.0) <= 0.000001 && abs(sz0 - 1.0) <= 0.000001{
      return _selection_bounds_add_offset(bounds, tx, ty, tz)
   }
   def x1 = float(bounds.get(0, 0.0)) def y1 = float(bounds.get(1, 0.0)) def z1 = float(bounds.get(2, 0.0))
   def x2 = float(bounds.get(3, 0.0)) def y2 = float(bounds.get(4, 0.0)) def z2 = float(bounds.get(5, 0.0))
   mut mnx, mny, mnz = 1e9, 1e9, 1e9
   mut mxx, mxy, mxz = -1e9, -1e9, -1e9
   mut i = 0
   while i < 8 {
      def p = _selection_transform_edit_point((i & 1) ? x2 : x1, (i & 2) ? y2 : y1, (i & 4) ? z2 : z1, sx0, sy0, sz0, rx, ry, rz, tx, ty, tz)
      def wx = float(p.get(0, 0.0))
      def wy = float(p.get(1, 0.0))
      def wz = float(p.get(2, 0.0))
      if wx < mnx { mnx = wx } if wx > mxx { mxx = wx }
      if wy < mny { mny = wy } if wy > mxy { mxy = wy }
      if wz < mnz { mnz = wz } if wz > mxz { mxz = wz }
      i += 1
   }
   [mnx, mny, mnz, mxx, mxy, mxz]
}

fn _selection_child_bounds(any scene_obj, int depth) list {
   if depth > 6 || !is_dict(scene_obj) { return [] }
   mut out = []
   def keys = ["children", "objects", "nodes", "items"]
   mut ki = 0
   while ki < keys.len {
      def arr = scene_obj.get(keys.get(ki), 0)
      if is_list(arr) {
         mut i = 0
         while i < arr.len {
            def child = arr.get(i, 0)
            if is_dict(child) { out = _accum_bounds(out, _selection_bounds_from_scene_inner(child, depth + 1)) }
            i += 1
         }
      }
      ki += 1
   }
   out
}

fn _selection_direct_bounds(any scene_obj) list {
   mut bmin = scene_obj.get("fit_world_min", 0)
   mut bmax = scene_obj.get("fit_world_max", 0)
   def has_fit_world = selection_bounds_valid(bmin) && selection_bounds_valid(bmax)
   def has_model_bounds = selection_bounds_valid(scene_obj.get("min", 0)) && selection_bounds_valid(scene_obj.get("max", 0))
   if !selection_bounds_valid(bmin) || !selection_bounds_valid(bmax) { bmin, bmax = scene_obj.get("min", 0), scene_obj.get("max", 0) }
   if selection_bounds_valid(bmin) && selection_bounds_valid(bmax) {
      def direct_bounds = _selection_bounds_from_minmax(bmin, bmax)
      if has_fit_world || !has_model_bounds { return _selection_bounds_apply_edit(direct_bounds, scene_obj) }
      def fit_bounds = _selection_bounds_fit(direct_bounds, scene_obj.get("fit_scale", 1.0), scene_obj.get("fit_tx", 0.0), scene_obj.get("fit_ty", 0.0), scene_obj.get("fit_tz", 0.0))
      return _selection_bounds_apply_edit(fit_bounds, scene_obj)
   }
   []
}

fn _selection_bounds_from_scene_inner(any scene_obj, int depth) list {
   if scene_obj == 0 || !is_dict(scene_obj) { return [] }
   mut render_bounds = []
   render_bounds = _accum_bounds(render_bounds, selection_bounds_from_gpu_parts(scene_obj.get("gpu_parts", 0)))
   render_bounds = _accum_bounds(render_bounds, selection_bounds_from_parts(scene_obj.get("parts", 0)))
   render_bounds = _accum_bounds(render_bounds, _selection_child_bounds(scene_obj, depth))
   if _selection_bounds_ready(render_bounds) { return _selection_bounds_apply_scene_transform(render_bounds, scene_obj) }
   _selection_direct_bounds(scene_obj)
}

fn selection_bounds_from_scene(any scene_obj) list {
   "Returns full transformed selection bounds for a scene object or hierarchy."
   _selection_bounds_from_scene_inner(scene_obj, 0)
}

#main {
   fn assert_bounds(list b, f64 x0, f64 y0, f64 z0, f64 x1, f64 y1, f64 z1, str label) any {
      assert(b.len == 6, label + " length")
      assert(float(b.get(0, 0.0)) == x0 && float(b.get(1, 0.0)) == y0 && float(b.get(2, 0.0)) == z0, label + " min")
      assert(float(b.get(3, 0.0)) == x1 && float(b.get(4, 0.0)) == y1 && float(b.get(5, 0.0)) == z1, label + " max")
   }
   assert(selection_gizmo_label(0) == "move" && selection_gizmo_label(1) == "rotate" && selection_gizmo_label(2) == "scale", "selection gizmo labels")
   assert(selection_gizmo_mode(" rot ") == 1 && selection_gizmo_mode("s") == 2 && selection_gizmo_mode("translate") == 0 && selection_gizmo_mode("bad", 2) == 2, "selection gizmo aliases")
   def rect = selection_rect(10, 20, 30, 40)
   assert(selection_rect_contains(rect, 25, 30) && !selection_rect_contains(rect, 45, 65), "selection rect hit test")
   def panel = selection_rect(0, 0, 200, 80)
   def toolbar = selection_rect(0, 0, 120, 30)
   def move = selection_rect(10, 10, 20, 20)
   def rotate = selection_rect(40, 10, 20, 20)
   def scale = selection_rect(70, 10, 20, 20)
   assert(selection_overlay_hit_test(15, 15, panel, toolbar, move, rotate, scale) == 1, "selection overlay move")
   assert(selection_overlay_hit_test(45, 15, panel, toolbar, move, rotate, scale) == 2, "selection overlay rotate")
   assert(selection_overlay_hit_test(75, 15, panel, toolbar, move, rotate, scale) == 3, "selection overlay scale")
   assert(selection_overlay_hit_test(150, 15, panel, toolbar, move, rotate, scale) == 0, "selection overlay panel")
   assert(selection_overlay_hit_test(250, 15, panel, toolbar, move, rotate, scale) == -1, "selection overlay miss")
   assert_bounds(selection_bounds_from_part_bounds([0, 0, 0], [1, 2, 3], 0), 0.0, 0.0, 0.0, 1.0, 2.0, 3.0, "selection raw bounds")
   def model = [
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      2, 3, 4, 1
   ]
   assert_bounds(selection_bounds_from_part_bounds([0, 0, 0], [1, 1, 1], model), 2.0, 3.0, 4.0, 3.0, 4.0, 5.0, "selection model bounds")
   def parts = [
      {"visible": true, "min": [0, 0, 0], "max": [1, 1, 1], "model": 0},
      {"visible": false, "min": [-10, -10, -10], "max": [10, 10, 10], "model": 0},
      {"visible": true, "mesh": {"min": [2, 3, 4], "max": [3, 4, 5]}, "model": 0}
   ]
   assert_bounds(selection_bounds_from_parts(parts), 0.0, 0.0, 0.0, 3.0, 4.0, 5.0, "selection parts bounds")
   assert_bounds(selection_bounds_from_scene({"min": [-10, -10, -10], "max": [10, 10, 10], "fit_scale": 2.0, "parts": [{"visible": true, "min": [1, 1, 1], "max": [2, 2, 2], "model": 0}]}), 2.0, 2.0, 2.0, 4.0, 4.0, 4.0, "selection rendered bounds prefer parts")
   assert_bounds(selection_bounds_from_scene({"min": [-10, -10, -10], "max": [10, 10, 10], "fit_applied": true, "parts_model_baked": true, "parts": [{"visible": true, "min": [1, 1, 1], "max": [2, 2, 2], "model": 0}]}), 1.0, 1.0, 1.0, 2.0, 2.0, 2.0, "selection baked parts no refit")
   assert_bounds(selection_bounds_from_scene({"min": [0, 0, 0], "max": [1, 1, 1], "edit_tx": 2.0, "edit_ty": 3.0, "edit_tz": 4.0}), 2.0, 3.0, 4.0, 3.0, 4.0, 5.0, "selection scene edit")
   assert_bounds(selection_bounds_from_scene({"min": [0, 0, 0], "max": [1, 1, 1], "edit_scale": 2.0}), 0.0, 0.0, 0.0, 2.0, 2.0, 2.0, "selection scene edit scale")
   assert_bounds(selection_bounds_from_scene({"min": [0, 0, 0], "max": [1, 1, 1], "fit_scale": 2.0, "fit_tx": 5.0, "fit_ty": 6.0, "fit_tz": 7.0}), 5.0, 6.0, 7.0, 7.0, 8.0, 9.0, "selection scene fit")
   assert_bounds(selection_bounds_from_scene({"children": [{"min": [-500, 0, -400], "max": [500, 80, 400]}, {"min": [600, 0, -10], "max": [620, 20, 10]}]}), -500.0, 0.0, -400.0, 620.0, 80.0, 400.0, "selection hierarchy sponza bounds")
   print("✓ std.os.ui.render.viewer.engine.selection self-test passed")
}
