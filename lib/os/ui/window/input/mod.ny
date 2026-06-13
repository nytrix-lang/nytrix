;; Keywords: window input keyboard mouse gamepad joystick controller key os ui
;; Window-input facade for key parsing, chords, mouse buttons, and gamepad input.
;; References:
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.input(
   KEY_NULL, KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_SPACE, KEY_UP, KEY_DOWN, KEY_LEFT,
   KEY_RIGHT, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END, KEY_INSERT, KEY_DELETE, KEY_CAPS_LOCK,
   KEY_SCROLL_LOCK, KEY_NUM_LOCK, KEY_PRINT_SCREEN, KEY_PAUSE, KEY_MENU, KEY_F1, KEY_F2, KEY_F3, KEY_F4,
   KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_F13, KEY_F14, KEY_F15, KEY_F16,
   KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_F25, KEY_KP_0, KEY_KP_1,
   KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9, KEY_KP_ADD,
   KEY_KP_SUBTRACT, KEY_KP_MULTIPLY, KEY_KP_DIVIDE, KEY_KP_DECIMAL, KEY_KP_ENTER, KEY_KP_EQUAL,
   KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL, KEY_LEFT_ALT, KEY_RIGHT_ALT,
   KEY_LEFT_SUPER, KEY_RIGHT_SUPER, KEY_SHIFT, KEY_CTRL, KEY_ALT, KEY_SUPER, KEY_ESC, KEY_GRAVE,
   KEY_MINUS, KEY_EQUAL, KEY_APOSTROPHE, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_SEMICOLON,
   KEY_LEFT_BRACKET, KEY_BACKSLASH, KEY_RIGHT_BRACKET, KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
   KEY_7, KEY_8, KEY_9, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K,
   KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y,
   KEY_Z, KEY_WORLD_1, KEY_WORLD_2, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META, MOUSE_LEFT,
   MOUSE_RIGHT, MOUSE_MIDDLE, normalize_key, parse_notation, mod_bit_for_key, mods_from_key_states,
   is_function_key, function_key_from_scancode, event_function_key, event_key, event_is_key,
   resize_event_size, resize_event_extent,
   key_down, key_pressed, key_chord, mod_down, is_input_event, is_mouse_event, event_mouse_xy, scale_event_xy,
   mouse_pos, mouse_view_pos, mouse_view_state, mouse_button_down, mouse_button_pressed,
   gamepad_count, gamepads, gamepad_connected, gamepad_mapped, gamepad_name, gamepad_guid, gamepad_axis,
   gamepad_button, gamepad_axis_count, gamepad_button_count, is_gamepad_connected, is_mapped,
   get_gamepad_name, get_gamepad_button, get_gamepad_axis, get_gamepad_guid, load_joysticks,
   get_joysticks, add_gamepad_mapping, get_gamepad_axis_count, get_gamepad_button_count,
   GAMEPAD_BUTTONS, GAMEPAD_AXES, GAMEPAD_BUTTON_MAP
)

use std.core
use std.core.str as str
use std.os.ui.window.consts
use std.os.ui.window.input.key as uikey
use std.os.ui.window.input.gamepad
use std.os.ui.window as window

def MOUSE_LEFT    = 0
def MOUSE_RIGHT   = 1
def MOUSE_MIDDLE  = 2

comptime table _CocoaFunctionKey {
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
}

fn normalize_key(any key) i32 {
   "Converts a key name or code to a standard Nytrix key code."
   uikey.normalize_key(key)
}

fn mod_bit_for_key(any key) i32 {
   "Returns the modifier bit mask for a specific modifier key(Shift, Ctrl, etc.)."
   uikey.mod_bit_for_key(key)
}

fn mods_from_key_states(any ks) i32 {
   "Calculates combined modifier flags from a map of current key states."
   uikey.mods_from_key_states(ks)
}

fn parse_notation(str notation) list {
   "Parses a standard key notation string(e.g., 'C-c') into a list of [key, mods] pairs."
   uikey.parse_notation(notation)
}

fn is_function_key(any key) bool {
   "Returns true when a key is in the F1..F25 range."
   def k = normalize_key(key)
   k >= KEY_F1 && k <= KEY_F25
}

