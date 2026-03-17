;; Keywords: shell workspace layout
;; Docking shell layout and workspace state for multi-panel UI applications.
module std.os.ui.shell(sanitize_workspace_state, workspace_visibility, focus_window_ids, capture_layout_state, shot_plan, tiled_layout_plan)
use std.core
use std.os.ui.dock as ui_editor

fn sanitize_workspace_state(any: editor_tab, any: workspace_mode, any: center_tab, any: side_tab, any: editor_tab_count=5): dict {
   mut out = dict(8)
   mut et = int(editor_tab)
   mut wm = int(workspace_mode)
   mut ct = int(center_tab)
   mut st = int(side_tab)
   if(et < 0 || et >= int(editor_tab_count)){ et = 0 }
   if(wm < 0 || wm > 1){ wm = 1 }
   if(ct < 0 || ct > 2){ ct = 0 }
   if(st < 0 || st > 3){ st = 0 }
   out["editor_tab"] = et
   out["workspace_mode"] = wm
   out["center_tab"] = ct
   out["side_tab"] = st
   out
}

fn workspace_visibility(any: workspace_mode, any: center_tab, any: side_tab): dict {
   mut out = dict(8)
   out["browser"] = false
   out["workspace"] = false
   out["graph"] = false
   out["inspector"] = false
   out["profiler"] = false
   out["probe"] = false
   out["gallery"] = false
   if(int(workspace_mode) != 1){ return out }
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
   any: show_profiler, any: show_workspace, any: show_graph, any: show_inspector,
   any: show_browser, any: show_editor, any: show_probe, any: show_gallery
): list {
   mut ids = []
   if(show_profiler){ ids = ids.append("profiler") }
   if(show_workspace){ ids = ids.append("workspace_grid") }
   if(show_graph){ ids = ids.append("node_graph") }
   if(show_inspector){ ids = ids.append("inspector") }
   if(show_browser && !show_editor){ ids = ids.append("asset_browser") }
   if(show_probe){ ids = ids.append("widget_probe") }
   if(show_gallery){ ids = ids.append("widget_gallery") }
   ids
}

fn capture_layout_state(
   any: preset, any: workspace_mode, any: center_tab, any: side_tab, any: editor_tab, any: gallery_tab,
   any: browser_tab, any: inspector_tab, any: show_gallery, any: show_probe, any: show_browser,
   any: show_profiler, any: show_workspace, any: show_graph, any: show_inspector, any: scale, any: gap
): dict {
   mut out = dict(24)
   out["preset"] = preset
   out["mode"] = int(workspace_mode)
   out["center_tab"] = int(center_tab)
   out["side_tab"] = int(side_tab)
   out["editor_tab"] = int(editor_tab)
   out["gallery_tab"] = int(gallery_tab)
   out["browser_tab"] = int(browser_tab)
   out["inspector_tab"] = int(inspector_tab)
   out["show_gallery"] = bool(show_gallery)
   out["show_probe"] = bool(show_probe)
   out["show_browser"] = bool(show_browser)
   out["show_profiler"] = bool(show_profiler)
   out["show_workspace"] = bool(show_workspace)
   out["show_graph"] = bool(show_graph)
   out["show_inspector"] = bool(show_inspector)
   out["scale"] = float(scale)
   out["gap"] = float(gap)
   out
}

fn _shell_show(
   any: editor=false, any: gallery=false, any: probe=false, any: browser=false,
   any: inspector=false, any: profiler=false, any: workspace=false, any: graph=false
): dict {
   mut out = dict(16)
   out["show_editor"] = bool(editor)
   out["show_gallery"] = bool(gallery)
   out["show_probe"] = bool(probe)
   out["show_browser"] = bool(browser)
   out["show_inspector"] = bool(inspector)
   out["show_profiler"] = bool(profiler)
   out["show_workspace"] = bool(workspace)
   out["show_graph"] = bool(graph)
   out
}

fn _shell_tabs(dict: out, any: editor_tab=0, any: workspace_mode=0, any: center_tab=0, any: side_tab=0): dict {
   out["editor_tab"] = int(editor_tab)
   out["workspace_mode"] = int(workspace_mode)
   out["center_tab"] = int(center_tab)
   out["side_tab"] = int(side_tab)
   out
}

fn _shell_plan(
   any: editor=false, any: gallery=false, any: probe=false, any: browser=false,
   any: inspector=false, any: profiler=false, any: workspace=false, any: graph=false,
   any: editor_tab=0, any: workspace_mode=0, any: center_tab=0, any: side_tab=0,
   any: browser_tab=0, any: gallery_tab=0, any: inspector_tab=0, any: model_filter="", any: probe_overlay=false
): dict {
   def show = _shell_show(editor, gallery, probe, browser, inspector, profiler, workspace, graph)
   mut out = _shell_tabs(show, editor_tab, workspace_mode, center_tab, side_tab)
   out["browser_tab"] = int(browser_tab)
   out["gallery_tab"] = int(gallery_tab)
   out["inspector_tab"] = int(inspector_tab)
   out["model_filter"] = to_str(model_filter)
   out["probe_overlay"] = bool(probe_overlay)
   out
}

