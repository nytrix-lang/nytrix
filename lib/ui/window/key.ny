;; Keywords: ui key keyboard notation
;; Key normalization and chord parsing for std.ui.
;;
;; normalize_key(): converts any native / legacy code TO the canonical Nytrix key code.
;;   Canonical codes = GLFW_KEY_* values (same as window/consts.ny KEY_* constants).
;;   Sources: X11 keysyms (0xFF__), legacy "simplified" 1000-series, lowercase ASCII.

module std.ui.window.key (
   normalize_key, normalize_mod, mod_bit_for_key, mods_from_key_states,
   parse_notation
)

use std.core *
use std.str *
use std.ui.window.consts *

def _MOD_MASK = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META | MOD_CAPS_LOCK | MOD_NUM_LOCK

fn normalize_mod(mod){
   "Masks a modifier bitset down to the supported Nytrix modifier flags."
   mod & _MOD_MASK
}

fn normalize_key(key){
   "Converts any native / legacy key code to the canonical Nytrix (GLFW) key code."
   ;; Lowercase letters → uppercase ASCII (same as GLFW)
   if(key >= 97 && key <= 122){ return key - 32 }
   ;; X11 keysyms (0xFF__) → GLFW codes
   if(key == 0xFF0D){ return 257 } ;; XK_Return  → KEY_ENTER
   if(key == 0xFF08){ return 259 } ;; XK_BackSpace → KEY_BACKSPACE
   if(key == 0xFF09){ return 258 } ;; XK_Tab      → KEY_TAB
   if(key == 0xFF1B){ return 256 } ;; XK_Escape   → KEY_ESCAPE
   if(key == 0xFF51){ return 263 } ;; XK_Left
   if(key == 0xFF52){ return 265 } ;; XK_Up
   if(key == 0xFF53){ return 262 } ;; XK_Right
   if(key == 0xFF54){ return 264 } ;; XK_Down
   if(key == 0xFF55){ return 266 } ;; XK_Prior (PageUp)
   if(key == 0xFF56){ return 267 } ;; XK_Next  (PageDown)
   if(key == 0xFF50){ return 268 } ;; XK_Home
   if(key == 0xFF57){ return 269 } ;; XK_End
   if(key == 0xFF63){ return 260 } ;; XK_Insert
   if(key == 0xFFFF){ return 261 } ;; XK_Delete
   if(key == 0xFFE1 || key == 0xFFE2){ return 340 } ;; XK_Shift_L/R → KEY_LEFT_SHIFT
   if(key == 0xFFE3 || key == 0xFFE4){ return 341 } ;; XK_Control_L/R
   if(key == 0xFFE9 || key == 0xFFEA){ return 342 } ;; XK_Alt_L/R
   if(key == 0xFFEB || key == 0xFFEC){ return 343 } ;; XK_Super_L/R
   if(key == 0xFFE7 || key == 0xFFE8){ return 343 } ;; XK_Meta_L/R → Super
   ;; Legacy 1000-series (old Nytrix scheme) → GLFW
   if(key == 1000){ return 263 } ;; old KEY_LEFT
   if(key == 1001){ return 265 } ;; old KEY_UP
   if(key == 1002){ return 262 } ;; old KEY_RIGHT
   if(key == 1003){ return 264 } ;; old KEY_DOWN
   if(key == 1004){ return 266 } ;; old KEY_PAGE_UP
   if(key == 1005){ return 267 } ;; old KEY_PAGE_DOWN
   if(key == 1006){ return 268 } ;; old KEY_HOME
   if(key == 1007){ return 269 } ;; old KEY_END
   if(key >= 1008 && key <= 1019){ return 290 + (key - 1008) } ;; old KEY_F1–F12
   ;; Legacy ASCII modifier codes → GLFW modifier key codes
   if(key == 16){ return 340 } ;; old KEY_SHIFT → KEY_LEFT_SHIFT
   if(key == 17){ return 341 } ;; old KEY_CTRL  → KEY_LEFT_CONTROL
   if(key == 18){ return 342 } ;; old KEY_ALT
   if(key == 91){ return 343 } ;; old KEY_SUPER
   ;; GLFW codes 256+ pass through unchanged
   key
}

