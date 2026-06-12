;; Keywords: platform window backend api contract gamepad joystick controller map os ui input
;; Backend-neutral window platform API.
;; References:
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform(
   init, terminate, init_hint, init_allocator, init_vulkan_loader, get_version, get_version_string,
   get_platform, get_error, set_error_callback, get_proc_address, get_instance_proc_address, make_context_current, get_time,
   set_time, get_timer_value, get_timer_frequency, wait_events, wait_events_timeout, create_window,
   destroy_window, should_close, set_should_close, set_title, get_window_title, set_window_title,
   set_window_icon, get_pos, set_pos, get_window_pos, set_window_pos, get_size, get_window_size,
   get_framebuffer_size, set_size, set_window_size, get_window_attrib, set_window_attrib, set_window_opacity,
   post_empty_event, poll_events, poll_events_for_window, pump_window_events, uses_native_events,
   supports_state_polling, swap_buffers, swap_interval, get_key, get_mouse_button, get_cursor_pos,
   cursor_visible, get_key_name, set_cursor_pos, create_cursor, create_standard_cursor, destroy_cursor, set_cursor,
   set_key_callback, set_mouse_button_callback, set_scroll_callback, set_cursor_pos_callback,
   set_cursor_enter_callback, set_drop_callback, set_joystick_callback, set_window_size_callback,
   set_close_callback, set_char_callback, set_input_mode, get_input_mode, raw_mouse_motion_supported,
   get_key_scancode, show_window, hide_window, iconify_window, restore_window, maximize_window,
   vulkan_supported, required_extensions, create_surface, get_surface_capabilities, apply_hints, focus_window,
   get_backend_name, set_gamma, get_x11_display, get_x11_window, get_wayland_display, get_wayland_window,
   get_wayland_monitor, get_glx_context, get_glx_window, get_win32_window, get_win32_adapter,
   get_win32_monitor, get_wgl_context, get_cocoa_window, get_nsgl_context, get_cocoa_monitor, get_cocoa_view,
   get_egl_display, get_egl_context, get_egl_surface, get_egl_config, get_osmesa_context,
   get_osmesa_color_buffer, get_osmesa_depth_buffer, get_window_monitor, set_window_monitor,
   get_x11_selection_string, set_x11_selection_string, set_window_decorated, set_window_floating,
   set_window_resizable, get_clipboard, set_clipboard, get_surface_support, joystick_present,
   get_joystick_axes, get_joystick_buttons, get_joystick_hats, get_joystick_name, get_joystick_guid,
   joystick_is_gamepad, get_gamepad_name, get_gamepad_state, update_gamepad_mappings,
   get_joystick_user_pointer, set_joystick_user_pointer, get_gamma_ramp, set_gamma_ramp, get_monitors,
   get_primary_monitor, get_monitor_pos, get_monitor_workarea, get_monitor_physical_size,
   get_monitor_content_scale, get_monitor_name, get_x11_monitor, get_x11_adapter, get_monitor_user_pointer,
   set_monitor_user_pointer, set_monitor_callback, get_video_mode, get_video_modes, xdnd_begin_drag,
   _handle_xdnd_status, _handle_xdnd_finished, set_video_mode, restore_video_mode, default_window_hints,
   window_hint, window_hint_string, set_window_size_limits, set_window_aspect_ratio, get_window_frame_size,
   get_window_content_scale, get_window_opacity, request_window_attention, get_window_user_pointer,
   set_window_user_pointer, set_window_pos_callback, set_window_maximize_callback,
   set_window_content_scale_callback, set_window_iconify_callback, set_window_focus_callback,
   set_window_refresh_callback, set_framebuffer_size_callback, _get_platform_val, _set_platform_val,
   ACTION_RELEASE, ACTION_PRESS, ACTION_REPEAT, NO_ERROR, NOT_INITIALIZED, NO_CURRENT_CONTEXT, INVALID_ENUM,
   INVALID_VALUE, OUT_OF_MEMORY, API_UNAVAILABLE, VERSION_UNAVAILABLE, PLATFORM_ERROR,
   KEY_SPACE, KEY_APOSTROPHE, KEY_COMMA, KEY_MINUS, KEY_PERIOD,
   KEY_SLASH, KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_SEMICOLON, KEY_EQUAL,
   KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
   KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z, KEY_LEFT_BRACKET,
   KEY_BACKSLASH, KEY_RIGHT_BRACKET, KEY_GRAVE_ACCENT, KEY_WORLD_1, KEY_WORLD_2, KEY_ESCAPE, KEY_ENTER,
   KEY_TAB, KEY_BACKSPACE, KEY_INSERT, KEY_DELETE, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP, KEY_PAGE_UP,
   KEY_PAGE_DOWN, KEY_HOME, KEY_END, KEY_CAPS_LOCK, KEY_SCROLL_LOCK, KEY_NUM_LOCK, KEY_PRINT_SCREEN, KEY_PAUSE,
   KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_F13,
   KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_F25,
   KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
   KEY_KP_DECIMAL, KEY_KP_DIVIDE, KEY_KP_MULTIPLY, KEY_KP_SUBTRACT, KEY_KP_ADD, KEY_KP_ENTER, KEY_KP_EQUAL,
   KEY_LEFT_SHIFT, KEY_LEFT_CONTROL, KEY_LEFT_ALT, KEY_LEFT_SUPER, KEY_RIGHT_SHIFT, KEY_RIGHT_CONTROL,
   KEY_RIGHT_ALT, KEY_RIGHT_SUPER, KEY_MENU, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_CAPS_LOCK,
   MOD_NUM_LOCK, MOUSE_BUTTON_1, MOUSE_BUTTON_2, MOUSE_BUTTON_3, MOUSE_BUTTON_4, MOUSE_BUTTON_5,
   MOUSE_BUTTON_6, MOUSE_BUTTON_7, MOUSE_BUTTON_8, MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,
   JOYSTICK_1, JOYSTICK_2, JOYSTICK_3, JOYSTICK_4, JOYSTICK_5, JOYSTICK_6, JOYSTICK_7, JOYSTICK_8, JOYSTICK_9,
   JOYSTICK_10, JOYSTICK_11, JOYSTICK_12, JOYSTICK_13, JOYSTICK_14, JOYSTICK_15, JOYSTICK_16, JOYSTICK_LAST,
   GAMEPAD_BUTTON_A, GAMEPAD_BUTTON_B, GAMEPAD_BUTTON_X, GAMEPAD_BUTTON_Y, GAMEPAD_BUTTON_LEFT_BUMPER,
   GAMEPAD_BUTTON_RIGHT_BUMPER, GAMEPAD_BUTTON_BACK, GAMEPAD_BUTTON_START, GAMEPAD_BUTTON_GUIDE,
   GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_DPAD_UP, GAMEPAD_BUTTON_DPAD_RIGHT,
   GAMEPAD_BUTTON_DPAD_DOWN, GAMEPAD_BUTTON_DPAD_LEFT, GAMEPAD_BUTTON_LAST, GAMEPAD_BUTTON_CROSS,
   GAMEPAD_BUTTON_CIRCLE, GAMEPAD_BUTTON_SQUARE, GAMEPAD_BUTTON_TRIANGLE, GAMEPAD_AXIS_LEFT_X,
   GAMEPAD_AXIS_LEFT_Y, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y, GAMEPAD_AXIS_LEFT_TRIGGER,
   GAMEPAD_AXIS_RIGHT_TRIGGER, GAMEPAD_AXIS_LAST, CURSOR, STICKY_KEYS, STICKY_MOUSE_BUTTONS, LOCK_KEY_MODS,
   RAW_MOUSE_MOTION, CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_DISABLED, CURSOR_CAPTURED, ARROW_CURSOR,
   IBEAM_CURSOR, CROSSHAIR_CURSOR, POINTING_HAND_CURSOR, RESIZE_EW_CURSOR, RESIZE_NS_CURSOR,
   RESIZE_NWSE_CURSOR, RESIZE_NESW_CURSOR, RESIZE_ALL_CURSOR, NOT_ALLOWED_CURSOR, CONNECTED, DISCONNECTED,
   CLIENT_API, NO_API, OPENGL_API, OPENGL_ES_API, RESIZABLE, VISIBLE, DECORATED, FOCUSED, ICONIFIED,
   AUTO_ICONIFY, FLOATING, MAXIMIZED, CENTER_CURSOR, TRANSPARENT_FRAMEBUFFER, FOCUS_ON_SHOW, HOVERED,
   MOUSE_PASSTHROUGH, DOUBLEBUFFER, SCALE_FRAMEBUFFER, SAMPLES, SRGB_CAPABLE, STEREO, RED_BITS, GREEN_BITS,
   BLUE_BITS, ALPHA_BITS, DEPTH_BITS, STENCIL_BITS, ACCUM_RED_BITS, ACCUM_GREEN_BITS, ACCUM_BLUE_BITS,
   ACCUM_ALPHA_BITS, AUX_BUFFERS, REFRESH_RATE, CONTEXT_VERSION_MAJOR, CONTEXT_VERSION_MINOR, CONTEXT_REVISION,
   CONTEXT_ROBUSTNESS, CONTEXT_DEBUG, CONTEXT_RELEASE_BEHAVIOR, CONTEXT_CREATION_API, CONTEXT_NO_ERROR,
   OPENGL_PROFILE, OPENGL_FORWARD_COMPAT, POSITION_X, POSITION_Y, ANY_RELEASE_BEHAVIOR, RELEASE_BEHAVIOR_FLUSH,
   RELEASE_BEHAVIOR_NONE, NO_ROBUSTNESS, NO_RESET_NOTIFICATION, LOSE_CONTEXT_ON_RESET, OPENGL_ANY_PROFILE,
   OPENGL_CORE_PROFILE, OPENGL_COMPAT_PROFILE, NATIVE_CONTEXT_API, EGL_CONTEXT_API, OSMESA_CONTEXT_API,
   X11_CLASS_NAME, X11_INSTANCE_NAME, JOYSTICK_HAT_BUTTONS, WAYLAND_LIBDECOR, X11_XCB_VULKAN_SURFACE,
   COCOA_RETINA_FRAMEBUFFER, COCOA_FRAME_NAME, COCOA_GRAPHICS_SWITCHING, WIN32_KEYBOARD_MENU,
   WIN32_SHOWDEFAULT, WAYLAND_APP_ID, FORMAT_UNAVAILABLE, NO_WINDOW_CONTEXT, DONT_CARE, PLATFORM,
   PLATFORM_WIN32, PLATFORM_COCOA, PLATFORM_X11, PLATFORM_WAYLAND, PLATFORM_NULL, PLATFORM_ANY, TRUE, FALSE
)

use std.core
use std.core.mem
use std.math as math
use std.core.str as str
use std.core.common as common
use std.os.ui.render.dump as ui_profile
use std.os.ui.window.event (is_event, make_event, event_type, event_window, event_window_id, event_data)
use std.os.ui.window.consts (EVENT_KEY_PRESSED, EVENT_KEY_RELEASED, EVENT_KEY_CHAR,
   EVENT_FLAGS_DATA_DROP_EVENTS, EVENT_FLAGS_MONITOR_EVENTS,
   EVENT_FLAGS_ALL,
   EVENT_KEY_CHAR_MODS, EVENT_FLAG_KEY_CHAR_MODS,
   EVENT_MOUSE_BUTTON_PRESSED, EVENT_MOUSE_BUTTON_RELEASED,
   EVENT_MOUSE_POS_CHANGED, EVENT_MOUSE_SCROLL,
   EVENT_MOUSE_ENTER, EVENT_MOUSE_LEAVE,
   EVENT_WINDOW_RESIZED, EVENT_WINDOW_MOVED, EVENT_WINDOW_REFRESH,
   EVENT_WINDOW_MAXIMIZED, EVENT_WINDOW_MINIMIZED, EVENT_WINDOW_RESTORED,
   EVENT_FOCUS_IN, EVENT_FOCUS_OUT, EVENT_SCALE_UPDATED,
   EVENT_MONITOR_CONNECTED, EVENT_MONITOR_DISCONNECTED,

EVENT_DATA_DROP, EVENT_QUIT)

use std.os.ui.window.platform.api
use std.os.ui.window.platform.api as api
use std.os.ui.window.platform.state as platform_state
use std.os.ui.window.platform.opengl as opengl_backend
use std.os.ui.window.platform.linux.joystick as linux_joystick
use std.os.ui.window.platform.win32.joystick as win32_joystick
use std.os.ui.window.platform.cocoa.joystick as cocoa_joystick
use std.os.ui.window.platform.win32 as win32_impl
use std.os.ui.window.platform.cocoa as cocoa_impl
use std.os.ui.window.platform.linux.x11 as x11_backend
use std.os.ui.window.platform.linux.wayland as wayland_backend
use std.os.ui.render.vk.vulkan (vk_get_instance_proc_addr)

mut _wayland_scroll_gain_cache = -1.0
mut _xwayland_scroll_fix_cache = -1
mut _wayland_scroll_fix_cache = -1

