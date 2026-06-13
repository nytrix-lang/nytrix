;; Keywords: engine gizmo transform move rotate scale os ui render viewer scene
;; Engine-specific transform gizmo interaction and selected-object manipulation helpers.
;; References:
;; - std.os.ui.render.viewer.gizmo
;; - std.os.ui.render.scene
module std.os.ui.render.viewer.engine.gizmo(_draw_scene_world_gizmo, _scene_world_gizmo_pick, _scene_world_gizmo_axis_tangent, _scene_world_gizmo_axis_drag_coord)
use std.core
use std.math
use std.os.ui.render.matrix as rmat
use std.os.ui.render as gfx
use std.os.ui.render.viewer.gizmo as viewer_gizmo
use std.os.ui.render.viewer.engine.selection as ui_selection
use std.os.ui.render.viewer.engine.state

fn _gizmo_selection_bounds() list {
   if(!is_dict(active_scene)){ return [] }
   if(_scene_drag_active && int(_scene_drag_mode) == 1){
      def drag_bounds = _scene_drag_state.get("bounds", [])
      if(is_list(drag_bounds) && drag_bounds.len >= 6){ return drag_bounds }
   }
   def cache_key = _gizmo_selection_cache_key()
   if(_scene_selection_cached_name == cache_key && is_list(_scene_selection_cached_bounds) && _scene_selection_cached_bounds.len >= 6){
      return _scene_selection_cached_bounds
   }
   _scene_selection_cached_bounds = ui_selection.selection_bounds_from_scene(active_scene)
   _scene_selection_cached_name = cache_key
   is_list(_scene_selection_cached_bounds) ? _scene_selection_cached_bounds : []
}

fn _gizmo_selection_cache_key() str {
   if(!is_dict(active_scene)){ return _loaded_scene_name + "|none" }
   _loaded_scene_name + "|" +
   to_str(active_scene.get("fit_scale", 1.0)) + "|" +
   to_str(active_scene.get("fit_tx", 0.0)) + "|" +
   to_str(active_scene.get("fit_ty", 0.0)) + "|" +
   to_str(active_scene.get("fit_tz", 0.0)) + "|" +
   to_str(active_scene.get("edit_tx", 0.0)) + "|" +
   to_str(active_scene.get("edit_ty", 0.0)) + "|" +
   to_str(active_scene.get("edit_tz", 0.0)) + "|" +
   to_str(active_scene.get("edit_rx", 0.0)) + "|" +
   to_str(active_scene.get("edit_ry", 0.0)) + "|" +
   to_str(active_scene.get("edit_rz", 0.0)) + "|" +
   to_str(active_scene.get("edit_scale", 1.0)) + "|" +
   to_str(active_scene.get("edit_sx", active_scene.get("edit_scale", 1.0))) + "|" +
   to_str(active_scene.get("edit_sy", active_scene.get("edit_scale", 1.0))) + "|" +
   to_str(active_scene.get("edit_sz", active_scene.get("edit_scale", 1.0)))
}

fn _gizmo_draw_transform(list b) bool {
   def x0, y0 = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   def z0 = float(b.get(2, 0.0))
   def x1, y1 = float(b.get(3, 0.0)), float(b.get(4, 0.0))
   def z1 = float(b.get(5, 0.0))
   def m = viewer_gizmo.metrics(b, _gizmo_mode)
   def pad = max(0.010, float(m.get(3, 1.0)) * 0.025)
   def span = float(m.get(3, 1.0))
   def box_col = [1.0, 0.68, 0.20, 0.86]
   def red = [1.00, 0.16, 0.12, 0.92]
   def green = [0.20, 0.92, 0.26, 0.92]
   def blue = [0.22, 0.42, 1.00, 0.92]
   def box_thick = clamp(span * 0.0016, 0.018, 0.090)
   def corner_len = clamp(span * 0.18, 0.45, span * 0.34)
   viewer_gizmo.draw_box_corners(x0 - pad, y0 - pad, z0 - pad, x1 + pad, y1 + pad, z1 + pad, box_col, box_thick, corner_len)
   viewer_gizmo.draw_axes(
      float(m.get(0, 0.0)), float(m.get(1, 0.0)), float(m.get(2, 0.0)),
      float(m.get(3, 1.0)), float(m.get(4, 1.0)), float(m.get(5, 0.004)),
      float(m.get(7, 0.02)), red, green, blue, _gizmo_mode, _win_w, _win_h, _gizmo_axis
   )
   true
}

