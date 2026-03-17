;; Keywords: ui window event input keyboard mouse gamepad render vulkan camera scene assets texture mesh font terminal profiling dump selection
;; UI facade for windowing, events, input, rendering support, cameras, scene assets, profiling, and probes.
module std.os.ui(window, event, consts, shader_mod, gamepad, shader_transpile, probe, camera, scene, assets, dump, selection, profile, probe_text, print_probe)
use std.os.ui.window as window
use std.os.ui.event as event
use std.os.ui.consts as consts
use std.os.ui.render.shader as lib_shader
use std.os.ui.camera as camera
use std.os.ui.scene as scene
use std.os.ui.assets as assets
use std.os.ui.dump as dump
use std.os.ui.selection as selection
use std.os.ui.profile as profile
use std.os.ui.window.input.gamepad as gamepad
use std.core
use std.core.common as common

fn shader_transpile(str: combined_src): any {
   "Proxy to graphics shader transpiler."
   lib_shader.transpile_shader_source(combined_src)
}

fn probe(): dict {
   "Performs a lightweight diagnostic probe of the UI system."
   {
      "os": __os_name(),
      "window_backend": window.backend(),
      "window_available": window.available(),
      "vulkan": false,
      "detail": "import std.os.ui.diag for renderer/Vulkan diagnostics"
   }
}

fn probe_text(): str {
   "Returns lightweight diagnostic information as a string."
   def d = probe()
   "os=" + to_str(d.get("os", "?")) +
   " window=" + to_str(d.get("window_backend", "ny")) +
   " available=" + common.yn(d.get("window_available", false)) +
   " vk=deferred"
}

fn print_probe(): any {
   "Prints diagnostic information to stdout."
   print("std.os.ui:", probe_text())
}
