;; Keywords: engine hierarchy selection tags assets os ui render viewer scene
;; Hierarchy tree helpers for scene nodes, materials, asset tags, and selection state.
;; References:
;; - std.os.ui.assets.catalog
;; - std.os.ui.render.viewer.icons
module std.os.ui.render.viewer.engine.hierarchy(
   active_gltf, gltf_list, draw_body, selected_node, selected_part, selected_material, reset_selection
)

use std.core
use std.math (clamp, max, min)
use std.os.ui.assets.catalog as asset_catalog
use std.os.ui.render.viewer.gui as gui
use std.os.ui.render.viewer.icons as icons

mut _selected_node = -1
mut _selected_part = -1
mut _selected_material = -1

fn selected_node() int {
   "Returns the currently selected hierarchy node index."
   _selected_node
}

fn selected_part() int {
   "Returns the currently selected draw part index."
   _selected_part
}

fn selected_material() int {
   "Returns the material index for the selected draw part."
   _selected_material
}

fn reset_selection() int {
   "Clears the selected hierarchy node."
   _selected_node = -1
   _selected_part = -1
   _selected_material = -1
   _selected_node
}

fn active_gltf(scene) {
   "Returns the active glTF dictionary from a loaded scene object."
   if(!is_dict(scene)){ return 0 }
   def gltf_data = scene.get("gltf_data", 0)
   is_dict(gltf_data) ? gltf_data.get("gltf", 0) : 0
}

fn gltf_list(g, key) {
   "Returns a glTF list field or an empty list."
   if(is_dict(g)){
      def value = g.get(key, [])
      if(is_list(value)){ return value }
   }
   []
}

fn _summary(nodes, parts, meshes, materials, animations) int {
   def node_count = is_list(nodes) ? nodes.len : 0
   def part_count = is_list(parts) ? parts.len : 0
   gui.text_colored("Hierarchy", [0.78, 0.86, 0.88, 1.0])
   gui.text(
      to_str(node_count) + " nodes   "
      + to_str(part_count) + " parts   "
      + to_str(meshes.len) + " meshes   "
      + to_str(materials.len) + " materials   "
      + to_str(animations.len) + " anim"
   )
   part_count
}

fn _icon_missing(any icon) bool {
   if(is_dict(icon)){ return int(icon.get("tex", -1)) < 0 }
   int(icon) < 0
}

fn _node_icon(node, parity_lock, camera_icon, model_icon, node_icon) any {
   if(parity_lock){ return -1 }
   mut mesh_icon = model_icon
   mut fallback_icon = node_icon
   if(_icon_missing(mesh_icon)){ mesh_icon = icons.icon_sprite("asset_model") }
   if(_icon_missing(fallback_icon)){ fallback_icon = icons.icon_sprite("node") }
   node.contains("mesh") ? mesh_icon : fallback_icon
}

fn _draw_node(idp, nodes, node_idx, depth, drawn, max_rows, row_w, parity_lock, camera_icon, model_icon, node_icon) int {
   if(drawn >= max_rows){ return drawn }
   def idx = int(node_idx)
   if(!is_list(nodes) || idx < 0 || idx >= nodes.len){ return drawn }
   def node = nodes[idx]
   if(!is_dict(node)){ return drawn }
   def label = asset_catalog.indent_prefix(depth) + asset_catalog.hierarchy_node_label(node, idx)
   def detail = asset_catalog.hierarchy_node_detail(node)
   if(gui.selectable(idp + "_node_" + to_str(idx), label, _selected_node == idx, row_w, 34.0, detail,
      _node_icon(node, parity_lock, camera_icon, model_icon, node_icon))){
      _selected_node = idx
      _selected_part = -1
      _selected_material = -1
   }
   mut out = drawn + 1
   def children = node.get("children", [])
   if(is_list(children) && depth < 8){
      mut ci = 0
      def children_n = children.len
      while(ci < children_n && out < max_rows){
         out = _draw_node(idp, nodes, int(children[ci]), depth + 1, out, max_rows, row_w,
         parity_lock, camera_icon, model_icon, node_icon)
         ci += 1
      }
   }
   out
}

fn _virtual_row_range(total_rows, row_step, overscan=3) {
   asset_catalog.virtual_row_range(total_rows,
      row_step,
      gui.scroll_area_visible_h(),
      gui.scroll_area_scroll_y(),
   overscan)
}

