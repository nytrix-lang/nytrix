;; Keywords: ui
;; Standard UI Entrypoint for Nytrix

module std.ui (
   window, event, consts, gfx, shader_mod, diag, terminal, vterm, gamepad,
   shader_transpile, probe, probe_text, print_probe
)

use std.ui.window as window
use std.ui.event as event
use std.ui.consts as consts
use std.ui.gfx as gfx
use std.ui.gfx.shader as lib_shader
use std.ui.diag as diag
use std.ui.gfx.term as terminal
use std.ui.gfx.vterm as vterm
use std.ui.window.input.gamepad as gamepad

fn shader_transpile(combined_src){
   "Proxy to graphics shader transpiler."
   lib_shader.transpile_shader_source(combined_src)
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
