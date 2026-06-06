;; Keywords: assets asset-browser catalog os ui
;; UI asset facade for catalogs and browser data.
;; References:
;; - std.os.ui
;; - std.os.ui.assets.catalog
module std.os.ui.assets(viewer, catalog, batch, catalog_filter_key, catalog_pick_cache, catalog_row_id, catalog_filter, scene_part_count, hierarchy_node_label, hierarchy_node_detail, indent_prefix, virtual_row_range, asset_grid_cols, asset_grid_usable_w, asset_tile_h, asset_grid_content_h, asset_grid_fit_h, asset_grid_view_h, format_name_list, asset_icon_name, asset_detail, asset_dirs_from_env, first_gltf_in_dir, gltf_catalog_make, gltf_catalog_roots, gltf_catalog_scan, gltf_catalog_ensure, gltf_catalog_resolve, gltf_catalog_names)
use std.core
use std.os.ui.assets.viewer as viewer
use std.os.ui.assets.catalog as catalog
use std.os.ui.assets.batch as batch

fn catalog_filter_key(any filter) str { catalog.catalog_filter_key(filter) }

fn catalog_pick_cache(any cache, any items, any value, any filter) dict { catalog.catalog_pick_cache(cache, items, value, filter) }

fn catalog_row_id(any name) str { catalog.catalog_row_id(name) }

fn catalog_filter(any names, any filter) list { catalog.catalog_filter(names, filter) }

fn scene_part_count(any scene_obj) int { catalog.scene_part_count(scene_obj) }

fn hierarchy_node_label(any node, int idx) str { catalog.hierarchy_node_label(node, idx) }

fn hierarchy_node_detail(any node) str { catalog.hierarchy_node_detail(node) }

fn indent_prefix(any depth) str { catalog.indent_prefix(depth) }

fn virtual_row_range(any total_rows, any row_step, any visible_h, any scroll_y, any overscan=3) list { catalog.virtual_row_range(total_rows, row_step, visible_h, scroll_y, overscan) }

fn asset_grid_cols(any win_w, any compact=false) int { catalog.asset_grid_cols(win_w, compact) }

fn asset_grid_usable_w(any win_w, any compact=false) f64 { catalog.asset_grid_usable_w(win_w, compact) }

fn asset_tile_h(any show_paths=false) f64 { catalog.asset_tile_h(show_paths) }

fn asset_grid_content_h(any model_count, any win_w, any compact=false, any show_paths=false) f64 { catalog.asset_grid_content_h(model_count, win_w, compact, show_paths) }

fn asset_grid_fit_h(any model_count, any win_w, any requested_h, any compact=false, any show_paths=false) f64 { catalog.asset_grid_fit_h(model_count, win_w, requested_h, compact, show_paths) }

fn asset_grid_view_h(any requested_h, any compact=false, any standalone=false) f64 { catalog.asset_grid_view_h(requested_h, compact, standalone) }

fn format_name_list(any items) str { catalog.format_name_list(items) }

fn asset_icon_name(any name, any rules=0) str { catalog.asset_icon_name(name, rules) }

fn asset_detail(any name, any loaded=false, any rules=0) str { catalog.asset_detail(name, loaded, rules) }

fn asset_dirs_from_env(any defaults=[], any env_names=[]) list { catalog.asset_dirs_from_env(defaults, env_names) }

fn first_gltf_in_dir(str dir, str prefer="") str { catalog.first_gltf_in_dir(dir, prefer) }

fn gltf_catalog_make(any roots=[]) dict { catalog.gltf_catalog_make(roots) }

fn gltf_catalog_roots(any catalog_or_roots) list { catalog.gltf_catalog_roots(catalog_or_roots) }

fn gltf_catalog_scan(any catalog_or_roots) list { catalog.gltf_catalog_scan(catalog_or_roots) }

fn gltf_catalog_ensure(dict c) bool { catalog.gltf_catalog_ensure(c) }

fn gltf_catalog_resolve(dict c, any spec) str { catalog.gltf_catalog_resolve(c, spec) }

fn gltf_catalog_names(dict c, int limit=24) list { catalog.gltf_catalog_names(c, limit) }

#main {
   def picked = catalog_pick_cache(0, ["Hero", "Lamp"], "Lamp", "")
   assert(catalog_filter_key(" Hero ") == "hero" && picked.get("idx") == 1 && catalog_row_id("a/b:c.d") == "a_b_c_d", "assets catalog basics")
   def filtered = catalog_filter(["Hero", "Lamp"], "amp")
   assert(filtered.len == 1 && filtered.get(0) == "Lamp", "assets filter")
   def scene = {"gpu_parts_count": 3}
   def node = {"name": "Root", "children": [1, 2]}
   assert(scene_part_count(scene) == 3 && hierarchy_node_label(node, 4) == "Root" && hierarchy_node_detail(node) == "2 children" && indent_prefix(2) == "    ", "assets hierarchy")
   def rows = virtual_row_range(100, 10, 25, 15)
   assert(rows.get(0) == 1 && rows.get(1) == 6 && asset_grid_cols(900) == 4 && asset_grid_usable_w(200, true) == 156.0 && asset_tile_h(true) == 66.0, "assets grid basics")
   assert(asset_grid_content_h(6, 600, false, false) > 58.0 && asset_grid_fit_h(100, 600, 120, false, false) == 120.0 && asset_grid_view_h(100, false, false) == 220.0, "assets grid sizing")
   assert(format_name_list(["a", "b"]) == "a, b" && asset_icon_name("camera rig") == "asset_camera", "assets names")
   assert(asset_detail("lamp", false) == "Light rig" && asset_detail("anything", true) == "Loaded scene", "assets detail")
   assert(asset_dirs_from_env(["/definitely/missing"], ["NYTRIX_ASSET_TEST_MISSING"]).len == 0, "assets dir env helper")
   def cat = gltf_catalog_make(["/definitely/missing"])
   assert(gltf_catalog_roots(cat).len == 0 && gltf_catalog_resolve(cat, "") == "" && first_gltf_in_dir("/definitely/missing") == "", "assets gltf catalog")
   print("✓ std.os.ui.assets self-test passed")
}
