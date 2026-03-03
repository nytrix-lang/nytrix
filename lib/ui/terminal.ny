;; Keywords: ui terminal
;; terminal emulator for std.ui

module std.ui.terminal (
   init, toggle, draw, handle_event, log, exec, clear,
   is_open, set_font, set_colors,
   get_history, get_input
)

use std.core *
use std.math *
use std.str as str
use std.ui.consts *
use std.ui.gfx *
use std.ui.window as window
use std.ui.input as uin

mut _is_open    = false
mut _input      = ""
mut _history     = []
mut _exec_history = []
mut _hist_idx    = -1
mut _saved_input  = ""
mut _cursor      = 0

mut _font        = 0
mut _bg_color    = color_pack(0.01, 0.01, 0.02, 0.9)
mut _text_color  = color_pack(1, 1, 1, 1)
mut _cyan_color  = color_pack(0.0, 0.9, 1.0, 1.0)

fn init(font, bg_color=0, text_color=0){
   _font = font
   if(bg_color != 0){ _bg_color = bg_color }
   if(text_color != 0){ _text_color = text_color }
}

fn toggle(win){
   _is_open = !_is_open
   def sz = window.size(win)
   window.set_cursor_mode(win, _is_open ? window.CURSOR_NORMAL : window.CURSOR_LOCKED)
   window.set_cursor_pos(win, float(get(sz, 0)) * 0.5, float(get(sz, 1)) * 0.5)
   
   if(_is_open){
      window.set_input_exclusive(win, true)
   } else {
      window.set_input_exclusive(win, false)
   }
}

fn is_open(){ _is_open }

fn log(msg){
   _history = append(_history, to_str(msg))
   if(len(_history) > 100){ _history = slice(_history, 1, 101) }
}

fn clear(){ _history = [] }

fn exec(callback){
   def line = str.strip(_input)
   if(len(line) == 0){ return }
   log("> " + line)
   if(len(_exec_history) == 0 || get(_exec_history, len(_exec_history)-1) != line){
      _exec_history = append(_exec_history, line)
      if(len(_exec_history) > 50){ _exec_history = slice(_exec_history, 1, 51) }
   }
   _hist_idx = -1
   callback(line)
   _input = ""
   _cursor = 0
}

fn draw(ww, wh){
   if(!_is_open){ return }
   def th = wh * 0.4
   draw_rect_fast(0, 0, ww, th, _bg_color)
   draw_rect_fast(0, th - 26, ww, 26, color_pack(0.05, 0.05, 0.1, 1.0))
   
   mut ty = th - 45
   mut bi = len(_history) - 1
   while(bi >= 0 && ty > 5){
      draw_text(_font, get(_history, bi), 10, ty, _text_color)
      ty -= 18 bi -= 1
   }

   def input_y = th - 22
   draw_text(_font, "> " + _input, 10, input_y, _cyan_color)
   def prefix = str.str_slice(_input, 0, _cursor)
   def psz = measure_text_fast(_font, "> " + prefix)
   draw_rect_fast(10.0 + get(psz, 0), input_y, 2, 18, _cyan_color)
}

fn handle_event(typ, data){
   if(!_is_open){ return false }
   if(typ == EVENT_KEY_CHAR){
      def c = dict_get(data, "char")
      if(c != 96 && c != 126){ 
         def left = str.str_slice(_input, 0, _cursor)
         def right = str.str_slice(_input, _cursor, len(_input))
         def ch_str = str.chr(c)
         _input = left + ch_str + right
         _cursor += len(ch_str)
         return true
      }
   } elif(typ == EVENT_KEY_PRESSED){
      def k = dict_get(data, "key")
      def mods = dict_get(data, "mod", 0)
      
      ; Fallback for key pressed (some systems don't send EV_CHAR for some keys)
      if(k >= 32 && k <= 90 && (mods == 0 || mods == MOD_SHIFT)){
           mut ch = k
           if(k >= 65 && k <= 90 && (mods & MOD_SHIFT) == 0){ ch += 32 }
           if(ch != 96 && ch != 126){
              def left = str.str_slice(_input, 0, _cursor)
              def right = str.str_slice(_input, _cursor, len(_input))
              def ch_str = str.chr(ch)
              _input = left + ch_str + right
              _cursor += len(ch_str)
              return true
           }
      }

      if(k == uin.KEY_ENTER){ return 2 } ; Signal exec
      elif(k == uin.KEY_BACKSPACE){ 
         if(_cursor > 0){ 
            mut prev_char_len = 1
            if(_cursor > 1){
               mut b = load8(_input, _cursor - 1)
               if((b & 0xC0) == 0x80){
                  while(_cursor - prev_char_len > 0 && (load8(_input, _cursor - prev_char_len) & 0xC0) == 0x80){ prev_char_len += 1 }
                  if(_cursor - prev_char_len >= 0 && (load8(_input, _cursor - prev_char_len) & 0xC0) != 0xC0){ prev_char_len = 1 }
               }
            }
            _input = str.str_slice(_input, 0, _cursor - prev_char_len) + str.str_slice(_input, _cursor, len(_input))
            _cursor -= prev_char_len
         } 
         return true
      }
      elif(k == uin.KEY_DELETE){
         if(_cursor < len(_input)){
            mut next_char_len = 1 mut b0 = load8(_input, _cursor)
            if((b0 & 0x80) != 0){
               if((b0 & 0xE0) == 0xC0){ next_char_len = 2 }
               elif((b0 & 0xF0) == 0xE0){ next_char_len = 3 }
               elif((b0 & 0xF8) == 0xF0){ next_char_len = 4 }
            }
            _input = str.str_slice(_input, 0, _cursor) + str.str_slice(_input, _cursor + next_char_len, len(_input))
         }
         return true
      }
      elif(k == uin.KEY_LEFT){ if(_cursor > 0){ mut step = 1 while(_cursor - step > 0 && (load8(_input, _cursor - step) & 0xC0) == 0x80){ step += 1 } _cursor -= step } return true }
      elif(k == uin.KEY_RIGHT){ if(_cursor < len(_input)){ mut step = 1 mut b0 = load8(_input, _cursor) if((b0 & 0x80) != 0){ if((b0 & 0xE0) == 0xC0){ step = 2 } elif((b0 & 0xF0) == 0xE0){ step = 3 } elif((b0 & 0xF8) == 0xF0){ step = 4 } } _cursor += step } return true }
      elif(k == uin.KEY_UP){ if(len(_exec_history) > 0){ if(_hist_idx == -1){ _saved_input = _input } if(_hist_idx < len(_exec_history) - 1){ _hist_idx += 1 _input = get(_exec_history, len(_exec_history) - 1 - _hist_idx) _cursor = len(_input) } } return true }
      elif(k == uin.KEY_DOWN){ if(_hist_idx > 0){ _hist_idx -= 1 _input = get(_exec_history, len(_exec_history) - 1 - _hist_idx) _cursor = len(_input) } elif(_hist_idx == 0){ _hist_idx = -1 _input = _saved_input _cursor = len(_input) } return true }
      elif(k == uin.KEY_TAB){ return 3 } ; Signal completion
   }
   false
}

fn set_font(font){ _font = font }
fn set_colors(bg, text, cyan){ _bg_color = bg _text_color = text _cyan_color = cyan }
fn get_history(){ _history }
fn get_input(){ _input }