fn shot_plan(any: shot_name): dict {
   def shot = to_str(shot_name)
   def gallery_shot = shot == "gallery_basics" || shot == "gallery_metrics" || shot == "gallery_theme"
   def browser_shot =
   shot == "browser_catalog" || shot == "browser_selected" ||
   shot == "browser_hierarchy" || shot == "browser_filtered"
   def inspector_shot =
   shot == "inspector_scene" || shot == "inspector_camera" ||
   shot == "inspector_env" || shot == "inspector_settings" ||
   shot == "inspector_diag"
   if(shot == "full_editor"){
      return _shell_plan(true, false, false, false, true, false, false, false, 0, 1, 0, 0)
   } elif(shot == "full_editor_workspace"){
      return _shell_plan(true, false, false, false, true, false, false, false, 0, 1, 1, 0)
   } elif(shot == "editor_scene"){
      return _shell_plan(true)
   } elif(shot == "editor_scene_compact"){
      return _shell_plan(true, false, false, false, true, false, true, false, 0, 1, 1, 0, 0, 0, 0, "box")
   } elif(shot == "editor_view"){
      return _shell_plan(true, false, false, false, false, false, false, false, 1)
   } elif(shot == "editor_theme"){
      return _shell_plan(true, false, false, false, false, false, false, false, 2)
   } elif(shot == "editor_console"){
      return _shell_plan(true, false, false, false, false, false, false, false, 3)
   } elif(shot == "editor_probe"){
      return _shell_plan(true, false, false, false, false, false, false, false, 4, 0, 0, 0, 0, 0, 0, "", true)
   } elif(shot == "probe_overlay"){
      return _shell_plan(false, false, true, false, false, false, false, false, 0, 0, 0, 2, 0, 0, 0, "", true)
   } elif(gallery_shot){
      def gallery_tab = (shot == "gallery_metrics") ? 1 : ((shot == "gallery_theme") ? 2 : 0)
      return _shell_plan(false, true, false, false, false, false, false, false, 0, 0, 0, 0, 0, gallery_tab)
   } elif(browser_shot){
      def browser_tab = (shot == "browser_selected") ? 2 : ((shot == "browser_hierarchy") ? 1 : 0)
      def model_filter = (shot == "browser_filtered") ? "box" : ""
      return _shell_plan(false, false, false, true, false, false, false, false,
      0, 0, 0, 0, browser_tab, 0, 0, model_filter)
   } elif(inspector_shot){
      mut inspector_tab = 0
      if(shot == "inspector_camera"){ inspector_tab = 1 }
      elif(shot == "inspector_env"){ inspector_tab = 3 }
      elif(shot == "inspector_settings"){ inspector_tab = 4 }
      elif(shot == "inspector_diag"){ inspector_tab = 6 }
      return _shell_plan(false, false, false, false, true, false, false, false,
      0, 0, 0, 0, 0, 0, inspector_tab)
   } elif(shot == "graph"){
      return _shell_plan(false, false, false, false, false, false, false, true)
   } elif(shot == "workspace"){
      return _shell_plan(false, false, false, false, false, false, true, false)
   } elif(shot == "profiler"){
      return _shell_plan(false, false, false, false, false, true, false, false)
   }
   _shell_plan()
}

fn _shell_rect(dict: tiles, any: key, any: fallback): list {
   tiles.get(to_str(key), fallback)
}

fn _shell_plan_rect(dict: out, any: id, any: show, any: rect): any {
   mut row = dict(3)
   row["id"] = to_str(id)
   row["show"] = bool(show)
   row["rect"] = is_list(rect) ? rect : [0.0, 0.0, 0.0, 0.0]
   out[to_str(id)] = row
}

fn tiled_layout_plan(
   any: preset, any: ww, any: wh, any: gap, any: workspace_mode,
   any: show_editor, any: show_gallery, any: show_probe, any: show_browser,
   any: show_inspector, any: show_profiler, any: show_workspace, any: show_graph
): dict {
   def p, w = to_str(preset), float(ww)
   def h, g = float(wh), float(gap)
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
   if(!editor){
      out["focus_only"] = true
      return out
   }
   def editor_only = editor && int(workspace_mode) != 1 &&
   !gallery && !probe && !browser && !inspector && !profiler && !workspace && !graph
   if(editor_only){
      out["focus_only"] = true
      return out
   }
   def auto_compact = w < 1460.0 || h < 860.0
   mut tiles = dict(12)
   if(p == "default" && auto_compact){
      tiles = ui_editor.tile_shell(w, h, max(2.0, g - 1.0), 0.25, 0.34, 0.22)
   } elif(p == "default"){
      tiles = ui_editor.tile_shell(w, h, g, 0.27, 0.30, 0.22)
   } else {
      tiles = ui_editor.tile_shell_preset(p, w, h, g)
   }
   _shell_plan_rect(out, "editor_main", editor, _shell_rect(tiles, "editor_main", [20.0, 20.0, 390.0, 680.0]))
   if(int(workspace_mode) == 1){
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
