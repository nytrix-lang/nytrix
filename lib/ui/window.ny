;; Keywords: ui window
;; Window/event core — backed by GLFW (static link).

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
   KEY_NULL, KEY_ESCAPE, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META,
   create_window, open_window, window_id, window_title, window_set_title, window_position, window_size,
   window_move, window_resize, window_should_close, window_set_should_close, window_close,
   window_exit_key, window_set_exit_key, window_key_down, window_modifiers, window_mod_down,
   window_mouse_position, window_mouse_button_down, window_mouse_button_pressed,
   window_match_chord, window_bind, window_key_pressed,
   window_push_event, window_check_event, event_type, event_window, event_window_id, event_data,
   window_on_key, poll_events, windows_open, window_last, window_swap_buffers, window_make_current,
   window_blit_buffer, window_update_input
)

use std.core *
use std.core.dict_mod *
use std.ui.consts *
use std.ui.event as ev
use std.os *
use std.os.time *
use std.text as str
use std.ui.glfw as ui_backend

def _MOD_MASK = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META

;; Key sequence helpers

fn _seq_equal(a, b) {
   "Internal: Checks if two key sequences are identical."
   if(len(a) != len(b)){ return false }
   mut i = 0
   while(i < len(a)) {
      def sa = get(a, i)
      def sb = get(b, i)
      if(get(sa, 0) != get(sb, 0) || (get(sa, 1) & _MOD_MASK) != (get(sb, 1) & _MOD_MASK)){ return false }
      i += 1
   }
   true
}

fn _seq_is_prefix(pref, full) {
   "Internal: Checks if one key sequence is a prefix of another."
   if(len(pref) >= len(full)){ return false }
   mut i = 0
   while(i < len(pref)) {
      def sa = get(pref, i)
      def sb = get(full, i)
      if(get(sa, 0) != get(sb, 0) || (get(sa, 1) & _MOD_MASK) != (get(sb, 1) & _MOD_MASK)){ return false }
      i += 1
   }
   true
}

;; Key parsing helpers

fn _str_slice(s, start) {
   "Internal: Returns a slice of a string starting from the given offset."
   def slen = str.str_len(s)
   if(start >= slen){ return "" }
   mut out = malloc(slen - start + 1)
   init_str(out, slen - start)
   mut i = 0
   while(i < (slen - start)) {
      store8(out, load8(s, start + i), i)
      i += 1
   }
   store8(out, 0, slen - start)
   out
}

fn _normalize_mod(mod) { "Internal: Masks modifiers to the supported set." mod & _MOD_MASK }

fn _normalize_key(key) {
   "Internal: Normalizes key codes (e.g., lowercase to uppercase, mapping GLFW/X11 codes)."
   if(key >= 97 && key <= 122){ return key - 32 }
   if(key == 0xFF1B){ return KEY_ESCAPE }
   if(key == 0xFF0D){ return 13 }
   if(key == 0xFF08){ return 8 }
   if(key == 0xFF09){ return 9 }
   if(key == 0xFF51){ return 1000 }
   if(key == 0xFF52){ return 1001 }
   if(key == 0xFF53){ return 1002 }
   if(key == 0xFF54){ return 1003 }
   key
}

fn _mod_bit_for_key(key) {
   "Internal: Returns the modifier bit associated with a specific modifier key."
   if(key == 0xFFE1 || key == 0xFFE2 || key == 16){ return MOD_SHIFT }
   if(key == 0xFFE3 || key == 0xFFE4 || key == 17){ return MOD_CONTROL }
   if(key == 0xFFE9 || key == 0xFFEA || key == 18){ return MOD_ALT }
   if(key == 0xFFEB || key == 0xFFEC || key == 91 || key == 92){ return MOD_SUPER }
   if(key == 0xFFE7 || key == 0xFFE8){ return MOD_META }
   0
}

