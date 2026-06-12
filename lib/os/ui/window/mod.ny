;; Keywords: window consts native native-windowing event os ui input
;; Window facade: creation, monitors, lifecycle, keyboard, mouse, gamepads, events, clipboard, and backend hooks.
;; References:
;; - std.os.ui
;; - std.os.ui.window.consts
module std.os.ui.window(BACKEND_NY, backend, available, EVENT_NONE, EVENT_KEY_PRESSED, EVENT_KEY_RELEASED, EVENT_KEY_CHAR, EVENT_MOUSE_BUTTON_PRESSED, EVENT_MOUSE_BUTTON_RELEASED, EVENT_MOUSE_SCROLL, EVENT_MOUSE_POS_CHANGED, EVENT_WINDOW_MOVED, EVENT_WINDOW_RESIZED, EVENT_FOCUS_IN, EVENT_FOCUS_OUT, EVENT_MOUSE_ENTER, EVENT_MOUSE_LEAVE, EVENT_WINDOW_REFRESH, EVENT_QUIT, EVENT_DATA_DROP, EVENT_DATA_DRAG, EVENT_WINDOW_MAXIMIZED, EVENT_WINDOW_MINIMIZED, EVENT_WINDOW_RESTORED, EVENT_SCALE_UPDATED, EVENT_MONITOR_CONNECTED, EVENT_MONITOR_DISCONNECTED, WINDOW_NORMAL, WINDOW_NO_BORDER, WINDOW_NO_RESIZE, WINDOW_ALLOW_DND, WINDOW_HIDE_MOUSE, WINDOW_FULLSCREEN, WINDOW_TRANSPARENT, WINDOW_CENTER, WINDOW_RAW_MOUSE, WINDOW_SCALE_TO_MONITOR, WINDOW_HIDE, WINDOW_MAXIMIZE, WINDOW_CENTER_CURSOR, WINDOW_FLOATING, WINDOW_FOCUS_ON_SHOW, WINDOW_MINIMIZE, WINDOW_FOCUS, WINDOW_CAPTURE_MOUSE, WINDOW_CPU, WINDOW_VULKAN, CLIENT_API, NO_API, CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_DISABLED, ARROW_CURSOR, IBEAM_CURSOR, CROSSHAIR_CURSOR, POINTING_HAND_CURSOR, RESIZE_EW_CURSOR, RESIZE_NS_CURSOR, RESIZE_NWSE_CURSOR, RESIZE_NESW_CURSOR, RESIZE_ALL_CURSOR, NOT_ALLOWED_CURSOR, KEY_NULL, KEY_ESCAPE, KEY_ESC, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_SPACE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END, KEY_INSERT, KEY_DELETE, KEY_CAPS_LOCK, KEY_SCROLL_LOCK, KEY_NUM_LOCK, KEY_PRINT_SCREEN, KEY_PAUSE, KEY_MENU, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_F25, KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9, KEY_KP_ADD, KEY_KP_SUBTRACT, KEY_KP_MULTIPLY, KEY_KP_DIVIDE, KEY_KP_DECIMAL, KEY_KP_ENTER, KEY_KP_EQUAL, KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL, KEY_LEFT_ALT, KEY_RIGHT_ALT, KEY_LEFT_SUPER, KEY_RIGHT_SUPER, KEY_SHIFT, KEY_CTRL, KEY_ALT, KEY_SUPER, KEY_GRAVE, KEY_MINUS, KEY_EQUAL, KEY_APOSTROPHE, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_SEMICOLON, KEY_LEFT_BRACKET, KEY_BACKSLASH, KEY_RIGHT_BRACKET, KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z, KEY_WORLD_1, KEY_WORLD_2, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META, get_monitors, get_primary_monitor, get_monitor_pos, get_monitor_workarea, get_monitor_physical_size, get_monitor_content_scale, get_monitor_name, get_video_mode, get_video_modes, get_monitor_resolution, get_monitor_rect, get_monitor_desktop_bounds, get_monitor_index_at, get_current_monitor_index, get_current_monitor, move_to_monitor, set_gamma, get_gamma_ramp, set_gamma_ramp, get_window_monitor, set_window_monitor, set_window_fullscreen, toggle_window_fullscreen, set_window_borderless, toggle_window_borderless, primary_mode, default_window_size, fit_to_workarea, open_window, create, show, hide, iconify, restore, maximize, id, title, set_title, pos, size, get_framebuffer_size, get_window_content_scale, get_window_scale_dpi, window_flags, has_window_flag, set_window_flag, toggle_window_flag, window_attrib, window_state, is_window_visible, is_window_focused, is_window_minimized, is_window_maximized, is_window_resizable, is_window_decorated, is_window_floating, is_window_fullscreen, is_window_borderless, window_vsync, set_window_vsync, toggle_window_vsync, toggle_window_resizable, toggle_window_decorated, toggle_window_floating, set_icon, move, resize, should_close, set_should_close, close, quit, exit_key, set_exit_key, key_state, key_name, key_down, key_pressed, get_modifiers, mod_down, mouse_pos, mouse_down, mouse_pressed, joystick_present, get_joystick_name, get_joystick_guid, get_joystick_axes, get_joystick_buttons, get_joystick_hats, joystick_is_gamepad, get_gamepad_state, get_gamepad_name, get_gamepad_guid, gamepad_count, gamepads, gamepad_connected, gamepad_mapped, gamepad_name, gamepad_guid, gamepad_axis, gamepad_button, gamepad_axis_count, gamepad_button_count, set_joystick_callback, update_gamepad_mappings, add_gamepad_mapping, JOYSTICK_1, JOYSTICK_2, JOYSTICK_3, JOYSTICK_4, JOYSTICK_5, JOYSTICK_6, JOYSTICK_7, JOYSTICK_8, JOYSTICK_9, JOYSTICK_10, JOYSTICK_11, JOYSTICK_12, JOYSTICK_13, JOYSTICK_14, JOYSTICK_15, JOYSTICK_16, JOYSTICK_LAST, JOYSTICK_HAT_BUTTONS, GAMEPAD_BUTTON_A, GAMEPAD_BUTTON_B, GAMEPAD_BUTTON_X, GAMEPAD_BUTTON_Y, GAMEPAD_BUTTON_LEFT_BUMPER, GAMEPAD_BUTTON_RIGHT_BUMPER, GAMEPAD_BUTTON_BACK, GAMEPAD_BUTTON_START, GAMEPAD_BUTTON_GUIDE, GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_DPAD_UP, GAMEPAD_BUTTON_DPAD_RIGHT, GAMEPAD_BUTTON_DPAD_DOWN, GAMEPAD_BUTTON_DPAD_LEFT, GAMEPAD_BUTTON_LAST, GAMEPAD_BUTTON_CROSS, GAMEPAD_BUTTON_SQUARE, GAMEPAD_BUTTON_TRIANGLE, GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y, GAMEPAD_AXIS_LEFT_TRIGGER, GAMEPAD_AXIS_RIGHT_TRIGGER, GAMEPAD_AXIS_LAST, set_cursor_mode, create_cursor, create_standard_cursor, destroy_cursor, set_cursor, cursor_pos, scroll_pos, mouse_state, set_cursor_pos, center_cursor, show_centered_cursor, sync_cursor, focus, transparent_framebuffer, set_window_opacity, set_window_resizable, set_window_decorated, set_window_floating, set_input_exclusive, match_chord, bind, push_event, check_event, event_type, event_window, event_window_id, event_data, event_key_is, on_key, poll_events, count_open, last, get_win, swap_buffers, make_current, blit_buffer, blit_software, update_input, set_blit_handler, set_clipboard, get_clipboard, get_error, get_proc_address, window_hint, window_hint_string, get_x11_display, get_x11_window, set_x11_selection_string, get_x11_selection_string)
use std.core
use std.core.dict_mod
use std.os.ui.window.consts (
   BACKEND_NY, EVENT_NONE, EVENT_KEY_PRESSED, EVENT_KEY_RELEASED, EVENT_KEY_CHAR,
   EVENT_MOUSE_BUTTON_PRESSED, EVENT_MOUSE_BUTTON_RELEASED, EVENT_MOUSE_SCROLL, EVENT_MOUSE_POS_CHANGED,
   EVENT_WINDOW_MOVED, EVENT_WINDOW_RESIZED, EVENT_FOCUS_IN, EVENT_FOCUS_OUT, EVENT_MOUSE_ENTER,
   EVENT_MOUSE_LEAVE, EVENT_WINDOW_REFRESH, EVENT_QUIT, EVENT_DATA_DROP, EVENT_DATA_DRAG,
   EVENT_WINDOW_MAXIMIZED, EVENT_WINDOW_MINIMIZED, EVENT_WINDOW_RESTORED, EVENT_SCALE_UPDATED,
   EVENT_MONITOR_CONNECTED, EVENT_MONITOR_DISCONNECTED, WINDOW_NORMAL, WINDOW_NO_BORDER, WINDOW_NO_RESIZE,
   WINDOW_ALLOW_DND, WINDOW_HIDE_MOUSE, WINDOW_FULLSCREEN, WINDOW_TRANSPARENT, WINDOW_CENTER,
   WINDOW_RAW_MOUSE, WINDOW_SCALE_TO_MONITOR, WINDOW_HIDE, WINDOW_MAXIMIZE, WINDOW_CENTER_CURSOR,
   WINDOW_FLOATING, WINDOW_FOCUS_ON_SHOW, WINDOW_MINIMIZE, WINDOW_FOCUS, WINDOW_CAPTURE_MOUSE, WINDOW_CPU,
   WINDOW_VULKAN, KEY_NULL, KEY_ESCAPE, KEY_ESC, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_SPACE, KEY_UP,
   KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END, KEY_INSERT, KEY_DELETE,
   KEY_CAPS_LOCK, KEY_SCROLL_LOCK, KEY_NUM_LOCK, KEY_PRINT_SCREEN, KEY_PAUSE, KEY_MENU, KEY_F1, KEY_F2,
   KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_F13, KEY_F14,
   KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_F25,
   KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
   KEY_KP_ADD, KEY_KP_SUBTRACT, KEY_KP_MULTIPLY, KEY_KP_DIVIDE, KEY_KP_DECIMAL, KEY_KP_ENTER,
   KEY_KP_EQUAL, KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL, KEY_LEFT_ALT,
   KEY_RIGHT_ALT, KEY_LEFT_SUPER, KEY_RIGHT_SUPER, KEY_SHIFT, KEY_CTRL, KEY_ALT, KEY_SUPER, KEY_GRAVE,
   KEY_GRAVE_ACCENT, KEY_MINUS, KEY_EQUAL, KEY_APOSTROPHE, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_SEMICOLON,
   KEY_LEFT_BRACKET, KEY_BACKSLASH, KEY_RIGHT_BRACKET, KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
   KEY_7, KEY_8, KEY_9, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K,
   KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y,
   KEY_Z, KEY_WORLD_1, KEY_WORLD_2, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META,
)

use std.os.ui.window.event as ev
use std.os.ui.window.input.key as ui_key
use std.os.ui.window.input.gamepad as ui_gamepad
use std.os (msleep)
use std.os.time
use std.math (abs)
use std.core.str as str
use std.core.common as common
use std.os.ui.render.dump as ui_profile
use std.os.ui.window.platform as ui_backend
use std.os.ui.window.platform.api (
   CLIENT_API, NO_API, CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_DISABLED, ARROW_CURSOR, IBEAM_CURSOR,
   CROSSHAIR_CURSOR, POINTING_HAND_CURSOR, RESIZE_EW_CURSOR, RESIZE_NS_CURSOR, RESIZE_NWSE_CURSOR,
   RESIZE_NESW_CURSOR, RESIZE_ALL_CURSOR, NOT_ALLOWED_CURSOR, JOYSTICK_1, JOYSTICK_2,
   JOYSTICK_3, JOYSTICK_4, JOYSTICK_5, JOYSTICK_6, JOYSTICK_7, JOYSTICK_8, JOYSTICK_9, JOYSTICK_10,
   JOYSTICK_11, JOYSTICK_12, JOYSTICK_13, JOYSTICK_14, JOYSTICK_15, JOYSTICK_16, JOYSTICK_LAST,
   JOYSTICK_HAT_BUTTONS, GAMEPAD_BUTTON_A, GAMEPAD_BUTTON_B, GAMEPAD_BUTTON_X, GAMEPAD_BUTTON_Y,
   GAMEPAD_BUTTON_LEFT_BUMPER, GAMEPAD_BUTTON_RIGHT_BUMPER, GAMEPAD_BUTTON_BACK, GAMEPAD_BUTTON_START,
   GAMEPAD_BUTTON_GUIDE, GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_DPAD_UP,
   GAMEPAD_BUTTON_DPAD_RIGHT, GAMEPAD_BUTTON_DPAD_DOWN, GAMEPAD_BUTTON_DPAD_LEFT, GAMEPAD_BUTTON_LAST,
   GAMEPAD_BUTTON_CROSS, GAMEPAD_BUTTON_CIRCLE, GAMEPAD_BUTTON_SQUARE, GAMEPAD_BUTTON_TRIANGLE,
   GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y,
   GAMEPAD_AXIS_LEFT_TRIGGER, GAMEPAD_AXIS_RIGHT_TRIGGER, GAMEPAD_AXIS_LAST,
)

use std.os.ui.window.native

def _MOD_MASK = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META
def _KEY_POLL_COUNT = 120

fn _key_poll_at(i32 idx) i32 {
   case idx {
      0 -> KEY_ESCAPE
      1 -> KEY_ENTER
      2 -> KEY_TAB
      3 -> KEY_BACKSPACE
      4 -> KEY_SPACE
      5 -> KEY_GRAVE_ACCENT
      6 -> KEY_INSERT
      7 -> KEY_DELETE
      8 -> KEY_WORLD_1
      9 -> KEY_WORLD_2
      10 -> KEY_LEFT
      11 -> KEY_UP
      12 -> KEY_RIGHT
      13 -> KEY_DOWN
      14 -> KEY_PAGE_UP
      15 -> KEY_PAGE_DOWN
      16 -> KEY_HOME
      17 -> KEY_END
      18..42 -> KEY_F1 + (idx - 18)
      43 -> KEY_LEFT_SHIFT
      44 -> KEY_RIGHT_SHIFT
      45 -> KEY_LEFT_CONTROL
      46 -> KEY_RIGHT_CONTROL
      47 -> KEY_LEFT_ALT
      48 -> KEY_RIGHT_ALT
      49 -> KEY_LEFT_SUPER
      50 -> KEY_RIGHT_SUPER
      51 -> KEY_MENU
      52..77 -> KEY_A + (idx - 52)
      78..87 -> KEY_0 + (idx - 78)
      88 -> KEY_COMMA
      89 -> KEY_PERIOD
      90 -> KEY_SLASH
      91 -> KEY_SEMICOLON
      92 -> KEY_EQUAL
      93 -> KEY_MINUS
      94 -> KEY_APOSTROPHE
      95 -> KEY_LEFT_BRACKET
      96 -> KEY_RIGHT_BRACKET
      97 -> KEY_BACKSLASH
      98 -> KEY_CAPS_LOCK
      99 -> KEY_SCROLL_LOCK
      100 -> KEY_NUM_LOCK
      101 -> KEY_PRINT_SCREEN
      102 -> KEY_PAUSE
      103..112 -> KEY_KP_0 + (idx - 103)
      113 -> KEY_KP_DECIMAL
      114 -> KEY_KP_DIVIDE
      115 -> KEY_KP_MULTIPLY
      116 -> KEY_KP_SUBTRACT
      117 -> KEY_KP_ADD
      118 -> KEY_KP_ENTER
      119 -> KEY_KP_EQUAL
      _ -> KEY_NULL
   }
}

