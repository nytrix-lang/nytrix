;; Keywords: engine catalog assets search filter os ui render viewer scene
;; Catalog panel helpers for filtering, listing, and selecting viewer assets.
;; References:
;; - std.os.ui.assets.catalog
;; - std.os.ui.assets.viewer
module std.os.ui.render.viewer.engine.catalog(draw_grid, filter_cached, grid_h, invalidate_caches, model_index, pick_index_cached, refresh_names, sync_selected)
use std.core
use std.math (max, min)
use std.os.ui.assets.catalog as asset_catalog
use std.os.ui.assets.viewer as ui_assets
use std.os.ui.render.viewer.gui as gui
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.viewer.icons as icons

mut _row_id_cache = dict(512)
mut _icon_name_cache = dict(512)
mut _icon_sprite_cache = dict(128)
mut _detail_cache = dict(512)

fn invalidate_caches() int {
   "Clears cached catalog row ids, icons, and details."
   _row_id_cache = dict(512)
   _icon_name_cache = dict(512)
   _icon_sprite_cache = dict(128)
   _detail_cache = dict(512)
   0
}

fn refresh_names(any current, int limit=4096) list {
   "Returns the cached model catalog, or loads and sorts it once."
   if(is_list(current) && current.len > 0){ return current }
   mut names = ui_assets.list_gltf_asset_names(limit)
   if(is_list(names) && names.len > 1){ sort(names) }
   is_list(names) ? names : []
}

fn model_index(any names, any loaded_name) int {
   "Finds the current model in a catalog by display name."
   def loaded = to_str(loaded_name)
   if(!is_list(names) || loaded.len == 0){ return -1 }
   mut i = 0
   while(i < names.len){
      if(to_str(names.get(i, "")) == loaded){ return i }
      i += 1
   }
   -1
}

fn sync_selected(any selected_name, any loaded_name, any names) str {
   "Keeps the browser selection anchored to the loaded model or first catalog item."
   def selected = to_str(selected_name)
   if(selected.len > 0){ return selected }
   def loaded = to_str(loaded_name)
   if(loaded.len > 0){ return loaded }
   (is_list(names) && names.len > 0) ? to_str(names.get(0, "")) : ""
}

fn filter_cached(any names, any filter, any cache_key="\x00", int source_len=-1, any cached=[]) dict {
   "Filters model names and returns updated cache metadata."
   if(!is_list(names)){
      return {"items": [], "key": "\x00", "source_len": -1}
   }
   def want = asset_catalog.catalog_filter_key(filter)
   if(want.len == 0){
      return {"items": names, "key": "\x00", "source_len": names.len}
   }
   if(to_str(cache_key) == want && int(source_len) == names.len && is_list(cached)){
      return {"items": cached, "key": want, "source_len": names.len}
   }
   {"items": asset_catalog.catalog_filter(names, want), "key": want, "source_len": names.len}
}

fn pick_index_cached(any cache, any items, any value, any filter) dict {
   "Finds the selected item index and returns updated cache metadata."
   def base = is_dict(cache) ? cache : {}
   asset_catalog.catalog_pick_cache({
         "name": to_str(base.get("name", "\x00")),
         "filter": to_str(base.get("filter", "\x00")),
         "len": int(base.get("len", -1)),
         "idx": int(base.get("idx", -1)),
   }, items, value, filter)
}

fn _row_id(name) str {
   def key = to_str(name)
   if(_row_id_cache.contains(key)){ return to_str(_row_id_cache.get(key, key)) }
   def safe = asset_catalog.catalog_row_id(key)
   _row_id_cache[key] = safe
   safe
}

fn _icon_name(name) str {
   def key = to_str(name)
   if(_icon_name_cache.contains(key)){ return to_str(_icon_name_cache.get(key, "asset_model")) }
   def icon = asset_catalog.asset_icon_name(name)
   _icon_name_cache[key] = icon
   icon
}

fn _icon_sprite(name) any {
   def icon = _icon_name(name)
   if(_icon_sprite_cache.contains(icon)){ return _icon_sprite_cache.get(icon, -1) }
   def spr = icons.icon_sprite(icon)
   _icon_sprite_cache[icon] = spr
   spr
}

fn _detail(name, loaded=false) str {
   if(bool(loaded)){ return "Loaded scene" }
   def key = to_str(name)
   if(_detail_cache.contains(key)){ return to_str(_detail_cache.get(key, "Model")) }
   def detail = asset_catalog.asset_detail(key, false)
   _detail_cache[key] = detail
   detail
}

fn _path_detail(catalog, name) str {
   is_dict(catalog) ? asset_catalog.gltf_catalog_resolve(catalog, name) : to_str(name)
}

fn grid_h(f64 requested_h, bool compact=false, bool standalone=false, f64 reserve=0.0) f64 {
   "Returns the visible asset-grid height for the current layout budget."
   def wanted = asset_catalog.asset_grid_view_h(requested_h, compact, standalone)
   def floor = bool(compact) ? 120.0 : 140.0
   def budget = max(0.0, gui.remaining_h(reserve))
   if(budget <= 1.0){ return max(1.0, min(wanted, float(requested_h))) }
   if(budget < floor){ return budget }
   min(wanted, budget)
}