fn _ensure_platform_state() dict {
   mut state = platform_state._ensure_platform_state()
   def hints = state.get("window_hints", 0)
   if(!is_dict(hints) || hints.len == 0){
      platform_state._set_platform_val("window_hints", _default_window_hints())
      state = platform_state._ensure_platform_state()
   }
   state
}

fn _get_platform_val(any key, any default=0) any {
   def state = _ensure_platform_state()
   state.get(key, default)
}

fn _set_platform_val(any key, any val) any { platform_state._set_platform_val(key, val) }

fn _native_handle(any win) any {
   if(is_dict(win)){ return win.get("handle", 0) }
   win
}

fn _native_window(any win) any {
   def handle = _native_handle(win)
   if(!handle){ return 0 }
   def native_windows = _get_platform_val("native_windows")
   def direct = native_windows.get(handle, 0)
   if(direct){ return direct }
   def wins = dict_values(native_windows)
   mut i = 0
   def wins_n = wins.len
   while(i < wins_n){
      def cand = wins.get(i, 0)
      if(is_dict(cand) && cand.get("handle", 0) == handle){ return cand }
      i += 1
   }
   0
}

fn _is_debug() bool {
   ui_profile.debug_enabled()
}

fn _dbg(any msg) any { if(_is_debug()){ ui_profile.print_text("[window:platform] " + msg) } }

fn _dbg_win(any win, any msg) any { if(_is_debug()){ ui_profile.print_text("[window] win=0x" + str.to_hex(win) + " " + msg) } }

fn _dbg_hint(any hint, any val) any { }

fn _dbg_attrib(any win, any attrib, any val) any { }

fn _dbg_err(any msg) any { if(_is_debug()){ ui_profile.print_text("[window:platform:ERROR] " + msg) } }

fn _dbg_warn(any msg) any { if(_is_debug()){ ui_profile.print_text("[window:platform:WARN] " + msg) } }

fn _dbg_v(any msg) any { if(_is_debug()){ ui_profile.print_text("[window:platform] " + msg) } }

fn _joystick_poll_enabled() bool {
   if(ui_profile.env_truthy_cached("NY_UI_JOYSTICK")){ return true }
   if(_get_platform_val("joystick_callback", 0) != 0){ return true }
   false
}

fn _poll_joysticks_if_needed() any {
   if(!_joystick_poll_enabled()){ return 0 }
   #linux { linux_joystick.poll_joysticks() }
   #windows { win32_joystick.poll_joysticks() }
   #macos { cocoa_joystick.poll_joysticks() }
}

fn _dump_window_state(any win) any { return 0 }

fn _dump_all_windows() any {
   if(!_is_debug()){ return 0 }
   ui_profile.print_text("[window:state] *** All Windows Dump ***")
   def handles = dict_keys(_get_platform_val("native_windows", dict(8)))
   mut i = 0
   def handles_n = handles.len
   while(i < handles_n){
      def h = handles.get(i)
      _dump_window_state(h)
      i += 1
   }
   ui_profile.print_text("[window:state] Total windows: " + to_str(handles_n))
}

fn _default_window_hints() dict {
   mut hints = dict(8)
   hints[api.RESIZABLE] = api.TRUE
   hints[api.VISIBLE] = api.TRUE
   hints[api.DECORATED] = api.TRUE
   hints[api.FOCUSED] = api.TRUE
   hints[api.AUTO_ICONIFY] = api.TRUE
   hints[api.FLOATING] = api.FALSE
   hints[api.MAXIMIZED] = api.FALSE
   hints[api.CENTER_CURSOR] = api.TRUE
   hints[api.TRANSPARENT_FRAMEBUFFER] = api.FALSE
   hints[api.FOCUS_ON_SHOW] = api.TRUE
   hints[api.MOUSE_PASSTHROUGH] = api.FALSE
   hints[api.DOUBLEBUFFER] = api.TRUE
   hints[api.SCALE_FRAMEBUFFER] = api.TRUE
   hints[api.RED_BITS] = 8
   hints[api.GREEN_BITS] = 8
   hints[api.BLUE_BITS] = 8
   hints[api.ALPHA_BITS] = 8
   hints[api.DEPTH_BITS] = 24
   hints[api.STENCIL_BITS] = 8
   hints[api.STEREO] = api.FALSE
   hints[api.SAMPLES] = 0
   hints[api.SRGB_CAPABLE] = api.FALSE
   hints[api.CLIENT_API] = api.NO_API
   hints[api.CONTEXT_VERSION_MAJOR] = 1
   hints[api.CONTEXT_VERSION_MINOR] = 0
   hints[api.CONTEXT_DEBUG] = api.FALSE
   hints[api.CONTEXT_NO_ERROR] = api.FALSE
   hints[api.OPENGL_PROFILE] = api.OPENGL_ANY_PROFILE
   hints[api.OPENGL_FORWARD_COMPAT] = api.FALSE
   hints[api.CONTEXT_RELEASE_BEHAVIOR] = api.ANY_RELEASE_BEHAVIOR
   hints[api.CONTEXT_CREATION_API] = api.NATIVE_CONTEXT_API
   hints
}

def NO_ERROR = int(api.NO_ERROR)
def NOT_INITIALIZED = int(api.NOT_INITIALIZED)
def NO_CURRENT_CONTEXT = int(api.NO_CURRENT_CONTEXT)
def INVALID_ENUM = int(api.INVALID_ENUM)
def INVALID_VALUE = int(api.INVALID_VALUE)
def OUT_OF_MEMORY = int(api.OUT_OF_MEMORY)
def API_UNAVAILABLE = int(api.API_UNAVAILABLE)
def VERSION_UNAVAILABLE = int(api.VERSION_UNAVAILABLE)
def PLATFORM_ERROR = int(api.PLATFORM_ERROR)

fn _select_backend_name() str {
   def current = _get_platform_val("backend_name", "")
   if(current != ""){ return current }
   def requested = common.env_lower("NY_UI_BACKEND")
   mut name = ""
   if(requested == "none"){ name = "none" }
   else {
      #macos {
         if((requested == "" || requested == "auto" || requested == "cocoa" || requested == "macos") && cocoa_impl.available()){ name = "cocoa" }
         else { name = "none" }
      } #elif windows {
         if((requested == "" || requested == "auto" || requested == "win32" || requested == "windows") && win32_impl.available()){ name = "win32" }
         else { name = "none" }
      } #else {
         if(requested == "x11" && x11_backend.available()){ name = "x11" }
         elif(requested == "wayland" && wayland_backend.available()){ name = "wayland" }
         elif(x11_backend.available()){ name = "x11" }
         elif(wayland_backend.available()){ name = "wayland" }
         else { name = "none" }
      } #endif
   }
   if(ui_profile.debug_enabled()){
      #linux {
         ui_profile.print_text("[window:platform] backend select: requested='" + requested + "' x11=" + to_str(x11_backend.available()) + " wayland=" + to_str(wayland_backend.available()) + " -> " + name)
      } #elif macos {
         ui_profile.print_text("[window:platform] backend select: requested='" + requested + "' cocoa=" + to_str(cocoa_impl.available()) + " -> " + name)
      } #elif windows {
         ui_profile.print_text("[window:platform] backend select: requested='" + requested + "' win32=" + to_str(win32_impl.available()) + " -> " + name)
      } #else {
         ui_profile.print_text("[window:platform] backend select: requested='" + requested + "' -> " + name)
      } #endif
   }
   _set_platform_val("backend_name", name)
   name
}

fn get_backend_name() str { "Returns the selected native window backend name." return _select_backend_name() }

fn uses_native_events() bool { "Returns true because this backend owns native event polling." true }

fn get_error() int {
   "Returns and clears the last platform error code."
   def code = _get_platform_val("last_error_code", 0)
   _set_platform_val("last_error_code", 0)
   _set_platform_val("last_error_desc", "")
   code
}

fn _set_error(int code, str desc) any {
   _set_platform_val("last_error_code", code)
   _set_platform_val("last_error_desc", desc)
   if(_is_debug()){ ui_profile.print_text("[window:platform:ERROR] code=0x" + str.to_hex(code) + " desc='" + desc + "'") }
   def cb = _get_platform_val("error_callback", 0)
   if(cb){
      _dbg_v("_set_error: firing callback with code=0x" + str.to_hex(code))
      cb(code, desc)
   }
}

fn set_error_callback(any cb) any {
   "Installs a platform error callback and returns the previous callback."
   def old = _get_platform_val("error_callback", 0)
   _set_platform_val("error_callback", cb)
   old
}

fn supports_state_polling() bool { "Returns true when platform state can be polled." true }

fn get_version() list {
   "Returns the compatibility version tuple."
   [3, 5, 0]
}

fn get_version_string() str {
   "Returns a backend-specific compatibility version string."
   def b = _select_backend_name()
   case b {
      "x11" -> "3.5.0 X11 EGL OSMesa dynamically-linked"
      "wayland" -> "3.5.0 Wayland EGL OSMesa dynamically-linked"
      "win32" -> "3.5.0 Win32 Vulkan native"
      "cocoa" -> "3.5.0 Cocoa Vulkan native"
      _ -> "3.5.0 Null native"
   }
}

fn get_platform() int {
   "Returns the active backend platform selector constant."
   def b = _select_backend_name()
   case b {
      "x11" -> PLATFORM_X11
      "wayland" -> PLATFORM_WAYLAND
      "win32" -> PLATFORM_WIN32
      "cocoa" -> PLATFORM_COCOA
      _ -> PLATFORM_NULL
   }
}

fn init_hint(any hint, any value) any {
   "Stores an initialization hint for backend selection."
   if(hint == PLATFORM){
      if(value == PLATFORM_X11){ _set_platform_val("backend_name", "x11") }
      elif(value == PLATFORM_WAYLAND){ _set_platform_val("backend_name", "wayland") }
      elif(value == PLATFORM_WIN32){ _set_platform_val("backend_name", "win32") }
      elif(value == PLATFORM_COCOA){ _set_platform_val("backend_name", "cocoa") }
      elif(value == PLATFORM_NULL){ _set_platform_val("backend_name", "none") }
   }
   mut hints = _get_platform_val("init_hints", dict(8))
   hints[hint] = value
   _set_platform_val("init_hints", hints)
}

fn init_allocator(any alloc) bool { "Accepts an allocator hook; custom allocators are not used yet." false }

fn init_vulkan_loader(any path) int {
   "Overrides the Vulkan shared library path used during init. Must be called before init()."
   if(is_str(path) && path.len > 0){
      api.set_vulkan_loader_path(path)
      1
   } else { 0 }
}

fn _timer_raw_ns() int {
   def ts = malloc(16)
   if(ts == 0){ return 0 }
   defer { free(ts) }
   def r = __clock_gettime(1, ts)
   if(r != 0){ return 0 }
   def s = from_int(load64(ts, 0))
   def ns = from_int(load64(ts, 8))
   def raw = s * 1000000000 + ns
   raw
}

fn _timer_base() int {
   def offset = _get_platform_val("timer_offset", -1)
   if(offset == -1){
      def now = _timer_raw_ns()
      _set_platform_val("timer_offset", now)
      return now
   }
   offset
}

fn get_timer_value() int { "Returns the monotonic timer value in nanoseconds." def base = _timer_base() _timer_raw_ns() - base }

fn get_timer_frequency() int { "Returns the timer frequency in ticks per second." 1000000000 }

fn get_time() f64 { "Returns monotonic time in seconds." get_timer_value() * 1.0 / 1000000000.0 }

fn set_time(any t) any { _set_platform_val("timer_offset", _timer_raw_ns() - to_int(t * 1000000000.0)) }

fn wait_events() any { poll_events() }

fn wait_events_timeout(any t) any { poll_events() }

fn init() bool {
   "Initializes the selected window backend."
   _dbg("init")
   def b = _select_backend_name()
   if(b == "x11"){
      _set_platform_val("initialized", true)
      return true
   }
   if(b == "wayland"){
      def display = wayland_backend.connect_display()
      if(!display){
         _dbg_warn("Wayland display connect failed, falling back to X11")
         _set_platform_val("backend_name", "x11")
         _set_platform_val("initialized", true)
         return true
      }
      def globals = wayland_backend.bootstrap_globals(display)
      wayland_backend.set_globals(globals)
      mut p = _get_platform_val("platform", dict(8))
      p["wayland_display"] = display
      p["wayland_globals"] = globals
      _set_platform_val("platform", p)
      _set_platform_val("initialized", true)
      return globals != 0
   }
   if(b == "win32"){
      win32_impl.init_dpi_awareness()
      win32_impl.init_timer()
      _set_platform_val("initialized", true)
      return true
   }
   if(b == "cocoa"){
      cocoa_impl.set_activation_policy_regular()
      cocoa_impl.finish_launching()
      _set_platform_val("initialized", true)
      return true
   }
   _set_platform_val("initialized", true)
   true
}