mut _windows = 0
mut _window_registry = 0
mut _backend_ready = false
mut _raw_key_query_cache = dict(32)
mut _scancode_query_cache = dict(32)
mut _wayland_scroll_gain_cache = -1.0
mut _xwayland_scroll_fix_cache = -1
mut _wayland_scroll_fix_cache = -1
mut _window_vsync_enabled = false

fn _cache_raw_key_pair(int nk, int a, int b) list {
   def out = [a, b]
   _raw_key_query_cache[nk] = out
   out
}

fn _is_debug() bool { "Internal: Checks if UI debug logging is enabled via environment variables." ui_profile.debug_enabled() }

fn _dbg(any msg) any { if(_is_debug()){ ui_profile.print_text("[ui:window] " + msg) } }

fn _dbg_win(any win, any msg) any { if(_is_debug()){ ui_profile.print_text("[ui:window] win=0x" + str.to_hex(win) + " " + msg) } }

fn _ensure_backend_ready() bool {
   if(_backend_ready){ return _backend_ready }
   if(!ui_backend.init()){
      _dbg("ERROR: backend init failed")
      return false
   }
   _backend_ready = true
   true
}

fn backend() str { "Returns the name of the active windowing platform(e.g., 'x11', 'wayland', 'win32', 'cocoa')." return ui_backend.get_backend_name() }

fn available() bool { "Checks if the windowing system is available on the current platform." true }

fn _is_window(any win) bool { "Internal: Type check for window dictionary." is_dict(win) && win.contains("handle") }

fn _ensure_windows() list {
   if(!is_list(_windows)){ _windows = [] }
   _windows
}

fn _ensure_window_registry() dict {
   if(!is_dict(_window_registry)){ _window_registry = dict(16) }
   _window_registry
}

fn _set_window_registry(any handle, any win) any {
   _window_registry = _ensure_window_registry()
   _window_registry[handle] = win
   win
}

fn _get_handle(any win) any {
   if(is_dict(win)){ return win.get("handle", 0) }
   win
}

fn _get_win(any win) any {
   def h = _get_handle(win)
   if(!h){ return win }
   def real = _ensure_window_registry().get(h, 0)
   if(real){ return real }
   win
}

fn _save_win(any win) any {
   def h = _get_handle(win)
   if(h){
      win["_live"] = true
      _set_window_registry(h, win)
   }
}

fn _remove_win(any win) bool {
   def h = _get_handle(win)
   if(!h){ return false }
   _window_registry = _ensure_window_registry().delete(h)
   mut next = []
   def wins = _ensure_windows()
   mut i = 0
   def wins_n = wins.len
   while(i < wins_n){
      def cand = wins.get(i, 0)
      if(_get_handle(cand) != h){ next = next.append(cand) }
      i += 1
   }
   _windows = next
   true
}

fn _sync_live_size(any win) any {
   win = _get_win(win)
   if(!_is_window(win)){ return win }
   def h = _get_handle(win)
   if(!h){ return win }
   def sz = ui_backend.get_size(h)
   def nw = int(sz.get(0, win.get("w", 0)))
   def nh = int(sz.get(1, win.get("h", 0)))
   if(nw > 0 && nh > 0 &&
      (nw != win.get("w", 0) || nh != win.get("h", 0))){
      win["w"] = nw
      win["h"] = nh
      _save_win(win)
   }
   win
}

fn _sync_live_pos(any win) any {
   win = _get_win(win)
   if(!_is_window(win)){ return win }
   if(ui_backend.get_backend_name() == "wayland"){ return win }
   def h = _get_handle(win)
   if(!h){ return win }
   def pos = ui_backend.get_pos(h)
   def nx = int(pos.get(0, win.get("x", 0)))
   def ny = int(pos.get(1, win.get("y", 0)))
   if(nx != win.get("x", 0) || ny != win.get("y", 0)){
      win["x"] = nx
      win["y"] = ny
      _save_win(win)
   }
   win
}

fn _seq_match(any a, any b, bool allow_prefix=false) bool {
   if(!is_list(a) || !is_list(b)){ return false }
   if(allow_prefix){ if(a.len >= b.len){ return false } }
   elif(a.len != b.len){ return false }
   def a_n = a.len
   mut i = 0 while(i < a_n){
      def sa = a.get(i) def sb = b.get(i)
      if(sa.get(0) != sb.get(0) || (sa.get(1) & _MOD_MASK) != (sb.get(1) & _MOD_MASK)){ return false }
      i += 1
   }
   true
}

fn _normalize_mod(any mod) int { "Internal: Normalize modifier bits." ui_key.normalize_mod(mod) }

fn _normalize_key(any key) int { "Internal: Normalize physical key code." ui_key.normalize_key(key) }

fn _mod_bit_for_key(any key) int { "Internal: Returns the modifier bit associated with a physical key." ui_key.mod_bit_for_key(key) }

fn _mods_from_key_states(any ks) int { "Internal: Calculates active modifier bits from a key state dictionary." ui_key.mods_from_key_states(ks) }

fn _parse_notation(str notation) list { "Internal: Parses a key notation string like 'Ctrl+Shift+A'." ui_key.parse_notation(notation) }

fn _normalize_backend_mods(any mods) int {
   "Internal: Normalizes backend modifier bitmasks to Nytrix modifier bitmasks."
   mut m = 0
   if((mods & 0x0001) != 0){ m = m | MOD_SHIFT }
   if((mods & 0x0002) != 0){ m = m | MOD_CONTROL }
   if((mods & 0x0004) != 0){ m = m | MOD_ALT }
   if((mods & 0x0008) != 0){ m = m | MOD_SUPER }
   m
}

fn _raw_keys_for_query(any key) list {
   def nk = _normalize_key(key)
   def cached = _raw_key_query_cache.get(nk, 0)
   if(is_list(cached)){ return cached }
   case nk {
      KEY_LEFT_SHIFT -> { return _cache_raw_key_pair(nk, KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT) }
      KEY_LEFT_CONTROL -> { return _cache_raw_key_pair(nk, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL) }
      KEY_LEFT_ALT -> { return _cache_raw_key_pair(nk, KEY_LEFT_ALT, KEY_RIGHT_ALT) }
      KEY_LEFT_SUPER -> { return _cache_raw_key_pair(nk, KEY_LEFT_SUPER, KEY_RIGHT_SUPER) }
      _ -> {}
   }
   def out = [nk]
   _raw_key_query_cache[nk] = out
   out
}

fn _backend_key_down(any win, any key) bool {
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def handle = win.get("handle", 0)
   if(!handle){ return false }
   def raw_keys = _raw_keys_for_query(key)
   def raw_keys_n = raw_keys.len
   mut i = 0
   while(i < raw_keys_n){
      if(ui_backend.get_key(handle, raw_keys.get(i)) == 1){ return true }
      i += 1
   }
   false
}

fn _event_scancode(any data) int {
   if(!is_dict(data)){ return 0 }
   int(data.get("scancode", data.get("raw_key", 0)))
}

fn _normalize_key_event_data(any data) dict {
   mut out = is_dict(data) ? data : dict(8)
   mut raw_key = KEY_NULL
   if(is_dict(data)){ raw_key = data.get("key", data.get("raw_key", KEY_NULL)) }
   elif(is_int(data) || is_float(data)){ raw_key = int(data) }
   def sc = _event_scancode(data)
   mut k = _normalize_key(raw_key)
   if(k == KEY_NULL && sc > 0){
      def fk = _function_key_from_scancode(sc)
      if(fk > 0){ k = fk }
   }
   out["key"] = k
   if(sc > 0){ out["scancode"] = sc }
   def mod = is_dict(data) ? _normalize_mod(data.get("mod", data.get("mods", 0))) : 0
   out["mod"] = mod
   out["mods"] = mod
   out
}

fn _apply_key_event_state(any win, int typ, any data) dict {
   win = _get_win(win)
   def out = _normalize_key_event_data(data)
   if(!_is_window(win)){ return out }
   def k = int(out.get("key", KEY_NULL))
   def sc = int(out.get("scancode", 0))
   def is_press = typ == EVENT_KEY_PRESSED
   mut ks = win.get("key_states", 0)
   mut scs = win.get("scancode_states", 0)
   if(!is_dict(ks)){ ks = dict(256) }
   if(!is_dict(scs)){ scs = dict(256) }
   if(k != KEY_NULL){ ks[k] = is_press }
   if(sc > 0){ scs[sc] = is_press }
   if(is_press && k != KEY_NULL){
      mut pk = win.get("pressed_keys", 0)
      if(!is_dict(pk)){ pk = dict(256) }
      pk[k] = true
      win["pressed_keys"] = pk
   }
   win["key_states"] = ks
   win["scancode_states"] = scs
   def event_mod = _normalize_mod(out.get("mod", out.get("mods", 0)))
   def mod = _normalize_mod(_mods_from_key_states(ks) | event_mod)
   win["modifiers"] = mod
   out["mod"] = mod
   out["mods"] = mod
   win["_input_dirty"] = true
   if(is_press && k != KEY_NULL && k == exit_key(win)){ win["should_close"] = true }
   _save_win(win)
   out
}

fn _append_scancode_unique(any out, any seen, int sc) list {
   if(sc > 0 && !seen.get(sc, false)){
      out = out.append(sc)
      seen[sc] = true
   }
   [out, seen]
}

fn _cache_scancode_pair(int nk, int a, int b) list {
   mut out = []
   mut seen = dict(8)
   def r0 = _append_scancode_unique(out, seen, ui_backend.get_key_scancode(a))
   out, seen = r0.get(0, out), r0.get(1, seen)
   def r1 = _append_scancode_unique(out, seen, ui_backend.get_key_scancode(b))
   out = r1.get(0, out)
   _scancode_query_cache[nk] = out
   out
}

fn _append_function_scancode_for_index(any out, any seen, int idx, str b) list {
   def any_backend = b == "" || b == "none"
   mut res = [out, seen]
   if(any_backend || b == "x11"){
      if(idx >= 0 && idx <= 9){ res = _append_scancode_unique(res.get(0), res.get(1), 67 + idx) }
      elif(idx == 10){ res = _append_scancode_unique(res.get(0), res.get(1), 95) }
      elif(idx == 11){ res = _append_scancode_unique(res.get(0), res.get(1), 96) }
   }
   if(any_backend || b == "wayland"){
      if(idx >= 0 && idx <= 9){ res = _append_scancode_unique(res.get(0), res.get(1), 59 + idx) }
      elif(idx == 10){ res = _append_scancode_unique(res.get(0), res.get(1), 87) }
      elif(idx == 11){ res = _append_scancode_unique(res.get(0), res.get(1), 88) }
      elif(idx >= 12 && idx <= 23){ res = _append_scancode_unique(res.get(0), res.get(1), 183 + (idx - 12)) }
   }
   if(any_backend || b == "win32"){
      if(idx >= 0 && idx <= 9){ res = _append_scancode_unique(res.get(0), res.get(1), 0x3B + idx) }
      elif(idx == 10){ res = _append_scancode_unique(res.get(0), res.get(1), 0x57) }
      elif(idx == 11){ res = _append_scancode_unique(res.get(0), res.get(1), 0x58) }
      elif(idx >= 12 && idx <= 22){ res = _append_scancode_unique(res.get(0), res.get(1), 0x64 + (idx - 12)) }
      if(idx >= 0 && idx <= 23){ res = _append_scancode_unique(res.get(0), res.get(1), 0x70 + idx) }
   }
   if(any_backend || b == "cocoa"){
      case idx {
         0 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x7A) }
         1 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x78) }
         2 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x63) }
         3 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x76) }
         4 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x60) }
         5 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x61) }
         6 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x62) }
         7 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x64) }
         8 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x65) }
         9 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x6D) }
         10 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x67) }
         11 -> { res = _append_scancode_unique(res.get(0), res.get(1), 0x6F) }
         _ -> {}
      }
   }
   res
}

fn _function_key_range(int code, int first, int last, int out_first) int {
   if(code >= first && code <= last){ return out_first + (code - first) }
   0
}

fn _function_key_from_scancode(int code) int {
   mut fk = _function_key_range(code, 67, 76, KEY_F1)
   if(fk > 0){ return fk }
   if(code == 95){ return KEY_F11 }
   if(code == 96){ return KEY_F12 }
   fk = _function_key_range(code, 59, 68, KEY_F1)
   if(fk > 0){ return fk }
   if(code == 87){ return KEY_F11 }
   if(code == 88){ return KEY_F12 }
   fk = _function_key_range(code, 183, 194, KEY_F13)
   if(fk > 0){ return fk }
   fk = _function_key_range(code, 0x3B, 0x44, KEY_F1)
   if(fk > 0){ return fk }
   if(code == 0x57){ return KEY_F11 }
   if(code == 0x58){ return KEY_F12 }
   fk = _function_key_range(code, 0x64, 0x6E, KEY_F13)
   if(fk > 0){ return fk }
   fk = _function_key_range(code, 0x70, 0x87, KEY_F1)
   if(fk > 0){ return fk }
   case code {
      0x7A -> KEY_F1
      0x78 -> KEY_F2
      0x63 -> KEY_F3
      0x76 -> KEY_F4
      0x60 -> KEY_F5
      0x61 -> KEY_F6
      0x62 -> KEY_F7
      0x64 -> KEY_F8
      0x65 -> KEY_F9
      0x6D -> KEY_F10
      0x67 -> KEY_F11
      0x6F -> KEY_F12
      _ -> 0
   }
}

fn _scancodes_for_query(any key) list {
   def nk = _normalize_key(key)
   def cached = _scancode_query_cache.get(nk, 0)
   if(is_list(cached)){ return cached }
   case nk {
      KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT -> { return _cache_scancode_pair(nk, KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT) }
      KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL -> { return _cache_scancode_pair(nk, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL) }
      KEY_LEFT_ALT, KEY_RIGHT_ALT -> { return _cache_scancode_pair(nk, KEY_LEFT_ALT, KEY_RIGHT_ALT) }
      KEY_LEFT_SUPER, KEY_RIGHT_SUPER -> { return _cache_scancode_pair(nk, KEY_LEFT_SUPER, KEY_RIGHT_SUPER) }
      _ -> {}
   }
   mut out = []
   mut seen = dict(8)
   def r = _append_scancode_unique(out, seen, ui_backend.get_key_scancode(nk))
   out, seen = r.get(0, out), r.get(1, seen)
   if(nk >= KEY_F1 && nk <= KEY_F1 + 24){
      def fr = _append_function_scancode_for_index(out, seen, nk - KEY_F1, ui_backend.get_backend_name())
      out = fr.get(0, out)
   }
   _scancode_query_cache[nk] = out
   out
}