fn _input_data_int(any data, any name, any fallback=0) int {
   if(!is_dict(data)){ return int(fallback) }
   def raw = data.get(name, fallback)
   if(is_int(raw) || is_float(raw)){ return int(raw) }
   int(fallback)
}

fn _function_key_range(int code, int first, int last, int out_first) int {
   if(code >= first && code <= last){ return out_first + (code - first) }
   0
}

fn _function_key_from_common_scancode(int code) int {
   mut key = _function_key_range(code, 67, 76, KEY_F1)
   if(key > 0){ return key }
   if(code == 95){ return KEY_F11 }
   if(code == 96){ return KEY_F12 }
   key = _function_key_range(code, 59, 68, KEY_F1)
   if(key > 0){ return key }
   if(code == 87){ return KEY_F11 }
   if(code == 88){ return KEY_F12 }
   key = _function_key_range(code, 183, 194, KEY_F13)
   if(key > 0){ return key }
   key = _function_key_range(code, 0x3B, 0x44, KEY_F1)
   if(key > 0){ return key }
   if(code == 0x57){ return KEY_F11 }
   if(code == 0x58){ return KEY_F12 }
   key = _function_key_range(code, 0x64, 0x6E, KEY_F13)
   if(key > 0){ return key }
   key = _function_key_range(code, 0x70, 0x87, KEY_F1)
   if(key > 0){ return key }
   comptime match _CocoaFunctionKey(code, KEY_NULL)
}

fn function_key_from_scancode(any scancode, any backend_name="") int {
   "Maps common backend scancodes to F-key constants, or KEY_NULL when unknown."
   if(!is_int(scancode) && !is_float(scancode)){ return KEY_NULL }
   def code = int(scancode)
   def b = str.lower(str.strip(to_str(backend_name)))
   if(b == "x11"){
      def key = _function_key_range(code, 67, 76, KEY_F1)
      if(key > 0){ return key }
      if(code == 95){ return KEY_F11 }
      if(code == 96){ return KEY_F12 }
   }
   elif(b == "wayland"){
      def key = _function_key_range(code, 59, 68, KEY_F1)
      if(key > 0){ return key }
      if(code == 87){ return KEY_F11 }
      if(code == 88){ return KEY_F12 }
      return _function_key_range(code, 183, 194, KEY_F13)
   }
   elif(b == "win32"){
      def key = _function_key_range(code, 0x3B, 0x44, KEY_F1)
      if(key > 0){ return key }
      if(code == 0x57){ return KEY_F11 }
      if(code == 0x58){ return KEY_F12 }
      def ext = _function_key_range(code, 0x64, 0x6E, KEY_F13)
      if(ext > 0){ return ext }
      return _function_key_range(code, 0x70, 0x87, KEY_F1)
   }
   elif(b == "cocoa"){
      return comptime match _CocoaFunctionKey(code, KEY_NULL)
   }
   _function_key_from_common_scancode(code)
}

fn event_function_key(any data, any backend_name="") int {
   "Returns the normalized F-key from an event-data dictionary, or KEY_NULL."
   if(!is_dict(data)){ return KEY_NULL }
   def from_key = normalize_key(data.get("key", KEY_NULL))
   if(is_function_key(from_key)){ return from_key }
   def from_raw = normalize_key(data.get("raw_key", data.get("scancode", KEY_NULL)))
   if(is_function_key(from_raw)){ return from_raw }
   mut b = str.lower(str.strip(to_str(backend_name)))
   if(b.len == 0){ b = window.backend() }
   function_key_from_scancode(_input_data_int(data, "scancode", _input_data_int(data, "raw_key", KEY_NULL)), b)
}

fn event_key(any data, any backend_name="") int {
   "Returns the normalized key from an event-data dictionary, including backend F-key scancodes."
   if(!is_dict(data)){ return KEY_NULL }
   def fn_key = event_function_key(data, backend_name)
   if(fn_key != KEY_NULL){ return fn_key }
   def key = _input_data_int(data, "key", KEY_NULL)
   key != KEY_NULL ? normalize_key(key) : KEY_NULL
}