fn _mods_from_key_states(ks) {
   "Internal: Calculates active modifier bits from the current key states dictionary."
   if(!is_dict(ks)){ return 0 }
   mut mods = 0
   if(dict_get(ks, 0xFFE1, false) || dict_get(ks, 0xFFE2, false) || dict_get(ks, 16, false)){
      mods = mods | MOD_SHIFT
   }
   if(dict_get(ks, 0xFFE3, false) || dict_get(ks, 0xFFE4, false) || dict_get(ks, 17, false)){
      mods = mods | MOD_CONTROL
   }
   if(dict_get(ks, 0xFFE9, false) || dict_get(ks, 0xFFEA, false) || dict_get(ks, 18, false)){
      mods = mods | MOD_ALT
   }
   if(dict_get(ks, 0xFFEB, false) || dict_get(ks, 0xFFEC, false) ||
      dict_get(ks, 91, false) || dict_get(ks, 92, false)){
      mods = mods | MOD_SUPER
   }
   if(dict_get(ks, 0xFFE7, false) || dict_get(ks, 0xFFE8, false)){
      mods = mods | MOD_META
   }
   mods
}

fn _parse_single_key(tok) {
   "Internal: Parses a single key notation string (e.g., 'C-S-a') into [key, mods]."
   mut mods = 0
   mut p = str.upper(tok)
   while(true) {
      if(str.startswith(p, "CONTROL-")){
         mods = mods | MOD_CONTROL
         p = _str_slice(p, 8)
      } elif(str.startswith(p, "CTRL-")){
         mods = mods | MOD_CONTROL
         p = _str_slice(p, 5)
      } elif(str.startswith(p, "C-")){
         mods = mods | MOD_CONTROL
         p = _str_slice(p, 2)
      } elif(str.startswith(p, "SHIFT-")){
         mods = mods | MOD_SHIFT
         p = _str_slice(p, 6)
      } elif(str.startswith(p, "S-")){
         mods = mods | MOD_SHIFT
         p = _str_slice(p, 2)
      } elif(str.startswith(p, "OPTION-")){
         mods = mods | MOD_ALT
         p = _str_slice(p, 7)
      } elif(str.startswith(p, "ALT-")){
         mods = mods | MOD_ALT
         p = _str_slice(p, 4)
      } elif(str.startswith(p, "A-")){
         mods = mods | MOD_ALT
         p = _str_slice(p, 2)
      } elif(str.startswith(p, "M-")){
         mods = mods | MOD_ALT
         p = _str_slice(p, 2)
      } elif(str.startswith(p, "META-")){
         mods = mods | MOD_META
         p = _str_slice(p, 5)
      } elif(str.startswith(p, "G-")){
         mods = mods | MOD_META
         p = _str_slice(p, 2)
      } elif(str.startswith(p, "COMMAND-")){
         mods = mods | MOD_SUPER
         p = _str_slice(p, 8)
      } elif(str.startswith(p, "CMD-")){
         mods = mods | MOD_SUPER
         p = _str_slice(p, 4)
      } elif(str.startswith(p, "WIN-")){
         mods = mods | MOD_SUPER
         p = _str_slice(p, 4)
      } elif(str.startswith(p, "SUPER-")){
         mods = mods | MOD_SUPER
         p = _str_slice(p, 6)
      } elif(str.startswith(p, "HYPER-")){
         mods = mods | MOD_SUPER
         p = _str_slice(p, 6)
      } elif(str.startswith(p, "H-")){
         mods = mods | MOD_SUPER
         p = _str_slice(p, 2)
      } elif(str.startswith(p, "D-")){
         mods = mods | MOD_SUPER
         p = _str_slice(p, 2)
      } else { break }
   }
   mut key = 0
   def _pl = str.upper(p) == p ? str.str_len(p) : 0
   if(str.str_len(p) == 1) {
      key = load8(p, 0)
      if(key >= 97 && key <= 122){ key -= 32 }
   } else {
      if(p == "ESC"){ key = KEY_ESCAPE }
      elif(p == "RET" || p == "ENTER"){ key = 13 }
      elif(p == "TAB"){ key = 9 }
      elif(p == "SPC" || p == "SPACE"){ key = 32 }
      elif(p == "SHIFT"){ key = 0xFFE1 }
      elif(p == "CTRL" || p == "CONTROL"){ key = 0xFFE3 }
      elif(p == "ALT" || p == "OPTION"){ key = 0xFFE9 }
      elif(p == "SUPER" || p == "WIN" || p == "CMD" || p == "COMMAND"){ key = 0xFFEB }
      elif(p == "META"){ key = 0xFFE7 }
      elif(p == "UP"){ key = 1001 }
      elif(p == "DOWN"){ key = 1003 }
      elif(p == "LEFT"){ key = 1000 }
      elif(p == "RIGHT"){ key = 1002 }
      elif(p == "DEL" || p == "BACKSPACE"){ key = 8 }
   }
   return [key, mods]
}

