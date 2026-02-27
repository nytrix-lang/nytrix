;; Keywords: ui window wayland
;; Wayland Backend for std.ui.window (Preliminary)

module std.ui.backend.wayland (
   available, create_native_window, poll_events, swap_buffers, make_current, blit_buffer
)
use std.core *
use std.os.ffi *
use std.os.sys *
use std.ui.consts *
use std.ui.event as ev
use std.text *
use std.os *

mut _wl_handle = 0
mut _disp = 0
mut _wl_display_connect = 0
mut _wl_display_disconnect = 0
mut _wl_display_roundtrip = 0
mut _wl_display_dispatch = 0
mut _wl_display_dispatch_pending = 0
mut _wl_display_flush = 0
mut _wl_display_get_registry = 0
mut _wl_proxy_add_listener = 0
mut _wl_proxy_marshal_flags = 0
mut _wl_proxy_marshal_array_flags = 0

mut _registry_ptr = 0
mut _compositor_ptr = 0
mut _shm_ptr = 0
mut _seat_ptr = 0
mut _xdg_wm_base_ptr = 0

;; Core interfaces we query from libwayland-client
mut _wl_compositor_interface = 0
mut _wl_seat_interface = 0
mut _wl_shm_interface = 0
mut _wl_shm_pool_interface = 0
mut _wl_buffer_interface = 0

;; Our custom xdg-shell interfaces dynamically constructed in memory
mut _xdg_wm_base_interface = 0
mut _xdg_surface_interface = 0
mut _xdg_toplevel_interface = 0

;; Native callback pointers
mut _registry_listener_ptr = 0
mut _xdg_surface_listener_ptr = 0
mut _xdg_wm_base_listener_ptr = 0
mut _xdg_toplevel_listener_ptr = 0

fn _touch(...args){
   "Auto-generated docstring: _touch."
   len(args)
}

fn _new_wl_interface(name, version, req_cnt, req_ptr, ev_cnt, ev_ptr){
   "Auto-generated docstring: _new_wl_interface."
   def ptr = malloc(48)
   store64(ptr, to_int(str_ptr(name)))
   store32(ptr + 8, version)
   store32(ptr + 12, req_cnt)
   store64(ptr + 16, to_int(req_ptr))
   store32(ptr + 24, ev_cnt)
   store64(ptr + 32, to_int(ev_ptr))
   ptr
}

fn _setup_xdg_interfaces(){
   "Auto-generated docstring: _setup_xdg_interfaces."
   if(_xdg_wm_base_interface != 0){ return 0 }
   ;; xdg_wm_base events: ping(u)
   def xdg_wm_base_events = malloc(24)
   store64(xdg_wm_base_events, to_int(str_ptr("ping")))
   store64(xdg_wm_base_events + 8, to_int(str_ptr("u")))
   store64(xdg_wm_base_events + 16, 0)
   _xdg_wm_base_interface = _new_wl_interface("xdg_wm_base", 1, 0, 0, 1, xdg_wm_base_events)
   ;; xdg_surface events: configure(u)
   def xdg_surface_events = malloc(24)
   store64(xdg_surface_events, to_int(str_ptr("configure")))
   store64(xdg_surface_events + 8, to_int(str_ptr("u")))
   store64(xdg_surface_events + 16, 0)
   _xdg_surface_interface = _new_wl_interface("xdg_surface", 1, 0, 0, 1, xdg_surface_events)
   ;; xdg_toplevel events: configure(iia), close()
   def xdg_toplevel_events = malloc(48)
   store64(xdg_toplevel_events, to_int(str_ptr("configure")))
   store64(xdg_toplevel_events + 8, to_int(str_ptr("iia")))
   store64(xdg_toplevel_events + 16, 0)
   store64(xdg_toplevel_events + 24, to_int(str_ptr("close")))
   store64(xdg_toplevel_events + 32, to_int(str_ptr("")))
   store64(xdg_toplevel_events + 40, 0)
   _xdg_toplevel_interface = _new_wl_interface("xdg_toplevel", 1, 0, 0, 2, xdg_toplevel_events)
}

