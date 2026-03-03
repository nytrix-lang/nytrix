;; Keywords: ui glfw
;; GLFW backend — verified constants and externs.

module std.ui.glfw (
   init, terminate,
   create_window, destroy_window, should_close, set_should_close,
   set_title, get_size, get_framebuffer_size, set_size,
   poll_events, swap_buffers, swap_interval,
   get_key, get_mouse_button, get_cursor_pos,
   set_key_callback, set_mouse_button_callback,
   set_scroll_callback, set_cursor_pos_callback,
   set_window_size_callback, set_close_callback,
   set_char_callback,
   set_input_mode,
   vulkan_supported, required_extensions, create_surface,
   apply_hints,
   focus_window,
   set_clipboard, get_clipboard,

   ; Verified Constants (GLFW 3.5)
   GLFW_RELEASE, GLFW_PRESS, GLFW_REPEAT,

   ; Keys
   KEY_SPACE, KEY_APOSTROPHE, KEY_COMMA, KEY_MINUS, KEY_PERIOD, KEY_SLASH,
   KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
   KEY_SEMICOLON, KEY_EQUAL,
   KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
   KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
   KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
   KEY_LEFT_BRACKET, KEY_BACKSLASH, KEY_RIGHT_BRACKET, KEY_GRAVE_ACCENT,
   KEY_WORLD_1, KEY_WORLD_2,

   KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_INSERT, KEY_DELETE,
   KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
   KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END,
   KEY_CAPS_LOCK, KEY_SCROLL_LOCK, KEY_NUM_LOCK, KEY_PRINT_SCREEN, KEY_PAUSE,
   KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
   KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,

   KEY_LEFT_SHIFT, KEY_LEFT_CONTROL, KEY_LEFT_ALT, KEY_LEFT_SUPER,
   KEY_RIGHT_SHIFT, KEY_RIGHT_CONTROL, KEY_RIGHT_ALT, KEY_RIGHT_SUPER,
   KEY_MENU,

   ; Modifiers
   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_CAPS_LOCK, MOD_NUM_LOCK,

   ; Mouse
   MOUSE_BUTTON_1, MOUSE_BUTTON_2, MOUSE_BUTTON_3, MOUSE_BUTTON_4,
   MOUSE_BUTTON_5, MOUSE_BUTTON_6, MOUSE_BUTTON_7, MOUSE_BUTTON_8,
   MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,

   ; Input Modes
   CURSOR, STICKY_KEYS, STICKY_MOUSE_BUTTONS, LOCK_KEY_MODS, RAW_MOUSE_MOTION,
   CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_DISABLED, CURSOR_CAPTURED,

   ; Window Hints
   CLIENT_API, NO_API, OPENGL_API, OPENGL_ES_API,
   RESIZABLE, VISIBLE, DECORATED, FOCUSED, AUTO_ICONIFY, FLOATING,
   MAXIMIZED, CENTER_CURSOR, TRANSPARENT_FRAMEBUFFER, FOCUS_ON_SHOW,
   SAMPLES,

   get_backend_name
)

use std.core *
use std.core.mem *
use std.ui.consts *

; Constants from glfw3.h
def GLFW_RELEASE                = 0
def GLFW_PRESS                  = 1
def GLFW_REPEAT                 = 2

def KEY_SPACE                   = 32
def KEY_APOSTROPHE              = 39
def KEY_COMMA                   = 44
def KEY_MINUS                   = 45
def KEY_PERIOD                  = 46
def KEY_SLASH                   = 47
def KEY_0                       = 48
def KEY_1                       = 49
def KEY_2                       = 50
def KEY_3                       = 51
def KEY_4                       = 52
def KEY_5                       = 53
def KEY_6                       = 54
def KEY_7                       = 55
def KEY_8                       = 56
def KEY_9                       = 57
def KEY_SEMICOLON               = 59
def KEY_EQUAL                   = 61
def KEY_A                       = 65
def KEY_B                       = 66
def KEY_C                       = 67
def KEY_D                       = 68
def KEY_E                       = 69
def KEY_F                       = 70
def KEY_G                       = 71
def KEY_H                       = 72
def KEY_I                       = 73
def KEY_J                       = 74
def KEY_K                       = 75
def KEY_L                       = 76
def KEY_M                       = 77
def KEY_N                       = 78
def KEY_O                       = 79
def KEY_P                       = 80
def KEY_Q                       = 81
def KEY_R                       = 82
def KEY_S                       = 83
def KEY_T                       = 84
def KEY_U                       = 85
def KEY_V                       = 86
def KEY_W                       = 87
def KEY_X                       = 88
def KEY_Y                       = 89
def KEY_Z                       = 90
def KEY_LEFT_BRACKET            = 91
def KEY_BACKSLASH               = 92
def KEY_RIGHT_BRACKET           = 93
def KEY_GRAVE_ACCENT            = 96
def KEY_WORLD_1                 = 161
def KEY_WORLD_2                 = 162

