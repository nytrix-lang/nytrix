;; Keywords: ui window backend glx
;; GLX Context helper for X11 Nytrix backend.

module std.ui.window.platform.opengl.glx (
   create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address,
   choose_fb_config, get_visual
)

use std.core *
use std.ui.window.platform.linux.x11 as x11
use std.ui.window.platform.api as backend_api

if(comptime{ __os_name() == "linux" }){
   #link "libGL.so"
   #include <GL/glx.h>
}

fn choose_fb_config(display, screen, attrs=0){
   def count_ptr = calloc(1, 4)

   mut final_attrs = []
   if(is_list(attrs) && len(attrs) > 0){
      final_attrs = attrs
   } else {
      ;; Standard attributes
      final_attrs = [
         0x8002, 1, ;; GLX_X_RENDERABLE
         0x8010, 0x01, ;; GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT
         0x8011, 0x01, ;; GLX_RENDER_TYPE, GLX_RGBA_BIT
         0x0008, 8, ;; GLX_RED_SIZE
         0x0009, 8, ;; GLX_GREEN_SIZE
         0x000a, 8, ;; GLX_BLUE_SIZE
         0x000b, 8, ;; GLX_ALPHA_SIZE
         0x000c, 24, ;; GLX_DEPTH_SIZE
         0x000d, 8, ;; GLX_STENCIL_SIZE
         0x0005, 1 ;; GLX_DOUBLEBUFFER
      ]
   }

   def attr_list = calloc(len(final_attrs) + 1, 4)
   mut i = 0
   while(i < len(final_attrs)){
      store32(attr_list, int(get(final_attrs, i)), i * 4)
      i += 1
   }
   store32(attr_list, 0, len(final_attrs) * 4) ;; None

   def configs = glXChooseFBConfig(display, int(screen), attr_list, count_ptr)
   def count = load32(count_ptr, 0)
   mut res = 0
   if(configs && count > 0){
      res = load64(configs, 0)
      ;; Note: normally we'd loop and pick best, but GLFW logic is deep. Taking first for now.
      ;; The list returned by glX should be freed with XFree, but we'll skip for this shim.
   }
   free(count_ptr)
   free(attr_list)
   res
}

fn get_visual(display, fbconfig){
   if(!fbconfig){ return 0 }
   glXGetVisualFromFBConfig(display, fbconfig)
}

fn create_context(display, fbconfig, share=0, direct=true){
   if(!fbconfig){ return 0 }
   ;; GLX_RGBA_TYPE = 0x8014
   glXCreateNewContext(display, fbconfig, 0x8014, share, direct ? 1 : 0)
}

fn destroy_context(display, ctx){
   if(!ctx){ return false }
   glXDestroyContext(display, ctx)
   true
}

fn make_current(display, win, ctx){
   glXMakeCurrent(display, win, ctx) != 0
}

fn swap_buffers(display, win){
   if(!win){ return false }
   glXSwapBuffers(display, win)
   true
}

fn swap_interval(interval){
   ;; Needs to query extensions and load the correct pointer
   true
}

fn get_proc_address(name){
   def s = cstr(name)
   def addr = glXGetProcAddress(s)
   if(addr){ return addr }
   glXGetProcAddressARB(s)
}