fn _scene_world_gizmo_pick(any x, any y) dict {
   if(!_scene_selected || !_scene_selection_rect || !is_dict(active_scene) || !show_scene){
      return {"hit": false, "axis": 0, "mode": _gizmo_mode}
   }
   def b = _gizmo_selection_bounds()
   if(!is_list(b) || b.len < 6){ return {"hit": false, "axis": 0, "mode": _gizmo_mode} }
   viewer_gizmo.hit_test(M_VP, _win_w, _win_h, b, _gizmo_mode, x, y)
}

fn _scene_world_gizmo_axis_tangent(any axis) dict {
   if(!_scene_selected || !_scene_selection_rect || !is_dict(active_scene) || !show_scene){
      return {"ok": false, "axis": int(axis), "screen_axis_x": 0.0, "screen_axis_y": 0.0}
   }
   def b = _gizmo_selection_bounds()
   if(!is_list(b) || b.len < 6){ return {"ok": false, "axis": int(axis), "screen_axis_x": 0.0, "screen_axis_y": 0.0} }
   def m = viewer_gizmo.metrics(b, _gizmo_mode)
   def v = _gizmo_axis_vec(int(axis))
   def n = viewer_gizmo.axis_tangent(
      M_VP, _win_w, _win_h,
      float(m.get(0, 0.0)), float(m.get(1, 0.0)), float(m.get(2, 0.0)),
      float(v.get(0, 0.0)), float(v.get(1, 0.0)), float(v.get(2, 0.0)),
      float(m.get(4, 1.0))
   )
   {"ok": true, "axis": int(axis), "screen_axis_x": float(n.get(0, 0.0)), "screen_axis_y": float(n.get(1, 0.0))}
}

fn _gizmo_axis_vec(int axis) list {
   if(axis == 1){ return [1.0, 0.0, 0.0] }
   if(axis == 2){ return [0.0, 1.0, 0.0] }
   if(axis == 3){ return [0.0, 0.0, 1.0] }
   [0.0, 0.0, 0.0]
}

fn _gizmo_bounds_center(any bounds) list {
   if(!is_list(bounds) || bounds.len < 6){ return [0.0, 0.0, 0.0] }
   [
      (float(bounds.get(0, 0.0)) + float(bounds.get(3, 0.0))) * 0.5,
      (float(bounds.get(1, 0.0)) + float(bounds.get(4, 0.0))) * 0.5,
      (float(bounds.get(2, 0.0)) + float(bounds.get(5, 0.0))) * 0.5
   ]
}

fn _gizmo_unproject(list inv_vp, f64 ndc_x, f64 ndc_y, f64 ndc_z) any {
   def v = rmat.mat4_mul_vec4(inv_vp, [ndc_x, ndc_y, ndc_z, 1.0])
   def w = float(v.get(3, 0.0))
   if(abs(w) <= 0.000001){ return 0 }
   [float(v.get(0, 0.0)) / w, float(v.get(1, 0.0)) / w, float(v.get(2, 0.0)) / w]
}

