;; Keywords: ui window
;; Window and Event Management for Nytrix

module std.ui.window (
   BACKEND_NY, backend, available,
   EVENT_NONE, EVENT_KEY_PRESSED, EVENT_KEY_RELEASED, EVENT_KEY_CHAR,
   EVENT_MOUSE_BUTTON_PRESSED, EVENT_MOUSE_BUTTON_RELEASED, EVENT_MOUSE_SCROLL, EVENT_MOUSE_POS_CHANGED,
   EVENT_WINDOW_MOVED, EVENT_WINDOW_RESIZED, EVENT_FOCUS_IN, EVENT_FOCUS_OUT, EVENT_MOUSE_ENTER, EVENT_MOUSE_LEAVE,
   EVENT_WINDOW_REFRESH, EVENT_QUIT, EVENT_DATA_DROP, EVENT_DATA_DRAG, EVENT_WINDOW_MAXIMIZED, EVENT_WINDOW_MINIMIZED,
   EVENT_WINDOW_RESTORED, EVENT_SCALE_UPDATED, EVENT_MONITOR_CONNECTED, EVENT_MONITOR_DISCONNECTED,
   WINDOW_NORMAL, WINDOW_NO_BORDER, WINDOW_NO_RESIZE, WINDOW_ALLOW_DND, WINDOW_HIDE_MOUSE, WINDOW_FULLSCREEN,
   WINDOW_TRANSPARENT, WINDOW_CENTER, WINDOW_RAW_MOUSE, WINDOW_SCALE_TO_MONITOR, WINDOW_HIDE,
   WINDOW_MAXIMIZE, WINDOW_CENTER_CURSOR, WINDOW_FLOATING, WINDOW_FOCUS_ON_SHOW, WINDOW_MINIMIZE,
   WINDOW_FOCUS, WINDOW_CAPTURE_MOUSE, WINDOW_CPU, WINDOW_VULKAN,
   CLIENT_API, NO_API,
   CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_LOCKED,
   ARROW_CURSOR, IBEAM_CURSOR, CROSSHAIR_CURSOR, POINTING_HAND_CURSOR,
   RESIZE_EW_CURSOR, RESIZE_NS_CURSOR, RESIZE_NWSE_CURSOR, RESIZE_NESW_CURSOR,
   RESIZE_ALL_CURSOR, NOT_ALLOWED_CURSOR,
   KEY_NULL, KEY_ESCAPE, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META,
   get_monitors, get_primary_monitor,
   get_monitor_pos, get_monitor_workarea, get_monitor_physical_size, get_monitor_content_scale, get_monitor_name,
   get_video_mode, get_video_modes, set_gamma, get_gamma_ramp, set_gamma_ramp, get_window_monitor, set_window_monitor,
   create_window, open_window, create, id, title, set_title, pos, size,
   set_icon,
   move, resize, should_close, set_should_close, close,
   exit_key, set_exit_key, key_down, key_pressed, mod_down,
   mouse_pos, mouse_down, mouse_pressed,
   set_cursor_mode, create_cursor, create_standard_cursor, destroy_cursor, set_cursor, cursor_pos, set_cursor_pos,
   focus,
   set_input_exclusive,
   match_chord, bind,
   push_event, check_event, event_type, event_window, event_window_id, event_data,
   on_key, poll_events, count_open, last, get_win, swap_buffers, make_current,
   blit_buffer, update_input, set_blit_handler,
   set_clipboard, get_clipboard,
   get_error, get_proc_address,
   window_hint, window_hint_string,
   get_x11_display, get_x11_window, set_x11_selection_string, get_x11_selection_string
)

use std.core *
print("INIT: std.ui.window")
use std.core.dict_mod *
use std.ui.consts *
use std.ui.event as ev
use std.ui.window.input.key as ui_key
use std.os *
use std.os.time *
use std.str as str
use std.util.common as common
use std.ui.window.platform as ui_backend

def _MOD_MASK = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META

def CURSOR_NORMAL = ui_backend.CURSOR_NORMAL
def CURSOR_HIDDEN = ui_backend.CURSOR_HIDDEN
def CURSOR_LOCKED = ui_backend.CURSOR_DISABLED
def ARROW_CURSOR = ui_backend.ARROW_CURSOR
def IBEAM_CURSOR = ui_backend.IBEAM_CURSOR
def CROSSHAIR_CURSOR = ui_backend.CROSSHAIR_CURSOR
def POINTING_HAND_CURSOR = ui_backend.POINTING_HAND_CURSOR
def RESIZE_EW_CURSOR = ui_backend.RESIZE_EW_CURSOR
def RESIZE_NS_CURSOR = ui_backend.RESIZE_NS_CURSOR
def RESIZE_NWSE_CURSOR = ui_backend.RESIZE_NWSE_CURSOR
def RESIZE_NESW_CURSOR = ui_backend.RESIZE_NESW_CURSOR
def RESIZE_ALL_CURSOR = ui_backend.RESIZE_ALL_CURSOR
def NOT_ALLOWED_CURSOR = ui_backend.NOT_ALLOWED_CURSOR
def CLIENT_API = ui_backend.CLIENT_API
def NO_API = ui_backend.NO_API

