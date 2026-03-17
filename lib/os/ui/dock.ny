;; Keywords: dock layout docking
;; Docking layout and tool-window state for editor-style UI surfaces.
module std.os.ui.dock(begin_tool, end_tool, show_tool, tool_closed, normalize_tool_ids, merge_tool_ids, visible_tool_ids, tool_snapshot, dump_tools, dump_visible_tools, apply_tool_rect_if_visible, focus_pref_rect, apply_focus_layout, apply_merged_focus_layout, tile_shell, tile_shell_preset)
use std.core
use std.math
use std.os.prim
use std.os.path as ospath
use std.os.sys
use std.parse.data.json as json
use std.core.str as str
use std.core.common as common
use std.os.ui.gui
use std.os.ui.gui as ui_layout

fn _clean_tool_id(any: id): str { str.strip(to_str(id)) }

fn _append_tool_id(list: out, any: id): list {
   def key = _clean_tool_id(id)
   if(key.len == 0 || key == "0"){ return out }
   mut i = 0
   def out_n = out.len
   while(i < out_n){
      if(to_str(out.get(i, "")) == key){ return out }
      i += 1
   }
   out.append(key)
}

fn _merge_tool_ids_into(list: out, any: ids): list {
   if(!ids){ return out }
   if(is_list(ids) || is_tuple(ids) || is_set(ids)){
      mut next = out
      mut i = 0
      def ids_n = ids.len
      while(i < ids_n){
         next = _merge_tool_ids_into(next, ids.get(i, ""))
         i += 1
      }
      return next
   }
   if(is_dict(ids)){
      if(ids.contains("id")){ return _append_tool_id(out, ids.get("id", "")) }
      mut next = out
      def keys = dict_keys(ids)
      mut i = 0
      def keys_n = keys.len
      while(i < keys_n){
         def key = to_str(keys.get(i, ""))
         if(bool(ids.get(key, false))){ next = _append_tool_id(next, key) }
         i += 1
      }
      return next
   }
   _append_tool_id(out, ids)
}

fn normalize_tool_ids(any: ids): list {
   "Returns a stable, de-duplicated list of non-empty editor tool ids."
   _merge_tool_ids_into([], ids)
}

fn merge_tool_ids(any: a, any: b=0, any: c=0, any: d=0): list {
   "Merges several tool id groups into one stable de-duplicated list."
   mut out = []
   out = _merge_tool_ids_into(out, a)
   out = _merge_tool_ids_into(out, b)
   out = _merge_tool_ids_into(out, c)
   out = _merge_tool_ids_into(out, d)
   out
}

fn visible_tool_ids(any: ids): list {
   "Returns normalized tool ids whose GUI window is currently visible."
   def all = normalize_tool_ids(ids)
   mut out = []
   mut i = 0
   def all_n = all.len
   while(i < all_n){
      def key = to_str(all.get(i, ""))
      if(gui.window_visible(key)){ out = out.append(key) }
      i += 1
   }
   out
}

fn _tool_snapshot_row(any: id): dict {
   def key = _clean_tool_id(id)
   def r = gui.window_rect(key)
   return {
      "id": key,
      "visible": gui.window_visible(key),
      "rect": r,
      "x": float(r.get(0, 0.0)),
      "y": float(r.get(1, 0.0)),
      "w": float(r.get(2, 0.0)),
      "h": float(r.get(3, 0.0))
   }
}

fn tool_snapshot(any: ids): list {
   "Returns a JSON-ready snapshot of editor tool visibility and rectangles."
   def all = normalize_tool_ids(ids)
   mut rows = []
   mut i = 0
   def all_n = all.len
   while(i < all_n){
      rows = rows.append(_tool_snapshot_row(all.get(i, "")))
      i += 1
   }
   rows
}

fn _default_dump_path(): str {
   def env_path = common.env_trim("NYTRIX_EDITOR_DUMP_PATH")
   if(env_path.len > 0){ return env_path }
   "build/release/editor_dump.json"
}

fn _write_text_file(str: path, str: content): bool {
   def p = ospath.normalize(path)
   match sys_open(p, 577, 420){
      ok(fd) -> {
         defer { sys_close_quiet(fd) }
         def n = content.len
         mut off = 0
         while(off < n){
            match sys_write(fd, to_int(content) + off, n - off){
               ok(w) -> {
                  if(w <= 0){ return false }
                  off += w
               }
               err(ignorederr) -> { ignorederr  return false }
            }
         }
         true
      }
      err(ignorederr) -> { ignorederr  false }
   }
}

