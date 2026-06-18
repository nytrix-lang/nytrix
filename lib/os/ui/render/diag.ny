;; Keywords: ui render diagnostics profile trace frame os
;; Runtime diagnostics for std.os.ui backends and graphics availability.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.diag(probe, probe_text, print_probe, snapshot, snapshot_text, print_snapshot)
use std.core
use std.os.ui.window as window
use std.os.ui.render as render
use std.core.common as common

fn probe() dict {
   "Returns a dictionary describing detected backend and graphics capabilities."
   def caps = render.backend_capabilities()
   return {
      "os": __os_name(),
      "window_backend": window.backend(),
      "window_available": window.available(),
      "renderer": caps.get("active", "none"),
      "vulkan": caps.get("vulkan", false),
      "opengl": caps.get("opengl", false),
      "webgl": caps.get("webgl", false),
      "software": caps.get("software", false),
      "double_buffered": caps.get("double_buffered", false)
   }
}

fn probe_text() str {
   "Returns a human-readable summary line for backend diagnostics."
   def d = probe()
   "os=" + to_str(d.get("os", "?")) +
   " window=" + to_str(d.get("window_backend", "ny")) +
   " available=" + common.yn(d.get("window_available", false)) +
   " renderer=" + to_str(d.get("renderer", "none")) +
   " gpu=" + common.yn(d.get("double_buffered", false)) +
   " gl=" + common.yn(d.get("opengl", false)) +
   " vk=" + common.yn(d.get("vulkan", false)) +
   " webgl=" + common.yn(d.get("webgl", false))
}

fn print_probe() any {
   "Prints std.os.ui diagnostics summary."
   print("std.os.ui.render.diag:", probe_text())
}

fn snapshot(any win=0) dict {
   "Returns a compact live UI snapshot for debugger overlays, logs, and probes."
   def w = win ? win : window.last()
   def sz = w ? window.size(w) : [0, 0]
   def fb = w ? window.get_framebuffer_size(w) : [0, 0]
   def scale = w ? window.get_window_content_scale(w) : [1.0, 1.0]
   def mouse = w ? window.mouse_pos(w) : [0, 0]
   def rs = render.renderer_frame_stats()
   {
      "os": __os_name(),
      "window_backend": window.backend(),
      "window_available": window.available(),
      "renderer": render.get_active_backend_name(),
      "win_w": int(sz.get(0, 0)),
      "win_h": int(sz.get(1, 0)),
      "fb_w": int(fb.get(0, 0)),
      "fb_h": int(fb.get(1, 0)),
      "scale_x": float(scale.get(0, 1.0)),
      "scale_y": float(scale.get(1, 1.0)),
      "mouse_x": float(mouse.get(0, 0.0)),
      "mouse_y": float(mouse.get(1, 0.0)),
      "should_close": w ? window.should_close(w) : true,
      "draws": int(rs.get("draws", 0)),
      "flushes": int(rs.get("flushes", 0)),
      "text_calls": int(rs.get("prim_text_calls", 0)),
      "text_glyphs": int(rs.get("prim_text_glyphs", 0)),
      "cpu_ms": float(rs.get("cpu_ms", 0.0)),
   }
}

fn snapshot_text(any win=0) str {
   "Returns a one-line live UI snapshot."
   def d = snapshot(win)
   "ui.snapshot os=" + to_str(d.get("os", "?")) +
   " window=" + to_str(d.get("window_backend", "?")) +
   " renderer=" + to_str(d.get("renderer", "?")) +
   " win=" + to_str(d.get("win_w", 0)) + "x" + to_str(d.get("win_h", 0)) +
   " fb=" + to_str(d.get("fb_w", 0)) + "x" + to_str(d.get("fb_h", 0)) +
   " scale=" + to_str(d.get("scale_x", 1.0)) + "x" + to_str(d.get("scale_y", 1.0)) +
   " mouse=" + to_str(d.get("mouse_x", 0.0)) + "," + to_str(d.get("mouse_y", 0.0)) +
   " draws=" + to_str(d.get("draws", 0)) +
   " flushes=" + to_str(d.get("flushes", 0)) +
   " text=" + to_str(d.get("text_calls", 0)) + "/" + to_str(d.get("text_glyphs", 0)) +
   " cpu_ms=" + to_str(d.get("cpu_ms", 0.0)) +
   " close=" + common.yn(d.get("should_close", false))
}

fn print_snapshot(any win=0) any {
   "Prints a one-line live UI snapshot."
   print(snapshot_text(win))
}

#main {
   def d = probe()
   assert(is_dict(d) && is_str(d.get("os", "")) && is_str(d.get("window_backend", "")), "ui diag probe")
   assert(probe_text().contains("os="), "ui diag text")
   assert(snapshot_text().contains("ui.snapshot"), "ui diag snapshot")
   print("✓ std.os.ui.render.diag self-test passed")
}
