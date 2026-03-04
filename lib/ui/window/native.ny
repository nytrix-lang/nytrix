;; Keywords: ui window native
;; Native window backend — platform-delegating API layer.

module std.ui.window.native (
   init, terminate,
   create_window, destroy_window, should_close, set_should_close,
   set_title, get_pos, set_pos, get_size, get_framebuffer_size, set_size,
   get_window_attrib, set_window_opacity,
   poll_events, swap_buffers, swap_interval,
   get_key, get_mouse_button, get_cursor_pos,
   set_key_callback, set_mouse_button_callback,
   set_scroll_callback, set_cursor_pos_callback,
   set_window_size_callback, set_close_callback,
   set_char_callback,
   set_input_mode,
   vulkan_supported, required_extensions, create_surface,
   get_instance_proc_address, make_context_current, get_current_context, extension_supported,
   get_osmesa_context, get_osmesa_color_buffer, get_osmesa_depth_buffer,
   get_win32_window, get_win32_adapter, get_win32_monitor, get_wgl_context,
   get_cocoa_window, get_cocoa_monitor, get_cocoa_view, get_nsgl_context,
   apply_hints,
   focus_window,
   set_clipboard, get_clipboard,

   joystick_present, get_joystick_name, get_joystick_guid, get_joystick_axes, get_joystick_buttons, get_joystick_hats,
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
   CONTEXT_CREATION_API, NATIVE_CONTEXT_API, EGL_CONTEXT_API, OSMESA_CONTEXT_API,
   X11_CLASS_NAME, X11_INSTANCE_NAME, WAYLAND_APP_ID, PLATFORM,
   JOYSTICK_HAT_BUTTONS, WAYLAND_LIBDECOR, X11_XCB_VULKAN_SURFACE, COCOA_RETINA_FRAMEBUFFER,
   COCOA_FRAME_NAME, COCOA_GRAPHICS_SWITCHING, WIN32_KEYBOARD_MENU, WIN32_SHOWDEFAULT,

   get_backend_name
)

use std.core *
use std.core.mem *
use std.ui.consts *
use std.util.common as common
use std.ui.window.platform as ui_backend

mut _debug = -1
fn _is_debug(){ _debug = common.cached_env_truthy(_debug, "NY_UI_DEBUG") _debug }
fn _dbg(msg){ if(_is_debug()){ print("[window:native] " + msg) } }

; Window input constants
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

def CONTEXT_CREATION_API        = 0x0002200B
def NATIVE_CONTEXT_API          = 0x00036001
def EGL_CONTEXT_API             = 0x00036002
def OSMESA_CONTEXT_API          = 0x00036003

def X11_CLASS_NAME              = 0x00024001
def X11_INSTANCE_NAME           = 0x00024002
def WAYLAND_APP_ID              = 0x00026001
def PLATFORM                    = 0x00050003
def JOYSTICK_HAT_BUTTONS        = 0x00050001
def WAYLAND_LIBDECOR            = 0x00026002
def X11_XCB_VULKAN_SURFACE      = 0x00052002
def COCOA_RETINA_FRAMEBUFFER    = 0x00023001
def COCOA_FRAME_NAME            = 0x00023002
def COCOA_GRAPHICS_SWITCHING    = 0x00023003
def WIN32_KEYBOARD_MENU         = 0x00025001
def WIN32_SHOWDEFAULT           = 0x00025002

