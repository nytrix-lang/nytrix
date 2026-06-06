;; Keywords: window native native-windowing os ui input
;; Native window backend — platform-delegating API layer.
;; References:
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.native(
   init, terminate, create_window, destroy_window, should_close, set_should_close, set_title, get_pos,
   set_pos, get_size, get_framebuffer_size, set_size, get_window_attrib, set_window_opacity, poll_events,
   swap_buffers, swap_interval, get_key, get_mouse_button, get_cursor_pos, set_key_callback,
   set_mouse_button_callback, set_scroll_callback, set_cursor_pos_callback, set_window_size_callback,
   set_close_callback, set_char_callback, set_input_mode, vulkan_supported, required_extensions,
   create_surface, get_instance_proc_address, make_context_current, get_current_context,
   extension_supported, get_osmesa_context, get_osmesa_color_buffer, get_osmesa_depth_buffer,
   get_win32_window, get_win32_adapter, get_win32_monitor, get_wgl_context, get_cocoa_window,
   get_cocoa_monitor, get_cocoa_view, get_nsgl_context, apply_hints, focus_window, set_clipboard,
   get_clipboard, joystick_present, get_joystick_name, get_joystick_guid, get_joystick_axes,
   get_joystick_buttons, get_joystick_hats, joystick_is_gamepad, get_gamepad_state, get_gamepad_name,
   set_joystick_callback, update_gamepad_mappings, JOYSTICK_1, JOYSTICK_2, JOYSTICK_3, JOYSTICK_4,
   JOYSTICK_5, JOYSTICK_6, JOYSTICK_7, JOYSTICK_8, JOYSTICK_9, JOYSTICK_10, JOYSTICK_11, JOYSTICK_12,
   JOYSTICK_13, JOYSTICK_14, JOYSTICK_15, JOYSTICK_16, JOYSTICK_LAST, GAMEPAD_BUTTON_A, GAMEPAD_BUTTON_B,
   GAMEPAD_BUTTON_X, GAMEPAD_BUTTON_Y, GAMEPAD_BUTTON_LEFT_BUMPER, GAMEPAD_BUTTON_RIGHT_BUMPER,
   GAMEPAD_BUTTON_BACK, GAMEPAD_BUTTON_START, GAMEPAD_BUTTON_GUIDE, GAMEPAD_BUTTON_LEFT_THUMB,
   GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_DPAD_UP, GAMEPAD_BUTTON_DPAD_RIGHT,
   GAMEPAD_BUTTON_DPAD_DOWN, GAMEPAD_BUTTON_DPAD_LEFT, GAMEPAD_BUTTON_LAST, GAMEPAD_BUTTON_CROSS,
   GAMEPAD_BUTTON_CIRCLE, GAMEPAD_BUTTON_SQUARE, GAMEPAD_BUTTON_TRIANGLE, GAMEPAD_AXIS_LEFT_X,
   GAMEPAD_AXIS_LEFT_Y, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y, GAMEPAD_AXIS_LEFT_TRIGGER,
   GAMEPAD_AXIS_RIGHT_TRIGGER, GAMEPAD_AXIS_LAST, KEY_SPACE, KEY_APOSTROPHE, KEY_COMMA, KEY_MINUS,
   KEY_PERIOD, KEY_SLASH, KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
   KEY_SEMICOLON, KEY_EQUAL, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K,
   KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y,
   KEY_Z, KEY_LEFT_BRACKET, KEY_BACKSLASH, KEY_RIGHT_BRACKET, KEY_GRAVE_ACCENT, KEY_WORLD_1, KEY_WORLD_2,
   KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_INSERT, KEY_DELETE, KEY_RIGHT, KEY_LEFT, KEY_DOWN,
   KEY_UP, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END, KEY_CAPS_LOCK, KEY_SCROLL_LOCK, KEY_NUM_LOCK,
   KEY_PRINT_SCREEN, KEY_PAUSE, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
   KEY_F10, KEY_F11, KEY_F12, KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20,
   KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_F25, KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4,
   KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9, KEY_KP_DECIMAL, KEY_KP_DIVIDE, KEY_KP_MULTIPLY,
   KEY_KP_SUBTRACT, KEY_KP_ADD, KEY_KP_ENTER, KEY_KP_EQUAL, KEY_LEFT_SHIFT, KEY_LEFT_CONTROL,
   KEY_LEFT_ALT, KEY_LEFT_SUPER, KEY_RIGHT_SHIFT, KEY_RIGHT_CONTROL, KEY_RIGHT_ALT, KEY_RIGHT_SUPER,
   KEY_MENU, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_CAPS_LOCK, MOD_NUM_LOCK, MOUSE_BUTTON_1,
   MOUSE_BUTTON_2, MOUSE_BUTTON_3, MOUSE_BUTTON_4, MOUSE_BUTTON_5, MOUSE_BUTTON_6, MOUSE_BUTTON_7,
   MOUSE_BUTTON_8, MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE, CURSOR, STICKY_KEYS,
   STICKY_MOUSE_BUTTONS, LOCK_KEY_MODS,
   RAW_MOUSE_MOTION, CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_DISABLED, CURSOR_CAPTURED, CONNECTED,
   DISCONNECTED, CLIENT_API, NO_API, OPENGL_API, OPENGL_ES_API, RESIZABLE, VISIBLE, DECORATED, FOCUSED,
   AUTO_ICONIFY, FLOATING, MAXIMIZED, CENTER_CURSOR, TRANSPARENT_FRAMEBUFFER, FOCUS_ON_SHOW,
   CONTEXT_CREATION_API, NATIVE_CONTEXT_API, EGL_CONTEXT_API, OSMESA_CONTEXT_API, X11_CLASS_NAME,
   X11_INSTANCE_NAME, WAYLAND_APP_ID, PLATFORM, JOYSTICK_HAT_BUTTONS, WAYLAND_LIBDECOR,
   X11_XCB_VULKAN_SURFACE, COCOA_RETINA_FRAMEBUFFER, COCOA_FRAME_NAME, COCOA_GRAPHICS_SWITCHING,
   WIN32_KEYBOARD_MENU, WIN32_SHOWDEFAULT, get_backend_name
)

