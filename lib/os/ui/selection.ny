;; Keywords: selection gizmo
;; Scene selection bounds, overlay drawing data, and gizmo state calculations.
module std.os.ui.selection(selection_bounds_valid, selection_bounds_from_part_bounds, selection_bounds_from_parts, selection_bounds_from_gpu_parts, selection_bounds_from_scene, selection_gizmo_label, selection_gizmo_mode, selection_rect, selection_rect_contains, selection_overlay_rects, selection_overlay_hit_test)
use std.core
use std.core.str as str

fn selection_bounds_valid(any: v): bool { is_list(v) && v.len >= 3 }

fn selection_gizmo_label(any: mode): str {
   "Returns the canonical label for a transform gizmo mode."
   case int(mode){
      1 -> "rotate"
      2 -> "scale"
      _ -> "move"
   }
}

fn selection_gizmo_mode(any: mode, any: fallback=0): int {
   "Normalizes transform gizmo mode text or numeric aliases to move=0, rotate=1, scale=2."
   if(is_int(mode)){
      def m = int(mode)
      return(m >= 0 && m <= 2) ? m : int(fallback)
   }
   if(!is_str(mode)){ return int(fallback) }
   def s = str.lower(str.strip(mode))
   case s {
      "" -> int(fallback)
      "1", "rotate", "rot", "r" -> 1
      "2", "scale", "s" -> 2
      "0", "move", "translate", "g", "w" -> 0
      _ -> int(fallback)
   }
}

fn selection_rect(any: x=0.0, any: y=0.0, any: w=0.0, any: h=0.0): list {
   "Returns a UI rectangle as `[x, y, w, h]`."
   [float(x), float(y), float(w), float(h)]
}

fn selection_rect_contains(any: rect, any: x, any: y): bool {
   "Returns true when point `(x, y)` lies inside a `[x, y, w, h]` UI rect."
   if(!is_list(rect) || rect.len < 4){ return false }
   def rx, ry = float(rect.get(0, 0.0)), float(rect.get(1, 0.0))
   def rw, rh = float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   if(rw <= 0.0 || rh <= 0.0){ return false }
   def px, py = float(x), float(y)
   px >= rx && py >= ry && px <= rx + rw && py <= ry + rh
}

fn selection_overlay_rects(): list {
   "Returns empty overlay rectangles: panel, toolbar, move, rotate, scale."
   [selection_rect(), selection_rect(), selection_rect(), selection_rect(), selection_rect()]
}

fn selection_overlay_hit_test(any: x, any: y, any: panel_rect, any: toolbar_rect, any: move_rect, any: rotate_rect, any: scale_rect): int {
   "Hit-tests selection overlay controls. Returns -1 none, 0 chrome, 1 move, 2 rotate, 3 scale."
   if(selection_rect_contains(move_rect, x, y)){ return 1 }
   if(selection_rect_contains(rotate_rect, x, y)){ return 2 }
   if(selection_rect_contains(scale_rect, x, y)){ return 3 }
   if(selection_rect_contains(toolbar_rect, x, y) || selection_rect_contains(panel_rect, x, y)){ return 0 }
   -1
}

fn _selection_bounds_ready(any: bounds): bool {
   is_list(bounds) && bounds.len >= 6 && float(bounds.get(0, 0.0)) <= float(bounds.get(3, 0.0))
}

fn _selection_bounds_from_minmax(any: bmin, any: bmax): list {
   if(!selection_bounds_valid(bmin) || !selection_bounds_valid(bmax)){ return [] }
   [
      float(bmin.get(0, 0.0)), float(bmin.get(1, 0.0)), float(bmin.get(2, 0.0)),
      float(bmax.get(0, 0.0)), float(bmax.get(1, 0.0)), float(bmax.get(2, 0.0))
   ]
}