fn _parse_notation(notation) {
   "Internal: Parses a key sequence notation string into a list of [key, mods]."
   def toks = str.split(notation, " ")
   mut seq = []
   mut i = 0
   while(i < len(toks)) {
      def t = get(toks, i)
      if(str.str_len(t) > 0){ seq = append(seq, _parse_single_key(t)) }
      i += 1
   }
   seq
}

;; Global state

mut _next_window_id = 1
mut _windows = []

mut _debug = -1
fn _is_debug() {
   "Internal: Checks if UI debugging is enabled."
   if(_debug == -1) {
      def v = env("NY_UI_DEBUG")
      if(v && (eq(v, "1") || eq(v, "true"))){ _debug = 1 } else { _debug = 0 }
   }
   _debug
}

fn backend() { "Returns the name of the active windowing backend." ui_backend.get_backend_name() }
fn available() { "Returns true if the windowing system is available." true }

fn _is_window(win) {
   "Internal: Validates that an object is a window dictionary."
   is_dict(win) && dict_has(win, "handle")
}

;; Window creation

fn open_window(name, x, y, w, h, flags=0) {
   "Opens a new native window with the specified parameters."
   if(!is_str(name)){ name = to_str(name) }
   if(w < 1){ w = 1 }
   if(h < 1){ h = 1 }
   mut handle = 0
   if((flags & WINDOW_CPU) == 0) {
      handle = ui_backend.create_window(name, w, h, flags)
      if(!handle){ return false }
   }
   mut win = dict(64)
   win = dict_set(win, "handle",         handle)
   win = dict_set(win, "id",             _next_window_id)
   win = dict_set(win, "title",          name)
   win = dict_set(win, "x",              x)
   win = dict_set(win, "y",              y)
   win = dict_set(win, "w",              w)
   win = dict_set(win, "h",              h)
   win = dict_set(win, "flags",          flags)
   win = dict_set(win, "should_close",   false)
   win = dict_set(win, "exit_key",       KEY_ESCAPE)
   win = dict_set(win, "events",         [])
   win = dict_set(win, "visible",        (flags & WINDOW_HIDE) == 0)
   win = dict_set(win, "key_states",     dict(256))
   win = dict_set(win, "last_key_states",dict(256))
   win = dict_set(win, "mouse_x",        0)
   win = dict_set(win, "mouse_y",        0)
   win = dict_set(win, "mouse_buttons",      dict(32))
   win = dict_set(win, "last_mouse_buttons", dict(32))
   win = dict_set(win, "chord_seq",      [])
   win = dict_set(win, "chord_time",     0)
   win = dict_set(win, "bindings",       [])
   win = dict_set(win, "modifiers",      0)
   _windows = append(_windows, win)
   _next_window_id += 1
   if(_is_debug()){ print(f"UI: Creating window '{name}' {w}x{h}") }
   win
}

fn create_window(name, x, y, w, h, flags=0) { "Alias for open_window." open_window(name, x, y, w, h, flags) }

;; Accessors

fn window_id(win) {
   "Returns the unique integer ID of the window."
   if(!_is_window(win)){ return 0 }
   dict_get(win, "id", 0)
}

fn window_title(win) {
   "Returns the current title of the window."
   if(!_is_window(win)){ return "" }
   dict_get(win, "title", "")
}

fn window_set_title(win, title) {
   "Updates the window title."
   if(!_is_window(win)){ return false }
   if(!is_str(title)){ title = to_str(title) }
   dict_set(win, "title", title)
   def handle = dict_get(win, "handle", 0)
   if(handle){ ui_backend.set_title(handle, title) }
   true
}

