;; Keywords: engine panels layout inspector hierarchy os ui render viewer scene
;; Panel layout helpers for hierarchy, inspector, profile, and viewport UI regions.
;; References:
;; - std.os.ui.render.viewer.engine.state
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.engine.panels(console_body, probe_body, workspace_body, profiler_body, gallery_body)
use std.core
use std.core.str as str
use std.math (clamp, max, min, sin)
use std.os.ui.render.viewer.gui as gui
use std.os.ui.render.viewer.editor.colorpicker as colorpicker
use std.os.ui.render as render

fn _action(action="", input="", command="") dict {
   {"action": action, "input": input, "command": command}
}

fn _txt(any value, str fallback="") str {
   if(value == nil){ return fallback }
   def out = to_str(value)
   out == "<nil>" ? fallback : out
}

fn _num(any value, f64 fallback=0.0) f64 {
   if(value == nil){ return fallback }
   float(value)
}

fn _intv(any value, int fallback=0) int {
   if(value == nil){ return fallback }
   int(value)
}

fn console_body(input, history) dict {
   "Runs the console body operation."
   gui.text_colored("Integrated Console", [0.82, 0.82, 0.82, 1.0])
   mut next_input = gui.input_text("console_cmd", "Command", input, "help, load Lantern, autofit, lookat, snapshot...")
   if(gui.button("console_run", "Run", 82.0)){
      def cmd_line = str.strip(next_input)
      if(cmd_line.len > 0){ return _action("command", "", cmd_line) }
   }
   gui.same_line()
   if(gui.button("console_help", "Help", 82.0)){ return _action("command", next_input, "help") }
   gui.same_line()
   if(gui.button("console_clear", "Clear", 82.0)){ return _action("clear", next_input) }
   if(gui.button("console_fit", "Autofit", 88.0)){ return _action("fit", next_input) }
   gui.same_line()
   if(gui.button("console_lookat", "Look At", 88.0)){ return _action("lookat", next_input) }
   gui.separator()
   def hist = is_list(history) ? history : []
   def hist_n = hist.len
   gui.text_colored("History: " + to_str(hist_n), [0.68, 0.68, 0.68, 1.0])
   if(hist_n <= 0){
      gui.text_colored("> help", [0.72, 0.72, 0.72, 1.0])
      gui.text("CMD: load, unload, autofit, lookat, snapshot, stats")
      gui.text_colored("> load Box", [0.72, 0.72, 0.72, 1.0])
      gui.text("Loaded: Box")
      gui.text_colored("> autofit", [0.72, 0.72, 0.72, 1.0])
      gui.text("Camera framed the active scene.")
      return _action("", next_input)
   }
   mut start = hist_n - 14
   if(start < 0){ start = 0 }
   mut i = start
   while(i < hist_n){
      def line = to_str(hist[i])
      gui.text((line.len > 0) ? line : " ")
      i += 1
   }
   _action("", next_input)
}