fn _gizmo_ray_from_screen(any x, any y) dict {
   def ww = max(float(_win_w), 1.0)
   def wh = max(float(_win_h), 1.0)
   def ndc_x = (float(x) / ww) * 2.0 - 1.0
   def ndc_y = 1.0 - (float(y) / wh) * 2.0
   def inv = rmat.mat4_inverse(M_VP)
   def near_p = _gizmo_unproject(inv, ndc_x, ndc_y, 0.0)
   def far_p = _gizmo_unproject(inv, ndc_x, ndc_y, 1.0)
   if(!is_list(near_p) || !is_list(far_p)){
      return {"ok": false, "ox": 0.0, "oy": 0.0, "oz": 0.0, "dx": 0.0, "dy": 0.0, "dz": -1.0}
   }
   def ox, oy = float(near_p.get(0, 0.0)), float(near_p.get(1, 0.0))
   def oz = float(near_p.get(2, 0.0))
   mut dx, dy = float(far_p.get(0, 0.0)) - ox, float(far_p.get(1, 0.0)) - oy
   mut dz = float(far_p.get(2, 0.0)) - oz
   def len = sqrt(dx * dx + dy * dy + dz * dz)
   if(len <= 0.000001){ return {"ok": false, "ox": ox, "oy": oy, "oz": oz, "dx": 0.0, "dy": 0.0, "dz": -1.0} }
   dx = dx / len
   dy = dy / len
   dz = dz / len
   {"ok": true, "ox": ox, "oy": oy, "oz": oz, "dx": dx, "dy": dy, "dz": dz}
}

fn _gizmo_axis_ray_coord(any bounds, int axis, any x, any y) dict {
   if(axis < 1 || axis > 3){ return {"ok": false, "axis": axis, "coord": 0.0} }
   def ray = _gizmo_ray_from_screen(x, y)
   if(!bool(ray.get("ok", false))){ return {"ok": false, "axis": axis, "coord": 0.0} }
   def center = _gizmo_bounds_center(bounds)
   def avec = _gizmo_axis_vec(axis)
   def px, py = float(center.get(0, 0.0)), float(center.get(1, 0.0))
   def pz = float(center.get(2, 0.0))
   def ax, ay = float(avec.get(0, 0.0)), float(avec.get(1, 0.0))
   def az = float(avec.get(2, 0.0))
   def ox, oy = float(ray.get("ox", 0.0)), float(ray.get("oy", 0.0))
   def oz = float(ray.get("oz", 0.0))
   def dx, dy = float(ray.get("dx", 0.0)), float(ray.get("dy", 0.0))
   def dz = float(ray.get("dz", -1.0))
   def wx, wy = px - ox, py - oy
   def wz = pz - oz
   def b = ax * dx + ay * dy + az * dz
   def d = ax * wx + ay * wy + az * wz
   def e = dx * wx + dy * wy + dz * wz
   def den = 1.0 - b * b
   if(abs(den) <= 0.00001){ return {"ok": false, "axis": axis, "coord": 0.0} }
   def u = (b * e - d) / den
   {"ok": true, "axis": axis, "coord": u}
}

fn _scene_world_gizmo_axis_drag_coord(any axis, any x, any y, any bounds=[]) dict {
   if(!_scene_selected || !_scene_selection_rect || !is_dict(active_scene) || !show_scene){
      return {"ok": false, "axis": int(axis), "coord": 0.0}
   }
   def b = (is_list(bounds) && bounds.len >= 6) ? bounds : _gizmo_selection_bounds()
   if(!is_list(b) || b.len < 6){ return {"ok": false, "axis": int(axis), "coord": 0.0} }
   _gizmo_axis_ray_coord(b, int(axis), x, y)
}

fn _gizmo_sync_overlay_rects() bool {
   _selection_overlay_panel_rect = [0.0, 0.0, 0.0, 0.0]
   _selection_overlay_toolbar_rect = [0.0, 0.0, 0.0, 0.0]
   _selection_overlay_move_rect = [0.0, 0.0, 0.0, 0.0]
   _selection_overlay_rotate_rect = [0.0, 0.0, 0.0, 0.0]
   _selection_overlay_scale_rect = [0.0, 0.0, 0.0, 0.0]
   _selection_overlay_hover_mode = -1
   true
}

fn _draw_scene_world_gizmo() bool {
   if(!_scene_selected || !_scene_selection_rect || !is_dict(active_scene) || !show_scene){
      _gizmo_sync_overlay_rects()
      return false
   }
   def b = _gizmo_selection_bounds()
   if(!is_list(b) || b.len < 6){
      _gizmo_sync_overlay_rects()
      return false
   }
   _gizmo_sync_overlay_rects()
   gfx.set_model_matrix(M_ID)
   gfx.set_unlit(true)
   _gizmo_draw_transform(b)
   gfx.set_unlit(false)
   true
}
