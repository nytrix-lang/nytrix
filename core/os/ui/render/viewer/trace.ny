;; Structured viewer diagnostics with an optional application-owned sink.
module core.os.ui.render.viewer.trace(set_sink, emit)
use core

mut _sink = nil

fn set_sink(any sink=nil) int {
   "Installs a callback accepting a diagnostic record; nil restores stdout."
   _sink = sink
   0
}

fn emit(str kind, int frame, str message) int {
   "Routes diagnostics through one sink while preserving default CLI output."
   def record = {"kind": kind, "frame": frame, "message": message}
   def sink = _sink
   if sink != nil { sink(record) }
   else { print(message) }
   0
}