fn _backend_mouse_down(any win, int button) bool {
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def handle = win.get("handle", 0)
   if(!handle){ return false }
   ui_backend.get_mouse_button(handle, button) == 1
}

fn _backend_mouse_pos(any win) list {
   win = _get_win(win)
   if(!_is_window(win)){ return [win.get("mouse_x", 0), win.get("mouse_y", 0)] }
   if(ui_backend.uses_native_events()){
      return [
         float(win.get("mouse_x", 0)),
         float(win.get("mouse_y", 0))
      ]
   }
   def handle = win.get("handle", 0)
   if(!handle){ return [win.get("mouse_x", 0), win.get("mouse_y", 0)] }
   def cur = ui_backend.get_cursor_pos(handle)
   [float(cur.get(0, win.get("mouse_x", 0))), float(cur.get(1, win.get("mouse_y", 0)))]
}

fn _key_cb(ptr h, i32 k, i32 sc, i32 act, i32 mods) any {
   mut win = _get_win(h)
   if(_is_window(win)){
      if(!win.get("has_key_cb", false)){ win["has_key_cb"] = true }
      def nm = _normalize_backend_mods(mods)
      def nk = _normalize_key(k)
      def data = {"raw_key": k, "key": nk, "scancode": sc, "action": act, "mod": nm, "mods": nm}
      win["modifiers"] = nm
      mut ks = win.get("key_states", 0)
      mut scs = win.get("scancode_states", 0)
      if(!is_dict(ks)){ ks = dict(256) }
      if(!is_dict(scs)){ scs = dict(256) }
      if(act == 1 || act == 2){
         ks[nk] = true
         if(sc > 0){ scs[sc] = true }
      } elif(act == 0){
         ks[nk] = false
         if(sc > 0){ scs[sc] = false }
      }
      win["key_states"] = ks
      win["scancode_states"] = scs
      win["_input_dirty"] = true
      _save_win(win)
      if(act == 1 || act == 2){ push_event(win, EVENT_KEY_PRESSED, data) }
      elif(act == 0){ push_event(win, EVENT_KEY_RELEASED, data) }
   }
}

fn _char_cb(ptr h, u32 c) any {
   mut win = _get_win(h)
   if(_is_window(win)){
      if(!win.get("has_char_cb", false)){
         win["has_char_cb"] = true
         _save_win(win)
      }
      def data = {"char": c, "mod": win.get("modifiers", 0)}
      push_event(win, EVENT_KEY_CHAR, data)
   }
}

fn _size_cb(ptr h, i32 w, i32 h2) any {
   mut win = _get_win(h)
   if(_is_window(win)){
      win["w"] = w
      win["h"] = h2
      _save_win(win)
      push_event(win, EVENT_WINDOW_RESIZED, {"w": w, "h": h2})
   }
}

fn _pos_cb(ptr h, i32 x, i32 y) any {
   mut win = _get_win(h)
   if(_is_window(win)){
      win["x"] = x
      win["y"] = y
      _save_win(win)
      push_event(win, EVENT_WINDOW_MOVED, {"x": x, "y": y})
   }
}

fn _normalize_scroll_component(f64 v) f64 {
   mut out = float(v)
   def a = abs(out)
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
   def a = abs(out)
   if(a <= 0.0){ return out }
   def gain = _wayland_scroll_gain()
   if(a < 0.35){
      out = out * gain
      def ao = abs(out)
      if(ao < 0.35){ out = (out < 0.0) ? -0.35 : 0.35 }
      if(ao > 3.0){ out = (out < 0.0) ? -3.0 : 3.0 }
   }
   out
}

fn _normalize_scroll_data(any data) any {
   if(!is_dict(data)){ return data }
   def b = ui_backend.get_backend_name()
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

fn _scroll_cb(ptr h, f64 f_xoff, f64 f_yoff) any {
   mut win = _get_win(h)
   if(_is_window(win)){
      win["scroll_dx"] = win.get("scroll_dx", 0.0) + f_xoff
      win["scroll_dy"] = win.get("scroll_dy", 0.0) + f_yoff
      win["scroll_x"] = win.get("scroll_x", 0.0) + f_xoff
      win["scroll_y"] = win.get("scroll_y", 0.0) + f_yoff
      _save_win(win)
      mut data = _normalize_scroll_data({"dx": f_xoff, "dy": f_yoff})
      data["scrolling"] = true
      data["mod"] = win.get("modifiers", 0)
      push_event(win, EVENT_MOUSE_SCROLL, data)
   }
}

fn _mouse_btn_cb(ptr h, i32 btn, i32 act, i32 mods) any {
   mut win = _get_win(h)
   if(_is_window(win)){
      def mx, my = win.get("mouse_x", 0), win.get("mouse_y", 0)
      def data = {"button": btn, "x": mx, "y": my, "mod": mods}
      win["modifiers"] = mods
      _save_win(win)
      if(act == 1){ push_event(win, EVENT_MOUSE_BUTTON_PRESSED, data) }
      elif(act == 0){ push_event(win, EVENT_MOUSE_BUTTON_RELEASED, data) }
   }
}

fn _cursor_pos_cb(ptr h, f64 dx, f64 dy) any {
   mut win = _get_win(h)
   if(_is_window(win)){
      def lx, ly = win.get("mouse_x", 0), win.get("mouse_y", 0)
      def moved = (int(dx) != lx) || (int(dy) != ly)
      win["mouse_x"] = int(dx)
      win["mouse_y"] = int(dy)
      _save_win(win)
      def data = {"x": int(dx), "y": int(dy), "dx": int(dx) - lx, "dy": int(dy) - ly, "moved": moved, "mod": win.get("modifiers", 0)}
      push_event(win, EVENT_MOUSE_POS_CHANGED, data)
   }
}

fn open_window(str name, int x, int y, int w, int h, int flags=0) any {
   "Creates and opens a new system window."
   if(!_ensure_backend_ready()){ return false }
   if(!is_str(name)){ name = to_str(name) }
   if(w < 1){ w = 1 } if(h < 1){ h = 1 }
   if(common.env_truthy("NY_UI_HEADLESS")){
      flags = flags | WINDOW_NO_RESIZE
      flags = flags | WINDOW_HIDE
      _dbg("open_window: NY_UI_HEADLESS forcing flags=0x" + str.to_hex(flags) + " pos=(" + to_str(x) + "," + to_str(y) + ")")
   }
   mut handle = ui_backend.create_window(name, x, y, w, h, flags)
   if(!handle){
      _dbg("ERROR: open_window backend create failed")
      return false
   }
   _dbg_win(handle, "backend window created")
   mut win = {
      "handle": handle, "title": name, "x": x, "y": y, "w": w, "h": h,
      "flags": flags, "should_close": false, "exit_key": KEY_ESCAPE,
      "events": [], "events_head": 0,
      "key_states": dict(256), "scancode_states": dict(256), "last_key_states": dict(256), "pressed_keys": dict(256),
      "_input_dirty": false, "mouse_x": 0, "mouse_y": 0,
      "mouse_buttons": dict(32), "last_mouse_buttons": dict(32), "pressed_buttons": dict(32),
      "chord_seq": [], "chord_time": 0, "bindings": [], "modifiers": 0,
      "key_polling": false, "has_char_cb": false
   }
   if(handle){
      _set_window_registry(handle, win)
      def native_events = ui_backend.uses_native_events()
      win["has_key_cb"] = native_events
      if(!native_events){
         ui_backend.set_char_callback(handle, _char_cb)
         ui_backend.set_key_callback(handle, _key_cb)
         ui_backend.set_window_size_callback(handle, _size_cb)
         ui_backend.set_window_pos_callback(handle, _pos_cb)
         ui_backend.set_scroll_callback(handle, _scroll_cb)
         ui_backend.set_mouse_button_callback(handle, _mouse_btn_cb)
         ui_backend.set_cursor_pos_callback(handle, _cursor_pos_cb)
      }
   }
   win = _sync_live_size(win)
   win = _sync_live_pos(win)
   _windows = _ensure_windows().append(win)
   win
}

fn window_hint(any hint, any value) any { "Sets a window hint for upcoming window creations." ui_backend.window_hint(hint, value) }

fn window_hint_string(any hint, str value) any { "Sets a string window hint for upcoming window creations." ui_backend.window_hint_string(hint, value) }

fn create(int w, int h, str name, int flags=0) any { "Common shortcut for creating a centered window." open_window(name, 0, 0, w, h, flags) }

fn id(any win) any { "Returns the low-level platform handle(ID) for the window." if(!_is_window(win)){ return 0 } win.get("handle", 0) }

fn key_state(any win, any key) int {
   "Returns the current state of the specified key."
   win = _get_win(win) if(!_is_window(win)){ return 0 }
   def handle = win.get("handle", 0)
   if(handle){ return ui_backend.get_key(handle, key) }
   0
}

fn key_name(any key, int scancode=0) str {
   "Returns the layout-specific name of the specified printable key."
   return ui_backend.get_key_name(key, scancode)
}

fn title(any win) str { "Returns the current title of the window." if(!_is_window(win)){ return "" } return win.get("title", "") }

fn set_title(any win, any t) bool {
   "Updates the window title."
   win = _get_win(win) if(!_is_window(win)){ return false }
   if(!is_str(t)){ t = to_str(t) }
   win["title"] = t
   _save_win(win)
   def h = win.get("handle", 0)
   if(h){
      _dbg_win(h, "set_title '" + t + "'")
      ui_backend.set_title(h, t)
   }
   true
}

fn show(any win) bool {
   "Shows the window."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h){
      _dbg_win(h, "show")
      ui_backend.show_window(h)
      if(ui_backend.get_backend_name() == "win32" && band(int(win.get("flags", 0)), WINDOW_FOCUS_ON_SHOW)){
         ui_backend.focus_window(h)
      }
      win["visible"] = true
      _save_win(win)
      _window_flag_set(win, WINDOW_HIDE, false)
   }
   true
}

fn hide(any win) bool {
   "Hides the window."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h){
      _dbg_win(h, "hide")
      ui_backend.hide_window(h)
      win["visible"] = false
      _save_win(win)
      _window_flag_set(win, WINDOW_HIDE, true)
   }
   true
}

fn iconify(any win) bool {
   "Minimizes the window."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h){
      _dbg_win(h, "iconify")
      ui_backend.iconify_window(h)
      win["minimized"] = true
      win["maximized"] = false
      _save_win(win)
      _window_flag_set(win, WINDOW_MINIMIZE, true)
      _window_flag_set(win, WINDOW_MAXIMIZE, false)
   }
   true
}

fn restore(any win) bool {
   "Restores the window from minimized or maximized state."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h){
      _dbg_win(h, "restore")
      ui_backend.restore_window(h)
      win["visible"] = true
      win["minimized"] = false
      win["maximized"] = false
      _save_win(win)
      _window_flag_set(win, WINDOW_HIDE, false)
      _window_flag_set(win, WINDOW_MINIMIZE, false)
      _window_flag_set(win, WINDOW_MAXIMIZE, false)
   }
   true
}

fn maximize(any win) bool {
   "Maximizes the window."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h){
      _dbg_win(h, "maximize")
      ui_backend.maximize_window(h)
      win["visible"] = true
      win["minimized"] = false
      win["maximized"] = true
      _save_win(win)
      _window_flag_set(win, WINDOW_HIDE, false)
      _window_flag_set(win, WINDOW_MINIMIZE, false)
      _window_flag_set(win, WINDOW_MAXIMIZE, true)
   }
   true
}

fn set_icon(any win, any images) any {
   "Sets the window icon from one or more RGBA8 image dictionaries."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(!h){ return false }
   win["icon_images"] = images
   _save_win(win)
   _dbg_win(h, "set_icon images=" + to_str(images.len))
   ui_backend.set_window_icon(h, images)
}

fn create_cursor(any image, int xhot=0, int yhot=0) any {
   "Creates a custom cursor from one RGBA8 image dictionary."
   ui_backend.create_cursor(image, xhot, yhot)
}

fn create_standard_cursor(any shape) any {
   "Creates a backend-native standard cursor."
   ui_backend.create_standard_cursor(shape)
}

fn destroy_cursor(any cursor) any {
   "Destroys a previously created cursor object."
   ui_backend.destroy_cursor(cursor)
}

fn set_cursor(any win, any cursor) any {
   "Applies a cursor object to the specified window, or clears it when cursor is zero."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(!h){ return false }
   win["cursor"] = cursor
   _save_win(win)
   _dbg_win(h, "set_cursor cursor=0x" + str.to_hex(cursor))
   ui_backend.set_cursor(h, cursor)
}

fn pos(any win) list {
   "Returns [x, y] screen coordinates of the live window."
   win = _sync_live_pos(win)
   if(!_is_window(win)){ return [0,0] }
   [win.get("x", 0), win.get("y", 0)]
}

fn size(any win) list {
   "Returns [width, height] dimensions of the live client area."
   win = _sync_live_size(win)
   if(!_is_window(win)){ return [0,0] }
   [win.get("w", 0), win.get("h", 0)]
}

fn get_framebuffer_size(any win) list {
   "Returns [width, height] dimensions of the live framebuffer in pixels."
   win = _get_win(win)
   if(!_is_window(win)){ return [0,0] }
   def h = win.get("handle", 0)
   if(!h){ return [win.get("w", 0), win.get("h", 0)] }
   ui_backend.get_framebuffer_size(h)
}

fn get_window_content_scale(any win) list {
   "Returns [xscale, yscale] for the live window content area."
   win = _get_win(win)
   if(!_is_window(win)){ return [1.0, 1.0] }
   def h = win.get("handle", 0)
   if(!h){ return [1.0, 1.0] }
   ui_backend.get_window_content_scale(h)
}

fn get_window_scale_dpi(any win) list {
   "Alias for the live window DPI/content scale as [xscale, yscale]."
   get_window_content_scale(win)
}

fn window_flags(any win) int {
   "Returns the facade window flag bitset."
   win = _get_win(win)
   if(!_is_window(win)){ return 0 }
   int(win.get("flags", 0))
}

fn _window_flag_set(any win, int flag, bool enabled) bool {
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   mut flags = int(win.get("flags", 0))
   def has = (flags & flag) != 0
   if(enabled && !has){ flags = flags | flag }
   if(!enabled && has){ flags -= flag }
   win["flags"] = flags
   _save_win(win)
   true
}

fn window_attrib(any win, int attrib) int {
   "Returns a raw backend window attribute value, or 0 when unavailable."
   win = _get_win(win)
   if(!_is_window(win)){ return 0 }
   def h = win.get("handle", 0)
   if(!h){ return 0 }
   int(ui_backend.get_window_attrib(h, attrib))
}

