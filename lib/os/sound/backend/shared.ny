;; Keywords: sound backend shared os
;; Shared probing and backend-selection code for platform sound drivers.
;; References:
;; - core.os.sound.backend
;; - core.os
module core.os.sound.backend.shared(append_output_device, init_output_device, probe_linux_library, probe_linux_library_once)
use core
use core.dict_mod
use core.os.ffi as ffi

fn append_output_device(any ctx, str name, str id) any {
   "Appends a default output device entry with display `name` and backend `id` to `ctx`."
   if !ctx { return 0 }
   mut dev = dict(8)
   dev = dev.set("name", name)
   dev = dev.set("id", id)
   dev = dev.set("ctx", ctx)
   mut devices = ctx.get("devices", list())
   if !is_list(devices) { devices = list() }
   devices = devices.append(dev)
   ctx.set("devices", devices)
}

fn init_output_device(any ctx, any ready, str name, str id) any {
   "Registers a default output device when backend readiness flag `ready` is truthy."
   if !ready { return 0 }
   append_output_device(ctx, name, id)
}

fn probe_linux_library(str name, str required_symbol) int {
   "Returns 1 when the Linux library and required symbol are loadable."
   #linux {
      def h = ffi.dlopen(name, ffi.RTLD_NOW() | ffi.RTLD_GLOBAL())
      if h == 0 { return 0 }
      if ffi.dlsym(h, required_symbol) == 0 {
         ffi.dlclose(h)
         return 0
      }
      ffi.dlclose(h)
      return 1
   }
   #endif
   0
}

fn probe_linux_library_once(int avail, any lib_handle, str name, str required_symbol) list {
   "Loads and validates a Linux backend library once, returning `[available, handle]`."
   if avail >= 0 { return [avail, lib_handle] }
   #linux {
      def h = ffi.dlopen(name, ffi.RTLD_NOW() | ffi.RTLD_GLOBAL())
      if h == 0 { return [0, 0] }
      if ffi.dlsym(h, required_symbol) != 0 { return [1, h] }
      ffi.dlclose(h)
   }
   #endif
   [0, 0]
}
