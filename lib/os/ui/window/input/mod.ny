;; Keywords: window input keyboard mouse gamepad joystick controller key
;; Window-input facade for key parsing, chords, mouse buttons, and gamepad input.
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
   key_down, key_pressed, key_chord, mod_down, mouse_pos, mouse_button_down, mouse_button_pressed
)

use std.core
use std.core.str
use std.os.ui.consts
use std.os.ui.window.input.key as uikey

use std.os.ui.window as window

def MOUSE_LEFT    = 0
def MOUSE_RIGHT   = 1
def MOUSE_MIDDLE  = 2

fn normalize_key(any: key): i32 {
   "Converts a key name or code to a standard Nytrix key code."
   uikey.normalize_key(key)
}

fn mod_bit_for_key(any: key): i32 {
   "Returns the modifier bit mask for a specific modifier key(Shift, Ctrl, etc.)."
   uikey.mod_bit_for_key(key)
}

fn mods_from_key_states(any: ks): i32 {
   "Calculates combined modifier flags from a map of current key states."
   uikey.mods_from_key_states(ks)
}

fn parse_notation(str: notation): list {
   "Parses a standard key notation string(e.g., 'C-c') into a list of [key, mods] pairs."
   uikey.parse_notation(notation)
}

fn _active_window(): any { window.last() }

comptime template _active_window_bool1(name, doc, call_fn){
   fn ${name}(any: arg0): bool {
      doc
      def win = _active_window()
      win ? call_fn(win, arg0) : false
   }
}

comptime emit _active_window_bool1(key_down, "Returns true if the specified key is currently held down in the active window.", window.key_down)
comptime emit _active_window_bool1(key_pressed, "Returns true if the specified key was pressed this frame in the active window.", window.key_pressed)

fn key_chord(str: notation): bool {
   "Returns true if a specific key combination(e.g., 'C-x') was triggered this frame."
   def win = _active_window()
   if(!win){ return false }
   def seq = parse_notation(notation)
   if(seq.len != 1){ return false }
   def pair = seq.get(0)
   if(!window.key_pressed(win, pair.get(0))){ return false }
   def mod = pair.get(1)
   if(mod != 0 && (window.get_modifiers(win) & mod) != mod){ return false }
   true
}

comptime emit _active_window_bool1(mod_down, "Returns true if the specified modifier flag is set in the active window.", window.mod_down)
comptime emit _active_window_bool1(mouse_button_down, "Returns true if the specified mouse button is currently held down.", window.mouse_down)
comptime emit _active_window_bool1(mouse_button_pressed, "Returns true if the specified mouse button was pressed this frame.", window.mouse_pressed)

fn mouse_pos(): list {
   "Returns the current mouse cursor position [x, y] relative to the active window."
   def win = _active_window()
   win ? window.mouse_pos(win) : [0, 0]
}