fn _registry_global(data, registry, id, interface_name_ptr, version){
   "Auto-generated docstring: _registry_global."
   _touch(data, version)
   def name = str_from_ptr(interface_name_ptr)
   if(name == "wl_compositor"){
      _compositor_ptr = call(_wl_proxy_marshal_flags, registry, 0, _wl_compositor_interface, 1, 0, id, interface_name_ptr, 1, 0)
   } elif(name == "wl_shm"){
      _shm_ptr = call(_wl_proxy_marshal_flags, registry, 0, _wl_shm_interface, 1, 0, id, interface_name_ptr, 1, 0)
   } elif(name == "wl_seat"){
      _seat_ptr = call(_wl_proxy_marshal_flags, registry, 0, _wl_seat_interface, 1, 0, id, interface_name_ptr, 1, 0)
   } elif(name == "xdg_wm_base"){
      _xdg_wm_base_ptr = call(_wl_proxy_marshal_flags, registry, 0, _xdg_wm_base_interface, 1, 0, id, interface_name_ptr, 1, 0)
   }
   0
}

fn _registry_global_remove(data, registry, id){
   "Auto-generated docstring: _registry_global_remove."
   _touch(data, registry, id)
   0
}

fn _xdg_wm_base_ping(data, wm_base, serial){
   "Auto-generated docstring: _xdg_wm_base_ping."
   _touch(data)
   ;; Pong back (opcode 3)
   call(_wl_proxy_marshal_flags, wm_base, 3, 0, 1, 0, serial)
   0
}

fn _xdg_surface_configure(data, surface, serial){
   "Auto-generated docstring: _xdg_surface_configure."
   ;; Ack configure (opcode 4)
   call(_wl_proxy_marshal_flags, surface, 4, 0, 1, 0, serial)
   ;; Commit the surface after ack (opcode 6 in wl_surface)
   ;; In real code, we would render here. The user `data` ptr points to our `win`.
   if(data != 0){
      def surf_ptr = get(data, 22, 0)
      if(surf_ptr != 0){
         call(_wl_proxy_marshal_flags, surf_ptr, 6, 0, 1, 0)
      }
   }
   0
}

fn _wl_emit(win, kind, data=0){
   "Auto-generated docstring: _wl_emit."
   if(win == 0){ return 0 }
   def q = get(win, 10, 0)
   set_idx(win, 10, ev.queue_push(q, ev.make_event(kind, win, get(win, 1), data)))
   0
}

fn _xdg_toplevel_configure(data, toplevel, w, h, states){
   "Auto-generated docstring: _xdg_toplevel_configure."
   _touch(toplevel, states)
   if(data == 0){ return 0 }
   mut nw = w
   mut nh = h
   if(nw <= 0){ nw = get(data, 5, 1) }
   if(nh <= 0){ nh = get(data, 6, 1) }
   if(nw < 1){ nw = 1 }
   if(nh < 1){ nh = 1 }
   def old_w = get(data, 5, nw)
   def old_h = get(data, 6, nh)
   if(nw != old_w || nh != old_h){
      set_idx(data, 5, nw)
      set_idx(data, 6, nh)
      mut r = dict(8)
      r = dict_set(r, "x", get(data, 3, 0))
      r = dict_set(r, "y", get(data, 4, 0))
      r = dict_set(r, "w", nw)
      r = dict_set(r, "h", nh)
      _wl_emit(data, EVENT_WINDOW_RESIZED, r)
   }
   0
}

fn _xdg_toplevel_close(data, toplevel){
   "Auto-generated docstring: _xdg_toplevel_close."
   _touch(toplevel)
   if(data != 0){
      set_idx(data, 8, true)
      _wl_emit(data, EVENT_QUIT, 0)
   }
   0
}

mut _debug = -1

fn _is_debug(){
   "Auto-generated docstring: _is_debug."
   if(_debug == -1){
      def v = env("NY_UI_DEBUG")
      _debug = (v && (eq(v, "1") || eq(v, "true"))) ? 1 : 0
   }
   _debug
}