fn event_is_key(any data, any key, any backend_name="") bool {
   "Returns true when event data resolves to the given key."
   event_key(data, backend_name) == normalize_key(key)
}

fn _event_axis_int(any data, any name, any alias, int idx, any fallback=0) int {
   if(is_dict(data)){
      def raw = data.get(name, data.get(alias, fallback))
      if(is_int(raw) || is_float(raw)){ return int(raw) }
      return int(fallback)
   }
   if(is_list(data)){
      def raw = data.get(idx, fallback)
      if(is_int(raw) || is_float(raw)){ return int(raw) }
   }
   int(fallback)
}

fn resize_event_size(any data, any fallback_w=1280, any fallback_h=720) list {
   "Returns `[w, h]` from resize event data, accepting dict aliases or list pairs."
   [
      _event_axis_int(data, "w", "width", 0, fallback_w),
      _event_axis_int(data, "h", "height", 1, fallback_h),
   ]
}

fn resize_event_extent(any win, any data, any current_w=1280, any current_h=720) list {
   "Returns framebuffer-corrected `[w, h]` for a resize event."
   def fallback_w = (float(current_w) > 0.0) ? int(current_w) : 1280
   def fallback_h = (float(current_h) > 0.0) ? int(current_h) : 720
   def event_size = resize_event_size(data, fallback_w, fallback_h)
   def event_w = int(event_size.get(0, fallback_w))
   def event_h = int(event_size.get(1, fallback_h))
   def fsz = window.get_framebuffer_size(win)
   mut live_w = float(fsz.get(0, event_w))
   mut live_h = float(fsz.get(1, event_h))
   if(live_w <= 0.0 || live_h <= 0.0){
      live_w = float(event_w)
      live_h = float(event_h)
   }
   if(event_w > 0 && event_h > 0 && live_w == float(current_w) && live_h == float(current_h) &&
      (event_w != int(current_w) || event_h != int(current_h))){
      live_w = float(event_w)
      live_h = float(event_h)
   }
   [live_w, live_h]
}

fn _active_window() any { window.last() }

fn _resolve_window(any maybe_win=0) any {
   def w = maybe_win ? window.get_win(maybe_win) : 0
   if(is_dict(w) && w.contains("handle")){ return w }
   _active_window()
}

fn key_down(any win_or_key, any key=KEY_NULL) bool {
   "Returns true if a key is currently held down. Accepts key_down(key) or key_down(win, key)."
   if(key == KEY_NULL){
      def win = _active_window()
      return win ? window.key_down(win, win_or_key) : false
   }
   def win = _resolve_window(win_or_key)
   win ? window.key_down(win, key) : false
}

fn key_pressed(any win_or_key, any key=KEY_NULL) bool {
   "Returns true if a key was pressed this frame. Accepts key_pressed(key) or key_pressed(win, key)."
   if(key == KEY_NULL){
      def win = _active_window()
      return win ? window.key_pressed(win, win_or_key) : false
   }
   def win = _resolve_window(win_or_key)
   win ? window.key_pressed(win, key) : false
}

fn key_chord(any win_or_notation, str notation="") bool {
   "Returns true if a specific key combination(e.g., 'C-x') was triggered this frame."
   def active_form = notation.len == 0
   def win = active_form ? _active_window() : _resolve_window(win_or_notation)
   if(active_form){ notation = to_str(win_or_notation) }
   if(!win){ return false }
   def seq = parse_notation(notation)
   if(seq.len != 1){ return false }
   def pair = seq.get(0)
   if(!window.key_pressed(win, pair.get(0))){ return false }
   def mod = pair.get(1)
   if(mod != 0 && (window.get_modifiers(win) & mod) != mod){ return false }
   true
}

fn mod_down(any win_or_mod, any mod=KEY_NULL) bool {
   "Returns true if a modifier bit is active. Accepts mod_down(mod) or mod_down(win, mod)."
   if(mod == KEY_NULL){
      def win = _active_window()
      return win ? window.mod_down(win, win_or_mod) : false
   }
   def win = _resolve_window(win_or_mod)
   win ? window.mod_down(win, mod) : false
}