fn _window_bool_attrib(any win, int attrib, str fallback_key, bool fallback) bool {
   win = _get_win(win)
   if(!_is_window(win)){ return fallback }
   def h = win.get("handle", 0)
   if(h){
      def got = int(ui_backend.get_window_attrib(h, attrib))
      if(got != 0){ return true }
   }
   !!win.get(fallback_key, fallback)
}

fn is_window_visible(any win) bool {
   "Returns true when the window is visible according to the backend or facade state."
   _window_bool_attrib(win, ui_backend.VISIBLE, "visible", true)
}

fn is_window_focused(any win) bool {
   "Returns true when the window is focused according to the backend or facade state."
   _window_bool_attrib(win, ui_backend.FOCUSED, "focused", false)
}

fn is_window_minimized(any win) bool {
   "Returns true when the window is minimized/iconified."
   _window_bool_attrib(win, ui_backend.ICONIFIED, "minimized", false)
}

fn is_window_maximized(any win) bool {
   "Returns true when the window is maximized."
   _window_bool_attrib(win, ui_backend.MAXIMIZED, "maximized", false)
}

fn is_window_floating(any win) bool {
   "Returns true when the window is topmost/floating."
   _window_bool_attrib(win, ui_backend.FLOATING, "floating", false)
}

fn is_window_resizable(any win) bool {
   "Returns true when the window can be resized by the user."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h && int(ui_backend.get_window_attrib(h, ui_backend.RESIZABLE)) != 0){ return true }
   !!win.get("resizable", band(int(win.get("flags", 0)), WINDOW_NO_RESIZE) == 0)
}

fn is_window_decorated(any win) bool {
   "Returns true when window decorations(title bar, border) are enabled."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h && int(ui_backend.get_window_attrib(h, ui_backend.DECORATED)) != 0){ return true }
   !!win.get("decorated", band(int(win.get("flags", 0)), WINDOW_NO_BORDER) == 0)
}

fn is_window_fullscreen(any win) bool {
   "Returns true when the window is associated with a fullscreen monitor."
   win = _get_win(win)
   _is_window(win) && !!win.get("fullscreen", false)
}

fn is_window_borderless(any win) bool {
   "Returns true when the facade has placed the window in borderless-windowed mode."
   win = _get_win(win)
   _is_window(win) && !!win.get("borderless", false)
}

fn window_vsync() bool {
   "Returns the current facade-level vsync request."
   _window_vsync_enabled
}

fn set_window_vsync(bool enabled) bool {
   "Requests swap interval 1 or 0 on backends that support it."
   _window_vsync_enabled = !!enabled
   ui_backend.swap_interval(_window_vsync_enabled ? 1 : 0)
   true
}

fn toggle_window_vsync() bool {
   "Toggles the facade-level vsync request and returns the new state."
   set_window_vsync(!_window_vsync_enabled)
   _window_vsync_enabled
}

fn move(any win, int x, int y) bool {
   "Moves the window to [x, y] coordinates."
   win = _get_win(win) if(!_is_window(win)){ return false }
   win["x"] = x
   win["y"] = y
   _save_win(win)
   def h = win.get("handle", 0)
   if(h){
      _dbg_win(h, "move pos=" + to_str(x) + "," + to_str(y))
      ui_backend.set_pos(h, x, y)
      win = _sync_live_pos(win)
   }
   push_event(win, EVENT_WINDOW_MOVED, {"x": win.get("x", x), "y": win.get("y", y)})
}

fn resize(any win, int w, int h) bool {
   "Resizes the window to [w, h] pixels."
   win = _get_win(win) if(!_is_window(win)){ return false }
   if(w < 1){ w = 1 } if(h < 1){ h = 1 }
   win["w"] = w
   win["h"] = h
   _save_win(win)
   def hh = win.get("handle", 0)
   if(hh){
      _dbg_win(hh, "resize size=" + to_str(w) + "x" + to_str(h))
      ui_backend.set_size(hh, w, h)
      win = _sync_live_size(win)
   }
   push_event(win, EVENT_WINDOW_RESIZED, {"w": win.get("w", w), "h": win.get("h", h)})
}

fn should_close(any win) bool {
   "Returns true if the window has been requested to close."
   win = _get_win(win) if(!_is_window(win)){ return true }
   !!win.get("should_close", false)
}

fn _drop_queued_event_type(any win, int typ) any {
   if(!_is_window(win)){ return win }
   mut q = win.get("events", [])
   mut head = int(win.get("events_head", 0))
   if(head < 0){ head = 0 }
   if(head >= q.len){
      win["events"] = []
      win["events_head"] = 0
      return win
   }
   mut out = []
   mut i = head
   def q_n = q.len
   while(i < q_n){
      def e = q.get(i)
      if(!ev.is_event(e) || ev.event_type(e) != typ){ out = out.append(e) }
      i += 1
   }
   win["events"] = out
   win["events_head"] = 0
   win
}

fn set_should_close(any win, bool sc=true) bool {
   "Sets the window close flag manually."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def old = !!win.get("should_close", false)
   win["should_close"] = !!sc
   if(!sc){ win = _drop_queued_event_type(win, 15) }
   _save_win(win)
   if(!!sc && !old){ push_event(win, 15) }
   def h = win.get("handle", 0)
   if(h){ ui_backend.set_should_close(h, !!sc ? 1 : 0) }
   true
}

fn close(any win) bool {
   "Closes and destroys the window, releasing backend state."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   win["should_close"] = true
   if(h){
      _save_win(win)
      ui_backend.set_should_close(h, true)
      ui_backend.destroy_window(h)
   }
   _remove_win(win)
   true
}

fn exit_key(any win) int { "Returns the current exit(close) key for the window." win = _get_win(win) if(!_is_window(win)){ return KEY_NULL } win.get("exit_key", KEY_ESCAPE) }

fn set_exit_key(any win, int k) bool { "Changes the exit(close) key for the window." win = _get_win(win) if(_is_window(win)){ win["exit_key"] = k _save_win(win) true } else { false } }

fn get_monitors() list {
   "Returns the currently connected monitors from the active backend."
   ui_backend.get_monitors()
}

fn get_primary_monitor() any {
   "Returns the primary monitor from the active backend."
   ui_backend.get_primary_monitor()
}

fn get_monitor_pos(any monitor) list {
   "Returns `[x, y]` for a monitor."
   ui_backend.get_monitor_pos(monitor)
}

fn get_monitor_workarea(any monitor) list {
   "Returns `[x, y, width, height]` for a monitor work area."
   ui_backend.get_monitor_workarea(monitor)
}

fn get_monitor_physical_size(any monitor) list {
   "Returns `[width_mm, height_mm]` for a monitor."
   ui_backend.get_monitor_physical_size(monitor)
}

fn get_monitor_content_scale(any monitor) list {
   "Returns `[xscale, yscale]` for a monitor."
   ui_backend.get_monitor_content_scale(monitor)
}

fn get_monitor_name(any monitor) str {
   "Returns the monitor name."
   return ui_backend.get_monitor_name(monitor)
}

fn get_video_mode(any monitor) any {
   "Returns the current video mode for a monitor."
   ui_backend.get_video_mode(monitor)
}

fn get_video_modes(any monitor) list {
   "Returns all video modes for a monitor."
   ui_backend.get_video_modes(monitor)
}

fn get_monitor_resolution(any monitor) list {
   "Returns [width, height, refresh_rate] for a monitor."
   def mode = get_video_mode(monitor)
   def fallback_w = is_dict(monitor) ? monitor.get("width", 0) : 0
   def fallback_h = is_dict(monitor) ? monitor.get("height", 0) : 0
   def fallback_hz = is_dict(monitor) ? monitor.get("refresh_rate", 0) : 0
   if(is_dict(mode)){
      return [
         max(1, int(mode.get("width", fallback_w))),
         max(1, int(mode.get("height", fallback_h))),
         int(mode.get("refresh_rate", fallback_hz))
      ]
   }
   [max(1, int(fallback_w)), max(1, int(fallback_h)), int(fallback_hz)]
}

fn get_monitor_rect(any monitor) list {
   "Returns [x, y, width, height] for a monitor."
   def p = get_monitor_pos(monitor)
   def r = get_monitor_resolution(monitor)
   def fallback_x = is_dict(monitor) ? monitor.get("x", 0) : 0
   def fallback_y = is_dict(monitor) ? monitor.get("y", 0) : 0
   [
      int(p.get(0, fallback_x)),
      int(p.get(1, fallback_y)),
      max(1, int(r.get(0, 1))),
      max(1, int(r.get(1, 1)))
   ]
}

fn get_monitor_desktop_bounds(any monitors=0) list {
   "Returns [min_x, min_y, max_x, max_y] covering all monitor rectangles."
   if(!is_list(monitors)){ monitors = get_monitors() }
   if(monitors.len == 0){ return [0, 0, 1, 1] }
   def first = get_monitor_rect(monitors.get(0))
   mut min_x = int(first.get(0, 0))
   mut min_y = int(first.get(1, 0))
   mut max_x = min_x + max(1, int(first.get(2, 1)))
   mut max_y = min_y + max(1, int(first.get(3, 1)))
   mut i = 1
   while(i < monitors.len){
      def r = get_monitor_rect(monitors.get(i))
      def x = int(r.get(0, 0))
      def y = int(r.get(1, 0))
      def w = max(1, int(r.get(2, 1)))
      def h = max(1, int(r.get(3, 1)))
      if(x < min_x){ min_x = x }
      if(y < min_y){ min_y = y }
      if(x + w > max_x){ max_x = x + w }
      if(y + h > max_y){ max_y = y + h }
      i += 1
   }
   [min_x, min_y, max_x, max_y]
}

fn get_monitor_index_at(any x, any y, any monitors=0) int {
   "Returns the monitor index containing point [x, y], or -1."
   if(!is_list(monitors)){ monitors = get_monitors() }
   def px = float(x)
   def py = float(y)
   mut i = 0
   while(i < monitors.len){
      def r = get_monitor_rect(monitors.get(i))
      def rx = float(r.get(0, 0))
      def ry = float(r.get(1, 0))
      def rw = float(max(1, int(r.get(2, 1))))
      def rh = float(max(1, int(r.get(3, 1))))
      if(px >= rx && px < rx + rw && py >= ry && py < ry + rh){ return i }
      i += 1
   }
   -1
}

fn _monitor_distance_to_point(any monitor, f64 x, f64 y) f64 {
   def r = get_monitor_rect(monitor)
   def cx = float(r.get(0, 0)) + float(max(1, int(r.get(2, 1)))) * 0.5
   def cy = float(r.get(1, 0)) + float(max(1, int(r.get(3, 1)))) * 0.5
   def dx = cx - x
   def dy = cy - y
   dx * dx + dy * dy
}

fn get_current_monitor_index(any win, any monitors=0) int {
   "Returns the monitor index containing the window center, falling back to nearest monitor."
   if(!is_list(monitors)){ monitors = get_monitors() }
   if(monitors.len == 0){ return -1 }
   def wp = pos(win)
   def ws = size(win)
   def cx = float(wp.get(0, 0)) + float(ws.get(0, 0)) * 0.5
   def cy = float(wp.get(1, 0)) + float(ws.get(1, 0)) * 0.5
   def hit = get_monitor_index_at(cx, cy, monitors)
   if(hit >= 0){ return hit }
   mut best = 0
   mut best_dist = _monitor_distance_to_point(monitors.get(0), cx, cy)
   mut i = 1
   while(i < monitors.len){
      def dist = _monitor_distance_to_point(monitors.get(i), cx, cy)
      if(dist < best_dist){
         best = i
         best_dist = dist
      }
      i += 1
   }
   best
}

fn get_current_monitor(any win, any monitors=0) any {
   "Returns the monitor containing the window center, falling back to nearest monitor."
   if(!is_list(monitors)){ monitors = get_monitors() }
   def idx = get_current_monitor_index(win, monitors)
   if(idx < 0 || idx >= monitors.len){ return false }
   monitors.get(idx)
}

fn move_to_monitor(any win, any monitor, bool centered=true, bool use_workarea=true) bool {
   "Moves a window to a monitor, centered in its work area by default."
   def ws = size(win)
   def rect = get_monitor_rect(monitor)
   mut x = int(rect.get(0, 0))
   mut y = int(rect.get(1, 0))
   mut w = max(1, int(rect.get(2, 1)))
   mut h = max(1, int(rect.get(3, 1)))
   if(use_workarea){
      def wa = get_monitor_workarea(monitor)
      if(is_list(wa) && wa.len >= 4 && int(wa.get(2, 0)) > 0 && int(wa.get(3, 0)) > 0){
         x = int(wa.get(0, x))
         y = int(wa.get(1, y))
         w = int(wa.get(2, w))
         h = int(wa.get(3, h))
      }
   }
   if(!centered){ return move(win, x, y) }
   def ww = max(1, int(ws.get(0, 800)))
   def wh = max(1, int(ws.get(1, 450)))
   move(win, x + max(0, (w - ww) / 2), y + max(0, (h - wh) / 2))
}

fn set_gamma(any monitor, f64 gamma) any {
   "Generates and applies a gamma ramp for a monitor."
   ui_backend.set_gamma(monitor, gamma)
}

fn get_gamma_ramp(any monitor) any {
   "Returns the current monitor gamma ramp as a dict with `size`, `red`, `green`, and `blue` arrays."
   ui_backend.get_gamma_ramp(monitor)
}

fn set_gamma_ramp(any monitor, any ramp) any {
   "Sets the current monitor gamma ramp from a dict with `size`, `red`, `green`, and `blue` arrays."
   ui_backend.set_gamma_ramp(monitor, ramp)
}

fn get_window_monitor(any win) any {
   "Returns the monitor associated with a fullscreen window."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   ui_backend.get_window_monitor(win.get("handle", 0))
}

fn set_window_monitor(any win, any monitor, int xpos, int ypos, int width, int height, int refresh_rate=0) bool {
   "Sets or clears the monitor/fullscreen association for a window."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def handle = win.get("handle", 0)
   if(!handle){ return false }
   def updated_native = ui_backend.set_window_monitor(handle, monitor, xpos, ypos, width, height, refresh_rate)
   if(is_dict(updated_native)){
      def nx, ny = updated_native.get("x", xpos), updated_native.get("y", ypos)
      def nw, nh = updated_native.get("w", width), updated_native.get("h", height)
      win["x"] = nx
      win["y"] = ny
      win["w"] = nw
      win["h"] = nh
   } else {
      win["x"] = xpos
      win["y"] = ypos
      win["w"] = width
      win["h"] = height
   }
   win["fullscreen"] = !!monitor
   _save_win(win)
   _window_flag_set(win, WINDOW_FULLSCREEN, !!monitor)
   true
}

fn _remember_window_geometry(any win, str prefix) any {
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def p = pos(win)
   def s = size(win)
   win = _get_win(win)
   win[prefix + "_x"] = int(p.get(0, win.get("x", 100)))
   win[prefix + "_y"] = int(p.get(1, win.get("y", 100)))
   win[prefix + "_w"] = max(1, int(s.get(0, win.get("w", 800))))
   win[prefix + "_h"] = max(1, int(s.get(1, win.get("h", 450))))
   _save_win(win)
   win
}

