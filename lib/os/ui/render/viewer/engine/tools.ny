;; Keywords: engine tools transform toolbar snap os ui render viewer scene
;; Toolbar helpers for transform modes, snapping, playback, and viewport tools.
;; References:
;; - std.os.ui.render.viewer.engine.state
;; - std.os.ui.render.viewer.gizmo
module std.os.ui.render.viewer.engine.tools(draw_gallery, draw_graph, draw_probe, draw_profiler, draw_workspace, profiler_hot_label, profiler_snapshot)
use std.core
use std.math (clamp)
use std.os.ui.render.viewer.app as ui_app
use std.os.ui.render.viewer.gui as gui
use std.os.ui.render.dump as ui_profile
use std.os.ui.window as ui_window
use std.os.ui.render.viewer.dock as ui_editor
use std.os.ui.render.viewer.engine.editor.chrome as demo_editor
use std.os.ui.render.viewer.engine.panels as viewer_panels

fn _state(any state) dict { is_dict(state) ? state : dict(0) }

fn _num(any value, f64 fallback=0.0) f64 {
   if value == nil { return fallback }
   float(value)
}

fn _txt(any value, str fallback="") str {
   if value == nil { return fallback }
   def out = to_str(value)
   out == "<nil>" ? fallback : out
}

fn _finish(st_in, bool show, bool closed=false) dict {
   mut st = st_in
   st["show"] = show
   st["closed"] = closed
   st
}

fn _tool(st_in, str id, str title, f64 x, f64 y, f64 w, f64 h) list {
   mut st = st_in
   def show = bool(st.get("show", false))
   def tool = ui_editor.begin_tool(id, show, title, x, y, w, h)
   if !show || bool(tool.get(1, false)) {
      return [false, _finish(st, false, bool(tool.get(1, false)))]
   }
   [bool(tool.get(0, false)), st]
}

fn profiler_hot_label(any last_update_ms, any last_world_ms, any last_draw_ms, any last_ui_ms) str {
   "Returns the dominant frame stage label."
   mut label = "draw"
   mut best = _num(last_draw_ms)
   if _num(last_world_ms) > best {
      best = _num(last_world_ms)
      label = "world"
   }
   if _num(last_update_ms) > best {
      best = _num(last_update_ms)
      label = "update"
   }
   if _num(last_ui_ms) > best { label = "ui" }
   label
}

fn profiler_snapshot(any fps_value, any last_frame_ms, any last_update_ms, any last_world_ms, any last_draw_ms, any last_ui_ms, any fps_samples, any frame_ms_samples, any draw_ms_samples, any ui_ms_samples, bool parity=false) dict {
   "Builds the profiler timing snapshot consumed by the profiler panel."
   if parity {
      return {
         "fps": 0, "last_frame": 0.0, "last_update": 0.0, "last_world": 0.0,
         "last_draw": 0.0, "last_ui": 0.0, "avg_fps": 0.0, "avg_frame": 0.0,
         "avg_draw": 0.0, "avg_ui": 0.0, "peak_frame": 0.0, "hot_label": "steady"
      }
   }
   {
      "fps": int(_num(fps_value)), "last_frame": _num(last_frame_ms), "last_update": _num(last_update_ms),
      "last_world": _num(last_world_ms), "last_draw": _num(last_draw_ms), "last_ui": _num(last_ui_ms),
      "avg_fps": float(ui_app.app_hist_mean(fps_samples)), "avg_frame": float(ui_app.app_hist_mean(frame_ms_samples)),
      "avg_draw": float(ui_app.app_hist_mean(draw_ms_samples)), "avg_ui": float(ui_app.app_hist_mean(ui_ms_samples)),
      "peak_frame": float(ui_app.app_hist_max(frame_ms_samples)),
      "hot_label": profiler_hot_label(last_update_ms, last_world_ms, last_draw_ms, last_ui_ms)
   }
}

fn draw_gallery(any state) dict {
   "Draws the widget gallery tool and returns updated state."
   mut st = _state(state)
   st["action"] = ""
   def opened = _tool(st, "widget_gallery", "Widget Gallery", 450.0, 20.0, 390.0, 690.0)
   if !bool(opened.get(0, false)) && bool(opened.get(1, st).get("closed", false)) { return opened.get(1, st) }
   st = opened.get(1, st)
   if bool(opened.get(0, false)) {
      def gallery_w = ui_app.app_window_w("widget_gallery", 420.0)
      def compact = gallery_w < 500.0
      st = viewer_panels.gallery_body({
            "tab": st.get("tab", 0), "context_items": st.get("context_items", []), "combo": st.get("combo", 0),
            "radio": st.get("radio", 0), "toggle_a": st.get("toggle_a", true), "toggle_b": st.get("toggle_b", false),
            "progress": st.get("progress", 0.25), "float": st.get("float", 1.0), "int": st.get("int", 0),
            "accent": st.get("accent", [0.86, 0.86, 0.86, 0.96]), "phase": st.get("phase", 0.0),
            "card_w": ui_app.app_card_w("widget_gallery", compact ? 2 : 3, 8.0, 104.0), "compact": compact,
            "frame_stats": st.get("frame_stats", dict(0)), "renderer_hotspot": st.get("renderer_hotspot", ""),
            "last_frame_ms": st.get("last_frame_ms", 0.0), "last_draw_ms": st.get("last_draw_ms", 0.0),
            "last_ui_ms": st.get("last_ui_ms", 0.0), "fps": st.get("fps", 0), "model_count": st.get("model_count", 0)
      })
   }
   ui_editor.end_tool()
   _finish(st, true)
}

