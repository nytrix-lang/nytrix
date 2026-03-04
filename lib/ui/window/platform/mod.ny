;; Keywords: ui window platform
;; Unified windowing and input system abstraction for Nytrix.
;; Distributes window operations to platform-specific backends.
;; GLFW 3.5 compatible API.

module std.ui.window.platform (
   init, terminate, init_hint, init_allocator, init_vulkan_loader,
   get_version, get_version_string, get_platform,
   get_error, set_error_callback,
   get_proc_address, get_instance_proc_address,
   get_time, set_time, get_timer_value, get_timer_frequency,
   wait_events, wait_events_timeout,
   create_window, destroy_window, should_close, set_should_close,
   set_title, get_window_title, set_window_title,
   set_window_icon, get_pos, set_pos, get_window_pos, set_window_pos,
   get_size, get_window_size, get_framebuffer_size, set_size, set_window_size,
   get_window_attrib, set_window_attrib, set_window_opacity, post_empty_event,
   poll_events, pump_window_events, uses_native_events, supports_state_polling,
   swap_buffers, swap_interval,
   get_key, get_mouse_button, get_cursor_pos, get_key_name,
   set_cursor_pos, create_cursor, create_standard_cursor, destroy_cursor, set_cursor,
   set_key_callback, set_mouse_button_callback,
   set_scroll_callback, set_cursor_pos_callback,
   set_cursor_enter_callback, set_drop_callback,
   set_joystick_callback,
   set_window_size_callback, set_close_callback,
   set_char_callback,
   set_input_mode, get_input_mode, raw_mouse_motion_supported,
   get_key_scancode,
   show_window, hide_window, iconify_window, restore_window, maximize_window,
   vulkan_supported, required_extensions, create_surface, get_surface_capabilities,
   apply_hints,
   focus_window,
   get_backend_name,
   set_gamma, get_x11_display, get_x11_window,
   get_wayland_display, get_wayland_window, get_wayland_monitor,
   get_glx_context, get_glx_window,
   get_win32_window, get_win32_adapter, get_win32_monitor, get_wgl_context,
   get_cocoa_window, get_nsgl_context, get_cocoa_monitor, get_cocoa_view,
   get_egl_display, get_egl_context, get_egl_surface, get_egl_config,
   get_osmesa_context, get_osmesa_color_buffer, get_osmesa_depth_buffer,
   get_window_monitor, set_window_monitor,
   get_x11_selection_string, set_x11_selection_string,
   set_window_decorated, set_window_floating, set_window_resizable,
   get_clipboard, set_clipboard,
   get_surface_support,
   joystick_present, get_joystick_axes, get_joystick_buttons, get_joystick_hats, get_joystick_name, get_joystick_guid, joystick_is_gamepad, get_gamepad_name, get_gamepad_state, update_gamepad_mappings,
   get_joystick_user_pointer, set_joystick_user_pointer,

   get_gamma_ramp, set_gamma_ramp,
   get_monitors, get_primary_monitor,
   get_monitor_pos, get_monitor_workarea,
   get_monitor_physical_size, get_monitor_content_scale, get_monitor_name,
   get_x11_monitor, get_x11_adapter, get_wayland_monitor,
   get_monitor_user_pointer, set_monitor_user_pointer, set_monitor_callback,
   get_video_mode, get_video_modes,
   xdnd_begin_drag, _handle_xdnd_status, _handle_xdnd_finished,
   set_video_mode, restore_video_mode,

   ;; Window hints (GLFW compatible)
   default_window_hints, window_hint, window_hint_string,
   set_window_size_limits, set_window_aspect_ratio,
   get_window_frame_size, get_window_content_scale,
   get_window_opacity, request_window_attention,
   get_window_user_pointer, set_window_user_pointer,
   set_window_pos_callback, set_window_maximize_callback, set_window_content_scale_callback,
   set_window_iconify_callback, set_window_focus_callback, set_window_refresh_callback,
   set_framebuffer_size_callback,

   ;; Constants (Forwarded from API)
   ACTION_RELEASE, ACTION_PRESS, ACTION_REPEAT,
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
   KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
   KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24,
   KEY_F25,
   KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4,
   KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
   KEY_KP_DECIMAL, KEY_KP_DIVIDE, KEY_KP_MULTIPLY, KEY_KP_SUBTRACT,
   KEY_KP_ADD, KEY_KP_ENTER, KEY_KP_EQUAL,
   KEY_LEFT_SHIFT, KEY_LEFT_CONTROL, KEY_LEFT_ALT, KEY_LEFT_SUPER,
   KEY_RIGHT_SHIFT, KEY_RIGHT_CONTROL, KEY_RIGHT_ALT, KEY_RIGHT_SUPER,
   KEY_MENU,
   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_CAPS_LOCK, MOD_NUM_LOCK,
   MOUSE_BUTTON_1, MOUSE_BUTTON_2, MOUSE_BUTTON_3, MOUSE_BUTTON_4,
   MOUSE_BUTTON_5, MOUSE_BUTTON_6, MOUSE_BUTTON_7, MOUSE_BUTTON_8,
   MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,
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
   CURSOR, STICKY_KEYS, STICKY_MOUSE_BUTTONS, LOCK_KEY_MODS, RAW_MOUSE_MOTION,
   CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_DISABLED, CURSOR_CAPTURED,
   ARROW_CURSOR, IBEAM_CURSOR, CROSSHAIR_CURSOR, POINTING_HAND_CURSOR,
   RESIZE_EW_CURSOR, RESIZE_NS_CURSOR, RESIZE_NWSE_CURSOR, RESIZE_NESW_CURSOR,
   RESIZE_ALL_CURSOR, NOT_ALLOWED_CURSOR,
   CONNECTED, DISCONNECTED,
   CLIENT_API, NO_API, OPENGL_API, OPENGL_ES_API,
   RESIZABLE, VISIBLE, DECORATED, FOCUSED, AUTO_ICONIFY, FLOATING,
   MAXIMIZED, CENTER_CURSOR, TRANSPARENT_FRAMEBUFFER, FOCUS_ON_SHOW, HOVERED, MOUSE_PASSTHROUGH,
   DOUBLEBUFFER, SCALE_FRAMEBUFFER,
   SAMPLES, SRGB_CAPABLE, STEREO,
   RED_BITS, GREEN_BITS, BLUE_BITS, ALPHA_BITS, DEPTH_BITS, STENCIL_BITS,
   CONTEXT_VERSION_MAJOR, CONTEXT_VERSION_MINOR, CONTEXT_REVISION,
   CONTEXT_ROBUSTNESS, CONTEXT_DEBUG, CONTEXT_RELEASE_BEHAVIOR, CONTEXT_CREATION_API,
   CONTEXT_NO_ERROR, OPENGL_PROFILE, OPENGL_FORWARD_COMPAT,
   POSITION_X, POSITION_Y,
   ANY_RELEASE_BEHAVIOR, RELEASE_BEHAVIOR_FLUSH, RELEASE_BEHAVIOR_NONE,
   NO_ROBUSTNESS, NO_RESET_NOTIFICATION, LOSE_CONTEXT_ON_RESET,
   OPENGL_ANY_PROFILE, OPENGL_CORE_PROFILE, OPENGL_COMPAT_PROFILE,
   NATIVE_CONTEXT_API, EGL_CONTEXT_API, OSMESA_CONTEXT_API,
   X11_CLASS_NAME, X11_INSTANCE_NAME, WAYLAND_APP_ID,
   TRUE, FALSE
)

use std.core *
use std.core.mem *
use std.math as math
use std.str as str
use std.os.prim as prim
use std.util.common as common
use std.ui.event (event_type, event_window_id, event_data)
use std.ui.window.consts (EVENT_KEY_PRESSED, EVENT_KEY_RELEASED, EVENT_KEY_CHAR,
   EVENT_FLAGS_DATA_DROP_EVENTS, EVENT_FLAGS_MONITOR_EVENTS,
   EVENT_FLAGS_ALL,
   EVENT_KEY_CHAR_MODS, EVENT_FLAG_KEY_CHAR_MODS,
   EVENT_MOUSE_BUTTON_PRESSED, EVENT_MOUSE_BUTTON_RELEASED,
   EVENT_MOUSE_POS_CHANGED, EVENT_MOUSE_SCROLL,
   EVENT_MOUSE_ENTER, EVENT_MOUSE_LEAVE,
   EVENT_WINDOW_RESIZED, EVENT_WINDOW_MOVED, EVENT_WINDOW_REFRESH,
   EVENT_WINDOW_MAXIMIZED, EVENT_WINDOW_MINIMIZED, EVENT_WINDOW_RESTORED,
   EVENT_FOCUS_IN, EVENT_FOCUS_OUT, EVENT_SCALE_UPDATED,
   EVENT_DATA_DROP, EVENT_QUIT)

use std.ui.window.platform.api as api
use std.ui.window.platform.opengl as opengl_backend
use std.ui.window.platform.linux.joystick as linux_joystick
use std.ui.window.platform.win32.joystick as win32_joystick
use std.ui.window.platform.cocoa.joystick as cocoa_joystick
use std.ui.window.platform.win32 as win32_impl
use std.ui.window.platform.cocoa as cocoa_impl
use std.ui.window.platform.linux.x11 as x11_backend
use std.ui.window.platform.linux.wayland as wayland_backend
use std.str (cstr_to_str, str_find)
use std.ui.gfx.vk.vulkan as vk