fn terminate() any {
   "Terminates backend state and joystick polling."
   _dbg("terminate")
   def b = _select_backend_name()
   if(b == "wayland"){
      def p = _get_platform_val("platform", dict(8))
      def wayland_globals = p.get("wayland_globals", 0)
      if(wayland_globals){ wayland_backend.destroy_globals(wayland_globals) }
   }
   #linux {
      linux_joystick.terminate()
   } #elif windows {
      win32_joystick.terminate()
   } #elif macos {
      cocoa_joystick.terminate()
   } #endif
}

fn create_window(str name, int x, int y, int w, int h, int flags) any {
   "Creates a native platform window and returns its handle."
   def b = _select_backend_name()
   if(ui_profile.debug_enabled()){ ui_profile.print_text("[window:platform] create_window: backend=" + b + " title='" + name + "'") }
   def hints = _get_platform_val("window_hints", dict(8))
   mut x11_class = common.env_trim("NY_UI_X11_CLASS_NAME")
   mut x11_inst = common.env_trim("NY_UI_X11_INSTANCE_NAME")
   mut wl_app_id = common.env_trim("NY_UI_WAYLAND_APP_ID")
   if(x11_class.len == 0){ x11_class = hints.get(api.X11_CLASS_NAME, "Nytrix") }
   if(x11_inst.len == 0){ x11_inst = hints.get(api.X11_INSTANCE_NAME, "nytrix") }
   if(wl_app_id.len == 0){ wl_app_id = hints.get(api.WAYLAND_APP_ID, "nytrix") }
   def client_api = band(flags, 0x80000) ? api.NO_API : hints.get(api.CLIENT_API, api.NO_API)
   mut visual = 0
   mut depth = 0
   mut x11_display = 0
   if(b == "x11" && client_api != api.NO_API){
      x11_display = x11_backend.open_display()
      if(x11_display){
         def s = x11_backend.default_screen(x11_display)
         def info = opengl_backend.choose_visual(hints, x11_display, s)
         visual, depth = info.get(0), info.get(1)
      }
   }
   mut native = 0
   if(b == "x11"){ native = x11_backend.create_basic_window(name, w, h, x, y, flags, 0, x11_class, x11_inst, visual, depth, x11_display) }
   elif(b == "wayland"){
      def wayland_globals = _get_platform_val("platform", dict(8)).get("wayland_globals", 0)
      native = wayland_backend.create_basic_window(wayland_globals, name, w, h, wl_app_id)
   } elif(b == "win32"){
      native = win32_impl.create_basic_window(name, w, h, x, y, flags)
   } elif(b == "cocoa"){
      native = cocoa_impl.create_basic_window(name, w, h, x, y, flags)
   } elif(b == "none"){
      def native_windows = _get_platform_val("native_windows", dict(8))
      def handle = len(dict_keys(native_windows)) + 1
      native = dict(8)
      native["handle"] = handle
      native["title"] = name
      native["w"] = w
      native["h"] = h
      native["x"] = x
      native["y"] = y
      native["flags"] = flags
      native["backend"] = "none"
      native["headless"] = true
   }
   if(native){
      def handle = _native_handle(native)
      if(!handle){
         _dbg_err("create_window: returned native state without a handle")
         _dump_window_state(0)
         return false
      }
      mut native_windows = _get_platform_val("native_windows", dict(16))
      native_windows[handle] = native
      _set_platform_val("native_windows", native_windows)
      mut should_close_flags = _get_platform_val("should_close_flags", dict(8))
      should_close_flags[handle] = false
      _set_platform_val("should_close_flags", should_close_flags)
      if((b == "x11" || b == "wayland") && client_api != api.NO_API){
         def ctx = opengl_backend.create_context(native, hints)
         if(ctx){
            mut window_contexts = _get_platform_val("window_contexts", dict(8))
            window_contexts[handle] = ctx
            _set_platform_val("window_contexts", window_contexts)
         } else {
            _dbg_warn("create_window: OpenGL context creation failed(may be expected for NO_API)")
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

fn destroy_window(any win) any {
   "Destroys a native window and clears stored platform state."
   def handle = _native_handle(win)
   def native = _native_window(win)
   _dbg_win(handle, "destroy_window called")
   _dump_window_state(handle)
   if(!handle){
      _dbg_err("destroy_window: called with invalid window(no handle)")
      return 0
   }
   def b = _select_backend_name()
   def window_contexts = _get_platform_val("window_contexts", dict(8))
   def ctx = window_contexts.get(handle, 0)
   if(ctx){
      if(_get_platform_val("current_win_context", 0) == handle){
         opengl_backend.release_context_current()
         _set_platform_val("current_win_context", 0)
         _dbg_v("destroy_window: cleared current context")
      }
      opengl_backend.destroy_offscreen_context(ctx)
      _set_platform_val("window_contexts", window_contexts.delete(handle))
      _dbg_v("destroy_window: OpenGL context destroyed")
   }
   if(b == "x11"){ x11_backend.destroy_basic_window(native) }
   elif(b == "wayland"){ wayland_backend.destroy_basic_window(native) }
   elif(b == "win32"){ win32_impl.destroy_basic_window(native) }
   elif(b == "cocoa"){ cocoa_impl.destroy_basic_window(native) }
   _set_platform_val("native_windows", _get_platform_val("native_windows", dict(8)).delete(handle))
   _set_platform_val("should_close_flags", _get_platform_val("should_close_flags", dict(8)).delete(handle))
   _set_platform_val("window_attribs", _get_platform_val("window_attribs", dict(8)).delete(handle))
   if(_get_platform_val("current_win_context", 0) == handle){
      _set_platform_val("current_win_context", 0)
      _dbg_v("destroy_window: cleared current context")
   }
   _set_platform_val("window_user_pointers", _get_platform_val("window_user_pointers", dict(8)).delete(handle))
   _set_platform_val("window_size_limits", _get_platform_val("window_size_limits", dict(8)).delete(handle))
   _set_platform_val("window_aspect_ratios", _get_platform_val("window_aspect_ratios", dict(8)).delete(handle))
   _set_platform_val("window_callbacks", _get_platform_val("window_callbacks", dict(8)).delete(handle))
   _dbg_win(handle, "destroyed - remaining windows: " + to_str(len(dict_keys(_get_platform_val("native_windows", dict(8))))))
}

fn should_close(any win) bool { "Returns the requested-close flag for a window." _get_platform_val("should_close_flags", dict(8)).get(win, false) }

fn set_should_close(any win, any value) any {
   "Sets the requested-close flag for a window."
   def flags = _get_platform_val("should_close_flags", dict(8))
   if(!flags.contains(win) || flags.get(win, false) != value){ _dbg("set_should_close: win=0x" + str.to_hex(win) + " value=" + to_str(value)) }
   flags[win] = value
   _set_platform_val("should_close_flags", flags)
}

fn default_window_hints() any {
   "Resets all window hints to their default values."
   _dbg("default_window_hints: resetting to defaults")
   _set_platform_val("window_hints", _default_window_hints())
}

fn window_hint(any hint, any value) any {
   "Sets a window hint to a specific value."
   _dbg_hint(hint, value)
   mut hints = _get_platform_val("window_hints", dict(8))
   hints[hint] = value
   _set_platform_val("window_hints", hints)
}

fn window_hint_string(any hint, str value) any {
   "Sets a string window hint(for class names, app IDs, etc.)."
   _dbg("window_hint_string: hint=0x" + str.to_hex(hint) + " value='" + value + "'")
   mut hints = _get_platform_val("window_hints", dict(8))
   hints[hint] = value
   _set_platform_val("window_hints", hints)
}

fn get_window_attrib(any win, any attrib) any {
   "Gets a window attribute."
   def handle = _native_handle(win)
   def window_attribs = _get_platform_val("window_attribs", dict(8))
   def attribs = window_attribs.get(handle, dict(8))
   def val = attribs.get(attrib, 0)
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
   } elif(b == "win32"){
      def res = win32_impl.get_window_attrib(native, attrib)
      _dbg_attrib(handle, attrib, res)
      return res
   } elif(b == "cocoa"){
      def res = cocoa_impl.get_window_attrib(native, attrib)
      _dbg_attrib(handle, attrib, res)
      return res
   }
   def window_hints = _get_platform_val("window_hints", dict(8))
   def hint_val = window_hints.get(attrib, 0)
   _dbg_attrib(handle, attrib, hint_val)
   hint_val
}

fn set_window_attrib(any win, any attrib, any value) any {
   "Sets a window attribute."
   def handle = _native_handle(win)
   _dbg("set_window_attrib: win=0x" + str.to_hex(handle) + " attrib=0x" + str.to_hex(attrib) + " value=" + to_str(value))
   mut window_attribs = _get_platform_val("window_attribs", dict(8))
   mut attribs = window_attribs.get(handle, dict(8))
   attribs[attrib] = value
   window_attribs[handle] = attribs
   _set_platform_val("window_attribs", window_attribs)
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      if(attrib == api.DECORATED){ x11_backend.set_window_decorated(native, value != 0) }
      if(attrib == api.RESIZABLE){ x11_backend.set_window_resizable(native, value != 0) }
      if(attrib == api.FLOATING){ x11_backend.set_window_floating(native, value != 0) }
      if(attrib == api.MOUSE_PASSTHROUGH){ x11_backend.set_window_mouse_passthrough(native, value != 0) }
   } elif(b == "wayland"){
   } elif(b == "win32"){
      if(attrib == api.DECORATED){ win32_impl.set_window_decorated(native, value != 0) }
      if(attrib == api.RESIZABLE){ win32_impl.set_window_resizable(native, value != 0) }
      if(attrib == api.FLOATING){ win32_impl.set_window_floating(native, value != 0) }
   } elif(b == "cocoa"){
      if(attrib == api.DECORATED){ cocoa_impl.set_window_decorated(native, value != 0) }
      if(attrib == api.RESIZABLE){ cocoa_impl.set_window_resizable(native, value != 0) }
      if(attrib == api.FLOATING){ cocoa_impl.set_window_floating(native, value != 0) }
   }
}

fn set_window_size_limits(any win, int min_w, int min_h, int max_w, int max_h) any {
   "Sets the size limits for a window."
   if(!win){
      _dbg_err("set_window_size_limits: called with null window")
      return 0
   }
   if(min_w < 0 || min_h < 0){ _dbg_warn("set_window_size_limits: negative min values(min_w=" + to_str(min_w) + ", min_h=" + to_str(min_h) + ")") }
   if(max_w < 0 || max_h < 0 && max_w != -1 && max_h != -1){ _dbg_warn("set_window_size_limits: negative max values(max_w=" + to_str(max_w) + ", max_h=" + to_str(max_h) + ")") }
   if(max_w != -1 && min_w != -1 && max_w < min_w){ _dbg_warn("set_window_size_limits: max_w(" + to_str(max_w) + ") < min_w(" + to_str(min_w) + ")") }
   if(max_h != -1 && min_h != -1 && max_h < min_h){ _dbg_warn("set_window_size_limits: max_h(" + to_str(max_h) + ") < min_h(" + to_str(min_h) + ")") }
   def limits = {"min_w": min_w, "min_h": min_h, "max_w": max_w, "max_h": max_h}
   mut window_size_limits = _get_platform_val("window_size_limits", dict(8))
   window_size_limits[win] = limits
   _set_platform_val("window_size_limits", window_size_limits)
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      def result = x11_backend.set_window_size_limits(native, min_w, min_h, max_w, max_h)
      if(!result){ _dbg_err("set_window_size_limits: x11_backend returned false") }
      _dbg_v("set_window_size_limits: X11 limits set min=" + to_str(min_w) + "x" + to_str(min_h) + " max=" + to_str(max_w) + "x" + to_str(max_h))
   } elif(b == "wayland"){
      def result = wayland_backend.set_window_size_limits(native, min_w, min_h, max_w, max_h)
      if(!result){ _dbg_err("set_window_size_limits: wayland_backend returned false") }
      _dbg_v("set_window_size_limits: Wayland limits set")
   } elif(b == "win32"){
      def result = win32_impl.set_window_size_limits(native, min_w, min_h, max_w, max_h)
      if(!result){ _dbg_err("set_window_size_limits: win32_impl returned false") }
      _dbg_v("set_window_size_limits: Win32 limits set")
   } elif(b == "cocoa"){
      _dbg_v("set_window_size_limits: Cocoa limits tracked")
   } else {
      _dbg_warn("set_window_size_limits: unknown backend '" + b + "'")
   }
}

fn set_window_aspect_ratio(any win, int numer, int denom) any {
   "Sets the aspect ratio for a window."
   def ratio = {"numer": numer, "denom": denom}
   mut window_aspect_ratios = _get_platform_val("window_aspect_ratios", dict(8))
   window_aspect_ratios[win] = ratio
   _set_platform_val("window_aspect_ratios", window_aspect_ratios)
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      x11_backend.set_window_aspect_ratio(native, numer, denom)
   }
}

fn get_window_frame_size(any win) list {
   "Returns the window frame size [left, top, right, bottom]."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_window_frame_size(native) }
   elif(b == "wayland"){ wayland_backend.get_window_frame_size(native) }
   elif(b == "cocoa"){ [0, 0, 0, 0] }
   elif(b == "win32"){ [0, 0, 0, 0] }
   else { [0, 0, 0, 0] }
}