fn available(){
   "Auto-generated docstring: available."
   def w_disp_env = env("WAYLAND_DISPLAY")
   def xdg_sess = env("XDG_SESSION_TYPE")
   if(str_len(w_disp_env) == 0 && xdg_sess != "wayland"){ return false }
   if(_disp != 0){ return true }
   if(_wl_handle == 0){
      if(_is_debug()){ print("Wayland: Loading libwayland-client...") }
      _wl_handle = dlopen_any("wayland-client", RTLD_NOW())
      if(_wl_handle == 0){ 
         if(_is_debug()){ print("Wayland: Failed to load libwayland-client.") }
         return false 
      }
      _wl_display_connect = bind(_wl_handle, "wl_display_connect")
      _wl_display_disconnect = bind(_wl_handle, "wl_display_disconnect")
      _wl_display_roundtrip = bind(_wl_handle, "wl_display_roundtrip")
      _wl_display_dispatch = bind(_wl_handle, "wl_display_dispatch")
      _wl_display_dispatch_pending = bind(_wl_handle, "wl_display_dispatch_pending")
      _wl_display_flush = bind(_wl_handle, "wl_display_flush")
      _wl_display_get_registry = bind(_wl_handle, "wl_display_get_registry")
      _wl_proxy_add_listener = bind(_wl_handle, "wl_proxy_add_listener")
      _wl_proxy_marshal_flags = bind(_wl_handle, "wl_proxy_marshal_flags")
      _wl_proxy_marshal_array_flags = bind(_wl_handle, "wl_proxy_marshal_array_flags")
      if(_wl_display_connect == 0 || _wl_proxy_marshal_flags == 0){ return false }
      _wl_compositor_interface = dlsym(_wl_handle, "wl_compositor_interface")
      _wl_seat_interface = dlsym(_wl_handle, "wl_seat_interface")
      _wl_shm_interface = dlsym(_wl_handle, "wl_shm_interface")
      _wl_shm_pool_interface = dlsym(_wl_handle, "wl_shm_pool_interface")
      _wl_buffer_interface = dlsym(_wl_handle, "wl_buffer_interface")
      _setup_xdg_interfaces()
   }
   ;; Attempt connection
   _disp = call1_i64(_wl_display_connect, 0)
   if(_disp == 0){ 
      if(_is_debug()){ print("Wayland: Could not connect to display.") }
      return false 
   }
   if(_is_debug()){ print(f"Wayland: Connected to display at 0x{hex(to_int(_disp))}") }
   ;; Get registry and bind globals
   _registry_ptr = call1_i64(_wl_display_get_registry, _disp)
   _registry_listener_ptr = malloc(16)
   store64(_registry_listener_ptr, to_int(fn_ptr(_registry_global)))
   store64(_registry_listener_ptr + 8, to_int(fn_ptr(_registry_global_remove)))
   call3(_wl_proxy_add_listener, _registry_ptr, _registry_listener_ptr, 0)
   _xdg_wm_base_listener_ptr = malloc(8)
   store64(_xdg_wm_base_listener_ptr, to_int(fn_ptr(_xdg_wm_base_ping)))
   _xdg_surface_listener_ptr = malloc(8)
   store64(_xdg_surface_listener_ptr, to_int(fn_ptr(_xdg_surface_configure)))
   _xdg_toplevel_listener_ptr = malloc(16)
   store64(_xdg_toplevel_listener_ptr, to_int(fn_ptr(_xdg_toplevel_configure)))
   store64(_xdg_toplevel_listener_ptr + 8, to_int(fn_ptr(_xdg_toplevel_close)))
   ;; Wait for globals
   call1(_wl_display_roundtrip, _disp)
   ;; Listen to xdg_wm_base for pings
   if(_xdg_wm_base_ptr != 0){
       call3(_wl_proxy_add_listener, _xdg_wm_base_ptr, _xdg_wm_base_listener_ptr, 0)
   }
   _disp != 0
}