fn draw_grid(idp, suffix, model_names, win_w, list_h, compact=false, opts=0) dict {
   "Draws the virtualized model catalog grid and returns selection state."
   def prof = ui_profile.enabled()
   mut t_prof = prof ? ui_profile.now() : 0
   def items = is_list(model_names) ? model_names : []
   def options = is_dict(opts) ? opts : dict(0)
   def show_paths = bool(options.get("show_paths", false))
   def scene_loaded = bool(options.get("scene_loaded", false))
   def loaded_name = to_str(options.get("loaded_name", ""))
   def selected_name = to_str(options.get("selected_name", ""))
   mut selected_idx = int(options.get("selected_idx", -1))
   def parity_lock = bool(options.get("parity_lock", false))
   def hide_detail = bool(options.get("hide_detail", false))
   def file_list = bool(options.get("file_list", false))
   def show_icons = bool(options.get("show_icons", bool(options.get("file_icons", true))))
   def catalog = options.get("catalog", 0)
   def grid_w = asset_catalog.asset_grid_usable_w(win_w, compact)
   def cols = file_list ? 1 : asset_catalog.asset_grid_cols(grid_w, compact)
   def tile_gap = file_list ? 0.0 : 8.0
   def detail_enabled = !hide_detail && !file_list
   def tile_h = file_list ? 28.0 : (detail_enabled ? asset_catalog.asset_tile_h(show_paths) : (bool(compact) ? 34.0 : 40.0))
   def tile_w = bool(compact) ? grid_w : max(120.0, (grid_w - float(cols - 1) * tile_gap) / float(cols))
   def row_step = tile_h + (file_list ? 2.0 : gui.layout_gap())
   def total_items = items.len
   def total_rows = (total_items + cols - 1) / cols
   def scroll_id = to_str(idp) + "_" + to_str(suffix) + "_model_catalog"
   def content_h = total_items <= 0 ? 58.0 : (float(total_rows) * row_step + 4.0)
   t_prof = ui_profile.mark_next(prof, "asset_grid_prep", t_prof)
   if(selected_idx < 0 && selected_name.len > 0){ selected_idx = model_index(items, selected_name) }
   gui.set_scroll_area_content_hint(scroll_id, content_h)
   if(selected_idx >= 0 && selected_idx < total_items){
      def row = selected_idx / max(1, cols)
      def top = float(row) * row_step
      gui.scroll_area_ensure_visible(scroll_id, top, top + tile_h, max(1.0, list_h - 8.0))
   }
   gui.begin_scroll_area(scroll_id, 0.0, list_h)
   t_prof = ui_profile.mark_next(prof, "asset_grid_begin_scroll", t_prof)
   def range = asset_catalog.virtual_row_range(total_rows, row_step, gui.scroll_area_visible_h(), gui.scroll_area_scroll_y(), 1)
   def first_row = int(range[0])
   def last_row = int(range[1])
   if(first_row > 0){ gui.spacer_px(float(first_row) * row_step) }
   t_prof = ui_profile.mark_next(prof, "asset_grid_range", t_prof,
   " rows=" + to_str(first_row) + ".." + to_str(last_row) + " cols=" + to_str(cols))
   def tile_id_prefix = to_str(idp) + "_" + to_str(suffix) + "_mdl_"
   def loaded_icon = (parity_lock || !show_icons) ? -1 : icons.icon_sprite("asset_loaded")
   mut clicked = ""
   mut i = first_row * cols
   def end_i = min(total_items, last_row * cols)
   while(i < end_i){
      def name = to_str(items[i])
      def is_loaded = scene_loaded && loaded_name == name
      def is_selected = selected_name == name
      def detail = detail_enabled ? (show_paths ? _path_detail(catalog, name) : _detail(name, is_loaded)) : ""
      def icon = (parity_lock || !show_icons) ? -1 : (is_loaded ? loaded_icon : _icon_sprite(name))
      if(file_list){
         if(gui.selectable_file(tile_id_prefix + _row_id(name), name, is_loaded || is_selected, tile_w, tile_h, icon)){
            clicked = name
         }
      } elif(gui.selectable(tile_id_prefix + _row_id(name), name, is_loaded || is_selected, tile_w, tile_h, detail, icon, false)){
         clicked = name
      }
      def next_i = i + 1
      if(next_i < end_i && (next_i % cols) != 0){ gui.same_line(tile_gap) }
      i += 1
   }
   t_prof = ui_profile.mark_next(prof, "asset_grid_items", t_prof, " count=" + to_str(max(0, end_i - first_row * cols)))
   if(last_row < total_rows){ gui.spacer_px(float(total_rows - last_row) * row_step) }
   if(total_items == 0){ gui.text_colored("No models match.", [0.86, 0.74, 0.55, 1.0]) }
   gui.end_scroll_area()
   ui_profile.mark_done(prof, "asset_grid_end_scroll", t_prof)
   {"clicked": clicked, "count": total_items}
}

#main {
   assert(invalidate_caches() == 0, "catalog cache reset")
   assert(_row_id("a/b:c.d") == "a_b_c_d" && _icon_name("camera rig") == "asset_camera", "catalog row/icon")
   assert(_detail("lamp", false) == "Light rig" && _detail("x", true) == "Loaded scene", "catalog detail")
   def names = ["Avocado", "CesiumMan"]
   assert(model_index(names, "CesiumMan") == 1 && sync_selected("", "", names) == "Avocado", "catalog selection")
   assert(filter_cached(names, "ces").get("items", []).len == 1, "catalog filter")
   assert(pick_index_cached({}, names, "CesiumMan", "").get("idx", -1) == 1, "catalog pick cache")
   print("✓ std.os.ui.render.viewer.engine.catalog self-test passed")
}
