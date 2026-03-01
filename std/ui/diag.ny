;; Keywords: ui diagnostics backend
;; Runtime diagnostics for std.ui backends and graphics availability.

module std.ui.diag (
   probe, probe_text, print_probe
)

use std.core *
use std.os *
use std.ui.window as window
use std.ui.backend as ui_backend
use std.ui.gfx.vulkan *

fn _yn(v){
   "Internal helper to convert a boolean to 'yes' or 'no'."
   if(v){ return "yes" }
   "no"
}

fn probe(){
   "Returns a dictionary describing detected backend and graphics capabilities."
   mut d = dict(16)
   d = dict_set(d, "os", __os_name())
   d = dict_set(d, "window_backend", window.backend())
   d = dict_set(d, "window_available", window.available())
   d = dict_set(d, "x11", ui_backend.x11_available())
   d = dict_set(d, "wayland", ui_backend.wayland_available())
   d = dict_set(d, "win32", ui_backend.win32_available())
   d = dict_set(d, "cocoa", ui_backend.cocoa_available())
   d = dict_set(d, "vulkan", vk_available())
   d
}

fn probe_text(){
   "Returns a human-readable summary line for backend diagnostics."
   def d = probe()
   "os=" + to_str(get(d, "os", "?")) +
   " window=" + to_str(get(d, "window_backend", "ny")) +
   " available=" + _yn(get(d, "window_available", false)) +
   " x11=" + _yn(get(d, "x11", false)) +
   " wayland=" + _yn(get(d, "wayland", false)) +
   " win32=" + _yn(get(d, "win32", false)) +
   " cocoa=" + _yn(get(d, "cocoa", false)) +
   " vk=" + _yn(get(d, "vulkan", false))
}

fn print_probe(){
   "Prints std.ui diagnostics summary."
   print("std.ui.diag:", probe_text())
}

if(comptime{__main()}){
   use std.core.error *

   def d = probe()
   assert(is_dict(d), "diag probe dict")
   assert(is_str(get(d, "os", "")), "diag os string")
   assert(is_str(get(d, "window_backend", "")), "diag backend string")
   print("✓ std.ui.diag tests passed")
}