mut _title_buf = 0
mut _title_cap = 0
fn _title_cstr(s){
   "Internal: Converts a string to a temporary C-style string for window titles."
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

;; Lifecycle

mut _ready = false

fn init(){
   "Initializes the window system. Returns true on success."
   if(_ready){
      _dbg("init: already ready")
      return true
   }
   _dbg("init: starting backend bootstrap")
   if(!ui_backend.init()){
      _dbg("ERROR: backend init failed")
      return false
   }
   _ready = true
   _dbg("init: ready backend=" + ui_backend.get_backend_name())
   true
}

fn terminate(){
   "Terminates the window system and frees resources."
   if(_ready){
      _dbg("terminate: backend=" + ui_backend.get_backend_name())
      ui_backend.terminate()
      _ready = false
   }
}

;; Window management

fn apply_hints(flags){
   "Internal: Applies window hints based on Nytrix window flags."
   mut hints = dict()
   hints = dict_set(hints, TRANSPARENT_FRAMEBUFFER, band(flags, 32) ? 1 : 0)
   hints = dict_set(hints, DECORATED, (band(flags, 1) || band(flags, 32)) ? 0 : 1)
   hints = dict_set(hints, RESIZABLE, band(flags, 2) ? 0 : 1)
   hints = dict_set(hints, FLOATING, band(flags, 4096) ? 1 : 0)
   hints = dict_set(hints, MAXIMIZED, band(flags, 1024) ? 1 : 0)
   hints = dict_set(hints, VISIBLE, band(flags, 512) ? 0 : 1)
   _dbg("apply_hints: flags=0x" + to_hex(flags) +
      " transparent=" + to_str(dict_get(hints, TRANSPARENT_FRAMEBUFFER, 0)) +
      " decorated=" + to_str(dict_get(hints, DECORATED, 0)) +
      " resizable=" + to_str(dict_get(hints, RESIZABLE, 0)) +
      " floating=" + to_str(dict_get(hints, FLOATING, 0)) +
      " maximized=" + to_str(dict_get(hints, MAXIMIZED, 0)) +
      " visible=" + to_str(dict_get(hints, VISIBLE, 0)))
   ui_backend.apply_hints(hints)
}

fn create_window(title, w, h, flags=0){
   "Creates a new window with Vulkan support and the specified hints."
   init()
   if(!title){ title = "nytrix" }
   _dbg("create_window: title='" + title + "' size=" + to_str(w) + "x" + to_str(h) + " flags=0x" + to_hex(flags))
   apply_hints(flags)
   def win = ui_backend.create_window(title, 0, 0, w, h, flags)
   _dbg("create_window: handle=0x" + to_hex(win))
   win
}

fn destroy_window(win){
   "Destroys the specified window."
   if(win){
      _dbg("destroy_window: win=0x" + to_hex(win))
      ui_backend.destroy_window(win)
   }
}
fn should_close(win){
   "Checks if the window's close flag is set."
   ui_backend.should_close(win)
}
fn set_should_close(win, v=1){
   "Sets or clears the window's close flag."
   _dbg("set_should_close: win=0x" + to_hex(win) + " value=" + to_str(v))
   ui_backend.set_should_close(win, v)
}
fn set_title(win, title){
   "Sets the title of the specified window."
   _dbg("set_title: win=0x" + to_hex(win) + " title='" + title + "'")
   ui_backend.set_title(win, title)
}
fn get_window_attrib(win, attrib){
   "Returns a window attribute such as TRANSPARENT_FRAMEBUFFER."
   def value = ui_backend.get_window_attrib(win, attrib)
   _dbg("get_window_attrib: win=0x" + to_hex(win) + " attrib=0x" + to_hex(attrib) + " value=" + to_str(value))
   value
}
fn get_pos(win){
   "Returns the window's screen position as [x, y]."
   ui_backend.get_pos(win)
}
fn set_pos(win, x, y){
   "Moves the window to the specified screen position."
   _dbg("set_pos: win=0x" + to_hex(win) + " pos=" + to_str(x) + "," + to_str(y))
   ui_backend.set_pos(win, x, y)
}
@jit
fn poll_events(){
   "Processes all pending window events."
   if(_ready){
      _dbg("poll_events")
      ui_backend.poll_events()
   }
}
@jit
fn swap_buffers(win){
   "Swaps the front and back buffers for the specified window."
   _dbg("swap_buffers: win=0x" + to_hex(win))
   ui_backend.swap_buffers(win)
}
fn swap_interval(n){
   "Sets the swap interval (vsync) for the current context."
   _dbg("swap_interval: interval=" + to_str(n))
   ui_backend.swap_interval(n)
}

fn get_size(win){
   "Returns the window's client area size as [w, h]."
   ui_backend.get_size(win)
}

fn get_framebuffer_size(win){
   "Returns the window's framebuffer size in pixels as [w, h]."
   ui_backend.get_framebuffer_size(win)
}

fn set_size(win, w, h){
   "Sets the window's client area size."
   _dbg("set_size: win=0x" + to_hex(win) + " size=" + to_str(w) + "x" + to_str(h))
   ui_backend.set_size(win, w, h)
}

;; Input

mut _cursor_xp = 0
mut _cursor_yp = 0

@jit
fn get_key(win, key){
   "Returns the current state of a physical key (GLFW_PRESS or GLFW_RELEASE)."
   ui_backend.get_key(win, key)
}
@jit
fn get_mouse_button(win, btn){
   "Returns the current state of a mouse button (GLFW_PRESS or GLFW_RELEASE)."
   ui_backend.get_mouse_button(win, btn)
}

fn get_cursor_pos(win){
   "Returns the current mouse cursor position relative to the client area."
   ui_backend.get_cursor_pos(win)
}

fn set_cursor_pos(win, x, y){
   "Moves the mouse cursor to the specified client area coordinates."
   _dbg("set_cursor_pos: win=0x" + to_hex(win) + " pos=" + to_str(x) + "," + to_str(y))
   ui_backend.set_cursor_pos(win, x, y)
}

;; Callbacks

fn set_key_callback(win, cb){
   "Sets the key event callback for the window."
   _dbg("set_key_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_key_callback(win, cb)
}
fn set_mouse_button_callback(win, cb){
   "Sets the mouse button event callback for the window."
   _dbg("set_mouse_button_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_mouse_button_callback(win, cb)
}
fn set_scroll_callback(win, cb){
   "Sets the mouse scroll event callback for the window."
   _dbg("set_scroll_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_scroll_callback(win, cb)
}
fn set_cursor_pos_callback(win, cb){
   "Sets the cursor position event callback for the window."
   _dbg("set_cursor_pos_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_cursor_pos_callback(win, cb)
}
fn set_window_size_callback(win, cb){
   "Sets the window resize event callback for the window."
   _dbg("set_window_size_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_window_size_callback(win, cb)
}
fn set_close_callback(win, cb){
   "Sets the window close event callback for the window."
   _dbg("set_close_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_close_callback(win, cb)
}

fn set_char_callback(win, cb){
   "Sets the character input event callback for the window."
   _dbg("set_char_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_char_callback(win, cb)
}

;; Vulkan integration

fn vulkan_supported(){
   "Checks if the system supports Vulkan."
   def ok = ui_backend.vulkan_supported()
   _dbg("vulkan_supported: " + to_str(ok))
   ok
}

fn required_extensions(){
   "Returns the Vulkan instance extensions required for surface creation."
   def exts = ui_backend.required_extensions()
   _dbg("required_extensions: " + to_str(exts))
   exts
}

fn create_surface(instance, win, allocator, surface){
   "Creates a Vulkan surface for the specified window."
   _dbg("create_surface: instance=0x" + to_hex(instance) + " win=0x" + to_hex(win) + " allocator=0x" + to_hex(allocator))
   def res = ui_backend.create_surface(instance, win, allocator, surface)
   _dbg("create_surface: result=" + to_str(res))
   res
}

fn get_backend_name(){ ui_backend.get_backend_name() }

fn set_input_mode(win, mode, value){
   "Configures window input modes (e.g., cursor visibility, sticky keys)."
   _dbg("set_input_mode: win=0x" + to_hex(win) + " mode=0x" + to_hex(mode) + " value=" + to_str(value))
   ui_backend.set_input_mode(win, mode, value)
}

fn focus_window(win){
   "Brings the specified window to the foreground."
   if(win){
      _dbg("focus_window: win=0x" + to_hex(win))
      ui_backend.focus_window(win)
   }
}

fn set_window_opacity(win, v){
   "Sets whole-window opacity when supported by the platform."
   if(win){
      _dbg("set_window_opacity: win=0x" + to_hex(win) + " opacity=" + to_str(v))
      ui_backend.set_window_opacity(win, float(v))
   }
}

fn set_clipboard(win, s){
   "Sets the system clipboard content for the specified window context."
   if(win){
      _dbg("set_clipboard: win=0x" + to_hex(win) + " bytes=" + to_str(len(s)))
      ui_backend.set_clipboard(win, s)
   }
}

fn get_clipboard(win){
   "Retrieves the current system clipboard content."
   if(!win){ return "" }
   def s = ui_backend.get_clipboard(win)
   _dbg("get_clipboard: win=0x" + to_hex(win) + " bytes=" + to_str(len(s)))
   s
}

;; Joystick / Gamepad

fn joystick_present(jid){
   "Returns true if the specified joystick is present."
   ui_backend.joystick_present(jid)
}

fn get_joystick_name(jid){
   "Returns the name of the specified joystick."
   ui_backend.get_joystick_name(jid)
}

fn get_joystick_guid(jid){
   "Returns the SDL-compatible GUID of the specified joystick."
   ui_backend.get_joystick_guid(jid)
}

fn get_joystick_axes(jid, count_ptr){
   "Returns a pointer to the axis values of the specified joystick."
   ui_backend.get_joystick_axes(jid, count_ptr)
}

fn get_joystick_buttons(jid, count_ptr){
   "Returns a pointer to the button states of the specified joystick."
   ui_backend.get_joystick_buttons(jid, count_ptr)
}

fn get_joystick_hats(jid, count_ptr){
   "Returns a pointer to the hat states of the specified joystick."
   ui_backend.get_joystick_hats(jid, count_ptr)
}

fn joystick_is_gamepad(jid){
   "Returns true if the specified joystick has a gamepad mapping."
   ui_backend.joystick_is_gamepad(jid)
}

fn get_gamepad_state(jid, state_ptr){
   "Retrieves the state of the specified joystick as a gamepad."
   ui_backend.get_gamepad_state(jid, state_ptr)
}

fn get_gamepad_name(jid){
   "Returns the name of the gamepad mapping."
   ui_backend.get_gamepad_name(jid)
}

fn set_joystick_callback(cb){
   "Sets the joystick connection callback."
   ui_backend.set_joystick_callback(cb)
}

fn update_gamepad_mappings(s){
   "Updates the gamepad mappings from a string."
   ui_backend.update_gamepad_mappings(s)
}

;; Context & GL extensions

fn make_context_current(win){
   "Makes the specified window's context current on the calling thread."
   _dbg("make_context_current: win=0x" + to_hex(win))
   ;; GL contexts are handled via specific backend APIs directly, stubbed for app compat
}

fn get_current_context(){
   "Returns the window whose context is current on the calling thread."
   0
}

fn extension_supported(ext){
   "Checks whether the specified API extension is supported."
   0
}

fn get_instance_proc_address(instance, procname){
   "Returns the address of the specified Vulkan instance function."
   ;; Standard apps should use vkGetInstanceProcAddr directly from Vulkan
   0
}

fn get_osmesa_context(win){
   "Returns the OSMesa context of the specified window."
   if(!win){ return 0 }
   def ctx = dict_get(win, "offscreen_context", 0)
   if(!ctx){ return 0 }
   dict_get(ctx, "context", 0)
}

fn get_osmesa_color_buffer(win, width_ptr, height_ptr, format_ptr, buffer_ptr){
   "Retrieves the color buffer associated with the specified OSMesa context attached to a window."
   ;; Nytrix windows using OSMesa will have this tracked
   if(!win){ return false }
   def ctx = dict_get(win, "offscreen_context", 0)
   if(!ctx){ return false }
   ;; The buffer is just a pointer, we don't return false if pointers aren't supplied but set what's available
   if(buffer_ptr){ store64(buffer_ptr, dict_get(ctx, "buffer", 0)) }
   if(width_ptr){ store64(width_ptr, dict_get(ctx, "width", 0)) }
   if(height_ptr){ store64(height_ptr, dict_get(ctx, "height", 0)) }
   true
}

fn get_osmesa_depth_buffer(win, width_ptr, height_ptr, bytes_ptr, buffer_ptr){
   "Retrieves the depth buffer associated with the specified window."
   ;; Usually not maintained explicitly by the basic OSMesa shim but can be extended
   false
}

;; Cross-platform stubs for Win32
fn get_win32_window(win){ ui_backend.get_win32_window(win) }
fn get_win32_adapter(monitor){ ui_backend.get_win32_adapter(monitor) }
fn get_win32_monitor(monitor){ ui_backend.get_win32_monitor(monitor) }
fn get_wgl_context(win){ ui_backend.get_wgl_context(win) }

;; Cross-platform stubs for macOS
fn get_cocoa_window(win){ ui_backend.get_cocoa_window(win) }
fn get_cocoa_monitor(monitor){ ui_backend.get_cocoa_monitor(monitor) }
fn get_cocoa_view(win){ ui_backend.get_cocoa_view(win) }
fn get_nsgl_context(win){ ui_backend.get_nsgl_context(win) }