fn _draw_parts(idp, parts, win_w, list_h, max_rows, parity_lock, model_icon) int {
   gui.text_colored("Draw Parts", [0.78, 0.86, 0.88, 1.0])
   if(!is_list(parts) || parts.len == 0){
      gui.text_colored("No draw parts available.", [0.86, 0.74, 0.55, 1.0])
      return 0
   }
   def row_h = 32.0
   def row_step = row_h + gui.layout_gap()
   def total_rows = min(parts.len, int(max_rows))
   gui.begin_scroll_area(idp + "_hierarchy_parts", 0.0, list_h)
   def range = _virtual_row_range(total_rows, row_step, 1)
   def first_row = int(range[0])
   def last_row = int(range[1])
   if(first_row > 0){ gui.spacer_px(float(first_row) * row_step) }
   mut pi = first_row
   def row_w = max(120.0, float(win_w) - 44.0)
   def icon = parity_lock ? -1 : (_icon_missing(model_icon) ? icons.icon_sprite("asset_model") : model_icon)
   while(pi < last_row){
      def part = parts[pi]
      def node_idx = is_dict(part) ? int(part.get("node_idx", -1)) : -1
      def mat_idx = is_dict(part) ? int(part.get("mat_idx", -1)) : -1
      if(gui.selectable(idp + "_part_" + to_str(pi), "Part " + to_str(pi), _selected_part == pi, row_w, row_h,
         "node " + to_str(node_idx) + "  mat " + to_str(mat_idx), icon)){
         _selected_part = pi
         _selected_node = node_idx
         _selected_material = mat_idx
      }
      pi += 1
   }
   if(last_row < total_rows){ gui.spacer_px(float(total_rows - last_row) * row_step) }
   if(parts.len > max_rows){
      gui.text_colored("Part list truncated to " + to_str(int(max_rows)) + " rows.", [0.86, 0.74, 0.55, 1.0])
   }
   gui.end_scroll_area()
   total_rows
}

fn _parts_h(win_h, compact) f64 {
   clamp(float(win_h) - (compact ? 92.0 : 148.0), 190.0, compact ? 900.0 : 620.0)
}

fn _roots(g, nodes) {
   mut roots = []
   def scenes = gltf_list(g, "scenes")
   def scene_idx = is_dict(g) ? int(g.get("scene", 0)) : 0
   if(scene_idx >= 0 && scene_idx < scenes.len){
      def scene_obj = scenes.get(scene_idx, 0)
      if(is_dict(scene_obj)){ roots = scene_obj.get("nodes", []) }
   }
   if(!is_list(roots) || roots.len == 0){
      mut ni = 0
      while(ni < nodes.len && ni < 160){
         roots = roots.append(ni)
         ni += 1
      }
   }
   roots
}

fn _tree_h(win_h, compact, part_count) f64 {
   if(part_count > 0){
      return clamp(float(win_h) * (compact ? 0.36 : 0.40), 150.0, compact ? 340.0 : 420.0)
   }
   clamp(float(win_h) - (compact ? 110.0 : 168.0), 210.0, compact ? 900.0 : 650.0)
}

fn _draw_tree(idp, nodes, roots, win_w, tree_h, parity_lock, camera_icon, model_icon, node_icon) int {
   def row_w = max(120.0, float(win_w) - 44.0)
   gui.begin_scroll_area(idp + "_hierarchy_tree", 0.0, tree_h)
   mut drawn = 0
   mut ri = 0
   while(ri < roots.len && drawn < 180){
      drawn = _draw_node(idp, nodes, int(roots.get(ri, -1)), 0, drawn, 180, row_w,
      parity_lock, camera_icon, model_icon, node_icon)
      ri += 1
   }
   if(drawn >= 180){
      gui.text_colored("Hierarchy truncated to 180 rows for UI responsiveness.", [0.86, 0.74, 0.55, 1.0])
   }
   gui.end_scroll_area()
   drawn
}

fn draw_body(idp, scene, win_w, win_h, compact=false, parity_lock=false, camera_icon=-1, model_icon=-1, node_icon=-1) int {
   "Draws the scene hierarchy and draw-part browser body."
   if(!is_dict(scene)){
      gui.text_colored("No loaded scene.", [0.86, 0.74, 0.55, 1.0])
      gui.text("Load a model from Catalog to populate the hierarchy.")
      return 0
   }
   def g = active_gltf(scene)
   def nodes = gltf_list(g, "nodes")
   def meshes = gltf_list(g, "meshes")
   def materials = gltf_list(g, "materials")
   def animations = gltf_list(g, "animations")
   def parts = scene.get("parts", [])
   def part_count = _summary(nodes, parts, meshes, materials, animations)
   if(nodes.len == 0){
      gui.text_colored("Scene has no glTF node list; showing draw parts instead.", [0.86, 0.74, 0.55, 1.0])
      return _draw_parts(idp, parts, win_w, _parts_h(win_h, compact), compact ? 260 : 360, parity_lock, model_icon)
   }
   def roots = _roots(g, nodes)
   def tree_h = _tree_h(win_h, compact, part_count)
   def rows = _draw_tree(idp, nodes, roots, win_w, tree_h, parity_lock, camera_icon, model_icon, node_icon)
   if(part_count > 0){
      def parts_h = clamp(float(win_h) - tree_h - (compact ? 148.0 : 190.0), 150.0, compact ? 520.0 : 620.0)
      def _parts_rows = _draw_parts(idp, parts, win_w, parts_h, compact ? 220 : 320, parity_lock, model_icon)
   }
   rows
}

#main {
   def g = {"scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": "root", "children": [1]}, {"mesh": 0}], "meshes": [0]}
   def scene = {"gltf_data": {"gltf": g}, "parts": [{"node_idx": 1, "mat_idx": 0}]}
   assert(gltf_list(active_gltf(scene), "nodes").len == 2, "hierarchy gltf list")
   assert(_roots(g, gltf_list(g, "nodes")).get(0, -1) == 0, "hierarchy roots")
   assert(selected_node() == -1 && selected_part() == -1 && selected_material() == -1 && reset_selection() == -1, "hierarchy selection")
   print("✓ std.os.ui.render.viewer.engine.hierarchy self-test passed")
}