fn set_window_fullscreen(any win, bool enabled, any monitor=0) bool {
   "Toggles fullscreen monitor association while preserving windowed geometry."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   if(!enabled){
      def rx = int(win.get("fullscreen_restore_x", win.get("x", 100)))
      def ry = int(win.get("fullscreen_restore_y", win.get("y", 100)))
      def rw = max(1, int(win.get("fullscreen_restore_w", win.get("w", 800))))
      def rh = max(1, int(win.get("fullscreen_restore_h", win.get("h", 450))))
      return set_window_monitor(win, false, rx, ry, rw, rh, 0)
   }
   if(!is_window_fullscreen(win)){ _remember_window_geometry(win, "fullscreen_restore") }
   if(!monitor){ monitor = get_current_monitor(win) }
   if(!monitor){ return false }
   def rect = get_monitor_rect(monitor)
   def res = get_monitor_resolution(monitor)
   set_window_monitor(
      win, monitor,
      int(rect.get(0, 0)),
      int(rect.get(1, 0)),
      max(1, int(rect.get(2, 1))),
      max(1, int(rect.get(3, 1))),
      int(res.get(2, 0))
   )
}

fn toggle_window_fullscreen(any win, any monitor=0) bool {
   "Toggles fullscreen and returns whether the operation succeeded."
   set_window_fullscreen(win, !is_window_fullscreen(win), monitor)
}

fn set_window_borderless(any win, bool enabled, any monitor=0) bool {
   "Toggles borderless-windowed placement on a monitor while preserving windowed geometry."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   if(!enabled){
      def decorated = !!win.get("borderless_restore_decorated", true)
      def rx = int(win.get("borderless_restore_x", win.get("x", 100)))
      def ry = int(win.get("borderless_restore_y", win.get("y", 100)))
      def rw = max(1, int(win.get("borderless_restore_w", win.get("w", 800))))
      def rh = max(1, int(win.get("borderless_restore_h", win.get("h", 450))))
      win["borderless"] = false
      _save_win(win)
      set_window_decorated(win, decorated)
      move(win, rx, ry)
      resize(win, rw, rh)
      return true
   }
   if(!is_window_borderless(win)){
      _remember_window_geometry(win, "borderless_restore")
      win = _get_win(win)
      win["borderless_restore_decorated"] = is_window_decorated(win)
      win["borderless"] = true
      _save_win(win)
   }
   if(!monitor){ monitor = get_current_monitor(win) }
   if(!monitor){ return false }
   def rect = get_monitor_rect(monitor)
   set_window_decorated(win, false)
   move(win, int(rect.get(0, 0)), int(rect.get(1, 0)))
   resize(win, max(1, int(rect.get(2, 1))), max(1, int(rect.get(3, 1))))
   true
}

fn toggle_window_borderless(any win, any monitor=0) bool {
   "Toggles borderless-windowed mode and returns whether the operation succeeded."
   set_window_borderless(win, !is_window_borderless(win), monitor)
}

fn push_event(any win, int kind, any data=0) bool {
   "Runs the push event operation."
   mut real_win = _is_window(win) ? win : _get_win(win)
   def h = _get_handle(real_win)
   if(!_is_window(real_win)){ return false }
   mut event_data = data
   if(kind == EVENT_WINDOW_RESIZED){
      event_data = _normalize_resize_event_data(real_win, data)
      def nw = event_data.get("w", 0)
      def nh = event_data.get("h", 0)
      if(nw > 0 && nh > 0){
         real_win["w"] = nw
         real_win["h"] = nh
      }
   } elif(kind == EVENT_QUIT){
      real_win["should_close"] = true
   } elif(kind == EVENT_KEY_PRESSED || kind == EVENT_KEY_RELEASED){
      event_data = _apply_key_event_state(real_win, kind, event_data)
      real_win = _get_win(real_win)
   } elif(kind == EVENT_MOUSE_POS_CHANGED){
      event_data = _normalize_mouse_event_data(real_win, kind, data)
      real_win["mouse_x"] = event_data.get("x", real_win.get("mouse_x", 0))
      real_win["mouse_y"] = event_data.get("y", real_win.get("mouse_y", 0))
      real_win["_input_dirty"] = true
   } elif(kind == EVENT_MOUSE_BUTTON_PRESSED || kind == EVENT_MOUSE_BUTTON_RELEASED){
      event_data = _normalize_mouse_event_data(real_win, kind, data)
      def btn = event_data.get("button", 0)
      def is_press = kind == EVENT_MOUSE_BUTTON_PRESSED
      mut mb = real_win.get("mouse_buttons", 0)
      if(!is_dict(mb)){ mb = dict(32) }
      mb[btn] = is_press
      real_win["mouse_buttons"] = mb
      if(is_press){
         mut pb = real_win.get("pressed_buttons", 0)
         if(!is_dict(pb)){ pb = dict(32) }
         pb[btn] = true
         real_win["pressed_buttons"] = pb
      }
      real_win["mouse_x"] = event_data.get("x", real_win.get("mouse_x", 0))
      real_win["mouse_y"] = event_data.get("y", real_win.get("mouse_y", 0))
      real_win["_input_dirty"] = true
   } elif(kind == EVENT_DATA_DROP || kind == EVENT_DATA_DRAG){
      event_data = _normalize_drop_event_data(real_win, kind, data)
      real_win["mouse_x"] = event_data.get("x", real_win.get("mouse_x", 0))
      real_win["mouse_y"] = event_data.get("y", real_win.get("mouse_y", 0))
   }
   mut q = real_win.get("events", [])
   mut head = int(real_win.get("events_head", 0))
   if(head >= q.len){ q = [] head = 0 }
   elif(head > 64 && head * 2 >= q.len){
      q = slice(q, head, q.len, 1)
      head = 0
   }
   q = ev.queue_push(q, ev.make_event(kind, real_win, h, event_data))
   real_win["events"] = q
   real_win["events_head"] = head
   _save_win(real_win)
   true
}

fn _normalize_mouse_event_data(any win, int typ, any data) any {
   if(is_dict(data)){
      mut out = data
      if(!out.contains("x")){ out["x"] = win.get("mouse_x", 0) }
      if(!out.contains("y")){ out["y"] = win.get("mouse_y", 0) }
      if(!out.contains("mod")){ out["mod"] = win.get("modifiers", 0) }
      if((typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED) && !out.contains("button")){ out["button"] = 0 }
      if(typ == EVENT_MOUSE_SCROLL){
         if(!out.contains("dx")){ out["dx"] = 0.0 }
         if(!out.contains("dy")){ out["dy"] = 0.0 }
         out["scrolling"] = true
      }
      return out
   }
   mut out = dict(8)
   out["x"] = win.get("mouse_x", 0)
   out["y"] = win.get("mouse_y", 0)
   out["mod"] = win.get("modifiers", 0)
   if(typ == EVENT_MOUSE_POS_CHANGED && is_list(data)){
      out["x"] = data.get(0, 0)
      out["y"] = data.get(1, 0)
      return out
   }
   if(typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED){
      out["button"] = is_int(data) ? data : 0
      return out
   }
   if(typ == EVENT_MOUSE_SCROLL){
      mut dx, dy = 0.0, 0.0
      if(is_list(data)){
         dx, dy = float(data.get(0, 0.0)), float(data.get(1, 0.0))
      } elif(is_float(data) || is_int(data)){
         dy = float(data)
      }
      out["dx"] = dx
      out["dy"] = dy
      out["scrolling"] = true
      return out
   }
   out
}

fn _uri_hex_digit(int c) int {
   if(c >= 48 && c <= 57){ return c - 48 }
   if(c >= 65 && c <= 70){ return c - 55 }
   if(c >= 97 && c <= 102){ return c - 87 }
   -1
}

fn _uri_decode(str text) str {
   mut out = ""
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      if(c == 37 && i + 2 < text.len){
         def hi = _uri_hex_digit(load8(text, i + 1))
         def lo = _uri_hex_digit(load8(text, i + 2))
         if(hi >= 0 && lo >= 0){
            out += chr(hi * 16 + lo)
            i += 3
            continue
         }
      }
      out += chr(c)
      i += 1
   }
   out
}

fn _drop_uri_to_path(str uri) str {
   mut path = str.strip(uri)
   if(str.startswith(path, "file://localhost/")){
      path = "/" + str.str_slice(path, 17, path.len)
   } elif(str.startswith(path, "file://")){
      path = str.str_slice(path, 7, path.len)
   }
   path = _uri_decode(path)
   if(OS == "windows" && path.len >= 3 && load8(path, 0) == 47 && load8(path, 2) == 58){
      path = str.str_slice(path, 1, path.len)
   }
   path
}

fn _drop_paths_from_text(str text) list {
   mut out = []
   def lines = str.split(str.replace(text, "\r\n", "\n"), "\n")
   mut i = 0
   while(i < lines.len){
      def line = str.strip(to_str(lines.get(i, "")))
      if(line.len <= 0 || str.startswith(line, "#")){ i += 1 continue }
      if(str.startswith(line, "file://")){ out = out.append(_drop_uri_to_path(line)) }
      elif(!str.str_contains(line, "://")){ out = out.append(line) }
      i += 1
   }
   out
}

fn _drop_paths_from_any(any value) list {
   if(is_list(value)){ return value }
   if(is_str(value)){
      def text = to_str(value)
      def parsed = _drop_paths_from_text(text)
      if(parsed.len > 0){ return parsed }
      return text.len > 0 ? [text] : []
   }
   []
}

fn _normalize_drop_event_data(any win, int typ, any data) dict {
   mut out = is_dict(data) ? data : dict(8)
   mut paths = []
   if(is_dict(data)){
      paths = _drop_paths_from_any(data.get("paths", data.get("files", [])))
      if(paths.len == 0){ paths = _drop_paths_from_any(data.get("path", data.get("file", ""))) }
      if(paths.len == 0 && is_str(data.get("text", ""))){ paths = _drop_paths_from_text(to_str(data.get("text", ""))) }
   } else {
      paths = _drop_paths_from_any(data)
   }
   out["paths"] = paths
   out["files"] = paths
   if(!out.contains("x")){ out["x"] = win.get("mouse_x", 0) }
   if(!out.contains("y")){ out["y"] = win.get("mouse_y", 0) }
   if(!out.contains("mod")){ out["mod"] = win.get("modifiers", 0) }
   if(!out.contains("mods")){ out["mods"] = out.get("mod", 0) }
   if(typ == EVENT_DATA_DRAG){ out["dragging"] = true }
   else { out["dropped"] = true }
   out
}

fn _resize_data_axis(any data, str primary, str alias, int idx, int fallback) int {
   if(is_dict(data)){
      def raw = data.get(primary, data.get(alias, fallback))
      if(is_int(raw) || is_float(raw)){ return int(raw) }
      return fallback
   }
   if(is_list(data)){
      def raw = data.get(idx, fallback)
      if(is_int(raw) || is_float(raw)){ return int(raw) }
   }
   fallback
}

fn _normalize_resize_event_data(any win, any data) dict {
   mut out = dict(4)
   def fw = int(win.get("w", 0))
   def fh = int(win.get("h", 0))
   def w = _resize_data_axis(data, "w", "width", 0, fw)
   def h = _resize_data_axis(data, "h", "height", 1, fh)
   out["w"] = w
   out["h"] = h
   out["width"] = w
   out["height"] = h
   out
}

fn _window_process_key_event(any win, any e, int typ) list {
   mut data = ev.event_data(e)
   mut raw_key = 0
   if(is_dict(data)){ raw_key = data.get("key", 0) }
   elif(is_int(data)){ raw_key = data }
   elif(is_float(data)){ raw_key = int(data) }
   def sc = _event_scancode(data)
   mut k = _normalize_key(raw_key)
   if(k == KEY_NULL && sc > 0){
      def fk = _function_key_from_scancode(sc)
      if(fk > 0){ k = fk }
   }
   mut ks = win.get("key_states", 0)
   mut scs = win.get("scancode_states", 0)
   def is_press = (typ == EVENT_KEY_PRESSED)
   if(!is_dict(ks)){ ks = dict(256) }
   if(!is_dict(scs)){ scs = dict(256) }
   ks[k] = is_press
   if(sc > 0){ scs[sc] = is_press }
   if(is_press){
      def pk = win.get("pressed_keys", 0)
      mut pkd = is_dict(pk) ? pk : dict(256)
      pkd[k] = true
      win["pressed_keys"] = pkd
   }
   win["key_states"] = ks
   win["scancode_states"] = scs
   def event_mod = is_dict(data) ? _normalize_mod(data.get("mod", data.get("mods", 0))) : 0
   def mod = _normalize_mod(_mods_from_key_states(ks) | event_mod)
   win["modifiers"] = mod
   win["_input_dirty"] = true
   _save_win(win)
   if(!is_dict(data)){ data = dict(8) }
   data["key"] = k
   if(sc > 0){ data["scancode"] = sc }
   data["mod"] = mod
   data["mods"] = mod
   def out_e = [e.get(0), e.get(1), e.get(2), e.get(3), data]
   mut consumed = false
   if(is_press && _mod_bit_for_key(k) == 0 && !win.get("input_exclusive", false)){
      def now = ticks() / 1000000
      mut seq = win.get("chord_seq", [])
      if(!is_list(seq)){ seq = [] }
      def last_time = win.get("chord_time", 0)
      if(seq.len > 0 && (now - last_time > 1000)){ seq = [] }
      seq = seq.append([k, mod])
      win["chord_seq"] = seq
      win["chord_time"] = now
      _save_win(win)
      mut found_match, partial = false, false
      mut binds = win.get("bindings", [])
      if(!is_list(binds)){ binds = [] }
      def binds_n = binds.len
      mut i = 0 while(i < binds_n){
         def b = binds.get(i) def tseq = b.get(0)
         if(_seq_match(tseq, seq)){
            def action = b.get(1) if(action){ action() }
            found_match = true break
         } elif(_seq_match(seq, tseq, true)){ partial = true }
         i += 1
      }
      if(found_match){ consumed = true win["chord_seq"] = [] _save_win(win) }
      elif(partial){ consumed = true }
      else {
         win["chord_seq"] = [] _save_win(win)
         consumed = false
      }
   }
   def close_key = exit_key(win)
   if(!consumed && is_press && close_key != KEY_NULL && k == close_key){ set_should_close(win, true) }
   [consumed, out_e]
}

fn _window_process_char_event(any win, any e) list {
   mut data = ev.event_data(e)
   if(!is_dict(data)){
      if(is_int(data) || is_float(data)){
         data = {"char": int(data)}
      } else {
         data = dict(4)
      }
   }
   if(!data.contains("char") && data.contains("codepoint")){ data["char"] = data.get("codepoint", 0) }
   def mod = _normalize_mod(data.get("mod", data.get("mods", win.get("modifiers", 0))))
   data["mod"] = mod
   data["mods"] = mod
   [false, [e.get(0), e.get(1), e.get(2), e.get(3), data]]
}

