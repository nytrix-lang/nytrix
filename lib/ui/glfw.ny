;; Keywords: ui glfw
;; GLFW backend — verified constants and externs.

module std.ui.glfw (
   init, terminate,
   create_window, destroy_window, should_close, set_should_close,
   set_title, get_pos, set_pos, get_size, get_framebuffer_size, set_size,
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

   joystick_present, get_joystick_name, get_joystick_guid, get_joystick_axes, get_joystick_buttons,
   joystick_is_gamepad, get_gamepad_state, get_gamepad_name, set_joystick_callback, update_gamepad_mappings,
   JOYSTICK_1, JOYSTICK_2, JOYSTICK_3, JOYSTICK_4, JOYSTICK_5, JOYSTICK_6, JOYSTICK_7, JOYSTICK_8,
   JOYSTICK_9, JOYSTICK_10, JOYSTICK_11, JOYSTICK_12, JOYSTICK_13, JOYSTICK_14, JOYSTICK_15, JOYSTICK_16,
   JOYSTICK_LAST,
   GAMEPAD_BUTTON_A, GAMEPAD_BUTTON_B, GAMEPAD_BUTTON_X, GAMEPAD_BUTTON_Y,
   GAMEPAD_BUTTON_LEFT_BUMPER, GAMEPAD_BUTTON_RIGHT_BUMPER,
   GAMEPAD_BUTTON_BACK, GAMEPAD_BUTTON_START, GAMEPAD_BUTTON_GUIDE,
   GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB,
   GAMEPAD_BUTTON_DPAD_UP, GAMEPAD_BUTTON_DPAD_RIGHT, GAMEPAD_BUTTON_DPAD_DOWN, GAMEPAD_BUTTON_DPAD_LEFT,
   GAMEPAD_BUTTON_LAST,
   GAMEPAD_BUTTON_CROSS, GAMEPAD_BUTTON_CIRCLE, GAMEPAD_BUTTON_SQUARE, GAMEPAD_BUTTON_TRIANGLE,
   GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y,
   GAMEPAD_AXIS_LEFT_TRIGGER, GAMEPAD_AXIS_RIGHT_TRIGGER, GAMEPAD_AXIS_LAST,

   GLFW_RELEASE, GLFW_PRESS, GLFW_REPEAT,

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

   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_CAPS_LOCK, MOD_NUM_LOCK,

   MOUSE_BUTTON_1, MOUSE_BUTTON_2, MOUSE_BUTTON_3, MOUSE_BUTTON_4,
   MOUSE_BUTTON_5, MOUSE_BUTTON_6, MOUSE_BUTTON_7, MOUSE_BUTTON_8,
   MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,

   CURSOR, STICKY_KEYS, STICKY_MOUSE_BUTTONS, LOCK_KEY_MODS, RAW_MOUSE_MOTION,
   CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_DISABLED, CURSOR_CAPTURED,

   CONNECTED, DISCONNECTED,

   CLIENT_API, NO_API, OPENGL_API, OPENGL_ES_API,
   RESIZABLE, VISIBLE, DECORATED, FOCUSED, AUTO_ICONIFY, FLOATING,
   MAXIMIZED, CENTER_CURSOR, TRANSPARENT_FRAMEBUFFER, FOCUS_ON_SHOW,
   SAMPLES,

   get_backend_name
)

use std.core *
use std.core.mem *
use std.ui.consts *
use std.util.common as common

mut _debug = -1
fn _is_debug(){ _debug = common.cached_env_truthy(_debug, "NY_UI_DEBUG") _debug }

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

def JOYSTICK_1                  = 0
def JOYSTICK_2                  = 1
def JOYSTICK_3                  = 2
def JOYSTICK_4                  = 3
def JOYSTICK_5                  = 4
def JOYSTICK_6                  = 5
def JOYSTICK_7                  = 6
def JOYSTICK_8                  = 7
def JOYSTICK_9                  = 8
def JOYSTICK_10                 = 9
def JOYSTICK_11                 = 10
def JOYSTICK_12                 = 11
def JOYSTICK_13                 = 12
def JOYSTICK_14                 = 13
def JOYSTICK_15                 = 14
def JOYSTICK_16                 = 15
def JOYSTICK_LAST               = 15

