;; Keywords: assets asset-browser catalog
;; Asset catalog indexing, filtering, and grid data for UI asset browsers.
module std.os.ui.assets.catalog(catalog_filter_key, catalog_pick_cache, catalog_row_id, catalog_filter, scene_part_count, hierarchy_node_label, hierarchy_node_detail, indent_prefix, virtual_row_range, asset_grid_cols, asset_grid_usable_w, asset_tile_h, asset_grid_content_h, asset_grid_fit_h, asset_grid_view_h, format_name_list, asset_icon_name, asset_detail)
use std.core
use std.core.str as str
use std.math (clamp)

def _ASSET_ICON_RULES = [
   ["asset_camera", ["camera"]],
   ["asset_light", ["light", "lamp", "lantern"]],
   ["asset_texture", ["texture", "uv", "normal"]],
   ["asset_material", ["material", "metal", "rough", "clearcoat", "specular", "iridescence", "transmission", "alpha", "sheen", "anisotropy", "dispersion"]],
   ["asset_animation", ["anim", "morph", "skin", "rigged", "interpolation"]]
]

fn catalog_filter_key(any: filter): str { str.lower(str.strip(to_str(filter))) }

fn catalog_pick_cache(any: cache, any: items, any: value, any: filter): dict {
   mut out = is_dict(cache) ? cache : dict(8)
   def name = catalog_filter_key(value)
   def filter_key = catalog_filter_key(filter)
   def source_len = is_list(items) ? items.len : 0
   if(to_str(out.get("name", "\x00")) == name &&
      to_str(out.get("filter", "\x00")) == filter_key &&
      int(out.get("len", -1)) == source_len){
      return out
   }
   mut idx = -1
   mut i = 0
   while(is_list(items) && i < items.len){
      if(catalog_filter_key(items.get(i, "")) == name){
         idx = i
         break
      }
      i += 1
   }
   out["name"] = name
   out["filter"] = filter_key
   out["len"] = source_len
   out["idx"] = idx
   out
}

fn catalog_row_id(any: name): str {
   mut s = str.strip(to_str(name))
   if(s.len == 0){ return "item" }
   s = str.str_replace(s, " ", "_")
   s = str.str_replace(s, "/", "_")
   s = str.str_replace(s, "\\", "_")
   s = str.str_replace(s, ":", "_")
   s = str.str_replace(s, ".", "_")
   s
}

fn catalog_filter(any: names, any: filter): list {
   if(!is_list(names)){ return [] }
   def want = catalog_filter_key(filter)
   if(want.len == 0){ return names }
   mut out = []
   mut i = 0
   while(i < names.len){
      def name = to_str(names.get(i, ""))
      if(str.find(str.lower(name), want) >= 0){ out = out.append(name) }
      i += 1
   }
   out
}

fn scene_part_count(any: scene_obj): int {
   if(scene_obj == 0 || !is_dict(scene_obj)){ return 0 }
   def gpu_n = int(scene_obj.get("gpu_parts_count", 0))
   if(gpu_n > 0){ return gpu_n }
   def parts = scene_obj.get("parts", [])
   is_list(parts) ? parts.len : 0
}

fn indent_prefix(any: depth): str {
   mut s, i = "", 0
   while(i < int(depth)){
      s = s + "  "
      i += 1
   }
   s
}

fn hierarchy_node_label(any: node, int: idx): str {
   if(!is_dict(node)){ return "Node " + to_str(idx) }
   def name = str.strip(to_str(node.get("name", "")))
   (name.len > 0) ? name : ("Node " + to_str(idx))
}

fn hierarchy_node_detail(any: node): str {
   if(!is_dict(node)){ return "" }
   mut parts = []
   if(node.contains("mesh")){ parts = parts.append("mesh " + to_str(int(node.get("mesh", -1)))) }
   if(node.contains("camera")){ parts = parts.append("camera " + to_str(int(node.get("camera", -1)))) }
   if(node.contains("skin")){ parts = parts.append("skin " + to_str(int(node.get("skin", -1)))) }
   def children = node.get("children", [])
   if(is_list(children) && children.len > 0){ parts = parts.append(to_str(children.len) + " children") }
   if(parts.len == 0){ return "transform node" }
   str.join(parts, "  ")
}