fn window_position(win) {
   "Returns the window position as [x, y]."
   if(!_is_window(win)){ return [0, 0] }
   [dict_get(win, "x", 0), dict_get(win, "y", 0)]
}

fn window_size(win) {
   "Returns the window dimensions as [width, height]."
   if(!_is_window(win)){ return [0, 0] }
   [dict_get(win, "w", 0), dict_get(win, "h", 0)]
}

fn window_move(win, x, y) {
   "Moves the window to a new position."
   if(!_is_window(win)){ return false }
   dict_set(win, "x", x)
   dict_set(win, "y", y)
   mut data = dict()
   data = dict_set(data, "x", x)
   data = dict_set(data, "y", y)
   window_push_event(win, EVENT_WINDOW_MOVED, data)
}

fn window_resize(win, w, h) {
   "Resizes the window."
   if(!_is_window(win)){ return false }
   if(w < 1){ w = 1 }
   if(h < 1){ h = 1 }
   dict_set(win, "w", w)
   dict_set(win, "h", h)
   mut data = dict()
   data = dict_set(data, "w", w)
   data = dict_set(data, "h", h)
   window_push_event(win, EVENT_WINDOW_RESIZED, data)
}

fn window_should_close(win) {
   "Returns true if the window has been requested to close. Also polls input events."
   if(!_is_window(win)){ return true }
   window_update_input(win)
   ui_backend.poll_events()
   _check_native_state(win)
   !!dict_get(win, "should_close", false)
}

fn window_set_should_close(win, should_close=true) {
   "Sets the window's close request flag."
   if(!_is_window(win)){ return false }
   def old = !!dict_get(win, "should_close", false)
   def now = !!should_close
   dict_set(win, "should_close", now)
   if(now && !old){ window_push_event(win, EVENT_QUIT) }
   if(now) {
      def handle = dict_get(win, "handle", 0)
      if(handle){ ui_backend.set_should_close(handle, 1) }
   }
   true
}

fn window_close(win) { "Closes the window." window_set_should_close(win, true) }

fn window_exit_key(win) {
   "Returns the key code that triggers window closure."
   if(!_is_window(win)){ return KEY_NULL }
   dict_get(win, "exit_key", KEY_ESCAPE)
}

fn window_set_exit_key(win, key) {
   "Configures the key that will trigger window closure."
   if(!_is_window(win)){ return false }
   dict_set(win, "exit_key", key)
   true
}

;; Event queue

fn window_push_event(win, kind, data=0) {
   "Pushes a new event into the window's event queue."
   if(!_is_window(win)){ return false }
   mut q = dict_get(win, "events", [])
   if(!is_list(q)){ q = [] }
   q = ev.queue_push(q, ev.make_event(kind, win, window_id(win), data))
   dict_set(win, "events", q)
   true
}

