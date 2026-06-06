;; Keywords: sound diag os
;; Sound diagnostics for backend probing, device status, and playback reporting.
;; References:
;; - std.os.sound
;; - std.os
module std.os.sound.diag(enabled, probe, probe_text, print_probe)
use std.core
use std.core.dict_mod
use std.core.common as common
use std.os
use std.os.sound.backend as audio_backend

mut _debug = -1

fn enabled() bool {
   "Returns whether sound debug logging is enabled."
   _debug = common.cached_env_enabled(_debug, "NY_AUDIO_DEBUG")
   _debug == 1
}

fn probe() dict {
   "Returns a dictionary describing detected sound backend capabilities."
   mut d = dict(8)
   d = d.set("os", os())
   d = d.set("backend", audio_backend.get_backend_name())
   d
}

fn probe_text() str {
   "Returns a human-readable summary line for sound diagnostics."
   def d = probe()
   "os=" + to_str(d.get("os", "?")) +
   " backend=" + to_str(d.get("backend", "none"))
}

fn print_probe() any {
   "Prints std.os.sound diagnostics summary."
   print("std.os.sound.diag:", probe_text())
}
