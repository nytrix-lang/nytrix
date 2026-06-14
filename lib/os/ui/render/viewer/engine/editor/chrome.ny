;; Keywords: engine editor chrome panels toolbar embedded os ui render viewer scene
;; Chrome and panel helpers for the embedded engine editor surface.
;; References:
;; - std.os.ui.render.viewer.editor.mod
;; - std.os.ui.render.viewer.engine.editor.mod
module std.os.ui.render.viewer.engine.editor.chrome(EDITOR_TAB_ITEMS, SHOT_PRESET_ITEMS, graph_node, graph_nodes, graph_links, selected_graph_node, standalone_now, standalone_rect, visibility_changed, visibility_snapshot, chrome_metrics, draw_header, draw_footer, draw_style_tab, draw_layout_tab, draw_graph_body)
use std.core
use std.math (clamp)
use std.os.ui.render.viewer.gui as gui
use std.os.ui.render.viewer.editor.colorpicker as colorpicker
use std.os.ui.render.viewer.app as ui_app
use std.os.ui.render.dump as ui_profile

def EDITOR_TAB_ITEMS = ["Catalog", "Hierarchy", "Theme", "Console"]
def SHOT_PRESET_ITEMS = [
   "full_editor",
   "full_editor_workspace",
   "editor_catalog",
   "editor_hierarchy",
   "editor_scene",
   "editor_scene_compact",
   "editor_view",
   "editor_probe",
   "gallery_basics",
   "browser_catalog",
   "inspector_scene", "inspector_settings",
   "graph", "profiler"
]

fn graph_node(any title, any x, any y, any inputs, any outputs, any selected=false, any w=180.0) dict { ui_app.app_graph_node(title, x, y, inputs, outputs, selected, w) }

fn graph_nodes() list {
   "Runs the graph nodes operation."
   [
      graph_node("Asset Source", 28.0, 28.0, [], ["Mesh", "Textures", "Anim"], true, 188.0),
      graph_node("Scene Build", 286.0, 46.0, ["Mesh", "Textures", "Anim"], ["Drawables"], false, 198.0),
      graph_node("Animation", 28.0, 184.0, ["Anim"], ["Skinned"], false, 176.0),
      graph_node("Visibility", 548.0, 58.0, ["Drawables", "Skinned"], ["Visible"], false, 186.0),
      graph_node("Lighting", 548.0, 212.0, ["Visible"], ["Lit Color"], false, 176.0),
      graph_node("Post / Present", 806.0, 140.0, ["Lit Color"], ["Frame"], false, 194.0)
   ]
}

fn graph_links() list {
   "Runs the graph links operation."
   [
      [0, 0, 1, 0],
      [0, 1, 1, 1],
      [0, 2, 2, 0],
      [1, 0, 3, 0],
      [2, 0, 3, 1],
      [3, 0, 4, 0],
      [4, 0, 5, 0]
   ]
}

fn selected_graph_node(any nodes) any {
   "Runs the selected graph node operation."
   mut i = 0
   def n = is_list(nodes) ? nodes.len : 0
   while i < n {
      def node = nodes.get(i, 0)
      if is_dict(node) && bool(node.get("selected", false)) { return node }
      i += 1
   }
   0
}

fn standalone_now(any show_editor, any workspace_mode, any show_gallery, any show_probe, any show_browser, any show_inspector, any show_profiler, any show_workspace, any show_graph) bool {
   "Runs the standalone now operation."
   bool(show_editor) && int(workspace_mode) != 1 &&
   !bool(show_gallery) && !bool(show_probe) && !bool(show_browser) &&
   !bool(show_inspector) && !bool(show_profiler) && !bool(show_workspace) && !bool(show_graph)
}

fn standalone_rect(any preset, any win_w, any win_h, any layout_gap) list {
   "Runs the standalone rect operation."
   def root_w = max(0.0, float(win_w))
   def root_h = max(0.0, float(win_h))
   [0.0, 0.0, root_w, root_h]
}

fn visibility_snapshot(any gallery, any probe, any browser, any profiler, any workspace, any graph, any inspector) list {
   "Packs editor tool visibility into a stable comparison row."
   [bool(gallery), bool(probe), bool(browser), bool(profiler), bool(workspace), bool(graph), bool(inspector)]
}