fn _window_process_internal(win, e) {
   "Internal: Processes an event, updating window state (keys, mouse, geometry)."
   def typ = event_type(e)
   if(typ == EVENT_KEY_PRESSED || typ == EVENT_KEY_RELEASED) {
      mut data = event_data(e)
      def raw_key = is_dict(data) ? dict_get(data, "key", 0) : data
      def key = _normalize_key(raw_key)
      mut mod = _normalize_mod(is_dict(data) ? dict_get(data, "mod", 0) : 0)
      mut ks = dict_get(win, "key_states", 0)
      if(!ks){ ks = dict() }
      ks = dict_set(ks, key, (typ == EVENT_KEY_PRESSED))
      dict_set(win, "key_states", ks)
      def mod_bit = _mod_bit_for_key(key)
      if(mod_bit != 0) {
         mod = _mods_from_key_states(ks)
      } elif(mod == 0) {
         mod = _mods_from_key_states(ks)
      }
      mod = _normalize_mod(mod)
      dict_set(win, "modifiers", mod)
      if(!is_dict(data)){ data = dict() }
      data = dict_set(data, "key",   key)
      data = dict_set(data, "mod",   mod)
      data = dict_set(data, "shift", (mod & MOD_SHIFT)   != 0)
      data = dict_set(data, "ctrl",  (mod & MOD_CONTROL) != 0)
      data = dict_set(data, "alt",   (mod & MOD_ALT)     != 0)
      data = dict_set(data, "super", (mod & MOD_SUPER)   != 0)
      data = dict_set(data, "meta",  (mod & MOD_META)    != 0)
      set_idx(e, 4, data)
      mut consumed = false
      if(typ == EVENT_KEY_PRESSED) {
         if(_mod_bit_for_key(key) == 0) {
            def now = ticks() / 1000000
            mut seq = dict_get(win, "chord_seq", [])
            def last_time = dict_get(win, "chord_time", 0)
            if(len(seq) > 0 && (now - last_time > 1000)){ seq = [] }
            seq = append(seq, [key, mod])
            dict_set(win, "chord_seq",  seq)
            dict_set(win, "chord_time", now)
            mut found_match = false
            mut partial = false
            def binds = dict_get(win, "bindings", [])
            mut i = 0
            while(i < len(binds)) {
               def b = get(binds, i)
               def tseq = get(b, 0)
               if(_seq_equal(tseq, seq)) {
                  def action = get(b, 1)
                  if(is_func(action)){ action() }
                  found_match = true
                  break
               } elif(_seq_is_prefix(seq, tseq)) {
                  partial = true
               }
               i += 1
            }
            if(found_match || partial){ consumed = true }
            if(found_match || !partial){ dict_set(win, "chord_seq", []) }
         }
      }
      if(!consumed && typ == EVENT_KEY_PRESSED && key == window_exit_key(win)) {
         window_set_should_close(win, true)
      }
      return consumed
   } elif(typ == EVENT_MOUSE_POS_CHANGED) {
      def data = event_data(e)
      dict_set(win, "mouse_x", dict_get(data, "x", 0))
      dict_set(win, "mouse_y", dict_get(data, "y", 0))
   } elif(typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED) {
      def data = event_data(e)
      def button = dict_get(data, "button", 0)
      mut mb = dict_get(win, "mouse_buttons", 0)
      if(!mb){ mb = dict() }
      mb = dict_set(mb, button, (typ == EVENT_MOUSE_BUTTON_PRESSED))
      dict_set(win, "mouse_buttons", mb)
      dict_set(win, "mouse_x", dict_get(data, "x", 0))
      dict_set(win, "mouse_y", dict_get(data, "y", 0))
   } elif(typ == EVENT_WINDOW_MOVED) {
      def data = event_data(e)
      if(is_dict(data)) {
         dict_set(win, "x", dict_get(data, "x", dict_get(win, "x", 0)))
         dict_set(win, "y", dict_get(data, "y", dict_get(win, "y", 0)))
      }
   } elif(typ == EVENT_WINDOW_RESIZED) {
      def data = event_data(e)
      if(is_dict(data)) {
         mut nw = dict_get(data, "w", dict_get(win, "w", 0))
         mut nh = dict_get(data, "h", dict_get(win, "h", 0))
         if(nw < 1){ nw = 1 }
         if(nh < 1){ nh = 1 }
         dict_set(win, "w", nw)
         dict_set(win, "h", nh)
         if(dict_has(data, "x")){ dict_set(win, "x", dict_get(data, "x", dict_get(win, "x", 0))) }
         if(dict_has(data, "y")){ dict_set(win, "y", dict_get(data, "y", dict_get(win, "y", 0))) }
      }
   } elif(typ == EVENT_FOCUS_OUT) {
      dict_set(win, "key_states",        dict())
      dict_set(win, "last_key_states",   dict())
      dict_set(win, "modifiers",         0)
      dict_set(win, "mouse_buttons",     dict())
      dict_set(win, "last_mouse_buttons",dict())
      dict_set(win, "chord_seq",         [])
   }
   false
}