fn probe_body(state) dict {
   "Runs the probe body operation."
   def hovered = _txt(state.get("hovered", ""), "")
   def active = _txt(state.get("active", ""), "")
   def focused = _txt(state.get("focused", ""), "")
   def layout_name = _txt(state.get("layout", ""), "")
   def shot = _txt(state.get("shot", ""), "")
   def mx = _num(state.get("mouse_x", 0.0))
   def my = _num(state.get("mouse_y", 0.0))
   def sx = _num(state.get("scroll_x", 0.0))
   def sy = _num(state.get("scroll_y", 0.0))
   def r = is_list(state.get("rect", [])) ? state.get("rect", []) : []
   def scene = _txt(state.get("scene", ""), "")
   def fps = _num(state.get("fps", 0.0))
   def probe_w = _num(state.get("probe_w", 480.0), 480.0)
   def card_w = _num(state.get("card_w", 120.0), 120.0)
   def win_w = _num(state.get("win_w", 1.0), 1.0)
   def win_h = _num(state.get("win_h", 1.0), 1.0)
   def last_frame_ms = _num(state.get("last_frame_ms", 0.0))
   gui.text("hovered id: " + hovered)
   gui.text("active id: " + active)
   gui.text("focused id: " + focused)
   gui.text("layout: " + layout_name)
   gui.text("shot: " + shot)
   gui.text("mouse: (" + to_str(int(mx)) + ", " + to_str(int(my)) + ")")
   gui.text("scroll: (" + to_str(sx) + ", " + to_str(sy) + ")")
   gui.text("last rect: x=" + to_str(int(r.get(0, 0.0))) +
      " y=" + to_str(int(r.get(1, 0.0))) +
      " w=" + to_str(int(r.get(2, 0.0))) +
   " h=" + to_str(int(r.get(3, 0.0))))
   gui.text("scene: " + scene)
   gui.text("fps: " + to_str(fps))
   gui.stat_card("probe_hover_card", "Hover", hovered, "active " + active, card_w, 64.0, [0.58, 0.86, 1.0, 1.0])
   gui.same_line()
   gui.stat_card("probe_focus_card", "Focus", focused, layout_name, card_w, 64.0, [0.34, 0.88, 0.52, 1.0])
   if(probe_w >= 560.0){
      gui.same_line()
      gui.stat_card("probe_frame_card", "Frame", f"{last_frame_ms:.2f}ms", to_str(int(fps)) + " fps", card_w, 64.0, [1.0, 0.78, 0.28, 1.0])
   }
   gui.progress_bar("probe_mouse_x", clamp(mx / max(1.0, win_w), 0.0, 1.0), "Mouse X")
   gui.progress_bar("probe_mouse_y", clamp(my / max(1.0, win_h), 0.0, 1.0), "Mouse Y")
   gui.separator()
   if(gui.button("probe_refresh", "Refresh Probe", 132.0)){ return _action("refresh") }
   def last_probe_text = _txt(state.get("last_probe_text", ""), "")
   if(last_probe_text.len > 0){ gui.text_colored(last_probe_text, [0.78, 0.90, 0.86, 1.0]) }
   _action()
}

fn workspace_body(canvas_h, grid, major, cam_x, cam_y, cam_z, font) list {
   "Runs the workspace body operation."
   gui.text_colored("Viewport helpers", [0.84, 0.84, 0.84, 1.0])
   gui.text("Grid and framing helpers for scene composition.")
   mut out_grid = gui.slider_float("workspace_cell", "Grid Cell", float(grid), 12.0, 72.0)
   mut out_major = gui.slider_int("workspace_major", "Major Step", int(major), 2, 8)
   gui.text_colored("Grid " + f"{out_grid:.0f}px" + "   Major " + to_str(out_major) +
      "   Camera(" + f"{float(cam_x):.1f}" + ", " + f"{float(cam_y):.1f}" + ", " + f"{float(cam_z):.1f}" + ")",
   [0.68, 0.68, 0.68, 1.0])
   def rect = gui.grid_canvas("workspace_canvas", "Viewport overlay grid", 0.0, float(canvas_h), out_grid, out_major)
   def rx, ry, rw, rh = float(rect.get(0, 0.0)), float(rect.get(1, 0.0)), float(rect.get(2, 0.0)), float(rect.get(3, 0.0))
   def cx, cy = rx + rw * 0.5, ry + rh * 0.5
   render.draw_line_2d(cx, ry, cx, ry + rh, [0.78, 0.78, 0.78, 0.90], 1.5)
   render.draw_line_2d(rx, cy, rx + rw, cy, [0.78, 0.78, 0.78, 0.90], 1.5)
   render.draw_rect_fast(cx - 3.0, cy - 3.0, 6.0, 6.0, render.color_pack(0.78, 0.78, 0.78, 0.90))
   render.draw_rect_fast(rx + 14.0, ry + 14.0, max(24.0, rw - 28.0), max(24.0, rh - 28.0), render.color_pack(0.00, 0.00, 0.00, 0.18))
   def node_w, node_h = clamp(rw * 0.18, 118.0, 180.0), 44.0
   def n1x, n1y = rx + rw * 0.18, ry + rh * 0.28
   def n2x, n2y = rx + rw * 0.42, min(ry + rh - node_h - 18.0, cy + 26.0)
   def n3x, n3y = rx + rw * 0.66, ry + rh * 0.34
   render.draw_line_2d(n1x + node_w, n1y + node_h * 0.5, n2x, n2y + node_h * 0.5, [0.72, 0.72, 0.72, 0.34], 2.0)
   render.draw_line_2d(n2x + node_w, n2y + node_h * 0.5, n3x, n3y + node_h * 0.5, [0.72, 0.72, 0.72, 0.34], 2.0)
   render.draw_rect_fast(n1x, n1y, node_w, node_h, render.color_pack(0.000, 0.000, 0.000, 0.78))
   render.draw_rect_fast(n2x, n2y, node_w, node_h, render.color_pack(0.000, 0.000, 0.000, 0.78))
   render.draw_rect_fast(n3x, n3y, node_w, node_h, render.color_pack(0.000, 0.000, 0.000, 0.78))
   render.draw_rect_fast(n1x, n1y, node_w, 2.0, render.color_pack(0.78, 0.78, 0.78, 0.66))
   render.draw_rect_fast(n2x, n2y, node_w, 2.0, render.color_pack(0.64, 0.64, 0.64, 0.64))
   render.draw_rect_fast(n3x, n3y, node_w, 2.0, render.color_pack(0.86, 0.86, 0.86, 0.62))
   render.draw_text(font, "Camera", n1x + 10.0, n1y + 12.0, [0.86, 0.86, 0.86, 0.94])
   render.draw_text(font, "Selection", n2x + 10.0, n2y + 12.0, [0.82, 0.82, 0.82, 0.94])
   render.draw_text(font, "Inspector", n3x + 10.0, n3y + 12.0, [0.90, 0.90, 0.90, 0.92])
   render.draw_text(font, "world origin", cx + 8.0, cy - 22.0, [0.76, 0.76, 0.76, 0.92])
   [out_grid, out_major]
}

