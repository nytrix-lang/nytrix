;; Keywords: editor chrome
;; Editor chrome widgets and graph-pane controls for UI tooling.
module std.os.ui.editor.chrome(EDITOR_TAB_ITEMS, SHOT_PRESET_ITEMS, graph_node, graph_nodes, graph_links, selected_graph_node, standalone_now, standalone_rect, chrome_metrics, draw_header, draw_footer, draw_graph_body)
use std.core
use std.math (clamp)
use std.os.ui.gui as gui
use std.os.ui.app as ui_app
use std.os.ui.profile as ui_profile

def EDITOR_TAB_ITEMS = ["Scene", "View", "Theme", "Console", "Probe"]
def SHOT_PRESET_ITEMS = [
   "full_editor",
   "full_editor_workspace",
   "editor_scene",
   "editor_scene_compact",
   "editor_view",
   "editor_probe",
   "gallery_basics",
   "browser_catalog",
   "inspector_scene", "inspector_settings",
   "graph", "profiler"
]

fn graph_node(any: title, any: x, any: y, any: inputs, any: outputs, any: selected=false, any: w=180.0): dict { ui_app.app_graph_node(title, x, y, inputs, outputs, selected, w) }

fn graph_nodes(): list {
   [
      graph_node("Asset Source", 28.0, 28.0, [], ["Mesh", "Textures", "Anim"], true, 188.0),
      graph_node("Scene Build", 286.0, 46.0, ["Mesh", "Textures", "Anim"], ["Drawables"], false, 198.0),
      graph_node("Animation", 28.0, 184.0, ["Anim"], ["Skinned"], false, 176.0),
      graph_node("Visibility", 548.0, 58.0, ["Drawables", "Skinned"], ["Visible"], false, 186.0),
      graph_node("Lighting", 548.0, 212.0, ["Visible"], ["Lit Color"], false, 176.0),
      graph_node("Post / Present", 806.0, 140.0, ["Lit Color"], ["Frame"], false, 194.0)
   ]
}