def GAMEPAD_BUTTON_A            = 0
def GAMEPAD_BUTTON_B            = 1
def GAMEPAD_BUTTON_X            = 2
def GAMEPAD_BUTTON_Y            = 3
def GAMEPAD_BUTTON_LEFT_BUMPER  = 4
def GAMEPAD_BUTTON_RIGHT_BUMPER = 5
def GAMEPAD_BUTTON_BACK         = 6
def GAMEPAD_BUTTON_START        = 7
def GAMEPAD_BUTTON_GUIDE        = 8
def GAMEPAD_BUTTON_LEFT_THUMB   = 9
def GAMEPAD_BUTTON_RIGHT_THUMB  = 10
def GAMEPAD_BUTTON_DPAD_UP      = 11
def GAMEPAD_BUTTON_DPAD_RIGHT   = 12
def GAMEPAD_BUTTON_DPAD_DOWN    = 13
def GAMEPAD_BUTTON_DPAD_LEFT    = 14
def GAMEPAD_BUTTON_LAST         = 14

def GAMEPAD_BUTTON_CROSS        = 0
def GAMEPAD_BUTTON_CIRCLE       = 1
def GAMEPAD_BUTTON_SQUARE       = 2
def GAMEPAD_BUTTON_TRIANGLE     = 3

def GAMEPAD_AXIS_LEFT_X         = 0
def GAMEPAD_AXIS_LEFT_Y         = 1
def GAMEPAD_AXIS_RIGHT_X        = 2
def GAMEPAD_AXIS_RIGHT_Y        = 3
def GAMEPAD_AXIS_LEFT_TRIGGER   = 4
def GAMEPAD_AXIS_RIGHT_TRIGGER  = 5
def GAMEPAD_AXIS_LAST           = 5

def CURSOR                      = 0x00033001
def STICKY_KEYS                 = 0x00033002
def STICKY_MOUSE_BUTTONS        = 0x00033003
def LOCK_KEY_MODS               = 0x00033004
def RAW_MOUSE_MOTION            = 0x00033005

def CURSOR_NORMAL               = 0x00034001
def CURSOR_HIDDEN               = 0x00034002
def CURSOR_DISABLED             = 0x00034003
def CURSOR_CAPTURED             = 0x00034004

def CONNECTED                   = 0x00040001
def DISCONNECTED                = 0x00040002

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
def RED_BITS                    = 0x00021001
def GREEN_BITS                  = 0x00021002
def BLUE_BITS                   = 0x00021003
def ALPHA_BITS                  = 0x00021004
def DEPTH_BITS                  = 0x00021005
def STENCIL_BITS                = 0x00021006
def DOUBLEBUFFER                = 0x00021010
def SAMPLES                     = 0x0002100D

mut _title_buf = 0
mut _title_cap = 0
fn _title_cstr(s){
   "Internal: Converts a string to a temporary C-style string for GLFW titles."
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
   extern fn glfwGetWindowPos(win: ptr, x: ptr, y: ptr) as "glfwGetWindowPos"
   extern fn glfwSetWindowPos(win: ptr, x: i32, y: i32) as "glfwSetWindowPos"
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
   extern fn glfwJoystickPresent(jid: i32): i32 as "glfwJoystickPresent"
   extern fn glfwGetJoystickName(jid: i32): ptr as "glfwGetJoystickName"
   extern fn glfwGetJoystickGUID(jid: i32): ptr as "glfwGetJoystickGUID"
   extern fn glfwGetJoystickAxes(jid: i32, count: ptr): ptr as "glfwGetJoystickAxes"
   extern fn glfwGetJoystickButtons(jid: i32, count: ptr): ptr as "glfwGetJoystickButtons"
   extern fn glfwJoystickIsGamepad(jid: i32): i32 as "glfwJoystickIsGamepad"
   extern fn glfwGetGamepadState(jid: i32, state: ptr): i32 as "glfwGetGamepadState"
   extern fn glfwGetGamepadName(jid: i32): ptr as "glfwGetGamepadName"
   extern fn glfwSetJoystickCallback(cb: ptr): ptr as "glfwSetJoystickCallback"
   extern fn glfwUpdateGamepadMappings(s: ptr): i32 as "glfwUpdateGamepadMappings"
   extern fn glfwSetWindowOpacity(win: ptr, v: f32) as "glfwSetWindowOpacity"
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
   extern fn glfwJoystickPresent(jid: i32): i32 as "glfwJoystickPresent"
   extern fn glfwGetJoystickName(jid: i32): ptr as "glfwGetJoystickName"
   extern fn glfwGetJoystickGUID(jid: i32): ptr as "glfwGetJoystickGUID"
   extern fn glfwGetJoystickAxes(jid: i32, count: ptr): ptr as "glfwGetJoystickAxes"
   extern fn glfwGetJoystickButtons(jid: i32, count: ptr): ptr as "glfwGetJoystickButtons"
   extern fn glfwJoystickIsGamepad(jid: i32): i32 as "glfwJoystickIsGamepad"
   extern fn glfwGetGamepadState(jid: i32, state: ptr): i32 as "glfwGetGamepadState"
   extern fn glfwGetGamepadName(jid: i32): ptr as "glfwGetGamepadName"
   extern fn glfwSetJoystickCallback(cb: ptr): ptr as "glfwSetJoystickCallback"
   extern fn glfwUpdateGamepadMappings(s: ptr): i32 as "glfwUpdateGamepadMappings"
}

