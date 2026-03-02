;; Keywords: ui window glfw ffi
;; GLFW Backend for std.ui.window

module std.ui.glfw (
   available, init, create_native_window, poll_events,
   swap_buffers, make_current, blit_buffer, get_backend_name,
   native_window, create_vulkan_surface, get_required_instance_extensions,
   _glfwVulkanSupported_sym, _glfwGetRequiredInstanceExtensions_sym
)

use std.core *
use std.core.dict *
use std.os *
use std.os.ffi *
use std.ui.consts *
use std.ui.event as ev
use std.text *

mut _glfw_handle = 0
mut _initialized = false

;; GLFW Constants
def GLFW_CLIENT_API = 0x00022001
def GLFW_NO_API = 0
def GLFW_OPENGL_API = 0x00030001
def GLFW_RESIZABLE = 0x00020003
def GLFW_VISIBLE = 0x00020004
def GLFW_DECORATED = 0x00020005
def GLFW_FOCUSED = 0x00020001
def GLFW_AUTO_ICONIFY = 0x00020006
def GLFW_FLOATING = 0x00020007
def GLFW_MAXIMIZED = 0x00020008
def GLFW_CENTER_CURSOR = 0x00020009
def GLFW_TRANSPARENT_FRAMEBUFFER = 0x0002000A
def GLFW_FOCUS_ON_SHOW = 0x0002000C
def GLFW_SCALE_TO_MONITOR = 0x0002000D

def GLFW_PRESS = 1
def GLFW_RELEASE = 0
def GLFW_REPEAT = 2

;; Key codes (partial)
def GLFW_KEY_ESCAPE = 256
def GLFW_KEY_ENTER = 257
def GLFW_KEY_TAB = 258
def GLFW_KEY_BACKSPACE = 259
def GLFW_KEY_LEFT = 263
def GLFW_KEY_UP = 265
def GLFW_KEY_RIGHT = 262
def GLFW_KEY_DOWN = 264

mut _glfwInit_sym = 0
mut _glfwTerminate_sym = 0
mut _glfwWindowHint_sym = 0
mut _glfwCreateWindow_sym = 0
mut _glfwDestroyWindow_sym = 0
mut _glfwWindowShouldClose_sym = 0
mut _glfwSetWindowShouldClose_sym = 0
mut _glfwPollEvents_sym = 0
mut _glfwSwapBuffers_sym = 0
mut _glfwMakeContextCurrent_sym = 0
mut _glfwGetWindowSize_sym = 0
mut _glfwSetWindowSize_sym = 0
mut _glfwGetCursorPos_sym = 0
mut _glfwGetKey_sym = 0
mut _glfwGetMouseButton_sym = 0
mut _glfwVulkanSupported_sym = 0
mut _glfwCreateWindowSurface_sym = 0
mut _glfwGetRequiredInstanceExtensions_sym = 0

fn available(){
   "Checks if GLFW is available on the system."
   if(_initialized){ return true }
   if(_glfw_handle == 0){
      _glfw_handle = dlopen_any("glfw", RTLD_NOW())
      if(_glfw_handle == 0){ _glfw_handle = dlopen_any("glfw3", RTLD_NOW()) }
      if(_glfw_handle == 0){ return false }

      _glfwInit_sym = dlsym(_glfw_handle, "glfwInit")
      _glfwTerminate_sym = dlsym(_glfw_handle, "glfwTerminate")
      _glfwWindowHint_sym = dlsym(_glfw_handle, "glfwWindowHint")
      _glfwCreateWindow_sym = dlsym(_glfw_handle, "glfwCreateWindow")
      _glfwDestroyWindow_sym = dlsym(_glfw_handle, "glfwDestroyWindow")
      _glfwWindowShouldClose_sym = dlsym(_glfw_handle, "glfwWindowShouldClose")
      _glfwSetWindowShouldClose_sym = dlsym(_glfw_handle, "glfwSetWindowShouldClose")
      _glfwPollEvents_sym = dlsym(_glfw_handle, "glfwPollEvents")
      _glfwSwapBuffers_sym = dlsym(_glfw_handle, "glfwSwapBuffers")
      _glfwMakeContextCurrent_sym = dlsym(_glfw_handle, "glfwMakeContextCurrent")
      _glfwGetWindowSize_sym = dlsym(_glfw_handle, "glfwGetWindowSize")
      _glfwSetWindowSize_sym = dlsym(_glfw_handle, "glfwSetWindowSize")
      _glfwGetCursorPos_sym = dlsym(_glfw_handle, "glfwGetCursorPos")
      _glfwGetKey_sym = dlsym(_glfw_handle, "glfwGetKey")
      _glfwGetMouseButton_sym = dlsym(_glfw_handle, "glfwGetMouseButton")
      _glfwVulkanSupported_sym = dlsym(_glfw_handle, "glfwVulkanSupported")
      _glfwCreateWindowSurface_sym = dlsym(_glfw_handle, "glfwCreateWindowSurface")
      _glfwGetRequiredInstanceExtensions_sym = dlsym(_glfw_handle, "glfwGetRequiredInstanceExtensions")
   }
   return true
}