fn get_window_content_scale(any win) list {
   "Returns the content scale [xscale, yscale]."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_window_content_scale(native) }
   elif(b == "wayland"){ wayland_backend.get_window_content_scale(native) }
   elif(b == "cocoa"){
      def s = cocoa_impl.get_window_content_scale(native)
      [s, s]
   }
   elif(b == "win32"){ [1.0, 1.0] }
   else { [1.0, 1.0] }
}

fn get_window_opacity(any win) f64 {
   "Returns the window opacity(0.0 to 1.0)."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.get_window_opacity(native) }
   elif(b == "wayland"){ wayland_backend.get_window_opacity(native) }
   elif(b == "win32"){ win32_impl.get_window_opacity(native) }
   else { 1.0 }
}

comptime template _set_win_cb_wrap(name, key){
   fn ${name}(any win, any cb) any { _set_win_cb(win, key, cb) }
}

comptime emit _set_win_cb_wrap(set_window_pos_callback, "pos")
comptime emit _set_win_cb_wrap(set_window_maximize_callback, "maximize")
comptime emit _set_win_cb_wrap(set_window_content_scale_callback, "scale")
comptime emit _set_win_cb_wrap(set_window_iconify_callback, "iconify")
comptime emit _set_win_cb_wrap(set_window_focus_callback, "focus")
comptime emit _set_win_cb_wrap(set_window_refresh_callback, "refresh")
comptime emit _set_win_cb_wrap(set_framebuffer_size_callback, "fbsize")

fn _merge_native_window_updates(any updated, any native_windows) any {
   if(!is_dict(updated)){ return native_windows }
   def handles = dict_keys(updated)
   mut current_native_windows = native_windows
   mut hi = 0
   def handles_n = handles.len
   while(hi < handles_n){
      def handle = handles.get(hi)
      def win = updated.get(handle, 0)
      if(handle && is_dict(win)){ current_native_windows[handle] = win }
      hi += 1
   }
   current_native_windows
}

fn _display_from_native_windows(any windows) any {
   if(!is_list(windows) || windows.len == 0){ return 0 }
   mut i = 0
   def n = windows.len
   while(i < n){
      def win = windows.get(i, 0)
      if(is_dict(win)){
         def display = win.get("display", 0)
         if(display){ return display }
      }
      i += 1
   }
   0
}


fn _native_queue_push(any q, any ne) list {
   if(!is_list(q)){ q = [] }
   if(is_event(ne) && event_type(ne) == EVENT_MOUSE_POS_CHANGED && q.len > 0){
      def last = q.get(q.len - 1, [])
      if(is_event(last) && event_type(last) == EVENT_MOUSE_POS_CHANGED){
         q[q.len - 1] = ne
         return q
      }
   }
   q.append(ne)
}

fn _queue_native_events(any evs, any fallback_handle=0, bool dispatch_callbacks=false) int {
   if(!is_list(evs) || evs.len <= 0){ return 0 }
   mut pending_events = _get_platform_val("pending_native_events", dict(8))
   mut normalized = []
   mut ei = 0
   def evs_n = evs.len
   while(ei < evs_n){
      def ne = _normalize_native_event(evs.get(ei))
      mut wid = event_window_id(ne)
      if(!wid){ wid = fallback_handle }
      if(wid){
         def q = pending_events.get(wid, [])
         pending_events[wid] = _native_queue_push(q, ne)
         if(event_type(ne) == EVENT_QUIT){
            mut flags = _get_platform_val("should_close_flags", dict(8))
            flags[wid] = true
            _set_platform_val("should_close_flags", flags)
         }
      }
      normalized = normalized.append(ne)
      ei += 1
   }
   _set_platform_val("pending_native_events", pending_events)
   if(dispatch_callbacks){
      ei = 0
      while(ei < evs_n){
         _dispatch_event_callbacks(normalized.get(ei))
         ei += 1
      }
   }
   evs_n
}

fn poll_events() any {
   "Polls native events and dispatches queued callbacks."
   def b = _select_backend_name()
   _poll_joysticks_if_needed()
   def native_windows = _get_platform_val("native_windows", dict(8))
   def windows = dict_values(native_windows)
   def have_window_callbacks = !!_get_platform_val("has_window_callbacks", false)
   if(b == "x11"){
      def display = _display_from_native_windows(windows)
      if(display && x11_backend.pending(display) == 0){ return 0 }
      if(windows.len > 0){
         def result = x11_backend.poll_display_events(windows)
         def updated = result.get(0, dict(8))
         _set_platform_val("native_windows", _merge_native_window_updates(updated, native_windows))
         def _ignored_x11_events = _queue_native_events(result.get(1, []), 0, have_window_callbacks)
      }
      return 0
   }
   if(b == "wayland"){
      def display = get_wayland_display()
      if(display){ wayland_backend.wait_events(display, 0) }
   }
   if(b == "win32"){
      mut i = 0
      def windows_n = windows.len
      while(i < windows_n){
         def win = windows.get(i)
         if(is_dict(win)){
            def handle = win.get("handle", 0)
            def result = win32_impl.poll_window_events(win)
            def updated_win = result.get(0, win)
            def evs = result.get(1, [])
            mut current_native_windows = _get_platform_val("native_windows", native_windows)
            if(handle && is_dict(updated_win)){
               current_native_windows[handle] = updated_win
               _set_platform_val("native_windows", current_native_windows)
            }
            def _ignored_win32_events = _queue_native_events(evs, handle, have_window_callbacks)
         }
         i += 1
      }
      return 0
   }
   if(b == "cocoa"){
      mut i = 0
      def windows_n = windows.len
      while(i < windows_n){
         def win = windows.get(i)
         if(is_dict(win)){
            def handle = win.get("handle", 0)
            def result = cocoa_impl.poll_window_events(win)
            def updated_win = result.get(0, win)
            def evs = result.get(1, [])
            mut current_native_windows = _get_platform_val("native_windows", native_windows)
            if(handle && is_dict(updated_win)){
               current_native_windows[handle] = updated_win
               _set_platform_val("native_windows", current_native_windows)
            }
            def _ignored_cocoa_events = _queue_native_events(evs, handle, have_window_callbacks)
         }
         i += 1
      }
      return 0
   }
   mut i = 0
   def windows_n = windows.len
   while(i < windows_n){
      def win = windows.get(i)
      if(!is_dict(win)){
         _dbg_warn("poll_events: window at index " + to_str(i) + " is not a dict")
         i += 1
         continue
      }
      def handle = win.get("handle", 0)
      if(!handle){
         _dbg_warn("poll_events: window at index " + to_str(i) + " has no handle")
         i += 1
         continue
      }
      if(b == "wayland"){
         def result = wayland_backend.poll_window_events(win)
         def updated_win = result.get(0, win)
         def evs = result.get(1, [])
         mut current_native_windows = _get_platform_val("native_windows", native_windows)
         if(is_dict(updated_win)){
            current_native_windows[handle] = updated_win
            _set_platform_val("native_windows", current_native_windows)
         }
         def _ignored_wayland_events = _queue_native_events(evs, handle, have_window_callbacks)
      } else {
         _dbg_warn("poll_events: unknown backend '" + b + "'")
      }
      i += 1
   }
}

fn poll_events_for_window(any win) list {
   "Polls native events and returns this window's event list without the global pending-event hop when safe."
   def handle = _native_handle(win)
   if(!handle){ return [] }
   def b = _select_backend_name()
   if(b == "x11"){
      _poll_joysticks_if_needed()
      def native_windows = _get_platform_val("native_windows", dict(8))
      def native = native_windows.get(handle, 0)
      def display = is_dict(native) ? native.get("display", 0) : 0
      if(display && x11_backend.pending(display) == 0){ return [] }
      if(native && native_windows.len == 1){
         def result = x11_backend.poll_window_events(native)
         def updated = result.get(0, native)
         def raw = result.get(1, [])
         if(is_dict(updated)){
            native_windows[handle] = updated
            _set_platform_val("native_windows", native_windows)
         }
         if(!is_list(raw) || raw.len == 0){ return [] }
         def have_window_callbacks = !!_get_platform_val("has_window_callbacks", false)
         mut out = []
         def raw_n = raw.len
         mut i = 0
         while(i < raw_n){
            def ne = _normalize_native_event(raw.get(i))
            out = _native_queue_push(out, ne)
            if(have_window_callbacks){ _dispatch_event_callbacks(ne) }
            i += 1
         }
         return out
      }
   }
   if(b == "win32"){
      _poll_joysticks_if_needed()
      def native_windows = _get_platform_val("native_windows", dict(8))
      def native = native_windows.get(handle, 0)
      def result = win32_impl.poll_window_events(native)
      def updated = result.get(0, native)
      def raw = result.get(1, [])
      if(is_dict(updated)){
         native_windows[handle] = updated
         _set_platform_val("native_windows", native_windows)
      }
      if(!is_list(raw) || raw.len == 0){ return [] }
      def have_window_callbacks = !!_get_platform_val("has_window_callbacks", false)
      mut out = []
      mut i = 0
      def raw_n = raw.len
      while(i < raw_n){
         def ne = _normalize_native_event(raw.get(i))
         out = _native_queue_push(out, ne)
         if(have_window_callbacks){ _dispatch_event_callbacks(ne) }
         i += 1
      }
      return out
   }
   if(b == "cocoa"){
      _poll_joysticks_if_needed()
      def native_windows = _get_platform_val("native_windows", dict(8))
      def native = native_windows.get(handle, 0)
      def result = cocoa_impl.poll_window_events(native)
      def updated = result.get(0, native)
      def raw = result.get(1, [])
      if(is_dict(updated)){
         native_windows[handle] = updated
         _set_platform_val("native_windows", native_windows)
      }
      if(!is_list(raw) || raw.len == 0){ return [] }
      def have_window_callbacks = !!_get_platform_val("has_window_callbacks", false)
      mut out = []
      mut i = 0
      def raw_n = raw.len
      while(i < raw_n){
         def ne = _normalize_native_event(raw.get(i))
         out = _native_queue_push(out, ne)
         if(have_window_callbacks){ _dispatch_event_callbacks(ne) }
         i += 1
      }
      return out
   }
   _poll_joysticks_if_needed()
   poll_events()
   pump_window_events(handle)
}

fn pump_window_events(any win) list {
   "Returns and clears queued events for one window."
   def handle = _native_handle(win)
   def pending_native_events = _get_platform_val("pending_native_events", dict(8))
   def evs = pending_native_events.get(handle, [])
   if(!is_list(evs) || evs.len == 0){ return evs }
   _set_platform_val("pending_native_events", pending_native_events.delete(handle))
   evs
}

fn swap_buffers(any win) any {
   "Presents the current OpenGL backbuffer for a window."
   def b = _select_backend_name()
   def handle = _native_handle(win)
   _dbg_v("swap_buffers: backend=" + b + " win=0x" + str.to_hex(handle))
   def window_contexts = _get_platform_val("window_contexts", dict(8))
   def ctx = window_contexts.get(handle, 0)
   if(ctx && (b == "x11" || b == "wayland")){ opengl_backend.swap_buffers(ctx) }
   if(b == "x11"){ x11_backend.flush(get_x11_display()) }
}

fn blit_software(any win, any buf, int w, int h) any {
   "Blits a software RGBA framebuffer directly to the native window."
   def b = _select_backend_name()
   if(b == "x11"){
      def dpy = get_x11_display()
      def native = _native_window(win)
      if(dpy && native){ x11_backend.put_pixels(dpy, native, buf, w, h) }
   }
}

fn get_proc_address(any name) any {
   "Runs the get proc address operation."
   def b = _select_backend_name()
   if(b == "x11" || b == "wayland"){ return opengl_backend.get_proc_address(name) }
   0
}

fn get_instance_proc_address(any instance, any name) any {
   "Returns a Vulkan instance procedure address for the active backend."
   def b = _select_backend_name()
   if(b == "x11" || b == "wayland" || b == "win32" || b == "cocoa"){ return vk_get_instance_proc_addr(instance, name) }
   0
}

fn make_context_current(any win) bool {
   "Makes a window OpenGL context current, or releases it when win is false."
   def b = _select_backend_name()
   if(!win){
      if(b == "x11" || b == "wayland"){ opengl_backend.release_context_current() }
      _set_platform_val("current_win_context", 0)
      return true
   }
   def window_contexts = _get_platform_val("window_contexts", dict(8))
   def ctx = window_contexts.get(win, 0)
   if(!ctx){ return false }
   if((b == "x11" || b == "wayland") && opengl_backend.make_context_current(ctx)){
      _set_platform_val("current_win_context", win)
      return true
   }
   false
}

fn get_current_context() any { _get_platform_val("current_win_context", 0) }
#linux {
   #link "libGL.so"
   #include <GL/gl.h>
} #else {
   fn glGetString(any _name) any {
      "Runs the glGetString operation."
      0
   }
} #endif