fn dump_tools(any: ids, str: dump_path=""): list {
   "Writes editor tool state JSON and returns the snapshot that was written."
   def p = str.strip(to_str(dump_path)).len > 0 ? to_str(dump_path) : _default_dump_path()
   def rows = tool_snapshot(ids)
   _ = _write_text_file(p, json.json_encode(rows) + "\n")
   rows
}

fn dump_visible_tools(any: ids, str: dump_path=""): list {
   "Writes a JSON dump containing only visible editor tools."
   dump_tools(visible_tool_ids(ids), dump_path)
}

fn show_tool(any: id, bool: visible=true): bool {
   def key = to_str(id)
   if(key.len == 0){ return false }
   gui.show_window(key, !!visible)
   !!visible
}

fn tool_closed(any: id): bool {
   def key = to_str(id)
   if(key.len == 0){ return false }
   def closed = gui.window_closed(key)
   if(closed){ gui.show_window(key, false) }
   closed
}

fn begin_tool(any: id, bool: visible, any: title, f64: x, f64: y, f64: w, f64: h, any: opts=0): list {
   def key = to_str(id)
   if(key.len == 0){ return [false, false] }
   def shown = bool(visible)
   gui.show_window(key, shown)
   if(!shown){ return [false, false] }
   def body = gui.begin_window(key, title, x, y, w, h, opts)
   if(tool_closed(key)){ return [false, true] }
   [body, false]
}

fn end_tool(): any { gui.end_window() }

fn apply_tool_rect_if_visible(any: id, bool: visible, list: r): any { ui_layout.apply_window_rect_if_visible(id, visible, r) }

fn focus_pref_rect(any: id, list: root): list {
   def rid = to_str(id)
   def rx = float(root.get(0, 0.0))
   def ry = float(root.get(1, 0.0))
   def rw = float(root.get(2, 0.0))
   def rh = float(root.get(3, 0.0))
   mut ww = clamp(rw * 0.72, 520.0, 1040.0)
   mut hh = clamp(rh * 0.78, 340.0, 920.0)
   if(rid == "editor_main"){
      ww = clamp(rw * 0.82, 900.0, 1520.0)
      hh = clamp(rh * 0.92, 680.0, 1020.0)
   } elif(rid == "profiler"){
      ww = clamp(rw * 0.82, 820.0, 1320.0)
      hh = clamp(rh * 0.76, 560.0, 860.0)
   } elif(rid == "asset_browser"){
      ww = rw
      hh = clamp(rh * 0.62, 520.0, 760.0)
      return [rx, ry + max(0.0, rh - hh), ww, hh]
   } elif(rid == "widget_gallery"){
      ww = clamp(rw * 0.62, 720.0, 1080.0)
      hh = clamp(rh * 0.68, 520.0, 780.0)
      return [rx + max(0.0, (rw - ww) * 0.5), ry + max(0.0, (rh - hh) * 0.34), ww, hh]
   } elif(rid == "node_graph"){
      ww = clamp(rw * 0.70, 640.0, 1040.0)
      hh = clamp(rh * 0.68, 460.0, 760.0)
   } elif(rid == "inspector"){
      ww = clamp(rw * 0.46, 620.0, 920.0)
      hh = rh
      return [rx + max(0.0, (rw - ww) * 0.5), ry, ww, hh]
   } elif(rid == "workspace_grid"){
      ww = clamp(rw * 0.78, 760.0, 1180.0)
      hh = clamp(rh * 0.70, 500.0, 800.0)
   } elif(rid == "widget_probe"){
      ww = clamp(rw * 0.42, 560.0, 760.0)
      hh = clamp(rh * 0.46, 380.0, 560.0)
      return [rx + max(0.0, (rw - ww) * 0.5), ry + max(0.0, (rh - hh) * 0.32), ww, hh]
   }
   ui_layout.center_rect(root, ww, hh)
}

fn _focus_pair_weights(str: a, str: b): list {
   if((a == "workspace_grid" && b == "node_graph") || (a == "node_graph" && b == "workspace_grid")){
      if(a == "workspace_grid"){ return [0.44, 0.56] }
      return [0.56, 0.44]
   }
   if((a == "asset_browser" && b == "widget_probe") || (a == "widget_probe" && b == "asset_browser")){
      if(a == "asset_browser"){ return [0.62, 0.38] }
      return [0.38, 0.62]
   }
   if((a == "inspector" && b == "asset_browser") || (a == "asset_browser" && b == "inspector")){
      if(a == "asset_browser"){ return [0.58, 0.42] }
      return [0.42, 0.58]
   }
   if((a == "profiler" && b == "widget_gallery") || (a == "widget_gallery" && b == "profiler")){
      if(a == "profiler"){ return [0.56, 0.44] }
      return [0.44, 0.56]
   }
   [1.0, 1.0]
}

