;; Keywords: ui input keyboard mouse
;; Input management for std.ui.

module std.ui.input (
   ; Key Codes
   KEY_NULL, KEY_ESCAPE,
   KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_SPACE,
   KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,

   ; Modifier flags
   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META,

   ; Mouse Buttons
   MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE,

   ; Logic
   normalize_key, parse_notation, mod_bit_for_key, mods_from_key_states,

   ; High-level (Active Window)
   key_down, key_pressed, key_chord, mouse_pos, mouse_button_down, mouse_button_pressed
)

use std.core *
use std.text *
use std.ui.key as uikey
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

fn normalize_key(key){ "Normalizes a key code for stable comparisons across different keyboard layouts and backends." uikey.normalize_key(key) }

fn mod_bit_for_key(key){ "Returns the modifier bitmask corresponding to a native keysym/VK code." uikey.mod_bit_for_key(key) }

fn mods_from_key_states(ks){ "Reconstructs modifier bitmask from the active key states dictionary." uikey.mods_from_key_states(ks) }

fn parse_notation(notation){ "Parses a full key sequence notation (e.g. 'Ctrl-X Ctrl-C')." uikey.parse_notation(notation) }

;; High-level (Proxies to active window in std.ui.window)

fn _active_window(){
   "Internal: returns the most recent active window, or `0` when none exists."
   uiw.window_last()
}

fn key_down(key){
   "Returns true if the given key is currently held down in the active window."
   def win = _active_window() ; For now, use last window if no active tracked here
   if(!win){ return false }
   uiw.window_key_down(win, key)
}

fn key_pressed(key){
   "Returns true if the given key was pressed in the active window since the last frame."
   def win = _active_window()
   if(!win){ return false }
   uiw.window_key_pressed(win, key)
}

fn key_chord(notation){
   "Returns true if a specific chord (e.g. 'Ctrl-S') was just pressed."
   def win = _active_window()
   if(!win){ return false }
   def seq = parse_notation(notation)
   if(len(seq) != 1){ return false }
   def pair = get(seq, 0)
   if(!uiw.window_key_pressed(win, get(pair, 0))){ return false }
   def mod = get(pair, 1)
   if(mod != 0 && (uiw.window_modifiers(win) & mod) != mod){ return false }
   true
}

fn mouse_pos(){
   "Returns the current mouse position in the active window [x, y]."
   def win = _active_window()
   if(!win){ return [0, 0] }
   uiw.window_mouse_position(win)
}

fn mouse_button_down(button){
   "Returns true if the specified mouse button is held down in the active window."
   def win = _active_window()
   if(!win){ return false }
   uiw.window_mouse_button_down(win, button)
}

fn mouse_button_pressed(button){
   "Returns true if the specified mouse button was pressed in the active window."
   def win = _active_window()
   if(!win){ return false }
   uiw.window_mouse_button_pressed(win, button)
}
