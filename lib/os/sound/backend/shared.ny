;; Keywords: sound backend shared
;; Shared probing and backend-selection code for platform sound drivers.
module std.os.sound.backend.shared(append_output_device, init_output_device, probe_linux_library, probe_linux_library_once)
use std.core
use std.core.dict_mod

fn append_output_device(any: ctx, str: name, str: id): any {
   "Appends a default output device entry with display `name` and backend `id` to `ctx`."
   if(!ctx){ return 0 }
   mut dev = dict(8)
   dev = dev.set("name", name)
   dev = dev.set("id", id)
   dev = dev.set("ctx", ctx)
   mut devices = ctx.get("devices", list())
   if(!is_list(devices)){ devices = list() }
   devices = devices.append(dev)
   ctx.set("devices", devices)
}

fn init_output_device(any: ctx, any: ready, str: name, str: id): any {
   "Registers a default output device when backend readiness flag `ready` is truthy."
   if(!ready){ return 0 }
   append_output_device(ctx, name, id)
}

fn probe_linux_library(str: name, str: required_symbol): int {
   "Returns 1 on Linux(library linked via #include), 0 on other platforms."
   #linux { return 1 }
   #endif
   0
}

fn probe_linux_library_once(int: avail, any: lib_handle, str: name, str: required_symbol): list {
   "Returns `[1, 0]` on Linux(library linked via #include), `[0, 0]` otherwise."
   if(avail != -1){ return [avail, lib_handle] }
   #linux { return [1, 0] }
   #endif
   [0, 0]
}