fn _check_native_state(win) {
   "Internal: Syncs window state with the native backend (close requests, resizing, input)."
   def handle = dict_get(win, "handle", 0)
   if(!handle){ return }
   if(ui_backend.should_close(handle) && !dict_get(win, "should_close", false)) {
      dict_set(win, "should_close", true)
      window_push_event(win, EVENT_QUIT)
   }
   def sz = ui_backend.get_size(handle)
   def nw = get(sz, 0, 0)
   def nh = get(sz, 1, 0)
   if(nw > 0 && nh > 0 && (nw != dict_get(win, "w", 0) || nh != dict_get(win, "h", 0))) {
      mut data = dict()
      data = dict_set(data, "w", nw)
      data = dict_set(data, "h", nh)
      window_resize(win, nw, nh)
   }

   ; Poll keyboard state from GLFW directly (no callback system).
   ; Map: [glfw_code, ny_code]
   def key_map = [
      [256, 27],    ; GLFW_KEY_ESCAPE  → NY KEY_ESCAPE
      [32,  32],    ; SPACE
      [65,  65], [66,66], [67,67], [68,68], [69,69], [70,70], [71,71], [72,72],
      [73,  73], [74,74], [75,75], [76,76], [77,77], [78,78], [79,79], [80,80],
      [81,  81], [82,82], [83,83], [84,84], [85,85], [86,86], [87,87], [88,88],
      [89,  89], [90,90],
      [48,  48], [49,49], [50,50], [51,51], [52,52],
      [53,  53], [54,54], [55,55], [56,56], [57,57],
      [262, 1002],  ; GLFW_KEY_RIGHT → 1002
      [263, 1000],  ; GLFW_KEY_LEFT  → 1000
      [264, 1003],  ; GLFW_KEY_DOWN  → 1003
      [265, 1001],  ; GLFW_KEY_UP    → 1001
      [340, 16],    ; GLFW_KEY_LEFT_SHIFT → 16
      [341, 17],    ; GLFW_KEY_LEFT_CONTROL → 17
      [342, 18],    ; GLFW_KEY_LEFT_ALT → 18
      [344, 16],    ; GLFW_KEY_RIGHT_SHIFT → 16
      [345, 17],    ; GLFW_KEY_RIGHT_CONTROL → 17
      [257, 13],    ; GLFW_KEY_ENTER → 13
      [259, 8],     ; GLFW_KEY_BACKSPACE → 8
      [258, 9],     ; GLFW_KEY_TAB → 9
      [290, 1004], [291, 1005], [292, 1006], [293, 1007], ; F1-F4
      [294, 1008], [295, 1009], [296, 1010], [297, 1011]  ; F5-F8
   ]

   mut ks = dict_get(win, "key_states", dict(256))
   if(!ks){ ks = dict(256) }
   mut changed = false
   mut i = 0
   while(i < len(key_map)){
      def pair   = get(key_map, i)
      def glfw_k = get(pair, 0)
      def ny_k   = get(pair, 1)
      def now    = ui_backend.get_key(handle, glfw_k) == 1
      def was    = !!dict_get(ks, ny_k, false)
      if(now != was){
         ks = dict_set(ks, ny_k, now)
         changed = true
         def typ = now ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED
         mut data = dict()
         data = dict_set(data, "key",     ny_k)
         data = dict_set(data, "pressed", now)
         data = dict_set(data, "repeat",  false)
         data = dict_set(data, "mod",     0)
         window_push_event(win, typ, data)
         if(now && ny_k == window_exit_key(win)){ window_set_should_close(win, true) }
      }
      i += 1
   }
   if(changed){
      dict_set(win, "key_states", ks)
      dict_set(win, "modifiers", _mods_from_key_states(ks))
   }

   ; Poll mouse cursor position
   def mpos = ui_backend.get_cursor_pos(handle)
   def mx   = int(get(mpos, 0, 0))
   def my   = int(get(mpos, 1, 0))
   def omx  = dict_get(win, "mouse_x", 0)
   def omy  = dict_get(win, "mouse_y", 0)
   if(mx != omx || my != omy){
      dict_set(win, "mouse_x", mx)
      dict_set(win, "mouse_y", my)
      mut mdata = dict()
      mdata = dict_set(mdata, "x", mx)
      mdata = dict_set(mdata, "y", my)
      window_push_event(win, EVENT_MOUSE_POS_CHANGED, mdata)
   }

   ; Poll mouse buttons (0=LMB, 1=RMB, 2=MMB)
   mut mb = dict_get(win, "mouse_buttons", dict(32))
   if(!mb){ mb = dict(32) }
   mut bi = 0
   while(bi < 3){
      def btn_now = ui_backend.get_mouse_button(handle, bi) == 1
      def btn_was = !!dict_get(mb, bi, false)
      if(btn_now != btn_was){
         mb = dict_set(mb, bi, btn_now)
         def btyp = btn_now ? EVENT_MOUSE_BUTTON_PRESSED : EVENT_MOUSE_BUTTON_RELEASED
         mut bdata = dict()
         bdata = dict_set(bdata, "button", bi)
         bdata = dict_set(bdata, "x", dict_get(win, "mouse_x", 0))
         bdata = dict_set(bdata, "y", dict_get(win, "mouse_y", 0))
         window_push_event(win, btyp, bdata)
      }
      bi += 1
   }
   dict_set(win, "mouse_buttons", mb)
}


