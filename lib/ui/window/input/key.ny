;; Keywords: ui key keyboard notation
;; Shared key normalization and key-chord parsing helpers for std.ui for Nytrixs.

module std.ui.window.input.key (
   normalize_key, normalize_mod, mod_bit_for_key, mods_from_key_states,
   parse_notation
)

use std.core *
use std.str *
use std.ui.consts *

def _MOD_MASK = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META

fn normalize_mod(mod){
   "Masks a modifier bitset down to the supported std.ui modifier flags."
   mod & _MOD_MASK
}

fn normalize_key(key){
   "Normalizes native key codes for stable comparisons across backends."
   if(key >= 97 && key <= 122){ return key - 32 }
   ;; GLFW modifier keys (Linux/Wayland/X11)
   if(key == 340 || key == 344){ return 16 } ; Left/Right Shift
   if(key == 341 || key == 345){ return 17 } ; Left/Right Control
   if(key == 342 || key == 346){ return 18 } ; Left/Right Alt
   if(key == 343 || key == 347){ return 91 } ; Left/Right Super
   if(key == 0xFF0D || key == 257){ return 13 }
   if(key == 0xFF08 || key == 259){ return 8 }
   if(key == 0xFF09 || key == 258){ return 9 }
   if(key == 0xFF1B || key == 256){ return 27 }
   if(key == 0xFF51 || key == 263){ return 1000 } ; Left
   if(key == 0xFF52 || key == 265){ return 1001 } ; Up
   if(key == 0xFF53 || key == 262){ return 1002 } ; Right
   if(key == 0xFF54 || key == 264){ return 1003 } ; Down
   key
}

fn mod_bit_for_key(key){
   "Returns the modifier bit corresponding to the given native key code."
   if(key == 0xFFE1 || key == 0xFFE2 || key == 16 || key == 340 || key == 344){ return MOD_SHIFT }
   if(key == 0xFFE3 || key == 0xFFE4 || key == 17 || key == 341 || key == 345){ return MOD_CONTROL }
   if(key == 0xFFE9 || key == 0xFFEA || key == 18 || key == 342 || key == 346){ return MOD_ALT }
   if(key == 0xFFEB || key == 0xFFEC || key == 91 || key == 92 || key == 343 || key == 347){ return MOD_SUPER }
   if(key == 0xFFE7 || key == 0xFFE8){ return MOD_META }
   0
}

fn mods_from_key_states(ks){
   "Reconstructs the active modifier bitset from a key-state dictionary."
   if(!is_dict(ks)){ return 0 }
   mut mods = 0
   if(dict_get(ks, 0xFFE1, false) || dict_get(ks, 0xFFE2, false) ||
      dict_get(ks, 16, false) || dict_get(ks, 340, false) || dict_get(ks, 344, false)){
      mods = mods | MOD_SHIFT
   }
   if(dict_get(ks, 0xFFE3, false) || dict_get(ks, 0xFFE4, false) ||
      dict_get(ks, 17, false) || dict_get(ks, 341, false) || dict_get(ks, 345, false)){
      mods = mods | MOD_CONTROL
   }
   if(dict_get(ks, 0xFFE9, false) || dict_get(ks, 0xFFEA, false) ||
      dict_get(ks, 18, false) || dict_get(ks, 342, false) || dict_get(ks, 346, false)){
      mods = mods | MOD_ALT
   }
   if(dict_get(ks, 0xFFEB, false) || dict_get(ks, 0xFFEC, false) ||
      dict_get(ks, 91, false) || dict_get(ks, 92, false) ||
      dict_get(ks, 343, false) || dict_get(ks, 347, false)){
      mods = mods | MOD_SUPER
   }
   if(dict_get(ks, 0xFFE7, false) || dict_get(ks, 0xFFE8, false)){
      mods = mods | MOD_META
   }
   mods
}

fn _parse_single_key(tok){
   "Parses a single key notation token into `[key, mod]`."
   mut mods = 0
   mut p = upper(tok)
   while(true){
      if(startswith(p, "CONTROL-")){ mods = mods | MOD_CONTROL
         p = str_slice(p, 8, str_len(p)) }
      elif(startswith(p, "CTRL-")){ mods = mods | MOD_CONTROL
         p = str_slice(p, 5, str_len(p)) }
      elif(startswith(p, "C-")){ mods = mods | MOD_CONTROL
         p = str_slice(p, 2, str_len(p)) }
      elif(startswith(p, "SHIFT-")){ mods = mods | MOD_SHIFT
         p = str_slice(p, 6, str_len(p)) }
      elif(startswith(p, "S-")){ mods = mods | MOD_SHIFT
         p = str_slice(p, 2, str_len(p)) }
      elif(startswith(p, "OPTION-")){ mods = mods | MOD_ALT
         p = str_slice(p, 7, str_len(p)) }
      elif(startswith(p, "ALT-")){ mods = mods | MOD_ALT
         p = str_slice(p, 4, str_len(p)) }
      elif(startswith(p, "A-")){ mods = mods | MOD_ALT
         p = str_slice(p, 2, str_len(p)) }
      elif(startswith(p, "M-")){ mods = mods | MOD_ALT
         p = str_slice(p, 2, str_len(p)) }
      elif(startswith(p, "META-")){ mods = mods | MOD_META
         p = str_slice(p, 5, str_len(p)) }
      elif(startswith(p, "G-")){ mods = mods | MOD_META
         p = str_slice(p, 2, str_len(p)) }
      elif(startswith(p, "COMMAND-")){ mods = mods | MOD_SUPER
         p = str_slice(p, 8, str_len(p)) }
      elif(startswith(p, "CMD-")){ mods = mods | MOD_SUPER
         p = str_slice(p, 4, str_len(p)) }
      elif(startswith(p, "WIN-")){ mods = mods | MOD_SUPER
         p = str_slice(p, 4, str_len(p)) }
      elif(startswith(p, "SUPER-")){ mods = mods | MOD_SUPER
         p = str_slice(p, 6, str_len(p)) }
      elif(startswith(p, "HYPER-")){ mods = mods | MOD_SUPER
         p = str_slice(p, 6, str_len(p)) }
      elif(startswith(p, "H-")){ mods = mods | MOD_SUPER
         p = str_slice(p, 2, str_len(p)) }
      elif(startswith(p, "D-")){ mods = mods | MOD_SUPER
         p = str_slice(p, 2, str_len(p)) }
      else { break }
   }
   mut key = 0
   if(str_len(p) == 1){
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
   [key, mods]
}

fn parse_notation(notation){
   "Parses a key-sequence notation string into a list of `[key, mod]` pairs."
   def toks = split(notation, " ")
   mut seq = []
   mut i = 0
   while(i < len(toks)){
      def tok = get(toks, i)
      if(str_len(tok) > 0){
         seq = append(seq, _parse_single_key(tok))
      }
      i += 1
   }
   seq
}