def KEY_ESCAPE                  = 256
def KEY_ENTER                   = 257
def KEY_TAB                     = 258
def KEY_BACKSPACE               = 259
def KEY_INSERT                  = 260
def KEY_DELETE                  = 261
def KEY_RIGHT                   = 262
def KEY_LEFT                    = 263
def KEY_DOWN                    = 264
def KEY_UP                      = 265
def KEY_PAGE_UP                 = 266
def KEY_PAGE_DOWN               = 267
def KEY_HOME                    = 268
def KEY_END                     = 269
def KEY_CAPS_LOCK               = 280
def KEY_SCROLL_LOCK             = 281
def KEY_NUM_LOCK                = 282
def KEY_PRINT_SCREEN            = 283
def KEY_PAUSE                   = 284
def KEY_F1                      = 290
def KEY_F2                      = 291
def KEY_F3                      = 292
def KEY_F4                      = 293
def KEY_F5                      = 294
def KEY_F6                      = 295
def KEY_F7                      = 296
def KEY_F8                      = 297
def KEY_F9                      = 298
def KEY_F10                     = 299
def KEY_F11                     = 300
def KEY_F12                     = 301
def KEY_LEFT_SHIFT              = 340
def KEY_LEFT_CONTROL            = 341
def KEY_LEFT_ALT                = 342
def KEY_LEFT_SUPER              = 343
def KEY_RIGHT_SHIFT             = 344
def KEY_RIGHT_CONTROL           = 345
def KEY_RIGHT_ALT               = 346
def KEY_RIGHT_SUPER             = 347
def KEY_MENU                    = 348

def MOD_SHIFT                   = 0x0001
def MOD_CONTROL                 = 0x0002
def MOD_ALT                     = 0x0004
def MOD_SUPER                   = 0x0008
def MOD_CAPS_LOCK               = 0x0010
def MOD_NUM_LOCK                = 0x0020

def MOUSE_BUTTON_1              = 0
def MOUSE_BUTTON_2              = 1
def MOUSE_BUTTON_3              = 2
def MOUSE_BUTTON_4              = 3
def MOUSE_BUTTON_5              = 4
def MOUSE_BUTTON_6              = 5
def MOUSE_BUTTON_7              = 6
def MOUSE_BUTTON_8              = 7
def MOUSE_BUTTON_LEFT           = 0
def MOUSE_BUTTON_RIGHT          = 1
def MOUSE_BUTTON_MIDDLE         = 2

def CURSOR                      = 0x00033001
def STICKY_KEYS                 = 0x00033002
def STICKY_MOUSE_BUTTONS        = 0x00033003
def LOCK_KEY_MODS               = 0x00033004
def RAW_MOUSE_MOTION            = 0x00033005

def CURSOR_NORMAL               = 0x00034001
def CURSOR_HIDDEN               = 0x00034002
def CURSOR_DISABLED             = 0x00034003
def CURSOR_CAPTURED             = 0x00034004

def CLIENT_API                  = 0x00022001
def NO_API                      = 0
def OPENGL_API                  = 0x00030001
def OPENGL_ES_API               = 0x00030002
def RESIZABLE                   = 0x00020003
def VISIBLE                     = 0x00020004
def DECORATED                   = 0x00020005
def FOCUSED                     = 0x00020001
def AUTO_ICONIFY                = 0x00020006
def FLOATING                    = 0x00020007
def MAXIMIZED                   = 0x00020008
def CENTER_CURSOR               = 0x00020009
def TRANSPARENT_FRAMEBUFFER     = 0x0002000A
def FOCUS_ON_SHOW               = 0x0002000C
def SAMPLES                     = 0x0002100D