def _KEY_MAP = [
   [ui_backend.KEY_ESCAPE,    27],
   [ui_backend.KEY_ENTER,     13],
   [ui_backend.KEY_TAB,       9],
   [ui_backend.KEY_BACKSPACE, 8],
   [ui_backend.KEY_SPACE,     32],
   [ui_backend.KEY_GRAVE_ACCENT, 96],
   [ui_backend.KEY_WORLD_1,   96],
   [ui_backend.KEY_WORLD_2,   96],
   [126,                      96],
   [ui_backend.KEY_LEFT,      1000],
   [ui_backend.KEY_UP,        1001],
   [ui_backend.KEY_RIGHT,     1002],
   [ui_backend.KEY_DOWN,      1003],
   [ui_backend.KEY_PAGE_UP,   1004],
   [ui_backend.KEY_PAGE_DOWN, 1005],
   [ui_backend.KEY_HOME,      1006],
   [ui_backend.KEY_END,       1007],
   [ui_backend.KEY_F1,        1008],
   [ui_backend.KEY_F2,        1009],
   [ui_backend.KEY_F3,        1010],
   [ui_backend.KEY_F4,        1011],
   [ui_backend.KEY_F5,        1012],
   [ui_backend.KEY_F6,        1013],
   [ui_backend.KEY_F7,        1014],
   [ui_backend.KEY_F8,        1015],
   [ui_backend.KEY_F9,        1016],
   [ui_backend.KEY_F10,       1017],
   [ui_backend.KEY_F11,       1018],
   [ui_backend.KEY_F12,       1019],
   [ui_backend.KEY_LEFT_SHIFT,    16],
   [ui_backend.KEY_RIGHT_SHIFT,   16],
   [ui_backend.KEY_LEFT_CONTROL,  17],
   [ui_backend.KEY_RIGHT_CONTROL, 17],
   [ui_backend.KEY_LEFT_ALT,      18],
   [ui_backend.KEY_RIGHT_ALT,     18],
   [ui_backend.KEY_A, 65], [ui_backend.KEY_B, 66], [ui_backend.KEY_C, 67], [ui_backend.KEY_D, 68],
   [ui_backend.KEY_E, 69], [ui_backend.KEY_F, 70], [ui_backend.KEY_G, 71], [ui_backend.KEY_H, 72],
   [ui_backend.KEY_I, 73], [ui_backend.KEY_J, 74], [ui_backend.KEY_K, 75], [ui_backend.KEY_L, 76],
   [ui_backend.KEY_M, 77], [ui_backend.KEY_N, 78], [ui_backend.KEY_O, 79], [ui_backend.KEY_P, 80],
   [ui_backend.KEY_Q, 81], [ui_backend.KEY_R, 82], [ui_backend.KEY_S, 83], [ui_backend.KEY_T, 84],
   [ui_backend.KEY_U, 85], [ui_backend.KEY_V, 86], [ui_backend.KEY_W, 87], [ui_backend.KEY_X, 88],
   [ui_backend.KEY_Y, 89], [ui_backend.KEY_Z, 90],
   [ui_backend.KEY_0, 48], [ui_backend.KEY_1, 49], [ui_backend.KEY_2, 50], [ui_backend.KEY_3, 51],
   [ui_backend.KEY_4, 52], [ui_backend.KEY_5, 53], [ui_backend.KEY_6, 54], [ui_backend.KEY_7, 55],
   [ui_backend.KEY_8, 56], [ui_backend.KEY_9, 57],
   [ui_backend.KEY_COMMA, 44], [ui_backend.KEY_PERIOD, 46], [ui_backend.KEY_SLASH, 47],
   [ui_backend.KEY_SEMICOLON, 59], [ui_backend.KEY_EQUAL, 61],
   [ui_backend.KEY_MINUS, 45], [ui_backend.KEY_APOSTROPHE, 39],
   [ui_backend.KEY_LEFT_BRACKET, 91], [ui_backend.KEY_RIGHT_BRACKET, 93], [ui_backend.KEY_BACKSLASH, 92]
]

;; Global state

mut _windows = []
mut _window_registry = dict(16)
mut _last_update_t   = 0

mut _debug = -1
fn _is_debug(){ "Internal: Checks if UI debug logging is enabled via environment variables." _debug = common.cached_env_truthy(_debug, "NY_UI_DEBUG") _debug }
fn _dbg(msg){ if(_is_debug()){ print("[ui:window] " + msg) } }
fn _dbg_win(win, msg){ if(_is_debug()){ print("[ui:window] win=0x" + to_hex(win) + " " + msg) } }

fn backend(){ "Returns the name of the active windowing platform (e.g., 'x11', 'wayland', 'win32', 'cocoa')." ui_backend.get_backend_name() }
fn available(){ "Checks if the windowing system is available on the current platform." true }

fn _is_window(win){ "Internal: Type check for window dictionary." is_dict(win) && dict_has(win, "handle") }

fn _get_handle(win){
   "Internal: Returns the raw window handle from a window object or raw pointer."
   "Internal: Extracts the native window handle from a window dictionary or handle."
   if(is_dict(win)){ return dict_get(win, "handle", 0) }
   win
}

fn _get_win(win){
   "Internal: Retrieves the window dictionary from the global registry if a handle is provided."
   "Internal: Resolves a window handle or dictionary to the authoritative window object in the registry."
   def h = _get_handle(win)
   if(!h){ return win }
   def real = dict_get(_window_registry, h, 0)
   if(real){ return real }
   win
}

fn _save_win(win){
   "Internal: Updates the global window registry with a new window state."
   "Internal: Updates the window registry with the current state of a window object."
   def h = _get_handle(win)
   if(h){ _window_registry = dict_set(_window_registry, h, win) }
}

;; Key sequence helpers

fn _seq_match(a, b, allow_prefix=false){
   "Internal: Deep sequence comparison for key combos/chords."
   "Internal: Compares two key sequences for equality or prefix matching."
   if(allow_prefix){ if(len(a) >= len(b)){ return false } }
   elif(len(a) != len(b)){ return false }
   mut i = 0 while(i < len(a)){
      def sa = get(a, i) def sb = get(b, i)
      if(get(sa, 0) != get(sb, 0) || (get(sa, 1) & _MOD_MASK) != (get(sb, 1) & _MOD_MASK)){ return false }
      i += 1
   }
   true
}

fn _seq_equal(a, b){ "Internal: Alias for exact sequence match." _seq_match(a, b) }
fn _seq_is_prefix(pref, full){ "Internal: Alias for prefix sequence match." _seq_match(pref, full, true) }

fn _normalize_mod(mod){ "Internal: Normalize modifier bits." ui_key.normalize_mod(mod) }
fn _normalize_key(key){ "Internal: Normalize physical key code." ui_key.normalize_key(key) }
fn _mod_bit_for_key(key){ "Internal: Returns the modifier bit associated with a physical key." ui_key.mod_bit_for_key(key) }
fn _mods_from_key_states(ks){ "Internal: Calculates active modifier bits from a key state dictionary." ui_key.mods_from_key_states(ks) }
fn _parse_notation(notation){ "Internal: Parses a key notation string like 'Ctrl+Shift+A'." ui_key.parse_notation(notation) }

fn _normalize_glfw_mods(mods){
   "Internal: Converts GLFW modifier bitmask to Nytrix standard format."
   "Internal: Normalizes GLFW modifier bitmasks to Nytrix modifier bitmasks."
   ;; GLFW mod bits: SHIFT=0x0001 CTRL=0x0002 ALT=0x0004 SUPER=0x0008
   mut m = 0
   if((mods & 0x0001) != 0){ m = m | MOD_SHIFT }
   if((mods & 0x0002) != 0){ m = m | MOD_CONTROL }
   if((mods & 0x0004) != 0){ m = m | MOD_ALT }
   if((mods & 0x0008) != 0){ m = m | MOD_SUPER }
   m
}

fn _map_glfw_key(glfw_key){
   "Internal: Maps GLFW key codes to Nytrix virtual key codes."
   "Maps a GLFW keycode to Nytrix's stable key codes (same as polling path)."
   mut i = 0
   while(i < len(_KEY_MAP)){
      def pair = get(_KEY_MAP, i)
      if(get(pair, 0) == glfw_key){
         return get(pair, 1)
      }
      i += 1
   }
   ;; Fallback: normalize what we can (letters/arrows/modifiers) without
   ;; hardcoding the full table twice.
   _normalize_key(glfw_key)
}

