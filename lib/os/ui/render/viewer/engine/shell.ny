;; Keywords: engine shell workspace layout panels docking os ui render viewer scene
;; Docking shell layout and workspace state for multi-panel UI applications.
;; References:
;; - std.os.ui.render.viewer.dock
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.engine.shell(TOOL_IDS, apply_tool_plan, hide_tools, reset_tool_scrolls, sanitize_workspace_state, workspace_visibility, focus_window_ids, capture_layout_state, shot_plan, tiled_layout_plan)
use std.core
use std.math (max)
use std.os.ui.render.viewer.gui as ui_layout
use std.os.ui.render.viewer.dock as ui_editor

def TOOL_IDS = ["editor_main", "profiler", "node_graph", "inspector", "workspace_grid", "asset_browser", "widget_probe", "widget_gallery"]

fn _tool_ids(any ids=0) list {
   is_list(ids) ? ids : TOOL_IDS
}

fn hide_tools(any ids=0) int {
   "Hides all requested editor tool windows."
   def rows = _tool_ids(ids)
   mut i = 0
   while i < rows.len {
      ui_editor.show_tool(to_str(rows.get(i, "")), false)
      i += 1
   }
   0
}

fn reset_tool_scrolls(any ids=0) int {
   "Resets scroll state for all requested editor tool windows."
   def rows = _tool_ids(ids)
   mut i = 0
   while i < rows.len {
      ui_layout.reset_window_scroll(to_str(rows.get(i, "")))
      i += 1
   }
   0
}

fn _apply_tool_plan_row(any plan, any id) any {
   def row = plan.get(to_str(id), dict(0))
   if is_dict(row) {
      ui_editor.apply_tool_rect_if_visible(id, bool(row.get("show", false)), row.get("rect", [0.0, 0.0, 0.0, 0.0]))
   }
}

fn apply_tool_plan(any plan, any ids=0) int {
   "Applies a tiled layout plan to visible editor tools."
   def rows = _tool_ids(ids)
   mut i = 0
   while i < rows.len {
      _apply_tool_plan_row(plan, to_str(rows.get(i, "")))
      i += 1
   }
   0
}

fn sanitize_workspace_state(any editor_tab, any workspace_mode, any center_tab, any side_tab, any editor_tab_count=5) dict {
   "Clamps editor/workspace tab state to valid ranges."
   mut out = dict(8)
   mut et = int(editor_tab)
   mut wm = int(workspace_mode)
   mut ct = int(center_tab)
   mut st = int(side_tab)
   if et < 0 || et >= int(editor_tab_count) { et = 0 }
   if wm < 0 || wm > 1 { wm = 1 }
   if ct < 0 || ct > 2 { ct = 0 }
   if st < 0 || st > 3 { st = 0 }
   out["editor_tab"] = et
   out["workspace_mode"] = wm
   out["center_tab"] = ct
   out["side_tab"] = st
   out
}

fn workspace_visibility(any workspace_mode, any center_tab, any side_tab) dict {
   "Returns which workspace tools should be visible for tab state."
   mut out = dict(8)
   out["browser"] = false
   out["workspace"] = false
   out["graph"] = false
   out["inspector"] = false
   out["profiler"] = false
   out["probe"] = false
   out["gallery"] = false
   if int(workspace_mode) != 1 { return out }
   def ct, st = int(center_tab), int(side_tab)
   out["workspace"] = ct == 1
   out["graph"] = ct == 2
   out["inspector"] = st == 0
   out["profiler"] = st == 1
   out["probe"] = st == 2
   out["gallery"] = st >= 3
   out
}

fn focus_window_ids(
   any show_profiler, any show_workspace, any show_graph, any show_inspector,
   any show_browser, any show_editor, any show_probe, any show_gallery
) list {
   "Returns visible tool ids that should participate in focus layout."
   mut ids = []
   if show_profiler { ids = ids.append("profiler") }
   if show_workspace { ids = ids.append("workspace_grid") }
   if show_graph { ids = ids.append("node_graph") }
   if show_inspector { ids = ids.append("inspector") }
   if show_browser && !show_editor { ids = ids.append("asset_browser") }
   if show_probe { ids = ids.append("widget_probe") }
   if show_gallery { ids = ids.append("widget_gallery") }
   ids
}