mut _title_buf = 0
mut _title_cap = 0
fn _title_cstr(s){
   s = cstr(s, "nytrix")
   def n = str_len(s)
   if(_title_cap < n + 1){
      def newp = realloc(_title_buf, n + 1)
      if(!newp){ return 0 }
      _title_buf = newp
      _title_cap = n + 1
   }
   strcpy(_title_buf, s)
   _title_buf
}

if(comptime{ __os_name() == "linux" || __os_name() == "macos" }){
   #link "glfw"

   extern fn glfwInit(): i32 as "glfwInit"
   extern fn glfwTerminate() as "glfwTerminate"
   extern fn glfwCreateWindow(w: i32, h: i32, title: ptr, monitor: ptr, share: ptr): ptr as "glfwCreateWindow"
   extern fn glfwDestroyWindow(win: ptr) as "glfwDestroyWindow"
   extern fn glfwWindowShouldClose(win: ptr): i32 as "glfwWindowShouldClose"
   extern fn glfwSetWindowShouldClose(win: ptr, v: i32) as "glfwSetWindowShouldClose"
   extern fn glfwSetWindowTitle(win: ptr, title: ptr) as "glfwSetWindowTitle"
   extern fn glfwPollEvents() as "glfwPollEvents"
   extern fn glfwSwapBuffers(win: ptr) as "glfwSwapBuffers"
   extern fn glfwSwapInterval(n: i32) as "glfwSwapInterval"
   extern fn glfwWindowHint(hint: i32, value: i32) as "glfwWindowHint"
   extern fn glfwGetWindowSize(win: ptr, w: ptr, h: ptr) as "glfwGetWindowSize"
   extern fn glfwGetFramebufferSize(win: ptr, w: ptr, h: ptr) as "glfwGetFramebufferSize"
   extern fn glfwSetWindowSize(win: ptr, w: i32, h: i32) as "glfwSetWindowSize"
   extern fn glfwGetKey(win: ptr, key: i32): i32 as "glfwGetKey"
   extern fn glfwGetMouseButton(win: ptr, btn: i32): i32 as "glfwGetMouseButton"
   extern fn glfwGetCursorPos(win: ptr, xpos: ptr, ypos: ptr) as "glfwGetCursorPos"
   extern fn glfwSetCursorPos(win: ptr, xpos: f64, ypos: f64) as "glfwSetCursorPos"
   extern fn glfwSetKeyCallback(win: ptr, cb: ptr): ptr as "glfwSetKeyCallback"
   extern fn glfwSetMouseButtonCallback(win: ptr, cb: ptr): ptr as "glfwSetMouseButtonCallback"
   extern fn glfwSetScrollCallback(win: ptr, cb: ptr): ptr as "glfwSetScrollCallback"
   extern fn glfwSetCursorPosCallback(win: ptr, cb: ptr): ptr as "glfwSetCursorPosCallback"
   extern fn glfwSetWindowSizeCallback(win: ptr, cb: ptr): ptr as "glfwSetWindowSizeCallback"
   extern fn glfwSetCloseCallback(win: ptr, cb: ptr): ptr as "glfwSetWindowCloseCallback"
   extern fn glfwVulkanSupported(): i32 as "glfwVulkanSupported"
   extern fn glfwGetRequiredInstanceExtensions(count: ptr): ptr as "glfwGetRequiredInstanceExtensions"
   extern fn glfwCreateWindowSurface(instance: ptr, win: ptr, alloc: ptr, surface: ptr): i32 as "glfwCreateWindowSurface"
   extern fn glfwSetCharCallback(win: ptr, cb: ptr): ptr as "glfwSetCharCallback"
   extern fn glfwSetInputMode(win: ptr, mode: i32, value: i32): i32 as "glfwSetInputMode"
   extern fn glfwFocusWindow(win: ptr) as "glfwFocusWindow"
   extern fn glfwSetClipboardString(win: ptr, s: ptr) as "glfwSetClipboardString"
   extern fn glfwGetClipboardString(win: ptr): ptr as "glfwGetClipboardString"
} else if(comptime{ __os_name() == "windows" }){
   #link "glfw3"

   extern fn glfwInit(): i32 as "glfwInit"
   extern fn glfwTerminate() as "glfwTerminate"
   extern fn glfwCreateWindow(w: i32, h: i32, title: ptr, monitor: ptr, share: ptr): ptr as "glfwCreateWindow"
   extern fn glfwDestroyWindow(win: ptr) as "glfwDestroyWindow"
   extern fn glfwWindowShouldClose(win: ptr): i32 as "glfwWindowShouldClose"
   extern fn glfwSetWindowShouldClose(win: ptr, v: i32) as "glfwSetWindowShouldClose"
   extern fn glfwSetWindowTitle(win: ptr, title: ptr) as "glfwSetWindowTitle"
   extern fn glfwPollEvents() as "glfwPollEvents"
   extern fn glfwSwapBuffers(win: ptr) as "glfwSwapBuffers"
   extern fn glfwSwapInterval(n: i32) as "glfwSwapInterval"
   extern fn glfwWindowHint(hint: i32, value: i32) as "glfwWindowHint"
   extern fn glfwGetWindowSize(win: ptr, w: ptr, h: ptr) as "glfwGetWindowSize"
   extern fn glfwGetFramebufferSize(win: ptr, w: ptr, h: ptr) as "glfwGetFramebufferSize"
   extern fn glfwSetWindowSize(win: ptr, w: i32, h: i32) as "glfwSetWindowSize"
   extern fn glfwGetKey(win: ptr, key: i32): i32 as "glfwGetKey"
   extern fn glfwGetMouseButton(win: ptr, btn: i32): i32 as "glfwGetMouseButton"
   extern fn glfwGetCursorPos(win: ptr, xpos: ptr, ypos: ptr) as "glfwGetCursorPos"
   extern fn glfwSetCursorPos(win: ptr, xpos: f64, ypos: f64) as "glfwSetCursorPos"
   extern fn glfwSetKeyCallback(win: ptr, cb: ptr): ptr as "glfwSetKeyCallback"
   extern fn glfwSetMouseButtonCallback(win: ptr, cb: ptr): ptr as "glfwSetMouseButtonCallback"
   extern fn glfwSetScrollCallback(win: ptr, cb: ptr): ptr as "glfwSetScrollCallback"
   extern fn glfwSetCursorPosCallback(win: ptr, cb: ptr): ptr as "glfwSetCursorPosCallback"
   extern fn glfwSetWindowSizeCallback(win: ptr, cb: ptr): ptr as "glfwSetWindowSizeCallback"
   extern fn glfwSetCloseCallback(win: ptr, cb: ptr): ptr as "glfwSetWindowCloseCallback"
   extern fn glfwVulkanSupported(): i32 as "glfwVulkanSupported"
   extern fn glfwGetRequiredInstanceExtensions(count: ptr): ptr as "glfwGetRequiredInstanceExtensions"
   extern fn glfwCreateWindowSurface(instance: ptr, win: ptr, alloc: ptr, surface: ptr): i32 as "glfwCreateWindowSurface"
   extern fn glfwSetCharCallback(win: ptr, cb: ptr): ptr as "glfwSetCharCallback"
   extern fn glfwSetInputMode(win: ptr, mode: i32, value: i32): i32 as "glfwSetInputMode"
   extern fn glfwFocusWindow(win: ptr) as "glfwFocusWindow"
   extern fn glfwSetClipboardString(win: ptr, s: ptr) as "glfwSetClipboardString"
   extern fn glfwGetClipboardString(win: ptr): ptr as "glfwGetClipboardString"
}

