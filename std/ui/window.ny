;; Keywords: ui window
;; Nytrix window/event core with OS native backends.

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
   window_mouse_position, window_mouse_button_down,
   window_match_chord, window_bind, window_key_pressed,
   window_push_event, window_check_event, event_type, event_window, event_window_id, event_data,
   window_on_key, poll_events, windows_open, window_last, window_swap_buffers, window_make_current,
   window_blit_buffer
)

use std.core *
use std.core.dict *
use std.ui.consts *
use std.ui.event as ev
use std.os *
use std.os.time *
use std.text as str

use std.ui.backend as ui_backend

def _W_TAG = 0
def _W_ID = 1
def _W_TITLE = 2
def _W_X = 3
def _W_Y = 4
def _W_W = 5
def _W_H = 6
def _W_FLAGS = 7
def _W_SHOULD_CLOSE = 8
def _W_EXIT_KEY = 9
def _W_EVENTS = 10
def _W_VISIBLE = 11
def _W_IS_OPEN = 12
def _W_BACKEND = 13
def _W_KEY_STATES = 14
def _W_MOUSE_X = 15
def _W_MOUSE_Y = 16
def _W_MOUSE_BUTTONS = 17
def _W_CHORD_SEQ = 18
def _W_CHORD_TIME = 19
def _W_BINDINGS = 20
def _W_LAST_KEY_STATES = 21
def _W_NATIVE_CTX = 22
def _W_NATIVE_AUX = 23
def _W_MODIFIERS = 24

def _MOD_MASK = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META

fn _seq_equal(a, b){
   "Checks if two key event sequences (chords) are functionally identical."
   if(len(a) != len(b)){ return false }
   mut i = 0
   def mask = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META
   while(i < len(a)){
      def sa = get(a, i)
      def sb = get(b, i)
      def k1 = get(sa, 0)
      def k2 = get(sb, 0)
      def m1 = (get(sa, 1) & mask)
      def m2 = (get(sb, 1) & mask)
      if(k1 != k2 || m1 != m2){ return false }
      i += 1
   }
   true
}

fn _seq_is_prefix(pref, full){
   "Checks if one key sequence is a proper prefix of another."
   if(len(pref) >= len(full)){ return false }
   mut i = 0
   def mask = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META
   while(i < len(pref)){
      def sa = get(pref, i)
      def sb = get(full, i)
      def k1 = get(sa, 0)
      def k2 = get(sb, 0)
      def m1 = (get(sa, 1) & mask)
      def m2 = (get(sb, 1) & mask)
      if(k1 != k2 || m1 != m2){ return false }
      i += 1
   }
   true
}

fn _str_slice(s, start){
   "Internal helper to slice a string from start to end."
   def slen = str.str_len(s)
   if(start >= slen){ return "" }
   mut out = malloc(slen - start + 1)
   init_str(out, slen - start)
   mut i = 0
   while(i < (slen - start)){
      store8(out, load8(s, start + i), i)
      i += 1
   }
   store8(out, 0, slen - start)
   out
}

fn _normalize_mod(mod){
   "Masks modifier bits to only the supported standard modifiers."
   mod & _MOD_MASK
}

fn _normalize_key(key){
   "Normalizes a key code for stable comparisons across different keyboard layouts and backends."
   ;; Normalize ASCII letters to uppercase for stable chord matching.
   if(key >= 97 && key <= 122){ return key - 32 }
   ;; Normalize common non-printable key aliases from X11 keysyms.
   if(key == 0xFF1B){ return KEY_ESCAPE }
   if(key == 0xFF0D){ return 13 } ;; Enter
   if(key == 0xFF08){ return 8 }  ;; Backspace
   if(key == 0xFF09){ return 9 }  ;; Tab
   if(key == 0xFF51){ return 1000 } ;; Left
   if(key == 0xFF52){ return 1001 } ;; Up
   if(key == 0xFF53){ return 1002 } ;; Right
   if(key == 0xFF54){ return 1003 } ;; Down
   key
}

