;; Keywords: ui render diagnostics profile trace frame os
;; Runtime diagnostics for std.os.ui backends and graphics availability.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.diag(probe, probe_text, print_probe)
use std.core
use std.os.ui.window as window
use std.os.ui.render as render
use std.os.ui.render.vk.vulkan
use std.core.common as common

fn probe() dict {
   "Returns a dictionary describing detected backend and graphics capabilities."
   return {
      "os": __os_name(),
      "window_backend": window.backend(),
      "window_available": window.available(),
      "vulkan": render.vk_available()
   }
}

fn probe_text() str {
   "Returns a human-readable summary line for backend diagnostics."
   def d = probe()
   "os=" + to_str(d.get("os", "?")) +
   " window=" + to_str(d.get("window_backend", "ny")) +
   " available=" + common.yn(d.get("window_available", false)) +
   " vk=" + common.yn(d.get("vulkan", false))
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