fn window_check_event(win) {
   "Polls and processes the next pending event for the window."
   if(!_is_window(win)){ return 0 }
   ui_backend.poll_events()
   _check_native_state(win)
   mut q = dict_get(win, "events", [])
   if(!is_list(q) || len(q) == 0){ return 0 }
   mut tries = 0
   while(tries < 16) {
      if(len(q) == 0){ break }
      def e = ev.queue_pop(q)
      dict_set(win, "events", q)
      if(!_window_process_internal(win, e)){ return e }
      tries += 1
      q = dict_get(win, "events", [])
   }
   0
}

;; Event accessors

fn event_type(e)      { "Returns the type of the event." ev.event_type(e) }
fn event_window(_e)    { "Returns the window object associated with the event." ev.event_window(_e) }
fn event_window_id(_e) { "Returns the ID of the window associated with the event." ev.event_window_id(_e) }
fn event_data(_e)      { "Returns the extra data associated with the event." ev.event_data(_e) }

;; Input state

fn window_key_down(win, key) {
   "Returns true if the specified key is currently held down."
   if(!_is_window(win)){ return false }
   key = _normalize_key(key)
   def ks = dict_get(win, "key_states", 0)
   if(!ks){ return false }
   dict_get(ks, key, false)
}

fn window_modifiers(win) {
   "Returns the current bitmask of active modifiers (Ctrl, Shift, etc.)."
   if(!_is_window(win)){ return 0 }
   _normalize_mod(dict_get(win, "modifiers", 0))
}

fn window_mod_down(win, mod) {
   "Returns true if the specified modifier bitmask is currently active."
   if(!_is_window(win)){ return false }
   mod = _normalize_mod(mod)
   if(mod == 0){ return false }
   (window_modifiers(win) & mod) == mod
}

fn window_key_pressed(win, key) {
   "Returns true if the specified key was pressed in the current frame."
   if(!_is_window(win)){ return false }
   key = _normalize_key(key)
   def ks  = dict_get(win, "key_states",      0)
   def lks = dict_get(win, "last_key_states",  0)
   if(!ks || !lks){ return false }
   !!dict_get(ks, key, false) && !dict_get(lks, key, false)
}

fn window_mouse_position(win) {
   "Returns the current mouse cursor position as [x, y]."
   if(!_is_window(win)){ return [0, 0] }
   [dict_get(win, "mouse_x", 0), dict_get(win, "mouse_y", 0)]
}

fn window_mouse_button_down(win, button) {
   "Returns true if the specified mouse button is currently held down."
   if(!_is_window(win)){ return false }
   def mb = dict_get(win, "mouse_buttons", 0)
   if(!mb){ return false }
   dict_get(mb, button, false)
}

fn window_mouse_button_pressed(win, button) {
   "Returns true if the specified mouse button was pressed in the current frame."
   if(!_is_window(win)){ return false }
   def mb  = dict_get(win, "mouse_buttons",      0)
   def lmb = dict_get(win, "last_mouse_buttons", 0)
   if(!mb){ return false }
   !!dict_get(mb, button, false) && !dict_get(lmb, button, false)
}

