;; Keywords: platform window backend opengl egl
;; EGL context creation for Wayland and X11 window backends.
module std.os.ui.window.platform.opengl.egl(create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address, init_display, choose_config, create_surface, destroy_surface, get_config_attrib)
use std.core
use std.core.mem (cstr)
use std.os.ui.window.platform.api as backend_api

#linux {
   #link "libEGL.so"
   #include <EGL/egl.h>
} #endif

fn init_display(any: native_display): any {
   def dpy = eglGetDisplay(native_display)
   if(!dpy){ return 0 }
   def major = zalloc(4)
   def minor = zalloc(4)
   if(eglInitialize(dpy, major, minor) == 0){
      free(major, minor)
      return 0
   }
   free(major, minor)
   dpy
}

fn choose_config(any: display, list: attrs): any {
   if(!display){ return 0 }
   def num_configs = zalloc(4)
   def config_ptr = zalloc(8)
   mut count = 0
   while(attrs.get(count, -1) != -1){ count += 1 }
   def attr_list = zalloc((count + 1) * 4)
   mut i = 0
   while(i < count){
      store32(attr_list, int(attrs.get(i)), i * 4)
      i += 1
   }
   store32(attr_list, 0x3038, count * 4) ;; EGL_NONE
   mut res = 0
   if(eglChooseConfig(display, attr_list, config_ptr, 1, num_configs) != 0){ if(load32(num_configs, 0) > 0){ res = load64(config_ptr, 0) } }
   free(num_configs, config_ptr, attr_list)
   res
}

fn create_context(any: display, any: config, any: share=0, int: gles_ver=2): any {
   if(!display || !config){ return 0 }
   eglBindAPI(0x30A0) ;; EGL_OPENGL_ES_API
   def attrs = zalloc(12)
   store32(attrs, 0x3098, 0) ;; EGL_CONTEXT_CLIENT_VERSION
   store32(attrs, int(gles_ver), 4)
   store32(attrs, 0x3038, 8) ;; EGL_NONE
   def ctx = eglCreateContext(display, config, share, attrs)
   free(attrs)
   ctx
}

fn create_surface(any: display, any: config, any: native_window): any {
   if(!display || !config || !native_window){ return 0 }
   eglCreateWindowSurface(display, config, native_window, 0)
}

fn destroy_surface(any: display, any: surface): bool {
   if(!display || !surface){ return false }
   eglDestroySurface(display, surface) != 0
}

fn get_config_attrib(any: display, any: config, any: attrib): int {
   if(!display || !config){ return 0 }
   def val = zalloc(4)
   mut res = 0
   if(eglGetConfigAttrib(display, config, int(attrib), val) != 0){ res = int(load32(val, 0)) }
   free(val)
   res
}

fn destroy_context(any: display, any: ctx): bool {
   if(!ctx){ return false }
   eglDestroyContext(display, ctx)
   true
}

fn make_current(any: display, any: draw, any: read, any: ctx): bool { eglMakeCurrent(display, draw, read, ctx) != 0 }

fn swap_buffers(any: display, any: surface): bool {
   if(!surface){ return false }
   eglSwapBuffers(display, surface)
   true
}

fn swap_interval(any: display, any: interval): bool {
   eglSwapInterval(display, interval)
   true
}

fn get_proc_address(any: name): any {
   def proc_name = to_str(name)
   if(!startswith(proc_name, "gl") && !startswith(proc_name, "egl")){ return 0 }
   def s = cstr(proc_name)
   eglGetProcAddress(s)
}