;; Lifecycle

mut _ready = false

fn init(){
   "Initializes the GLFW library. Returns true on success."
   if(_ready){ return true }
   if(glfwInit() == 0){ return false }
   _ready = true
   true
}

fn terminate(){
   "Terminates the GLFW library and frees its resources."
   if(_ready){ glfwTerminate() _ready = false }
}

;; Window management

fn apply_hints(flags){
   "Internal: Applies GLFW window hints based on Nytrix window flags."
   if(band(flags, 32)){ ;; WINDOW_TRANSPARENT
      glfwWindowHint(TRANSPARENT_FRAMEBUFFER, 1)
   } else {
      glfwWindowHint(TRANSPARENT_FRAMEBUFFER, 0)
   }
   if(band(flags, 1) || band(flags, 32)){ glfwWindowHint(DECORATED, 0) } ;; FORCE borderless for transparency
   if(band(flags, 2)){  glfwWindowHint(RESIZABLE, 0) }
   if(band(flags, 4096)){ glfwWindowHint(FLOATING, 1) }
   if(band(flags, 1024)){ glfwWindowHint(MAXIMIZED, 1) }
   if(band(flags, 512)){  glfwWindowHint(VISIBLE, 0) }
}

fn create_window(title, w, h, flags=0){
   "Creates a new GLFW window with Vulkan support and the specified hints."
   init()
   if(!title){ title = "nytrix" }
   glfwWindowHint(CLIENT_API, NO_API)
   apply_hints(flags)
   mut win = glfwCreateWindow(w, h, cstr(title), 0, 0)
   win
}

fn destroy_window(win){
   "Destroys the specified GLFW window."
   if(win){ glfwDestroyWindow(win) }
}
fn should_close(win){
   "Checks if the window's close flag is set."
   glfwWindowShouldClose(win) != 0
}
fn set_should_close(win, v=1){
   "Sets or clears the window's close flag."
   glfwSetWindowShouldClose(win, v)
}
fn set_title(win, title){
   "Sets the title of the specified window."
   glfwSetWindowTitle(win, cstr(title))
}
fn get_pos(win){
   "Returns the window's screen position as [x, y]."
   _ensure_size_bufs()
   glfwGetWindowPos(win, _size_wp, _size_hp)
   [load32(_size_wp, 0), load32(_size_hp, 0)]
}
fn set_pos(win, x, y){
   "Moves the window to the specified screen position."
   glfwSetWindowPos(win, x, y)
}
@jit
fn poll_events(){
   "Processes all pending GLFW events."
   if(_ready){ glfwPollEvents() }
}
@jit
fn swap_buffers(win){
   "Swaps the front and back buffers for the specified window."
   glfwSwapBuffers(win)
}
fn swap_interval(n){
   "Sets the swap interval (vsync) for the current context."
   glfwSwapInterval(n)
}

mut _size_wp = 0
mut _size_hp = 0

fn _ensure_size_bufs(){
   "Internal: Ensures temporary buffers for size/position retrieval are allocated."
   if(!_size_wp){ _size_wp = malloc(4) _size_hp = malloc(4) }
}

fn _size_pair(win, framebuffer=false){
   "Internal: Retrieves window or framebuffer dimensions as a pair."
   _ensure_size_bufs()
   if(framebuffer){
      glfwGetFramebufferSize(win, _size_wp, _size_hp)
   } else {
      glfwGetWindowSize(win, _size_wp, _size_hp)
   }
   [load32(_size_wp, 0), load32(_size_hp, 0)]
}

fn get_size(win){
   "Returns the window's client area size as [w, h]."
   _size_pair(win)
}

fn get_framebuffer_size(win){
   "Returns the window's framebuffer size in pixels as [w, h]."
   _size_pair(win, true)
}

fn set_size(win, w, h){
   "Sets the window's client area size."
   glfwSetWindowSize(win, w, h)
}

;; Input

mut _cursor_xp = 0
mut _cursor_yp = 0

@jit
fn get_key(win, key){
   "Returns the current state of a physical key (GLFW_PRESS or GLFW_RELEASE)."
   glfwGetKey(win, key)
}
@jit
fn get_mouse_button(win, btn){
   "Returns the current state of a mouse button (GLFW_PRESS or GLFW_RELEASE)."
   glfwGetMouseButton(win, btn)
}