fn extension_supported(str name) bool {
   "Checks if the specified OpenGL extension is supported."
   def win = _get_platform_val("current_win_context", 0)
   if(!win){ return false }
   def exts_ptr = glGetString(0x1F03)
   if(!exts_ptr){ return false }
   def exts = str.cstr_to_str(exts_ptr)
   str.find(exts, name) != -1
}

fn vulkan_supported() bool {
   "Returns true when the active backend can create Vulkan surfaces."
   def b = _select_backend_name()
   mut ok = false
   if(b == "x11"){ ok = x11_backend.vulkan_supported() }
   elif(b == "wayland"){ ok = wayland_backend.vulkan_supported() }
   elif(b == "win32"){ ok = win32_impl.vulkan_supported() }
   elif(b == "cocoa"){ ok = cocoa_impl.vulkan_supported() }
   _dbg("vulkan_supported: backend=" + b + " supported=" + to_str(ok))
   ok
}

fn required_extensions() list {
   "Returns Vulkan instance extensions required by the active backend."
   def b = _select_backend_name()
   mut exts = []
   if(b == "x11"){ exts = x11_backend.vulkan_required_extensions() }
   elif(b == "wayland"){ exts = wayland_backend.vulkan_required_extensions() }
   elif(b == "win32"){ exts = win32_impl.vulkan_required_extensions() }
   elif(b == "cocoa"){ exts = cocoa_impl.vulkan_required_extensions() }
   exts
}

fn create_surface(any instance, any win, any allocator, any surface_out) int {
   "Creates a Vulkan surface for a native window."
   def b = _select_backend_name()
   def native = _native_window(win)
   mut res = 1
   if(b == "x11"){ res = x11_backend.create_surface(instance, native, allocator, surface_out) }
   elif(b == "wayland"){ res = wayland_backend.create_surface(instance, native, allocator, surface_out) }
   elif(b == "win32"){ res = win32_impl.create_surface(instance, native, allocator, surface_out) }
   elif(b == "cocoa"){ res = cocoa_impl.create_surface(instance, native, allocator, surface_out) }
   res
}

comptime template _dispatch_native_get0_list(name, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any win) list {
      def b = _select_backend_name()
      def native = _native_window(win)
      if(!is_dict(native)){ return [0, 0] }
      if(b == "x11"){ x11_fn(native) }
      elif(b == "wayland"){ wayland_fn(native) }
      elif(b == "win32"){ win32_fn(native) }
      elif(b == "cocoa"){ cocoa_fn(native) }
      else { [0, 0] }
   }
}

comptime template _dispatch_native_get1_int(name, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any win, any arg0) int {
      def b = _select_backend_name()
      def native = _native_window(win)
      if(!is_dict(native)){ return 0 }
      if(b == "x11"){ x11_fn(native, arg0) }
      elif(b == "wayland"){ wayland_fn(native, arg0) }
      elif(b == "win32"){ win32_fn(native, arg0) }
      elif(b == "cocoa"){ cocoa_fn(native, arg0) }
      else { 0 }
   }
}

comptime template _dispatch_native_set2(name, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any win, any arg0, any arg1) bool {
      def b = _select_backend_name()
      def native = _native_window(win)
      if(!is_dict(native)){ return false }
      if(b == "x11"){ x11_fn(native, arg0, arg1) }
      elif(b == "wayland"){ wayland_fn(native, arg0, arg1) }
      elif(b == "win32"){ win32_fn(native, arg0, arg1) }
      elif(b == "cocoa"){ cocoa_fn(native, arg0, arg1) }
      else { false }
   }
}

comptime emit _dispatch_native_get1_int(get_key, x11_backend.get_key_state, wayland_backend.get_key_state, win32_impl.get_key_state, cocoa_impl.get_key_state)
comptime emit _dispatch_native_get1_int(get_mouse_button, x11_backend.get_mouse_button_state, wayland_backend.get_mouse_button_state, win32_impl.get_mouse_button_state, cocoa_impl.get_mouse_button_state)
comptime emit _dispatch_native_get0_list(get_cursor_pos, x11_backend.get_cursor_pos, wayland_backend.get_cursor_pos, win32_impl.get_cursor_pos, cocoa_impl.get_cursor_pos)
comptime emit _dispatch_native_set2(set_cursor_pos, x11_backend.set_cursor_pos, wayland_backend.set_cursor_pos, win32_impl.set_cursor_pos, cocoa_impl.set_cursor_pos)

fn cursor_visible() bool {
   "Returns false only when a backend can prove the system cursor is hidden."
   def b = _select_backend_name()
   if(b == "win32"){ return win32_impl.cursor_visible() }
   true
}

fn _store_updated_native_window(any updated) bool {
   if(!is_dict(updated)){ return false }
   def handle = updated.get("handle", 0)
   if(!handle){ return false }
   mut native_windows = _get_platform_val("native_windows", dict(8))
   native_windows[handle] = updated
   _set_platform_val("native_windows", native_windows)
   true
}

fn set_input_mode(any win, int mode, int value) any {
   "Sets an input mode on the native window."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      _store_updated_native_window(x11_backend.set_input_mode(native, mode, value))
      return 0
   }
   if(b == "wayland"){ _store_updated_native_window(wayland_backend.set_input_mode(native, mode, value)) }
   if(b == "win32"){ _store_updated_native_window(win32_impl.set_input_mode(native, mode, value)) }
   if(b == "cocoa"){
      cocoa_impl.set_input_mode(native, mode, value)
      _store_updated_native_window(native)
   }
}

fn raw_mouse_motion_supported() bool {
   "Returns true when the backend can provide raw mouse motion."
   def b = _select_backend_name()
   if(b == "x11"){ return true } ;; Assume XInput2 is checked by backend internally
   if(b == "wayland"){ return true } ;; Relative pointer protocol
   if(b == "win32"){ return true }
   if(b == "cocoa"){ return true }
   false
}

fn get_input_mode(any win, any mode) int {
   "Returns a cached input mode for backends without a native getter."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(!is_dict(native)){ return 0 }
   if(b == "x11"){ return x11_backend.get_input_mode(native, mode) }
   if(b == "wayland"){ return wayland_backend.get_input_mode(native, mode) }
   if(mode == api.CURSOR){ return int(native.get("cursor_mode", api.CURSOR_NORMAL)) }
   if(mode == api.RAW_MOUSE_MOTION){ return native.get("raw_mouse_motion", false) ? 1 : 0 }
   if(mode == api.STICKY_KEYS){ return native.get("sticky_keys", false) ? 1 : 0 }
   if(mode == api.STICKY_MOUSE_BUTTONS){ return native.get("sticky_mouse", false) ? 1 : 0 }
   0
}

fn set_title(any win, str title) any {
   "Sets the native window title."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.set_title(native, title) }
   elif(b == "wayland"){ wayland_backend.set_title(native, title) }
   elif(b == "win32"){ win32_impl.set_title(native, title) }
   elif(b == "cocoa"){ cocoa_impl.set_title(native, title) }
}

fn set_window_title(any win, str title) any { set_title(win, title) }

fn get_window_title(any win) str {
   "Returns the window title."
   def native = _native_window(win)
   if(!is_dict(native)){ return "Untitled" }
   native.get("title", "Untitled")
}

comptime emit _dispatch_native_get0_list(get_size, x11_backend.get_size, wayland_backend.get_size, win32_impl.get_size, cocoa_impl.get_size)

fn get_window_size(any win) list { get_size(win) }

fn get_framebuffer_size(any win) list {
   "Returns the framebuffer size after content-scale expansion."
   def sz, sc = get_size(win), get_window_content_scale(win)
   def w, h = int(float(sz.get(0, 0)) * float(sc.get(0, 1.0))), int(float(sz.get(1, 0)) * float(sc.get(1, 1.0)))
   [w, h]
}

fn set_size(any win, int w, int h) any {
   "Sets the native window client size."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.set_size(native, w, h) }
   elif(b == "wayland"){ wayland_backend.set_size(native, w, h) }
   elif(b == "win32"){ win32_impl.set_size(native, w, h) }
   elif(b == "cocoa"){ cocoa_impl.set_size(native, w, h) }
}

fn set_window_size(any win, int w, int h) any { set_size(win, w, h) }

fn focus_window(any win) any {
   "Requests keyboard focus for a native window."
   def b = _select_backend_name()
   mut native = _native_window(win)
   if(b == "x11"){
      x11_backend.focus_window(native)
      if(is_dict(native)){
         def handle = native.get("handle", 0)
         if(handle){
            native["focused"] = true
            def native_windows = _get_platform_val("native_windows", dict(8))
            native_windows[handle] = native
            _set_platform_val("native_windows", native_windows)
         }
      }
   }
   elif(b == "win32"){ win32_impl.focus_window(native) }
   elif(b == "cocoa"){ cocoa_impl.focus_window(native) }
}

fn apply_hints(any hints) any {
   "Merges window hints into the current hint set."
   _dbg("apply_hints: count=" + to_str(hints.len) + " values=" + to_str(hints))
   def current_hints = _get_platform_val("window_hints", dict(8))
   _set_platform_val("window_hints", dict_merge(current_hints, hints))
}

fn post_empty_event(any win=0) any {
   "Posts or flushes a wake event for the platform loop."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.post_empty_event(common.value_or(native, win)) }
   elif(b == "wayland"){ wayland_backend.flush(0) }
   elif(b == "cocoa"){ cocoa_impl.post_event(0) }
}

fn get_key_name(int key, int scancode) str {
   "Returns the platform display name for a key/scancode pair."
   def b = _select_backend_name()
   def native_windows = _get_platform_val("native_windows", dict(8))
   def windows = dict_values(native_windows)
   if(windows.len == 0){ return "" }
   def win = windows.get(0)
   if(b == "x11"){ x11_backend.get_key_name(win, key, scancode) }
   elif(b == "wayland"){ wayland_backend.get_key_name(win, key, scancode) }
   elif(b == "win32"){ win32_impl.get_key_name(win, key, scancode) }
   elif(b == "cocoa"){ cocoa_impl.get_key_name(win, key, scancode) }
   else { "" }
}

fn get_key_scancode(int key) int {
   "Returns the platform scancode for a key."
   def b = _select_backend_name()
   def native_windows = _get_platform_val("native_windows", dict(8))
   def windows = dict_values(native_windows)
   if(windows.len == 0){ return -1 }
   def win = windows.get(0)
   if(b == "x11"){ x11_backend.get_key_scancode(win, key) }
   elif(b == "cocoa"){ key }
   else { -1 }
}

comptime template _dispatch_backend_ret0(name, fallback, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}() any {
      def b = _select_backend_name()
      if(b == "x11"){ x11_fn() }
      elif(b == "wayland"){ wayland_fn() }
      elif(b == "win32"){ win32_fn() }
      elif(b == "cocoa"){ cocoa_fn() }
      else { fallback }
   }
}

comptime template _dispatch_backend_ret1(name, fallback, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any arg0) any {
      def b = _select_backend_name()
      if(b == "x11"){ x11_fn(arg0) }
      elif(b == "wayland"){ wayland_fn(arg0) }
      elif(b == "win32"){ win32_fn(arg0) }
      elif(b == "cocoa"){ cocoa_fn(arg0) }
      else { fallback }
   }
}

comptime template _dispatch_backend_ret3(name, fallback, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any arg0, any arg1, any arg2) any {
      def b = _select_backend_name()
      if(b == "x11"){ x11_fn(arg0, arg1, arg2) }
      elif(b == "wayland"){ wayland_fn(arg0, arg1, arg2) }
      elif(b == "win32"){ win32_fn(arg0, arg1, arg2) }
      elif(b == "cocoa"){ cocoa_fn(arg0, arg1, arg2) }
      else { fallback }
   }
}

comptime template _dispatch_backend_void1(name, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any arg0) any {
      def b = _select_backend_name()
      if(b == "x11"){ x11_fn(arg0) }
      elif(b == "wayland"){ wayland_fn(arg0) }
      elif(b == "win32"){ win32_fn(arg0) }
      elif(b == "cocoa"){ cocoa_fn(arg0) }
   }
}

comptime template _dispatch_backend_void2(name, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any arg0, any arg1) any {
      def b = _select_backend_name()
      if(b == "x11"){ x11_fn(arg0, arg1) }
      elif(b == "wayland"){ wayland_fn(arg0, arg1) }
      elif(b == "win32"){ win32_fn(arg0, arg1) }
      elif(b == "cocoa"){ cocoa_fn(arg0, arg1) }
   }
}

comptime template _dispatch_native_void2(name, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any win, any arg0) any {
      def b = _select_backend_name()
      def native = _native_window(win)
      if(b == "x11"){ x11_fn(native, arg0) }
      elif(b == "wayland"){ wayland_fn(native, arg0) }
      elif(b == "win32"){ win32_fn(native, arg0) }
      elif(b == "cocoa"){ cocoa_fn(native, arg0) }
   }
}