fn _profiler_summary(rs, p, hotspot, card_w) int {
   def last_frame = _num(p.get("last_frame", 0.0))
   gui.text_colored("Live runtime timings", [0.84, 0.84, 0.84, 1.0])
   gui.text("Frame budget, draw pressure, and renderer-state churn in one place.")
   gui.stat_card("prof_fps_card", "FPS", to_str(_intv(p.get("fps", 0))), f"{last_frame:.2f}ms frame", card_w, 76.0, [0.76, 0.76, 0.76, 1.0])
   gui.same_line()
   gui.stat_card("prof_hot_card", "Dominant", _txt(p.get("hot_label", ""), "steady"), _txt(hotspot, ""), card_w, 76.0, [0.82, 0.82, 0.82, 1.0])
   gui.same_line()
   gui.stat_card("prof_draw_card", "Renderer", to_str(_intv(rs.get("draws", 0))) + " draws", to_str(_intv(rs.get("flushes", 0))) + " flushes", card_w, 76.0, [0.84, 0.84, 0.84, 1.0])
   0
}

fn _profiler_breakdown(p, mini_w) int {
   def avg_frame, avg_draw, avg_ui =
   _num(p.get("avg_frame", 0.0)), _num(p.get("avg_draw", 0.0)), _num(p.get("avg_ui", 0.0))
   def last_frame, last_update, last_world, last_draw, last_ui =
   _num(p.get("last_frame", 0.0)), _num(p.get("last_update", 0.0)), _num(p.get("last_world", 0.0)),
   _num(p.get("last_draw", 0.0)), _num(p.get("last_ui", 0.0))
   gui.separator()
   gui.text_colored("Frame breakdown", [0.84, 0.84, 0.84, 1.0])
   gui.stat_card("prof_update_card", "Update", f"{last_update:.2f}ms", "avg frame " + f"{avg_frame:.2f}ms", mini_w, 72.0, [0.76, 0.76, 0.76, 1.0])
   gui.same_line()
   gui.stat_card("prof_world_card", "World", f"{last_world:.2f}ms", "scene + sim", mini_w, 72.0, [0.72, 0.72, 0.72, 1.0])
   gui.same_line()
   gui.stat_card("prof_draw_ms_card", "Draw", f"{last_draw:.2f}ms", "avg " + f"{avg_draw:.2f}ms", mini_w, 72.0, [0.82, 0.82, 0.82, 1.0])
   gui.same_line()
   gui.stat_card("prof_ui_card", "UI", f"{last_ui:.2f}ms", "avg " + f"{avg_ui:.2f}ms", mini_w, 72.0, [0.86, 0.86, 0.86, 1.0])
   gui.progress_bar("frame_budget", clamp(last_frame / 16.667, 0.0, 1.0), "16.67ms frame budget")
   gui.progress_bar("draw_budget", clamp(last_draw / 16.667, 0.0, 1.0), "Draw share")
   0
}

