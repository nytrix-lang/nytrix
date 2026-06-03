;; Keywords: window input keyboard mouse gamepad joystick controller
;; Gamepad input normalization, button labels, axes, hats, and mapping state.
module std.os.ui.window.input.gamepad(gamepad_count, gamepads, gamepad_connected, gamepad_mapped, gamepad_name, gamepad_guid, gamepad_axis, gamepad_button, gamepad_axis_count, gamepad_button_count, is_gamepad_connected, is_mapped, get_gamepad_name, get_gamepad_button, get_gamepad_axis, get_gamepad_guid, load_joysticks, get_joysticks, add_gamepad_mapping, get_gamepad_axis_count, get_gamepad_button_count, GAMEPAD_BUTTONS, GAMEPAD_AXES, GAMEPAD_BUTTON_MAP)
use std.core
use std.math.float as fmath
use std.os.ui.window.platform as ui_backend
use std.core.str as str

def _MAX_JOYSTICKS = 16
def _GAMEPAD_AXIS_COUNT = 6
def _GAMEPAD_BUTTON_COUNT = 15
mut _gamepad_states = dict(8)

comptime table GamepadAxisName {
   "LEFTX" -> 0
   "LEFTY" -> 1
   "RIGHTX" -> 2
   "RIGHTY" -> 3
   "LEFTTRIGGER" -> 4
   "RIGHTTRIGGER" -> 5
}

comptime table GamepadButtonName {
   "A", "CROSS" -> 0
   "B", "CIRCLE" -> 1
   "X", "SQUARE" -> 2
   "Y", "TRIANGLE" -> 3
   "LEFT_BUMPER" -> 4
   "RIGHT_BUMPER" -> 5
   "BACK" -> 6
   "START" -> 7
   "GUIDE" -> 8
   "LEFT_THUMB" -> 9
   "RIGHT_THUMB" -> 10
   "DPAD_UP" -> 11
   "DPAD_RIGHT" -> 12
   "DPAD_DOWN" -> 13
   "DPAD_LEFT" -> 14
}

fn _gamepad_alloc(i32: size): ptr {
   def p = zalloc(size)
   if(!p){ panic("gamepad allocation failed") }
   p
}

fn _sanitize_axis(any: v): f64 {
   def fv = fmath.float(v)
   if(fmath.is_nan(fv) || fmath.is_inf(fv)){ return 0.0 }
   if(fv < -1.0){ return -1.0 }
   if(fv > 1.0){ return 1.0 }
   fv
}

fn _resolve_axis_index(any: axis): i32 { is_int(axis) ? int(axis) : comptime match GamepadAxisName(str.upper(to_str(axis)), -1) }

fn _resolve_button_index(any: button): i32 { is_int(button) ? int(button) : comptime match GamepadButtonName(str.upper(to_str(button)), -1) }

fn _in_bounds(i32: idx, i32: count): bool { idx >= 0 && idx < count }

fn _query_axis_snapshot(i32: jid): list {
   mut count_ptr = _gamepad_alloc(4)
   def raw = ui_backend.get_joystick_axes(jid, count_ptr)
   def count = load32(count_ptr, 0)
   free(count_ptr)
   [raw, count]
}

fn _query_button_snapshot(i32: jid): list {
   mut count_ptr = _gamepad_alloc(4)
   def raw = ui_backend.get_joystick_buttons(jid, count_ptr)
   def count = load32(count_ptr, 0)
   free(count_ptr)
   [raw, count]
}

fn _joystick_connected(i32: jid): bool {
   if(jid < 0 || jid >= _MAX_JOYSTICKS){ return false }
   if(ui_backend.joystick_is_gamepad(jid)){ return true }
   def name = ui_backend.get_joystick_name(jid)
   if(name.len > 0 && name != "Unknown"){ return true }
   def buttons = _query_button_snapshot(jid)
   if(int(buttons.get(1, 0)) > 0){ return true }
   def axes = _query_axis_snapshot(jid)
   int(axes.get(1, 0)) > 0
}

fn load_joysticks(): i32 {
   "Returns the number of currently connected joysticks."
   mut jid = 0
   mut count = 0
   while(jid < 16){
      if(ui_backend.joystick_present(jid) || _joystick_connected(jid)){ count += 1 }
      jid += 1
   }
   count
}

fn get_joysticks(): list {
   "Returns a list of currently connected joystick IDs."
   mut out = []
   mut jid = 0
   while(jid < 16){
      if(ui_backend.joystick_present(jid) || _joystick_connected(jid)){ out = out.append(jid) }
      jid += 1
   }
   out
}

fn add_gamepad_mapping(str: mapping_string): bool {
   "Adds one SDL-style mapping string to the native gamepad database."
   ui_backend.update_gamepad_mappings(mapping_string)
}

fn is_mapped(i32: jid): bool {
   "Returns true if the joystick is recognized as a mapped gamepad."
   ui_backend.joystick_is_gamepad(jid)
}

fn is_gamepad_connected(i32: jid): bool {
   "Returns true when a connected joystick is mapped as a gamepad."
   _joystick_connected(jid) && ui_backend.joystick_is_gamepad(jid)
}

fn get_gamepad_name(i32: jid): str {
   "Returns mapped gamepad name when available, else raw joystick name."
   if(ui_backend.joystick_is_gamepad(jid)){ return ui_backend.get_gamepad_name(jid) }
   ui_backend.get_joystick_name(jid)
}