fn _selection_bounds_fit(any: bounds, any: scale, any: x, any: y, any: z): list {
   if(!is_list(bounds) || bounds.len < 6){ return [] }
   def sc = float(scale)
   def tx, ty = float(x), float(y)
   def tz = float(z)
   [
      float(bounds.get(0, 0.0)) * sc + tx, float(bounds.get(1, 0.0)) * sc + ty, float(bounds.get(2, 0.0)) * sc + tz,
      float(bounds.get(3, 0.0)) * sc + tx, float(bounds.get(4, 0.0)) * sc + ty, float(bounds.get(5, 0.0)) * sc + tz
   ]
}

fn _model_data_off(any: m): int {
   if(!is_list(m)){ return -1 }
   def n = m.len
   if(n >= 18 && int(m.get(0, 0)) == 4 && int(m.get(1, 0)) == 4){ return 2 }
   if(n >= 16){ return 0 }
   -1
}

fn _transform_bound_point(any: m, any: x, any: y, any: z): list {
   def off = _model_data_off(m)
   if(off < 0){ return [float(x), float(y), float(z)] }
   [
      float(x) * float(m.get(off + 0, 1.0)) + float(y) * float(m.get(off + 4, 0.0)) + float(z) * float(m.get(off + 8, 0.0)) + float(m.get(off + 12, 0.0)),
      float(x) * float(m.get(off + 1, 0.0)) + float(y) * float(m.get(off + 5, 1.0)) + float(z) * float(m.get(off + 9, 0.0)) + float(m.get(off + 13, 0.0)),
      float(x) * float(m.get(off + 2, 0.0)) + float(y) * float(m.get(off + 6, 0.0)) + float(z) * float(m.get(off + 10, 1.0)) + float(m.get(off + 14, 0.0))
   ]
}

fn selection_bounds_from_part_bounds(any: pmin, any: pmax, any: model): list {
   if(!selection_bounds_valid(pmin) || !selection_bounds_valid(pmax)){ return [] }
   def x1 = float(pmin.get(0, 0.0)) def y1 = float(pmin.get(1, 0.0)) def z1 = float(pmin.get(2, 0.0))
   def x2 = float(pmax.get(0, 0.0)) def y2 = float(pmax.get(1, 0.0)) def z2 = float(pmax.get(2, 0.0))
   mut mnx, mny, mnz = 1e9, 1e9, 1e9
   mut mxx, mxy, mxz = -1e9, -1e9, -1e9
   mut i = 0
   while(i < 8){
      def px, py = (i & 1) ? x2 : x1, (i & 2) ? y2 : y1
      def pz = (i & 4) ? z2 : z1
      def p = _transform_bound_point(model, px, py, pz)
      def wx = float(p.get(0, 0.0))
      def wy = float(p.get(1, 0.0))
      def wz = float(p.get(2, 0.0))
      if(wx < mnx){ mnx = wx } if(wx > mxx){ mxx = wx }
      if(wy < mny){ mny = wy } if(wy > mxy){ mxy = wy }
      if(wz < mnz){ mnz = wz } if(wz > mxz){ mxz = wz }
      i += 1
   }
   [mnx, mny, mnz, mxx, mxy, mxz]
}

fn _accum_bounds(any: cur, any: part_bounds): list {
   if(!is_list(part_bounds) || part_bounds.len < 6){ return cur }
   mut out = cur
   if(!is_list(out) || out.len < 6){ out = [1e9, 1e9, 1e9, -1e9, -1e9, -1e9] }
   def mnx, mny = float(part_bounds.get(0, 0.0)), float(part_bounds.get(1, 0.0))
   def mnz = float(part_bounds.get(2, 0.0))
   def mxx = float(part_bounds.get(3, 0.0))
   def mxy = float(part_bounds.get(4, 0.0))
   def mxz = float(part_bounds.get(5, 0.0))
   if(mnx < float(out.get(0, 0.0))){ out[0] = mnx }
   if(mny < float(out.get(1, 0.0))){ out[1] = mny }
   if(mnz < float(out.get(2, 0.0))){ out[2] = mnz }
   if(mxx > float(out.get(3, 0.0))){ out[3] = mxx }
   if(mxy > float(out.get(4, 0.0))){ out[4] = mxy }
   if(mxz > float(out.get(5, 0.0))){ out[5] = mxz }
   out
}