fn virtual_row_range(any: total_rows, any: row_step, any: visible_h, any: scroll_y, any: overscan=3): list {
   def total = max(0, int(total_rows))
   def step = max(1.0, float(row_step))
   mut first_row = int(float(scroll_y) / step)
   if(first_row < 0){ first_row = 0 }
   if(first_row > total){ first_row = total }
   mut visible_rows = int(float(visible_h) / step) + int(overscan)
   if(visible_rows < 1){ visible_rows = 1 }
   mut last_row = first_row + visible_rows
   if(last_row > total){ last_row = total }
   [first_row, last_row]
}

fn asset_grid_cols(any: win_w, any: compact=false): int {
   if(bool(compact)){ return 1 }
   def ww = int(float(win_w))
   case ww {
      1420..1000000 -> 6
      1120..1419 -> 5
      840..1119 -> 4
      560..839 -> 3
      420..559 -> 2
      _ -> 1
   }
}

fn asset_grid_usable_w(any: win_w, any: compact=false): f64 { bool(compact) ? max(120.0, float(win_w) - 44.0) : max(1.0, float(win_w) - 64.0) }

fn asset_tile_h(any: show_paths=false): f64 { bool(show_paths) ? 66.0 : 50.0 }

fn asset_grid_content_h(any: model_count, any: win_w, any: compact=false, any: show_paths=false): f64 {
   def total_items = int(model_count)
   if(total_items <= 0){ return 58.0 }
   def cols = asset_grid_cols(asset_grid_usable_w(win_w, compact), compact)
   def rows = (total_items + cols - 1) / cols
   max(58.0, float(rows) * asset_tile_h(show_paths) + float(max(0, rows - 1)) * 8.0 + 4.0)
}

fn asset_grid_fit_h(any: model_count, any: win_w, any: requested_h, any: compact=false, any: show_paths=false): f64 { clamp(asset_grid_content_h(model_count, win_w, compact, show_paths), 90.0, float(requested_h)) }

fn asset_grid_view_h(any: requested_h, any: compact=false, any: standalone=false): f64 {
   def max_h = bool(standalone) ? 920.0 : (bool(compact) ? 520.0 : 320.0)
   clamp(float(requested_h), bool(compact) ? 160.0 : 220.0, max_h)
}

fn format_name_list(any: items): str {
   if(!is_list(items)){ return "" }
   mut out = ""
   mut i = 0
   while(i < items.len){
      def item = to_str(items.get(i, ""))
      if(item.len > 0){ out = (out.len > 0) ? (out + ", " + item) : item }
      i += 1
   }
   out
}

fn _text_has_any(str: haystack, any: needles): bool {
   mut i = 0
   def n = is_list(needles) ? needles.len : 0
   while(i < n){
      if(str.find(haystack, to_str(needles.get(i, ""))) >= 0){ return true }
      i += 1
   }
   false
}

fn asset_icon_name(any: name, any: rules=0): str {
   "Returns a stable UI icon name for a human-readable asset/model label."
   def s = str.lower(to_str(name))
   def rows = is_list(rules) ? rules : _ASSET_ICON_RULES
   mut i = 0
   while(i < rows.len){
      def row = rows[i]
      if(_text_has_any(s, row.get(1, []))){ return to_str(row.get(0, "asset_model")) }
      i += 1
   }
   "asset_model"
}

fn asset_detail(any: name, any: loaded=false, any: rules=0): str {
   "Returns a compact asset detail label for browsers and inspectors."
   if(bool(loaded)){ return "Loaded scene" }
   def icon = asset_icon_name(name, rules)
   if(icon == "asset_camera"){ return "Camera" }
   if(icon == "asset_light"){ return "Light rig" }
   if(icon == "asset_texture"){ return "Texture set" }
   if(icon == "asset_material"){ return "Material test" }
   if(icon == "asset_animation"){ return "Animation rig" }
   "Model"
}
