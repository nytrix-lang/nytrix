;; Keywords: ui diagnostics backend
;; Runtime diagnostics for std.ui backends and graphics availability.

module std.ui.diag (
   probe, probe_text, print_probe
)

use std.core *
use std.os *
use std.ui.window as window
use std.ui.gfx.vulkan *

fn _yn(v){
   "Internal helper to convert a boolean to 'yes' or 'no'."
   if(v){ return "yes" }
   "no"
}

fn probe(){
   "Returns a dictionary describing detected backend and graphics capabilities."
   mut d = dict(8)
   d = dict_set(d, "os", __os_name())
   d = dict_set(d, "window_backend", window.backend())
   d = dict_set(d, "window_available", window.available())
   d = dict_set(d, "vulkan", vk_available())
   d
}

fn probe_text(){
   "Returns a human-readable summary line for backend diagnostics."
   def d = probe()
   "os=" + to_str(dict_get(d, "os", "?")) +
   " window=" + to_str(dict_get(d, "window_backend", "ny")) +
   " available=" + _yn(dict_get(d, "window_available", false)) +
   " vk=" + _yn(dict_get(d, "vulkan", false))
}

fn print_probe(){
   "Prints std.ui diagnostics summary."
   print("std.ui.diag:", probe_text())
}

if(comptime{__main()}){
   use std.core.error *

   def d = probe()
   assert(is_dict(d), "diag probe dict")
   assert(is_str(dict_get(d, "os", "")), "diag os string")
   assert(is_str(dict_get(d, "window_backend", "")), "diag backend string")
   print("✓ std.ui.diag tests passed")
}