fn _key_cb(h: ptr, k: i32, sc: i32, act: i32, mods: i32){
   "Internal: Callback for GLFW key events."
   mut win = _get_win(h)
   if(_is_window(win)){
      ;; Mark that callbacks are actually firing (used to disable polling).
      if(!dict_get(win, "has_key_cb", false)){
         win = dict_set(win, "has_key_cb", true)
      }
      mut data = dict()
      data = dict_set(data, "raw_key", k)
      data = dict_set(data, "key", _map_glfw_key(k))
      data = dict_set(data, "action", act)
      def nm = _normalize_glfw_mods(mods)
      data = dict_set(data, "mod", nm)
      win = dict_set(win, "modifiers", nm)
      _save_win(win)
      if(act == 1 || act == 2){ push_event(win, EVENT_KEY_PRESSED, data) }
      elif(act == 0){ push_event(win, EVENT_KEY_RELEASED, data) }
   }
}

;; Callbacks

fn _char_cb(h: ptr, c: u32){
   "Internal: Callback for GLFW character input events."
   mut win = _get_win(h)
   if(_is_window(win)){
      ;; NOTE: a working char callback does not guarantee that the key callback
      ;; works (we still need polling fallback for arrows/F-keys until we see a
      ;; key event).
      if(!dict_get(win, "has_char_cb", false)){
         win = dict_set(win, "has_char_cb", true)
         _save_win(win)
      }
      ;; Include current modifiers so higher layers can correctly handle
      ;; Alt/Control combinations without double-input.
      mut data = dict()
      data = dict_set(data, "char", c)
      data = dict_set(data, "mod", dict_get(win, "modifiers", 0))
      push_event(win, EVENT_KEY_CHAR, data)
   }
}

fn _size_cb(h: ptr, w: i32, h2: i32){
   "Internal: Callback for GLFW window resize events."
   mut win = _get_win(h)
   if(_is_window(win)){
      win = dict_set(win, "w", w)
      win = dict_set(win, "h", h2)
      _save_win(win)
      mut data = dict() data = dict_set(data, "w", w) data = dict_set(data, "h", h2)
      push_event(win, EVENT_WINDOW_RESIZED, data)
   }
}

fn _pos_cb(h: ptr, x: i32, y: i32){
   "Internal: Callback for GLFW window move events."
   mut win = _get_win(h)
   if(_is_window(win)){
      win = dict_set(win, "x", x)
      win = dict_set(win, "y", y)
      _save_win(win)
      mut data = dict() data = dict_set(data, "x", x) data = dict_set(data, "y", y)
      push_event(win, EVENT_WINDOW_MOVED, data)
   }
}

fn _scroll_cb(h: ptr, f_xoff: f64, f_yoff: f64){
   "Internal: Callback for GLFW mouse scroll events."
   mut win = _get_win(h)
   if(_is_window(win)){
      win = dict_set(win, "scroll_dx", dict_get(win, "scroll_dx", 0.0) + f_xoff)
      win = dict_set(win, "scroll_dy", dict_get(win, "scroll_dy", 0.0) + f_yoff)
      win = dict_set(win, "scroll_x",  dict_get(win, "scroll_x", 0.0) + f_xoff)
      win = dict_set(win, "scroll_y",  dict_get(win, "scroll_y", 0.0) + f_yoff)
      _save_win(win)

      mut data = dict()
      data = dict_set(data, "dx", f_xoff)
      data = dict_set(data, "dy", f_yoff)
      data = dict_set(data, "scrolling", true)
      data = dict_set(data, "mod", dict_get(win, "modifiers", 0))
      push_event(win, EVENT_MOUSE_SCROLL, data)
   }
}

fn _mouse_btn_cb(h: ptr, btn: i32, act: i32, mods: i32){
   "Internal: Callback for GLFW mouse button events."
   mut win = _get_win(h)
   if(_is_window(win)){
      def mx = dict_get(win, "mouse_x", 0)
      def my = dict_get(win, "mouse_y", 0)
      mut data = dict()
      data = dict_set(data, "button", btn)
      data = dict_set(data, "x", mx)
      data = dict_set(data, "y", my)
      data = dict_set(data, "mod", mods)
      win = dict_set(win, "modifiers", mods)
      _save_win(win)
      if(act == 1){ push_event(win, EVENT_MOUSE_BUTTON_PRESSED, data) }
      elif(act == 0){ push_event(win, EVENT_MOUSE_BUTTON_RELEASED, data) }
   }
}

fn _cursor_pos_cb(h: ptr, dx: f64, dy: f64){
   mut win = _get_win(h)
   if(_is_window(win)){
      def lx = dict_get(win, "mouse_x", 0)
      def ly = dict_get(win, "mouse_y", 0)
      def moved = (int(dx) != lx) || (int(dy) != ly)

      win = dict_set(win, "mouse_x", int(dx))
      win = dict_set(win, "mouse_y", int(dy))
      _save_win(win)

      mut data = dict()
      data = dict_set(data, "x", int(dx))
      data = dict_set(data, "y", int(dy))
      data = dict_set(data, "dx", int(dx) - lx)
      data = dict_set(data, "dy", int(dy) - ly)
      data = dict_set(data, "moved", moved)
      push_event(win, EVENT_MOUSE_POS_CHANGED, data)
   }
}

;; Window creation

fn open_window(name, x, y, w, h, flags=0){
   "Creates and opens a new system window."
   if(!is_str(name)){ name = to_str(name) }
   if(w < 1){ w = 1 } if(h < 1){ h = 1 }
   _dbg("open_window: name='" + name + "' pos=" + to_str(x) + "," + to_str(y) + " size=" + to_str(w) + "x" + to_str(h) + " flags=0x" + to_hex(flags))
   if(common.env_truthy("NY_UI_HEADLESS")){
      flags = flags | WINDOW_HIDE | WINDOW_NO_RESIZE
      _dbg("open_window: NY_UI_HEADLESS forcing flags=0x" + to_hex(flags))
   }
   mut handle = ui_backend.create_window(name, x, y, w, h, flags)
   if(!handle){
      _dbg("ERROR: open_window backend create failed")
      return false
   }
   _dbg_win(handle, "backend window created")

   mut win = dict(64)
   win = dict_set(win, "handle",         handle)
   win = dict_set(win, "title",          name)
   win = dict_set(win, "x",              x)
   win = dict_set(win, "y",              y)
   win = dict_set(win, "w",              w)
   win = dict_set(win, "h",              h)
   win = dict_set(win, "flags",          flags)
   win = dict_set(win, "should_close",   false)
   win = dict_set(win, "exit_key",       KEY_ESCAPE)
   win = dict_set(win, "events",         [])
   win = dict_set(win, "key_states",     dict(256))
   win = dict_set(win, "last_key_states",dict(256))
   win = dict_set(win, "pressed_keys",    dict(256))
   win = dict_set(win, "mouse_x",        0)
   win = dict_set(win, "mouse_y",        0)
   win = dict_set(win, "mouse_buttons",      dict(32))
   win = dict_set(win, "last_mouse_buttons", dict(32))
   win = dict_set(win, "pressed_buttons",    dict(32))
   win = dict_set(win, "chord_seq",      [])
   win = dict_set(win, "chord_time",     0)
   win = dict_set(win, "bindings",       [])
   win = dict_set(win, "modifiers",      0)
   ;; Key events are delivered by GLFW callbacks. A separate polling path
   ;; exists for fallback/debug, but emitting both causes double keypresses
   ;; (e.g. Enter => two newlines, repeated navigation keys, etc).
   win = dict_set(win, "key_polling",    false)
   ;; Do not assume callbacks work: enable polling until we see callbacks fire.
   win = dict_set(win, "has_char_cb",    false)

   if(handle){
      _window_registry = dict_set(_window_registry, handle, win)
      def native_events = ui_backend.uses_native_events()
      win = dict_set(win, "has_key_cb", native_events)
      _dbg_win(handle, "registry attached native_events=" + to_str(native_events))
      if(!native_events){
         _dbg_win(handle, "registering char/key/size/scroll callbacks")
         ui_backend.set_char_callback(handle, _char_cb)
         ui_backend.set_key_callback(handle, _key_cb)
         ui_backend.set_window_size_callback(handle, _size_cb)
         ui_backend.set_scroll_callback(handle, _scroll_cb)
      }
   }

   _windows = append(_windows, win)
   if(_is_debug()){ print("[ui] creating window name='" + name + "' size=" + to_str(w) + "x" + to_str(h)) }
   _dbg_win(handle, "open_window ready total_windows=" + to_str(len(_windows)))
   win
}
fn window_hint(hint, value){ "Sets a window hint for upcoming window creations." ui_backend.window_hint(hint, value) }
fn window_hint_string(hint, value){ "Sets a string window hint for upcoming window creations." ui_backend.window_hint_string(hint, value) }

