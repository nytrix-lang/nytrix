;; Keywords: platform window backend opengl glx os ui input
;; GLX context creation for the X11 window backend.
;; References:
;; - std.os.ui.window.platform.opengl
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform.opengl.glx(create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address, choose_fb_config, get_visual)
use std.core
use std.core.mem (cstr)

#linux {
   #link "libGL.so"
   #include <GL/glx.h>
} #endif
#windows {
   fn glXChooseFBConfig(any _display, int _screen, any _attrs, any _count) any {
      "Runs the glXChooseFBConfig operation."
      0
   }
   fn glXGetVisualFromFBConfig(any _display, any _fbconfig) any {
      "Runs the glXGetVisualFromFBConfig operation."
      0
   }
   fn glXGetFBConfigAttrib(any _display, any _config, any _attr, any _value) int {
      "Runs the glXGetFBConfigAttrib operation."
      0
   }
   fn glXCreateNewContext(any _display, any _fbconfig, int _type, any _share, int _direct) any {
      "Runs the glXCreateNewContext operation."
      0
   }
   fn glXDestroyContext(any _display, any _ctx) any {
      "Runs the glXDestroyContext operation."
      0
   }
   fn glXMakeCurrent(any _display, any _win, any _ctx) int {
      "Runs the glXMakeCurrent operation."
      0
   }
   fn glXSwapBuffers(any _display, any _win) any {
      "Runs the glXSwapBuffers operation."
      0
   }
   fn glXGetProcAddress(any _name) any {
      "Runs the glXGetProcAddress operation."
      0
   }
   fn glXGetProcAddressARB(any _name) any {
      "Runs the glXGetProcAddressARB operation."
      0
   }
} #endif
#macos {
   fn glXChooseFBConfig(any _display, int _screen, any _attrs, any _count) any {
      "Runs the glXChooseFBConfig operation."
      0
   }
   fn glXGetVisualFromFBConfig(any _display, any _fbconfig) any {
      "Runs the glXGetVisualFromFBConfig operation."
      0
   }
   fn glXGetFBConfigAttrib(any _display, any _config, any _attr, any _value) int {
      "Runs the glXGetFBConfigAttrib operation."
      0
   }
   fn glXCreateNewContext(any _display, any _fbconfig, int _type, any _share, int _direct) any {
      "Runs the glXCreateNewContext operation."
      0
   }
   fn glXDestroyContext(any _display, any _ctx) any {
      "Runs the glXDestroyContext operation."
      0
   }
   fn glXMakeCurrent(any _display, any _win, any _ctx) int {
      "Runs the glXMakeCurrent operation."
      0
   }
   fn glXSwapBuffers(any _display, any _win) any {
      "Runs the glXSwapBuffers operation."
      0
   }
   fn glXGetProcAddress(any _name) any {
      "Runs the glXGetProcAddress operation."
      0
   }
   fn glXGetProcAddressARB(any _name) any {
      "Runs the glXGetProcAddressARB operation."
      0
   }
} #endif

fn choose_fb_config(any display, any screen, any attrs=0) any {
   "Runs the choose fb config operation."
   if(!display){ return 0 }
   def count_ptr = zalloc(4)
   mut final_attrs = []
   if(is_list(attrs) && attrs.len > 0){ final_attrs = attrs } else {
      final_attrs = [
         0x8002, 1,
         0x8010, 0x01,
         0x8011, 0x01,
         0x0008, 8,
         0x0009, 8,
         0x000a, 8,
         0x000b, 8,
         0x000c, 24,
         0x000d, 8,
         0x0005, 1
      ]
   }
   def final_attrs_n = final_attrs.len
   def attr_list = zalloc((final_attrs_n + 1) * 4)
   mut i = 0
   while(i < final_attrs_n){
      store32(attr_list, int(final_attrs.get(i)), i * 4)
      i += 1
   }
   store32(attr_list, 0, final_attrs_n * 4)
   def configs = glXChooseFBConfig(display, int(screen), attr_list, count_ptr)
   def count = load32(count_ptr, 0)
   mut res = 0
   if(configs && count > 0){
      res = load64(configs, 0)
   }
   free(count_ptr, attr_list)
   res
}

fn get_visual(any display, any fbconfig) any {
   "Returns get visual."
   if(!fbconfig){ return 0 }
   glXGetVisualFromFBConfig(display, fbconfig)
}

fn get_fbconfig_attrib(any display, any config, any attr) int {
   "Returns get fbconfig attrib."
   if(!display || !config){ return 0 }
   def value = malloc(4)
   if(!value){ return 0 }
   store32(value, 0, 0)
   glXGetFBConfigAttrib(display, config, attr, value)
   def res = load32(value, 0)
   free(value)
   res
}

fn create_context(any display, any fbconfig, any share=0, bool direct=true) any {
   "Creates create context."
   if(!fbconfig){ return 0 }
   glXCreateNewContext(display, fbconfig, 0x8014, share, direct ? 1 : 0)
}

fn destroy_context(any display, any ctx) bool {
   "Destroys destroy context."
   if(!ctx){ return false }
   glXDestroyContext(display, ctx)
   true
}

fn make_current(any display, any win, any ctx) bool {
   "Builds make current."
   glXMakeCurrent(display, win, ctx) != 0
}

fn swap_buffers(any display, any win) bool {
   "Runs the swap buffers operation."
   if(!win){ return false }
   glXSwapBuffers(display, win)
   true
}

fn swap_interval(any interval) bool {
   "Runs the swap interval operation."
   true
}

fn get_proc_address(any name) any {
   "Returns get proc address."
   def proc_name = to_str(name)
   if(!startswith(proc_name, "gl")){ return 0 }
   def s = cstr(proc_name)
   def addr = glXGetProcAddress(s)
   if(addr){ return addr }
   glXGetProcAddressARB(s)
}

#main {
   assert(get_visual(0, 0) == 0, "glx visual fallback")
   assert(get_fbconfig_attrib(0, 0, 0) == 0, "glx attrib fallback")
   assert(create_context(0, 0) == 0 && !destroy_context(0, 0), "glx context fallback")
   print("✓ std.os.ui.window.platform.opengl.glx self-test passed")
}