fn selection_bounds_from_parts(any: parts): list {
   if(!is_list(parts) || parts.len == 0){ return [] }
   mut out = []
   mut i = 0
   while(i < parts.len){
      def part = parts.get(i, 0)
      if(is_dict(part) && part.get("visible", true)){
         mut pmin = part.get("min", 0)
         mut pmax = part.get("max", 0)
         if(!selection_bounds_valid(pmin) || !selection_bounds_valid(pmax)){
            def mesh = part.get("mesh", 0)
            if(is_dict(mesh)){ pmin, pmax = mesh.get("min", pmin), mesh.get("max", pmax) }
         }
         out = _accum_bounds(out, selection_bounds_from_part_bounds(pmin, pmax, part.get("model", 0)))
      }
      i += 1
   }
   if(!_selection_bounds_ready(out)){ return [] }
   out
}

fn selection_bounds_from_gpu_parts(any: parts): list {
   if(!is_list(parts) || parts.len == 0){ return [] }
   mut out = []
   mut i = 0
   while(i < parts.len){
      def rec = parts.get(i, 0)
      if(is_list(rec) && rec.len >= 40 && int(rec.get(37, 1)) != 0){ out = _accum_bounds(out, selection_bounds_from_part_bounds(rec.get(38, 0), rec.get(39, 0), rec.get(10, 0))) }
      i += 1
   }
   if(!_selection_bounds_ready(out)){ return [] }
   out
}

fn _selection_bounds_add_offset(any: bounds, any: x, any: y, any: z): list {
   _selection_bounds_fit(bounds, 1.0, x, y, z)
}

fn selection_bounds_from_scene(any: scene_obj): list {
   if(scene_obj == 0 || !is_dict(scene_obj)){ return [] }
   def edit_tx = float(scene_obj.get("edit_tx", 0.0))
   def edit_ty = float(scene_obj.get("edit_ty", 0.0))
   def edit_tz = float(scene_obj.get("edit_tz", 0.0))
   mut bmin = scene_obj.get("fit_world_min", 0)
   mut bmax = scene_obj.get("fit_world_max", 0)
   def has_fit_world = selection_bounds_valid(bmin) && selection_bounds_valid(bmax)
   def has_model_bounds = selection_bounds_valid(scene_obj.get("min", 0)) && selection_bounds_valid(scene_obj.get("max", 0))
   if(!selection_bounds_valid(bmin) || !selection_bounds_valid(bmax)){ bmin, bmax = scene_obj.get("min", 0), scene_obj.get("max", 0) }
   if(selection_bounds_valid(bmin) && selection_bounds_valid(bmax)){
      def direct_bounds = _selection_bounds_from_minmax(bmin, bmax)
      if(has_fit_world || !has_model_bounds){ return _selection_bounds_add_offset(direct_bounds, edit_tx, edit_ty, edit_tz) }
      def fit_bounds = _selection_bounds_fit(direct_bounds, scene_obj.get("fit_scale", 1.0), scene_obj.get("fit_tx", 0.0), scene_obj.get("fit_ty", 0.0), scene_obj.get("fit_tz", 0.0))
      return _selection_bounds_add_offset(fit_bounds, edit_tx, edit_ty, edit_tz)
   }
   def part_bounds = selection_bounds_from_parts(scene_obj.get("parts", 0))
   if(is_list(part_bounds) && part_bounds.len >= 6){ return _selection_bounds_add_offset(part_bounds, edit_tx, edit_ty, edit_tz) }
   def gpu_bounds = selection_bounds_from_gpu_parts(scene_obj.get("gpu_parts", 0))
   if(is_list(gpu_bounds) && gpu_bounds.len >= 6){ return _selection_bounds_add_offset(gpu_bounds, edit_tx, edit_ty, edit_tz) }
   []
}