mut _backend_name = ""
mut _wayland_display = 0
mut _wayland_globals = 0
mut _native_windows = dict(16)
mut _pending_native_events = []
mut _window_hints = dict()
mut _init_hints = dict()
mut _should_close_flags = dict(16)
mut _window_attribs = dict(16)
mut _window_contexts       = dict(16)
mut _current_win_context   = 0
mut _window_user_pointers  = dict(16)
mut _monitor_user_pointers = dict(8)
mut _error_callback        = 0
mut _last_error_code       = 0
mut _last_error_desc       = ""
mut _monitor_callback      = 0
mut _window_size_limits = dict(16)
mut _window_aspect_ratios = dict(16)
mut _window_callbacks = dict(16)
mut _joystick_user_pointers = dict(16)

fn _native_handle(win){
   if(is_dict(win)){ return dict_get(win, "handle", 0) }
   win
}

fn _native_window(win){
   if(is_dict(win) && dict_has(win, "handle")){
      if(dict_has(win, "display") || dict_has(win, "globals") || dict_has(win, "instance")){
         return win
      }
   }
   def handle = _native_handle(win)
   if(!handle){ return 0 }
   def direct = dict_get(_native_windows, handle, 0)
   if(direct){ return direct }
   def wins = dict_values(_native_windows)
   mut i = 0
   while(i < len(wins)){
      def cand = get(wins, i, 0)
      if(is_dict(cand) && dict_get(cand, "handle", 0) == handle){ return cand }
      i += 1
   }
   0
}

mut _debug = -1
mut _debug_init_done = false

fn _is_debug(){
   if(!_debug_init_done){
      _debug = common.cached_env_truthy(_debug, "NY_UI_DEBUG")
      _debug_init_done = true
      if(_debug){
         print("[window:platform] NY_UI_DEBUG enabled - verbose logging active")
      }
   }
   _debug
}

fn _dbg(msg){ if(_is_debug()){ print("[window:platform] " + msg) } }
fn _dbg_win(win, msg){ if(_is_debug()){ print("[window] win=0x" + to_hex(win) + " " + msg) } }
fn _dbg_hint(hint, val){ if(_is_debug()){ print("[window:hint] hint=0x" + to_hex(hint) + " val=" + to_str(val)) } }
fn _dbg_attrib(win, attrib, val){ if(_is_debug()){ print("[window:attrib] win=0x" + to_hex(win) + " attrib=0x" + to_hex(attrib) + " val=" + to_str(val)) } }
fn _dbg_err(msg){ if(_is_debug()){ print("[window:platform:ERROR] " + msg) } }
fn _dbg_warn(msg){ if(_is_debug()){ print("[window:platform:WARN] " + msg) } }
fn _dbg_v(msg){ if(_is_debug()){ print("[window:platform:v] " + msg) } }

fn _debug_print_allocation_failure(name){
   if(_is_debug()){
      print("[window:platform:ERROR] Memory allocation failed: " + name)
      print("[window:platform:ERROR]   (allocation failed)")
   }
}

fn _dump_window_state(win){
   if(!_is_debug()){ return }
   print("[window:state] === Window State Dump (0x" + to_hex(win) + ") ===")
   def hints_count = len(_window_hints)
   def attribs_count = len(_window_attribs)
   def user_ptr = get_window_user_pointer(win)
   print("[window:state]   hints: " + to_str(hints_count) + " entries")
   print("[window:state]   attribs: " + to_str(attribs_count) + " entries")
   print("[window:state]   user_ptr: 0x" + to_hex(user_ptr))

   def limits = dict_get(_window_size_limits, win, 0)
   if(limits){
      def min_w = dict_get(limits, "min_w", 0)
      def min_h = dict_get(limits, "min_h", 0)
      def max_w = dict_get(limits, "max_w", 0)
      def max_h = dict_get(limits, "max_h", 0)
      print("[window:state]   size_limits: min=" + to_str(min_w) + "x" + to_str(min_h) + " max=" + to_str(max_w) + "x" + to_str(max_h))
   } else {
      print("[window:state]   size_limits: none")
   }

   def ratio = dict_get(_window_aspect_ratios, win, 0)
   if(ratio){
      def numer = dict_get(ratio, "numer", 0)
      def denom = dict_get(ratio, "denom", 0)
      print("[window:state]   aspect_ratio: " + to_str(numer) + ":" + to_str(denom))
   } else {
      print("[window:state]   aspect_ratio: none")
   }

   def ctx = dict_get(_window_contexts, win, 0)
   if(ctx){
      def ctx_type = dict_get(ctx, "type", "unknown")
      print("[window:state]   context: " + ctx_type)
   } else {
      print("[window:state]   context: none")
   }

   print("[window:state] =========================================")
}

fn _dump_all_windows(){
   if(!_is_debug()){ return }
   print("[window:state] *** All Windows Dump ***")
   def handles = dict_keys(_native_windows)
   mut i = 0
   while(i < len(handles)){
      def h = get(handles, i)
      _dump_window_state(h)
      i += 1
   }
   print("[window:state] Total windows: " + to_str(len(handles)))
}

fn _dbgu(msg){ if(_is_debug()){ print("[window:platform:v] " + msg) } }

;; Default window hints (GLFW compatible)
fn _default_window_hints(){
   mut hints = dict()
   hints = dict_set(hints, api.RESIZABLE, api.TRUE)
   hints = dict_set(hints, api.VISIBLE, api.TRUE)
   hints = dict_set(hints, api.DECORATED, api.TRUE)
   hints = dict_set(hints, api.FOCUSED, api.TRUE)
   hints = dict_set(hints, api.AUTO_ICONIFY, api.TRUE)
   hints = dict_set(hints, api.FLOATING, api.FALSE)
   hints = dict_set(hints, api.MAXIMIZED, api.FALSE)
   hints = dict_set(hints, api.CENTER_CURSOR, api.TRUE)
   hints = dict_set(hints, api.TRANSPARENT_FRAMEBUFFER, api.FALSE)
   hints = dict_set(hints, api.FOCUS_ON_SHOW, api.TRUE)
   hints = dict_set(hints, api.MOUSE_PASSTHROUGH, api.FALSE)
   hints = dict_set(hints, api.DOUBLEBUFFER, api.TRUE)
   hints = dict_set(hints, api.SCALE_FRAMEBUFFER, api.TRUE)
   hints = dict_set(hints, api.RED_BITS, 8)
   hints = dict_set(hints, api.GREEN_BITS, 8)
   hints = dict_set(hints, api.BLUE_BITS, 8)
   hints = dict_set(hints, api.ALPHA_BITS, 8)
   hints = dict_set(hints, api.DEPTH_BITS, 24)
   hints = dict_set(hints, api.STENCIL_BITS, 8)
   hints = dict_set(hints, api.STEREO, api.FALSE)
   hints = dict_set(hints, api.SAMPLES, 0)
   hints = dict_set(hints, api.SRGB_CAPABLE, api.FALSE)
   hints = dict_set(hints, api.CLIENT_API, api.OPENGL_API)
   hints = dict_set(hints, api.CONTEXT_VERSION_MAJOR, 1)
   hints = dict_set(hints, api.CONTEXT_VERSION_MINOR, 0)
   hints = dict_set(hints, api.CONTEXT_DEBUG, api.FALSE)
   hints = dict_set(hints, api.CONTEXT_NO_ERROR, api.FALSE)
   hints = dict_set(hints, api.OPENGL_PROFILE, api.OPENGL_ANY_PROFILE)
   hints = dict_set(hints, api.OPENGL_FORWARD_COMPAT, api.FALSE)
   hints = dict_set(hints, api.CONTEXT_RELEASE_BEHAVIOR, api.ANY_RELEASE_BEHAVIOR)
   hints = dict_set(hints, api.CONTEXT_CREATION_API, api.NATIVE_CONTEXT_API)
   hints
}