fn visibility_changed(any prev, any gallery, any probe, any browser, any profiler, any workspace, any graph, any inspector) bool {
   "Returns true when any editor tool visibility bit changed."
   def row = is_list(prev) ? prev : []
   row.get(0, false) != bool(gallery) ||
   row.get(1, false) != bool(probe) ||
   row.get(2, false) != bool(browser) ||
   row.get(3, false) != bool(profiler) ||
   row.get(4, false) != bool(workspace) ||
   row.get(5, false) != bool(graph) ||
   row.get(6, false) != bool(inspector)
}

fn chrome_metrics(any editor_w, any editor_h) dict {
   "Runs the chrome metrics operation."
   def w, h = float(editor_w), float(editor_h)
   def compact = w < 620.0 || h < 430.0
   def dense = compact || w < 1180.0 || h < 700.0
   {
      "compact": compact,
      "dense": dense,
      "compact_header": compact || h < 420.0,
      "summary_cols": compact ? 1 : (dense ? 2 : 3),
      "toolbar_w": compact ? 30.0 : (dense ? 34.0 : 68.0),
      "toolbar_w_console": compact ? 30.0 : (dense ? 34.0 : 78.0)
   }
}

fn _footer_chars(any editor_w) int {
   def w = float(editor_w)
   if w <= 0.0 { return 96 }
   int(clamp((w - 36.0) / 10.0, 24.0, 160.0))
}

fn _ellipsize_middle(any s, any max_chars) str {
   def txt = to_str(s)
   def limit = int(max_chars)
   if limit <= 0 { return "" }
   if txt.len <= limit { return txt }
   if limit <= 6 { return str.str_slice(txt, 0, max(0, limit)) }
   def keep = max(1, limit - 3)
   def head = max(3, keep / 2)
   def tail = max(1, keep - head)
   str.str_slice(txt, 0, head) + "..." + str.str_slice(txt, max(0, txt.len - tail), txt.len)
}

fn _basename(any path) str {
   def s = to_str(path)
   if s.len <= 0 { return "" }
   mut i = s.len - 1
   while i >= 0 {
      def c = load8(s, i)
      case c {
         47, 92 -> { return str.str_slice(s, i + 1, s.len) }
         _ -> {}
      }
      i -= 1
   }
   s
}

fn _txt(any value, str fallback="") str {
   if value == nil { return fallback }
   def out = to_str(value)
   out == "<nil>" ? fallback : out
}

fn _footer_path(any path, any max_chars) str {
   def p = to_str(path)
   def limit = int(max_chars)
   if p.len <= limit { return p }
   def base = _basename(p)
   if base.len > 0 {
      def compact = ".../" + base
      if compact.len <= limit { return compact }
      return ".../" + _ellipsize_middle(base, max(4, limit - 4))
   }
   _ellipsize_middle(p, limit)
}

fn _toolbar_button(any id, any icon, any label, any w, any active) bool { gui.icon_button(id, icon, label, float(w), 28.0, bool(active)) }

fn _stat_card(any id, any title, any value, any subtitle, any w, any h, list color) any {
   gui.stat_card(id, title, value, subtitle, float(w), float(h), color)
}