fn mouse_button_down(any win_or_button=MOUSE_LEFT, any button=nil) bool {
   "Returns true if a mouse button is held. Accepts mouse_button_down(button) or mouse_button_down(win, button)."
   def explicit_win = is_dict(window.get_win(win_or_button))
   def win = explicit_win ? _resolve_window(win_or_button) : _active_window()
   def b = explicit_win ? (button == nil ? MOUSE_LEFT : int(button)) : int(win_or_button)
   win ? window.mouse_down(win, b) : false
}

fn mouse_button_pressed(any win_or_button=MOUSE_LEFT, any button=nil) bool {
   "Returns true if a mouse button was pressed this frame. Accepts mouse_button_pressed(button) or mouse_button_pressed(win, button)."
   def explicit_win = is_dict(window.get_win(win_or_button))
   def win = explicit_win ? _resolve_window(win_or_button) : _active_window()
   def b = explicit_win ? (button == nil ? MOUSE_LEFT : int(button)) : int(win_or_button)
   win ? window.mouse_pressed(win, b) : false
}

fn mouse_pos(any win_ref=0) list {
   "Returns the current mouse cursor position [x, y]. Defaults to the active window."
   def win = _resolve_window(win_ref)
   win ? window.mouse_pos(win) : [0, 0]
}

fn is_input_event(int typ) bool {
   "Returns true for keyboard or mouse input event types."
   typ == EVENT_KEY_PRESSED ||
   typ == EVENT_KEY_RELEASED ||
   typ == EVENT_KEY_CHAR ||
   typ == EVENT_MOUSE_SCROLL ||
   typ == EVENT_MOUSE_BUTTON_PRESSED ||
   typ == EVENT_MOUSE_BUTTON_RELEASED ||
   typ == EVENT_MOUSE_POS_CHANGED
}

fn is_mouse_event(int typ) bool {
   "Returns true for mouse motion, button, or scroll event types."
   typ == EVENT_MOUSE_POS_CHANGED ||
   typ == EVENT_MOUSE_BUTTON_PRESSED ||
   typ == EVENT_MOUSE_BUTTON_RELEASED ||
   typ == EVENT_MOUSE_SCROLL
}

fn event_mouse_xy(any win, any data) list {
   "Returns `[x, y]` from mouse event data, falling back to the live cursor."
   if(is_dict(data) && (data.contains("x") || data.contains("y"))){
      return [float(data.get("x", 0.0)), float(data.get("y", 0.0))]
   }
   def cur = window.cursor_pos(win)
   [float(cur.get(0, 0.0)), float(cur.get(1, 0.0))]
}

fn _view_scale(any win, any view_w, any view_h) list {
   def req_w = float(view_w)
   def req_h = float(view_h)
   def fallback_w = max(1.0, req_w)
   def fallback_h = max(1.0, req_h)
   def sz = win ? window.size(win) : [fallback_w, fallback_h]
   def logical_w = max(1.0, float(sz.get(0, fallback_w)))
   def logical_h = max(1.0, float(sz.get(1, fallback_h)))
   def vw = req_w > 0.0 ? req_w : logical_w
   def vh = req_h > 0.0 ? req_h : logical_h
   [vw, vh, logical_w, logical_h, vw / logical_w, vh / logical_h]
}

fn scale_event_xy(any win, any ev_data, any view_w, any view_h) any {
   "Returns event data with x/y/dx/dy scaled into a framebuffer or view size."
   if(!is_dict(ev_data) || !ev_data.contains("x") || !ev_data.contains("y")){ return ev_data }
   mut out = dict(16)
   def items = dict_items(ev_data)
   mut i = 0
   while(i < items.len){
      def kv = items.get(i, [])
      if(is_list(kv) && kv.len >= 2){ out[kv.get(0)] = kv.get(1) }
      i += 1
   }
   def sc = _view_scale(win, view_w, view_h)
   def vw = float(sc.get(0, view_w))
   def vh = float(sc.get(1, view_h))
   def sx = float(sc.get(4, 1.0))
   def sy = float(sc.get(5, 1.0))
   def raw_x = float(ev_data.get("x", 0.0))
   def raw_y = float(ev_data.get("y", 0.0))
   out["raw_x"] = raw_x
   out["raw_y"] = raw_y
   out["x"] = min(max(raw_x * sx, 0.0), max(vw - 1.0, 0.0))
   out["y"] = min(max(raw_y * sy, 0.0), max(vh - 1.0, 0.0))
   if(ev_data.contains("dx")){ out["dx"] = float(ev_data.get("dx", 0.0)) * sx }
   if(ev_data.contains("dy")){ out["dy"] = float(ev_data.get("dy", 0.0)) * sy }
   out
}