fn create_window(name, x, y, w, h, flags=0){ "Alias for open_window." open_window(name, x, y, w, h, flags) }
fn create(w, h, name, flags=0){ "Common shortcut for creating a centered window." open_window(name, 0, 0, w, h, flags) }

;; Accessors

fn id(win){ "Returns the low-level platform handle (ID) for the window." if(!_is_window(win)){ return 0 } dict_get(win, "handle", 0) }
fn key_state(win, key){
   "Returns the current state of the specified key."
   win = _get_win(win) if(!_is_window(win)){ return 0 }
   def handle = dict_get(win, "handle", 0)
   if(handle){ return ui_backend.get_key(handle, key) }
   0
}

fn mouse_state(win, btn){
   "Returns the current state of the specified mouse button."
   win = _get_win(win) if(!_is_window(win)){ return 0 }
   def handle = dict_get(win, "handle", 0)
   if(handle){ return ui_backend.get_mouse_button(handle, btn) }
   0
}

fn key_name(key, scancode=0){
   "Returns the layout-specific name of the specified printable key."
   ui_backend.get_key_name(key, scancode)
}

fn title(win){ "Returns the current title of the window." if(!_is_window(win)){ return "" } dict_get(win, "title", "") }

fn set_title(win, t){
   "Updates the window title."
   win = _get_win(win) if(!_is_window(win)){ return false }
   if(!is_str(t)){ t = to_str(t) }
   win = dict_set(win, "title", t)
   _save_win(win)
   def h = dict_get(win, "handle", 0)
   if(h){
      _dbg_win(h, "set_title '" + t + "'")
      ui_backend.set_title(h, t)
   }
   true
}

fn show(win){
   "Shows the window."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = dict_get(win, "handle", 0)
   if(h){
      _dbg_win(h, "show")
      ui_backend.show_window(h)
   }
   true
}

fn hide(win){
   "Hides the window."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = dict_get(win, "handle", 0)
   if(h){
      _dbg_win(h, "hide")
      ui_backend.hide_window(h)
   }
   true
}

fn iconify(win){
   "Minimizes the window."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = dict_get(win, "handle", 0)
   if(h){
      _dbg_win(h, "iconify")
      ui_backend.iconify_window(h)
   }
   true
}

fn restore(win){
   "Restores the window from minimized or maximized state."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = dict_get(win, "handle", 0)
   if(h){
      _dbg_win(h, "restore")
      ui_backend.restore_window(h)
   }
   true
}

fn maximize(win){
   "Maximizes the window."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = dict_get(win, "handle", 0)
   if(h){
      _dbg_win(h, "maximize")
      ui_backend.maximize_window(h)
   }
   true
}

fn set_icon(win, images){
   "Sets the window icon from one or more RGBA8 image dictionaries."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = dict_get(win, "handle", 0)
   if(!h){ return false }
   win = dict_set(win, "icon_images", images)
   _save_win(win)
   _dbg_win(h, "set_icon images=" + to_str(len(images)))
   ui_backend.set_window_icon(h, images)
}

fn create_cursor(image, xhot=0, yhot=0){
   "Creates a custom cursor from one RGBA8 image dictionary."
   ui_backend.create_cursor(image, xhot, yhot)
}

fn create_standard_cursor(shape){
   "Creates a backend-native standard cursor."
   ui_backend.create_standard_cursor(shape)
}

fn destroy_cursor(cursor){
   "Destroys a previously created cursor object."
   ui_backend.destroy_cursor(cursor)
}

fn set_cursor(win, cursor){
   "Applies a cursor object to the specified window, or clears it when cursor is zero."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = dict_get(win, "handle", 0)
   if(!h){ return false }
   win = dict_set(win, "cursor", cursor)
   _save_win(win)
   _dbg_win(h, "set_cursor cursor=0x" + to_hex(cursor))
   ui_backend.set_cursor(h, cursor)
}

fn pos(win){ "Returns [x, y] screen coordinates of the window." win = _get_win(win) if(!_is_window(win)){ return [0,0] } [dict_get(win, "x", 0), dict_get(win, "y", 0)] }
fn size(win){ "Returns [width, height] dimensions of the window." win = _get_win(win) if(!_is_window(win)){ return [0,0] } [dict_get(win, "w", 0), dict_get(win, "h", 0)] }

fn move(win, x, y){
   "Moves the window to [x, y] coordinates."
   win = _get_win(win) if(!_is_window(win)){ return false }
   win = dict_set(win, "x", x) win = dict_set(win, "y", y)
   _save_win(win)
   def h = dict_get(win, "handle", 0)
   if(h){
      _dbg_win(h, "move pos=" + to_str(x) + "," + to_str(y))
      ui_backend.set_pos(h, x, y)
   }
   mut data = dict() data = dict_set(data, "x", x) data = dict_set(data, "y", y)
   push_event(win, EVENT_WINDOW_MOVED, data)
}

fn resize(win, w, h){
   "Resizes the window to [w, h] pixels."
   win = _get_win(win) if(!_is_window(win)){ return false }
   if(w < 1){ w = 1 } if(h < 1){ h = 1 }
   win = dict_set(win, "w", w) win = dict_set(win, "h", h)
   _save_win(win)
   def hh = dict_get(win, "handle", 0)
   if(hh){
      _dbg_win(hh, "resize size=" + to_str(w) + "x" + to_str(h))
      ui_backend.set_size(hh, w, h)
   }
   mut data = dict() data = dict_set(data, "w", w) data = dict_set(data, "h", h)
   push_event(win, EVENT_WINDOW_RESIZED, data)
}