fn window_on_key(win, key, pressed=true, repeat=false, mod=0) {
   "Internal: Injects a key event into the window state (used by backend callbacks)."
   if(!_is_window(win)){ return false }
   key = _normalize_key(key)
   mod = _normalize_mod(mod)
   mut ks = dict_get(win, "key_states", 0)
   if(!ks){ ks = dict() }
   ks = dict_set(ks, key, !!pressed)
   dict_set(win, "key_states", ks)
   mut cur_mod = mod
   if(cur_mod == 0 || _mod_bit_for_key(key) != 0) {
      cur_mod = _mods_from_key_states(ks)
   }
   dict_set(win, "modifiers", _normalize_mod(cur_mod))
   def typ = pressed ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED
   mut data = dict()
   data = dict_set(data, "key",     key)
   data = dict_set(data, "pressed", !!pressed)
   data = dict_set(data, "repeat",  !!repeat)
   data = dict_set(data, "mod",     _normalize_mod(cur_mod))
   window_push_event(win, typ, data)
   if(pressed && key == window_exit_key(win)){ window_set_should_close(win, true) }
   true
}

fn window_match_chord(event, key, mod=0) {
   "Checks if a key event matches a specific key and modifier combination."
   if(event_type(event) != EVENT_KEY_PRESSED){ return false }
   def data = event_data(event)
   if(!is_dict(data)){ return false }
   key = _normalize_key(key)
   mod = _normalize_mod(mod)
   if(dict_get(data, "key", 0) != key){ return false }
   if(mod != 0 && (_normalize_mod(dict_get(data, "mod", 0)) & mod) != mod){ return false }
   true
}

fn window_bind(win, notation, action) {
   "Binds a key sequence notation (e.g., 'C-x C-c') to an action function."
   if(!_is_window(win) || !is_str(notation)){ return false }
   def seq = _parse_notation(notation)
   if(len(seq) == 0){ return false }
   mut b = dict_get(win, "bindings", [])
   if(!is_list(b)){ b = [] }
   mut found = false
   mut i = 0
   while(i < len(b)) {
      def item = get(b, i)
      if(_seq_equal(get(item, 0), seq)) {
         set_idx(item, 1, action)
         found = true
         break
      }
      i += 1
   }
   if(!found){ b = append(b, [seq, action]) }
   dict_set(win, "bindings", b)
   true
}

fn window_update_input(win) {
   "Internal: Cycles input state at the start of a frame (swap current to last)."
   if(!_is_window(win)){ return }
   def ks = dict_get(win, "key_states", dict())
   def mb = dict_get(win, "mouse_buttons", dict())
   dict_set(win, "last_key_states",    ks)
   dict_set(win, "last_mouse_buttons", mb)
   dict_set(win, "key_states",         dict_clone(ks))
   dict_set(win, "mouse_buttons",      dict_clone(mb))
}

;; Frame / buffer

fn window_swap_buffers(win) {
   "Swaps the window's front and back buffers."
   if(!_is_window(win)){ return }
   def handle = dict_get(win, "handle", 0)
   if(handle){ ui_backend.swap_buffers(handle) }
}

fn window_make_current(_win) {
   "Sets the window as the current graphics context."
   ; No-op in Vulkan/static mode — retained for API compatibility.
}

fn window_blit_buffer(_win, _buf, _w, _h) {
   "Blits a raw pixel buffer into the window."
   ; CPU blit not yet implemented in the static GLFW backend.
   0
}

;; Global window management

fn poll_events() {
   "Polls for pending OS events for all managed windows."
   ui_backend.poll_events()
   mut i = 0
   while(i < len(_windows)) {
      def w = get(_windows, i)
      if(_is_window(w)){ _check_native_state(w) }
      i += 1
   }
   0
}

fn windows_open() {
   "Returns the count of windows that are currently open."
   mut i = 0
   mut n = 0
   while(i < len(_windows)) {
      def w = get(_windows, i)
      if(_is_window(w) && !window_should_close(w)){ n += 1 }
      i += 1
   }
   n
}

fn window_last() {
   "Returns the most recently created window object."
   if(len(_windows) == 0){ return 0 }
   def w = get(_windows, len(_windows) - 1, 0)
   if(_is_window(w)){ return w }
   0
}