fn draw_graph(any state) dict {
   "Draws the node graph tool and returns updated state."
   mut st = _state(state)
   st["action"] = ""
   def opened = _tool(st, "node_graph", "Node Graph", 860.0, 20.0, 360.0, 360.0)
   if !bool(opened.get(0, false)) && bool(opened.get(1, st).get("closed", false)) { return opened.get(1, st) }
   st = opened.get(1, st)
   if bool(opened.get(0, false)) {
      if gui.small_button("graph_reset", "Reset Layout") {
         st["nodes"] = []
         st["links"] = []
         st["action"] = "reset_graph"
      } else {
         def nodes = st.get("nodes", [])
         st["nodes"] = demo_editor.draw_graph_body(nodes, st.get("links", []), demo_editor.selected_graph_node(nodes),
            ui_app.app_card_w("node_graph", 3, 10.0, 100.0),
            clamp(ui_app.app_window_body_h("node_graph", 260.0, 168.0), 220.0, 560.0),
         float(st.get("workspace_grid", 32.0)))
      }
   }
   ui_editor.end_tool()
   _finish(st, true)
}

fn draw_probe(any state) dict {
   "Draws the GUI probe tool and returns updated state."
   mut st = _state(state)
   st["action"] = ""
   def opened = _tool(st, "widget_probe", "Probe", 860.0, 20.0, 360.0, 360.0)
   if !bool(opened.get(0, false)) && bool(opened.get(1, st).get("closed", false)) { return opened.get(1, st) }
   st = opened.get(1, st)
   if bool(opened.get(0, false)) {
      def cur = ui_window.cursor_pos(st.get("win", 0))
      def scr = ui_window.scroll_pos(st.get("win", 0))
      def probe_w = ui_app.app_window_w("widget_probe", 480.0)
      def res = viewer_panels.probe_body({
            "hovered": gui.hovered_id(), "active": gui.active_id(), "focused": gui.focused_id(),
            "layout": st.get("layout", ""), "shot": st.get("shot", ""), "mouse_x": cur.get(0, 0.0),
            "mouse_y": cur.get(1, 0.0), "scroll_x": scr.get(0, 0.0), "scroll_y": scr.get(1, 0.0),
            "rect": gui.last_item_rect(), "scene": st.get("scene", ""), "fps": st.get("fps", 0),
            "probe_w": probe_w, "card_w": ui_app.app_card_w("widget_probe", probe_w < 560.0 ? 2 : 3, 8.0, 112.0),
            "win_w": st.get("win_w", 1.0), "win_h": st.get("win_h", 1.0),
            "last_frame_ms": st.get("last_frame_ms", 0.0), "last_probe_text": st.get("last_probe_text", "")
      })
      st["action"] = to_str(res.get("action", ""))
   }
   ui_editor.end_tool()
   _finish(st, true)
}

fn draw_profiler(any state) dict {
   "Draws the profiler tool and returns updated state."
   mut st = _state(state)
   def opened = _tool(st, "profiler", "Profiler", 450.0, 20.0, 390.0, 420.0)
   if !bool(opened.get(0, false)) && bool(opened.get(1, st).get("closed", false)) { return opened.get(1, st) }
   st = opened.get(1, st)
   if bool(opened.get(0, false)) {
      viewer_panels.profiler_body(st.get("renderer", dict(0)), st.get("profile", dict(0)), _txt(st.get("renderer_hotspot", ""), ""),
      ui_app.app_card_w("profiler", 3, 10.0, 110.0), ui_app.app_card_w("profiler", 4, 10.0, 84.0))
   }
   ui_editor.end_tool()
   _finish(st, true)
}

fn draw_workspace(any state) dict {
   "Draws the workspace grid tool and returns updated state."
   mut st = _state(state)
   def opened = _tool(st, "workspace_grid", "Workspace", 450.0, 460.0, 390.0, 260.0)
   if ui_profile.gui_trace_enabled() { ui_profile.print_text("[ui:gui-workspace] rect=" + ui_app.app_rect_text("workspace_grid") + " body=" + to_str(bool(opened.get(0, false)))) }
   if !bool(opened.get(0, false)) && bool(opened.get(1, st).get("closed", false)) { return opened.get(1, st) }
   st = opened.get(1, st)
   if bool(opened.get(0, false)) {
      def out = viewer_panels.workspace_body(clamp(ui_app.app_window_body_h("workspace_grid", 210.0, 170.0), 190.0, 520.0),
      st.get("grid", 32.0), st.get("major", 4), st.get("cam_x", 0.0), st.get("cam_y", 0.0), st.get("cam_z", 0.0), st.get("font", 0))
      st["grid"] = float(out.get(0, st.get("grid", 32.0)))
      st["major"] = int(out.get(1, st.get("major", 4)))
   }
   ui_editor.end_tool()
   _finish(st, true)
}

#main {
   assert(is_dict(_state(0)) && is_dict(_finish(dict(0), true)), "viewer tools state helpers")
   assert(profiler_hot_label(4.0, 2.0, 1.0, 3.0) == "update", "viewer tools profiler hot label")
   assert(profiler_snapshot(60, 16.0, 2.0, 3.0, 4.0, 1.0, [60], [16.0], [4.0], [1.0]).get("hot_label", "") == "draw", "viewer tools profiler snapshot")
   print("✓ viewer tools self-test passed")
}