def _FB_EMPTY_LIST = []
def _FB_ZERO = 0
def _FB_POS2 = [0, 0]
def _FB_RECT4 = [0, 0, 0, 0]
def _FB_SCALE2 = [1.0, 1.0]
comptime emit _dispatch_backend_ret0(get_monitors, _FB_EMPTY_LIST, x11_backend.get_monitors, wayland_backend.get_monitors, win32_impl.get_monitors, cocoa_impl.get_monitors)
comptime emit _dispatch_backend_ret0(get_primary_monitor, 0, x11_backend.get_primary_monitor, wayland_backend.get_primary_monitor, win32_impl.get_primary_monitor, cocoa_impl.get_primary_monitor)
comptime emit _dispatch_backend_ret1(get_monitor_pos, _FB_POS2, x11_backend.get_monitor_pos, wayland_backend.get_monitor_pos, win32_impl.get_monitor_pos, cocoa_impl.get_monitor_pos)
comptime emit _dispatch_backend_ret1(get_monitor_workarea, _FB_RECT4, x11_backend.get_monitor_workarea, wayland_backend.get_monitor_workarea, win32_impl.get_monitor_workarea, cocoa_impl.get_monitor_workarea)
comptime emit _dispatch_backend_ret1(get_monitor_physical_size, _FB_POS2, x11_backend.get_monitor_physical_size, wayland_backend.get_monitor_physical_size, win32_impl.get_monitor_physical_size, cocoa_impl.get_monitor_physical_size)
comptime emit _dispatch_backend_ret1(get_monitor_content_scale, _FB_SCALE2, x11_backend.get_monitor_content_scale, wayland_backend.get_monitor_content_scale, win32_impl.get_monitor_content_scale, cocoa_impl.get_monitor_content_scale)
comptime emit _dispatch_backend_ret1(get_monitor_name, "unknown", x11_backend.get_monitor_name, wayland_backend.get_monitor_name, win32_impl.get_monitor_name, cocoa_impl.get_monitor_name)

fn get_x11_monitor(any mon) any { x11_backend.get_x11_monitor(mon) }

fn get_x11_adapter(any mon) any { x11_backend.get_x11_adapter(mon) }

fn get_wayland_monitor(any mon) any { wayland_backend.get_wayland_monitor(mon) }

fn set_monitor_callback(any cb) any {
   "Installs a monitor callback and returns the previous callback."
   def old = _get_platform_val("monitor_callback", 0)
   _set_platform_val("monitor_callback", cb)
   old
}

comptime emit _dispatch_backend_ret1(get_video_mode, 0, x11_backend.get_video_mode, wayland_backend.get_video_mode, win32_impl.get_video_mode, cocoa_impl.get_video_mode)
comptime emit _dispatch_backend_ret1(get_video_modes, _FB_EMPTY_LIST, x11_backend.get_video_modes, wayland_backend.get_video_modes, win32_impl.get_video_modes, cocoa_impl.get_video_modes)
comptime emit _dispatch_backend_ret3(create_cursor, 0, x11_backend.create_cursor, wayland_backend.create_cursor, win32_impl.create_cursor, cocoa_impl.create_cursor)
comptime emit _dispatch_backend_ret1(create_standard_cursor, 0, x11_backend.create_standard_cursor, wayland_backend.create_standard_cursor, win32_impl.create_standard_cursor, cocoa_impl.create_standard_cursor)
comptime emit _dispatch_backend_void1(destroy_cursor, x11_backend.destroy_cursor, wayland_backend.destroy_cursor, win32_impl.destroy_cursor, cocoa_impl.destroy_cursor)
comptime emit _dispatch_native_void2(set_cursor, x11_backend.set_cursor, wayland_backend.set_cursor, win32_impl.set_cursor, cocoa_impl.set_cursor)
comptime emit _dispatch_backend_ret1(get_gamma_ramp, _FB_ZERO, x11_backend.get_gamma_ramp, wayland_backend.get_gamma_ramp, win32_impl.get_gamma_ramp, cocoa_impl.get_gamma_ramp)
comptime emit _dispatch_backend_void2(set_gamma_ramp, x11_backend.set_gamma_ramp, wayland_backend.set_gamma_ramp, win32_impl.set_gamma_ramp, cocoa_impl.set_gamma_ramp)

fn set_gamma(any mon, f64 gamma) any {
   "Builds and applies a gamma ramp for a monitor."
   def size = 256
   def ramp = list()
   mut i = 0
   while(i < size){
      def value = int(math.pow(i * 1.0 / (size - 1), 1.0 / gamma) * 65535.0 + 0.5)
      ramp.append([value, value, value])
      i += 1
   }
   set_gamma_ramp(mon, ramp)
}

fn get_x11_display() any {
   "Returns the cached X11 display, opening it on first use."
   if(_select_backend_name() != "x11"){ return 0 }
   def p = _get_platform_val("platform", dict(8))
   mut dpy = p.get("x11_display", 0)
   if(dpy == 0){
      dpy = x11_backend.open_display()
      p["x11_display"] = dpy
      _set_platform_val("platform", p)
   }
   dpy
}

fn swap_interval(int interval) any {
   "Runs the swap interval operation."
   def b = _select_backend_name()
   if(b == "x11" || b == "wayland"){ return opengl_backend.swap_interval(interval) }
   0
}

fn get_x11_window(any win) any {
   "Runs the get x11 window operation."
   _select_backend_name() == "x11" ? _native_handle(_native_window(win)) : 0
}

fn get_x11_selection_string(any win) str {
   "Returns the X11 primary selection text for a window."
   def b = _select_backend_name()
   if(b == "x11"){
      def native = _native_window(win)
      return _native_text_or_owned(
         x11_backend.get_primary_selection(native), native, "primary_owned", "primary_selection_string",
      )
   }
   ""
}

fn set_x11_selection_string(any win, str s) any {
   "Sets the X11 primary selection text for a window."
   def b = _select_backend_name()
   if(b == "x11"){
      def native = _native_window(win)
      native["primary_selection_string"] = s
      def ok = x11_backend.set_primary_selection(native, s)
      native["primary_owned"] = ok
      _store_updated_native_window(native)
      return ok
   }
   false
}

comptime template _native_set1_all(name, x11_fn, win32_fn, cocoa_fn){
   fn ${name}(any win, any arg0) any {
      def b = _select_backend_name()
      def native = _native_window(win)
      if(b == "x11"){ x11_fn(native, arg0) }
      elif(b == "win32"){ win32_fn(native, arg0) }
      elif(b == "cocoa"){ cocoa_fn(native, arg0) }
   }
}

fn _native_text_or_owned(str text, any native, str owned_key, str text_key) str {
   if(text.len > 0){ return text }
   if(is_dict(native) && native.get(owned_key, false)){ return native.get(text_key, "") }
   text
}

comptime template _native_get_list2_all(name, x11_fn, win32_fn, cocoa_fn){
   fn ${name}(any win) list {
      def b = _select_backend_name()
      def native = _native_window(win)
      if(b == "x11"){ return x11_fn(native) }
      elif(b == "win32"){ return win32_fn(native) }
      elif(b == "cocoa"){ return cocoa_fn(native) }
      elif(b == "none" && is_dict(native)){ return [native.get("x", 0), native.get("y", 0)] }
      [0, 0]
   }
}

comptime template _native_get_zero_all(name, x11_fn, win32_fn, cocoa_fn){
   fn ${name}(any win) any {
      def b = _select_backend_name()
      def native = _native_window(win)
      if(b == "x11"){ return x11_fn(native) }
      elif(b == "win32"){ return win32_fn(native) }
      elif(b == "cocoa"){ return cocoa_fn(native) }
      0
   }
}

comptime template _native_set2_all(name, x11_fn, win32_fn, cocoa_fn){
   fn ${name}(any win, int x, int y) any {
      def b = _select_backend_name()
      def native = _native_window(win)
      if(b == "x11"){ x11_fn(native, x, y) }
      elif(b == "win32"){ win32_fn(native, x, y) }
      elif(b == "cocoa"){ cocoa_fn(native, x, y) }
      elif(b == "none" && is_dict(native)){
         native["x"] = x
         native["y"] = y
         _store_updated_native_window(native)
         true
      } else { false }
   }
}

comptime emit _native_set1_all(set_window_decorated, x11_backend.set_window_decorated, win32_impl.set_window_decorated, cocoa_impl.set_window_decorated)
comptime emit _native_set1_all(set_window_floating, x11_backend.set_window_floating, win32_impl.set_window_floating, cocoa_impl.set_window_floating)
comptime emit _native_set1_all(set_window_resizable, x11_backend.set_window_resizable, win32_impl.set_window_resizable, cocoa_impl.set_window_resizable)

fn get_clipboard(any win) str {
   "Returns system clipboard text for the active native backend."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      return _native_text_or_owned(x11_backend.get_clipboard(native), native, "clipboard_owned", "clipboard_string")
   }
   elif(b == "wayland"){ return wayland_backend.get_clipboard(native) }
   elif(b == "win32"){ return win32_impl.get_clipboard(native) }
   elif(b == "cocoa"){ return cocoa_impl.get_clipboard(native) }
   ""
}

fn set_clipboard(any win, any text) any {
   "Sets system clipboard text for the active native backend."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      native["clipboard_string"] = to_str(text)
      def ok = x11_backend.set_clipboard(native, text)
      native["clipboard_owned"] = ok
      _store_updated_native_window(native)
      return ok
   }
   elif(b == "wayland"){
      def ok = wayland_backend.set_clipboard(native, text)
      _store_updated_native_window(native)
      return ok
   }
   elif(b == "win32"){ return win32_impl.set_clipboard(native, text) }
   elif(b == "cocoa"){ return cocoa_impl.set_clipboard(native, text) }
   false
}

comptime emit _native_get_list2_all(get_pos, x11_backend.get_pos, win32_impl.get_pos, cocoa_impl.get_pos)

fn get_window_pos(any win) list { get_pos(win) }
comptime emit _native_set2_all(set_pos, x11_backend.set_pos, win32_impl.set_pos, cocoa_impl.set_pos)

fn set_window_pos(any win, int x, int y) any { set_pos(win, x, y) }
comptime emit _native_get_zero_all(get_window_monitor, x11_backend.get_window_monitor, win32_impl.get_window_monitor, cocoa_impl.get_window_monitor)

fn set_window_monitor(any win, any mon, int x, int y, int w, int h, int refresh_rate) any {
   "Moves a window to a monitor or restores windowed placement."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){
      def updated = x11_backend.set_window_monitor(native, mon, x, y, w, h, refresh_rate)
      _store_updated_native_window(updated)
      return updated
   }
   if(b == "win32"){
      def updated = win32_impl.set_window_monitor(native, mon, x, y, w, h, refresh_rate)
      _store_updated_native_window(updated)
      return updated
   }
   if(b == "cocoa"){
      def updated = cocoa_impl.set_window_monitor(native, mon, x, y, w, h, refresh_rate)
      _store_updated_native_window(updated)
      return updated
   }
   native
}

fn _get_win_cbs(any win_handle) dict { _get_platform_val("window_callbacks", dict(8)).get(win_handle, dict(8)) }

fn _set_win_cb(any win, str name, any cb) any {
   def handle = _native_handle(win)
   if(!handle){ return 0 }
   mut window_callbacks = _get_platform_val("window_callbacks", dict(8))
   mut cbs = window_callbacks.get(handle, dict(8))
   cbs[name] = cb
   window_callbacks[handle] = cbs
   _set_platform_val("window_callbacks", window_callbacks)
   _set_platform_val("has_window_callbacks", true)
}

fn _normalize_scroll_component(f64 v) f64 {
   mut out = float(v)
   def a = math.abs(out)
   if(a >= 8.0){ return out / 15.0 }
   if(a >= 3.0){ return out / 3.0 }
   out
}

fn _wayland_scroll_gain() f64 {
   if(_wayland_scroll_gain_cache >= 0.0){ return _wayland_scroll_gain_cache }
   mut env_gain = common.env_trim("NY_UI_WAYLAND_SCROLL_GAIN")
   if(env_gain.len == 0){ env_gain = common.env_trim("NY_TERM_WAYLAND_SCROLL_GAIN") }
   mut gain = 8.0
   if(env_gain.len > 0){
      def gv = str.atof(env_gain)
      if(gv >= 1.0 && gv <= 64.0){ gain = gv }
   }
   _wayland_scroll_gain_cache = gain
   gain
}

fn _xwayland_scroll_fix_enabled() bool {
   if(_xwayland_scroll_fix_cache != -1){ return _xwayland_scroll_fix_cache == 1 }
   def disable_ui = common.env_truthy("NY_UI_DISABLE_XWAYLAND_SCROLL_FIX")
   def disable_term = common.env_truthy("NY_TERM_DISABLE_XWAYLAND_SCROLL_FIX")
   _xwayland_scroll_fix_cache = (common.env_present("WAYLAND_DISPLAY") && !disable_ui && !disable_term) ? 1 : 0
   _xwayland_scroll_fix_cache == 1
}