use std.core
use std.core.mem
use std.core.str (to_hex)
use std.core.common as common
use std.os.ui.render.dump as ui_profile
use std.os.ui.window.platform.api
use std.os.ui.window.platform as ui_backend

fn _is_debug() bool { ui_profile.debug_enabled() }

fn _dbg(any msg) any { if(_is_debug()){ ui_profile.print_text("[window:native] " + msg) } }
mut _title_buf = 0
mut _title_cap = 0
mut _ready = false

fn init() bool {
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

fn terminate() any {
   "Terminates the window system and frees resources."
   if(_ready){
      _dbg("terminate: backend=" + ui_backend.get_backend_name())
      ui_backend.terminate()
      _ready = false
   }
}

fn apply_hints(int flags) any {
   "Internal: Applies window hints based on Nytrix window flags."
   mut hints = dict(8)
   hints = hints.set(TRANSPARENT_FRAMEBUFFER, band(flags, 32) ? 1 : 0)
   hints = hints.set(DECORATED, band(flags, 1) ? 0 : 1)
   hints = hints.set(RESIZABLE, band(flags, 2) ? 0 : 1)
   hints = hints.set(FLOATING, band(flags, 4096) ? 1 : 0)
   hints = hints.set(MAXIMIZED, band(flags, 1024) ? 1 : 0)
   hints = hints.set(VISIBLE, band(flags, 512) ? 0 : 1)
   _dbg("apply_hints: flags=0x" + to_hex(flags) +
      " transparent=" + to_str(hints.get(TRANSPARENT_FRAMEBUFFER, 0)) +
      " decorated=" + to_str(hints.get(DECORATED, 0)) +
      " resizable=" + to_str(hints.get(RESIZABLE, 0)) +
      " floating=" + to_str(hints.get(FLOATING, 0)) +
      " maximized=" + to_str(hints.get(MAXIMIZED, 0)) +
   " visible=" + to_str(hints.get(VISIBLE, 0)))
   ui_backend.apply_hints(hints)
}

fn create_window(any title, int w, int h, int flags=0) any {
   "Creates a new window with Vulkan support and the specified hints."
   init()
   if(!title){ title = "nytrix" }
   _dbg("create_window: title='" + title + "' size=" + to_str(w) + "x" + to_str(h) + " flags=0x" + to_hex(flags))
   apply_hints(flags)
   def win = ui_backend.create_window(title, 0, 0, w, h, flags)
   _dbg("create_window: handle=0x" + to_hex(win))
   win
}

fn destroy_window(any win) any {
   "Destroys the specified window."
   if(win){
      _dbg("destroy_window: win=0x" + to_hex(win))
      ui_backend.destroy_window(win)
   }
}

fn should_close(any win) any {
   "Checks if the window's close flag is set."
   ui_backend.should_close(win)
}

fn set_should_close(any win, any v=true) any {
   "Sets or clears the window's close flag."
   ui_backend.set_should_close(win, !!v ? 1 : 0)
}

fn set_title(any win, any title) any {
   "Sets the title of the specified window."
   _dbg("set_title: win=0x" + to_hex(win) + " title='" + title + "'")
   ui_backend.set_title(win, title)
}

fn get_window_attrib(any win, int attrib) any {
   "Returns a window attribute such as TRANSPARENT_FRAMEBUFFER."
   def value = ui_backend.get_window_attrib(win, attrib)
   value
}

fn get_pos(any win) list {
   "Returns the window's screen position as [x, y]."
   ui_backend.get_pos(win)
}

fn set_pos(any win, int x, int y) any {
   "Moves the window to the specified screen position."
   _dbg("set_pos: win=0x" + to_hex(win) + " pos=" + to_str(x) + "," + to_str(y))
   ui_backend.set_pos(win, x, y)
}

@jit
fn poll_events() any {
   "Processes all pending window events."
   if(_ready){
      _dbg("poll_events")
      ui_backend.poll_events()
   }
}

@jit
fn swap_buffers(any win) any {
   "Swaps the front and back buffers for the specified window."
   _dbg("swap_buffers: win=0x" + to_hex(win))
   ui_backend.swap_buffers(win)
}

fn swap_interval(int n) any {
   "Sets the swap interval(vsync) for the current context."
   _dbg("swap_interval: interval=" + to_str(n))
   ui_backend.swap_interval(n)
}

fn get_size(any win) list {
   "Returns the window's client area size as [w, h]."
   ui_backend.get_size(win)
}

fn get_framebuffer_size(any win) list {
   "Returns the window's framebuffer size in pixels as [w, h]."
   ui_backend.get_framebuffer_size(win)
}

fn set_size(any win, int w, int h) any {
   "Sets the window's client area size."
   _dbg("set_size: win=0x" + to_hex(win) + " size=" + to_str(w) + "x" + to_str(h))
   ui_backend.set_size(win, w, h)
}

mut _cursor_xp = 0
mut _cursor_yp = 0

@jit
fn get_key(any win, int key) any {
   "Returns the current state of a physical key(ACTION_PRESS or ACTION_RELEASE)."
   ui_backend.get_key(win, key)
}

@jit
fn get_mouse_button(any win, int btn) any {
   "Returns the current state of a mouse button(ACTION_PRESS or ACTION_RELEASE)."
   ui_backend.get_mouse_button(win, btn)
}

fn get_cursor_pos(any win) list {
   "Returns the current mouse cursor position relative to the client area."
   ui_backend.get_cursor_pos(win)
}

fn set_cursor_pos(any win, any x, any y) any {
   "Moves the mouse cursor to the specified client area coordinates."
   _dbg("set_cursor_pos: win=0x" + to_hex(win) + " pos=" + to_str(x) + "," + to_str(y))
   ui_backend.set_cursor_pos(win, x, y)
}

fn set_key_callback(any win, any cb) any {
   "Sets the key event callback for the window."
   _dbg("set_key_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_key_callback(win, cb)
}

fn set_mouse_button_callback(any win, any cb) any {
   "Sets the mouse button event callback for the window."
   _dbg("set_mouse_button_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_mouse_button_callback(win, cb)
}

fn set_scroll_callback(any win, any cb) any {
   "Sets the mouse scroll event callback for the window."
   _dbg("set_scroll_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_scroll_callback(win, cb)
}

fn set_cursor_pos_callback(any win, any cb) any {
   "Sets the cursor position event callback for the window."
   _dbg("set_cursor_pos_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_cursor_pos_callback(win, cb)
}

fn set_window_size_callback(any win, any cb) any {
   "Sets the window resize event callback for the window."
   _dbg("set_window_size_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_window_size_callback(win, cb)
}

fn set_close_callback(any win, any cb) any {
   "Sets the window close event callback for the window."
   _dbg("set_close_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_close_callback(win, cb)
}

fn set_char_callback(any win, any cb) any {
   "Sets the character input event callback for the window."
   _dbg("set_char_callback: win=0x" + to_hex(win) + " cb=0x" + to_hex(cb))
   ui_backend.set_char_callback(win, cb)
}

fn vulkan_supported() bool {
   "Checks if the system supports Vulkan."
   def ok = ui_backend.vulkan_supported()
   _dbg("vulkan_supported: " + to_str(ok))
   ok
}

fn required_extensions() list {
   "Returns the Vulkan instance extensions required for surface creation."
   def exts = ui_backend.required_extensions()
   exts
}

fn create_surface(any instance, any win, any allocator, any surface) any {
   "Creates a Vulkan surface for the specified window."
   def res = ui_backend.create_surface(instance, win, allocator, surface)
   res
}

fn get_backend_name() str { ui_backend.get_backend_name() }

fn set_input_mode(any win, int mode, int value) any {
   "Configures window input modes(e.g., cursor visibility, sticky keys)."
   ui_backend.set_input_mode(win, mode, value)
}

fn focus_window(any win) any {
   "Brings the specified window to the foreground."
   if(win){
      _dbg("focus_window: win=0x" + to_hex(win))
      ui_backend.focus_window(win)
   }
}

fn set_window_opacity(any win, any v) any {
   "Sets whole-window opacity when supported by the platform."
   if(win){
      _dbg("set_window_opacity: win=0x" + to_hex(win) + " opacity=" + to_str(v))
      ui_backend.set_window_opacity(win, float(v))
   }
}

fn set_clipboard(any win, str s) any {
   "Sets the system clipboard content for the specified window context."
   if(win){
      _dbg("set_clipboard: win=0x" + to_hex(win) + " bytes=" + to_str(s.len))
      ui_backend.set_clipboard(win, s)
   }
}

fn get_clipboard(any win) str {
   "Retrieves the current system clipboard content."
   if(!win){ return "" }
   def s = ui_backend.get_clipboard(win)
   _dbg("get_clipboard: win=0x" + to_hex(win) + " bytes=" + to_str(s.len))
   s
}

fn joystick_present(int jid) bool {
   "Returns true if the specified joystick is present."
   ui_backend.joystick_present(jid)
}

fn get_joystick_name(int jid) str {
   "Returns the name of the specified joystick."
   ui_backend.get_joystick_name(jid)
}

fn get_joystick_guid(int jid) str {
   "Returns the SDL-compatible GUID of the specified joystick."
   ui_backend.get_joystick_guid(jid)
}

fn get_joystick_axes(int jid, any count_ptr) any {
   "Returns a pointer to the axis values of the specified joystick."
   ui_backend.get_joystick_axes(jid, count_ptr)
}

fn get_joystick_buttons(int jid, any count_ptr) any {
   "Returns a pointer to the button states of the specified joystick."
   ui_backend.get_joystick_buttons(jid, count_ptr)
}

fn get_joystick_hats(int jid, any count_ptr) any {
   "Returns a pointer to the hat states of the specified joystick."
   ui_backend.get_joystick_hats(jid, count_ptr)
}

fn joystick_is_gamepad(int jid) bool {
   "Returns true if the specified joystick has a gamepad mapping."
   ui_backend.joystick_is_gamepad(jid)
}

fn get_gamepad_state(int jid, any state_ptr) bool {
   "Retrieves the state of the specified joystick as a gamepad."
   ui_backend.get_gamepad_state(jid, state_ptr)
}

fn get_gamepad_name(int jid) str {
   "Returns the name of the gamepad mapping."
   ui_backend.get_gamepad_name(jid)
}

fn set_joystick_callback(any cb) any {
   "Sets the joystick connection callback."
   ui_backend.set_joystick_callback(cb)
}

fn update_gamepad_mappings(str s) any {
   "Updates the gamepad mappings from a string."
   ui_backend.update_gamepad_mappings(s)
}

fn make_context_current(any win) any {
   "Makes the specified window's context current on the calling thread."
   _dbg("make_context_current: win=0x" + to_hex(win))
}

fn get_current_context() any {
   "Returns the window whose context is current on the calling thread."
   0
}

fn extension_supported(any ext) bool {
   "Checks whether the specified API extension is supported."
   false
}

fn get_instance_proc_address(any instance, any procname) any {
   "Returns the address of the specified Vulkan instance function."
   0
}

fn get_osmesa_context(any win) any {
   "Returns the OSMesa context of the specified window."
   if(!win){ return 0 }
   def ctx = win.get("offscreen_context", 0)
   if(!ctx){ return 0 }
   ctx.get("context", 0)
}

fn get_osmesa_color_buffer(any win, any width_ptr, any height_ptr, any format_ptr, any buffer_ptr) bool {
   "Retrieves the color buffer associated with the specified OSMesa context attached to a window."
   if(!win){ return false }
   def ctx = win.get("offscreen_context", 0)
   if(!ctx){ return false }
   if(buffer_ptr){ store64(buffer_ptr, ctx.get("buffer", 0)) }
   if(width_ptr){ store64(width_ptr, ctx.get("width", 0)) }
   if(height_ptr){ store64(height_ptr, ctx.get("height", 0)) }
   true
}

fn get_osmesa_depth_buffer(any win, any width_ptr, any height_ptr, any bytes_ptr, any buffer_ptr) bool {
   "Retrieves the depth buffer associated with the specified window."
   false
}

fn get_win32_window(any win) any { ui_backend.get_win32_window(win) }

fn get_win32_adapter(any monitor) any { ui_backend.get_win32_adapter(monitor) }

fn get_win32_monitor(any monitor) any { ui_backend.get_win32_monitor(monitor) }

fn get_wgl_context(any win) any { ui_backend.get_wgl_context(win) }

fn get_cocoa_window(any win) any { ui_backend.get_cocoa_window(win) }

fn get_cocoa_monitor(any monitor) any { ui_backend.get_cocoa_monitor(monitor) }

fn get_cocoa_view(any win) any { ui_backend.get_cocoa_view(win) }

fn get_nsgl_context(any win) any { ui_backend.get_nsgl_context(win) }

#main {
   assert(KEY_A == 65 && KEY_F25 == 314 && KEY_KP_ENTER == 335, "window native key constants")
   assert(MOUSE_BUTTON_LEFT == MOUSE_BUTTON_1 && JOYSTICK_LAST == JOYSTICK_16 && GAMEPAD_BUTTON_CROSS == GAMEPAD_BUTTON_A && CURSOR_DISABLED != CURSOR_NORMAL, "window native aliases")
   assert(get_backend_name().len > 0 && get_current_context() == 0 && extension_supported("probe") == false, "window native backend stubs")
   assert(get_instance_proc_address(0, "vkGetInstanceProcAddr") == 0 && get_clipboard(0) == "" && get_osmesa_context(0) == 0, "window native null handles")
   assert(get_osmesa_color_buffer(0, 0, 0, 0, 0) == false && get_osmesa_depth_buffer(0, 0, 0, 0, 0) == false, "window native osmesa stubs")
   print("✓ std.os.ui.window.native self-test passed")
}
