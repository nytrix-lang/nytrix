;; Keywords: ui window
;; Window/event core

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
   CURSOR_NORMAL, CURSOR_HIDDEN, CURSOR_LOCKED,
   KEY_NULL, KEY_ESCAPE, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META,
   create_window, open_window, create, id, title, set_title, pos, size,
   move, resize, should_close, set_should_close, close,
   exit_key, set_exit_key, key_down, key_pressed, mod_down,
   mouse_pos, mouse_down, mouse_pressed,
   set_cursor_mode, cursor_pos, set_cursor_pos,
   focus,
   set_input_exclusive,
   match_chord, bind,
   push_event, check_event, event_type, event_window, event_window_id, event_data,
   on_key, poll_events, count_open, last, swap_buffers, make_current,
   blit_buffer, update_input, set_blit_handler,
   set_clipboard, get_clipboard
)

use std.core *
use std.core.dict_mod *
use std.ui.consts *
use std.ui.event as ev
use std.ui.key as ui_key
use std.os *
use std.os.time *
use std.str as str
use std.util.common as common
use std.ui.glfw as ui_backend

def _MOD_MASK = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META

def CURSOR_NORMAL = ui_backend.CURSOR_NORMAL
def CURSOR_HIDDEN = ui_backend.CURSOR_HIDDEN
def CURSOR_LOCKED = ui_backend.CURSOR_DISABLED

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
fn _is_debug(){ _debug = common.cached_env_truthy(_debug, "NY_UI_DEBUG") _debug }

fn backend(){ ui_backend.get_backend_name() }
fn available(){ true }

fn _is_window(win){ is_dict(win) && dict_has(win, "handle") }

fn _get_handle(win){
   if(is_dict(win)){ return dict_get(win, "handle", 0) }
   win
}

fn _get_win(win){
   def h = _get_handle(win)
   if(!h){ return win }
   def real = dict_get(_window_registry, h, 0)
   if(real){ return real }
   win
}

fn _save_win(win){
   def h = _get_handle(win)
   if(h){ _window_registry = dict_set(_window_registry, h, win) }
}

;; Key sequence helpers

fn _seq_match(a, b, allow_prefix=false){
   if(allow_prefix){ if(len(a) >= len(b)){ return false } }
   elif(len(a) != len(b)){ return false }
   mut i = 0 while(i < len(a)){
      def sa = get(a, i) def sb = get(b, i)
      if(get(sa, 0) != get(sb, 0) || (get(sa, 1) & _MOD_MASK) != (get(sb, 1) & _MOD_MASK)){ return false }
      i += 1
   }
   true
}

fn _seq_equal(a, b){ _seq_match(a, b) }
fn _seq_is_prefix(pref, full){ _seq_match(pref, full, true) }

fn _normalize_mod(mod){ ui_key.normalize_mod(mod) }
fn _normalize_key(key){ ui_key.normalize_key(key) }
fn _mod_bit_for_key(key){ ui_key.mod_bit_for_key(key) }
fn _mods_from_key_states(ks){ ui_key.mods_from_key_states(ks) }
fn _parse_notation(notation){ ui_key.parse_notation(notation) }

fn _key_cb(h, k, sc, act, mods){
   mut win = _get_win(h)
   if(_is_window(win)){
      mut data = dict()
      data = dict_set(data, "key", k)
      data = dict_set(data, "action", act) ; 1=Press, 2=Repeat, 0=Release
      data = dict_set(data, "mod", mods)
      win = dict_set(win, "modifiers", mods)
      _save_win(win)
      if(act == 1 || act == 2){ push_event(win, EVENT_KEY_PRESSED, data) }
      elif(act == 0){ push_event(win, EVENT_KEY_RELEASED, data) }
   }
}

;; Callbacks

fn _char_cb(h, c){
   mut win = _get_win(h)
   if(_is_window(win)){
      mut data = dict() data = dict_set(data, "char", c)
      push_event(win, EVENT_KEY_CHAR, data)
   }
}

fn _size_cb(h, w, h2){
   mut win = _get_win(h)
   if(_is_window(win)){
      win = dict_set(win, "w", w)
      win = dict_set(win, "h", h2)
      _save_win(win)
      mut data = dict() data = dict_set(data, "w", w) data = dict_set(data, "h", h2)
      push_event(win, EVENT_WINDOW_RESIZED, data)
   }
}

fn _pos_cb(h, x, y){
   mut win = _get_win(h)
   if(_is_window(win)){
      win = dict_set(win, "x", x)
      win = dict_set(win, "y", y)
      _save_win(win)
      mut data = dict() data = dict_set(data, "x", x) data = dict_set(data, "y", y)
      push_event(win, EVENT_WINDOW_MOVED, data)
   }
}