; Foward Constants
def ACTION_RELEASE = api.ACTION_RELEASE
def ACTION_PRESS = api.ACTION_PRESS
def ACTION_REPEAT = api.ACTION_REPEAT
def KEY_SPACE = api.KEY_SPACE
def KEY_APOSTROPHE = api.KEY_APOSTROPHE
def KEY_COMMA = api.KEY_COMMA
def KEY_MINUS = api.KEY_MINUS
def KEY_PERIOD = api.KEY_PERIOD
def KEY_SLASH = api.KEY_SLASH
def KEY_0 = api.KEY_0
def KEY_1 = api.KEY_1
def KEY_2 = api.KEY_2
def KEY_3 = api.KEY_3
def KEY_4 = api.KEY_4
def KEY_5 = api.KEY_5
def KEY_6 = api.KEY_6
def KEY_7 = api.KEY_7
def KEY_8 = api.KEY_8
def KEY_9 = api.KEY_9
def KEY_SEMICOLON = api.KEY_SEMICOLON
def KEY_EQUAL = api.KEY_EQUAL
def KEY_A = api.KEY_A
def KEY_B = api.KEY_B
def KEY_C = api.KEY_C
def KEY_D = api.KEY_D
def KEY_E = api.KEY_E
def KEY_F = api.KEY_F
def KEY_G = api.KEY_G
def KEY_H = api.KEY_H
def KEY_I = api.KEY_I
def KEY_J = api.KEY_J
def KEY_K = api.KEY_K
def KEY_L = api.KEY_L
def KEY_M = api.KEY_M
def KEY_N = api.KEY_N
def KEY_O = api.KEY_O
def KEY_P = api.KEY_P
def KEY_Q = api.KEY_Q
def KEY_R = api.KEY_R
def KEY_S = api.KEY_S
def KEY_T = api.KEY_T
def KEY_U = api.KEY_U
def KEY_V = api.KEY_V
def KEY_W = api.KEY_W
def KEY_X = api.KEY_X
def KEY_Y = api.KEY_Y
def KEY_Z = api.KEY_Z
def KEY_LEFT_BRACKET = api.KEY_LEFT_BRACKET
def KEY_BACKSLASH = api.KEY_BACKSLASH
def KEY_RIGHT_BRACKET = api.KEY_RIGHT_BRACKET
def KEY_GRAVE_ACCENT = api.KEY_GRAVE_ACCENT
def KEY_WORLD_1 = api.KEY_WORLD_1
def KEY_WORLD_2 = api.KEY_WORLD_2
def KEY_ESCAPE = api.KEY_ESCAPE
def KEY_ENTER = api.KEY_ENTER
def KEY_TAB = api.KEY_TAB
def KEY_BACKSPACE = api.KEY_BACKSPACE
def KEY_INSERT = api.KEY_INSERT
def KEY_DELETE = api.KEY_DELETE
def KEY_RIGHT = api.KEY_RIGHT
def KEY_LEFT = api.KEY_LEFT
def KEY_DOWN = api.KEY_DOWN
def KEY_UP = api.KEY_UP
def KEY_PAGE_UP = api.KEY_PAGE_UP
def KEY_PAGE_DOWN = api.KEY_PAGE_DOWN
def KEY_HOME = api.KEY_HOME
def KEY_END = api.KEY_END
def KEY_CAPS_LOCK = api.KEY_CAPS_LOCK
def KEY_SCROLL_LOCK = api.KEY_SCROLL_LOCK
def KEY_NUM_LOCK = api.KEY_NUM_LOCK
def KEY_PRINT_SCREEN = api.KEY_PRINT_SCREEN
def KEY_PAUSE = api.KEY_PAUSE
def KEY_F1 = api.KEY_F1
def KEY_F2 = api.KEY_F2
def KEY_F3 = api.KEY_F3
def KEY_F4 = api.KEY_F4
def KEY_F5 = api.KEY_F5
def KEY_F6 = api.KEY_F6
def KEY_F7 = api.KEY_F7
def KEY_F8 = api.KEY_F8
def KEY_F9 = api.KEY_F9
def KEY_F10 = api.KEY_F10
def KEY_F11 = api.KEY_F11
def KEY_F12 = api.KEY_F12
def KEY_F13 = api.KEY_F13
def KEY_F14 = api.KEY_F14
def KEY_F15 = api.KEY_F15
def KEY_F16 = api.KEY_F16
def KEY_F17 = api.KEY_F17
def KEY_F18 = api.KEY_F18
def KEY_F19 = api.KEY_F19
def KEY_F20 = api.KEY_F20
def KEY_F21 = api.KEY_F21
def KEY_F22 = api.KEY_F22
def KEY_F23 = api.KEY_F23
def KEY_F24 = api.KEY_F24
def KEY_F25 = api.KEY_F25
def KEY_KP_0 = api.KEY_KP_0
def KEY_KP_1 = api.KEY_KP_1
def KEY_KP_2 = api.KEY_KP_2
def KEY_KP_3 = api.KEY_KP_3
def KEY_KP_4 = api.KEY_KP_4
def KEY_KP_5 = api.KEY_KP_5
def KEY_KP_6 = api.KEY_KP_6
def KEY_KP_7 = api.KEY_KP_7
def KEY_KP_8 = api.KEY_KP_8
def KEY_KP_9 = api.KEY_KP_9
def KEY_KP_DECIMAL = api.KEY_KP_DECIMAL
def KEY_KP_DIVIDE = api.KEY_KP_DIVIDE
def KEY_KP_MULTIPLY = api.KEY_KP_MULTIPLY
def KEY_KP_SUBTRACT = api.KEY_KP_SUBTRACT
def KEY_KP_ADD = api.KEY_KP_ADD
def KEY_KP_ENTER = api.KEY_KP_ENTER
def KEY_KP_EQUAL = api.KEY_KP_EQUAL
def KEY_LEFT_SHIFT = api.KEY_LEFT_SHIFT
def KEY_LEFT_CONTROL = api.KEY_LEFT_CONTROL
def KEY_LEFT_ALT = api.KEY_LEFT_ALT
def KEY_LEFT_SUPER = api.KEY_LEFT_SUPER
def KEY_RIGHT_SHIFT = api.KEY_RIGHT_SHIFT
def KEY_RIGHT_CONTROL = api.KEY_RIGHT_CONTROL
def KEY_RIGHT_ALT = api.KEY_RIGHT_ALT
def KEY_RIGHT_SUPER = api.KEY_RIGHT_SUPER
def KEY_MENU = api.KEY_MENU
def MOD_SHIFT = api.MOD_SHIFT
def MOD_CONTROL = api.MOD_CONTROL
def MOD_ALT = api.MOD_ALT
def MOD_SUPER = api.MOD_SUPER
def MOD_CAPS_LOCK = api.MOD_CAPS_LOCK
def MOD_NUM_LOCK = api.MOD_NUM_LOCK
def MOUSE_BUTTON_1 = api.MOUSE_BUTTON_1
def MOUSE_BUTTON_2 = api.MOUSE_BUTTON_2
def MOUSE_BUTTON_3 = api.MOUSE_BUTTON_3
def MOUSE_BUTTON_4 = api.MOUSE_BUTTON_4
def MOUSE_BUTTON_5 = api.MOUSE_BUTTON_5
def MOUSE_BUTTON_6 = api.MOUSE_BUTTON_6
def MOUSE_BUTTON_7 = api.MOUSE_BUTTON_7
def MOUSE_BUTTON_8 = api.MOUSE_BUTTON_8
def MOUSE_BUTTON_LEFT = api.MOUSE_BUTTON_LEFT
def MOUSE_BUTTON_RIGHT = api.MOUSE_BUTTON_RIGHT
def MOUSE_BUTTON_MIDDLE = api.MOUSE_BUTTON_MIDDLE
def JOYSTICK_1 = api.JOYSTICK_1
def JOYSTICK_2 = api.JOYSTICK_2
def JOYSTICK_3 = api.JOYSTICK_3
def JOYSTICK_4 = api.JOYSTICK_4
def JOYSTICK_5 = api.JOYSTICK_5
def JOYSTICK_6 = api.JOYSTICK_6
def JOYSTICK_7 = api.JOYSTICK_7
def JOYSTICK_8 = api.JOYSTICK_8
def JOYSTICK_9 = api.JOYSTICK_9
def JOYSTICK_10 = api.JOYSTICK_10
def JOYSTICK_11 = api.JOYSTICK_11
def JOYSTICK_12 = api.JOYSTICK_12
def JOYSTICK_13 = api.JOYSTICK_13
def JOYSTICK_14 = api.JOYSTICK_14
def JOYSTICK_15 = api.JOYSTICK_15
def JOYSTICK_16 = api.JOYSTICK_16
def JOYSTICK_LAST = api.JOYSTICK_LAST
def GAMEPAD_BUTTON_A = api.GAMEPAD_BUTTON_A
def GAMEPAD_BUTTON_B = api.GAMEPAD_BUTTON_B
def GAMEPAD_BUTTON_X = api.GAMEPAD_BUTTON_X
def GAMEPAD_BUTTON_Y = api.GAMEPAD_BUTTON_Y
def GAMEPAD_BUTTON_LEFT_BUMPER = api.GAMEPAD_BUTTON_LEFT_BUMPER
def GAMEPAD_BUTTON_RIGHT_BUMPER = api.GAMEPAD_BUTTON_RIGHT_BUMPER
def GAMEPAD_BUTTON_BACK = api.GAMEPAD_BUTTON_BACK
def GAMEPAD_BUTTON_START = api.GAMEPAD_BUTTON_START
def GAMEPAD_BUTTON_GUIDE = api.GAMEPAD_BUTTON_GUIDE
def GAMEPAD_BUTTON_LEFT_THUMB = api.GAMEPAD_BUTTON_LEFT_THUMB
def GAMEPAD_BUTTON_RIGHT_THUMB = api.GAMEPAD_BUTTON_RIGHT_THUMB
def GAMEPAD_BUTTON_DPAD_UP = api.GAMEPAD_BUTTON_DPAD_UP
def GAMEPAD_BUTTON_DPAD_RIGHT = api.GAMEPAD_BUTTON_DPAD_RIGHT
def GAMEPAD_BUTTON_DPAD_DOWN = api.GAMEPAD_BUTTON_DPAD_DOWN
def GAMEPAD_BUTTON_DPAD_LEFT = api.GAMEPAD_BUTTON_DPAD_LEFT
def GAMEPAD_BUTTON_LAST = api.GAMEPAD_BUTTON_LAST
def GAMEPAD_BUTTON_CROSS = api.GAMEPAD_BUTTON_CROSS
def GAMEPAD_BUTTON_CIRCLE = api.GAMEPAD_BUTTON_CIRCLE
def GAMEPAD_BUTTON_SQUARE = api.GAMEPAD_BUTTON_SQUARE
def GAMEPAD_BUTTON_TRIANGLE = api.GAMEPAD_BUTTON_TRIANGLE
def GAMEPAD_AXIS_LEFT_X = api.GAMEPAD_AXIS_LEFT_X
def GAMEPAD_AXIS_LEFT_Y = api.GAMEPAD_AXIS_LEFT_Y
def GAMEPAD_AXIS_RIGHT_X = api.GAMEPAD_AXIS_RIGHT_X
def GAMEPAD_AXIS_RIGHT_Y = api.GAMEPAD_AXIS_RIGHT_Y
def GAMEPAD_AXIS_LEFT_TRIGGER = api.GAMEPAD_AXIS_LEFT_TRIGGER
def GAMEPAD_AXIS_RIGHT_TRIGGER = api.GAMEPAD_AXIS_RIGHT_TRIGGER
def GAMEPAD_AXIS_LAST = api.GAMEPAD_AXIS_LAST
def CURSOR = api.CURSOR
def STICKY_KEYS = api.STICKY_KEYS
def STICKY_MOUSE_BUTTONS = api.STICKY_MOUSE_BUTTONS
def LOCK_KEY_MODS = api.LOCK_KEY_MODS
def RAW_MOUSE_MOTION = api.RAW_MOUSE_MOTION
def CURSOR_NORMAL = api.CURSOR_NORMAL
def CURSOR_HIDDEN = api.CURSOR_HIDDEN
def CURSOR_DISABLED = api.CURSOR_DISABLED
def CURSOR_CAPTURED = api.CURSOR_CAPTURED
def ARROW_CURSOR = api.ARROW_CURSOR
def IBEAM_CURSOR = api.IBEAM_CURSOR
def CROSSHAIR_CURSOR = api.CROSSHAIR_CURSOR
def POINTING_HAND_CURSOR = api.POINTING_HAND_CURSOR
def RESIZE_EW_CURSOR = api.RESIZE_EW_CURSOR
def RESIZE_NS_CURSOR = api.RESIZE_NS_CURSOR
def RESIZE_NWSE_CURSOR = api.RESIZE_NWSE_CURSOR
def RESIZE_NESW_CURSOR = api.RESIZE_NESW_CURSOR
def RESIZE_ALL_CURSOR = api.RESIZE_ALL_CURSOR
def NOT_ALLOWED_CURSOR = api.NOT_ALLOWED_CURSOR
def CONNECTED = api.CONNECTED
def DISCONNECTED = api.DISCONNECTED
def CLIENT_API = api.CLIENT_API
def NO_API = api.NO_API
def OPENGL_API = api.OPENGL_API
def OPENGL_ES_API = api.OPENGL_ES_API
def RESIZABLE = api.RESIZABLE
def VISIBLE = api.VISIBLE
def DECORATED = api.DECORATED
def FOCUSED = api.FOCUSED
def AUTO_ICONIFY = api.AUTO_ICONIFY
def FLOATING = api.FLOATING
def MAXIMIZED = api.MAXIMIZED
def CENTER_CURSOR = api.CENTER_CURSOR
def TRANSPARENT_FRAMEBUFFER = api.TRANSPARENT_FRAMEBUFFER
def FOCUS_ON_SHOW = api.FOCUS_ON_SHOW
def SAMPLES = api.SAMPLES

