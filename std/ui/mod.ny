;; Keywords: ui
;; Aggregated std.ui entrypoint.

module std.ui (
   window, event, consts, gfx, shader_mod, diag,
   shader_transpile, probe, probe_text, print_probe
)

use std.ui.window as window
use std.ui.event as event
use std.ui.consts as consts
use std.ui.gfx as gfx
use std.ui.gfx.shader as shader_mod
use std.ui.diag as diag

fn shader_transpile(combined_src){
   "Proxy to graphics shader transpiler."
   shader_mod.transpile_shader_source(combined_src)
}

fn probe(){
   "Performs a diagnostic probe of the UI system."
   diag.probe()
}

fn probe_text(){
   "Returns diagnostic information as a string."
   diag.probe_text()
}

fn print_probe(){
   "Prints diagnostic information to stdout."
   diag.print_probe()
}
