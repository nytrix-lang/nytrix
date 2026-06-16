;; Keywords: platform window backend opengl egl os ui input
;; EGL context creation for Wayland and X11 window backends.
;; References:
;; - std.os.ui.window.platform.opengl
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform.opengl.egl(create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address, init_display, choose_config, create_surface, destroy_surface, get_config_attrib)
use std.core
use std.core.mem (cstr)
use std.os.ui.window.platform.api as backend_api

#linux {
   #link "libEGL.so"
   extern "EGL" {
      fn eglGetDisplay(ptr native_display) ptr
      fn eglInitialize(ptr display, ptr major, ptr minor) i32
      fn eglChooseConfig(ptr display, ptr attrs, ptr configs, i32 config_size, ptr num_configs) i32
      fn eglBindAPI(i32 api) i32
      fn eglCreateContext(ptr display, ptr config, ptr share, ptr attrs) ptr
      fn eglCreateWindowSurface(ptr display, ptr config, ptr native_window, ptr attrs) ptr
      fn eglDestroySurface(ptr display, ptr surface) i32
      fn eglGetConfigAttrib(ptr display, ptr config, i32 attrib, ptr value) i32
      fn eglDestroyContext(ptr display, ptr ctx) i32
      fn eglMakeCurrent(ptr display, ptr draw, ptr read, ptr ctx) i32
      fn eglSwapBuffers(ptr display, ptr surface) i32
      fn eglSwapInterval(ptr display, i32 interval) i32
      fn eglGetProcAddress(ptr name) ptr
   }
} #endif
#windows {
   fn eglGetDisplay(any _native_display) any {
      "Runs the eglGetDisplay operation."
      0
   }
   fn eglInitialize(any _display, any _major, any _minor) int {
      "Runs the eglInitialize operation."
      0
   }
   fn eglChooseConfig(any _display, any _attrs, any _configs, int _config_size, any _num_configs) int {
      "Runs the eglChooseConfig operation."
      0
   }
   fn eglBindAPI(any _api) int {
      "Runs the eglBindAPI operation."
      0
   }
   fn eglCreateContext(any _display, any _config, any _share, any _attrs) any {
      "Runs the eglCreateContext operation."
      0
   }
   fn eglCreateWindowSurface(any _display, any _config, any _native_window, any _attrs) any {
      "Runs the eglCreateWindowSurface operation."
      0
   }
   fn eglDestroySurface(any _display, any _surface) int {
      "Runs the eglDestroySurface operation."
      0
   }
   fn eglGetConfigAttrib(any _display, any _config, any _attrib, any _value) int {
      "Runs the eglGetConfigAttrib operation."
      0
   }
   fn eglDestroyContext(any _display, any _ctx) int {
      "Runs the eglDestroyContext operation."
      0
   }
   fn eglMakeCurrent(any _display, any _draw, any _read, any _ctx) int {
      "Runs the eglMakeCurrent operation."
      0
   }
   fn eglSwapBuffers(any _display, any _surface) int {
      "Runs the eglSwapBuffers operation."
      0
   }
   fn eglSwapInterval(any _display, any _interval) int {
      "Runs the eglSwapInterval operation."
      0
   }
   fn eglGetProcAddress(any _name) any {
      "Runs the eglGetProcAddress operation."
      0
   }
} #endif
#macos {
   fn eglGetDisplay(any _native_display) any {
      "Runs the eglGetDisplay operation."
      0
   }
   fn eglInitialize(any _display, any _major, any _minor) int {
      "Runs the eglInitialize operation."
      0
   }
   fn eglChooseConfig(any _display, any _attrs, any _configs, int _config_size, any _num_configs) int {
      "Runs the eglChooseConfig operation."
      0
   }
   fn eglBindAPI(any _api) int {
      "Runs the eglBindAPI operation."
      0
   }
   fn eglCreateContext(any _display, any _config, any _share, any _attrs) any {
      "Runs the eglCreateContext operation."
      0
   }
   fn eglCreateWindowSurface(any _display, any _config, any _native_window, any _attrs) any {
      "Runs the eglCreateWindowSurface operation."
      0
   }
   fn eglDestroySurface(any _display, any _surface) int {
      "Runs the eglDestroySurface operation."
      0
   }
   fn eglGetConfigAttrib(any _display, any _config, any _attrib, any _value) int {
      "Runs the eglGetConfigAttrib operation."
      0
   }
   fn eglDestroyContext(any _display, any _ctx) int {
      "Runs the eglDestroyContext operation."
      0
   }
   fn eglMakeCurrent(any _display, any _draw, any _read, any _ctx) int {
      "Runs the eglMakeCurrent operation."
      0
   }
   fn eglSwapBuffers(any _display, any _surface) int {
      "Runs the eglSwapBuffers operation."
      0
   }
   fn eglSwapInterval(any _display, any _interval) int {
      "Runs the eglSwapInterval operation."
      0
   }
   fn eglGetProcAddress(any _name) any {
      "Runs the eglGetProcAddress operation."
      0
   }
} #endif