fn graph_links(): list {
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

fn selected_graph_node(any: nodes): any {
   mut i = 0
   def n = is_list(nodes) ? nodes.len : 0
   while(i < n){
      def node = nodes.get(i, 0)
      if(is_dict(node) && bool(node.get("selected", false))){ return node }
      i += 1
   }
   0
}

fn standalone_now(any: show_editor, any: workspace_mode, any: show_gallery, any: show_probe, any: show_browser, any: show_inspector, any: show_profiler, any: show_workspace, any: show_graph): bool {
   bool(show_editor) && int(workspace_mode) != 1 &&
   !bool(show_gallery) && !bool(show_probe) && !bool(show_browser) &&
   !bool(show_inspector) && !bool(show_profiler) && !bool(show_workspace) && !bool(show_graph)
}

fn standalone_rect(any: preset, any: win_w, any: win_h, any: layout_gap): list {
   def gap = max(2.0, float(layout_gap))
   def root_w = max(0.0, float(win_w) - gap * 2.0)
   def root_h = max(0.0, float(win_h) - gap * 2.0)
   def compact = to_str(preset) == "compact"
   def max_w = min(compact ? 1380.0 : 1520.0, root_w)
   def min_w = min(compact ? 760.0 : 900.0, max_w)
   def min_h = min(compact ? 620.0 : 680.0, root_h)
   def ew, eh = clamp(root_w * (compact ? 0.76 : 0.82), min_w, max_w),
   clamp(root_h * (compact ? 0.88 : 0.92), min_h, root_h)
   [
      gap + max(0.0, (root_w - ew) * 0.5),
      gap + max(0.0, (root_h - eh) * 0.24),
      ew,
      eh
   ]
}

fn chrome_metrics(any: editor_w, any: editor_h): dict {
   def w, h = float(editor_w), float(editor_h)
   def compact = w < 500.0
   def dense = w < 660.0
   {
      "compact": compact,
      "dense": dense,
      "compact_header": compact || h < 360.0,
      "summary_cols": compact ? 1 : (dense ? 2 : 3),
      "toolbar_w": dense ? 42.0 : 92.0,
      "toolbar_w_console": dense ? 42.0 : 112.0
   }
}

fn _footer_chars(any: editor_w): int {
   def w = float(editor_w)
   if(w <= 0.0){ return 96 }
   int(clamp((w - 36.0) / 10.0, 24.0, 160.0))
}

fn _ellipsize_middle(any: s, any: max_chars): str {
   def txt = to_str(s)
   def limit = int(max_chars)
   if(limit <= 0){ return "" }
   if(txt.len <= limit){ return txt }
   if(limit <= 6){ return str.str_slice(txt, 0, max(0, limit)) }
   def keep = max(1, limit - 3)
   def head = max(3, keep / 2)
   def tail = max(1, keep - head)
   str.str_slice(txt, 0, head) + "..." + str.str_slice(txt, max(0, txt.len - tail), txt.len)
}

fn _basename(any: path): str {
   def s = to_str(path)
   if(s.len <= 0){ return "" }
   mut i = s.len - 1
   while(i >= 0){
      def c = load8(s, i)
      case c {
         47, 92 -> { return str.str_slice(s, i + 1, s.len) }
         _ -> {}
      }
      i -= 1
   }
   s
}

fn _footer_path(any: path, any: max_chars): str {
   def p = to_str(path)
   def limit = int(max_chars)
   if(p.len <= limit){ return p }
   def base = _basename(p)
   if(base.len > 0){
      def compact = ".../" + base
      if(compact.len <= limit){ return compact }
      return ".../" + _ellipsize_middle(base, max(4, limit - 4))
   }
   _ellipsize_middle(p, limit)
}

fn _toolbar_button(any: id, any: icon, any: label, any: w, any: active): bool { gui.icon_button(id, icon, label, float(w), 34.0, bool(active)) }

fn _stat_card(any: id, any: title, any: value, any: subtitle, any: w, any: h, list: color): any {
   gui.stat_card(id, title, value, subtitle, float(w), float(h), color)
}

fn draw_header(any: scene_name, any: fps_value, any: layout_name, any: active_shot, any: tab, any: editor_w, any: editor_h, any: card_w, any: scene_icon, any: view_icon, any: theme_icon, any: console_icon, any: probe_icon, dict: renderer_stats, any: renderer_hotspot): int {
   def metrics = chrome_metrics(editor_w, editor_h)
   def dense = bool(metrics.get("dense", false))
   def compact = bool(metrics.get("compact", false))
   def compact_header = bool(metrics.get("compact_header", false))
   def toolbar_w = float(metrics.get("toolbar_w", 74.0))
   def toolbar_w_console = float(metrics.get("toolbar_w_console", 86.0))
   def shown_scene = (to_str(scene_name).len > 0) ? to_str(scene_name) : "No scene"
   def layout_now = to_str(layout_name)
   mut selected = int(tab)
   gui.text_colored("Nytrix Editor", [0.94, 0.94, 0.94, 0.98])
   gui.text_colored(shown_scene + "  " + to_str(int(fps_value)) + " fps  " + layout_now, [0.66, 0.66, 0.66, 0.90])
   if(_toolbar_button("toolbar_scene", scene_icon, dense ? "" : "Scene", toolbar_w, selected == 0)){ selected = 0 }
   gui.same_line()
   if(_toolbar_button("toolbar_view", view_icon, dense ? "" : "View", dense ? toolbar_w : 82.0, selected == 1)){ selected = 1 }
   gui.same_line()
   if(_toolbar_button("toolbar_theme", theme_icon, dense ? "" : "Theme", toolbar_w, selected == 2)){ selected = 2 }
   gui.same_line()
   if(_toolbar_button("toolbar_console", console_icon, dense ? "" : "Console", toolbar_w_console, selected == 3)){ selected = 3 }
   gui.same_line()
   if(_toolbar_button("toolbar_probe", probe_icon, dense ? "" : "Probe", dense ? toolbar_w : 90.0, selected == 4)){ selected = 4 }
   if(compact_header){
      gui.text_colored("Shot: " + to_str(active_shot) + "   layout " + layout_now, [0.66, 0.66, 0.66, 0.90])
      return selected
   }
   def summary_cols = int(metrics.get("summary_cols", 1))
   if(dense){
      gui.text_colored(
         "Scene " + shown_scene +
         "   " + to_str(int(fps_value)) + " fps" +
         "   " + to_str(int(renderer_stats.get("draws", 0))) + " draws" +
         "   shot " + to_str(active_shot),
      [0.68, 0.68, 0.68, 0.92])
      return selected
   }
   _stat_card("editor_scene_card",
      "Scene",
      (to_str(scene_name).len > 0) ? to_str(scene_name) : "<none>",
      "layout " + layout_now,
      float(card_w), 58.0,
   [0.86, 0.86, 0.86, 0.96])
   if(summary_cols > 1){ gui.same_line() }
   def frame_ms = float(renderer_stats.get("frame_ms", 0.0))
   _stat_card("editor_runtime_card",
      "Runtime",
      to_str(int(fps_value)) + " fps",
      f"{frame_ms:.2f}ms frame",
      float(card_w), 58.0,
   [0.74, 0.74, 0.74, 0.94])
   if(summary_cols > 2){ gui.same_line() }
   if(!compact){
      _stat_card("editor_renderer_card",
         "Renderer",
         to_str(int(renderer_stats.get("draws", 0))) + " draws",
         to_str(renderer_hotspot),
         float(card_w), 58.0,
      [0.82, 0.82, 0.82, 0.94])
   }
   gui.text_colored("Shot: " + to_str(active_shot), [0.66, 0.66, 0.66, 0.90])
   selected
}

fn draw_footer(any: last_dump_path, any: last_probe_text, bool: trace=false, any: editor_w=0.0): any {
   if(trace){ ui_profile.print_text("[ui:gui-editor] footer") }
   gui.separator()
   if(trace){ ui_profile.print_text("[ui:gui-editor] footer_sep") }
   def budget = _footer_chars(editor_w)
   def dump_path = to_str(last_dump_path)
   if(dump_path.len > 0){
      if(trace){ ui_profile.print_text("[ui:gui-editor] footer_dump") }
      gui.text_colored("Last dump: " + _footer_path(dump_path, max(8, budget - 11)), [0.66, 0.66, 0.66, 0.92])
   }
   if(trace){ ui_profile.print_text("[ui:gui-editor] footer_probe_check") }
   def probe_text = to_str(last_probe_text)
   if(probe_text.len > 0){
      if(trace){ ui_profile.print_text("[ui:gui-editor] footer_probe") }
      gui.text_colored("Probe: " + _ellipsize_middle(probe_text, max(8, budget - 7)), [0.72, 0.72, 0.72, 0.94])
   }
}

fn draw_graph_body(any: nodes, any: links, any: selected, any: card_w, any: canvas_h, any: grid): list {
   gui.text_colored("Editor graph shell", [0.84, 0.84, 0.84, 1.0])
   gui.text("Drag node headers to rearrange the pipeline view.")
   _stat_card("graph_nodes_card",
      "Nodes",
      to_str(is_list(nodes) ? nodes.len : 0),
      "editable graph blocks",
      float(card_w), 72.0,
   [0.84, 0.84, 0.84, 1.0])
   gui.same_line()
   _stat_card("graph_links_card",
      "Links",
      to_str(is_list(links) ? links.len : 0),
      "routing edges",
      float(card_w), 72.0,
   [0.76, 0.76, 0.76, 1.0])
   gui.same_line()
   def sel_title = is_dict(selected) ? to_str(selected.get("title", "Node")) : "<none>"
   def sel_hint = is_dict(selected) ? "live details below" : "click a node header"
   _stat_card("graph_selected_card",
      "Selected",
      sel_title,
      sel_hint,
      float(card_w), 72.0,
   [1.0, 0.78, 0.28, 1.0])
   mut out = gui.node_canvas("editor_graph",
      is_list(nodes) ? nodes : [],
      is_list(links) ? links : [],
      0.0,
      float(canvas_h),
   float(grid))
   if(is_dict(selected)){
      gui.separator()
      gui.text("Selected: " + to_str(selected.get("title", "Node")))
      gui.text("Inputs: " + to_str(selected.get("inputs", []).len) + "  Outputs: " + to_str(selected.get("outputs", []).len))
   }
   out
}