fn should_close(win){
   "Returns true if the window has been requested to close."
   win = _get_win(win) if(!_is_window(win)){ return true }
   !!dict_get(win, "should_close", false)
}

fn set_should_close(win, sc=true){
   "Sets the window close flag manually."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def old = !!dict_get(win, "should_close", false)
   win = dict_set(win, "should_close", !!sc)
   _save_win(win)
   if(!!sc && !old){ push_event(win, EVENT_QUIT) }
   if(!!sc){
      def h = dict_get(win, "handle", 0)
      if(h){ ui_backend.set_should_close(h, 1) }
   }
   true
}

fn close(win){ "Closes the window." set_should_close(win, true) }

fn exit_key(win){ "Returns the current exit (close) key for the window." win = _get_win(win) if(!_is_window(win)){ return KEY_NULL } dict_get(win, "exit_key", KEY_ESCAPE) }
fn set_exit_key(win, k){ "Changes the exit (close) key for the window." win = _get_win(win) if(_is_window(win)){ win = dict_set(win, "exit_key", k) _save_win(win) true } else { false } }

fn get_monitors(){
   "Returns the currently connected monitors from the active backend."
   ui_backend.get_monitors()
}

fn get_primary_monitor(){
   "Returns the primary monitor from the active backend."
   ui_backend.get_primary_monitor()
}

fn get_monitor_pos(monitor){
   "Returns `[x, y]` for a monitor."
   ui_backend.get_monitor_pos(monitor)
}

fn get_monitor_workarea(monitor){
   "Returns `[x, y, width, height]` for a monitor work area."
   ui_backend.get_monitor_workarea(monitor)
}

fn get_monitor_physical_size(monitor){
   "Returns `[width_mm, height_mm]` for a monitor."
   ui_backend.get_monitor_physical_size(monitor)
}

fn get_monitor_content_scale(monitor){
   "Returns `[xscale, yscale]` for a monitor."
   ui_backend.get_monitor_content_scale(monitor)
}

fn get_monitor_name(monitor){
   "Returns the monitor name."
   ui_backend.get_monitor_name(monitor)
}

fn get_video_mode(monitor){
   "Returns the current video mode for a monitor."
   ui_backend.get_video_mode(monitor)
}

fn get_video_modes(monitor){
   "Returns all video modes for a monitor."
   ui_backend.get_video_modes(monitor)
}

fn set_gamma(monitor, gamma){
   "Generates and applies a gamma ramp for a monitor."
   ui_backend.set_gamma(monitor, gamma)
}

fn get_gamma_ramp(monitor){
   "Returns the current monitor gamma ramp as a dict with `size`, `red`, `green`, and `blue` arrays."
   ui_backend.get_gamma_ramp(monitor)
}

fn set_gamma_ramp(monitor, ramp){
   "Sets the current monitor gamma ramp from a dict with `size`, `red`, `green`, and `blue` arrays."
   ui_backend.set_gamma_ramp(monitor, ramp)
}

fn get_window_monitor(win){
   "Returns the monitor associated with a fullscreen window."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   ui_backend.get_window_monitor(dict_get(win, "handle", 0))
}

fn set_window_monitor(win, monitor, xpos, ypos, width, height, refresh_rate=0){
   "Sets or clears the monitor/fullscreen association for a window."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def handle = dict_get(win, "handle", 0)
   if(!handle){ return false }
   ui_backend.set_window_monitor(handle, monitor, xpos, ypos, width, height, refresh_rate)
   win = dict_set(win, "x", xpos)
   win = dict_set(win, "y", ypos)
   win = dict_set(win, "w", width)
   win = dict_set(win, "h", height)
   win = dict_set(win, "fullscreen", !!monitor)
   _save_win(win)
   true
}

;; Event queue

fn push_event(win, kind, data=0){
   def h = _get_handle(win)
   mut real_win = _get_win(h)
   if(!_is_window(real_win)){ return false }
   mut q = dict_get(real_win, "events", [])
   q = ev.queue_push(q, ev.make_event(kind, real_win, h, data))
   real_win = dict_set(real_win, "events", q)
   _save_win(real_win)
   true
}

fn _window_process_internal(win, e){
   win = _get_win(win)
   def typ = ev.event_type(e)
   mut out_e = e
   if(typ == EVENT_KEY_PRESSED || typ == EVENT_KEY_RELEASED){
      mut data = ev.event_data(e)
      def k = _normalize_key(is_dict(data) ? dict_get(data, "key", 0) : data)
      mut ks = dict_get(win, "key_states", 0)
      def is_press = (typ == EVENT_KEY_PRESSED)
      ks = dict_set(ks, k, is_press)
      if(is_press){
         def pk = dict_get(win, "pressed_keys", 0)
         win = dict_set(win, "pressed_keys", dict_set(pk, k, true))
      }
      win = dict_set(win, "key_states", ks)
      def mod = _mods_from_key_states(ks)
      win = dict_set(win, "modifiers", mod)
      _save_win(win)

      if(!is_dict(data)){ data = dict() }
      data = dict_set(data, "key",   k) data = dict_set(data, "mod",   mod)

      out_e = [get(e,0), get(e,1), get(e,2), get(e,3), data]

      mut consumed = false
      if(is_press){
         if(_mod_bit_for_key(k) == 0 && !dict_get(win, "input_exclusive", false)){
         def now = ticks() / 1000000
         mut seq = dict_get(win, "chord_seq", [])
         def last_time = dict_get(win, "chord_time", 0)
         if(len(seq) > 0 && (now - last_time > 1000)){ seq = [] }
         seq = append(seq, [k, mod])
         win = dict_set(win, "chord_seq",  seq)
         win = dict_set(win, "chord_time", now)
         _save_win(win)

         mut found_match = false mut partial = false
         def binds = dict_get(win, "bindings", [])
         mut i = 0 while(i < len(binds)){
               def b = get(binds, i) def tseq = get(b, 0)
               if(_seq_equal(tseq, seq)){
                  def action = get(b, 1) if(is_func(action)){ action() }
                  found_match = true break
               } elif(_seq_is_prefix(seq, tseq)){ partial = true }
               i += 1
         }
         if(found_match){ consumed = true win = dict_set(win, "chord_seq", []) _save_win(win) }
         elif(partial){ consumed = true }
         else {
               win = dict_set(win, "chord_seq", []) _save_win(win)
               consumed = false
         }
         }
      }
      if(!consumed && is_press && k == exit_key(win)){ set_should_close(win, true) }
      return [consumed, out_e]
   } elif(typ == EVENT_KEY_CHAR){
      return [false, e]
   } elif(typ == EVENT_MOUSE_POS_CHANGED){
      def data = ev.event_data(e)
      win = dict_set(win, "mouse_x", dict_get(data, "x", 0))
      win = dict_set(win, "mouse_y", dict_get(data, "y", 0))
      _save_win(win)
   } elif(typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED){
      def data = ev.event_data(e)
      def btn = dict_get(data, "button", 0)
      mut mb = dict_get(win, "mouse_buttons", 0)
      def is_press = (typ == EVENT_MOUSE_BUTTON_PRESSED)
      mb = dict_set(mb, btn, is_press)
      if(is_press){
         def pb = dict_get(win, "pressed_buttons", 0)
         win = dict_set(win, "pressed_buttons", dict_set(pb, btn, true))
      }
      win = dict_set(win, "mouse_buttons", mb)
      win = dict_set(win, "mouse_x", dict_get(data, "x", 0))
      win = dict_set(win, "mouse_y", dict_get(data, "y", 0))
      _save_win(win)
   } elif(typ == EVENT_WINDOW_RESIZED){
      def data = ev.event_data(e)
      def nw = dict_get(data, "w", 0)
      def nh = dict_get(data, "h", 0)
      if(nw > 0 && nh > 0){
         win = dict_set(win, "w", nw)
         win = dict_set(win, "h", nh)
         _save_win(win)
      }
   } elif(typ == EVENT_WINDOW_MOVED){
      def data = ev.event_data(e)
      win = dict_set(win, "x", dict_get(data, "x", 0))
      win = dict_set(win, "y", dict_get(data, "y", 0))
      _save_win(win)
   } elif(typ == EVENT_FOCUS_IN){
      win = dict_set(win, "focused", true)
      _save_win(win)
   } elif(typ == EVENT_FOCUS_OUT){
      win = dict_set(win, "focused", false)
      _save_win(win)
   } elif(typ == EVENT_MOUSE_SCROLL){
      def data = ev.event_data(e)
      win = dict_set(win, "scroll_x", dict_get(win, "scroll_x", 0.0) + dict_get(data, "dx", 0.0))
      win = dict_set(win, "scroll_y", dict_get(win, "scroll_y", 0.0) + dict_get(data, "dy", 0.0))
      win = dict_set(win, "scroll_dx", dict_get(data, "dx", 0.0))
      win = dict_set(win, "scroll_dy", dict_get(data, "dy", 0.0))
      _save_win(win)
   }
   [false, e]
}