fn capture_layout_state(
   any preset, any workspace_mode, any center_tab, any side_tab, any editor_tab, any gallery_tab,
   any browser_tab, any inspector_tab, any show_gallery, any show_probe, any show_browser,
   any show_profiler, any show_workspace, any show_graph, any show_inspector, any scale, any gap
) dict {
   "Builds a serializable snapshot of the current editor layout state."
   {
      "preset": preset, "mode": int(workspace_mode), "center_tab": int(center_tab),
      "side_tab": int(side_tab), "editor_tab": int(editor_tab), "gallery_tab": int(gallery_tab),
      "browser_tab": int(browser_tab), "inspector_tab": int(inspector_tab),
      "show_gallery": bool(show_gallery), "show_probe": bool(show_probe), "show_browser": bool(show_browser),
      "show_profiler": bool(show_profiler), "show_workspace": bool(show_workspace),
      "show_graph": bool(show_graph), "show_inspector": bool(show_inspector),
      "scale": float(scale), "gap": float(gap)
   }
}

fn _shell_show(
   any editor=false, any gallery=false, any probe=false, any browser=false,
   any inspector=false, any profiler=false, any workspace=false, any graph=false
) dict {
   {
      "show_editor": bool(editor), "show_gallery": bool(gallery), "show_probe": bool(probe),
      "show_browser": bool(browser), "show_inspector": bool(inspector), "show_profiler": bool(profiler),
      "show_workspace": bool(workspace), "show_graph": bool(graph)
   }
}

fn _shell_tabs(dict out, any editor_tab=0, any workspace_mode=0, any center_tab=0, any side_tab=0) dict {
   out["editor_tab"] = int(editor_tab)
   out["workspace_mode"] = int(workspace_mode)
   out["center_tab"] = int(center_tab)
   out["side_tab"] = int(side_tab)
   out
}

fn _shell_plan(
   any editor=false, any gallery=false, any probe=false, any browser=false,
   any inspector=false, any profiler=false, any workspace=false, any graph=false,
   any editor_tab=0, any workspace_mode=0, any center_tab=0, any side_tab=0,
   any browser_tab=0, any gallery_tab=0, any inspector_tab=0, any model_filter="", any probe_overlay=false
) dict {
   def show = _shell_show(editor, gallery, probe, browser, inspector, profiler, workspace, graph)
   mut out = _shell_tabs(show, editor_tab, workspace_mode, center_tab, side_tab)
   out["browser_tab"] = int(browser_tab)
   out["gallery_tab"] = int(gallery_tab)
   out["inspector_tab"] = int(inspector_tab)
   out["model_filter"] = to_str(model_filter)
   out["probe_overlay"] = bool(probe_overlay)
   out
}

fn _shot_tab(any shot_name, list names, int fallback=0) int {
   def shot = to_str(shot_name)
   mut i = 0
   while i < names.len {
      if shot == to_str(names.get(i, "")) { return i }
      i += 1
   }
   fallback
}

fn _shot_in(any shot_name, list names) bool {
   _shot_tab(shot_name, names, -1) >= 0
}

fn _editor_plan(any tab=0, any inspector=false, any workspace=false, any center_tab=0, any model_filter="", any probe_overlay=false, any inspector_tab=1) dict {
   def workspace_mode = (bool(inspector) || bool(workspace) || int(center_tab) != 0) ? 1 : 0
   _shell_plan(true, false, false, false, inspector, false, workspace, false,
   tab, workspace_mode, center_tab, 0, 0, 0, inspector_tab, model_filter, probe_overlay)
}

fn _tool_plan(str tool, any tab=0, any model_filter="", any probe_overlay=false) dict {
   _shell_plan(false, tool == "gallery", tool == "probe", tool == "browser",
      tool == "inspector", tool == "profiler", tool == "workspace", tool == "graph",
      0, 0, 0, (tool == "probe") ? 2 : 0,
      (tool == "browser") ? tab : 0,
      (tool == "gallery") ? tab : 0,
      (tool == "inspector") ? tab : 0,
      (tool == "browser") ? model_filter : "",
   (tool == "probe") ? probe_overlay : false)
}

fn _inspector_tab(any shot_name) int {
   int({
         "inspector_camera": 1,
         "inspector_viewport": 1,
         "inspector_env": 1,
         "inspector_settings": 1,
         "inspector_diag": 2,
         "inspector_renderer": 2
   }.get(to_str(shot_name), 0))
}