fn _profiler_counters(rs, p, hotspot) int {
   def peak_frame = _num(p.get("peak_frame", 0.0))
   gui.separator()
   gui.text_colored("Renderer counters", [0.84, 0.84, 0.84, 1.0])
   gui.text("Backend " + _txt(rs.get("backend", "none"), "none") + "   Draws " + to_str(_intv(rs.get("draws", 0))) + "   Flushes " + to_str(_intv(rs.get("flushes", 0))))
   gui.text("Dynamic " + to_str(_intv(rs.get("dynamic_draws", 0))) + "   Static " + to_str(_intv(rs.get("static_draws", 0))) + "   Indexed " + to_str(_intv(rs.get("indexed_draws", 0))))
   gui.text("Pipe binds " + to_str(_intv(rs.get("pipeline_binds", 0))) + "   DS binds " + to_str(_intv(rs.get("descriptor_binds", 0))) + "   Verts " + to_str(_intv(rs.get("submitted_vertices", 0))))
   gui.text_colored("Heuristic: " + _txt(hotspot, "") + "   Peak frame " + f"{peak_frame:.2f}ms", [0.72, 0.72, 0.72, 1.0])
   0
}

fn _profiler_history(p) int {
   def avg_fps, avg_frame, avg_draw, avg_ui =
   _num(p.get("avg_fps", 0.0)), _num(p.get("avg_frame", 0.0)), _num(p.get("avg_draw", 0.0)), _num(p.get("avg_ui", 0.0))
   gui.separator()
   gui.text_colored("History", [0.84, 0.84, 0.84, 1.0])
   gui.progress_bar("prof_fps_history", clamp(avg_fps / 120.0, 0.0, 1.0), "Avg FPS " + f"{avg_fps:.1f}")
   gui.progress_bar("prof_frame_history", clamp(avg_frame / 16.667, 0.0, 1.0), "Avg frame " + f"{avg_frame:.2f}ms")
   gui.progress_bar("prof_draw_history", clamp(avg_draw / 16.667, 0.0, 1.0), "Avg draw " + f"{avg_draw:.2f}ms")
   gui.progress_bar("prof_ui_history", clamp(avg_ui / 16.667, 0.0, 1.0), "Avg UI " + f"{avg_ui:.2f}ms")
   0
}

fn profiler_body(rs, p, hotspot, card_w, mini_w) int {
   "Runs the profiler body operation."
   _profiler_summary(rs, p, hotspot, card_w)
   _profiler_breakdown(p, mini_w)
   _profiler_counters(rs, p, hotspot)
   _profiler_history(p)
   0
}

fn _gallery_context_label(items, idx) str {
   to_str((is_list(items) ? items : ["Scene"]).get(int(idx), "Scene"))
}

fn _gallery_mode_label(idx) str {
   def i = int(idx)
   i == 0 ? "Edit" : (i == 1 ? "Paint" : "Bake")
}

fn _gallery_result(state, action="", progress=0.0) dict {
   {
      "tab": int(state.get("tab", 0)),
      "combo": int(state.get("combo", 0)),
      "radio": int(state.get("radio", 0)),
      "toggle_a": bool(state.get("toggle_a", true)),
      "toggle_b": bool(state.get("toggle_b", false)),
      "progress": progress,
      "float": float(state.get("float", 1.0)),
      "int": int(state.get("int", 0)),
      "accent": state.get("accent", [0.72, 0.58, 0.96, 1.0]),
      "action": action
   }
}

fn _gallery_basics(state, card_w, phase) dict {
   mut out = _gallery_result(state, "", 0.5 + 0.5 * sin(phase * 0.7))
   def items = state.get("context_items", ["Scene"])
   gui.stat_card("gallery_ctx_card", "Context", _gallery_context_label(items, out.get("combo", 0)), "active widgets", card_w, 60.0, [0.72, 0.82, 0.86, 1.0])
   gui.same_line()
   gui.stat_card("gallery_state_card", "Mode", _gallery_mode_label(out.get("radio", 0)), bool(out.get("toggle_a", true)) ? "bloom on" : "bloom off", card_w, 60.0, [0.68, 0.82, 0.74, 1.0])
   out["combo"] = gui.combo_box("demo_context", "Editor Context", items, int(out.get("combo", 0)), 0.0, 6)
   if(gui.button("primary_btn", "Primary Button", 150.0)){ out["action"] = "primary" }
   gui.same_line()
   if(gui.small_button("small_btn", "Small")){ out["action"] = "small" }
   out["toggle_a"] = gui.toggle("toggle_a", "Enable bloom", bool(out.get("toggle_a", true)))
   out["toggle_b"] = gui.checkbox("toggle_b", "Enable gizmos", bool(out.get("toggle_b", false)))
   if(gui.radio_button("radio_a", "Edit Mode", int(out.get("radio", 0)) == 0)){ out["radio"] = 0 }
   if(gui.radio_button("radio_b", "Paint Mode", int(out.get("radio", 0)) == 1)){ out["radio"] = 1 }
   if(gui.radio_button("radio_c", "Bake Mode", int(out.get("radio", 0)) == 2)){ out["radio"] = 2 }
   gui.progress_bar("gallery_basics_density", float(out.get("progress", 0.0)), "Interaction cadence")
   out
}

