;; Keywords: ui render diagnostics profile trace frame os
;; Runtime diagnostics for std.os.ui backends and graphics availability.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.diag(probe, probe_text, print_probe)
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

#main {
   def d = probe()
   assert(is_dict(d) && is_str(d.get("os", "")) && is_str(d.get("window_backend", "")), "ui diag probe")
   assert(probe_text().contains("os="), "ui diag text")
   print("✓ std.os.ui.render.diag self-test passed")
}