;; Lifecycle

mut _ready = false

fn init(){
   if(_ready){ return true }
   if(glfwInit() == 0){ return false }
   _ready = true
   true
}

fn terminate(){
   if(_ready){ glfwTerminate() _ready = false }
}

;; Window management

fn apply_hints(flags){
   if((flags & WINDOW_TRANSPARENT) != 0){ glfwWindowHint(TRANSPARENT_FRAMEBUFFER, 1) }
   if((flags & WINDOW_NO_BORDER)   != 0){ glfwWindowHint(DECORATED, 0) }
   if((flags & WINDOW_NO_RESIZE)   != 0){ glfwWindowHint(RESIZABLE, 0) }
   if((flags & WINDOW_FLOATING)    != 0){ glfwWindowHint(FLOATING, 1) }
   if((flags & WINDOW_MAXIMIZE)    != 0){ glfwWindowHint(MAXIMIZED, 1) }
   if((flags & WINDOW_HIDE)        != 0){ glfwWindowHint(VISIBLE, 0) }
}

fn create_window(title, w, h, flags=0){
   init()
   if(!title){ title = "nytrix" }
   glfwWindowHint(CLIENT_API, NO_API)
   apply_hints(flags)
   glfwCreateWindow(w, h, _title_cstr(title), 0, 0)
}

