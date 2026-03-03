;; Keywords: ui glfw
;; GLFW backend — dynamic loading via FFI.

module std.ui.glfw (
   ; Lifecycle
   init, terminate,
   ; Windows
   create_window, destroy_window, should_close, set_should_close,
   set_title, get_size, get_framebuffer_size, set_size,
   ; Frame
   poll_events, swap_buffers, swap_interval,
   ; Input
   get_key, get_mouse_button, get_cursor_pos,
   ; Callbacks
   set_key_callback, set_mouse_button_callback,
   set_scroll_callback, set_cursor_pos_callback,
   set_window_size_callback, set_close_callback,
   ; Vulkan
   vulkan_supported, required_extensions, create_surface,
   ; Hints / consts
   apply_hints,
   GLFW_NO_API, GLFW_CLIENT_API, GLFW_RESIZABLE, GLFW_DECORATED,
   GLFW_VISIBLE, GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FOCUSED,
   GLFW_FLOATING, GLFW_MAXIMIZED, GLFW_FOCUS_ON_SHOW,
   GLFW_PRESS, GLFW_RELEASE, GLFW_REPEAT,
   KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_SPACE,
   KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
   KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
   KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
   MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,
   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER,
   get_backend_name
)

use std.core *
use std.ui.consts *
use std.os.ffi as ffi

;; FFI Helper

mut _lib = 0
mut _size_wp = 0
mut _size_hp = 0
mut _cursor_xp = 0
mut _cursor_yp = 0

fn _call(nm, args) {
   "Internal: Dynamic dispatch for GLFW C functions."
   if(!_lib){
      _lib = ffi.dlopen_any("glfw", ffi.RTLD_LAZY() | ffi.RTLD_GLOBAL())
      if(!_lib){
         _lib = ffi.dlopen_any("glfw3", ffi.RTLD_LAZY() | ffi.RTLD_GLOBAL())
      }
   }
   if(!_lib){ return 0 }
   def f = ffi.dlsym(_lib, nm)
   if(!f){ return 0 }
   ffi.ffi_call(f, args)
}

;; Window hint constants

def GLFW_CLIENT_API             = 0x00022001
def GLFW_NO_API                 = 0
def GLFW_OPENGL_API             = 0x00030001
def GLFW_RESIZABLE              = 0x00020003
def GLFW_VISIBLE                = 0x00020004
def GLFW_DECORATED              = 0x00020005
def GLFW_FOCUSED                = 0x00020001
def GLFW_AUTO_ICONIFY           = 0x00020006
def GLFW_FLOATING               = 0x00020007
def GLFW_MAXIMIZED              = 0x00020008
def GLFW_CENTER_CURSOR          = 0x00020009
def GLFW_TRANSPARENT_FRAMEBUFFER= 0x0002000A
def GLFW_FOCUS_ON_SHOW          = 0x0002000C
def GLFW_SCALE_TO_MONITOR       = 0x0002000D

def GLFW_PRESS   = 1
def GLFW_RELEASE = 0
def GLFW_REPEAT  = 2

;; Key codes
def KEY_ESCAPE    = 256
def KEY_ENTER     = 257
def KEY_TAB       = 258
def KEY_BACKSPACE = 259
def KEY_RIGHT     = 262
def KEY_LEFT      = 263
def KEY_DOWN      = 264
def KEY_UP        = 265
def KEY_F1  = 290
def KEY_F2  = 291
def KEY_F3  = 292
def KEY_F4  = 293
def KEY_F5  = 294
def KEY_F6  = 295
def KEY_F7  = 296
def KEY_F8  = 297
def KEY_F9  = 298
def KEY_F10 = 299
def KEY_F11 = 300
def KEY_F12 = 301
def KEY_SPACE = 32

def MOUSE_BUTTON_LEFT   = 0
def MOUSE_BUTTON_RIGHT  = 1
def MOUSE_BUTTON_MIDDLE = 2

def MOD_SHIFT   = 0x0001
def MOD_CONTROL = 0x0002
def MOD_ALT     = 0x0004
def MOD_SUPER   = 0x0008

;; Lifecycle

mut _ready = false

fn init() {
   "Initializes the GLFW library."
   if(_ready){ return true }
   if(_call("glfwInit", []) == 0){ return false }
   _ready = true
   true
}

fn terminate() {
   "Shuts down the GLFW library."
   if(_ready){ _call("glfwTerminate", []) _ready = false }
}

;; Window management