fn _wayland_scroll_fix_enabled() bool {
   if(_wayland_scroll_fix_cache != -1){ return _wayland_scroll_fix_cache == 1 }
   _wayland_scroll_fix_cache = (!common.env_truthy("NY_UI_DISABLE_WAYLAND_SCROLL_FIX") && !common.env_truthy("NY_TERM_DISABLE_WAYLAND_SCROLL_FIX")) ? 1 : 0
   _wayland_scroll_fix_cache == 1
}

fn _normalize_wayland_scroll_component(f64 v) f64 {
   mut out = float(v)
   def a = math.abs(out)
   if(a <= 0.0){ return out }
   def gain = _wayland_scroll_gain()
   if(a < 0.35){
      out = out * gain
      def ao = math.abs(out)
      if(ao < 0.35){ out = (out < 0.0) ? -0.35 : 0.35 }
      if(ao > 3.0){ out = (out < 0.0) ? -3.0 : 3.0 }
   }
   out
}

fn _normalize_scroll_data(any data) any {
   if(!is_dict(data)){ return data }
   def b = _select_backend_name()
   mut out = data
   if(b == "x11" && _xwayland_scroll_fix_enabled()){
      out["dx"] = _normalize_scroll_component(out.get("dx", 0.0))
      out["dy"] = _normalize_scroll_component(out.get("dy", 0.0))
      return out
   }
   if(b == "wayland" && _wayland_scroll_fix_enabled()){
      out["dx"] = _normalize_wayland_scroll_component(out.get("dx", 0.0))
      out["dy"] = _normalize_wayland_scroll_component(out.get("dy", 0.0))
   }
   out
}

fn _window_size_event_data(any data) any {
   if(is_dict(data)){
      mut out = data
      if(!out.contains("w") && out.contains("width")){ out["w"] = out.get("width", 0) }
      if(!out.contains("h") && out.contains("height")){ out["h"] = out.get("height", 0) }
      if(!out.contains("width")){ out["width"] = out.get("w", 0) }
      if(!out.contains("height")){ out["height"] = out.get("h", 0) }
      return out
   }
   if(is_list(data)){
      mut out = dict(4)
      def w = data.get(0, 0)
      def h = data.get(1, 0)
      out["w"] = w
      out["h"] = h
      out["width"] = w
      out["height"] = h
      return out
   }
   data
}

fn _normalize_native_event(any ev) any {
   if(event_type(ev) == EVENT_WINDOW_RESIZED){
      def data = _window_size_event_data(event_data(ev))
      return make_event(event_type(ev), event_window(ev), event_window_id(ev), data)
   }
   if(event_type(ev) != EVENT_MOUSE_SCROLL){ return ev }
   def data = event_data(ev)
   if(!is_dict(data)){ return ev }
   def norm = _normalize_scroll_data(data)
   make_event(event_type(ev), event_window(ev), event_window_id(ev), norm)
}

comptime emit _set_win_cb_wrap(set_key_callback, "key")
comptime emit _set_win_cb_wrap(set_mouse_button_callback, "mouse_button")
comptime emit _set_win_cb_wrap(set_scroll_callback, "scroll")
comptime emit _set_win_cb_wrap(set_cursor_pos_callback, "cursor_pos")
comptime emit _set_win_cb_wrap(set_cursor_enter_callback, "cursor_enter")
comptime emit _set_win_cb_wrap(set_drop_callback, "drop")

fn set_joystick_callback(any cb) any {
   "Installs a joystick callback and returns the previous callback."
   def old = _get_platform_val("joystick_callback", 0)
   _set_platform_val("joystick_callback", cb)
   #linux { linux_joystick.set_joystick_callback(cb) }
   #windows { win32_joystick.set_joystick_callback(cb) }
   #macos { cocoa_joystick.set_joystick_callback(cb) }
   old
}

comptime emit _set_win_cb_wrap(set_window_size_callback, "window_size")
comptime emit _set_win_cb_wrap(set_close_callback, "close")
comptime emit _set_win_cb_wrap(set_char_callback, "char")
comptime emit _set_win_cb_wrap(set_char_mods_callback, "char_mods")

fn _dispatch_key_event_callbacks(int typ, any win_handle, any cbs, any data) bool {
   if(typ == EVENT_KEY_PRESSED){
      def cb = cbs.get("key", 0)
      if(cb){
         def key = data.get("key", 0)
         def sc = data.get("scancode", 0)
         def action = data.get("action", 0)
         def mods = data.get("mod", 0)
         cb(win_handle, key, sc, action, mods)
      }
      def ccb = cbs.get("char", 0)
      if(ccb && is_dict(data)){
         def cp = data.get("char", -1)
         if(cp >= 0){ ccb(win_handle, cp) }
      }
   } elif(typ == EVENT_KEY_RELEASED){
      def cb = cbs.get("key", 0)
      if(cb){
         def key = data.get("key", 0)
         def sc = data.get("scancode", 0)
         def mods = data.get("mod", 0)
         cb(win_handle, key, sc, api.ACTION_RELEASE, mods)
      }
   } elif(typ == EVENT_KEY_CHAR){
      def cb = cbs.get("char", 0)
      if(cb && is_dict(data)){
         def cp = data.get("char", -1)
         if(cp >= 0){ cb(win_handle, cp) }
      }
   } else { return false }
   true
}

fn _dispatch_pointer_event_callbacks(int typ, any win_handle, any cbs, any data) bool {
   if(typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED){
      def cb = cbs.get("mouse_button", 0)
      if(cb && is_dict(data)){
         def btn = data.get("button", 0)
         def action = (typ == EVENT_MOUSE_BUTTON_PRESSED) ? api.ACTION_PRESS : api.ACTION_RELEASE
         def mods = data.get("mod", 0)
         cb(win_handle, btn, action, mods)
      }
   } elif(typ == EVENT_MOUSE_POS_CHANGED){
      def cb = cbs.get("cursor_pos", 0)
      if(cb && is_dict(data)){
         def x, y = data.get("x", 0), data.get("y", 0)
         cb(win_handle, x, y)
      }
   } elif(typ == EVENT_MOUSE_SCROLL){
      def cb = cbs.get("scroll", 0)
      if(cb && is_dict(data)){
         def sx, sy = data.get("dx", 0.0), data.get("dy", 0.0)
         cb(win_handle, sx, sy)
      }
   } elif(typ == EVENT_MOUSE_ENTER || typ == EVENT_MOUSE_LEAVE){
      def cb = cbs.get("cursor_enter", 0)
      if(cb){ cb(win_handle, (typ == EVENT_MOUSE_ENTER) ? 1 : 0) }
   } else { return false }
   true
}

fn _dispatch_window_event_callbacks(int typ, any win_handle, any cbs, any data) bool {
   if(typ == EVENT_WINDOW_RESIZED){
      def cb = cbs.get("window_size", 0)
      def fcb = cbs.get("fbsize", 0)
      def size_data = _window_size_event_data(data)
      if(is_dict(size_data)){
         def w, h = size_data.get("w", 0), size_data.get("h", 0)
         if(cb){ cb(win_handle, w, h) }
         if(fcb){ fcb(win_handle, w, h) }
      }
   } elif(typ == EVENT_QUIT){
      def cb = cbs.get("close", 0)
      if(cb){ cb(win_handle) }
   } elif(typ == EVENT_DATA_DROP){
      def cb = cbs.get("drop", 0)
      if(cb && is_dict(data)){
         def paths = data.get("paths", [])
         cb(win_handle, paths)
      }
   } elif(typ == EVENT_WINDOW_MOVED){
      def cb = cbs.get("pos", 0)
      if(cb && is_dict(data)){
         def x, y = data.get("x", 0), data.get("y", 0)
         cb(win_handle, x, y)
      }
   } elif(typ == EVENT_WINDOW_MAXIMIZED){
      def cb = cbs.get("maximize", 0)
      if(cb){ cb(win_handle, 1) }
   } elif(typ == EVENT_WINDOW_MINIMIZED){
      def cb = cbs.get("iconify", 0)
      if(cb){ cb(win_handle, 1) }
   } elif(typ == EVENT_WINDOW_RESTORED){
      def icb = cbs.get("iconify", 0)
      if(icb){ icb(win_handle, 0) }
      def mcb = cbs.get("maximize", 0)
      if(mcb){ mcb(win_handle, 0) }
   } elif(typ == EVENT_FOCUS_IN || typ == EVENT_FOCUS_OUT){
      def cb = cbs.get("focus", 0)
      if(cb){ cb(win_handle, (typ == EVENT_FOCUS_IN) ? 1 : 0) }
   } elif(typ == EVENT_WINDOW_REFRESH){
      def cb = cbs.get("refresh", 0)
      if(cb){ cb(win_handle) }
   } elif(typ == EVENT_SCALE_UPDATED){
      def cb = cbs.get("scale", 0)
      if(cb && is_dict(data)){
         mut sx, sy = data.get("xscale", 0.0), data.get("yscale", 0.0)
         if(sx <= 0.0){ sx = data.get("x", 1.0) }
         if(sy <= 0.0){ sy = data.get("y", 1.0) }
         cb(win_handle, sx, sy)
      }
   } else { return false }
   true
}

fn _dispatch_monitor_event_callbacks(int typ, any data) bool {
   if(typ == EVENT_MONITOR_CONNECTED || typ == EVENT_MONITOR_DISCONNECTED){
      def monitor_callback = _get_platform_val("monitor_callback", 0)
      if(monitor_callback){
         def monitor = data
         def event = (typ == EVENT_MONITOR_CONNECTED) ? 0x00040001 : 0x00040002
         monitor_callback(monitor, event)
      }
   } else { return false }
   true
}

fn _dispatch_event_callbacks(any ev) any {
   "Dispatch a single event to any registered window callbacks."
   def typ = event_type(ev)
   def win_handle = event_window_id(ev)
   if(!win_handle){ return 0 }
   def cbs = _get_win_cbs(win_handle)
   if(!is_dict(cbs)){ return 0 }
   def data = event_data(ev)
   if(_dispatch_key_event_callbacks(typ, win_handle, cbs, data)){ return 0 }
   if(_dispatch_pointer_event_callbacks(typ, win_handle, cbs, data)){ return 0 }
   if(_dispatch_window_event_callbacks(typ, win_handle, cbs, data)){ return 0 }
   if(_dispatch_monitor_event_callbacks(typ, data)){ return 0 }
}

fn request_window_attention(any win) any {
   "Runs the request window attention operation."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.request_window_attention(native) }
   elif(b == "win32"){ win32_impl.request_window_attention(native) }
   elif(b == "cocoa"){ cocoa_impl.request_user_attention() }
}

fn get_window_user_pointer(any win) any {
   "Returns the window user pointer."
   def handle = _native_handle(win)
   if(handle){ return _get_platform_val("window_user_pointers", dict(8)).get(handle, 0) }
   if(is_dict(win)){ return win.get("user_pointer", 0) }
   0
}

fn set_window_user_pointer(any win, any user_ptr) any {
   "Stores a user pointer for a window."
   def handle = _native_handle(win)
   if(handle){
      mut ptrs = _get_platform_val("window_user_pointers", dict(8))
      ptrs[handle] = user_ptr
      _set_platform_val("window_user_pointers", ptrs)
   }
   if(is_dict(win)){
      mut out = win
      out["user_pointer"] = user_ptr
      return out
   }
   win
}

fn get_monitor_user_pointer(any mon) any {
   "Returns the monitor user pointer."
   if(is_dict(mon)){ return mon.get("user_pointer", 0) }
   0
}

fn set_monitor_user_pointer(any mon, any user_ptr) any {
   "Stores a user pointer for a monitor."
   if(is_dict(mon)){
      mut out = mon
      out["user_pointer"] = user_ptr
      return out
   }
   mon
}

fn set_window_opacity(any win, f64 op) any {
   "Sets platform window opacity when supported."
   def b = _select_backend_name()
   def native = _native_window(win)
   _dbg("set_window_opacity: backend=" + b + " win=0x" + str.to_hex(_native_handle(win)) + " opacity=" + to_str(op))
   if(b == "x11"){ x11_backend.set_window_opacity(native, op) }
   elif(b == "wayland"){ wayland_backend.set_window_opacity(native, op) }
   elif(b == "win32"){ win32_impl.set_window_opacity(native, op) }
   elif(b == "cocoa"){ cocoa_impl.set_window_opacity(native, op) }
}

comptime template _dispatch_native_window_action(name, label, x11_fn, wayland_fn, win32_fn, cocoa_fn){
   fn ${name}(any win) any {
      def b = _select_backend_name()
      def native = _native_window(win)
      _dbg(label + ": backend=" + b + " win=0x" + str.to_hex(_native_handle(win)))
      if(b == "x11"){ x11_fn(native) }
      elif(b == "wayland"){ wayland_fn(native) }
      elif(b == "win32"){ win32_fn(native) }
      elif(b == "cocoa"){ cocoa_fn(native) }
   }
}