fn shot_plan(any shot_name) dict {
   "Returns the tool visibility/tab plan for a named GUI capture shot."
   def shot = to_str(shot_name)
   def gallery_shots = ["gallery_basics", "gallery_metrics", "gallery_theme"]
   def browser_shots = ["browser_catalog", "browser_hierarchy", "browser_filtered"]
   def inspector_shot =
   shot == "inspector_scene" || shot == "inspector_camera" ||
   shot == "inspector_viewport" || shot == "inspector_env" ||
   shot == "inspector_settings" || shot == "inspector_diag" ||
   shot == "inspector_renderer"
   if shot == "full_editor" {
      return _editor_plan(0, true)
   } elif shot == "full_editor_workspace" {
      return _editor_plan(0, true, false, 1)
   } elif shot == "editor_catalog" || shot == "editor_scene" {
      return _editor_plan()
   } elif shot == "editor_hierarchy" {
      return _editor_plan(1)
   } elif shot == "editor_scene_compact" {
      return _editor_plan(0, true, true, 1, "box")
   } elif shot == "editor_view" {
      return _editor_plan(0, true)
   } elif shot == "editor_theme" {
      return _editor_plan(2)
   } elif shot == "editor_console" {
      return _editor_plan(3)
   } elif shot == "editor_probe" {
      return _editor_plan(3, false, false, 0, "", true)
   } elif shot == "probe_overlay" {
      return _tool_plan("probe", 0, "", true)
   } elif _shot_in(shot, gallery_shots) {
      return _tool_plan("gallery", _shot_tab(shot, gallery_shots, 0))
   } elif _shot_in(shot, browser_shots) {
      def browser_tab = (shot == "browser_hierarchy") ? 1 : 0
      def model_filter = (shot == "browser_filtered") ? "box" : ""
      return _tool_plan("browser", browser_tab, model_filter)
   } elif inspector_shot {
      return _tool_plan("inspector", _inspector_tab(shot))
   } elif shot == "graph" {
      return _tool_plan("graph")
   } elif shot == "workspace" {
      return _tool_plan("workspace")
   } elif shot == "profiler" {
      return _tool_plan("profiler")
   }
   _shell_plan()
}

fn _shell_rect(dict tiles, any key, any fallback) list {
   tiles.get(to_str(key), fallback)
}

fn _shell_plan_rect(dict out, any id, any show, any rect) any {
   mut row = dict(3)
   row["id"] = to_str(id)
   row["show"] = bool(show)
   row["rect"] = is_list(rect) ? rect : [0.0, 0.0, 0.0, 0.0]
   out[to_str(id)] = row
}

fn tiled_layout_plan(
   any preset, any ww, any wh, any gap, any workspace_mode,
   any show_editor, any show_gallery, any show_probe, any show_browser,
   any show_inspector, any show_profiler, any show_workspace, any show_graph
) dict {
   "Builds tile rectangles for the current editor shell layout."
   def p, w = to_str(preset), float(ww)
   def h, g = float(wh), 0.0
   def editor = bool(show_editor)
   def gallery = bool(show_gallery)
   def probe = bool(show_probe)
   def browser = bool(show_browser)
   def inspector = bool(show_inspector)
   def profiler = bool(show_profiler)
   def workspace = bool(show_workspace)
   def graph = bool(show_graph)
   def standalone_browser = browser && !editor
   mut out = dict(16)
   out["standalone_browser"] = standalone_browser
   out["focus_only"] = false
   if !editor {
      out["focus_only"] = true
      return out
   }
   def editor_only = editor && int(workspace_mode) != 1 &&
   !gallery && !probe && !browser && !inspector && !profiler && !workspace && !graph
   if editor_only {
      out["focus_only"] = true
      return out
   }
   def tiny = w < 900.0 || h < 560.0
   def auto_compact = w < 1680.0 || h < 940.0
   mut tiles = dict(12)
   if p == "default" && tiny {
      tiles = ui_layout.tile_editor_shell_preset("compact", w, h, g)
   } elif p == "default" && auto_compact {
      tiles = ui_layout.tile_editor_shell(w, h, g, 0.24, 0.30, 0.20)
   } elif p == "default" {
      tiles = ui_layout.tile_editor_shell(w, h, g, 0.27, 0.30, 0.22)
   } else {
      tiles = ui_layout.tile_editor_shell_preset(p, w, h, g)
   }
   _shell_plan_rect(out, "editor_main", editor, _shell_rect(tiles, "editor_main", [20.0, 20.0, 390.0, 680.0]))
   if int(workspace_mode) == 1 {
      def center_main = _shell_rect(tiles, "center_main", _shell_rect(tiles, "workspace_grid", [450.0, 460.0, 390.0, 260.0]))
      def side_main = _shell_rect(tiles, "side_main", _shell_rect(tiles, "inspector", [860.0, 20.0, 360.0, 360.0]))
      _shell_plan_rect(out, "asset_browser", standalone_browser, center_main)
      _shell_plan_rect(out, "workspace_grid", workspace, center_main)
      _shell_plan_rect(out, "node_graph", graph, center_main)
      _shell_plan_rect(out, "inspector", inspector, side_main)
      _shell_plan_rect(out, "profiler", profiler, side_main)
      _shell_plan_rect(out, "widget_probe", probe, side_main)
      _shell_plan_rect(out, "widget_gallery", gallery, side_main)
      return out
   }
   _shell_plan_rect(out, "profiler", profiler, _shell_rect(tiles, "profiler", [450.0, 20.0, 390.0, 420.0]))
   _shell_plan_rect(out, "node_graph", graph, _shell_rect(tiles, "node_graph", [860.0, 20.0, 360.0, 360.0]))
   _shell_plan_rect(out, "inspector", inspector, _shell_rect(tiles, "inspector", [860.0, 20.0, 360.0, 360.0]))
   _shell_plan_rect(out, "workspace_grid", workspace, _shell_rect(tiles, "workspace_grid", [450.0, 460.0, 390.0, 260.0]))
   _shell_plan_rect(out, "asset_browser", standalone_browser, _shell_rect(tiles, "asset_browser", [860.0, 400.0, 360.0, 420.0]))
   _shell_plan_rect(out, "widget_probe", probe, _shell_rect(tiles, "widget_probe", [860.0, 20.0, 360.0, 360.0]))
   _shell_plan_rect(out, "widget_gallery", gallery, _shell_rect(tiles, "widget_gallery", [450.0, 20.0, 390.0, 690.0]))
   out
}