fn _select_backend_name(){
   if(_backend_name != ""){ return _backend_name }
   def requested = str.lower(to_str(prim.env("NY_UI_BACKEND")))
   if(requested == "x11" && x11_backend.available()){ _backend_name = "x11" }
   elif(requested == "wayland" && wayland_backend.available()){ _backend_name = "wayland" }
   else {
      if(wayland_backend.available()){ _backend_name = "wayland" }
      elif(x11_backend.available()){ _backend_name = "x11" }
      else { _backend_name = "none" }
   }
   _dbg("selected backend: " + _backend_name + " (requested=" + requested + ")")
   _backend_name
}

fn get_backend_name(){ _select_backend_name() }
fn uses_native_events(){ true }
fn get_error(){
   def code = _last_error_code
   _last_error_code = 0
   _last_error_desc = ""
   code
}
fn _set_error(code, desc){
   _last_error_code = code
   _last_error_desc = desc
   if(_is_debug()){
      print("[window:platform:ERROR] code=0x" + to_hex(code) + " desc='" + desc + "'")
   }
   if(_error_callback){
      _dbg_v("_set_error: firing callback with code=0x" + to_hex(code))
      call(_error_callback, code, desc)
   }
}
fn set_error_callback(cb){ def old = _error_callback _error_callback = cb old }
fn supports_state_polling(){ true }

;; Version/platform info (GLFW compatible)
fn get_version(){ [3, 5, 0] }
fn get_version_string(){
   def b = _select_backend_name()
   if(b == "x11"){ "3.5.0 X11 EGL OSMesa dynamically-linked" }
   elif(b == "wayland"){ "3.5.0 Wayland EGL OSMesa dynamically-linked" }
   else { "3.5.0 Null OSMesa dynamically-linked" }
}
fn get_platform(){
   def b = _select_backend_name()
   if(b == "x11"){ 0x60004 }
   elif(b == "wayland"){ 0x60003 }
   else { 0x60001 }
}
fn init_hint(hint, value){
   if(hint == 0x00050001){ ;; GLFW_PLATFORM
      if(value == 0x60004){ _backend_name = "x11" }
      elif(value == 0x60003){ _backend_name = "wayland" }
   }
   _init_hints = dict_set(_init_hints, hint, value)
}

fn init_allocator(alloc){ false }
fn init_vulkan_loader(path){
   "Overrides the Vulkan shared library path used during init. Must be called before init()."
   if(is_str(path) && str_len(path) > 0){
      backend_api.set_vulkan_loader_path(path)
      1
   } else { 0 }
}

;; High-resolution timer (GLFW compatible, nanosecond resolution)
mut _timer_offset = -1
fn _timer_raw_ns(){
   def ts = malloc(16)
   def r = __clock_gettime(1, ts)
   if(r != 0){ free(ts) return 0 }
   def s = from_int(load64(ts, 0))
   def ns = from_int(load64(ts, 8))
   def raw = s * 1000000000 + ns
   free(ts)
   raw
}
fn _timer_base(){
   if(_timer_offset == -1){ _timer_offset = _timer_raw_ns() }
   _timer_offset
}
fn get_timer_value(){ def base = _timer_base() _timer_raw_ns() - base }
fn get_timer_frequency(){ 1000000000 }
fn get_time(){ get_timer_value() * 1.0 / 1000000000.0 }
fn set_time(t){
   _timer_offset = _timer_raw_ns() - to_int(t * 1000000000.0)
}

;; Wait-event variants
fn wait_events(){ poll_events() }
fn wait_events_timeout(t){ poll_events() }

fn init(){
   _dbg("init")
   def b = _select_backend_name()
   if(b == "x11"){ return true }
   if(b == "wayland"){
      _wayland_display = wayland_backend.connect_display()
      if(!_wayland_display){ return false }
      _wayland_globals = wayland_backend.bootstrap_globals(_wayland_display)
      wayland_backend.set_globals(_wayland_globals)
      return _wayland_globals != 0
   }
   true
}

fn terminate(){
   _dbg("terminate")
   def b = _select_backend_name()
   if(b == "wayland"){ wayland_backend.destroy_globals(_wayland_globals) }
}

fn create_window(name, x, y, w, h, flags){
   _dbg("create_window: name='" + name + "' pos=" + to_str(x) + "," + to_str(y) + " size=" + to_str(w) + "x" + to_str(h) + " flags=0x" + to_hex(flags))
   def b = _select_backend_name()
   _dbg("  backend=" + b)
   def x11_class = dict_get(_window_hints, X11_CLASS_NAME, "Nytrix")
   def x11_inst = dict_get(_window_hints, X11_INSTANCE_NAME, "nytrix")
   def wl_app_id = dict_get(_window_hints, WAYLAND_APP_ID, "nytrix")

   mut native = 0
   if(b == "x11"){ native = x11_backend.create_basic_window(name, w, h, x, y, flags, 0, x11_class, x11_inst) }
   elif(b == "wayland"){ native = wayland_backend.create_basic_window(_wayland_globals, name, w, h, wl_app_id) }
   if(native){
      def handle = _native_handle(native)
      if(!handle){
         _dbg_err("create_window: returned native state without a handle")
         _dump_window_state(0)
         return false
      }
      _native_windows = dict_set(_native_windows, handle, native)
      _dbg_win(handle, "created successfully")

      def client_api = dict_get(_window_hints, CLIENT_API, OPENGL_API)
      if(client_api != NO_API){
         def ctx = opengl_backend.create_context(native, _window_hints)
         if(ctx){
         _window_contexts = dict_set(_window_contexts, handle, ctx)
         _dbg_v("create_window: OpenGL context created: " + dict_get(ctx, "type", "unknown"))
         } else {
         _dbg_warn("create_window: OpenGL context creation failed (may be expected for NO_API)")
         }
      }

      _dump_window_state(handle)
      return handle
   } else {
      _dbg_err("create_window: backend returned false!")
      _dbg_err("  backend=" + b)
      _dbg_err("  name='" + name + "' size=" + to_str(w) + "x" + to_str(h))
      _dump_all_windows()
   }
   false
}

fn destroy_window(win){
   def handle = _native_handle(win)
   def native = _native_window(win)
   _dbg_win(handle, "destroy_window called")
   _dump_window_state(handle)

   if(!handle){
      _dbg_err("destroy_window: called with invalid window (no handle)")
      return
   }

   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.destroy_basic_window(native) }
   elif(b == "wayland"){ wayland_backend.destroy_basic_window(native) }

   _native_windows = dict_del(_native_windows, handle)
   _should_close_flags = dict_del(_should_close_flags, handle)
   _window_attribs = dict_del(_window_attribs, handle)

   def ctx = dict_get(_window_contexts, handle, 0)
   if(ctx){
      opengl_backend.destroy_offscreen_context(ctx)
      _window_contexts = dict_del(_window_contexts, handle)
      _dbg_v("destroy_window: OpenGL context destroyed")
   }

   if(_current_win_context == handle){
      _current_win_context = 0
      _dbg_v("destroy_window: cleared current context")
   }

   _window_user_pointers = dict_del(_window_user_pointers, handle)
   _window_size_limits = dict_del(_window_size_limits, handle)
   _window_aspect_ratios = dict_del(_window_aspect_ratios, handle)
   _window_callbacks = dict_del(_window_callbacks, handle)
   _win_callbacks = dict_del(_win_callbacks, handle)

   _dbg_win(handle, "destroyed - remaining windows: " + to_str(len(dict_keys(_native_windows))))
}