fn _check_native_state(win){
   win = _get_win(win)
   def handle = dict_get(win, "handle", 0)
   if(!handle){ return }
   if(!ui_backend.supports_state_polling()){ return }

   if(ui_backend.should_close(handle)){
      if(!dict_get(win, "should_close", false)){
         win = dict_set(win, "should_close", true)
         _save_win(win)
         push_event(win, EVENT_QUIT)
      }
   }

   def sz = ui_backend.get_size(handle)
   def nw = get(sz, 0, 0) def nh = get(sz, 1, 0)
   if(nw > 0 && nh > 0 && (nw != dict_get(win, "w", 0) || nh != dict_get(win, "h", 0))){
      ; Soft update: sync registry without calling XResizeWindow (WM owns the size)
      win = dict_set(win, "w", nw)
      win = dict_set(win, "h", nh)
      _save_win(win)
      mut sz_data = dict()
      sz_data = dict_set(sz_data, "w", nw)
      sz_data = dict_set(sz_data, "h", nh)
      push_event(win, EVENT_WINDOW_RESIZED, sz_data)
   }

   ;; Always poll as a fallback. We dedupe in the terminal layer to avoid
   ;; double input if callbacks are also firing.

   mut ks = dict_get(win, "key_states", 0)
   mut now_ks = dict(64)
   mut i = 0 while(i < len(_KEY_MAP)){
      def pair = get(_KEY_MAP, i)
      def glfw_k = get(pair, 0)
      if(ui_backend.get_key(handle, glfw_k) == 1){
         now_ks = dict_set(now_ks, get(pair, 1), true)
      }
      i += 1
   }

   ;; Skip polling if key callbacks are working (avoids double input).
   ;; Polling is only a fallback for broken GLFW callback setups.
   if(dict_get(win, "has_key_cb", false)){
      ;; Callbacks are working - update modifier state only.
      mut seen = dict(64)
      mut changed = false
      mut j = 0 while(j < len(_KEY_MAP)){
         def row = get(_KEY_MAP, j)
         def gk = get(row, 0) def nk = get(row, 1)
         if(dict_has(seen, nk)){ j += 1 continue }
         seen = dict_set(seen, nk, true)
         def real_now = ui_backend.get_key(handle, gk) == 1
         def was = !!dict_get(ks, nk, false)
         if(real_now != was){ ks = dict_set(ks, nk, real_now) changed = true }
         j += 1
      }
      if(changed){
         win = dict_set(win, "key_states", ks)
         win = dict_set(win, "modifiers", _mods_from_key_states(ks))
         _save_win(win)
      }
   } else {
      ;; Callbacks not working - use polling as fallback.
      mut seen = dict(64)
      mut changed = false
      mut j = 0 while(j < len(_KEY_MAP)){
         def row = get(_KEY_MAP, j)
         def gk = get(row, 0) def nk = get(row, 1)
         if(dict_has(seen, nk)){ j += 1 continue }
         seen = dict_set(seen, nk, true)

         def real_now = ui_backend.get_key(handle, gk) == 1
         def was = !!dict_get(ks, nk, false)
         if(real_now != was){
         ks = dict_set(ks, nk, real_now)
         changed = true
         mut data = dict() data = dict_set(data, "key", nk) data = dict_set(data, "pressed", real_now)
         data = dict_set(data, "native", true)
         push_event(win, real_now ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED, data)
         if(real_now){
         win = dict_set(win, f"rt_{nk}", ticks())
         win = dict_set(win, f"rc_{nk}", 0)
         _save_win(win)
         }
         } elif(real_now){
         ;; Software Repeat (660ms delay, 40ms interval)
         def last_t = dict_get(win, f"rt_{nk}", 0)
         def count = dict_get(win, f"rc_{nk}", 0)
         def delay = (count == 0) ? 660000000 : 40000000
         if(ticks() - last_t > delay){
         mut data = dict() data = dict_set(data, "key", nk) data = dict_set(data, "pressed", true)
         data = dict_set(data, "native", true) data = dict_set(data, "action", 2)
         push_event(win, EVENT_KEY_PRESSED, data)
         win = dict_set(win, f"rt_{nk}", ticks())
         win = dict_set(win, f"rc_{nk}", count + 1)
         _save_win(win)
         }
         }
         j += 1
      }
      if(changed){
         win = dict_set(win, "key_states", ks)
         win = dict_set(win, "modifiers", _mods_from_key_states(ks))
         _save_win(win)
      }
   }

   ;; Poll mouse position/buttons directly to avoid fragile GLFW callback paths.
   def cur = ui_backend.get_cursor_pos(handle)
   def mx = int(get(cur, 0, 0.0))
   def my = int(get(cur, 1, 0.0))
   if(mx != dict_get(win, "mouse_x", 0) || my != dict_get(win, "mouse_y", 0)){
      mut data = dict()
      data = dict_set(data, "x", mx)
      data = dict_set(data, "y", my)
      data = dict_set(data, "dx", mx - dict_get(win, "mouse_x", 0))
      data = dict_set(data, "dy", my - dict_get(win, "mouse_y", 0))
      data = dict_set(data, "moved", true)
      data = dict_set(data, "native", true)
      push_event(win, EVENT_MOUSE_POS_CHANGED, data)
      win = dict_set(win, "mouse_x", mx)
      win = dict_set(win, "mouse_y", my)
      _save_win(win)
   }

   mut mb = dict_get(win, "mouse_buttons", 0)
   mut mb_changed = false
   mut b = 0
   while(b < 8){
      def real_now = ui_backend.get_mouse_button(handle, b) == 1
      def was = !!dict_get(mb, b, false)
      if(real_now != was){
         mb = dict_set(mb, b, real_now)
         mb_changed = true
         mut data = dict()
         data = dict_set(data, "button", b)
         data = dict_set(data, "x", dict_get(win, "mouse_x", 0))
         data = dict_set(data, "y", dict_get(win, "mouse_y", 0))
         data = dict_set(data, "mod", dict_get(win, "modifiers", 0))
         data = dict_set(data, "native", true)
         push_event(win, real_now ? EVENT_MOUSE_BUTTON_PRESSED : EVENT_MOUSE_BUTTON_RELEASED, data)
      }
      b += 1
   }
   if(mb_changed){
      win = dict_set(win, "mouse_buttons", mb)
      _save_win(win)
   }
}