fn draw_header(any scene_name, any fps_value, any layout_name, any active_shot, any tab, any editor_w, any editor_h, any card_w, any scene_icon, any hierarchy_icon, any theme_icon, any console_icon, dict renderer_stats, any renderer_hotspot) int {
   "Draws draw header."
   def metrics = chrome_metrics(editor_w, editor_h)
   def dense = bool(metrics.get("dense", false))
   def compact = bool(metrics.get("compact", false))
   def compact_header = bool(metrics.get("compact_header", false))
   def toolbar_w = float(metrics.get("toolbar_w", 74.0))
   def toolbar_w_console = float(metrics.get("toolbar_w_console", 86.0))
   def shown_scene = (to_str(scene_name).len > 0) ? to_str(scene_name) : "No scene"
   def layout_now = to_str(layout_name)
   mut selected = int(tab)
   if compact_header {
      gui.text_colored(shown_scene + "  " + to_str(int(fps_value)) + " fps", [0.74, 0.74, 0.74, 0.92])
   } else {
      gui.text_colored("Nytrix Editor", [0.94, 0.94, 0.94, 0.98])
      gui.text_colored(shown_scene + "  " + to_str(int(fps_value)) + " fps  " + layout_now, [0.66, 0.66, 0.66, 0.90])
   }
   if _toolbar_button("toolbar_catalog", scene_icon, dense ? "" : "Catalog", dense ? toolbar_w : 76.0, selected == 0) { selected = 0 }
   gui.same_line()
   if _toolbar_button("toolbar_hierarchy", hierarchy_icon, dense ? "" : "Hierarchy", dense ? toolbar_w : 88.0, selected == 1) { selected = 1 }
   gui.same_line()
   if _toolbar_button("toolbar_theme", theme_icon, dense ? "" : "Theme", toolbar_w, selected == 2) { selected = 2 }
   gui.same_line()
   if _toolbar_button("toolbar_console", console_icon, dense ? "" : "Console", toolbar_w_console, selected == 3) { selected = 3 }
   if compact_header {
      return selected
   }
   def summary_cols = int(metrics.get("summary_cols", 1))
   if dense {
      def hotspot = _txt(renderer_hotspot, "steady")
      gui.text_colored(
         to_str(int(renderer_stats.get("draws", 0))) + " draws  " +
         hotspot + "  " +
         _ellipsize_middle(active_shot, 28),
      [0.68, 0.68, 0.68, 0.92])
      return selected
   }
   _stat_card("editor_scene_card",
      "Scene",
      (to_str(scene_name).len > 0) ? to_str(scene_name) : "<none>",
      "layout " + layout_now,
      float(card_w), 50.0,
   [0.86, 0.86, 0.86, 0.96])
   if summary_cols > 1 { gui.same_line() }
   def frame_ms = float(renderer_stats.get("frame_ms", 0.0))
   _stat_card("editor_runtime_card",
      "Runtime",
      to_str(int(fps_value)) + " fps",
      f"{frame_ms:.2f}ms frame",
      float(card_w), 50.0,
   [0.74, 0.74, 0.74, 0.94])
   if summary_cols > 2 { gui.same_line() }
   if !compact {
      def hotspot = _txt(renderer_hotspot, "steady")
      _stat_card("editor_renderer_card",
         "Renderer",
         to_str(int(renderer_stats.get("draws", 0))) + " draws",
         hotspot,
         float(card_w), 50.0,
      [0.82, 0.82, 0.82, 0.94])
   }
   gui.text_colored("Shot: " + to_str(active_shot), [0.66, 0.66, 0.66, 0.90])
   selected
}

fn draw_footer(any last_dump_path, any last_probe_text, bool trace=false, any editor_w=0.0) any {
   "Draws draw footer."
   if trace { ui_profile.print_text("[ui:gui-editor] footer") }
   if gui.remaining_h(0.0) < 18.0 { return 0 }
   gui.separator()
   if trace { ui_profile.print_text("[ui:gui-editor] footer_sep") }
   def budget = _footer_chars(editor_w)
   def dump_path = to_str(last_dump_path)
   if dump_path.len > 0 && gui.remaining_h(0.0) >= 16.0 {
      if trace { ui_profile.print_text("[ui:gui-editor] footer_dump") }
      gui.text_colored("Last dump: " + _footer_path(dump_path, max(8, budget - 11)), [0.66, 0.66, 0.66, 0.92])
   }
   if trace { ui_profile.print_text("[ui:gui-editor] footer_probe_check") }
   def probe_text = to_str(last_probe_text)
   if probe_text.len > 0 && gui.remaining_h(0.0) >= 16.0 {
      if trace { ui_profile.print_text("[ui:gui-editor] footer_probe") }
      gui.text_colored("Probe: " + _ellipsize_middle(probe_text, max(8, budget - 7)), [0.72, 0.72, 0.72, 0.94])
   }
}