fn should_close(win){ dict_get(_should_close_flags, win, false) }
fn set_should_close(win, value){
   _dbg("set_should_close: win=0x" + to_hex(win) + " value=" + to_str(value))
   _should_close_flags = dict_set(_should_close_flags, win, value)
}

;; Window hints system (GLFW compatible)
fn default_window_hints(){
   "Resets all window hints to their default values."
   _dbg("default_window_hints: resetting to defaults")
   _window_hints = _default_window_hints()
}

fn window_hint(hint, value){
   "Sets a window hint to a specific value."
   _dbg_hint(hint, value)
   _window_hints = dict_set(_window_hints, hint, value)
}

fn window_hint_string(hint, value){
   "Sets a string window hint (for class names, app IDs, etc.)."
   _dbg("window_hint_string: hint=0x" + to_hex(hint) + " value='" + value + "'")
   _window_hints = dict_set(_window_hints, hint, value)
}

fn get_window_attrib(win, attrib){
   "Gets a window attribute."
   def handle = _native_handle(win)
   def attribs = dict_get(_window_attribs, handle, dict())
   def val = dict_get(attribs, attrib, 0)
   if(val != 0){
      _dbg_attrib(handle, attrib, val)
      return val
   }
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      def res = x11_backend.get_window_attrib(native, attrib)
      _dbg_attrib(handle, attrib, res)
      return res
   } elif(b == "wayland"){
      def res = wayland_backend.get_window_attrib(native, attrib)
      _dbg_attrib(handle, attrib, res)
      return res
   }
   def hint_val = dict_get(_window_hints, attrib, 0)
   _dbg_attrib(handle, attrib, hint_val)
   hint_val
}

fn set_window_attrib(win, attrib, value){
   "Sets a window attribute."
   def handle = _native_handle(win)
   _dbg("set_window_attrib: win=0x" + to_hex(handle) + " attrib=0x" + to_hex(attrib) + " value=" + to_str(value))
   mut attribs = dict_get(_window_attribs, handle, dict())
   attribs = dict_set(attribs, attrib, value)
   _window_attribs = dict_set(_window_attribs, handle, attribs)

   ;; Apply attribute changes immediately where possible
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      if(attrib == api.DECORATED){ x11_backend.set_window_decorated(native, value != 0) }
      if(attrib == api.RESIZABLE){ x11_backend.set_window_resizable(native, value != 0) }
      if(attrib == api.FLOATING){ x11_backend.set_window_floating(native, value != 0) }
      if(attrib == api.MOUSE_PASSTHROUGH){ x11_backend.set_window_mouse_passthrough(native, value != 0) }
   } elif(b == "wayland"){
      ;; Wayland attributes are applied on next window update
   }
}

fn set_window_size_limits(win, min_w, min_h, max_w, max_h){
   "Sets the size limits for a window."
   if(!win){
      _dbg_err("set_window_size_limits: called with null window")
      return
   }

   if(min_w < 0 || min_h < 0){
      _dbg_warn("set_window_size_limits: negative min values (min_w=" + to_str(min_w) + ", min_h=" + to_str(min_h) + ")")
   }
   if(max_w < 0 || max_h < 0 && max_w != -1 && max_h != -1){
      _dbg_warn("set_window_size_limits: negative max values (max_w=" + to_str(max_w) + ", max_h=" + to_str(max_h) + ")")
   }
   if(max_w != -1 && min_w != -1 && max_w < min_w){
      _dbg_warn("set_window_size_limits: max_w (" + to_str(max_w) + ") < min_w (" + to_str(min_w) + ")")
   }
   if(max_h != -1 && min_h != -1 && max_h < min_h){
      _dbg_warn("set_window_size_limits: max_h (" + to_str(max_h) + ") < min_h (" + to_str(min_h) + ")")
   }

   mut limits = dict()
   limits = dict_set(limits, "min_w", min_w)
   limits = dict_set(limits, "min_h", min_h)
   limits = dict_set(limits, "max_w", max_w)
   limits = dict_set(limits, "max_h", max_h)
   _window_size_limits = dict_set(_window_size_limits, win, limits)

   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      def result = x11_backend.set_window_size_limits(native, min_w, min_h, max_w, max_h)
      if(!result){
         _dbg_err("set_window_size_limits: x11_backend returned false")
      }
      _dbg_v("set_window_size_limits: X11 limits set min=" + to_str(min_w) + "x" + to_str(min_h) + " max=" + to_str(max_w) + "x" + to_str(max_h))
   } elif(b == "wayland"){
      def result = wayland_backend.set_window_size_limits(native, min_w, min_h, max_w, max_h)
      if(!result){
         _dbg_err("set_window_size_limits: wayland_backend returned false")
      }
      _dbg_v("set_window_size_limits: Wayland limits set")
   } else {
      _dbg_warn("set_window_size_limits: unknown backend '" + b + "'")
   }
}

fn set_window_aspect_ratio(win, numer, denom){
   "Sets the aspect ratio for a window."
   mut ratio = dict()
   ratio = dict_set(ratio, "numer", numer)
   ratio = dict_set(ratio, "denom", denom)
   _window_aspect_ratios = dict_set(_window_aspect_ratios, win, ratio)

   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      ;; X11: Set WM_NORMAL_HINTS with aspect ratio
      x11_backend.set_window_aspect_ratio(native, numer, denom)
   }
}

fn get_window_frame_size(win){
   "Returns the window frame size [left, top, right, bottom]."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_window_frame_size(native) }
   elif(b == "wayland"){ wayland_backend.get_window_frame_size(native) }
   else { [0, 0, 0, 0] }
}

fn get_window_content_scale(win){
   "Returns the content scale [xscale, yscale]."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_window_content_scale(native) }
   elif(b == "wayland"){ wayland_backend.get_window_content_scale(native) }
   else { [1.0, 1.0] }
}

fn get_window_opacity(win){
   "Returns the window opacity (0.0 to 1.0)."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_window_opacity(native) }
   elif(b == "wayland"){ wayland_backend.get_window_opacity(native) }
   else { 1.0 }
}

fn request_window_attention(win){
   "Requests user attention for the window."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.request_window_attention(native) }
   elif(b == "wayland"){ wayland_backend.request_window_attention(native) }
}

fn get_window_user_pointer(win){
   "Returns the user pointer for a window."
   dict_get(_window_user_pointers, win, 0)
}

fn set_window_user_pointer(win, ptr){
   "Sets the user pointer for a window."
   _window_user_pointers = dict_set(_window_user_pointers, win, ptr)
}

fn set_window_pos_callback(win, cb){ _set_win_cb(win, "pos", cb) }
fn set_window_maximize_callback(win, cb){ _set_win_cb(win, "maximize", cb) }
fn set_window_content_scale_callback(win, cb){ _set_win_cb(win, "scale", cb) }
fn set_window_iconify_callback(win, cb){ _set_win_cb(win, "iconify", cb) }
fn set_window_focus_callback(win, cb){ _set_win_cb(win, "focus", cb) }
fn set_window_refresh_callback(win, cb){ _set_win_cb(win, "refresh", cb) }
fn set_framebuffer_size_callback(win, cb){ _set_win_cb(win, "fbsize", cb) }

fn poll_events(){
   def b = _select_backend_name()
   def windows = dict_values(_native_windows)
   _pending_native_events = []

   if(_is_debug()){
      _dbg_v("poll_events: backend=" + b + " windows=" + to_str(len(windows)))
   }

   mut i = 0
   while(i < len(windows)){
      def win = get(windows, i)
      if(!is_dict(win)){
         _dbg_warn("poll_events: window at index " + to_str(i) + " is not a dict")
         i += 1
         continue
      }

      def handle = dict_get(win, "handle", 0)
      if(!handle){
         _dbg_warn("poll_events: window at index " + to_str(i) + " has no handle")
         i += 1
         continue
      }

      if(b == "x11"){
         def result = x11_backend.poll_window_events(win)
         def updated = get(result, 0)
         def evs = get(result, 1, [])
         def upd_handle = is_dict(updated) ? dict_get(updated, "handle", 0) : 0
         if(upd_handle){
         _native_windows = dict_set(_native_windows, upd_handle, updated)
         } else {
         _dbg_warn("poll_events: x11_backend returned updated window without handle")
         }
         if(is_list(evs) && len(evs) > 0){
         _pending_native_events = extend(_pending_native_events, evs)
         if(_is_debug()){
               _dbg_v("poll_events: received " + to_str(len(evs)) + " events from X11")
         }
         ;; Dispatch GLFW-style callbacks for each new event
         mut ei = 0
         while(ei < len(evs)){
               _dispatch_event_callbacks(get(evs, ei))
               ei += 1
         }
         }
      } elif(b == "wayland"){
         wayland_backend.poll_window_events(win)
      } else {
         _dbg_warn("poll_events: unknown backend '" + b + "'")
      }
      i += 1
   }

   if(_is_debug() && len(_pending_native_events) > 0){
      _dbg_v("poll_events: total pending events: " + to_str(len(_pending_native_events)))
   }
}

fn pump_window_events(win){
   def evs = _pending_native_events
   _pending_native_events = []
   evs
}

fn swap_buffers(win){
   def b = _select_backend_name()
   _dbg("swap_buffers: backend=" + b + " win=0x" + to_hex(_native_handle(win)))
   def ctx = dict_get(_window_contexts, _native_handle(win), 0)
   if(ctx){ opengl_backend.swap_buffers(ctx) }
   if(b == "x11"){ x11_backend.flush(get_x11_display()) }
}