fn _scroll_cb(h, xoff, yoff){
  mut win = _get_win(h)
  if(_is_window(win)){
     mut f_xoff = float(xoff) mut f_yoff = float(yoff)
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

fn _mouse_btn_cb(h, btn, act, mods){
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

fn _cursor_pos_cb(h, x, y){
   mut win = _get_win(h)
   if(_is_window(win)){
      def lx = dict_get(win, "mouse_x", 0)
      def ly = dict_get(win, "mouse_y", 0)
      def moved = (int(x) != lx) || (int(y) != ly)

      win = dict_set(win, "mouse_x", int(x))
      win = dict_set(win, "mouse_y", int(y))
      _save_win(win)

      mut data = dict()
      data = dict_set(data, "x", int(x))
      data = dict_set(data, "y", int(y))
      data = dict_set(data, "moved", moved)
      push_event(win, EVENT_MOUSE_POS_CHANGED, data)
   }
}
;; Window creation

fn open_window(name, x, y, w, h, flags=0){
   if(!is_str(name)){ name = to_str(name) }
   if(w < 1){ w = 1 } if(h < 1){ h = 1 }
   mut handle = 0
   if((flags & WINDOW_CPU) == 0){
      handle = ui_backend.create_window(name, w, h, flags)
   }
   if(!handle && (flags & WINDOW_CPU) == 0){ return false }

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

   if(handle){
      _window_registry = dict_set(_window_registry, handle, win)
      ui_backend.set_char_callback(handle, fn_ptr(_char_cb))
      ui_backend.set_key_callback(handle, fn_ptr(_key_cb))
      ui_backend.set_window_size_callback(handle, fn_ptr(_size_cb))
      ui_backend.set_scroll_callback(handle, fn_ptr(_scroll_cb))
      ui_backend.set_mouse_button_callback(handle, fn_ptr(_mouse_btn_cb))
      ui_backend.set_cursor_pos_callback(handle, fn_ptr(_cursor_pos_cb))
   }

   _windows = append(_windows, win)
   if(_is_debug()){ print(f"UI: Creating window '{name}' {w}x{h}") }
   win
}

fn create_window(name, x, y, w, h, flags=0){ open_window(name, x, y, w, h, flags) }
fn create(w, h, name, flags=0){ open_window(name, 0, 0, w, h, flags) }

;; Accessors

fn id(win){ if(!_is_window(win)){ return 0 } dict_get(win, "handle", 0) }
fn title(win){ if(!_is_window(win)){ return "" } dict_get(win, "title", "") }

fn set_title(win, t){
   win = _get_win(win) if(!_is_window(win)){ return false }
   if(!is_str(t)){ t = to_str(t) }
   win = dict_set(win, "title", t)
   _save_win(win)
   def h = dict_get(win, "handle", 0)
   if(h){ ui_backend.set_title(h, t) }
   true
}

fn pos(win){ win = _get_win(win) if(!_is_window(win)){ return [0,0] } [dict_get(win, "x", 0), dict_get(win, "y", 0)] }
fn size(win){ win = _get_win(win) if(!_is_window(win)){ return [0,0] } [dict_get(win, "w", 0), dict_get(win, "h", 0)] }

fn move(win, x, y){
   win = _get_win(win) if(!_is_window(win)){ return false }
   win = dict_set(win, "x", x) win = dict_set(win, "y", y)
   _save_win(win)
   mut data = dict() data = dict_set(data, "x", x) data = dict_set(data, "y", y)
   push_event(win, EVENT_WINDOW_MOVED, data)
}

fn resize(win, w, h){
   win = _get_win(win) if(!_is_window(win)){ return false }
   if(w < 1){ w = 1 } if(h < 1){ h = 1 }
   win = dict_set(win, "w", w) win = dict_set(win, "h", h)
   _save_win(win)
   mut data = dict() data = dict_set(data, "w", w) data = dict_set(data, "h", h)
   push_event(win, EVENT_WINDOW_RESIZED, data)
}

fn should_close(win){
   win = _get_win(win) if(!_is_window(win)){ return true }
   !!dict_get(win, "should_close", false)
}

fn set_should_close(win, sc=true){
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

fn close(win){ set_should_close(win, true) }

fn exit_key(win){ win = _get_win(win) if(!_is_window(win)){ return KEY_NULL } dict_get(win, "exit_key", KEY_ESCAPE) }
fn set_exit_key(win, k){ win = _get_win(win) if(_is_window(win)){ win = dict_set(win, "exit_key", k) _save_win(win) true } else { false } }

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
   }
   [false, e]
}

fn _check_native_state(win){
   win = _get_win(win)
   def handle = dict_get(win, "handle", 0)
   if(!handle){ return }

   if(ui_backend.should_close(handle)){
      if(!dict_get(win, "should_close", false)){
         win = dict_set(win, "should_close", true)
         _save_win(win)
         push_event(win, EVENT_QUIT)
      }
   }

   def sz = ui_backend.get_size(handle)
   def nw = get(sz, 0, 0) def nh = get(sz, 1, 0)
   if(nw > 0 && nh > 0 && (nw != dict_get(win, "w", 0) || nh != dict_get(win, "h", 0))){ resize(win, nw, nh) }

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

   if(_is_debug()){
      ; Scan ALL keys for debug
      mut k = 32 while(k < 348){
         if(ui_backend.get_key(handle, k) == 1){
         print(f"UI: DEBUG: GLFW key {k} is DOWN")
         }
         k += 1
      }
   }

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

   def mpos = ui_backend.get_cursor_pos(handle)
   def mx = int(get(mpos, 0)) def my = int(get(mpos, 1))
   if(mx != dict_get(win, "mouse_x", 0) || my != dict_get(win, "mouse_y", 0)){
      win = dict_set(win, "mouse_x", mx) win = dict_set(win, "mouse_y", my)
      _save_win(win)
      mut md = dict() md = dict_set(md, "x", mx) md = dict_set(md, "y", my)
      push_event(win, EVENT_MOUSE_POS_CHANGED, md)
   }

   mut mb = dict_get(win, "mouse_buttons", 0)
   mut bi = 0 while(bi < 3){
      def bnow = ui_backend.get_mouse_button(handle, bi) == 1
      def bwas = !!dict_get(mb, bi, false)
      if(bnow != bwas){
         mb = dict_set(mb, bi, bnow)
         if(bnow){
         def pb = dict_get(win, "pressed_buttons", 0)
         win = dict_set(win, "pressed_buttons", dict_set(pb, bi, true))
         }
         win = dict_set(win, "mouse_buttons", mb)
         _save_win(win)
         mut bd = dict() bd = dict_set(bd, "button", bi) bd = dict_set(bd, "x", mx) bd = dict_set(bd, "y", my)
         push_event(win, bnow ? EVENT_MOUSE_BUTTON_PRESSED : EVENT_MOUSE_BUTTON_RELEASED, bd)
      }
      bi += 1
   }
   win = dict_set(win, "mouse_buttons", mb)
   _save_win(win)
}

fn check_event(win){
   if(!_is_window(win)){ return 0 }
   def now = ticks()
   if(now - _last_update_t > 1000000){ ; 1ms toggle
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

fn event_type(e){ ev.event_type(e) }
fn event_window(e){ ev.event_window(e) }
fn event_window_id(e){ ev.event_window_id(e) }
fn event_data(e){ ev.event_data(e) }

fn key_down(win, k){ win = _get_win(win) if(!_is_window(win)){ return false } dict_get(dict_get(win, "key_states", 0), _normalize_key(k), false) }
fn get_modifiers(win){ win = _get_win(win) if(!_is_window(win)){ return 0 } _normalize_mod(dict_get(win, "modifiers", 0)) }
fn mod_down(win, m){ win = _get_win(win) if(!_is_window(win)){ return false } def nm = _normalize_mod(m) if(nm == 0){ return false } (get_modifiers(win) & nm) == nm }

fn key_pressed(win, k){
   win = _get_win(win) if(!_is_window(win)){ return false }
   def nk = _normalize_key(k) def ks = dict_get(win, "key_states", 0)
   def lks = dict_get(win, "last_key_states", 0) def pk = dict_get(win, "pressed_keys", 0)
   (!!dict_get(ks, nk, false) && !dict_get(lks, nk, false)) || !!dict_get(pk, nk, false)
}

fn mouse_pos(win){ win = _get_win(win) if(!_is_window(win)){ return [0,0] } [dict_get(win, "mouse_x", 0), dict_get(win, "mouse_y", 0)] }
fn mouse_down(win, b){ win = _get_win(win) if(!_is_window(win)){ return false } dict_get(dict_get(win, "mouse_buttons", 0), b, false) }
fn mouse_pressed(win, b){
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

fn swap_buffers(win){ win = _get_win(win) if(_is_window(win)){ def h = dict_get(win, "handle", 0) if(h){ ui_backend.swap_buffers(h) } } }
fn make_current(_win){ common.touch(_win) }

mut _blit_hook = 0
fn set_blit_handler(h){ _blit_hook = h }
fn blit_buffer(win, buf, w, h){ win = _get_win(win) if(_is_window(win) && is_ptr(buf) && _blit_hook){ _blit_hook(buf, w, h) } 0 }

fn poll_events(){
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

fn set_cursor_mode(win, m){ win = _get_win(win) if(!_is_window(win)){ return false } def h = dict_get(win, "handle", 0) if(h){ ui_backend.set_input_mode(h, 0x00033001, m) } true }
fn focus(win){ win = _get_win(win) if(!_is_window(win)){ return false } def h = dict_get(win, "handle", 0) if(h){ ui_backend.focus_window(h) } true }
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
fn set_input_exclusive(win, val){
   win = _get_win(win)
   if(_is_window(win)){
      win = dict_set(win, "input_exclusive", !!val)
      _save_win(win)
      true
   } else { false }
}

fn set_clipboard(win, s){
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){ ui_backend.set_clipboard(h, s) }
   }
}

fn get_clipboard(win){
   win = _get_win(win)
   if(_is_window(win)){
      def h = dict_get(win, "handle", 0)
      if(h){ return ui_backend.get_clipboard(h) }
   }
   ""
}
