;; Keywords: platform window backend opengl glx
;; GLX context creation for the X11 window backend.
module std.os.ui.window.platform.opengl.glx(create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address, choose_fb_config, get_visual)
use std.core
use std.core.mem (cstr)

#linux {
   #link "libGL.so"
   #include <GL/glx.h>
} #endif

fn choose_fb_config(any: display, any: screen, any: attrs=0): any {
   if(!display){ return 0 }
   def count_ptr = zalloc(4)
   mut final_attrs = []
   if(is_list(attrs) && attrs.len > 0){ final_attrs = attrs } else {
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
   def final_attrs_n = final_attrs.len
   def attr_list = zalloc((final_attrs_n + 1) * 4)
   mut i = 0
   while(i < final_attrs_n){
      store32(attr_list, int(final_attrs.get(i)), i * 4)
      i += 1
   }
   store32(attr_list, 0, final_attrs_n * 4) ;; None
   def configs = glXChooseFBConfig(display, int(screen), attr_list, count_ptr)
   def count = load32(count_ptr, 0)
   mut res = 0
   if(configs && count > 0){
      res = load64(configs, 0)
   }
   free(count_ptr, attr_list)
   res
}

fn get_visual(any: display, any: fbconfig): any {
   if(!fbconfig){ return 0 }
   glXGetVisualFromFBConfig(display, fbconfig)
}

fn get_fbconfig_attrib(any: display, any: config, any: attr): int {
   if(!display || !config){ return 0 }
   def value = malloc(4)
   if(!value){ return 0 }
   store32(value, 0, 0)
   glXGetFBConfigAttrib(display, config, attr, value)
   def res = load32(value, 0)
   free(value)
   res
}

fn create_context(any: display, any: fbconfig, any: share=0, bool: direct=true): any {
   if(!fbconfig){ return 0 }
   glXCreateNewContext(display, fbconfig, 0x8014, share, direct ? 1 : 0)
}

fn destroy_context(any: display, any: ctx): bool {
   if(!ctx){ return false }
   glXDestroyContext(display, ctx)
   true
}

fn make_current(any: display, any: win, any: ctx): bool { glXMakeCurrent(display, win, ctx) != 0 }

fn swap_buffers(any: display, any: win): bool {
   if(!win){ return false }
   glXSwapBuffers(display, win)
   true
}

fn swap_interval(any: interval): bool {
   true
}

fn get_proc_address(any: name): any {
   def proc_name = to_str(name)
   if(!startswith(proc_name, "gl")){ return 0 }
   def s = cstr(proc_name)
   def addr = glXGetProcAddress(s)
   if(addr){ return addr }
   glXGetProcAddressARB(s)
}