fn swap_interval(interval){
   _dbg("swap_interval: interval=" + to_str(interval))
   opengl_backend.swap_interval(interval)
}

fn get_proc_address(name){ opengl_backend.get_proc_address(name) }
fn get_instance_proc_address(instance, name){
   def b = _select_backend_name()
   if(b == "x11" || b == "wayland"){
      return vk.vk_get_instance_proc_addr(instance, name)
   }
   0
}

fn make_context_current(win){
   if(!win){
      opengl_backend.release_context_current()
      _current_win_context = 0
      return true
   }
   def ctx = dict_get(_window_contexts, win, 0)
   if(!ctx){ return false }
   if(opengl_backend.make_context_current(ctx)){
      _current_win_context = win
      return true
   }
   false
}

fn get_current_context(){ _current_win_context }

if(comptime{ __os_name() == "linux" }){
   #link "libGL.so"
   #include <GL/gl.h>
}
if(comptime{ __os_name() == "windows" }){
   #link "opengl32.lib"
   #include <GL/gl.h>
}
if(comptime{ __os_name() == "macos" }){
   #link "-framework OpenGL"
   #include <OpenGL/gl.h>
}

fn extension_supported(name){
   "Checks if the specified OpenGL extension is supported."
   def win = _current_win_context
   if(!win){ return false }
   def exts_ptr = glGetString(0x1F03) ;; GL_EXTENSIONS
   if(!exts_ptr){ return false }
   def exts = cstr_to_str(exts_ptr)
   str_find(exts, name) != -1
}

fn swap_interval(interval){
   opengl_backend.swap_interval(interval)
}

fn vulkan_supported(){
   def b = _select_backend_name()
   mut ok = false
   if(b == "x11"){ ok = x11_backend.vulkan_supported() }
   elif(b == "wayland"){ ok = wayland_backend.vulkan_supported() }
   _dbg("vulkan_supported: backend=" + b + " supported=" + to_str(ok))
   ok
}

fn required_extensions(){
   def b = _select_backend_name()
   mut exts = []
   if(b == "x11"){ exts = x11_backend.vulkan_required_extensions() }
   elif(b == "wayland"){ exts = wayland_backend.vulkan_required_extensions() }
   _dbg("required_extensions: backend=" + b + " values=" + to_str(exts))
   exts
}

fn create_surface(instance, win, allocator, surface_ptr){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(_is_debug()){
      def raw_handle = _native_handle(win)
      def native_handle = _native_handle(native)
      _dbg("create_surface: backend=" + b + " raw=0x" + to_hex(raw_handle) + " native=0x" + to_hex(native_handle) +
         " raw_is_dict=" + to_str(is_dict(win)) + " native_is_dict=" + to_str(is_dict(native)))
   }
   mut res = 1
   if(b == "x11"){ res = x11_backend.create_surface(instance, native, allocator, surface_ptr) }
   elif(b == "wayland"){ res = wayland_backend.create_surface(instance, native, allocator, surface_ptr) }
   _dbg("create_surface: backend=" + b + " result=" + to_str(res))
   res
}

fn get_key(win, key){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_key_state(native, key) }
   elif(b == "wayland"){ wayland_backend.get_key_state(native, key) }
   else { 0 }
}

fn get_mouse_button(win, button){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_mouse_button_state(native, button) }
   elif(b == "wayland"){ wayland_backend.get_mouse_button_state(native, button) }
   else { 0 }
}

fn get_cursor_pos(win){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_cursor_pos(native) }
   elif(b == "wayland"){ wayland_backend.get_cursor_pos(native) }
   else { [0, 0] }
}

fn set_cursor_pos(win, x, y){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.set_cursor_pos(native, x, y) }
   elif(b == "wayland"){ wayland_backend.set_cursor_pos(native, x, y) }
}

fn set_input_mode(win, mode, value){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.set_input_mode(native, mode, value) }
   elif(b == "wayland"){ wayland_backend.set_input_mode(native, mode, value) }
}

fn raw_mouse_motion_supported(){
   def b = _select_backend_name()
   if(b == "x11"){ return true } ;; Assume XInput2 is checked by backend internally
   if(b == "wayland"){ return true } ;; Relative pointer protocol
   false
}

fn get_input_mode(win, mode){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_input_mode(native, mode) }
   elif(b == "wayland"){ wayland_backend.get_input_mode(native, mode) }
   else { 0 }
}

fn set_title(win, title){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.set_title(native, title) }
   elif(b == "wayland"){ wayland_backend.set_title(native, title) }
}

fn set_window_title(win, title){ set_title(win, title) }

fn get_window_title(win){
   "Returns the window title."
   def native = _native_window(win)
   dict_get(native, "title", "Untitled")
}

fn get_size(win){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_size(native) }
   elif(b == "wayland"){ wayland_backend.get_size(native) }
   else { [0, 0] }
}

fn get_window_size(win){ get_size(win) }
fn get_framebuffer_size(win){ get_size(win) }

fn set_size(win, w, h){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.set_size(native, w, h) }
   elif(b == "wayland"){ wayland_backend.set_size(native, w, h) }
}

fn set_window_size(win, w, h){ set_size(win, w, h) }

fn focus_window(win){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.focus_window(native) }
}

fn apply_hints(hints){
   _dbg("apply_hints: count=" + to_str(len(hints)) + " values=" + to_str(hints))
   _window_hints = dict_merge(_window_hints, hints)
}

fn post_empty_event(win=0){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.post_empty_event(native ? native : win) }
   elif(b == "wayland"){ wayland_backend.flush(0) }
}

fn get_key_name(key, scancode){
   def b = _select_backend_name()
   def windows = dict_values(_native_windows)
   if(len(windows) == 0){ return "" }
   def win = get(windows, 0)
   if(b == "x11"){ x11_backend.get_key_name(win, key, scancode) }
   elif(b == "wayland"){ wayland_backend.get_key_name(win, key, scancode) }
   else { "" }
}

fn get_key_scancode(key){
   def b = _select_backend_name()
   def windows = dict_values(_native_windows)
   if(len(windows) == 0){ return -1 }
   def win = get(windows, 0)
   if(b == "x11"){ x11_backend.get_key_scancode(win, key) }
   else { -1 }
}

fn get_monitors(){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_monitors() }
   elif(b == "wayland"){ wayland_backend.get_monitors() }
   else { [] }
}

fn get_primary_monitor(){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_primary_monitor() }
   elif(b == "wayland"){ wayland_backend.get_primary_monitor() }
   else { 0 }
}

fn get_monitor_pos(monitor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_monitor_pos(monitor) }
   elif(b == "wayland"){ wayland_backend.get_monitor_pos(monitor) }
   else { [0, 0] }
}

fn get_monitor_workarea(monitor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_monitor_workarea(monitor) }
   elif(b == "wayland"){ wayland_backend.get_monitor_workarea(monitor) }
   else { [0, 0, 0, 0] }
}

fn get_monitor_physical_size(monitor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_monitor_physical_size(monitor) }
   elif(b == "wayland"){ wayland_backend.get_monitor_physical_size(monitor) }
   else { [0, 0] }
}

fn get_monitor_content_scale(monitor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_monitor_content_scale(monitor) }
   elif(b == "wayland"){ wayland_backend.get_monitor_content_scale(monitor) }
   else { [1.0, 1.0] }
}

fn get_monitor_name(monitor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_monitor_name(monitor) }
   elif(b == "wayland"){ wayland_backend.get_monitor_name(monitor) }
   else { "unknown" }
}

fn get_x11_monitor(mon){ x11_backend.get_x11_monitor(mon) }
fn get_x11_adapter(mon){ x11_backend.get_x11_adapter(mon) }
fn get_wayland_monitor(mon){ wayland_backend.get_wayland_monitor(mon) }

fn get_monitor_user_pointer(monitor){
   def h = dict_get(monitor, "handle", 0)
   if(!h){ return 0 }
   dict_get(_monitor_user_pointers, h, 0)
}

fn set_monitor_user_pointer(monitor, ptr){
   def h = dict_get(monitor, "handle", 0)
   if(!h){ return 0 }
   def old = dict_get(_monitor_user_pointers, h, 0)
   _monitor_user_pointers = dict_set(_monitor_user_pointers, h, ptr)
   old
}

fn set_monitor_callback(cb){
   def old = _monitor_callback
   _monitor_callback = cb
   old
}

fn get_video_mode(monitor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_video_mode(monitor) }
   elif(b == "wayland"){ x11_backend.get_video_mode(monitor) }
   else { 0 }
}

fn get_video_modes(monitor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_video_modes(monitor) }
   elif(b == "wayland"){ wayland_backend.get_video_modes(monitor) }
   else { [] }
}

fn create_cursor(image, xhot, yhot){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.create_cursor(image, xhot, yhot) }
   elif(b == "wayland"){ wayland_backend.create_cursor(image, xhot, yhot) }
   else { 0 }
}

fn create_standard_cursor(shape){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.create_standard_cursor(shape) }
   elif(b == "wayland"){ wayland_backend.create_standard_cursor(shape) }
   else { 0 }
}

fn destroy_cursor(cursor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.destroy_cursor(cursor) }
   elif(b == "wayland"){ wayland_backend.destroy_cursor(cursor) }
}

fn set_cursor(win, cursor){
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.set_cursor(native, cursor) }
   elif(b == "wayland"){ wayland_backend.set_cursor(native, cursor) }
}