@jit
fn check_event(win){
   "Polls for new events and returns the next one from the queue, or 0 if empty."
   if(!_is_window(win)){ return 0 }
   def now = ticks()
   if(true){ ; Always poll for best responsiveness
      mut cw = _get_win(win)
      cw = dict_set(cw, "mouse_last_x",  dict_get(cw, "mouse_x", 0))
      cw = dict_set(cw, "mouse_last_y",  dict_get(cw, "mouse_y", 0))
      cw = dict_set(cw, "scroll_last_x", dict_get(cw, "scroll_x", 0.0))
      cw = dict_set(cw, "scroll_last_y", dict_get(cw, "scroll_y", 0.0))
      cw = dict_set(cw, "scroll_dx", 0.0)
      cw = dict_set(cw, "scroll_dy", 0.0)
      _window_registry = dict_set(_window_registry, _get_handle(win), cw)

      update_input(win)
      ui_backend.poll_events()
      if(ui_backend.uses_native_events()){
         def native_events = ui_backend.pump_window_events(_get_handle(win))
         if(is_list(native_events) && len(native_events) > 0){
         mut i = 0
         while(i < len(native_events)){
               def ne = get(native_events, i)
               if(ev.is_event(ne)){ push_event(cw, ev.event_type(ne), ev.event_data(ne)) }
               i += 1
         }
         }
      }
      _check_native_state(win)
      _last_update_t = now
   }
   mut cw = _get_win(win)
   mut q = dict_get(cw, "events", [])
   while(len(q) > 0){
      def e = get(q, 0)
      q = slice(q, 1, len(q))
      cw = dict_set(cw, "events", q)
      _window_registry = dict_set(_window_registry, _get_handle(win), cw)

      def p_res = _window_process_internal(cw, e)
      if(!get(p_res, 0)){ return get(p_res, 1) }

      ; Consumed, keep draining from registry for next event in same call
      cw = _get_win(win)
      q = dict_get(cw, "events", [])
   }
   0
}

fn event_type(e){
   "Returns the type ID of event `e`."
   ev.event_type(e)
}
fn event_window(e){
   "Returns the window handle associated with event `e`."
   ev.event_window(e)
}
fn event_window_id(e){
   "Returns the numeric window ID for event `e`."
   ev.event_window_id(e)
}
fn event_data(e){
   "Returns the extra data payload of event `e`."
   ev.event_data(e)
}

@jit
fn key_down(win, k){
   "Returns true if key `k` is currently held down in `win`."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   dict_get(dict_get(win, "key_states", 0), _normalize_key(k), false)
}
fn get_modifiers(win){
   "Returns the bitmask of active modifier keys (Shift, Ctrl, Alt, etc.)."
   win = _get_win(win)
   if(!_is_window(win)){ return 0 }
   _normalize_mod(dict_get(win, "modifiers", 0))
}
fn mod_down(win, m){
   "Returns true if modifier combination `m` is active."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def nm = _normalize_mod(m)
   if(nm == 0){ return false }
   (get_modifiers(win) & nm) == nm
}

fn key_pressed(win, k){
   win = _get_win(win) if(!_is_window(win)){ return false }
   def nk = _normalize_key(k) def ks = dict_get(win, "key_states", 0)
   def lks = dict_get(win, "last_key_states", 0) def pk = dict_get(win, "pressed_keys", 0)
   (!!dict_get(ks, nk, false) && !dict_get(lks, nk, false)) || !!dict_get(pk, nk, false)
}

@jit
fn mouse_pos(win){
   "Returns [x, y] coordinates of the mouse cursor relative to the window."
   win = _get_win(win)
   if(!_is_window(win)){ return [0, 0] }
   [dict_get(win, "mouse_x", 0), dict_get(win, "mouse_y", 0)]
}
fn mouse_down(win, b){
   "Returns true if mouse button `b` is currently held down."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   dict_get(dict_get(win, "mouse_buttons", 0), b, false)
}
fn mouse_pressed(win, b){
   "Returns true if mouse button `b` was clicked this frame."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def mb = dict_get(win, "mouse_buttons", 0) def lmb = dict_get(win, "last_mouse_buttons", 0) def pb = dict_get(win, "pressed_buttons", 0)
   (!!dict_get(mb, b, false) && !dict_get(lmb, b, false)) || !!dict_get(pb, b, false)
}

fn on_key(win, k, p=true, r=false, m=0){
   common.touch(r) win = _get_win(win) if(!_is_window(win)){ return false }
   def nk = _normalize_key(k) mut ks = dict_get(win, "key_states", 0)
   ks = dict_set(ks, nk, !!p)
   if(!!p){ def pk = dict_get(win, "pressed_keys", 0) win = dict_set(win, "pressed_keys", dict_set(pk, nk, true)) }
   mut cm = _normalize_mod(m) if(cm == 0 || _mod_bit_for_key(nk) != 0){ cm = _mods_from_key_states(ks) }
   win = dict_set(win, "modifiers", cm)
   _save_win(win)
   mut data = dict() data = dict_set(data, "key", nk) data = dict_set(data, "pressed", !!p) data = dict_set(data, "mod", cm)
   push_event(win, !!p ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED, data)
   if(!!p && nk == exit_key(win)){ set_should_close(win, true) }
   true
}

fn match_chord(e, k, m=0){
   if(event_type(e) != EVENT_KEY_PRESSED){ return false }
   def d = event_data(e) if(!is_dict(d)){ return false }
   def nk = _normalize_key(k) def nm = _normalize_mod(m)
   if(dict_get(d, "key", 0) != nk){ return false }
   if(nm != 0 && (_normalize_mod(dict_get(d, "mod", 0)) & nm) != nm){ return false }
   true
}