fn _mod_bit_for_key(key){
   "Returns the modifier bitmask corresponding to a native keysym/VK code."
   ;; X11 keysyms + Win32 virtual-key codes for modifier keys.
   if(key == 0xFFE1 || key == 0xFFE2 || key == 16){ return MOD_SHIFT }
   if(key == 0xFFE3 || key == 0xFFE4 || key == 17){ return MOD_CONTROL }
   if(key == 0xFFE9 || key == 0xFFEA || key == 18){ return MOD_ALT }
   if(key == 0xFFEB || key == 0xFFEC || key == 91 || key == 92){ return MOD_SUPER }
   if(key == 0xFFE7 || key == 0xFFE8){ return MOD_META }
   0
}

fn _mods_from_key_states(ks){
   "Reconstructs modifier bitmask from the active key states dictionary."
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

fn _parse_single_key(tok){
   "Parses a single key notation string (e.g. 'Ctrl-Alt-K') into a [key, mod] list."
   mut mods = 0
   mut p = str.upper(tok)
   while(true){
      if(str.startswith(p, "CONTROL-")){ mods = mods | MOD_CONTROL p = _str_slice(p, 8) }
      elif(str.startswith(p, "CTRL-")){ mods = mods | MOD_CONTROL p = _str_slice(p, 5) }
      elif(str.startswith(p, "C-")){ mods = mods | MOD_CONTROL p = _str_slice(p, 2) }
      elif(str.startswith(p, "SHIFT-")){ mods = mods | MOD_SHIFT p = _str_slice(p, 6) }
      elif(str.startswith(p, "S-")){ mods = mods | MOD_SHIFT p = _str_slice(p, 2) }
      elif(str.startswith(p, "OPTION-")){ mods = mods | MOD_ALT p = _str_slice(p, 7) }
      elif(str.startswith(p, "ALT-")){ mods = mods | MOD_ALT p = _str_slice(p, 4) }
      elif(str.startswith(p, "A-")){ mods = mods | MOD_ALT p = _str_slice(p, 2) }
      elif(str.startswith(p, "M-")){ mods = mods | MOD_ALT p = _str_slice(p, 2) } ;; Emacs-style Meta/Alt
      elif(str.startswith(p, "META-")){ mods = mods | MOD_META p = _str_slice(p, 5) }
      elif(str.startswith(p, "G-")){ mods = mods | MOD_META p = _str_slice(p, 2) }
      elif(str.startswith(p, "COMMAND-")){ mods = mods | MOD_SUPER p = _str_slice(p, 8) }
      elif(str.startswith(p, "CMD-")){ mods = mods | MOD_SUPER p = _str_slice(p, 4) }
      elif(str.startswith(p, "WIN-")){ mods = mods | MOD_SUPER p = _str_slice(p, 4) }
      elif(str.startswith(p, "SUPER-")){ mods = mods | MOD_SUPER p = _str_slice(p, 6) }
      elif(str.startswith(p, "HYPER-")){ mods = mods | MOD_SUPER p = _str_slice(p, 6) }
      elif(str.startswith(p, "H-")){ mods = mods | MOD_SUPER p = _str_slice(p, 2) }
      elif(str.startswith(p, "D-")){ mods = mods | MOD_SUPER p = _str_slice(p, 2) }
      else { break }
   }
   mut key = 0
   def pl = str.str_len(p)
   if(pl == 1){
      key = load8(p, 0)
      if(key >= 97 && key <= 122){ key -= 32 } ;; to upper to match backend/consts
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

fn _parse_notation(notation){
   "Parses a full key sequence notation (e.g. 'Ctrl-X Ctrl-C')."
   def toks = str.split(notation, " ")
   mut seq = []
   mut i = 0
   while(i < len(toks)){
      def t = get(toks, i)
      if(str.str_len(t) > 0){
         seq = append(seq, _parse_single_key(t))
      }
      i += 1
   }
   seq
}

mut _next_window_id = 1
mut _windows = list(8)
mut _sys_backend = 0 ;; unresolved; then 1=X11, 2=Win32, 3=Cocoa, 4=Wayland, -1=mock

fn backend(){
   "Returns the name of the active UI backend."
   ui_backend.get_backend_name()
}

fn available(){
   "Returns true if a native UI backend is available on this system."
   ui_backend.available()
}

mut _debug = -1
fn _is_debug(){
   "Returns true if UI debugging is enabled via NY_UI_DEBUG."
   if(_debug == -1){
      def v = env("NY_UI_DEBUG")
      _debug = (v && (eq(v, "1") || eq(v, "true"))) ? 1 : 0
   }
   _debug
}

fn _is_window(win){
   "Validation helper to check if an object is a valid Nytrix window list."
   is_list(win) && len(win) > 10 && get(win, 0, "") == "std.ui.window"
}

fn _sys_init(){
   "Ensures the underlying platform backend is initialized."
   _sys_backend = ui_backend.init()
}

fn open_window(name, x, y, w, h, flags=0){
   "Creates and opens a new native window."
   if(!is_str(name)){ name = to_str(name) }
   if(w < 1){ w = 1 }
   if(h < 1){ h = 1 }
   _sys_init()
   mut win = list()
   win = append(win, "std.ui.window")
   win = append(win, _next_window_id)
   win = append(win, name)
   win = append(win, x)
   win = append(win, y)
   win = append(win, w)
   win = append(win, h)
   win = append(win, flags)
   win = append(win, false)
   win = append(win, KEY_ESCAPE)
   win = append(win, list(8))
   win = append(win, ((flags & WINDOW_HIDE) == 0))
   win = append(win, true)
   win = append(win, _sys_backend)
   win = append(win, dict(16)) ;; KEY_STATES
   win = append(win, 0)        ;; MOUSE_X
   win = append(win, 0)        ;; MOUSE_Y
   win = append(win, dict(8))  ;; MOUSE_BUTTONS
   win = append(win, [])       ;; CHORD_SEQ
   win = append(win, 0)        ;; CHORD_TIME
   win = append(win, [])       ;; BINDINGS
   win = append(win, dict(16)) ;; LAST_KEY_STATES
   win = append(win, 0)        ;; NATIVE_CTX
   win = append(win, 0)        ;; NATIVE_AUX
   win = append(win, 0)        ;; MODIFIERS (normalized MOD_* mask)
   if(_is_debug()){ print(f"UI: Creating window '{name}' {w}x{h}") }
   def created = ui_backend.create_native_window(win)
   if(!created){ return false }
   _windows = append(_windows, win)
   _next_window_id += 1
   win
}

fn create_window(name, x, y, w, h, flags=0){
   "Alias for open_window."
   open_window(name, x, y, w, h, flags)
}

fn window_id(win){
   "Returns the numeric ID of the window."
   if(!_is_window(win)){ return 0 }
   get(win, _W_ID, 0)
}

fn window_title(win){
   "Returns the current title of the window."
   if(!_is_window(win)){ return "" }
   get(win, _W_TITLE, "")
}

fn window_set_title(win, title){
   "Sets the title of the window."
   if(!_is_window(win)){ return false }
   if(!is_str(title)){ title = to_str(title) }
   set_idx(win, _W_TITLE, title)
   true
}

fn window_position(win){
   "Returns the current screen position coordinates [x, y] of the window."
   if(!_is_window(win)){ return [0, 0] }
   return [get(win, _W_X, 0), get(win, _W_Y, 0)]
}

fn window_size(win){
   "Returns the current dimensions [width, height] of the window."
   if(!_is_window(win)){ return [0, 0] }
   return [get(win, _W_W, 0), get(win, _W_H, 0)]
}

fn window_move(win, x, y){
   "Moves the window to a new screen position."
   if(!_is_window(win)){ return false }
   set_idx(win, _W_X, x)
   set_idx(win, _W_Y, y)
   mut data = dict(4)
   data = dict_set(data, "x", x)
   data = dict_set(data, "y", y)
   window_push_event(win, EVENT_WINDOW_MOVED, data)
}

fn window_resize(win, w, h){
   "Resizes the window."
   if(!_is_window(win)){ return false }
   if(w < 1){ w = 1 }
   if(h < 1){ h = 1 }
   set_idx(win, _W_W, w)
   set_idx(win, _W_H, h)
   mut data = dict(4)
   data = dict_set(data, "w", w)
   data = dict_set(data, "h", h)
   window_push_event(win, EVENT_WINDOW_RESIZED, data)
}

fn window_should_close(win){
   "Returns true if the window has been marked for closure."
   if(!_is_window(win)){ return true }
   !!get(win, _W_SHOULD_CLOSE, false)
}

fn window_set_should_close(win, should_close=true){
   "Marks the window as needing to close."
   if(!_is_window(win)){ return false }
   def old = window_should_close(win)
   def now = !!should_close
   set_idx(win, _W_SHOULD_CLOSE, now)
   if(now && !old){ window_push_event(win, EVENT_QUIT) }
   true
}

fn window_close(win){
   "Initiates the window closure process."
   window_set_should_close(win, true)
}

fn window_exit_key(win){
   "Returns the key code that triggers window closure (default ESC)."
   if(!_is_window(win)){ return KEY_NULL }
   get(win, _W_EXIT_KEY, KEY_ESCAPE)
}

fn window_set_exit_key(win, key){
   "Configures the key that triggers window closure."
   if(!_is_window(win)){ return false }
   set_idx(win, _W_EXIT_KEY, key)
   true
}

fn window_push_event(win, kind, data=0){
   "Pushes an artificial event into the window's event queue."
   if(!_is_window(win)){ return false }
   mut q = get(win, _W_EVENTS, 0)
   if(!is_list(q)){ q = list(8) }
   q = ev.queue_push(q, ev.make_event(kind, win, window_id(win), data))
   set_idx(win, _W_EVENTS, q)
   true
}

fn window_bind(win, notation, action){
   "Binds a key sequence to a callback function."
   if(!_is_window(win) || !is_str(notation)){ return false }
   def seq = _parse_notation(notation)
   if(len(seq) == 0){ return false }
   mut b = get(win, _W_BINDINGS, [])
   if(!is_list(b)){ b = [] }
   mut found = false
   mut i = 0
   while(i < len(b)){
      def item = get(b, i)
      if(_seq_equal(get(item, 0), seq)){
         set_idx(item, 1, action)
         found = true
         break
      }
      i += 1
   }
   if(!found){ b = append(b, [seq, action]) }
   set_idx(win, _W_BINDINGS, b)
   true
}

fn _window_process_internal(win, e){
   "Internal dispatcher for processing raw events and updating window state."
   def typ = event_type(e)
   if(typ == EVENT_KEY_PRESSED || typ == EVENT_KEY_RELEASED){
      mut data = event_data(e)
      def raw_key = is_dict(data) ? dict_get(data, "key", 0) : data
      def key = _normalize_key(raw_key)
      mut mod = _normalize_mod(is_dict(data) ? dict_get(data, "mod", 0) : 0)
      mut ks = get(win, _W_KEY_STATES, 0)
      if(!ks){ ks = dict(16) }
      ks = dict_set(ks, key, (typ == EVENT_KEY_PRESSED))
      set_idx(win, _W_KEY_STATES, ks)
      ;; Resolve effective modifier state from event payload and live key states.
      def mod_bit = _mod_bit_for_key(key)
      if(mod_bit != 0){
         mod = _mods_from_key_states(ks)
      } elif(mod == 0){
         mod = _mods_from_key_states(ks)
      }
      mod = _normalize_mod(mod)
      set_idx(win, _W_MODIFIERS, mod)
      ;; Normalize event payload for downstream consumers.
      if(!is_dict(data)){ data = dict(8) }
      data = dict_set(data, "key", key)
      data = dict_set(data, "mod", mod)
      data = dict_set(data, "shift", (mod & MOD_SHIFT) != 0)
      data = dict_set(data, "ctrl", (mod & MOD_CONTROL) != 0)
      data = dict_set(data, "alt", (mod & MOD_ALT) != 0)
      data = dict_set(data, "super", (mod & MOD_SUPER) != 0)
      data = dict_set(data, "meta", (mod & MOD_META) != 0)
      set_idx(e, 4, data)
      mut consumed = false
      if(typ == EVENT_KEY_PRESSED){
         ;; Ignore modifier keys in the chord sequence.
         if(_mod_bit_for_key(key) == 0){
            ;; Chord processing
            def now = ticks() / 1000000 ;; ms
            mut seq = get(win, _W_CHORD_SEQ, [])
            def last_time = get(win, _W_CHORD_TIME, 0)
            if(len(seq) > 0 && (now - last_time > 1000)){ seq = [] }
            seq = append(seq, [key, mod])
            ;; print("DEBUG: chord seq build:", seq)
            set_idx(win, _W_CHORD_SEQ, seq)
            set_idx(win, _W_CHORD_TIME, now)
            mut found_match = false
            mut partial = false
            def binds = get(win, _W_BINDINGS, [])
            mut i = 0
            while(i < len(binds)){
               def b = get(binds, i)
               def tseq = get(b, 0)
               if(_seq_equal(tseq, seq)){
                  ;; print("DEBUG: chord MATCH!")
                  def action = get(b, 1)
                  if(is_func(action)){ action() }
                  found_match = true
                  break
               } elif(_seq_is_prefix(seq, tseq)){
                  ;; print("DEBUG: chord PARTIAL match")
                  partial = true
               }
               i += 1
            }
            if(found_match || partial){ consumed = true }
            if(found_match || !partial){ set_idx(win, _W_CHORD_SEQ, []) }
         }
      }
      ;; Handle auto-exit
      if(!consumed && typ == EVENT_KEY_PRESSED && key == window_exit_key(win)){
         window_set_should_close(win, true)
      }
      return consumed
   } elif(typ == EVENT_MOUSE_POS_CHANGED){
      def data = event_data(e)
      set_idx(win, _W_MOUSE_X, dict_get(data, "x", 0))
      set_idx(win, _W_MOUSE_Y, dict_get(data, "y", 0))
   } elif(typ == EVENT_MOUSE_BUTTON_PRESSED || typ == EVENT_MOUSE_BUTTON_RELEASED){
      def data = event_data(e)
      def button = dict_get(data, "button", 0)
      mut mb = get(win, _W_MOUSE_BUTTONS, 0)
      if(!mb){ mb = dict(8) }
      mb = dict_set(mb, button, (typ == EVENT_MOUSE_BUTTON_PRESSED))
      set_idx(win, _W_MOUSE_BUTTONS, mb)
      set_idx(win, _W_MOUSE_X, dict_get(data, "x", 0))
      set_idx(win, _W_MOUSE_Y, dict_get(data, "y", 0))
   } elif(typ == EVENT_WINDOW_MOVED){
      def data = event_data(e)
      if(is_dict(data)){
         set_idx(win, _W_X, dict_get(data, "x", get(win, _W_X, 0)))
         set_idx(win, _W_Y, dict_get(data, "y", get(win, _W_Y, 0)))
      }
   } elif(typ == EVENT_WINDOW_RESIZED){
      def data = event_data(e)
      if(is_dict(data)){
         mut nw = dict_get(data, "w", get(win, _W_W, 0))
         mut nh = dict_get(data, "h", get(win, _W_H, 0))
         if(nw < 1){ nw = 1 }
         if(nh < 1){ nh = 1 }
         set_idx(win, _W_W, nw)
         set_idx(win, _W_H, nh)
         if(dict_has(data, "x")){ set_idx(win, _W_X, dict_get(data, "x", get(win, _W_X, 0))) }
         if(dict_has(data, "y")){ set_idx(win, _W_Y, dict_get(data, "y", get(win, _W_Y, 0))) }
      }
   } elif(typ == EVENT_FOCUS_OUT){
      ;; Drop transient input state to prevent sticky keys/modifiers after alt-tab or focus loss.
      set_idx(win, _W_KEY_STATES, dict(16))
      set_idx(win, _W_LAST_KEY_STATES, dict(16))
      set_idx(win, _W_MODIFIERS, 0)
      set_idx(win, _W_MOUSE_BUTTONS, dict(8))
      set_idx(win, _W_CHORD_SEQ, [])
   }
   false
}

fn window_check_event(win){
   "Polls and returns the next pending event for the window."
   if(!_is_window(win)){ return 0 }
   ;; Poll platform evts
   ui_backend.poll_events(win)
   mut q = get(win, _W_EVENTS, 0)
   if(!is_list(q) || len(q) == 0){ return 0 }
   
   mut tries = 0
   while(tries < 16){
      if(len(q) == 0){ break }
      def e = ev.queue_pop(q)
      set_idx(win, _W_EVENTS, q) ;; Update queue state after pop
      if(!_window_process_internal(win, e)){
         return e
      }
      tries += 1
      q = get(win, _W_EVENTS, 0) ;; Refresh queue ref if updated internally
   }
   0
}

fn event_type(e){
   "Returns the type constant of the event."
   ev.event_type(e)
}

fn event_window(e){
   "Returns the window object associated with the event."
   ev.event_window(e)
}

fn event_window_id(e){
   "Returns the unique ID of the window associated with the event."
   ev.event_window_id(e)
}

fn event_data(e){
   "Returns the payload/data associated with the event."
   ev.event_data(e)
}

fn window_key_down(win, key){
   "Returns true if the specified key is currently held down in the window."
   if(!_is_window(win)){ return false }
   key = _normalize_key(key)
   mut ks = get(win, _W_KEY_STATES, 0)
   if(!ks){ return false }
   dict_get(ks, key, false)
}

fn window_modifiers(win){
   "Returns the current modifier bitmask for the window."
   if(!_is_window(win)){ return 0 }
   _normalize_mod(get(win, _W_MODIFIERS, 0))
}

fn window_mod_down(win, mod){
   "Returns true if the specified modifier combination is currently active."
   if(!_is_window(win)){ return false }
   mod = _normalize_mod(mod)
   if(mod == 0){ return false }
   (window_modifiers(win) & mod) == mod
}

fn window_key_pressed(win, key){
   "Returns true if the specified key was pressed in the most recent frame."
   if(!_is_window(win)){ return false }
   key = _normalize_key(key)
   def ks = get(win, _W_KEY_STATES, 0)
   def lks = get(win, _W_LAST_KEY_STATES, 0)
   if(!ks || !lks){ return false }
   !!dict_get(ks, key, false) && !dict_get(lks, key, false)
}

fn window_swap_buffers(win){
   "Swaps the front and back buffers for the window (GPU context)."
   if(!_is_window(win)){ return }
   ui_backend.swap_buffers(win)
}
fn window_make_current(win){
   "Makes the window's graphics context current for the calling thread."
   if(!_is_window(win)){ return }
   ui_backend.make_current(win)
}

fn window_update_input(win){
   "Snapshots the current input state for delta comparisons (e.g. key_pressed)."
   if(!_is_window(win)){ return }
   set_idx(win, _W_LAST_KEY_STATES, dict_clone(get(win, _W_KEY_STATES, dict(16))))
}

fn window_mouse_position(win){
   "Returns the current mouse cursor position [x, y] relative to the window."
   if(!_is_window(win)){ return [0, 0] }
   return [get(win, _W_MOUSE_X, 0), get(win, _W_MOUSE_Y, 0)]
}

fn window_mouse_button_down(win, button){
   "Returns true if the specified mouse button is currently held down."
   if(!_is_window(win)){ return false }
   mut mb = get(win, _W_MOUSE_BUTTONS, 0)
   if(!mb){ return false }
   dict_get(mb, button, false)
}

fn window_on_key(win, key, pressed=true, repeat=false, mod=0){
   "Internal entry point for reporting raw key events into the window system."
   if(!_is_window(win)){ return false }
   key = _normalize_key(key)
   mod = _normalize_mod(mod)
   mut ks = get(win, _W_KEY_STATES, 0)
   if(!ks){ ks = dict(16) }
   ks = dict_set(ks, key, !!pressed)
   set_idx(win, _W_KEY_STATES, ks)
   mut cur_mod = mod
   if(cur_mod == 0 || _mod_bit_for_key(key) != 0){
      cur_mod = _mods_from_key_states(ks)
   }
   set_idx(win, _W_MODIFIERS, _normalize_mod(cur_mod))
   def typ = pressed ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED
   mut data = dict(8)
   data = dict_set(data, "key", key)
   data = dict_set(data, "pressed", !!pressed)
   data = dict_set(data, "repeat", !!repeat)
   data = dict_set(data, "mod", _normalize_mod(cur_mod))
   window_push_event(win, typ, data)
   if(pressed && key == window_exit_key(win)){ window_set_should_close(win, true) }
   true
}

fn window_match_chord(event, key, mod=0){
   "Convenience helper to check if an event matches a specific key+mod chord."
   if(event_type(event) != EVENT_KEY_PRESSED){ return false }
   def data = event_data(event)
   if(!is_dict(data)){ return false }
   key = _normalize_key(key)
   mod = _normalize_mod(mod)
   if(dict_get(data, "key", 0) != key){ return false }
   if(mod != 0 && (_normalize_mod(dict_get(data, "mod", 0)) & mod) != mod){ return false }
   true
}

fn poll_events(){
   "Polls events for all currently open windows."
   mut i = 0
   while(i < len(_windows)){
      def w = get(_windows, i)
      if(_is_window(w)){
         window_check_event(w)
      }
      i += 1
   }
   0
}

fn windows_open(){
   "Returns the count of windows that are currently open."
   mut i = 0
   mut n = 0
   while(i < len(_windows)){
      def w = get(_windows, i)
      if(_is_window(w) && !window_should_close(w)){ n += 1 }
      i += 1
   }
   n
}

fn window_last(){
   "Returns the most recently created window object."
   if(len(_windows) == 0){ return 0 }
   def w = get(_windows, len(_windows) - 1, 0)
   if(_is_window(w)){ return w }
   0
}

fn window_blit_buffer(win, buf, w, h){
   "Blits a raw pixel buffer directly to the window (CPU mode)."
   if(!_is_window(win)){ return 0 }
   ui_backend.blit_buffer(win, buf, w, h)
}

if(comptime{__main()}){
   use std.core.error *

   def w = open_window("std.ui.window.test", 0, 0, 64, 64, WINDOW_HIDE | WINDOW_CPU)
   assert(w, "window create")
   assert(window_id(w) > 0, "window id")
   assert(window_title(w) == "std.ui.window.test", "window title")

   ;; Synthetic key event path: inject + dequeue + normalized payload checks.
   window_on_key(w, 97, true, false, MOD_CONTROL | MOD_SHIFT) ;; 'a'
   def e = window_check_event(w)
   assert(e != 0, "window key event")
   assert(event_type(e) == EVENT_KEY_PRESSED, "window key event type")
   assert(window_key_down(w, 65), "window key down normalized")
   assert(window_mod_down(w, MOD_CONTROL), "window mod ctrl")
   assert(window_mod_down(w, MOD_SHIFT), "window mod shift")
   assert(window_match_chord(e, 65, MOD_CONTROL), "window chord match")

   window_on_key(w, 65, false, false, MOD_CONTROL | MOD_SHIFT)
   def e2 = window_check_event(w)
   assert(e2 != 0, "window key release event")
   assert(event_type(e2) == EVENT_KEY_RELEASED, "window key release type")

   window_close(w)
   assert(window_should_close(w), "window close flag")
   print("✓ std.ui.window tests passed")
}
