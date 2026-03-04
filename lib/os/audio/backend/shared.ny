;; Keywords: audio backend shared
;; Shared helpers for platform audio backends.

module std.os.audio.backend.shared (
   append_output_device, init_output_device, probe_linux_library, probe_linux_library_once
)

use std.core *
use std.core.dict_mod *

fn append_output_device(ctx, name, id){
   "Appends a default output device entry with display `name` and backend `id` to `ctx`."
   if(!ctx){ return 0 }
   mut dev = dict(8)
   dev = dict_set(dev, "name", name)
   dev = dict_set(dev, "id", id)
   dev = dict_set(dev, "ctx", ctx)
   mut devices = dict_get(ctx, "devices", list())
   if(!is_list(devices)){ devices = list() }
   devices = append(devices, dev)
   dict_set(ctx, "devices", devices)
}

fn init_output_device(ctx, ready, name, id){
   "Registers a default output device when backend readiness flag `ready` is truthy."
   if(!ready){ return 0 }
   append_output_device(ctx, name, id)
}

fn probe_linux_library(name, required_symbol){
   "Returns 1 on Linux (library linked via #include), 0 on other platforms."
   if(!comptime{ __os_name() == "linux" }){ return 0 }
   1
}

fn probe_linux_library_once(avail, handle, name, required_symbol){
   "Returns `[1, 0]` on Linux (library linked via #include), `[0, 0]` otherwise."
   if(avail != -1){ return [avail, handle] }
   if(!comptime{ __os_name() == "linux" }){ return [0, 0] }
   [1, 0]
}