fn get_cursor_pos(win){
   "Returns the current mouse cursor position relative to the client area."
   if(!_cursor_xp){ _cursor_xp = malloc(8) _cursor_yp = malloc(8) }
   glfwGetCursorPos(win, _cursor_xp, _cursor_yp)
   [load64_f64(_cursor_xp, 0), load64_f64(_cursor_yp, 0)]
}

fn set_cursor_pos(win, x, y){
   "Moves the mouse cursor to the specified client area coordinates."
   glfwSetCursorPos(win, float(x), float(y))
}

;; Callbacks

fn set_key_callback(win, cb){
   "Sets the key event callback for the window."
   glfwSetKeyCallback(win, cb)
}
fn set_mouse_button_callback(win, cb){
   "Sets the mouse button event callback for the window."
   glfwSetMouseButtonCallback(win, cb)
}
fn set_scroll_callback(win, cb){
   "Sets the mouse scroll event callback for the window."
   glfwSetScrollCallback(win, cb)
}
fn set_cursor_pos_callback(win, cb){
   "Sets the cursor position event callback for the window."
   glfwSetCursorPosCallback(win, cb)
}
fn set_window_size_callback(win, cb){
   "Sets the window resize event callback for the window."
   glfwSetWindowSizeCallback(win, cb)
}
fn set_close_callback(win, cb){
   "Sets the window close event callback for the window."
   glfwSetCloseCallback(win, cb)
}

fn set_char_callback(win, cb){
   "Sets the character input event callback for the window."
   if(_is_debug()){ print("GLFW: set_char_callback win=", win, " cb=", cb) }
   glfwSetCharCallback(win, cb)
}

;; Vulkan integration

fn vulkan_supported(){
   "Checks if the system supports Vulkan via GLFW."
   glfwVulkanSupported() != 0
}

fn required_extensions(){
   "Returns the Vulkan instance extensions required by GLFW for surface creation."
   _ensure_size_bufs()
   def exts = glfwGetRequiredInstanceExtensions(_size_wp)
   [load32(_size_wp, 0), exts]
}

fn create_surface(instance, win, allocator, surface){
   "Creates a Vulkan surface for the specified window."
   glfwCreateWindowSurface(instance, win, allocator, surface)
}

fn get_backend_name(){ "glfw" }

fn set_input_mode(win, mode, value){
   "Configures GLFW input modes (e.g., cursor visibility, sticky keys)."
   glfwSetInputMode(win, mode, value)
}

fn focus_window(win){
   "Brings the specified window to the foreground."
   if(win){ glfwFocusWindow(win) }
}

fn set_clipboard(win, s){
   "Sets the system clipboard content for the specified window context."
   if(win){ glfwSetClipboardString(win, cstr(s)) }
}

fn get_clipboard(win){
   "Retrieves the current system clipboard content."
   if(!win){ return "" }
   def p = glfwGetClipboardString(win)
   if(!p){ return "" }
   cstr_to_str(p)
}

;; Joystick / Gamepad

fn joystick_present(jid){
   "Returns true if the specified joystick is present."
   glfwJoystickPresent(jid) != 0
}

fn get_joystick_name(jid){
   "Returns the name of the specified joystick."
   def p = glfwGetJoystickName(jid)
   if(!p){ return "Unknown" }
   cstr_to_str(p)
}

fn get_joystick_guid(jid){
   "Returns the SDL-compatible GUID of the specified joystick."
   def p = glfwGetJoystickGUID(jid)
   if(!p){ return "00000000000000000000000000000000" }
   cstr_to_str(p)
}

fn get_joystick_axes(jid, count_ptr){
   "Returns a pointer to the axis values of the specified joystick."
   glfwGetJoystickAxes(jid, count_ptr)
}

fn get_joystick_buttons(jid, count_ptr){
   "Returns a pointer to the button states of the specified joystick."
   glfwGetJoystickButtons(jid, count_ptr)
}

fn joystick_is_gamepad(jid){
   "Returns true if the specified joystick has a gamepad mapping."
   glfwJoystickIsGamepad(jid) != 0
}

fn get_gamepad_state(jid, state_ptr){
   "Retrieves the state of the specified joystick as a gamepad."
   glfwGetGamepadState(jid, state_ptr) != 0
}

fn get_gamepad_name(jid){
   "Returns the name of the gamepad mapping."
   def p = glfwGetGamepadName(jid)
   if(!p){ return "Generic Gamepad" }
   cstr_to_str(p)
}

fn set_joystick_callback(cb){
   "Sets the joystick connection callback."
   glfwSetJoystickCallback(cb)
}

fn update_gamepad_mappings(s){
   "Updates the gamepad mappings from a string."
   glfwUpdateGamepadMappings(cstr(s))
}
