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
   "Auto-generated docstring: shader_transpile."
   shader_mod.transpile_shader_source(combined_src)
}

fn probe(){
   "Auto-generated docstring: probe."
   diag.probe()
}

fn probe_text(){
   "Auto-generated docstring: probe_text."
   diag.probe_text()
}

fn print_probe(){
   "Auto-generated docstring: print_probe."
   diag.print_probe()
}