fn _window_process_internal(any win, any e) list {
   win = _get_win(win)
   def typ = ev.event_type(e)
   mut out_e = e
   mut dirty = false
   if(typ == EVENT_KEY_PRESSED || typ == EVENT_KEY_RELEASED){ return _window_process_key_event(win, e, typ) } elif(typ == EVENT_KEY_CHAR){
      return _window_process_char_event(win, e)
   } elif(typ == EVENT_MOUSE_POS_CHANGED){
      def data = _normalize_mouse_event_data(win, typ, ev.event_data(e))
      out_e = [e.get(0), e.get(1), e.get(2), e.get(3), data]
      win["mouse_x"] = data.get("x", 0)
      win["mouse_y"] = data.get("y", 0)
      dirty = true
   } elif(typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED){
      def data = _normalize_mouse_event_data(win, typ, ev.event_data(e))
      out_e = [e.get(0), e.get(1), e.get(2), e.get(3), data]
      def btn = data.get("button", 0)
      mut mb = win.get("mouse_buttons", 0)
      def is_press = (typ == EVENT_MOUSE_BUTTON_PRESSED)
      if(!is_dict(mb)){ mb = dict(32) }
      mb[btn] = is_press
      if(is_press){
         def pb = win.get("pressed_buttons", 0)
         mut pbd = is_dict(pb) ? pb : dict(32)
         pbd[btn] = true
         win["pressed_buttons"] = pbd
      }
      win["mouse_buttons"] = mb
      win["_input_dirty"] = true
      win["mouse_x"] = data.get("x", 0)
      win["mouse_y"] = data.get("y", 0)
      dirty = true
   } elif(typ == EVENT_WINDOW_RESIZED){
      def data = _normalize_resize_event_data(win, ev.event_data(e))
      out_e = [e.get(0), e.get(1), e.get(2), e.get(3), data]
      def nw = data.get("w", 0)
      def nh = data.get("h", 0)
      if(nw > 0 && nh > 0){
         win["w"] = nw
         win["h"] = nh
         dirty = true
      }
   } elif(typ == EVENT_QUIT){
      win["should_close"] = true
      dirty = true
   } elif(typ == EVENT_WINDOW_MOVED){
      def data = ev.event_data(e)
      win["x"] = data.get("x", 0)
      win["y"] = data.get("y", 0)
      dirty = true
   } elif(typ == EVENT_FOCUS_IN){
      def windows = _ensure_windows()
      def windows_n = windows.len
      mut i = 0
      while(i < windows_n){
         mut ow = _get_win(windows.get(i, 0))
         if(_is_window(ow) && ow.get("handle", 0) != win.get("handle", 0) && ow.get("focused", false)){
            ow["focused"] = false
            _save_win(ow)
         }
         i += 1
      }
      win["focused"] = true
      dirty = true
   } elif(typ == EVENT_FOCUS_OUT){
      win["key_states"] = dict(256)
      win["scancode_states"] = dict(256)
      win["pressed_keys"] = dict(256)
      win["mouse_buttons"] = dict(32)
      win["pressed_buttons"] = dict(32)
      win["modifiers"] = 0
      win["_input_dirty"] = true
      win["focused"] = false
      dirty = true
   } elif(typ == EVENT_MOUSE_SCROLL){
      def data = _normalize_mouse_event_data(win, typ, ev.event_data(e))
      out_e = [e.get(0), e.get(1), e.get(2), e.get(3), data]
      win["scroll_x"] = win.get("scroll_x", 0.0) + data.get("dx", 0.0)
      win["scroll_y"] = win.get("scroll_y", 0.0) + data.get("dy", 0.0)
      win["scroll_dx"] = data.get("dx", 0.0)
      win["scroll_dy"] = data.get("dy", 0.0)
      dirty = true
   } elif(typ == EVENT_DATA_DROP || typ == EVENT_DATA_DRAG){
      def data = _normalize_drop_event_data(win, typ, ev.event_data(e))
      out_e = [e.get(0), e.get(1), e.get(2), e.get(3), data]
      win["mouse_x"] = data.get("x", win.get("mouse_x", 0))
      win["mouse_y"] = data.get("y", win.get("mouse_y", 0))
      dirty = true
   }
   if(dirty){ _save_win(win) }
   [false, out_e]
}

fn _check_native_state(any win) any {
   win = _get_win(win)
   def handle = win.get("handle", 0)
   if(!handle){ return 0 }
   if(!ui_backend.supports_state_polling()){ return 0 }
   if(!ui_backend.uses_native_events() && ui_backend.should_close(handle)){
      if(!win.get("should_close", false)){
         win["should_close"] = true
         push_event(win, 15)
         win = _get_win(win)
      }
   }
   if(!ui_backend.uses_native_events()){
      mut state_dirty = false
      def sz = ui_backend.get_size(handle)
      def nw = sz.get(0, 0) def nh = sz.get(1, 0)
      if(nw > 0 && nh > 0 && (nw != win.get("w", 0) || nh != win.get("h", 0))){
         win["w"] = nw
         win["h"] = nh
         mut sz_data = dict(8)
         sz_data["w"] = nw
         sz_data["h"] = nh
         push_event(win, EVENT_WINDOW_RESIZED, sz_data)
         win = _get_win(win)
      }
      mut ks = win.get("key_states", 0)
      mut changed = false
      mut j = 0 while(j < _KEY_POLL_COUNT){
         def nk = _key_poll_at(j)
         if(nk == KEY_NULL){ j += 1 continue }
         def real_now = ui_backend.get_key(handle, nk) == 1
         def was = !!ks.get(nk, false)
         if(real_now != was){
            ks[nk] = real_now
            changed = true
         }
         j += 1
      }
      if(changed){
         win["key_states"] = ks
         win["modifiers"] = _mods_from_key_states(ks)
         win["_input_dirty"] = true
         state_dirty = true
      }
      def cur = ui_backend.get_cursor_pos(handle)
      def mx = int(cur.get(0, 0.0))
      def my = int(cur.get(1, 0.0))
      if(mx != win.get("mouse_x", 0) || my != win.get("mouse_y", 0)){
         win["mouse_x"] = mx
         win["mouse_y"] = my
         state_dirty = true
      }
      mut mb = win.get("mouse_buttons", 0)
      mut mb_changed = false
      mut b = 0
      while(b < 8){
         def real_now = ui_backend.get_mouse_button(handle, b) == 1
         def was = !!mb.get(b, false)
         if(real_now != was){
            mb[b] = real_now
            mb_changed = true
         }
         b += 1
      }
      if(mb_changed){
         win["mouse_buttons"] = mb
         win["_input_dirty"] = true
         state_dirty = true
      }
      if(state_dirty){ _save_win(win) }
   }
}

@jit
fn check_event(any win) any {
   "Polls for new events and returns the next one from the queue, or 0 if empty."
   if(!_is_window(win)){ return 0 }
   if(win.get("_mock", false)){
      mut q0 = win.get("events", [])
      mut head0 = int(win.get("events_head", 0))
      if(head0 >= q0.len){
         win["events"] = []
         win["events_head"] = 0
         return 0
      }
      def e0 = q0.get(head0)
      head0 += 1
      if(head0 >= q0.len){
         win["events"] = []
         win["events_head"] = 0
      } else {
         win["events_head"] = head0
      }
      return e0
   }
   def handle = _get_handle(win)
   def native_events = ui_backend.uses_native_events()
   mut cw = _get_win(win)
   mut q = cw.get("events", [])
   mut head = int(cw.get("events_head", 0))
   if(head >= q.len){ q = [] head = 0 }
   if(q.len == 0){
      def mx0, my0 = cw.get("mouse_x", 0), cw.get("mouse_y", 0)
      def sx0, sy0 = cw.get("scroll_x", 0.0), cw.get("scroll_y", 0.0)
      mut state_dirty = false
      if(cw.get("mouse_last_x", mx0) != mx0){ cw["mouse_last_x"] = mx0 state_dirty = true }
      if(cw.get("mouse_last_y", my0) != my0){ cw["mouse_last_y"] = my0 state_dirty = true }
      if(cw.get("scroll_last_x", sx0) != sx0){ cw["scroll_last_x"] = sx0 state_dirty = true }
      if(cw.get("scroll_last_y", sy0) != sy0){ cw["scroll_last_y"] = sy0 state_dirty = true }
      if(cw.get("scroll_dx", 0.0) != 0.0){ cw["scroll_dx"] = 0.0 state_dirty = true }
      if(cw.get("scroll_dy", 0.0) != 0.0){ cw["scroll_dy"] = 0.0 state_dirty = true }
      if(state_dirty){ _set_window_registry(handle, cw) }
      if(native_events){
         def polled_events = ui_backend.poll_events_for_window(handle)
         if(is_list(polled_events) && polled_events.len > 0){
            mut q2 = cw.get("events", [])
            mut head2 = int(cw.get("events_head", 0))
            if(head2 >= q2.len){ q2 = [] head2 = 0 }
            elif(head2 > 64 && head2 * 2 >= q2.len){
               q2 = slice(q2, head2, q2.len, 1)
               head2 = 0
            }
            def polled_events_n = polled_events.len
            mut i = 0
            while(i < polled_events_n){
               def ne = polled_events.get(i)
               if(ev.is_event(ne)){ q2 = ev.queue_push(q2, ne) }
               i += 1
            }
            cw["events"] = q2
            cw["events_head"] = head2
            _set_window_registry(handle, cw)
         }
      } else {
         update_input(win)
         ui_backend.poll_events()
         _check_native_state(win)
      }
      cw = _get_win(win)
      q = cw.get("events", [])
      head = int(cw.get("events_head", 0))
      if(head >= q.len){ q = [] head = 0 }
   }
   mut q_n = q.len
   while(head < q_n){
      def e = q.get(head)
      head += 1
      if(head >= q_n){
         q = []
         head = 0
         q_n = 0
      } elif(head > 64 && head * 2 >= q_n){
         q = slice(q, head, q_n, 1)
         head = 0
         q_n = q.len
      }
      cw["events"] = q
      cw["events_head"] = head
      _set_window_registry(handle, cw)
      def p_res = _window_process_internal(cw, e)
      if(!p_res.get(0)){ return p_res.get(1) }
      cw = _get_win(win)
      q = cw.get("events", [])
      head = int(cw.get("events_head", 0))
      if(head >= q.len){ q = [] head = 0 }
      q_n = q.len
   }
   0
}

fn event_type(any e) int {
   "Returns the type ID of event `e`."
   ev.event_type(e)
}

fn event_window(any e) any {
   "Returns the window handle associated with event `e`."
   ev.event_window(e)
}

fn event_window_id(any e) any {
   "Returns the numeric window ID for event `e`."
   ev.event_window_id(e)
}

fn event_data(any e) any {
   "Returns the extra data payload of event `e`."
   ev.event_data(e)
}

fn event_key_is(any data, any key) bool {
   "Returns true if event payload `data` matches `key`, including backend scancode fallback."
   if(!is_dict(data)){ return false }
   def nk = _normalize_key(key)
   if(_normalize_key(data.get("key", 0)) == nk){ return true }
   def sc = _event_scancode(data)
   if(sc <= 0){ return false }
   def qsc = _scancodes_for_query(nk)
   def qsc_n = qsc.len
   mut i = 0
   while(i < qsc_n){
      if(qsc.get(i, 0) == sc){ return true }
      i += 1
   }
   false
}

fn quit(any e) bool {
   "Returns true if the event is a quit/close request."
   event_type(e) == 15
}

@jit
fn key_down(any win, any k) bool {
   "Returns true if key `k` is currently held down in `win`."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def nk = _normalize_key(k)
   def ks = win.get("key_states", 0)
   if(is_dict(ks) && ks.get(nk, false)){ return true }
   def scs = win.get("scancode_states", 0)
   def qsc = _scancodes_for_query(nk)
   def qsc_n = qsc.len
   mut i = 0
   while(i < qsc_n){
      if(scs.get(qsc.get(i), false)){ return true }
      i += 1
   }
   if(ui_backend.uses_native_events()){ return false }
   _backend_key_down(win, nk)
}

fn get_modifiers(any win) int {
   "Returns the bitmask of active modifier keys(Shift, Ctrl, Alt, etc.)."
   win = _get_win(win)
   if(!_is_window(win)){ return 0 }
   _normalize_mod(win.get("modifiers", 0))
}

fn mod_down(any win, any m) bool {
   "Returns true if modifier combination `m` is active."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def nm = _normalize_mod(m)
   if(nm == 0){ return false }
   (get_modifiers(win) & nm) == nm
}

fn key_pressed(any win, any k) bool {
   "Runs the key pressed operation."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def nk = _normalize_key(k) def ks = win.get("key_states", 0)
   def lks = win.get("last_key_states", 0) def pk = win.get("pressed_keys", 0)
   def cached = (!!ks.get(nk, false) && !lks.get(nk, false)) || !!pk.get(nk, false)
   if(cached){ return true }
   if(ui_backend.uses_native_events()){ return false }
   _backend_key_down(win, nk) && !lks.get(nk, false)
}

@jit
fn mouse_pos(any win) list {
   "Returns [x, y] coordinates of the mouse cursor relative to the window."
   win = _get_win(win)
   if(!_is_window(win)){ return [0, 0] }
   def cur = _backend_mouse_pos(win)
   def mx = cur.get(0, win.get("mouse_x", 0))
   def my = cur.get(1, win.get("mouse_y", 0))
   if(mx != win.get("mouse_x", 0) || my != win.get("mouse_y", 0)){
      win["mouse_x"] = mx
      win["mouse_y"] = my
      _save_win(win)
   }
   [mx, my]
}

fn mouse_down(any win, int b) bool {
   "Returns true if mouse button `b` is currently held down."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def live = _backend_mouse_down(win, b)
   def mb = win.get("mouse_buttons", 0)
   if(live){
      if(is_dict(mb) && !mb.get(b, false)){
         mb[b] = true
         win["mouse_buttons"] = mb
         win["_input_dirty"] = true
         _save_win(win)
      }
      return true
   }
   if(is_dict(mb) && mb.get(b, false)){
      mb[b] = false
      win["mouse_buttons"] = mb
      win["_input_dirty"] = true
      _save_win(win)
   }
   false
}

fn mouse_pressed(any win, int b) bool {
   "Returns true if mouse button `b` was clicked this frame."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def mb = win.get("mouse_buttons", 0) def lmb = win.get("last_mouse_buttons", 0) def pb = win.get("pressed_buttons", 0)
   def down_now = !!mb.get(b, false)
   def cached = (down_now && !lmb.get(b, false)) || !!pb.get(b, false)
   if(cached){ return true }
   _backend_mouse_down(win, b) && !lmb.get(b, false)
}

