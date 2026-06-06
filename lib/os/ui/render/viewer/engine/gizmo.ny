;; Keywords: engine gizmo transform move rotate scale os ui render viewer scene
;; Engine-specific transform gizmo interaction and selected-object manipulation helpers.
;; References:
;; - std.os.ui.render.viewer.gizmo
;; - std.os.ui.render.scene
module std.os.ui.render.viewer.engine.gizmo(_draw_scene_world_gizmo, _scene_world_gizmo_pick)
use std.core
use std.math
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