fn draw_style_tab(dict st) dict {
   "Draws draw style tab."
   mut out = st
   out["scale"] = gui.slider_float("ui_scale", "UI Scale", float(st.get("scale", 1.0)), 0.70, 1.80)
   out["gap"] = float(gui.slider_int("layout_gap", "Panel Gap", int(st.get("gap", 0.0)), 0, 16))
   def bg0 = colorpicker.rgba(st.get("bg", [0.0, 0.0, 0.0, 1.0]), [0.0, 0.0, 0.0, 1.0])
   def accent0 = colorpicker.rgba(st.get("accent", [0.76, 0.76, 0.76, 1.0]), [0.76, 0.76, 0.76, 1.0])
   out["bg"] = colorpicker.edit4("clear_color", "Clear Color", bg0, [0.0, 0.0, 0.0, 1.0])
   out["accent"] = colorpicker.edit4("accent_color", "UI Accent", accent0, [0.76, 0.76, 0.76, 1.0])
   out["bg_changed"] = colorpicker.changed(bg0, out.get("bg", bg0))
   out["accent_changed"] = colorpicker.changed(accent0, out.get("accent", accent0))
   out
}

fn draw_layout_tab(dict st) list {
   "Draws draw layout tab."
   def dense = bool(st.get("dense", false))
   def compact_header = bool(st.get("compact_header", false))
   def compact = bool(st.get("compact", false))
   def layout_items = st.get("layout_items", [])
   def shot_items = st.get("shot_items", [])
   def slot_items = st.get("slot_items", [])
   def layout_now = to_str(st.get("layout_now", ""))
   def active_shot = to_str(st.get("active_shot", ""))
   mut probe_overlay = bool(st.get("probe_overlay", false))
   mut layout_name = to_str(st.get("layout_name", layout_now))
   mut shot_name = to_str(st.get("shot_name", active_shot))
   mut slot_idx = int(st.get("slot_idx", 0))
   mut layout_dirty = false
   mut action = ""
   probe_overlay = gui.checkbox("probe_overlay", (dense || compact_header) ? "Debug" : "Show GUI Debug Overlay", probe_overlay)
   gui.set_debug_overlay(probe_overlay)
   def layout_idx = max(0, ui_app.app_list_find_text(layout_items, layout_now))
   def next_layout_idx = gui.combo_box("layout_preset", "Layout Preset", layout_items, layout_idx, 0.0, 4)
   if next_layout_idx >= 0 && next_layout_idx != layout_idx {
      layout_name = to_str(layout_items.get(next_layout_idx, layout_now))
      layout_dirty = true
   }
   def shot_idx = max(0, ui_app.app_list_find_text(shot_items, active_shot))
   def next_shot_idx = gui.combo_box("shot_preset", "GUI Shot", shot_items, shot_idx, 0.0, 6)
   if next_shot_idx >= 0 && next_shot_idx != shot_idx {
      shot_name = to_str(shot_items.get(next_shot_idx, active_shot))
   }
   slot_idx = gui.combo_box("layout_slot", "Layout Slot", slot_items, slot_idx, 0.0, 4)
   if gui.button("apply_shot", compact ? "Shot" : "Apply", compact ? 68.0 : 76.0) { action = "apply_shot" }
   gui.same_line()
   if gui.button("layout_tile", "Tile", compact ? 60.0 : 66.0) { action = "retile" }
   gui.same_line()
   if gui.button("layout_save", "Save", compact ? 60.0 : 66.0) { action = "save_slot" }
   gui.same_line()
   if gui.button("layout_load", "Load", compact ? 60.0 : 66.0) { action = "load_slot" }
   gui.separator()
   if gui.button("dump_shot", "Shot", 66.0) { action = "dump_shot" }
   gui.same_line()
   if gui.button("frame_dump", "Frame", 70.0) { action = "frame_dump" }
   gui.same_line()
   if gui.button("graph_reset_shell", compact ? "Graph" : "Reset Graph", compact ? 70.0 : 94.0) { action = "reset_graph" }
   if gui.button("probe_now", "Probe", 68.0) { action = "probe" }
   gui.same_line()
   if gui.button("focus_probe", compact ? "Print" : "Print Probe", compact ? 68.0 : 90.0) { action = "print_probe" }
   [probe_overlay, layout_name, shot_name, slot_idx, layout_dirty, action]
}