fn mod_bit_for_key(key){
   "Returns the modifier flag bit for a modifier key code."
   if(key == 340 || key == 344 || key == 0xFFE1 || key == 0xFFE2 || key == 16){ return MOD_SHIFT }
   if(key == 341 || key == 345 || key == 0xFFE3 || key == 0xFFE4 || key == 17){ return MOD_CONTROL }
   if(key == 342 || key == 346 || key == 0xFFE9 || key == 0xFFEA || key == 18){ return MOD_ALT }
   if(key == 343 || key == 347 || key == 0xFFEB || key == 0xFFEC || key == 91 || key == 92){ return MOD_SUPER }
   if(key == 0xFFE7 || key == 0xFFE8){ return MOD_META }
   if(key == 280){ return MOD_CAPS_LOCK }
   if(key == 282){ return MOD_NUM_LOCK }
   0
}

fn mods_from_key_states(ks){
   "Reconstructs the active modifier bitset from a key-state dictionary."
   if(!is_dict(ks)){ return 0 }
   mut mods = 0
   if(dict_get(ks, 340, false) || dict_get(ks, 344, false) ||
      dict_get(ks, 0xFFE1, false) || dict_get(ks, 16, false)){ mods = mods | MOD_SHIFT }
   if(dict_get(ks, 341, false) || dict_get(ks, 345, false) ||
      dict_get(ks, 0xFFE3, false) || dict_get(ks, 17, false)){ mods = mods | MOD_CONTROL }
   if(dict_get(ks, 342, false) || dict_get(ks, 346, false) ||
      dict_get(ks, 0xFFE9, false) || dict_get(ks, 18, false)){ mods = mods | MOD_ALT }
   if(dict_get(ks, 343, false) || dict_get(ks, 347, false) ||
      dict_get(ks, 0xFFEB, false) || dict_get(ks, 91, false)){ mods = mods | MOD_SUPER }
   if(dict_get(ks, 0xFFE7, false)){ mods = mods | MOD_META }
   if(dict_get(ks, 280, false)){ mods = mods | MOD_CAPS_LOCK }
   if(dict_get(ks, 282, false)){ mods = mods | MOD_NUM_LOCK }
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
      if(p == "ESC" || p == "ESCAPE"){ key = 256 }
      elif(p == "RET" || p == "RETURN" || p == "ENTER"){ key = 257 }
      elif(p == "TAB"){ key = 258 }
      elif(p == "DEL" || p == "DELETE"){ key = 261 }
      elif(p == "BS" || p == "BACKSPACE"){ key = 259 }
      elif(p == "SPC" || p == "SPACE"){ key = 32 }
      elif(p == "INS" || p == "INSERT"){ key = 260 }
      elif(p == "UP"){ key = 265 }
      elif(p == "DOWN"){ key = 264 }
      elif(p == "LEFT"){ key = 263 }
      elif(p == "RIGHT"){ key = 262 }
      elif(p == "PGUP" || p == "PAGE_UP" || p == "PAGEUP"){ key = 266 }
      elif(p == "PGDN" || p == "PAGE_DOWN" || p == "PAGEDOWN"){ key = 267 }
      elif(p == "HOME"){ key = 268 }
      elif(p == "END"){ key = 269 }
      elif(p == "SHIFT"){ key = 340 }
      elif(p == "CTRL" || p == "CONTROL"){ key = 341 }
      elif(p == "ALT" || p == "OPTION"){ key = 342 }
      elif(p == "SUPER" || p == "WIN" || p == "CMD" || p == "COMMAND"){ key = 343 }
      elif(p == "META"){ key = 343 }
      elif(p == "F1"){  key = 290 } elif(p == "F2"){  key = 291 }
      elif(p == "F3"){  key = 292 } elif(p == "F4"){  key = 293 }
      elif(p == "F5"){  key = 294 } elif(p == "F6"){  key = 295 }
      elif(p == "F7"){  key = 296 } elif(p == "F8"){  key = 297 }
      elif(p == "F9"){  key = 298 } elif(p == "F10"){ key = 299 }
      elif(p == "F11"){ key = 300 } elif(p == "F12"){ key = 301 }
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