fn bind(win, n, a){
   win = _get_win(win) if(!_is_window(win) || !is_str(n)){ return false }
   def seq = _parse_notation(n) if(len(seq) == 0){ return false }
   mut b = dict_get(win, "bindings", [])
   mut found = false mut i = 0 while(i < len(b)){
      def item = get(b, i) if(_seq_equal(get(item, 0), seq)){ set_idx(item, 1, a) found = true break }
      i += 1
   }
   if(!found){ b = append(b, [seq, a]) }
   win = dict_set(win, "bindings", b)
   _save_win(win)
   true
}

fn update_input(win){
   win = _get_win(win) if(!_is_window(win)){ return }
   def now = ticks() if(now - dict_get(win, "_last_upd", 0) < 500000){ return }
   win = dict_set(win, "_last_upd", now)
   def ks = dict_get(win, "key_states", 0) def mb = dict_get(win, "mouse_buttons", 0)
   win = dict_set(win, "last_key_states", ks) win = dict_set(win, "last_mouse_buttons", mb)
   win = dict_set(win, "key_states",         dict_clone(ks))
   win = dict_set(win, "mouse_buttons",      dict_clone(mb))
   win = dict_set(win, "pressed_keys",       dict(32))
   win = dict_set(win, "pressed_buttons",    dict(8))
   _save_win(win)
}

fn swap_buffers(win){
   "Swaps front and back buffers (double buffering)."
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){
         _dbg_win(h, "swap_buffers")
         ui_backend.swap_buffers(h)
      }
   }
}
fn make_current(_win){ common.touch(_win) }

mut _blit_hook = 0
fn set_blit_handler(h){ _blit_hook = h }
fn blit_buffer(win, buf, w, h){ win = _get_win(win) if(_is_window(win) && is_ptr(buf) && _blit_hook){ _blit_hook(buf, w, h) } 0 }

fn poll_events(){
   "Triggers the system to process window and input events."
   _dbg("poll_events")
   ui_backend.poll_events()
}

fn count_open(){
   mut i = 0 mut n = 0 while(i < len(_windows)){
      def w = get(_windows, i) if(_is_window(w) && !should_close(w)){ n += 1 }
      i += 1
   }
   n
}

fn last(){ if(len(_windows) == 0){ return 0 } def w = get(_windows, len(_windows) - 1, 0) if(_is_window(w)){ return _get_win(w) } 0 }
fn get_win(win){ _get_win(win) }

fn set_cursor_mode(win, m){ win = _get_win(win) if(!_is_window(win)){ return false } def h = dict_get(win, "handle", 0) if(h){ _dbg_win(h, "set_cursor_mode mode=" + to_str(m)) ui_backend.set_input_mode(h, 0x00033001, m) } true }
fn focus(win){ win = _get_win(win) if(!_is_window(win)){ return false } def h = dict_get(win, "handle", 0) if(h){ _dbg_win(h, "focus") ui_backend.focus_window(h) } true }
fn cursor_pos(win){ win = _get_win(win) if(!_is_window(win)){ return [0.0, 0.0] } def h = dict_get(win, "handle", 0) if(h){ return ui_backend.get_cursor_pos(h) } [0.0, 0.0] }
fn scroll_pos(win){ win = _get_win(win) if(!_is_window(win)){ return [0.0, 0.0] } [dict_get(win, "scroll_x", 0.0), dict_get(win, "scroll_y", 0.0)] }
fn mouse_state(win){
   win = _get_win(win)
   if(!_is_window(win)){ return dict() }
   mut m = dict()
   def x = dict_get(win, "mouse_x", 0)
   def y = dict_get(win, "mouse_y", 0)
   def lx = dict_get(win, "mouse_last_x", x)
   def ly = dict_get(win, "mouse_last_y", y)
   m = dict_set(m, "x", x)
   m = dict_set(m, "y", y)
   m = dict_set(m, "last_x", lx)
   m = dict_set(m, "last_y", ly)
   m = dict_set(m, "moved", (x != lx) || (y != ly))

   mut s = dict()
   def sx = dict_get(win, "scroll_x", 0.0)
   def sy = dict_get(win, "scroll_y", 0.0)
   def lsx = dict_get(win, "scroll_last_x", sx)
   def lsy = dict_get(win, "scroll_last_y", sy)
   s = dict_set(s, "x", sx)
   s = dict_set(s, "y", sy)
   s = dict_set(s, "last_x", lsx)
   s = dict_set(s, "last_y", lsy)
   s = dict_set(s, "scrolling", (sx != lsx) || (sy != lsy))

   m = dict_set(m, "scroll", s)
   m = dict_set(m, "scrolling", (sx != lsx) || (sy != lsy))
   m
}
fn set_cursor_pos(win, x, y){ win = _get_win(win) if(!_is_window(win)){ return false } def h = dict_get(win, "handle", 0) if(h){ ui_backend.set_cursor_pos(h, x, y) } true }
fn set_window_opacity(win, val){
   "Sets the whole-window opacity (alpha) value from 0.0 to 1.0."
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){ ui_backend.set_window_opacity(h, val) }
   }
}

fn set_window_resizable(win, val){
   "Toggles whether the window can be resized by the user."
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){ ui_backend.set_window_resizable(h, !!val) }
   }
}

fn set_window_decorated(win, val){
   "Toggles window decorations (title bar, borders)."
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){ ui_backend.set_window_decorated(h, !!val) }
   }
}

fn set_window_floating(win, val){
   "Toggles the always-on-top state of the window."
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){ ui_backend.set_window_floating(h, !!val) }
   }
}

fn set_input_exclusive(win, val){
   win = _get_win(win)
   if(_is_window(win)){
      win = dict_set(win, "input_exclusive", !!val)
      _save_win(win)
      true
   } else { false }
}

fn set_clipboard(win, s){
   "Sets the system clipboard to string `s`."
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){ ui_backend.set_clipboard(h, s) }
   }
}

fn get_clipboard(win){
   "Retrieves string contents from the system clipboard."
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){ return ui_backend.get_clipboard(h) }
   }
   ""
}

fn get_error(){
   "Returns the last backend error as [code, description]."
   ui_backend.get_error()
}

fn get_proc_address(name){
   "Returns the address of the specified OpenGL function."
   ui_backend.get_proc_address(name)
}

fn get_x11_display(){
   "Returns the active X11 Display* when the backend supports it."
   ui_backend.get_x11_display()
}

fn get_x11_window(win){
   "Returns the native X11 Window handle for `win` when available."
   win = _get_win(win)
   if(!_is_window(win)){ return 0 }
   def h = dict_get(win, "handle", 0)
   if(!h){ return 0 }
   ui_backend.get_x11_window(h)
}

fn set_x11_selection_string(win, s){
   "Sets the X11 PRIMARY selection string for the specified window."
   ui_backend.set_x11_selection_string(win, s)
}

fn get_x11_selection_string(win){
   "Returns the X11 PRIMARY selection string for the specified window."
   ui_backend.get_x11_selection_string(win)
}