fn init_display(any native_display) any {
   "Initializes init display."
   def dpy = eglGetDisplay(native_display)
   if !dpy { return 0 }
   def major = zalloc(4)
   def minor = zalloc(4)
   if eglInitialize(dpy, major, minor) == 0 {
      free(major, minor)
      return 0
   }
   free(major, minor)
   dpy
}

fn choose_config(any display, list attrs) any {
   "Runs the choose config operation."
   if !display { return 0 }
   def num_configs = zalloc(4)
   def config_ptr = zalloc(8)
   mut count = 0
   while attrs.get(count, 0x3038) != 0x3038 { count += 1 }
   def attr_list = zalloc((count + 1) * 4)
   mut i = 0
   while i < count {
      store32(attr_list, int(attrs.get(i)), i * 4)
      i += 1
   }
   store32(attr_list, 0x3038, count * 4)
   mut res = 0
   if eglChooseConfig(display, attr_list, config_ptr, 1, num_configs) != 0 { if load32(num_configs, 0) > 0 { res = load64(config_ptr, 0) } }
   free(num_configs, config_ptr, attr_list)
   res
}

fn _create_context_with_attrs(any display, any config, any share, any attrs) any {
   eglCreateContext(display, config, share, attrs)
}

fn create_context(any display, any config, any share=0, int gles_ver=0) any {
   "Creates create context."
   if !display || !config { return 0 }
   ;; The renderer in std.os.ui.render.gl uses desktop OpenGL fixed-function
   ;; entry points. Request an EGL desktop-GL context first; GLES lacks glBegin,
   ;; matrix stack, and the client-array calls this backend intentionally uses.
   if eglBindAPI(0x30A2) != 0 {
      def ctx = _create_context_with_attrs(display, config, share, 0)
      if ctx { return ctx }
   }
   if gles_ver <= 0 { return 0 }
   eglBindAPI(0x30A0)
   def attrs = zalloc(12)
   store32(attrs, 0x3098, 0)
   store32(attrs, int(gles_ver), 4)
   store32(attrs, 0x3038, 8)
   def ctx = _create_context_with_attrs(display, config, share, attrs)
   free(attrs)
   ctx
}

fn create_surface(any display, any config, any native_window) any {
   "Creates create surface."
   if !display || !config || !native_window { return 0 }
   eglCreateWindowSurface(display, config, native_window, 0)
}

fn destroy_surface(any display, any surface) bool {
   "Destroys destroy surface."
   if !display || !surface { return false }
   eglDestroySurface(display, surface) != 0
}

fn get_config_attrib(any display, any config, any attrib) int {
   "Returns get config attrib."
   if !display || !config { return 0 }
   def val = zalloc(4)
   mut res = 0
   if eglGetConfigAttrib(display, config, int(attrib), val) != 0 { res = int(load32(val, 0)) }
   free(val)
   res
}

fn destroy_context(any display, any ctx) bool {
   "Destroys destroy context."
   if !ctx { return false }
   eglDestroyContext(display, ctx)
   true
}

fn make_current(any display, any draw, any read, any ctx) bool {
   "Builds make current."
   eglMakeCurrent(display, draw, read, ctx) != 0
}

fn swap_buffers(any display, any surface) bool {
   "Runs the swap buffers operation."
   if !surface { return false }
   eglSwapBuffers(display, surface)
   true
}

fn swap_interval(any display, any interval) bool {
   "Runs the swap interval operation."
   eglSwapInterval(display, interval)
   true
}

fn get_proc_address(any name) any {
   "Returns get proc address."
   def proc_name = to_str(name)
   if !startswith(proc_name, "gl") && !startswith(proc_name, "egl") { return 0 }
   def s = cstr(proc_name)
   eglGetProcAddress(s)
}

#main {
   assert(choose_config(0, []) == 0, "egl config fallback")
   assert(create_context(0, 0) == 0 && create_surface(0, 0, 0) == 0, "egl create fallback")
   assert(!destroy_surface(0, 0) && get_config_attrib(0, 0, 0) == 0, "egl surface fallback")
   assert(!destroy_context(0, 0), "egl destroy context fallback")
   assert(get_proc_address("definitely_missing_nytrix_probe_symbol") == 0, "egl proc fallback")
   print("✓ std.os.ui.window.platform.opengl.egl self-test passed")
}