fn _gallery_metrics(state, card_w, compact, phase) dict {
   mut out = _gallery_result(state, "", 0.5 + 0.5 * sin(phase * 0.7))
   def rs = state.get("frame_stats", {})
   def fps = float(state.get("fps", 0.0))
   def last_frame_ms = float(state.get("last_frame_ms", 0.0))
   gui.stat_card("gallery_metric_fps", "Frame", f"{last_frame_ms:.2f}ms", to_str(int(fps)) + " fps", card_w, 64.0, [0.58, 0.86, 1.0, 1.0])
   gui.same_line()
   gui.stat_card("gallery_metric_draw", "Draws", to_str(_intv(rs.get("draws", 0))), _txt(state.get("renderer_hotspot", ""), "steady"), card_w, 64.0, [0.34, 0.88, 0.52, 1.0])
   if(!compact){
      gui.same_line()
      gui.stat_card("gallery_metric_mem", "Budget", "streaming", "cache warm", card_w, 64.0, [1.0, 0.78, 0.28, 1.0])
   }
   out["float"] = gui.slider_float("demo_float", "Viewport Exposure", float(out.get("float", 1.0)), 0.1, 4.0)
   out["int"] = gui.slider_int("demo_int", "Model Index Preview", int(out.get("int", 0)), 0, max(int(state.get("model_count", 0)) - 1, 0))
   gui.progress_bar("stream_progress", float(out.get("progress", 0.0)), "Asset streaming")
   gui.progress_bar("shader_progress", 0.18 + float(out.get("progress", 0.0)) * 0.64, "Shader compilation")
   def last_ui_ms = float(state.get("last_ui_ms", 0.0))
   def last_draw_ms = float(state.get("last_draw_ms", 0.0))
   gui.progress_bar("ui_budget_progress", clamp(last_ui_ms / 8.0, 0.0, 1.0), "UI budget " + f"{last_ui_ms:.2f}ms")
   gui.progress_bar("draw_budget_progress", clamp(last_draw_ms / 8.0, 0.0, 1.0), "Draw budget " + f"{last_draw_ms:.2f}ms")
   out
}

fn _gallery_theme(state) dict {
   mut out = _gallery_result(state, "", float(state.get("progress", 0.0)))
   out["accent"] = colorpicker.edit4("gallery_accent", "Accent Tuning", out.get("accent", [0.72, 0.58, 0.96, 1.0]), [0.72, 0.58, 0.96, 1.0])
   gui.set_accent(out.get("accent", [0.72, 0.58, 0.96, 1.0]))
   gui.text_colored("Immediate mode theme preview", out.get("accent", [0.72, 0.58, 0.96, 1.0]))
   out
}

fn gallery_body(state) dict {
   "Runs the gallery body operation."
   mut st = is_dict(state) ? state : dict(0)
   def phase = float(st.get("phase", 0.0))
   def card_w = float(st.get("card_w", 120.0))
   def compact = bool(st.get("compact", false))
   st["tab"] = gui.tab_strip("gallery_tabs", ["Basics", "Metrics", "Theme"], int(st.get("tab", 0)))
   gui.separator()
   def tab = int(st.get("tab", 0))
   if(tab == 0){ return _gallery_basics(st, card_w, phase) }
   if(tab == 1){ return _gallery_metrics(st, card_w, compact, phase) }
   _gallery_theme(st)
}

#main {
   def a = _action("x", "in", "cmd")
   assert(a.get("action", "") == "x" && a.get("command", "") == "cmd", "viewer panels action")
   print("✓ std.os.ui.render.viewer.engine.panels self-test passed")
}