fn create_native_window(win){
   "Auto-generated docstring: create_native_window."
   if(!available() || _compositor_ptr == 0 || _xdg_wm_base_ptr == 0){ return false }
   def title = get(win, 2, "Window")
   def w = get(win, 5, 800)
   def h = get(win, 6, 600)
   def wl_surface_interface = dlsym(_wl_handle, "wl_surface_interface")
   def surface_ptr = call(_wl_proxy_marshal_flags, _compositor_ptr, 0, wl_surface_interface, 1, 0, 0)
   set_idx(win, 22, surface_ptr)
   ;; Create XDG Surface
   def xdg_surface_ptr = call(_wl_proxy_marshal_flags, _xdg_wm_base_ptr, 2, _xdg_surface_interface, 1, 0, 0, surface_ptr)
   set_idx(win, 23, xdg_surface_ptr)
   call3(_wl_proxy_add_listener, xdg_surface_ptr, _xdg_surface_listener_ptr, ptr_to_i64(win))
   ;; Create XDG Toplevel
   def xdg_toplevel_ptr = call(_wl_proxy_marshal_flags, xdg_surface_ptr, 1, _xdg_toplevel_interface, 1, 0, 0)
   if(_xdg_toplevel_listener_ptr != 0){
      call3(_wl_proxy_add_listener, xdg_toplevel_ptr, _xdg_toplevel_listener_ptr, ptr_to_i64(win))
   }
   ;; Set Title
   call(_wl_proxy_marshal_flags, xdg_toplevel_ptr, 2, 0, 1, 0, str_ptr(title))
   ;; Commit surface initially to trigger configure
   call(_wl_proxy_marshal_flags, surface_ptr, 6, 0, 1, 0)
   call1(_wl_display_roundtrip, _disp)
   ;; Hack: create a dummy transparent buffer if SHM is available, just so the window maps immediately!
   if(_shm_ptr != 0){
      def size = w * h * 4
      def fd = __syscall(319, str_ptr("nytrix-wl"), 0, 0, 0, 0, 0) ;; memfd_create
      if(fd >= 0){
         __syscall(77, fd, size, 0, 0, 0, 0) ;; ftruncate
         ;; create pool: wl_proxy_marshal_flags(shm, 0, wl_shm_pool_interface, 1, 0, NULL, fd, size)
         def args_pool = malloc(24)
         store64(args_pool, 0)
         store64(args_pool + 8, fd)
         store64(args_pool + 16, size)
         def pool = call(_wl_proxy_marshal_array_flags, _shm_ptr, 0, _wl_shm_pool_interface, 1, 0, args_pool)
         free(args_pool)
         ;; create buffer: pool->create_buffer(new_id, offset, w, h, stride, format=WL_SHM_FORMAT_ARGB8888=0)
         def args_buf = malloc(48)
         store64(args_buf, 0) ;; new_id object
         store64(args_buf + 8, 0) ;; offset
         store64(args_buf + 16, w)
         store64(args_buf + 24, h)
         store64(args_buf + 32, w * 4) ;; stride
         store64(args_buf + 40, 0) ;; format ARGB8888
         def buffer = call(_wl_proxy_marshal_array_flags, pool, 0, _wl_buffer_interface, 1, 0, args_buf)
         free(args_buf)
         ;; attach buffer
         def args_attach = malloc(24)
         store64(args_attach, buffer)
         store64(args_attach + 8, 0)
         store64(args_attach + 16, 0)
         call(_wl_proxy_marshal_array_flags, surface_ptr, 1, 0, 1, 0, args_attach)
         free(args_attach)
         ;; damage buffer (opcode 2)
         def args_dmg = malloc(32)
         store64(args_dmg, 0)
         store64(args_dmg + 8, 0)
         store64(args_dmg + 16, w)
         store64(args_dmg + 24, h)
         call(_wl_proxy_marshal_array_flags, surface_ptr, 2, 0, 1, 0, args_dmg)
         free(args_dmg)
         ;; commit surface
         call(_wl_proxy_marshal_flags, surface_ptr, 6, 0, 1, 0)
         __syscall(3, fd, 0, 0, 0, 0, 0) ;; close fd
      }
   }
   call1(_wl_display_roundtrip, _disp)
   return true
}

fn poll_events(win){
   "Auto-generated docstring: poll_events."
   _touch(win)
   if(_disp == 0){ return 0 }
   call1(_wl_display_dispatch_pending, _disp)
   call1(_wl_display_flush, _disp)
   0
}

fn swap_buffers(win){
   "Auto-generated docstring: swap_buffers."
   _touch(win)
   ;; TODO: Implement EGL/Wayland swap
}
fn make_current(win){
   "Auto-generated docstring: make_current."
   _touch(win)
   ;; TODO: Implement EGL/Wayland make_current
}
fn blit_buffer(win, buf, w, h){
   "Auto-generated docstring: blit_buffer."
   _touch(win, buf, w, h)
   0
}
