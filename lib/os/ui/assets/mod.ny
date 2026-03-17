;; Keywords: assets asset-browser catalog
;; UI asset facade for catalogs and browser data.
module std.os.ui.assets(catalog, catalog_filter_key, catalog_pick_cache, catalog_row_id, catalog_filter, scene_part_count, hierarchy_node_label, hierarchy_node_detail, indent_prefix, virtual_row_range, asset_grid_cols, asset_grid_usable_w, asset_tile_h, asset_grid_content_h, asset_grid_fit_h, asset_grid_view_h, format_name_list, asset_icon_name, asset_detail)
use std.os.ui.assets.catalog as catalog

fn catalog_filter_key(any: filter): str { catalog.catalog_filter_key(filter) }

fn catalog_pick_cache(any: cache, any: items, any: value, any: filter): dict { catalog.catalog_pick_cache(cache, items, value, filter) }

fn catalog_row_id(any: name): str { catalog.catalog_row_id(name) }

fn catalog_filter(any: names, any: filter): list { catalog.catalog_filter(names, filter) }

fn scene_part_count(any: scene_obj): int { catalog.scene_part_count(scene_obj) }

fn hierarchy_node_label(any: node, int: idx): str { catalog.hierarchy_node_label(node, idx) }

fn hierarchy_node_detail(any: node): str { catalog.hierarchy_node_detail(node) }

fn indent_prefix(any: depth): str { catalog.indent_prefix(depth) }

fn virtual_row_range(any: total_rows, any: row_step, any: visible_h, any: scroll_y, any: overscan=3): list { catalog.virtual_row_range(total_rows, row_step, visible_h, scroll_y, overscan) }

fn asset_grid_cols(any: win_w, any: compact=false): int { catalog.asset_grid_cols(win_w, compact) }

fn asset_grid_usable_w(any: win_w, any: compact=false): f64 { catalog.asset_grid_usable_w(win_w, compact) }

fn asset_tile_h(any: show_paths=false): f64 { catalog.asset_tile_h(show_paths) }

fn asset_grid_content_h(any: model_count, any: win_w, any: compact=false, any: show_paths=false): f64 { catalog.asset_grid_content_h(model_count, win_w, compact, show_paths) }

fn asset_grid_fit_h(any: model_count, any: win_w, any: requested_h, any: compact=false, any: show_paths=false): f64 { catalog.asset_grid_fit_h(model_count, win_w, requested_h, compact, show_paths) }

fn asset_grid_view_h(any: requested_h, any: compact=false, any: standalone=false): f64 { catalog.asset_grid_view_h(requested_h, compact, standalone) }

fn format_name_list(any: items): str { catalog.format_name_list(items) }

fn asset_icon_name(any: name, any: rules=0): str { catalog.asset_icon_name(name, rules) }

fn asset_detail(any: name, any: loaded=false, any: rules=0): str { catalog.asset_detail(name, loaded, rules) }