fn init(){
   "Initializes GLFW."
   if(_initialized){ return true }
   if(!available()){ return false }
   def res = call0(_glfwInit_sym)
   if(res == 0){ return false }
   _initialized = true
   true
}

fn create_native_window(win){
   "Creates a native GLFW window."
   if(!init()){ return false }

   def title = get(win, 2, "Window")
   def w = get(win, 5, 800)
   def h = get(win, 6, 600)
   def flags = get(win, 7, 0)

   ;; Set hints
   if((flags & WINDOW_VULKAN) != 0){
      call2_void(_glfwWindowHint_sym, GLFW_CLIENT_API, GLFW_NO_API)
   }

   if((flags & WINDOW_TRANSPARENT) != 0){
      call2_void(_glfwWindowHint_sym, GLFW_TRANSPARENT_FRAMEBUFFER, 1)
   }

   if((flags & WINDOW_NO_BORDER) != 0){
      call2_void(_glfwWindowHint_sym, GLFW_DECORATED, 0)
   }

   if((flags & WINDOW_NO_RESIZE) != 0){
      call2_void(_glfwWindowHint_sym, GLFW_RESIZABLE, 0)
   }

   ;; Create window
   def glfw_win = call5(_glfwCreateWindow_sym, w, h, title, 0, 0)
   if(glfw_win == 0){ return false }

   set_idx(win, 22, glfw_win) ;; NATIVE_CTX

   if((flags & WINDOW_VULKAN) == 0){
      call1_void(_glfwMakeContextCurrent_sym, glfw_win)
   }

   true
}

fn poll_events(win){
   "Polls GLFW events and updates window state."
   if(!_initialized){ return 0 }
   call0_void(_glfwPollEvents_sym)

   def glfw_win = get(win, 22, 0)
   if(glfw_win == 0){ return 0 }

   ;; Check for close
   if(call1(_glfwWindowShouldClose_sym, glfw_win) != 0){
      set_idx(win, 8, true) ;; SHOULD_CLOSE
      def q = get(win, 10, [])
      set_idx(win, 10, append(q, ev.make_event(EVENT_QUIT, win, get(win, 1), 0)))
   }

   ;; Manually poll some keys for simulation if needed,
   ;; but GLFW doesn't give us a list of changed keys easily without callbacks.
   ;; Since we don't have callbacks yet, we might just poll the common quit keys.

   if(call2(_glfwGetKey_sym, glfw_win, GLFW_KEY_ESCAPE) == GLFW_PRESS){
      set_idx(win, 8, true)
   }

   ;; Update size if changed
   mut w_ptr = malloc(4)
   mut h_ptr = malloc(4)
   call3_void(_glfwGetWindowSize_sym, glfw_win, w_ptr, h_ptr)
   def nw = load32(w_ptr)
   def nh = load32(h_ptr)
   free(w_ptr)
   free(h_ptr)

   if(nw != get(win, 5) || nh != get(win, 6)){
      def old_w = get(win, 5)
      def old_h = get(win, 6)
      set_idx(win, 5, nw)
      set_idx(win, 6, nh)
      mut r = dict(4)
      r = dict_set(r, "w", nw)
      r = dict_set(r, "h", nh)
      def q = get(win, 10, [])
      set_idx(win, 10, append(q, ev.make_event(EVENT_WINDOW_RESIZED, win, get(win, 1), r)))
   }

   ;; Mouse pos
   mut x_ptr = malloc(8)
   mut y_ptr = malloc(8)
   call3_void(_glfwGetCursorPos_sym, glfw_win, x_ptr, y_ptr)
   ;; Cursor pos is double (8 bytes)
   ;; For now, just load as 64-bit int and we'll have issues if it's float,
   ;; but Nytrix unbox might handle it if we use load32_f32 or similar.
   ;; Actually let's assume they are small enough for now or we need a proper double loader.
   ;; We'll just use load64 and hope for the best for a quick prototype.
   ;; Wait, Nytrix doesn't have load64_f64.

   0
}

