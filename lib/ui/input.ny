;; Keywords: ui input keyboard mouse
;; Input management for std.ui.

module std.ui.input (
   ; Key Codes
   KEY_NULL, KEY_ESCAPE,
   KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_SPACE,
   KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
   KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END,
   KEY_W, KEY_S, KEY_A, KEY_D,
   KEY_E, KEY_Q,
   KEY_P, KEY_O, KEY_N,
   KEY_C, KEY_V, KEY_U, KEY_K, KEY_L, KEY_R,
   KEY_SHIFT,
   KEY_CTRL,
   KEY_ESC,
   KEY_GRAVE,
   KEY_DELETE,
   KEY_MINUS, KEY_EQUAL,
   KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
   KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,

   ; Modifier flags
   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META,

   ; Mouse Buttons
   MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE,

   ; Logic
   normalize_key, parse_notation, mod_bit_for_key, mods_from_key_states,

   ; High-level (Active Window)
   key_down, key_pressed, key_chord, mod_down, mouse_pos, mouse_button_down, mouse_button_pressed
)

use std.core *
use std.str *
use std.ui.key as uikey
use std.ui.consts *
use std.ui.window as window
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
def KEY_PAGE_UP   = 1004
def KEY_PAGE_DOWN = 1005
def KEY_HOME      = 1006
def KEY_END       = 1007
def KEY_F1        = 1008
def KEY_F2        = 1009
def KEY_F3        = 1010
def KEY_F4        = 1011
def KEY_F5        = 1012
def KEY_F6        = 1013
def KEY_F7        = 1014
def KEY_F8        = 1015
def KEY_F9        = 1016
def KEY_F10       = 1017
def KEY_F11       = 1018
def KEY_F12       = 1019
def KEY_W = 87
def KEY_S = 83
def KEY_A = 65
def KEY_D = 68
def KEY_E = 69
def KEY_Q = 81
def KEY_P = 80
def KEY_O = 79
def KEY_N = 78
def KEY_C = 67
def KEY_U = 85
def KEY_K = 75
def KEY_L = 76
def KEY_R = 82
def KEY_SHIFT = 16
def KEY_CTRL = 17
def KEY_ESC = 27
def KEY_GRAVE = 96
def KEY_DELETE = 127
def KEY_V = 86
def KEY_MINUS = 45
def KEY_EQUAL = 61

;; Mouse
def MOUSE_LEFT    = 0
def MOUSE_RIGHT   = 1
def MOUSE_MIDDLE  = 2

fn normalize_key(key){ uikey.normalize_key(key) }
fn mod_bit_for_key(key){ uikey.mod_bit_for_key(key) }
fn mods_from_key_states(ks){ uikey.mods_from_key_states(ks) }
fn parse_notation(notation){ uikey.parse_notation(notation) }

;; High-level (Proxies to active window in std.ui.window)

fn _active_window(){
   window.last()
}

fn key_down(key){
   def win = _active_window()
   if(!win){ return false }
   window.key_down(win, key)
}

fn key_pressed(key){
   def win = _active_window()
   if(!win){ return false }
   window.key_pressed(win, key)
}

fn key_chord(notation){
   def win = _active_window()
   if(!win){ return false }
   def seq = parse_notation(notation)
   if(len(seq) != 1){ return false }
   def pair = get(seq, 0)
   if(!window.key_pressed(win, get(pair, 0))){ return false }
   def mod = get(pair, 1)
   if(mod != 0 && (window.get_modifiers(win) & mod) != mod){ return false }
   true
}

fn mod_down(mod){
   def win = _active_window()
   if(!win){ return false }
   window.mod_down(win, mod)
}

fn mouse_pos(){
   def win = _active_window()
   if(!win){ return [0, 0] }
   window.mouse_pos(win)
}

fn mouse_button_down(button){
   def win = _active_window()
   if(!win){ return false }
   window.mouse_down(win, button)
}

fn mouse_button_pressed(button){
   def win = _active_window()
   if(!win){ return false }
   window.mouse_pressed(win, button)
}