fn _ones(int: count): list {
   mut out = []
   mut i = 0
   while(i < count){
      out = out.append(1.0)
      i += 1
   }
   out
}

fn apply_focus_layout(list: ids, f64: ww, f64: wh, f64: gap=14.0): int {
   def g = max(2.0, float(gap))
   def root = ui_layout.rect(g, g, max(0.0, float(ww) - g * 2.0), max(0.0, float(wh) - g * 2.0))
   def n = ids.len
   if(n <= 0){ return 0 }
   if(n == 1){
      def id0 = to_str(ids.get(0, ""))
      ui_layout.apply_window_rect(id0, focus_pref_rect(id0, root))
      return 1
   }
   if(n == 2){
      def a, b = to_str(ids.get(0, "")), to_str(ids.get(1, ""))
      def cols = ui_layout.split_cols(root, _focus_pair_weights(a, b), g)
      ui_layout.apply_window_rect(a, cols.get(0, root))
      ui_layout.apply_window_rect(b, cols.get(1, root))
      return 2
   }
   if(n == 3){
      if(to_str(ids.get(0, "")) == "profiler" && to_str(ids.get(1, "")) == "inspector" && to_str(ids.get(2, "")) == "widget_probe"){
         def cols3 = ui_layout.split_cols(root, [0.54, 0.46], g)
         def right_rows3 = ui_layout.split_rows(cols3.get(1, root), [0.74, 0.26], g)
         ui_layout.apply_window_rect("profiler", cols3.get(0, root))
         ui_layout.apply_window_rect("inspector", right_rows3.get(0, root))
         ui_layout.apply_window_rect("widget_probe", right_rows3.get(1, root))
         return 3
      }
      def rows = ui_layout.split_rows(root, [0.56, 0.44], g)
      def cols = ui_layout.split_cols(rows.get(0, root), [1.0, 1.0], g)
      ui_layout.apply_window_rect(to_str(ids.get(0, "")), cols.get(0, root))
      ui_layout.apply_window_rect(to_str(ids.get(1, "")), cols.get(1, root))
      ui_layout.apply_window_rect(to_str(ids.get(2, "")), rows.get(1, root))
      return 3
   }
   if(n == 4){
      def rows = ui_layout.split_rows(root, [1.0, 1.0], g)
      def top = ui_layout.split_cols(rows.get(0, root), [1.0, 1.0], g)
      def bot = ui_layout.split_cols(rows.get(1, root), [1.0, 1.0], g)
      ui_layout.apply_window_rect(to_str(ids.get(0, "")), top.get(0, root))
      ui_layout.apply_window_rect(to_str(ids.get(1, "")), top.get(1, root))
      ui_layout.apply_window_rect(to_str(ids.get(2, "")), bot.get(0, root))
      ui_layout.apply_window_rect(to_str(ids.get(3, "")), bot.get(1, root))
      return 4
   }
   def cols = ui_layout.split_cols(root, [1.0, 1.0], g)
   def left_n = int((n + 1) / 2)
   def right_n = max(1, n - left_n)
   def left_rows = ui_layout.split_rows(cols.get(0, root), _ones(left_n), g)
   def right_rows = ui_layout.split_rows(cols.get(1, root), _ones(right_n), g)
   mut i = 0
   while(i < left_n && i < ids.len){
      ui_layout.apply_window_rect(to_str(ids.get(i, "")), left_rows.get(i, root))
      i += 1
   }
   mut rj = 0
   while(rj < right_n && left_n + rj < ids.len){
      ui_layout.apply_window_rect(to_str(ids.get(left_n + rj, "")), right_rows.get(rj, root))
      rj += 1
   }
   n
}

fn apply_merged_focus_layout(any: a, any: b, f64: ww, f64: wh, f64: gap=14.0): int {
   "Merges two tool groups, filters hidden windows, then applies the focus layout."
   apply_focus_layout(visible_tool_ids(merge_tool_ids(a, b)), ww, wh, gap)
}

fn tile_shell(f64: ww, f64: wh, f64: gap=14.0, f64: left_ratio=0.30, f64: top_ratio=0.33, f64: mid_ratio=0.24): dict { ui_layout.tile_editor_shell(ww, wh, gap, left_ratio, top_ratio, mid_ratio) }

fn tile_shell_preset(str: name, f64: ww, f64: wh, f64: gap=14.0): dict { ui_layout.tile_editor_shell_preset(name, ww, wh, gap) }