fn get_gamma_ramp(monitor){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.get_gamma_ramp(monitor) }
   elif(b == "wayland"){ wayland_backend.get_gamma_ramp(monitor) }
}

fn set_gamma_ramp(monitor, ramp){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.set_gamma_ramp(monitor, ramp) }
   elif(b == "wayland"){ wayland_backend.set_gamma_ramp(monitor, ramp) }
}

fn set_gamma(mon, gamma){
   ;; Build a 256-entry gamma ramp from the exponent, then apply it
   def size = 256
   def ramp = list()
   mut i = 0
   while(i < size){
      def value = int(math.pow(i * 1.0 / (size - 1), 1.0 / gamma) * 65535.0 + 0.5)
      append(ramp, [value, value, value])
      i += 1
   }
   set_gamma_ramp(mon, ramp)
}
mut _x11_display = 0
fn get_x11_display(){ if(_x11_display == 0){ _x11_display = x11_backend.open_display() } _x11_display }
fn get_x11_window(win){ _native_handle(_native_window(win)) }
fn get_x11_selection_string(win){
   def b = _select_backend_name()
   if(b == "x11"){ return x11_backend.get_primary_selection(_native_window(win)) }
   ""
}
fn set_x11_selection_string(win, s){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.set_primary_selection(_native_window(win), s) }
}
fn set_window_decorated(win, v){ def b = _select_backend_name() if(b == "x11"){ x11_backend.set_window_decorated(win, v) } }
fn set_window_floating(win, v){ def b = _select_backend_name() if(b == "x11"){ x11_backend.set_window_floating(win, v) } }
fn set_window_resizable(win, v){ def b = _select_backend_name() if(b == "x11"){ x11_backend.set_window_resizable(win, v) } }
fn get_clipboard(win){ def b = _select_backend_name() if(b == "x11"){ x11_backend.get_clipboard(win) } else { "" } }
fn set_clipboard(win, s){ def b = _select_backend_name() if(b == "x11"){ x11_backend.set_clipboard(win, s) } }
fn get_pos(win){ def b = _select_backend_name() def native = _native_window(win) if(b == "x11"){ x11_backend.get_pos(native) } else { [0, 0] } }
fn get_window_pos(win){ get_pos(win) }
fn set_pos(win, x, y){ def b = _select_backend_name() def native = _native_window(win) if(b == "x11"){ x11_backend.set_pos(native, x, y) } }
fn set_window_pos(win, x, y){ set_pos(win, x, y) }
fn get_window_monitor(win){ def b = _select_backend_name() def native = _native_window(win) if(b == "x11"){ x11_backend.get_window_monitor(native) } else { 0 } }
fn set_window_monitor(win, mon, x, y, w, h, ref){ def b = _select_backend_name() def native = _native_window(win) if(b == "x11"){ x11_backend.set_window_monitor(native, mon, x, y, w, h, ref) } }

;; Per-window callback registry: dict(handle -> dict(name -> fn))
mut _win_callbacks = dict(16)
mut _joystick_cb = 0

fn _get_win_cbs(handle){
   dict_get(_win_callbacks, handle, dict())
}
fn _set_win_cb(win, name, cb){
   def handle = _native_handle(win)
   if(!handle){ return }
   mut cbs = dict_get(_win_callbacks, handle, dict())
   cbs = dict_set(cbs, name, cb)
   _win_callbacks = dict_set(_win_callbacks, handle, cbs)
}

fn set_key_callback(win, cb){ _set_win_cb(win, "key", cb) }
fn set_mouse_button_callback(win, cb){ _set_win_cb(win, "mouse_button", cb) }
fn set_scroll_callback(win, cb){ _set_win_cb(win, "scroll", cb) }
fn set_cursor_pos_callback(win, cb){ _set_win_cb(win, "cursor_pos", cb) }
fn set_cursor_enter_callback(win, cb){ _set_win_cb(win, "cursor_enter", cb) }
fn set_drop_callback(win, cb){ _set_win_cb(win, "drop", cb) }
fn set_joystick_callback(cb){ _joystick_cb = cb }
fn set_window_size_callback(win, cb){ _set_win_cb(win, "window_size", cb) }
fn set_close_callback(win, cb){ _set_win_cb(win, "close", cb) }
fn set_char_callback(win, cb){ _set_win_cb(win, "char", cb) }
fn set_char_mods_callback(win, cb){ _set_win_cb(win, "char_mods", cb) }

fn _dispatch_event_callbacks(ev){
   "Dispatch a single event to any registered GLFW-style callbacks."
   def typ = event_type(ev)
   def win_handle = event_window_id(ev)
   if(!win_handle){ return }
   def cbs = _get_win_cbs(win_handle)
   if(!is_dict(cbs)){ return }
   def data = event_data(ev)
   if(typ == EVENT_KEY_PRESSED){
      def cb = dict_get(cbs, "key", 0)
      if(cb){
         def key = dict_get(data, "key", 0)
         def sc = dict_get(data, "scancode", 0)
         def action = dict_get(data, "action", 0)
         def mods = dict_get(data, "mod", 0)
         cb(win_handle, key, sc, action, mods)
      }
      def ccb = dict_get(cbs, "char", 0)
      if(ccb && is_dict(data)){
         def cp = dict_get(data, "char", -1)
         if(cp >= 0){ ccb(win_handle, cp) }
      }
   } elif(typ == EVENT_KEY_RELEASED){
      def cb = dict_get(cbs, "key", 0)
      if(cb){
         def key = dict_get(data, "key", 0)
         def sc = dict_get(data, "scancode", 0)
         def mods = dict_get(data, "mod", 0)
         cb(win_handle, key, sc, api.ACTION_RELEASE, mods)
      }
   } elif(typ == EVENT_KEY_CHAR){
      def cb = dict_get(cbs, "char", 0)
      if(cb && is_dict(data)){
         def cp = dict_get(data, "char", -1)
         if(cp >= 0){ cb(win_handle, cp) }
      }
   } elif(typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED){
      def cb = dict_get(cbs, "mouse_button", 0)
      if(cb && is_dict(data)){
         def btn = dict_get(data, "button", 0)
         def action = (typ == EVENT_MOUSE_BUTTON_PRESSED) ? api.ACTION_PRESS : api.ACTION_RELEASE
         def mods = dict_get(data, "mod", 0)
         cb(win_handle, btn, action, mods)
      }
   } elif(typ == EVENT_MOUSE_POS_CHANGED){
      def cb = dict_get(cbs, "cursor_pos", 0)
      if(cb && is_dict(data)){
         def x = dict_get(data, "x", 0)
         def y = dict_get(data, "y", 0)
         cb(win_handle, x, y)
      }
   } elif(typ == EVENT_MOUSE_SCROLL){
      def cb = dict_get(cbs, "scroll", 0)
      if(cb && is_dict(data)){
         def sx = dict_get(data, "x", 0)
         def sy = dict_get(data, "y", 0)
         cb(win_handle, sx, sy)
      }
   } elif(typ == EVENT_WINDOW_RESIZED){
      def cb = dict_get(cbs, "window_size", 0)
      def fcb = dict_get(cbs, "fbsize", 0)
      if(is_dict(data)){
         def w = dict_get(data, "w", 0)
         def h = dict_get(data, "h", 0)
         if(cb){ cb(win_handle, w, h) }
         if(fcb){ fcb(win_handle, w, h) }
      }
   } elif(typ == EVENT_QUIT){
      def cb = dict_get(cbs, "close", 0)
      if(cb){ cb(win_handle) }
   } elif(typ == EVENT_MOUSE_ENTER || typ == EVENT_MOUSE_LEAVE){
      def cb = dict_get(cbs, "cursor_enter", 0)
      if(cb){ cb(win_handle, (typ == EVENT_MOUSE_ENTER) ? 1 : 0) }
   } elif(typ == EVENT_DATA_DROP){
      def cb = dict_get(cbs, "drop", 0)
      if(cb && is_dict(data)){
         def paths = dict_get(data, "paths", [])
         cb(win_handle, paths)
      }
   } elif(typ == EVENT_WINDOW_MOVED){
      def cb = dict_get(cbs, "pos", 0)
      if(cb && is_dict(data)){
         def x = dict_get(data, "x", 0)
         def y = dict_get(data, "y", 0)
         cb(win_handle, x, y)
      }
   } elif(typ == EVENT_WINDOW_MAXIMIZED){
      def cb = dict_get(cbs, "maximize", 0)
      if(cb){ cb(win_handle, 1) }
   } elif(typ == EVENT_WINDOW_MINIMIZED){
      def cb = dict_get(cbs, "iconify", 0)
      if(cb){ cb(win_handle, 1) }
   } elif(typ == EVENT_WINDOW_RESTORED){
      def icb = dict_get(cbs, "iconify", 0)
      if(icb){ icb(win_handle, 0) }
      def mcb = dict_get(cbs, "maximize", 0)
      if(mcb){ mcb(win_handle, 0) }
   } elif(typ == EVENT_FOCUS_IN || typ == EVENT_FOCUS_OUT){
      def cb = dict_get(cbs, "focus", 0)
      if(cb){ cb(win_handle, (typ == EVENT_FOCUS_IN) ? 1 : 0) }
   } elif(typ == EVENT_WINDOW_REFRESH){
      def cb = dict_get(cbs, "refresh", 0)
      if(cb){ cb(win_handle) }
   } elif(typ == EVENT_SCALE_UPDATED){
      def cb = dict_get(cbs, "scale", 0)
      if(cb && is_dict(data)){
         def sx = dict_get(data, "x", 1.0)
         def sy = dict_get(data, "y", 1.0)
         cb(win_handle, sx, sy)
      }
   } elif(typ == EVENT_MONITOR_CONNECTED || typ == EVENT_MONITOR_DISCONNECTED){
      if(_monitor_callback){
         def monitor = data ; Assume data is monitor dict or ID
         def event = (typ == EVENT_MONITOR_CONNECTED) ? 0x00040001 : 0x00040002 ; GLFW_CONNECTED/DISCONNECTED
         _monitor_callback(monitor, event)
      }
   }
}