fn destroy_window(win){
   if(win){ glfwDestroyWindow(win) }
}
fn should_close(win){
   glfwWindowShouldClose(win) != 0
}
fn set_should_close(win, v=1){
   glfwSetWindowShouldClose(win, v)
}
fn set_title(win, title){
   glfwSetWindowTitle(win, _title_cstr(title))
}
fn poll_events(){
   if(_ready){ glfwPollEvents() }
}
fn swap_buffers(win){
   glfwSwapBuffers(win)
}
fn swap_interval(n){
   glfwSwapInterval(n)
}

mut _size_wp = 0
mut _size_hp = 0

fn _ensure_size_bufs(){
   if(!_size_wp){ _size_wp = malloc(4) _size_hp = malloc(4) }
}

fn _size_pair(win, framebuffer=false){
   _ensure_size_bufs()
   if(framebuffer){
      glfwGetFramebufferSize(win, _size_wp, _size_hp)
   } else {
      glfwGetWindowSize(win, _size_wp, _size_hp)
   }
   [load32(_size_wp, 0), load32(_size_hp, 0)]
}

fn get_size(win){
   _size_pair(win)
}

fn get_framebuffer_size(win){
   _size_pair(win, true)
}

fn set_size(win, w, h){
   glfwSetWindowSize(win, w, h)
}

;; Input

mut _cursor_xp = 0
mut _cursor_yp = 0

fn get_key(win, key){
   glfwGetKey(win, key)
}
fn get_mouse_button(win, btn){
   glfwGetMouseButton(win, btn)
}

fn get_cursor_pos(win){
   if(!_cursor_xp){ _cursor_xp = malloc(8) _cursor_yp = malloc(8) }
   glfwGetCursorPos(win, _cursor_xp, _cursor_yp)
   [load64_f64(_cursor_xp, 0), load64_f64(_cursor_yp, 0)]
}

fn set_cursor_pos(win, x, y){
   glfwSetCursorPos(win, float(x), float(y))
}

;; Callbacks

fn set_key_callback(win, cb){
   glfwSetKeyCallback(win, cb)
}
fn set_mouse_button_callback(win, cb){
   glfwSetMouseButtonCallback(win, cb)
}
fn set_scroll_callback(win, cb){
   glfwSetScrollCallback(win, cb)
}
fn set_cursor_pos_callback(win, cb){
   glfwSetCursorPosCallback(win, cb)
}
fn set_window_size_callback(win, cb){
   glfwSetWindowSizeCallback(win, cb)
}
fn set_close_callback(win, cb){
   glfwSetCloseCallback(win, cb)
}

fn set_char_callback(win, cb){
   glfwSetCharCallback(win, cb)
}

;; Vulkan integration

fn vulkan_supported(){
   glfwVulkanSupported() != 0
}

fn required_extensions(){
   _ensure_size_bufs()
   def exts = glfwGetRequiredInstanceExtensions(_size_wp)
   [load32(_size_wp, 0), exts]
}

fn create_surface(instance, win, allocator, surface){
   glfwCreateWindowSurface(instance, win, allocator, surface)
}

fn get_backend_name(){ "glfw" }

fn set_input_mode(win, mode, value){
   glfwSetInputMode(win, mode, value)
}

fn focus_window(win){
   if(win){ glfwFocusWindow(win) }
}

fn set_clipboard(win, s){
   if(win){ glfwSetClipboardString(win, cstr(s)) }
}

fn get_clipboard(win){
   if(!win){ return "" }
   def p = glfwGetClipboardString(win)
   if(!p){ return "" }
   cstr_to_str(p)
}