#main {
   def cleaned = sanitize_workspace_state(99, -4, 8, 9, 5)
   assert(cleaned.get("editor_tab", -1) == 0 && cleaned.get("workspace_mode", -1) == 1, "shell sanitize workspace")
   assert(cleaned.get("center_tab", -1) == 0 && cleaned.get("side_tab", -1) == 0, "shell sanitize tabs")
   def vis = workspace_visibility(1, 2, 3)
   assert(vis.get("graph", false) && vis.get("gallery", false) && !vis.get("workspace", true), "shell workspace visibility")
   def ids = focus_window_ids(true, true, false, true, true, false, true, false)
   assert(ids == ["profiler", "workspace_grid", "inspector", "asset_browser", "widget_probe"], "shell focus ids")
   def captured = capture_layout_state("default", 1, 2, 3, 4, 5, 6, 7, true, false, true, false, true, false, true, 1.5, 10.0)
   assert(captured.get("preset", "") == "default" && captured.get("browser_tab", -1) == 6 && captured.get("scale", 0.0) == 1.5, "shell capture layout")
   assert(captured.get("show_inspector", false), "shell capture flags")
   def browser_plan = shot_plan("browser_filtered")
   assert(browser_plan.get("show_browser", false) && browser_plan.get("model_filter", "") == "box", "shell browser shot")
   def editor_plan = shot_plan("editor_probe")
   assert(editor_plan.get("show_editor", false) && editor_plan.get("probe_overlay", false), "shell editor probe shot")
   def full_plan = shot_plan("full_editor")
   assert(full_plan.get("show_inspector", false) && full_plan.get("inspector_tab", -1) == 1, "shell full editor view inspector")
   def focus_only = tiled_layout_plan("default", 1200.0, 700.0, 8.0, 0, false, false, false, true, false, false, false, false)
   assert(focus_only.get("focus_only", false), "shell focus-only plan")
   def tiled = tiled_layout_plan("default", 1600.0, 920.0, 12.0, 1, true, true, true, false, true, true, true, true)
   assert(!tiled.get("focus_only", true) && is_dict(tiled.get("editor_main", 0)), "shell tiled layout active")
   assert(is_dict(tiled.get("workspace_grid", 0)) && is_dict(tiled.get("node_graph", 0)) && is_dict(tiled.get("inspector", 0)), "shell workspace tiles")
   assert(!is_dict(tiled.get("widget_gallery", 0)), "shell workspace mode suppresses gallery tile")
   print("✓ std.os.ui.render.viewer.engine.shell self-test passed")
}
