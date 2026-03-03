;; Keywords: sound diagnostics backend

module std.os.audio.diag (
   probe, probe_text, print_probe
)

use std.core *
use std.core.dict_mod *
use std.os *
use std.os.audio.backend as backend

fn _yn(v){
   "Internal helper for `yn`."
   if(v){ return "yes" }
   "no"
}

fn probe(){
   "Returns a dictionary describing detected sound backend capabilities."
   mut d = dict(8)
   d = dict_set(d, "os", os())
   d = dict_set(d, "backend", backend.get_backend_name())
   d
}

fn probe_text(){
   "Returns a human-readable summary line for sound diagnostics."
   def d = probe()
   "os=" + to_str(get(d, "os", "?")) +
   " backend=" + to_str(get(d, "backend", "none"))
}

fn print_probe(){
   "Prints std.os.audio diagnostics summary."
   print("std.os.audio.diag:", probe_text())
}