fn on_key(any win, any k, bool p=true, bool r=false, any m=0) bool {
   "Runs the on key operation."
   win = _get_win(win) if(!_is_window(win)){ return false }
   def nk = _normalize_key(k) mut ks = win.get("key_states", 0)
   if(!is_dict(ks)){ ks = dict(256) }
   ks[nk] = !!p
   if(!!p){
      def pk = win.get("pressed_keys", 0)
      mut pkd = is_dict(pk) ? pk : dict(256)
      pkd[nk] = true
      win["pressed_keys"] = pkd
   }
   mut cm = _normalize_mod(m) if(cm == 0 || _mod_bit_for_key(nk) != 0){ cm = _mods_from_key_states(ks) }
   win["key_states"] = ks
   win["modifiers"] = cm
   win["_input_dirty"] = true
   _save_win(win)
   push_event(win, !!p ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED, {"key": nk, "pressed": !!p, "mod": cm})
   def close_key = exit_key(win)
   if(!!p && close_key != KEY_NULL && nk == close_key){ set_should_close(win, true) }
   true
}

fn match_chord(any e, any k, any m=0) bool {
   "Runs the match chord operation."
   if(event_type(e) != EVENT_KEY_PRESSED){ return false }
   def d = event_data(e) if(!is_dict(d)){ return false }
   def nk = _normalize_key(k) def nm = _normalize_mod(m)
   if(d.get("key", 0) != nk){ return false }
   if(nm != 0 && (_normalize_mod(d.get("mod", 0)) & nm) != nm){ return false }
   true
}

fn bind(any win, str n, any a) bool {
   "Runs the bind operation."
   win = _get_win(win) if(!_is_window(win) || !is_str(n)){ return false }
   def seq = _parse_notation(n) if(seq.len == 0){ return false }
   mut b = win.get("bindings", [])
   if(!is_list(b)){ b = [] }
   def b_n = b.len
   mut found, i = false, 0 while(i < b_n){
      def item = b.get(i) if(_seq_match(item.get(0), seq)){ item[1] = a found = true break }
      i += 1
   }
   if(!found){ b = b.append([seq, a]) }
   win["bindings"] = b
   _save_win(win)
   true
}

mut _input_update_last_handle = 0
mut _input_update_last_ns = 0
mut _input_update_ns_by_handle = dict(8)

fn update_input(any win) any {
   "Runs the update input operation."
   win = _get_win(win) if(!_is_window(win)){ return 0 }
   if(!win.get("_input_dirty", false)){ return 0 }
   def handle = _get_handle(win)
   def now = ticks()
   mut last = 0
   if(handle == _input_update_last_handle){ last = _input_update_last_ns }
   else { last = _input_update_ns_by_handle.get(handle, 0) }
   if(last > 0 && now - last < 500000){ return 0 }
   _input_update_last_handle = handle
   _input_update_last_ns = now
   _input_update_ns_by_handle[handle] = now
   def ks = win.get("key_states", 0)
   def mb = win.get("mouse_buttons", 0)
   mut dirty = false
   if(win.get("last_key_states", 0) != ks){
      win["last_key_states"] = is_dict(ks) ? clone(ks) : dict(32)
      dirty = true
   }
   if(win.get("last_mouse_buttons", 0) != mb){
      win["last_mouse_buttons"] = is_dict(mb) ? clone(mb) : dict(8)
      dirty = true
   }
   def pk = win.get("pressed_keys", 0)
   if(is_dict(pk) && pk.len > 0){
      win["pressed_keys"] = dict(32)
      dirty = true
   }
   def pb = win.get("pressed_buttons", 0)
   if(is_dict(pb) && pb.len > 0){
      win["pressed_buttons"] = dict(8)
      dirty = true
   }
   win["_input_dirty"] = false
   dirty = true
   if(dirty){ _save_win(win) }
}

fn swap_buffers(any win) any {
   "Swaps front and back buffers(double buffering)."
   win = _get_win(win)
   if(_is_window(win)){
      def h = win.get("handle", 0)
      if(h){
         if(_is_debug()){ _dbg_win(h, "swap_buffers") }
         ui_backend.swap_buffers(h)
      }
   }
}

fn make_current(any win) any {
   "Makes a window OpenGL context current, or releases it when win is false."
   if(!win){ return ui_backend.make_context_current(0) }
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def h = _get_handle(win)
   if(!h){ return false }
   ui_backend.make_context_current(h)
}

mut _blit_hook = 0

fn set_blit_handler(any h) any {
   "Runs the set blit handler operation."
   _blit_hook = h
}

fn blit_buffer(any win, any buf, int w, int h) any {
   "Runs the blit buffer operation."
   win = _get_win(win) if(_is_window(win) && is_ptr(buf) && _blit_hook){ _blit_hook(buf, w, h) } 0
}

fn blit_software(any win, any buf, int w, int h) any {
   "Blits a software RGBA pixel buffer directly to the native window surface."
   win = _get_win(win)
   if(_is_window(win) && is_ptr(buf) && w > 0 && h > 0){
      def handle = win.get("handle", 0)
      if(handle){ ui_backend.blit_software(handle, buf, w, h) }
   }
}

fn poll_events() bool {
   "Triggers the system to process window and input events."
   if(!_ensure_backend_ready()){ return false }
   ui_backend.poll_events()
   def windows = _ensure_windows()
   def windows_n = windows.len
   def native_events = ui_backend.uses_native_events()
   mut i = 0
   while(i < windows_n){
      def raw_win = windows.get(i, 0)
      mut win = _get_win(raw_win)
      if(!_is_window(win)){ i += 1 continue }
      def mx0, my0 = win.get("mouse_x", 0), win.get("mouse_y", 0)
      def sx0, sy0 = win.get("scroll_x", 0.0), win.get("scroll_y", 0.0)
      mut state_dirty = false
      if(win.get("mouse_last_x", mx0) != mx0){ win["mouse_last_x"] = mx0 state_dirty = true }
      if(win.get("mouse_last_y", my0) != my0){ win["mouse_last_y"] = my0 state_dirty = true }
      if(win.get("scroll_last_x", sx0) != sx0){ win["scroll_last_x"] = sx0 state_dirty = true }
      if(win.get("scroll_last_y", sy0) != sy0){ win["scroll_last_y"] = sy0 state_dirty = true }
      if(win.get("scroll_dx", 0.0) != 0.0){ win["scroll_dx"] = 0.0 state_dirty = true }
      if(win.get("scroll_dy", 0.0) != 0.0){ win["scroll_dy"] = 0.0 state_dirty = true }
      if(state_dirty){ _save_win(win) }
      update_input(win)
      if(native_events){
         def queued_native_events = ui_backend.pump_window_events(_get_handle(win))
         if(is_list(queued_native_events) && queued_native_events.len > 0){
            def native_events_n = queued_native_events.len
            mut j = 0
            while(j < native_events_n){
               def ne = queued_native_events.get(j)
               if(ev.is_event(ne)){ push_event(win, ev.event_type(ne), ev.event_data(ne)) }
               j += 1
            }
         }
      }
      _check_native_state(win)
      i += 1
   }
   true
}

fn count_open() int {
   "Runs the count open operation."
   def windows = _ensure_windows()
   def windows_n = windows.len
   mut i, n = 0, 0 while(i < windows_n){
      def w = windows.get(i) if(_is_window(w) && !should_close(w)){ n += 1 }
      i += 1
   }
   n
}

fn last() any {
   "Runs the last operation."
   def windows = _ensure_windows()
   if(windows.len == 0){ return 0 }
   mut i = windows.len - 1
   while(i >= 0){
      def w = _get_win(windows.get(i, 0))
      if(_is_window(w) && w.get("focused", false)){ return w }
      i -= 1
   }
   def w = windows.get(windows.len - 1, 0)
   if(_is_window(w)){ return _get_win(w) }
   0
}

fn get_win(any win) any { _get_win(win) }

fn primary_mode() list {
   "Returns `[width, height, monitor]` for the primary monitor's current video mode."
   def monitor = get_primary_monitor()
   if(!monitor){ return [1280, 720, 0] }
   def mode = get_video_mode(monitor)
   if(!is_dict(mode)){ return [1280, 720, monitor] }
   def w, h = int(mode.get("width", 1280)), int(mode.get("height", 720))
   [w > 0 ? w : 1280, h > 0 ? h : 720, monitor]
}

fn default_window_size(int screen_w,
   int screen_h,
   f64 width_ratio=0.72,
   f64 height_ratio=0.78,
   int min_w=960,
   int min_h=640,
   int max_w=1600,
   int max_h=1000) list {
   "Returns a clamped default window size derived from the current screen size."
   mut w, h = int(float(screen_w) * width_ratio), int(float(screen_h) * height_ratio)
   if(w > max_w){ w = max_w }
   if(h > max_h){ h = max_h }
   if(w < min_w){ w = min_w }
   if(h < min_h){ h = min_h }
   [w, h]
}

fn fit_to_workarea(any win) bool {
   "Sizes a window to the primary monitor work area without using WM maximize."
   if(!win){ return false }
   def mon = get_primary_monitor()
   if(!mon){ return false }
   def wa = get_monitor_workarea(mon)
   if(!wa || wa.len < 4){ return false }
   def mx, my = int(wa.get(0, 0)), int(wa.get(1, 0))
   def mw, mh = int(wa.get(2, 0)), int(wa.get(3, 0))
   if(mw <= 0 || mh <= 0){ return false }
   move(win, mx, my)
   resize(win, mw, mh)
   true
}

fn set_cursor_mode(any win, int m) bool {
   "Runs the set cursor mode operation."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h){
      if(m != CURSOR_NORMAL && m != CURSOR_HIDDEN && m != CURSOR_DISABLED && m != ui_backend.CURSOR_CAPTURED){
         _dbg_win(h, "set_cursor_mode INVALID mode=" + to_str(m) + " - ignoring")
         return false
      }
      ui_backend.set_input_mode(h, 0x00033001, m)
      win["cursor_mode"] = m
      _save_win(win)
   }
   true
}

fn focus(any win) bool {
   "Runs the focus operation."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h){
      _dbg_win(h, "focus")
      ui_backend.focus_window(h)
      win["focused"] = true
      _save_win(win)
   }
   true
}

fn cursor_pos(any win) list {
   "Runs the cursor pos operation."
   win = _get_win(win)
   if(!_is_window(win)){ return [0.0, 0.0] }
   def cur = _backend_mouse_pos(win)
   [float(cur.get(0, 0)), float(cur.get(1, 0))]
}

fn scroll_pos(any win) list {
   "Runs the scroll pos operation."
   win = _get_win(win) if(!_is_window(win)){ return [0.0, 0.0] } [win.get("scroll_x", 0.0), win.get("scroll_y", 0.0)]
}

fn mouse_state(any win) dict {
   "Runs the mouse state operation."
   win = _get_win(win)
   if(!_is_window(win)){ return dict(8) }
   mut m = dict(8)
   def x, y = win.get("mouse_x", 0), win.get("mouse_y", 0)
   def lx, ly = win.get("mouse_last_x", x), win.get("mouse_last_y", y)
   m["x"] = x
   m["y"] = y
   m["last_x"] = lx
   m["last_y"] = ly
   m["moved"] = (x != lx) || (y != ly)
   mut s = dict(8)
   def sx, sy = win.get("scroll_x", 0.0), win.get("scroll_y", 0.0)
   def lsx, lsy = win.get("scroll_last_x", sx), win.get("scroll_last_y", sy)
   s["x"] = sx
   s["y"] = sy
   s["last_x"] = lsx
   s["last_y"] = lsy
   s["scrolling"] = (sx != lsx) || (sy != lsy)
   m["scroll"] = s
   m["scrolling"] = (sx != lsx) || (sy != lsy)
   m
}

fn set_cursor_pos(any win, any x, any y) bool {
   "Runs the set cursor pos operation."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(h){
      ui_backend.set_cursor_pos(h, x, y)
      win["mouse_x"] = int(x)
      win["mouse_y"] = int(y)
      _save_win(win)
   }
   true
}

fn center_cursor(any win) bool {
   "Moves the cursor to the center of the current window."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def sz = size(win)
   def cx = float(sz.get(0, 0)) * 0.5
   def cy = float(sz.get(1, 0)) * 0.5
   set_cursor_pos(win, cx, cy)
}

fn show_centered_cursor(any win) bool {
   "Shows the cursor and centers it, reasserting normal mode for backends that need it."
   set_cursor_mode(win, CURSOR_NORMAL)
   if(!center_cursor(win)){ return false }
   center_cursor(win)
   set_cursor_mode(win, CURSOR_NORMAL)
}

fn sync_cursor(any win, int mode) bool {
   "Applies a cursor mode. Locked/captured modes recenter; normal/hidden modes should not warp."
   if(!set_cursor_mode(win, mode)){ return false }
   if(mode == CURSOR_DISABLED || mode == ui_backend.CURSOR_CAPTURED){ return center_cursor(win) }
   true
}

fn set_window_opacity(any win, f64 val) any {
   "Sets the whole-window opacity(alpha) value from 0.0 to 1.0."
   win = _get_win(win)
   if(_is_window(win)){
      def h = win.get("handle", 0)
      if(h){ ui_backend.set_window_opacity(h, val) }
   }
}

fn set_window_resizable(any win, bool val) bool {
   "Toggles whether the window can be resized by the user."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   win["resizable"] = !!val
   _save_win(win)
   def h = win.get("handle", 0)
   if(h){ ui_backend.set_window_resizable(h, !!val) }
   _window_flag_set(win, WINDOW_NO_RESIZE, !val)
   true
}

fn set_window_decorated(any win, bool val) bool {
   "Toggles window decorations(title bar, borders)."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   win["decorated"] = !!val
   _save_win(win)
   def h = win.get("handle", 0)
   if(h){ ui_backend.set_window_decorated(h, !!val) }
   _window_flag_set(win, WINDOW_NO_BORDER, !val)
   true
}

fn transparent_framebuffer(any win) bool {
   "Returns whether the window reports a transparent framebuffer."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   def h = win.get("handle", 0)
   if(!h){ return false }
   ui_backend.get_window_attrib(h, ui_backend.TRANSPARENT_FRAMEBUFFER) != 0
}

fn set_window_floating(any win, bool val) bool {
   "Toggles the always-on-top state of the window."
   win = _get_win(win)
   if(!_is_window(win)){ return false }
   win["floating"] = !!val
   _save_win(win)
   def h = win.get("handle", 0)
   if(h){ ui_backend.set_window_floating(h, !!val) }
   _window_flag_set(win, WINDOW_FLOATING, val)
   true
}

fn toggle_window_resizable(any win) bool {
   "Toggles user-resizable state and returns whether the operation succeeded."
   set_window_resizable(win, !is_window_resizable(win))
}

fn toggle_window_decorated(any win) bool {
   "Toggles window decorations and returns whether the operation succeeded."
   set_window_decorated(win, !is_window_decorated(win))
}

fn toggle_window_floating(any win) bool {
   "Toggles topmost/floating state and returns whether the operation succeeded."
   set_window_floating(win, !is_window_floating(win))
}

