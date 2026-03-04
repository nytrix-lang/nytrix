;; Keywords: ui window backend egl
;; EGL Context helper for Wayland/X11 Nytrix backends.

module std.ui.window.platform.opengl.egl (
   create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address,
   init_display, choose_config, create_surface, destroy_surface, get_config_attrib
)

use std.core *
use std.ui.window.platform.api as backend_api

if(comptime{ __os_name() == "linux" }){
   #link "libEGL.so"
   #include <EGL/egl.h>
}
fn init_display(native_display){
   def dpy = eglGetDisplay(native_display)
   if(!dpy){ return 0 }
   def major = calloc(1, 4)
   def minor = calloc(1, 4)
   if(eglInitialize(dpy, major, minor) == 0){
      free(major)
      free(minor)
      return 0
   }
   free(major)
   free(minor)
   dpy
}

fn choose_config(display, attrs){
   if(!display){ return 0 }
   def num_configs = calloc(1, 4)
   def config_ptr = calloc(1, 8)

   mut count = 0
   while(get(attrs, count, -1) != -1){ count += 1 }
   def attr_list = calloc(count + 1, 4)
   mut i = 0
   while(i < count){
      store32(attr_list, int(get(attrs, i)), i * 4)
      i += 1
   }
   store32(attr_list, 0x3038, count * 4) ;; EGL_NONE

   mut res = 0
   if(eglChooseConfig(display, attr_list, config_ptr, 1, num_configs) != 0){
      if(load32(num_configs, 0) > 0){
         res = load64(config_ptr, 0)
      }
   }
   free(num_configs)
   free(config_ptr)
   free(attr_list)
   res
}

fn create_context(display, config, share=0, gles_ver=2){
   if(!display || !config){ return 0 }

   ;; Bind OpenGL ES API by default for now
   eglBindAPI(0x30A0) ;; EGL_OPENGL_ES_API

   def attrs = calloc(3, 4)
   store32(attrs, 0x3098, 0) ;; EGL_CONTEXT_CLIENT_VERSION
   store32(attrs, int(gles_ver), 4)
   store32(attrs, 0x3038, 8) ;; EGL_NONE

   def ctx = eglCreateContext(display, config, share, attrs)
   free(attrs)
   ctx
}

fn create_surface(display, config, native_window){
   if(!display || !config || !native_window){ return 0 }
   eglCreateWindowSurface(display, config, native_window, 0)
}

fn destroy_surface(display, surface){
   if(!display || !surface){ return false }
   eglDestroySurface(display, surface) != 0
}

fn get_config_attrib(display, config, attrib){
   if(!display || !config){ return 0 }
   def val = calloc(1, 4)
   mut res = 0
   if(eglGetConfigAttrib(display, config, int(attrib), val) != 0){
      res = int(load32(val, 0))
   }
   free(val)
   res
}

fn destroy_context(display, ctx){
   if(!ctx){ return false }
   eglDestroyContext(display, ctx)
   true
}

fn make_current(display, draw, read, ctx){
   eglMakeCurrent(display, draw, read, ctx) != 0
}

fn swap_buffers(display, surface){
   if(!surface){ return false }
   eglSwapBuffers(display, surface)
   true
}

fn swap_interval(display, interval){
   eglSwapInterval(display, interval)
   true
}

fn get_proc_address(name){
   def s = cstr(name)
   eglGetProcAddress(s)
}
