;; Keywords: sound diag os
;; Sound diagnostics for backend probing, device status, and playback reporting.
;; References:
;; - core.os.sound
;; - core.os
module core.os.sound.diag(enabled, probe, probe_text, print_probe)
use core
use core.dict_mod
use core.common as common
use core.os
use core.os.sound.backend as audio_backend

mut _debug = false
mut _debug_cached = false

fn enabled() bool {
   "Returns whether sound debug logging is enabled."
   if !_debug_cached {
      _debug = common.env_enabled("NY_AUDIO_DEBUG")
      _debug_cached = true
   }
   _debug
}

fn probe() dict {
   "Returns a dictionary describing detected sound backend capabilities."
   mut d = dict(8)
   d = d.set("os", os())
   d = d.set("backend", audio_backend.get_backend_name())
   d = d.set("drivers", audio_backend.available_backends())
   d = d.set("ready", audio_backend.is_ready())
   d
}

fn probe_text() str {
   "Returns a human-readable summary line for sound diagnostics."
   def d = probe()
   "os=" + to_str(d.get("os", "?")) +
   " backend=" + to_str(d.get("backend", "none")) +
   " ready=" + to_str(d.get("ready", false)) +
   " drivers=" + to_str(d.get("drivers", []))
}

fn print_probe() any {
   "Prints core.os.sound diagnostics summary."
   print("core.os.sound.diag:", probe_text())
}