fn set_window_opacity(win, op){
   def b = _select_backend_name()
   def native = _native_window(win)
   _dbg("set_window_opacity: backend=" + b + " win=0x" + to_hex(_native_handle(win)) + " opacity=" + to_str(op))
   if(b == "x11"){ x11_backend.set_window_opacity(native, op) }
   elif(b == "wayland"){ wayland_backend.set_window_opacity(native, op) }
}

fn show_window(win){
   def b = _select_backend_name()
   def native = _native_window(win)
   _dbg("show_window: backend=" + b + " win=0x" + to_hex(_native_handle(win)))
   if(b == "x11"){ x11_backend.show_window(native) }
   elif(b == "wayland"){ wayland_backend.show_window(native) }
}

fn hide_window(win){
   def b = _select_backend_name()
   def native = _native_window(win)
   _dbg("hide_window: backend=" + b + " win=0x" + to_hex(_native_handle(win)))
   if(b == "x11"){ x11_backend.hide_window(native) }
   elif(b == "wayland"){ wayland_backend.hide_window(native) }
}

fn iconify_window(win){
   def b = _select_backend_name()
   def native = _native_window(win)
   _dbg("iconify_window: backend=" + b + " win=0x" + to_hex(_native_handle(win)))
   if(b == "x11"){ x11_backend.iconify_window(native) }
   elif(b == "wayland"){ wayland_backend.iconify_window(native) }
}

fn restore_window(win){
   def b = _select_backend_name()
   def native = _native_window(win)
   _dbg("restore_window: backend=" + b + " win=0x" + to_hex(_native_handle(win)))
   if(b == "x11"){ x11_backend.restore_window(native) }
   elif(b == "wayland"){ wayland_backend.restore_window(native) }
}

fn maximize_window(win){
   def b = _select_backend_name()
   def native = _native_window(win)
   _dbg("maximize_window: backend=" + b + " win=0x" + to_hex(_native_handle(win)))
   if(b == "x11"){ x11_backend.maximize_window(native) }
   elif(b == "wayland"){ wayland_backend.maximize_window(native) }
}

fn vulkan_get_surface_capabilities(phys, surf, caps){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.vulkan_get_surface_capabilities(phys, surf, caps) }
   else { 1 }
}

fn set_window_icon(win, images){
   def b = _select_backend_name()
   if(b == "x11"){ x11_backend.set_window_icon(win, images) }
   elif(b == "wayland"){ wayland_backend.set_window_icon(win, images) }
}

fn get_surface_capabilities(phys, surf, caps){
   vulkan_get_surface_capabilities(phys, surf, caps)
}

fn xdnd_begin_drag(win, data){ }
fn _handle_xdnd_status(win, data){ }
fn _handle_xdnd_finished(win, data){ }
fn set_video_mode(mon, mode){ }
fn restore_video_mode(mon){ }

fn get_surface_support(phys, family, surf, ptr){ def b = _select_backend_name() if(b == "x11"){ x11_backend.vulkan_get_surface_support(phys, family, surf, ptr) } else { 1 } }

fn joystick_present(jid){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.joystick_present(jid) }
   if(os == "windows"){ return win32_joystick.joystick_present(jid) }
   if(os == "macos"){ return cocoa_joystick.joystick_present(jid) }
   false
}
fn get_joystick_axes(jid, count_ptr){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.get_joystick_axes(jid, count_ptr) }
   if(os == "windows"){ return win32_joystick.get_joystick_axes(jid, count_ptr) }
   if(os == "macos"){ return cocoa_joystick.get_joystick_axes(jid, count_ptr) }
   []
}
fn get_joystick_buttons(jid, count_ptr){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.get_joystick_buttons(jid, count_ptr) }
   if(os == "windows"){ return win32_joystick.get_joystick_buttons(jid, count_ptr) }
   if(os == "macos"){ return cocoa_joystick.get_joystick_buttons(jid, count_ptr) }
   []
}
fn get_joystick_hats(jid, count_ptr){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.get_joystick_hats(jid, count_ptr) }
   if(count_ptr){ store32(count_ptr, 0, 0) }
   0
}
fn get_joystick_name(jid){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.get_joystick_name(jid) }
   if(os == "windows"){ return win32_joystick.get_joystick_name(jid) }
   if(os == "macos"){ return cocoa_joystick.get_joystick_name(jid) }
   ""
}
fn get_joystick_guid(jid){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.get_joystick_guid(jid) }
   if(os == "windows"){ return win32_joystick.get_joystick_guid(jid) }
   if(os == "macos"){ return cocoa_joystick.get_joystick_guid(jid) }
   ""
}
fn joystick_is_gamepad(jid){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.joystick_is_gamepad(jid) }
   if(os == "windows"){ return win32_joystick.joystick_is_gamepad(jid) }
   if(os == "macos"){ return cocoa_joystick.joystick_is_gamepad(jid) }
   false
}
fn get_gamepad_name(jid){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.get_gamepad_name(jid) }
   if(os == "windows"){ return win32_joystick.get_gamepad_name(jid) }
   if(os == "macos"){ return cocoa_joystick.get_gamepad_name(jid) }
   ""
}
fn get_gamepad_state(jid, state){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.get_gamepad_state(jid, state) }
   if(os == "windows"){ return win32_joystick.get_gamepad_state(jid, state) }
   if(os == "macos"){ return cocoa_joystick.get_gamepad_state(jid, state) }
   false
}
fn update_gamepad_mappings(m){
   def os = __os_name()
   if(os == "linux"){ return linux_joystick.update_gamepad_mappings(m) }
   if(os == "windows"){ return win32_joystick.update_gamepad_mappings(m) }
   if(os == "macos"){ return cocoa_joystick.update_gamepad_mappings(m) }
   false
}

fn get_wayland_display(){ _wayland_display }
fn get_wayland_window(win){
   def native = _native_window(win)
   if(!is_dict(native)){ return 0 }
   dict_get(native, "surface", 0)
}

fn get_glx_context(win){
   def ctx = dict_get(_window_contexts, _native_handle(win), 0)
   if(!ctx){ return 0 }
   if(dict_get(ctx, "type", "") == "glx_offscreen"){ return dict_get(ctx, "context", 0) }
   0
}

fn get_glx_window(win){ _native_handle(win) }

fn get_egl_display(){
   def ctx = get_current_context()
   if(!ctx){ return 0 }
   def c = dict_get(_window_contexts, ctx, 0)
   dict_get(c, "display", 0)
}

fn get_egl_context(win){
   def ctx = dict_get(_window_contexts, _native_handle(win), 0)
   if(!ctx){ return 0 }
   if(dict_get(ctx, "type", "") == "egl_offscreen"){ return dict_get(ctx, "context", 0) }
   0
}

fn get_egl_surface(win){
   def ctx = dict_get(_window_contexts, _native_handle(win), 0)
   if(!ctx){ return 0 }
   dict_get(ctx, "surface", 0)
}

fn get_egl_config(win){
   def ctx = dict_get(_window_contexts, _native_handle(win), 0)
   if(!ctx){ return 0 }
   dict_get(ctx, "config", 0)
}

fn get_osmesa_context(win){
   def ctx = dict_get(_window_contexts, _native_handle(win), 0)
   if(!ctx){ return 0 }
   if(dict_get(ctx, "type", "") == "osmesa"){ return dict_get(ctx, "context", 0) }
   0
}

fn get_osmesa_color_buffer(win){
   def ctx = dict_get(_window_contexts, _native_handle(win), 0)
   if(!ctx){ return 0 }
   dict_get(ctx, "buffer", 0)
}

fn get_osmesa_depth_buffer(win){
   def ctx = dict_get(_window_contexts, _native_handle(win), 0)
   if(!ctx){ return 0 }
   0 ; Placeholder: OSMesa usually shared depth
}

fn get_joystick_user_pointer(jid){ dict_get(_joystick_user_pointers, jid, 0) }
fn set_joystick_user_pointer(jid, ptr){ _joystick_user_pointers = dict_set(_joystick_user_pointers, jid, ptr) }

;; Win32 / Cocoa Native Handle Accessors
fn get_win32_window(win){
   if(_select_backend_name() != "win32"){ return 0 }
   def native = _native_window(win)
   is_dict(native) ? dict_get(native, "handle", 0) : native
}
fn get_win32_adapter(mon){
   if(_select_backend_name() != "win32"){ return 0 }
   win32_impl.get_win32_adapter(mon)
}
fn get_win32_monitor(mon){
   if(_select_backend_name() != "win32"){ return 0 }
   is_dict(mon) ? dict_get(mon, "handle", 0) : 0
}
fn get_wgl_context(win){ 0 }
fn get_cocoa_window(win){
   if(_select_backend_name() != "cocoa"){ return 0 }
   def native = _native_window(win)
   is_dict(native) ? dict_get(native, "handle", 0) : native
}
fn get_nsgl_context(win){ 0 }
fn get_cocoa_monitor(mon){
   if(_select_backend_name() != "cocoa"){ return 0 }
   is_dict(mon) ? dict_get(mon, "handle", 0) : 0
}
fn get_cocoa_view(win){
   if(_select_backend_name() != "cocoa"){ return 0 }
   def native = _native_window(win)
   cocoa_impl.get_cocoa_view(native)
}