fn draw_graph_body(any nodes, any links, any selected, any card_w, any canvas_h, any grid) list {
   "Draws draw graph body."
   _stat_card("graph_nodes_card",
      "Nodes",
      to_str(is_list(nodes) ? nodes.len : 0),
      "graph blocks",
      float(card_w), 58.0,
   [0.84, 0.84, 0.84, 1.0])
   gui.same_line()
   _stat_card("graph_links_card",
      "Links",
      to_str(is_list(links) ? links.len : 0),
      "edges",
      float(card_w), 58.0,
   [0.76, 0.76, 0.76, 1.0])
   gui.same_line()
   def sel_title = is_dict(selected) ? to_str(selected.get("title", "Node")) : "<none>"
   def sel_hint = is_dict(selected) ? "live details below" : "click a node header"
   _stat_card("graph_selected_card",
      "Selected",
      sel_title,
      sel_hint,
      float(card_w), 58.0,
   [0.78, 0.78, 0.78, 1.0])
   mut out = gui.node_canvas("editor_graph",
      is_list(nodes) ? nodes : [],
      is_list(links) ? links : [],
      0.0,
      float(canvas_h),
   float(grid))
   if is_dict(selected) {
      gui.separator()
      gui.text("Selected: " + to_str(selected.get("title", "Node")))
      gui.text("Inputs: " + to_str(selected.get("inputs", []).len) + "  Outputs: " + to_str(selected.get("outputs", []).len))
   }
   out
}

#main {
   def nodes, links = graph_nodes(), graph_links()
   assert(nodes.len == 6, "editor chrome node count")
   assert(nodes[0].get("title", "") == "Asset Source" && nodes[0].get("selected", false), "editor chrome selected source")
   assert(links.len == 7 && links[0] == [0, 0, 1, 0], "editor chrome links")
   def selected = selected_graph_node(nodes)
   assert(is_dict(selected) && selected.get("title", "") == "Asset Source", "editor chrome selected node")
   assert(selected_graph_node([]) == 0, "editor chrome empty selection")
   def custom = graph_node("Probe", 1.0, 2.0, ["in"], ["out"], true, 222.0)
   assert(custom.get("title", "") == "Probe" && custom.get("inputs", []).len == 1 && custom.get("outputs", []).len == 1, "editor chrome custom node")
   assert(standalone_now(true, 0, false, false, false, false, false, false, false) && !standalone_now(true, 1, false, false, false, false, false, false, false) && !standalone_now(true, 0, true, false, false, false, false, false, false), "editor chrome standalone policy")
   def vis = visibility_snapshot(false, false, true, false, false, false, false)
   assert(!visibility_changed(vis, false, false, true, false, false, false, false) && visibility_changed(vis, true, false, true, false, false, false, false), "editor chrome visibility")
   def compact_rect = standalone_rect("compact", 1200.0, 800.0, 10.0)
   assert(compact_rect.len == 4 && float(compact_rect[0]) == 0.0 && float(compact_rect[2]) == 1200.0 && float(compact_rect[3]) == 800.0, "editor chrome compact rect")
   def regular_rect = standalone_rect("regular", 1600.0, 1000.0, 12.0)
   assert(float(regular_rect[0]) == 0.0 && float(regular_rect[1]) == 0.0 && float(regular_rect[2]) == 1600.0, "editor chrome regular rect")
   def compact_metrics = chrome_metrics(420.0, 500.0)
   assert(compact_metrics.get("compact", false) && compact_metrics.get("summary_cols", 0) == 1, "editor chrome compact metrics")
   def dense_metrics = chrome_metrics(620.0, 500.0)
   assert(!dense_metrics.get("compact", true) && dense_metrics.get("dense", false) && dense_metrics.get("summary_cols", 0) == 2, "editor chrome dense metrics")
   assert(chrome_metrics(1440.0, 800.0).get("summary_cols", 0) == 3, "editor chrome wide metrics")
   print("✓ std.os.ui.render.viewer.engine.editor.chrome self-test passed")
}