fn mouse_view_pos(any win, any view_w, any view_h) list {
   "Returns mouse position scaled into a framebuffer or view size."
   if(!win){ return [0.0, 0.0] }
   def p = window.mouse_pos(win)
   def sc = _view_scale(win, view_w, view_h)
   def vw = float(sc.get(0, view_w))
   def vh = float(sc.get(1, view_h))
   def sx = float(sc.get(4, 1.0))
   def sy = float(sc.get(5, 1.0))
   [
      min(max(float(p.get(0, 0.0)) * sx, 0.0), vw),
      min(max(float(p.get(1, 0.0)) * sy, 0.0), vh),
   ]
}

fn mouse_view_state(any win, any view_w, any view_h, int button=MOUSE_LEFT) dict {
   "Returns scaled mouse x/y, deltas, scroll deltas, and the requested button state."
   def p = mouse_view_pos(win, view_w, view_h)
   def raw = win ? window.mouse_pos(win) : [0, 0]
   def st = win ? window.mouse_state(win) : dict(8)
   def sc = _view_scale(win, view_w, view_h)
   def sx = float(sc.get(4, 1.0))
   def sy = float(sc.get(5, 1.0))
   def rx = float(raw.get(0, 0.0))
   def ry = float(raw.get(1, 0.0))
   def lx = float(st.get("last_x", rx))
   def ly = float(st.get("last_y", ry))
   def scroll = st.get("scroll", dict(4))
   {
      "x": p.get(0, 0.0),
      "y": p.get(1, 0.0),
      "raw_x": rx,
      "raw_y": ry,
      "last_x": lx,
      "last_y": ly,
      "dx": (rx - lx) * sx,
      "dy": (ry - ly) * sy,
      "moved": st.get("moved", false),
      "down": win ? window.mouse_down(win, button) : false,
      "pressed": win ? window.mouse_pressed(win, button) : false,
      "scroll_x": scroll.get("x", 0.0),
      "scroll_y": scroll.get("y", 0.0),
      "scroll_dx": scroll.get("x", 0.0) - scroll.get("last_x", scroll.get("x", 0.0)),
      "scroll_dy": scroll.get("y", 0.0) - scroll.get("last_y", scroll.get("y", 0.0)),
      "scrolling": st.get("scrolling", false),
   }
}

#main {
   assert(is_function_key(KEY_F1) && is_function_key("F12") && !is_function_key(KEY_A), "function key range")
   assert(function_key_from_scancode(67, "x11") == KEY_F1 && function_key_from_scancode(88, "wayland") == KEY_F12, "backend F-key scancodes")
   assert(function_key_from_scancode(0x3B, "win32") == KEY_F1 && function_key_from_scancode(0x7A, "cocoa") == KEY_F1, "native F-key scancodes")
   assert(event_key({"key": 0, "scancode": 67}, "x11") == KEY_F1 && event_is_key({"raw_key": 0xFFBE}, KEY_F1, "x11"), "event key normalization")
   assert(resize_event_size({"width": 640, "height": 480}, 1, 1) == [640, 480] && resize_event_size([320, 200], 1, 1) == [320, 200], "resize event size")
   def ev = scale_event_xy(0, {"x": 8.0, "y": 4.0, "dx": 2.0}, 16, 8)
   assert(float(ev.get("x", 0.0)) == 8.0 && float(ev.get("dx", 0.0)) == 2.0, "event position scaling")
   assert(!key_down(KEY_A) && !key_down(0, KEY_A) && !mouse_button_down(MOUSE_LEFT) && !mouse_button_down(0, MOUSE_LEFT), "active input without window")
   print("✓ std.os.ui.window.input self-test passed")
}