comptime emit _dispatch_native_window_action(show_window, "show_window", x11_backend.show_window, wayland_backend.show_window, win32_impl.show_window, cocoa_impl.show_window)
comptime emit _dispatch_native_window_action(hide_window, "hide_window", x11_backend.hide_window, wayland_backend.hide_window, win32_impl.hide_window, cocoa_impl.hide_window)
comptime emit _dispatch_native_window_action(iconify_window, "iconify_window", x11_backend.iconify_window, wayland_backend.iconify_window, win32_impl.iconify_window, cocoa_impl.iconify_window)
comptime emit _dispatch_native_window_action(restore_window, "restore_window", x11_backend.restore_window, wayland_backend.restore_window, win32_impl.restore_window, cocoa_impl.restore_window)
comptime emit _dispatch_native_window_action(maximize_window, "maximize_window", x11_backend.maximize_window, wayland_backend.maximize_window, win32_impl.maximize_window, cocoa_impl.maximize_window)

fn _platform_vulkan_get_surface_capabilities(any phys, any surf, any caps) int {
   "Queries Vulkan surface capabilities for the active backend."
   def b = _select_backend_name()
   if(b == "x11"){ return x11_backend.vulkan_get_surface_capabilities(phys, surf, caps) }
   1
}

fn set_window_icon(any win, any images) any {
   "Sets native window icon images when supported."
   def b = _select_backend_name()
   def native = _native_window(win)
   if(b == "x11"){ x11_backend.set_window_icon(native, images) }
   elif(b == "wayland"){ wayland_backend.set_window_icon(native, images) }
   elif(b == "win32"){ win32_impl.set_window_icon(native, images) }
   elif(b == "cocoa"){ cocoa_impl.set_window_icon(native, images) }
}

fn get_surface_capabilities(any phys, any surf, any caps) int { "Alias for Vulkan surface capability queries." _platform_vulkan_get_surface_capabilities(phys, surf, caps) }

fn xdnd_begin_drag(any win, any data) any { "Begins an Xdnd drag operation when implemented." 0 }

fn _handle_xdnd_status(any win, any data) any { 0 }

fn _handle_xdnd_finished(any win, any data) any { 0 }

fn set_video_mode(any mon, any mode) any { "Sets a monitor video mode when supported." 0 }

fn restore_video_mode(any mon) any { "Restores the previous monitor video mode when supported." 0 }

fn get_surface_support(any phys, int family, any surf, any support_out) int {
   "Queries Vulkan queue-family surface support."
   def b = _select_backend_name()
   if(b == "x11"){ return x11_backend.vulkan_get_surface_support(phys, family, surf, support_out) }
   1
}

fn joystick_present(int jid) bool {
   "Returns true when the joystick slot is connected."
   #linux { return linux_joystick.joystick_present(jid) }
   #windows { return win32_joystick.joystick_present(jid) }
   #macos { return cocoa_joystick.joystick_present(jid) }
   false
}

fn get_joystick_axes(int jid, any count_out) any {
   "Returns raw joystick axes and writes the axis count."
   #linux { return linux_joystick.get_joystick_axes(jid, count_out) }
   #windows { return win32_joystick.get_joystick_axes(jid, count_out) }
   #macos { return cocoa_joystick.get_joystick_axes(jid, count_out) }
   if(count_out){ store32(count_out, 0, 0) }
   0
}

fn get_joystick_buttons(int jid, any count_out) any {
   "Returns raw joystick buttons and writes the button count."
   #linux { return linux_joystick.get_joystick_buttons(jid, count_out) }
   #windows { return win32_joystick.get_joystick_buttons(jid, count_out) }
   #macos { return cocoa_joystick.get_joystick_buttons(jid, count_out) }
   if(count_out){ store32(count_out, 0, 0) }
   0
}

fn get_joystick_hats(int jid, any count_out) any {
   "Returns joystick hats and writes the hat count when supported."
   #linux { return linux_joystick.get_joystick_hats(jid, count_out) }
   #windows { return win32_joystick.get_joystick_hats(jid, count_out) }
   if(count_out){ store32(count_out, 0, 0) }
   0
}

fn get_joystick_name(int jid) str {
   "Returns the raw joystick device name."
   #linux { return linux_joystick.get_joystick_name(jid) }
   #windows { return win32_joystick.get_joystick_name(jid) }
   #macos { return cocoa_joystick.get_joystick_name(jid) }
   "Unknown"
}

fn get_joystick_guid(int jid) str {
   "Returns the SDL-style joystick GUID."
   #linux { return linux_joystick.get_joystick_guid(jid) }
   #windows { return win32_joystick.get_joystick_guid(jid) }
   #macos { return cocoa_joystick.get_joystick_guid(jid) }
   "00000000000000000000000000000000"
}

fn joystick_is_gamepad(int jid) bool {
   "Returns true when the joystick has a gamepad mapping."
   #linux { return linux_joystick.joystick_is_gamepad(jid) }
   #windows { return win32_joystick.joystick_is_gamepad(jid) }
   #macos { return cocoa_joystick.joystick_is_gamepad(jid) }
   false
}

fn get_gamepad_name(int jid) str {
   "Returns mapped gamepad name when available."
   #linux { return linux_joystick.get_gamepad_name(jid) }
   #windows { return win32_joystick.get_gamepad_name(jid) }
   #macos { return cocoa_joystick.get_gamepad_name(jid) }
   "Unknown"
}

fn get_gamepad_state(int jid, any state_out) bool {
   "Writes mapped gamepad state for the joystick."
   #linux { return linux_joystick.get_gamepad_state(jid, state_out) }
   #windows { return win32_joystick.get_gamepad_state(jid, state_out) }
   #macos { return cocoa_joystick.get_gamepad_state(jid, state_out) }
   if(state_out){ memset(state_out, 0, 64) }
   false
}

fn update_gamepad_mappings(str mappings) bool {
   "Adds SDL-style gamepad mapping data."
   #linux { return linux_joystick.update_gamepad_mappings(mappings) != 0 }
   #windows { return win32_joystick.update_gamepad_mappings(mappings) != 0 }
   #macos { return cocoa_joystick.update_gamepad_mappings(mappings) != 0 }
   false
}

fn get_wayland_display() any { "Returns the cached Wayland display handle." _get_platform_val("platform", dict(8)).get("wayland_display", 0) }

fn get_wayland_window(any win) any {
   "Returns the native Wayland surface for a window."
   def native = _native_window(win)
   if(!is_dict(native)){ return 0 }
   native.get("surface", 0)
}

fn _window_ctx_by_handle(any win_handle) any { _get_platform_val("window_contexts", dict(8)).get(win_handle, 0) }

fn _window_ctx(any win) any { _window_ctx_by_handle(_native_handle(win)) }

fn _window_ctx_field(any win, str field) any {
   def ctx = _window_ctx(win)
   if(!ctx){ return 0 }
   ctx.get(field, 0)
}

fn _window_ctx_typed_field(any win, str typ, str field) any {
   def ctx = _window_ctx(win)
   if(!ctx){ return 0 }
   if(ctx.get("type", "") != typ){ return 0 }
   ctx.get(field, 0)
}

fn get_glx_context(any win) any {
   "Returns the GLX context for a window when available."
   _window_ctx_typed_field(win, "glx_offscreen", "context")
}

fn get_glx_window(any win) any { "Returns the GLX drawable handle for a window." _native_handle(win) }

fn get_egl_display() any {
   "Returns the EGL display for the current context."
   def ctx = get_current_context()
   if(!ctx){ return 0 }
   def c = _window_ctx_by_handle(ctx)
   c.get("display", 0)
}

fn get_egl_context(any win) any {
   "Returns the EGL context for a window when available."
   _window_ctx_typed_field(win, "egl_offscreen", "context")
}

fn get_egl_surface(any win) any {
   "Returns the EGL surface for a window when available."
   _window_ctx_field(win, "surface")
}

fn get_egl_config(any win) any {
   "Returns the EGL config for a window when available."
   _window_ctx_field(win, "config")
}

fn get_osmesa_context(any win) any {
   "Returns the OSMesa context for a window when available."
   _window_ctx_typed_field(win, "osmesa", "context")
}

fn get_osmesa_color_buffer(any win) any {
   "Returns the OSMesa color buffer pointer for a window."
   _window_ctx_field(win, "buffer")
}

fn get_osmesa_depth_buffer(any win) any {
   "Returns the OSMesa depth buffer pointer when available."
   def ctx = _window_ctx(win)
   if(!ctx){ return 0 }
   0
}

fn get_joystick_user_pointer(int jid) any { "Returns the joystick user pointer." _get_platform_val("joystick_user_pointers", dict(8)).get(jid, 0) }

fn set_joystick_user_pointer(int jid, any user_ptr) any {
   "Stores a user pointer for a joystick."
   mut ptrs = _get_platform_val("joystick_user_pointers", dict(8))
   ptrs[jid] = user_ptr
   _set_platform_val("joystick_user_pointers", ptrs)
}

comptime template _native_window_handle_accessor(name, backend_name){
   fn ${name}(any win) any {
      if(_select_backend_name() != backend_name){ return 0 }
      def native = _native_window(win)
      if(!is_dict(native)){ return native }
      native.get("handle", 0)
   }
}

comptime template _monitor_handle_accessor(name, backend_name){
   fn ${name}(any mon) any {
      if(_select_backend_name() != backend_name){ return 0 }
      if(!is_dict(mon)){ return 0 }
      mon.get("handle", 0)
   }
}

comptime emit _native_window_handle_accessor(get_win32_window, "win32")

fn get_win32_adapter(any mon) any {
   "Returns the Win32 adapter handle for a monitor."
   if(_select_backend_name() != "win32"){ return 0 }
   win32_impl.get_win32_adapter(mon)
}

comptime emit _monitor_handle_accessor(get_win32_monitor, "win32")

fn get_wgl_context(any win) any { "Returns the WGL context when available." 0 }
comptime emit _native_window_handle_accessor(get_cocoa_window, "cocoa")

fn get_nsgl_context(any win) any { "Returns the NSGL context when available." 0 }
comptime emit _monitor_handle_accessor(get_cocoa_monitor, "cocoa")

fn get_cocoa_view(any win) any {
   "Returns the Cocoa view for a window."
   if(_select_backend_name() != "cocoa"){ return 0 }
   def native = _native_window(win)
   cocoa_impl.get_cocoa_view(native)
}

#main {
   init_hint(PLATFORM, PLATFORM_NULL)
   assert(get_backend_name() == "none" && uses_native_events() && supports_state_polling(), "platform null backend")
   assert(get_version() == [3, 5, 0] && get_version_string().len > 0, "platform version")
   assert(get_timer_frequency() == 1000000000 && get_timer_value() >= 0 && get_time() >= 0.0, "platform timer")
   default_window_hints()
   assert(get_window_attrib(0, api.RESIZABLE) == api.TRUE, "platform default window hint")
   window_hint(api.RESIZABLE, api.FALSE)
   window_hint(api.CLIENT_API, api.NO_API)
   assert(get_window_attrib(0, api.RESIZABLE) == api.FALSE, "platform updated window hint")
   assert(set_error_callback(0) == 0 && get_error() == 0, "platform error callback")
   def win = create_window("probe", 1, 2, 64, 48, 0)
   assert(win > 0 && get_window_title(win) == "probe", "platform headless window")
   assert(get_window_size(win) == [0, 0] && get_framebuffer_size(win) == [0, 0], "platform null window size fallback")
   assert(get_window_frame_size(win) == [0, 0, 0, 0] && get_window_content_scale(win) == [1.0, 1.0], "platform null window frame")
   assert(get_window_opacity(win) == 1.0 && get_cursor_pos(win) == [0, 0], "platform null cursor and opacity")
   set_should_close(win, true)
   assert(should_close(win), "platform close flag set")
   set_should_close(win, false)
   assert(!should_close(win), "platform close flag cleared")
   assert(get_key_name(api.KEY_A, 0) == "" && get_key_scancode(api.KEY_A) == -1 && get_key(win, api.KEY_A) == 0, "platform key fallback")
   assert(!set_cursor_pos(win, 3, 4) && get_input_mode(win, api.CURSOR) == api.CURSOR_NORMAL && !raw_mouse_motion_supported(), "platform input fallback")
   assert(poll_events_for_window(win) == [] && pump_window_events(win) == [], "platform event fallback")
   assert(!vulkan_supported() && required_extensions() == [], "platform vulkan fallback")
   assert(create_surface(0, win, 0, 0) == 1 && get_surface_support(0, 0, 0, 0) == 1 && get_surface_capabilities(0, 0, 0) == 1, "platform surface fallback")
   assert(set_window_user_pointer(win, 1234) == win && get_window_user_pointer(win) == 1234, "platform window user pointer")
   def mon = set_monitor_user_pointer({"handle": 7}, 55)
   assert(get_monitor_user_pointer(mon) == 55, "platform monitor user pointer")
   set_joystick_user_pointer(0, 42)
   assert(get_joystick_user_pointer(0) == 42, "platform joystick user pointer")
   assert(get_wayland_display() == 0 && get_win32_window(win) == 0 && get_cocoa_window(win) == 0 && get_egl_display() == 0, "platform native handle fallback")
   destroy_window(win)
   assert(!should_close(win), "platform destroy clears close flag")
   print("✓ std.os.ui.window.platform self-test passed")
}
