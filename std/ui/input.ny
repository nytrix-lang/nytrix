;; Keywords: ui input keyboard mouse
;; Input management for std.ui.

module std.ui.input (
   ;; Key Codes
   KEY_NULL, KEY_ESCAPE, 
   KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_SPACE,
   KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
   
   ;; Modifier flags
   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META,
   
   ;; Mouse Buttons
   MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE,
   
   ;; Logic
   normalize_key, parse_notation, mod_bit_for_key, mods_from_key_states,
   
   ;; High-level (Active Window)
   key_down, key_pressed, mouse_pos, mouse_button_down, mouse_button_pressed
)

use std.core *
use std.text *
use std.ui.consts *
use std.ui.window as uiw
use std.os.time *

;; Keyboard
def KEY_ENTER     = 13
def KEY_TAB       = 9
def KEY_BACKSPACE = 8
def KEY_SPACE     = 32
def KEY_UP        = 1001
def KEY_DOWN      = 1003
def KEY_LEFT      = 1000
def KEY_RIGHT     = 1002

;; Mouse
def MOUSE_LEFT    = 0
def MOUSE_RIGHT   = 1
def MOUSE_MIDDLE  = 2

fn normalize_key(key){
   "Normalizes a key code for stable comparisons across different keyboard layouts and backends."
   ;; Normalize ASCII letters to uppercase for stable chord matching.
   if(key >= 97 && key <= 122){ return key - 32 }
   ;; Normalize common non-printable key aliases from X11 keysyms.
   if(key == 0xFF1B){ return KEY_ESCAPE }
   if(key == 0xFF0D){ return KEY_ENTER }
   if(key == 0xFF08){ return KEY_BACKSPACE }
   if(key == 0xFF09){ return KEY_TAB }
   if(key == 0xFF51){ return KEY_LEFT }
   if(key == 0xFF52){ return KEY_UP }
   if(key == 0xFF53){ return KEY_RIGHT }
   if(key == 0xFF54){ return KEY_DOWN }
   key
}

fn mod_bit_for_key(key){
   "Returns the modifier bitmask corresponding to a native keysym/VK code."
   ;; X11 keysyms + Win32 virtual-key codes for modifier keys.
   if(key == 0xFFE1 || key == 0xFFE2 || key == 16){ return MOD_SHIFT }
   if(key == 0xFFE3 || key == 0xFFE4 || key == 17){ return MOD_CONTROL }
   if(key == 0xFFE9 || key == 0xFFEA || key == 18){ return MOD_ALT }
   if(key == 0xFFEB || key == 0xFFEC || key == 91 || key == 92){ return MOD_SUPER }
   if(key == 0xFFE7 || key == 0xFFE8){ return MOD_META }
   0
}

fn mods_from_key_states(ks){
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
   mut p = upper(tok)
   while(true){
      if(startswith(p, "CONTROL-")){ mods = mods | MOD_CONTROL p = str_slice(p, 8) }
      elif(startswith(p, "CTRL-")){ mods = mods | MOD_CONTROL p = str_slice(p, 5) }
      elif(startswith(p, "C-")){ mods = mods | MOD_CONTROL p = str_slice(p, 2) }
      elif(startswith(p, "SHIFT-")){ mods = mods | MOD_SHIFT p = str_slice(p, 6) }
      elif(startswith(p, "S-")){ mods = mods | MOD_SHIFT p = str_slice(p, 2) }
      elif(startswith(p, "OPTION-")){ mods = mods | MOD_ALT p = str_slice(p, 7) }
      elif(startswith(p, "ALT-")){ mods = mods | MOD_ALT p = str_slice(p, 4) }
      elif(startswith(p, "A-")){ mods = mods | MOD_ALT p = str_slice(p, 2) }
      elif(startswith(p, "M-")){ mods = mods | MOD_ALT p = str_slice(p, 2) } 
      elif(startswith(p, "META-")){ mods = mods | MOD_META p = str_slice(p, 5) }
      elif(startswith(p, "G-")){ mods = mods | MOD_META p = str_slice(p, 2) }
      elif(startswith(p, "COMMAND-")){ mods = mods | MOD_SUPER p = str_slice(p, 8) }
      elif(startswith(p, "CMD-")){ mods = mods | MOD_SUPER p = str_slice(p, 4) }
      elif(startswith(p, "WIN-")){ mods = mods | MOD_SUPER p = str_slice(p, 4) }
      elif(startswith(p, "SUPER-")){ mods = mods | MOD_SUPER p = str_slice(p, 6) }
      elif(startswith(p, "HYPER-")){ mods = mods | MOD_SUPER p = str_slice(p, 6) }
      elif(startswith(p, "H-")){ mods = mods | MOD_SUPER p = str_slice(p, 2) }
      elif(startswith(p, "D-")){ mods = mods | MOD_SUPER p = str_slice(p, 2) }
      else { break }
   }
   mut key = 0
   def pl = str_len(p)
   if(pl == 1){
      key = load8(p, 0)
      if(key >= 97 && key <= 122){ key -= 32 }
   } else {
      if(p == "ESC"){ key = KEY_ESCAPE }
      elif(p == "RET" || p == "ENTER"){ key = KEY_ENTER }
      elif(p == "TAB"){ key = KEY_TAB }
      elif(p == "SPC" || p == "SPACE"){ key = KEY_SPACE }
      elif(p == "SHIFT"){ key = 0xFFE1 }
      elif(p == "CTRL" || p == "CONTROL"){ key = 0xFFE3 }
      elif(p == "ALT" || p == "OPTION"){ key = 0xFFE9 }
      elif(p == "SUPER" || p == "WIN" || p == "CMD" || p == "COMMAND"){ key = 0xFFEB }
      elif(p == "META"){ key = 0xFFE7 }
      elif(p == "UP"){ key = KEY_UP }
      elif(p == "DOWN"){ key = KEY_DOWN }
      elif(p == "LEFT"){ key = KEY_LEFT }
      elif(p == "RIGHT"){ key = KEY_RIGHT }
      elif(p == "DEL" || p == "BACKSPACE"){ key = KEY_BACKSPACE }
   }
   return [key, mods]
}

fn parse_notation(notation){
   "Parses a full key sequence notation (e.g. 'Ctrl-X Ctrl-C')."
   def toks = split(notation, " ")
   mut seq = []
   mut i = 0
   while(i < len(toks)){
      def t = get(toks, i)
      if(str_len(t) > 0){
         seq = append(seq, _parse_single_key(t))
      }
      i += 1
   }
   seq
}

;; High-level (Proxies to active window in std.ui.window)

fn key_down(key){
   "Returns true if the given key is currently held down in the active window."
   def win = uiw.window_last() ;; For now, use last window if no active tracked here
   if(!win){ return false }
   uiw.window_key_down(win, key)
}

fn key_pressed(key){
   "Returns true if the given key was pressed in the active window since the last frame."
   def win = uiw.window_last()
   if(!win){ return false }
   uiw.window_key_pressed(win, key)
}

fn mouse_pos(){
   "Returns the current mouse position in the active window [x, y]."
   def win = uiw.window_last()
   if(!win){ return [0, 0] }
   uiw.window_mouse_position(win)
}

fn mouse_button_down(button){
   "Returns true if the specified mouse button is held down in the active window."
   def win = uiw.window_last()
   if(!win){ return false }
   uiw.window_mouse_button_down(win, button)
}

fn mouse_button_pressed(button){
   "Returns true if the specified mouse button was pressed in the active window."
   def win = uiw.window_last()
   if(!win){ return false }
   uiw.window_mouse_button_down(win, button)
}