fn apply_hints(flags) {
   "Applies window creation hints based on the specified flags."
   if((flags & WINDOW_TRANSPARENT) != 0){ _call("glfwWindowHint", [GLFW_TRANSPARENT_FRAMEBUFFER, 1]) }
   if((flags & WINDOW_NO_BORDER)   != 0){ _call("glfwWindowHint", [GLFW_DECORATED, 0]) }
   if((flags & WINDOW_NO_RESIZE)   != 0){ _call("glfwWindowHint", [GLFW_RESIZABLE, 0]) }
   if((flags & WINDOW_FLOATING)    != 0){ _call("glfwWindowHint", [GLFW_FLOATING, 1]) }
   if((flags & WINDOW_MAXIMIZE)    != 0){ _call("glfwWindowHint", [GLFW_MAXIMIZED, 1]) }
   if((flags & WINDOW_HIDE)        != 0){ _call("glfwWindowHint", [GLFW_VISIBLE, 0]) }
}

fn create_window(title, w, h, flags=0) {
   "Creates a new GLFW window."
   init()
   _call("glfwWindowHint", [GLFW_CLIENT_API, GLFW_NO_API])
   apply_hints(flags)
   _call("glfwCreateWindow", [w, h, title, 0, 0])
}

fn destroy_window(win)         { "Destroys a GLFW window." if(win){ _call("glfwDestroyWindow", [win]) } }
fn should_close(win)           { "Returns true if the window should close." _call("glfwWindowShouldClose", [win]) != 0 }
fn set_should_close(win, v=1)  { "Sets the window's close flag." _call("glfwSetWindowShouldClose", [win, v]) }
fn set_title(win, title)       { "Updates the window title." _call("glfwSetWindowTitle", [win, title]) }
fn poll_events()               { "Polls for pending window events." if(_ready){ _call("glfwPollEvents", []) } }
fn swap_buffers(win)           { "Swaps the front and back buffers." _call("glfwSwapBuffers", [win]) }
fn swap_interval(n)            { "Sets the swap interval (VSync)." _call("glfwSwapInterval", [n]) }

fn _ensure_size_bufs(){
   if(!_size_wp){ _size_wp = malloc(4) _size_hp = malloc(4) }
}

fn get_size(win) {
   "Returns the window size as [width, height]."
   _ensure_size_bufs()
   _call("glfwGetWindowSize", [win, _size_wp, _size_hp])
   [load32(_size_wp, 0), load32(_size_hp, 0)]
}

fn get_framebuffer_size(win) {
   "Returns the framebuffer size as [width, height]."
   _ensure_size_bufs()
   _call("glfwGetFramebufferSize", [win, _size_wp, _size_hp])
   [load32(_size_wp, 0), load32(_size_hp, 0)]
}

fn set_size(win, w, h) { "Resizes the window." _call("glfwSetWindowSize", [win, w, h]) }

;; Input

fn get_key(win, key)          { "Returns the state of a key." _call("glfwGetKey", [win, key]) }
fn get_mouse_button(win, btn) { "Returns the state of a mouse button." _call("glfwGetMouseButton", [win, btn]) }

fn get_cursor_pos(win) {
   "Returns the cursor position as [x, y]."
   if(!_cursor_xp){ _cursor_xp = malloc(8) _cursor_yp = malloc(8) }
   _call("glfwGetCursorPos", [win, _cursor_xp, _cursor_yp])
   [load64_f64(_cursor_xp, 0), load64_f64(_cursor_yp, 0)]
}

;; Callbacks

fn set_key_callback(win, cb)          { "Sets the key input callback." _call("glfwSetKeyCallback", [win, cb]) }
fn set_mouse_button_callback(win, cb) { "Sets the mouse button callback." _call("glfwSetMouseButtonCallback", [win, cb]) }
fn set_scroll_callback(win, cb)       { "Sets the scroll callback." _call("glfwSetScrollCallback", [win, cb]) }
fn set_cursor_pos_callback(win, cb)   { "Sets the cursor position callback." _call("glfwSetCursorPosCallback", [win, cb]) }
fn set_window_size_callback(win, cb)  { "Sets the window resize callback." _call("glfwSetWindowSizeCallback", [win, cb]) }
fn set_close_callback(win, cb)        { "Sets the window close callback." _call("glfwSetWindowCloseCallback", [win, cb]) }

;; Vulkan integration

fn vulkan_supported() { "Returns true if Vulkan is supported by the backend." _call("glfwVulkanSupported", []) != 0 }

fn required_extensions() {
   "Returns the Vulkan instance extensions required by GLFW."
   _ensure_size_bufs()
   def exts = _call("glfwGetRequiredInstanceExtensions", [_size_wp])
   [load32(_size_wp, 0), exts]
}

fn create_surface(instance, win, allocator, surface) {
   "Creates a Vulkan window surface."
   _call("glfwCreateWindowSurface", [instance, win, allocator, surface])
}

fn get_backend_name() { "Returns the backend name." "glfw" }