fn get_gamepad_guid(i32: jid): str {
   "Returns the SDL-style GUID for the joystick device."
   ui_backend.get_joystick_guid(jid)
}

fn _ensure_state_ptr(i32: jid): ptr {
   if(!_gamepad_states.get(jid, 0)){ _gamepad_states[jid] = _gamepad_alloc(64) }
   _gamepad_states.get(jid)
}

fn _mapped_state_ptr(i32: jid): ptr {
   if(!ui_backend.joystick_is_gamepad(jid)){ return 0 }
   def p = _ensure_state_ptr(jid)
   ui_backend.get_gamepad_state(jid, p) ? p : 0
}

fn _raw_button_value(i32: jid, i32: idx): bool {
   def raw = _query_button_snapshot(jid)
   def raw_btns = raw.get(0, 0)
   def count = int(raw.get(1, 0))
   raw_btns && _in_bounds(idx, count) && load8(raw_btns, idx) != 0
}

fn _raw_axis_value(i32: jid, i32: idx): f64 {
   def raw = _query_axis_snapshot(jid)
   def raw_axes = raw.get(0, 0)
   def count = int(raw.get(1, 0))
   if(!raw_axes || !_in_bounds(idx, count)){ return 0.0 }
   _sanitize_axis(load32_f32(raw_axes, idx * 4))
}

fn get_gamepad_axis_count(i32: jid): i32 {
   "Returns mapped axis count(6) or raw joystick axis count."
   if(!_joystick_connected(jid)){ return 0 }
   if(is_mapped(jid)){ return 6 }
   int(_query_axis_snapshot(jid).get(1, 0))
}

fn get_gamepad_button_count(i32: jid): i32 {
   "Returns mapped button count(15) or raw joystick button count."
   if(!_joystick_connected(jid)){ return 0 }
   if(is_mapped(jid)){ return 15 }
   int(_query_button_snapshot(jid).get(1, 0))
}

fn get_gamepad_button(i32: jid, any: button): bool {
   "Returns mapped gamepad button state, with raw-joystick fallback."
   if(!_joystick_connected(jid)){ return false }
   def idx = _resolve_button_index(button)
   def ptr = _mapped_state_ptr(jid)
   if(ptr && _in_bounds(idx, _GAMEPAD_BUTTON_COUNT)){ return load8(ptr, idx) != 0 }
   _raw_button_value(jid, idx)
}

fn get_gamepad_axis(i32: jid, any: axis): f64 {
   "Returns mapped gamepad axis value in [-1, 1], with raw fallback."
   if(!_joystick_connected(jid)){ return 0.0 }
   def idx = _resolve_axis_index(axis)
   def ptr = _mapped_state_ptr(jid)
   if(ptr && _in_bounds(idx, _GAMEPAD_AXIS_COUNT)){ return _sanitize_axis(load32_f32(ptr, 16 + idx * 4)) }
   _raw_axis_value(jid, idx)
}

fn gamepad_count(): i32 {
   "Returns the number of connected joysticks."
   load_joysticks()
}

fn gamepads(): list {
   "Returns connected joystick IDs."
   get_joysticks()
}

fn gamepad_connected(i32: jid): bool {
   "Returns true when joystick `jid` is connected and mapped as a gamepad."
   is_gamepad_connected(jid)
}

fn gamepad_mapped(i32: jid): bool {
   "Returns true when joystick `jid` has a gamepad mapping."
   is_mapped(jid)
}

fn gamepad_name(i32: jid): str {
   "Returns mapped gamepad name when available, else raw joystick name."
   get_gamepad_name(jid)
}

fn gamepad_guid(i32: jid): str {
   "Returns the SDL-style GUID for joystick `jid`."
   get_gamepad_guid(jid)
}

fn gamepad_axis(i32: jid, any: axis): f64 {
   "Returns mapped axis value in [-1, 1], with raw fallback."
   get_gamepad_axis(jid, axis)
}

fn gamepad_button(i32: jid, any: button): bool {
   "Returns mapped button state, with raw fallback."
   get_gamepad_button(jid, button)
}

fn gamepad_axis_count(i32: jid): i32 {
   "Returns mapped axis count or raw joystick axis count."
   get_gamepad_axis_count(jid)
}

fn gamepad_button_count(i32: jid): i32 {
   "Returns mapped button count or raw joystick button count."
   get_gamepad_button_count(jid)
}

def GAMEPAD_BUTTONS = [
   "A", "B", "X", "Y", "LEFT_BUMPER", "RIGHT_BUMPER", "BACK", "START",
   "GUIDE", "LEFT_THUMB", "RIGHT_THUMB", "DPAD_UP", "DPAD_RIGHT",
   "DPAD_DOWN", "DPAD_LEFT",
]

def GAMEPAD_AXES = {
   "LEFTX": 0, "LEFTY": 1, "RIGHTX": 2,
   "RIGHTY": 3, "LEFTTRIGGER": 4, "RIGHTTRIGGER": 5,
}

def GAMEPAD_BUTTON_MAP = {
   "A": 0, "CROSS": 0, "B": 1, "CIRCLE": 1, "X": 2, "SQUARE": 2,
   "Y": 3, "TRIANGLE": 3, "LEFT_BUMPER": 4, "RIGHT_BUMPER": 5,
   "BACK": 6, "START": 7, "GUIDE": 8, "LEFT_THUMB": 9,
   "RIGHT_THUMB": 10, "DPAD_UP": 11, "DPAD_RIGHT": 12,
   "DPAD_DOWN": 13, "DPAD_LEFT": 14,
}