fn swap_buffers(win){
   "Swaps GLFW buffers."
   def glfw_win = get(win, 22, 0)
   if(glfw_win != 0){
      call1_void(_glfwSwapBuffers_sym, glfw_win)
   }
}

fn make_current(win){
   "Makes GLFW context current."
   def glfw_win = get(win, 22, 0)
   if(glfw_win != 0){
      call1_void(_glfwMakeContextCurrent_sym, glfw_win)
   }
}

fn blit_buffer(win, buf, w, h){
   "GLFW doesn't have a direct blit, use OpenGL or similar if needed."
   0
}

fn native_window(win){
   "Returns the native GLFW window handle."
   get(win, 22, 0)
}

fn create_vulkan_surface(instance, glfw_win, allocator, pSurface){
   "Wraps glfwCreateWindowSurface."
   if(_glfwCreateWindowSurface_sym == 0){ return -1 }
   call4(_glfwCreateWindowSurface_sym, instance, glfw_win, allocator, pSurface)
}

fn get_required_instance_extensions(){
   "Returns [count, pExtensions] for Vulkan instance creation."
   if(_glfwGetRequiredInstanceExtensions_sym == 0){
      init()
   }
   if(_glfwGetRequiredInstanceExtensions_sym == 0){
      print("GLFW: _glfwGetRequiredInstanceExtensions_sym is 0 even after init!")
      mut l = list(2)
      l = append(l, 0)
      l = append(l, 0)
      return l
   }

   if(_glfwVulkanSupported_sym != 0){
      def sup = call0(_glfwVulkanSupported_sym)
      if(sup == 0){
         print("GLFW ERROR: glfwVulkanSupported returned 0 inside get_required_instance_extensions!")
      } else {
         print("GLFW: glfwVulkanSupported is TRUE inside get_required_instance_extensions.")
      }
   }

   mut count_ptr = malloc(4)
   def exts = call1(_glfwGetRequiredInstanceExtensions_sym, count_ptr)
   def count = load32(count_ptr)

   if(count == 0){
      print("GLFW ERROR: get_required_instance_extensions returned count 0!")
   }

   mut l2 = list(2)
   l2 = append(l2, count)
   l2 = append(l2, exts)
   l2
}

fn get_backend_name(){
   "glfw"
}

if(comptime{__main()}){
   use std.core.error *

   if(available()){
      print("✓ GLFW Backend available")
      assert(init(), "GLFW init")
      assert(_glfwCreateWindow_sym != 0, "glfwCreateWindow symbol resolved")
      assert(_glfwPollEvents_sym != 0, "glfwPollEvents symbol resolved")
      assert(_glfwTerminate_sym != 0, "glfwTerminate symbol resolved")
      print("✓ GLFW Backend initialized and symbols resolved")
      call0_void(_glfwTerminate_sym)
   } else {
      print("! GLFW Backend NOT available on this system (skipping functional tests)")
   }
}