fn has_window_flag(any win, int flag) bool {
   "Returns whether a raw or facade-managed window flag is active."
   case flag {
      WINDOW_NORMAL -> window_flags(win) == 0
      WINDOW_NO_BORDER -> !is_window_decorated(win)
      WINDOW_NO_RESIZE -> !is_window_resizable(win)
      WINDOW_HIDE_MOUSE -> {
         win = _get_win(win)
         return _is_window(win) && (int(win.get("cursor_mode", CURSOR_NORMAL)) == CURSOR_HIDDEN || (window_flags(win) & flag) != 0)
      }
      WINDOW_FULLSCREEN -> is_window_fullscreen(win)
      WINDOW_TRANSPARENT -> transparent_framebuffer(win) || (window_flags(win) & flag) != 0
      WINDOW_HIDE -> !is_window_visible(win)
      WINDOW_MAXIMIZE -> is_window_maximized(win)
      WINDOW_FLOATING -> is_window_floating(win)
      WINDOW_MINIMIZE -> is_window_minimized(win)
      WINDOW_FOCUS -> is_window_focused(win)
      WINDOW_CAPTURE_MOUSE -> {
         win = _get_win(win)
         return _is_window(win) && (int(win.get("cursor_mode", CURSOR_NORMAL)) == ui_backend.CURSOR_CAPTURED || (window_flags(win) & flag) != 0)
      }
      _ -> (window_flags(win) & flag) != 0
   }
}

fn set_window_flag(any win, int flag, bool enabled) bool {
   "Applies a mutable window flag when supported, otherwise records the requested bit."
   case flag {
      WINDOW_NORMAL -> {
         if(enabled){
            win = _get_win(win)
            if(!_is_window(win)){ return false }
            win["flags"] = WINDOW_NORMAL
            _save_win(win)
            return true
         }
      }
      WINDOW_NO_BORDER -> { return set_window_decorated(win, !enabled) }
      WINDOW_NO_RESIZE -> { return set_window_resizable(win, !enabled) }
      WINDOW_HIDE_MOUSE -> {
         def ok = set_cursor_mode(win, enabled ? CURSOR_HIDDEN : CURSOR_NORMAL)
         if(ok){ _window_flag_set(win, flag, enabled) }
         return ok
      }
      WINDOW_FULLSCREEN -> { return set_window_fullscreen(win, enabled) }
      WINDOW_HIDE -> { return enabled ? hide(win) : show(win) }
      WINDOW_MAXIMIZE -> { return enabled ? maximize(win) : restore(win) }
      WINDOW_FLOATING -> { return set_window_floating(win, enabled) }
      WINDOW_MINIMIZE -> { return enabled ? iconify(win) : restore(win) }
      WINDOW_FOCUS -> {
         if(enabled){ return focus(win) }
         return _window_flag_set(win, flag, false)
      }
      WINDOW_CAPTURE_MOUSE -> {
         def ok = sync_cursor(win, enabled ? ui_backend.CURSOR_CAPTURED : CURSOR_NORMAL)
         if(ok){ _window_flag_set(win, flag, enabled) }
         return ok
      }
      _ -> {}
   }
   _window_flag_set(win, flag, enabled)
}

fn toggle_window_flag(any win, int flag) bool {
   "Toggles a window flag and returns whether the operation succeeded."
   set_window_flag(win, flag, !has_window_flag(win, flag))
}

fn window_state(any win) dict {
   "Returns a compact snapshot of common window flags."
   {
      "flags": window_flags(win),
      "visible": is_window_visible(win),
      "hidden": !is_window_visible(win),
      "focused": is_window_focused(win),
      "minimized": is_window_minimized(win),
      "maximized": is_window_maximized(win),
      "resizable": is_window_resizable(win),
      "no_resize": !is_window_resizable(win),
      "decorated": is_window_decorated(win),
      "no_border": !is_window_decorated(win),
      "floating": is_window_floating(win),
      "fullscreen": is_window_fullscreen(win),
      "borderless": is_window_borderless(win),
      "transparent": transparent_framebuffer(win),
      "allow_dnd": has_window_flag(win, WINDOW_ALLOW_DND),
      "hide_mouse": has_window_flag(win, WINDOW_HIDE_MOUSE),
      "raw_mouse": has_window_flag(win, WINDOW_RAW_MOUSE),
      "scale_to_monitor": has_window_flag(win, WINDOW_SCALE_TO_MONITOR),
      "center_cursor": has_window_flag(win, WINDOW_CENTER_CURSOR),
      "focus_on_show": has_window_flag(win, WINDOW_FOCUS_ON_SHOW),
      "capture_mouse": has_window_flag(win, WINDOW_CAPTURE_MOUSE),
      "cpu": has_window_flag(win, WINDOW_CPU),
      "vulkan": has_window_flag(win, WINDOW_VULKAN),
      "vsync": window_vsync()
   }
}

fn set_input_exclusive(any win, bool val) bool {
   "Runs the set input exclusive operation."
   win = _get_win(win)
   if(_is_window(win)){
      win["input_exclusive"] = !!val
      _save_win(win)
      true
   } else { false }
}

fn joystick_present(any jid) bool {
   "Returns true when the joystick slot is connected."
   ui_backend.joystick_present(jid)
}

fn get_joystick_name(int jid) str {
   "Returns the raw joystick device name."
   ui_backend.get_joystick_name(jid)
}

fn get_joystick_guid(int jid) str {
   "Returns the joystick GUID."
   ui_backend.get_joystick_guid(jid)
}

fn get_joystick_axes(int jid, any count_out) any {
   "Returns raw joystick axes and writes the axis count."
   ui_backend.get_joystick_axes(jid, count_out)
}

fn get_joystick_buttons(int jid, any count_out) any {
   "Returns raw joystick buttons and writes the button count."
   ui_backend.get_joystick_buttons(jid, count_out)
}

fn get_joystick_hats(int jid, any count_out) any {
   "Returns raw joystick hats and writes the hat count."
   ui_backend.get_joystick_hats(jid, count_out)
}

fn joystick_is_gamepad(int jid) bool {
   "Returns true when the joystick has a gamepad mapping."
   ui_backend.joystick_is_gamepad(jid)
}

fn get_gamepad_state(int jid, any state_out) bool {
   "Writes mapped gamepad state for the joystick."
   ui_backend.get_gamepad_state(jid, state_out)
}

fn get_gamepad_name(int jid) str {
   "Returns mapped gamepad name when available."
   ui_backend.get_gamepad_name(jid)
}

fn get_gamepad_guid(int jid) str {
   "Returns the gamepad GUID."
   ui_gamepad.get_gamepad_guid(jid)
}

fn gamepad_count() i32 {
   "Returns the number of connected joysticks."
   ui_gamepad.gamepad_count()
}

fn gamepads() list {
   "Returns connected joystick IDs."
   ui_gamepad.gamepads()
}

fn gamepad_connected(int jid) bool {
   "Returns true when joystick `jid` is connected and mapped as a gamepad."
   ui_gamepad.gamepad_connected(jid)
}

fn gamepad_mapped(int jid) bool {
   "Returns true when joystick `jid` has a gamepad mapping."
   ui_gamepad.gamepad_mapped(jid)
}

fn gamepad_name(int jid) str {
   "Returns mapped gamepad name when available, else raw joystick name."
   ui_gamepad.gamepad_name(jid)
}

fn gamepad_guid(int jid) str {
   "Returns the GUID for joystick `jid`."
   ui_gamepad.gamepad_guid(jid)
}

fn gamepad_axis(int jid, any axis) f64 {
   "Returns mapped axis value in [-1, 1], with raw fallback."
   ui_gamepad.gamepad_axis(jid, axis)
}

fn gamepad_button(int jid, any button) bool {
   "Returns mapped button state, with raw fallback."
   ui_gamepad.gamepad_button(jid, button)
}

fn gamepad_axis_count(int jid) i32 {
   "Returns mapped axis count or raw joystick axis count."
   ui_gamepad.gamepad_axis_count(jid)
}

fn gamepad_button_count(int jid) i32 {
   "Returns mapped button count or raw joystick button count."
   ui_gamepad.gamepad_button_count(jid)
}

fn set_joystick_callback(any cb) any {
   "Installs a joystick connection callback."
   ui_backend.set_joystick_callback(cb)
}

fn update_gamepad_mappings(str mappings) bool {
   "Adds gamepad mapping data."
   ui_backend.update_gamepad_mappings(mappings)
}

fn add_gamepad_mapping(str mapping) bool {
   "Adds one gamepad mapping row."
   ui_gamepad.add_gamepad_mapping(mapping)
}

fn set_clipboard(any win, str s) any {
   "Sets the system clipboard to string `s`."
   win = _get_win(win)
   if(_is_window(win)){
      def h = win.get("handle", 0)
      if(h){ ui_backend.set_clipboard(h, s) }
   }
}

fn get_clipboard(any win) str {
   "Retrieves string contents from the system clipboard."
   win = _get_win(win)
   if(_is_window(win)){
      def h = win.get("handle", 0)
      if(h){ return ui_backend.get_clipboard(h) }
   }
   ""
}

fn get_error() int {
   "Returns the last backend error as [code, description]."
   ui_backend.get_error()
}

fn get_proc_address(any name) any {
   "Returns the address of the specified OpenGL function."
   ui_backend.get_proc_address(name)
}

fn get_x11_display() any {
   "Returns the active X11 Display* when the backend supports it."
   ui_backend.get_x11_display()
}

fn get_x11_window(any win) any {
   "Returns the native X11 Window handle for `win` when available."
   win = _get_win(win)
   if(!_is_window(win)){ return 0 }
   def h = win.get("handle", 0)
   if(!h){ return 0 }
   ui_backend.get_x11_window(h)
}

fn set_x11_selection_string(any win, str s) any {
   "Sets the X11 PRIMARY selection string for the specified window."
   ui_backend.set_x11_selection_string(win, s)
}

fn get_x11_selection_string(any win) str {
   "Returns the X11 PRIMARY selection string for the specified window."
   ui_backend.get_x11_selection_string(win)
}

#main {
   ui_backend.init_hint(ui_backend.PLATFORM, ui_backend.PLATFORM_NULL)
   window_hint(CLIENT_API, NO_API)
   assert(backend() == "none" && available(), "window null backend")
   assert(KEY_W == 87 && KEY_A == 65 && KEY_F1 == 290 && KEY_ESCAPE == 256, "window key constants")
   assert(EVENT_KEY_PRESSED == 1 && WINDOW_FOCUS_ON_SHOW == 0x2000, "window event constants")
   def win = open_window("facade", 0, 0, 80, 50, WINDOW_HIDE)
   assert(id(win) > 0 && title(win) == "facade" && size(win) == [80, 50] && pos(win) == [0, 0], "window create state")
   assert(set_title(win, "renamed") && title(win) == "renamed", "window title update")
   assert(move(win, 5, 6) && resize(win, 120, 90), "window move resize")
   assert(pos(win) == [5, 6] && size(win) == [120, 90], "window moved resized state")
   assert(event_type(check_event(win)) == EVENT_WINDOW_MOVED, "window moved event")
   assert(event_type(check_event(win)) == EVENT_WINDOW_RESIZED, "window resized event")
   assert(set_exit_key(win, KEY_NULL) && exit_key(win) == KEY_NULL, "window exit key")
   assert(focus(win) && last().get("handle", 0) == id(win), "window focus")
   assert(set_cursor_mode(win, CURSOR_NORMAL) && set_cursor_pos(win, 10, 11), "window cursor state")
   assert(cursor_pos(win) == [10.0, 11.0] && mouse_pos(win) == [10, 11], "window cursor readback")
   assert(push_event(win, EVENT_KEY_PRESSED, {"key": KEY_B, "scancode": 56}), "window queued key press")
   assert(key_down(win, KEY_B) && key_pressed(win, KEY_B), "window queued key state before drain")
   def queued_key_event = check_event(win)
   assert(event_type(queued_key_event) == EVENT_KEY_PRESSED && event_key_is(event_data(queued_key_event), KEY_B), "window queued key event")
   assert(push_event(win, EVENT_KEY_RELEASED, {"key": KEY_B, "scancode": 56}), "window queued key release")
   assert(!key_down(win, KEY_B), "window queued key release state before drain")
   check_event(win)
   assert(on_key(win, KEY_A), "window synthetic key press")
   def key_event = check_event(win)
   assert(event_type(key_event) == EVENT_KEY_PRESSED && event_key_is(event_data(key_event), KEY_A), "window key event")
   assert(key_down(win, KEY_A) && key_pressed(win, KEY_A), "window key state")
   msleep(1)
   update_input(win)
   assert(!key_pressed(win, KEY_A), "window key edge clears")
   assert(on_key(win, KEY_A, false), "window synthetic key release")
   def release_event = check_event(win)
   assert(event_type(release_event) == EVENT_KEY_RELEASED && !key_down(win, KEY_A), "window key release")
   assert(push_event(win, EVENT_KEY_PRESSED, {"key": 0, "scancode": 0x57, "action": 1}), "window raw scancode press")
   def raw_event = check_event(win)
   assert(event_key_is(event_data(raw_event), KEY_F11) && key_down(win, KEY_F11), "window raw scancode normalizes")
   assert(push_event(win, EVENT_KEY_RELEASED, {"key": 0, "scancode": 0x57, "action": 0}), "window raw scancode release")
   check_event(win)
   assert(!key_down(win, KEY_F11), "window raw scancode release state")
   assert(push_event(win, EVENT_KEY_CHAR, {"codepoint": 97, "mods": MOD_SHIFT}), "window char event")
   def char_event = check_event(win)
   def char_data = event_data(char_event)
   assert(event_type(char_event) == EVENT_KEY_CHAR && char_data.get("char", 0) == 97 && (char_data.get("mods", 0) & MOD_SHIFT) != 0, "window char normalization")
   assert(push_event(win, EVENT_WINDOW_RESIZED, [320, 240]), "window list resize event")
   def resize_event = check_event(win)
   def resize_data = event_data(resize_event)
   assert(resize_data.get("w", 0) == 320 && resize_data.get("height", 0) == 240 && size(win) == [320, 240], "window resize normalization")
   assert(push_event(win, EVENT_MOUSE_SCROLL, {"dx": 1.0, "dy": -1.0}), "window scroll event")
   def scroll_event = check_event(win)
   def scroll_data = event_data(scroll_event)
   assert(scroll_data.get("dx", 0.0) == 1.0 && scroll_data.get("dy", 0.0) == -1.0 && scroll_pos(win) == [1.0, -1.0], "window scroll state")
   assert(push_event(win, EVENT_KEY_PRESSED, {"key": KEY_W, "scancode": 25, "mod": MOD_SHIFT}), "window focus key setup")
   check_event(win)
   assert(key_down(win, KEY_W) && mod_down(win, MOD_SHIFT), "window key state before focus loss")
   assert(push_event(win, EVENT_FOCUS_OUT), "window focus out event")
   check_event(win)
   assert(!key_down(win, KEY_W) && !mod_down(win, MOD_SHIFT), "window focus loss clears keys")
   assert(push_event(win, EVENT_QUIT), "window quit event")
   assert(should_close(win), "window quit latches close")
   def quit_event = check_event(win)
   assert(quit(quit_event), "window quit recognized")
   assert(count_open() == 0, "window quit leaves open count")
   assert(close(win) && should_close(win) && count_open() == 0, "window close lifecycle")
   print("✓ std.os.ui.window self-test passed")
}
