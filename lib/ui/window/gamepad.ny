module std.ui.window.gamepad (
   is_gamepad_connected,
   is_mapped,
   get_gamepad_name,
   get_gamepad_button,
   get_gamepad_axis,
   get_gamepad_guid,
   load_joysticks,
   get_joysticks,
   add_gamepad_mapping,
   get_gamepad_axis_count,
   get_gamepad_button_count,
   GAMEPAD_BUTTONS,
   GAMEPAD_AXES,
   GAMEPAD_BUTTON_MAP
)

use std.core *
use std.ui.window as window
use std.str as str

;; Globals
mut _gamepad_states = dict()
mut _gamepad_names = dict()
mut _joystick_names = dict()

fn load_joysticks(){
   ;; Legacy stub, no longer strictly needed but kept for API compatibility
   0
}

fn get_joysticks(){
   "Returns a list of currently connected joystick IDs."
   mut out = []
   mut jid = 0
   while(jid < 16){
      if(window.joystick_present(jid)){ out = append(out, jid) }
      jid += 1
   }
   out
}

fn add_gamepad_mapping(mapping_string){
   window.update_gamepad_mappings(mapping_string)
}

fn is_mapped(jid){
   "Returns true if the joystick is recognized as a mapped gamepad."
   window.joystick_is_gamepad(jid)
}

fn is_gamepad_connected(jid){
   window.joystick_present(jid) && window.joystick_is_gamepad(jid)
}

fn get_gamepad_name(jid){
   if(window.joystick_is_gamepad(jid)){
      return window.get_gamepad_name(jid)
   }
   window.get_joystick_name(jid)
}

fn get_gamepad_guid(jid){
   window.get_joystick_guid(jid)
}

fn _ensure_state_ptr(jid){
   if(!dict_get(_gamepad_states, jid, 0)){
      _gamepad_states = dict_set(_gamepad_states, jid, malloc(64))
   }
   dict_get(_gamepad_states, jid)
}

fn get_gamepad_axis_count(jid){
   if(!window.joystick_present(jid)){ return 0 }
   if(window.joystick_is_gamepad(jid)){ return 6 }
   mut count_ptr = malloc(4)
   window.get_joystick_axes(jid, count_ptr)
   def count = load32(count_ptr, 0)
   free(count_ptr)
   count
}

fn get_gamepad_button_count(jid){
   if(!window.joystick_present(jid)){ return 0 }
   if(window.joystick_is_gamepad(jid)){ return 15 }
   mut count_ptr = malloc(4)
   window.get_joystick_buttons(jid, count_ptr)
   def count = load32(count_ptr, 0)
   free(count_ptr)
   count
}

fn get_gamepad_button(jid, button){
   if(!window.joystick_present(jid)){ return false }
   def ptr = _ensure_state_ptr(jid)
   if(window.joystick_is_gamepad(jid)){
      if(window.get_gamepad_state(jid, ptr)){
         mut idx = -1
         if(is_int(button)){ idx = button }
         else { idx = dict_get(GAMEPAD_BUTTON_MAP, str.upper(to_str(button)), -1) }
         if(idx >= 0 && idx < 15){ return load8(ptr, idx) != 0 }
      }
   } else {
      ;; Fallback to raw buttons
      mut count_ptr = malloc(4)
      def raw_btns = window.get_joystick_buttons(jid, count_ptr)
      def count = load32(count_ptr, 0)
      free(count_ptr)
      mut idx = -1
      if(is_int(button)){ idx = button }
      else { idx = dict_get(GAMEPAD_BUTTON_MAP, str.upper(to_str(button)), -1) }
      if(idx >= 0 && idx < count){ return load8(raw_btns, idx) != 0 }
   }
   false
}

fn get_gamepad_axis(jid, axis){
   if(!window.joystick_present(jid)){ return 0.0 }
   def ptr = _ensure_state_ptr(jid)
   if(window.joystick_is_gamepad(jid)){
      if(window.get_gamepad_state(jid, ptr)){
         mut idx = -1
         if(is_int(axis)){ idx = axis }
         else { idx = dict_get(GAMEPAD_AXES, str.upper(to_str(axis)), -1) }
         if(idx >= 0 && idx < 6){ return load32_f32(ptr, 16 + idx * 4) }
      }
   } else {
      ;; Fallback to raw axes
      mut count_ptr = malloc(4)
      def raw_axes = window.get_joystick_axes(jid, count_ptr)
      def count = load32(count_ptr, 0)
      free(count_ptr)
      mut idx = -1
      if(is_int(axis)){ idx = axis }
      else { idx = dict_get(GAMEPAD_AXES, str.upper(to_str(axis)), -1) }
      if(idx >= 0 && idx < count){ return load32_f32(raw_axes, idx * 4) }
   }
   0.0
}

def GAMEPAD_BUTTONS = ["A","B","X","Y","LEFT_BUMPER","RIGHT_BUMPER","BACK","START","GUIDE","LEFT_THUMB","RIGHT_THUMB","DPAD_UP","DPAD_RIGHT","DPAD_DOWN","DPAD_LEFT"]
def GAMEPAD_AXES = dict()
GAMEPAD_AXES = dict_set(GAMEPAD_AXES, "LEFTX", 0)
GAMEPAD_AXES = dict_set(GAMEPAD_AXES, "LEFTY", 1)
GAMEPAD_AXES = dict_set(GAMEPAD_AXES, "RIGHTX", 2)
GAMEPAD_AXES = dict_set(GAMEPAD_AXES, "RIGHTY", 3)
GAMEPAD_AXES = dict_set(GAMEPAD_AXES, "LEFTTRIGGER", 4)
GAMEPAD_AXES = dict_set(GAMEPAD_AXES, "RIGHTTRIGGER", 5)

def GAMEPAD_BUTTON_MAP = dict()
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "A", 0)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "B", 1)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "X", 2)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "Y", 3)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "CROSS", 0)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "CIRCLE", 1)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "SQUARE", 2)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "TRIANGLE", 3)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "LEFT_BUMPER", 4)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "RIGHT_BUMPER", 5)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "BACK", 6)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "START", 7)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "GUIDE", 8)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "LEFT_THUMB", 9)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "RIGHT_THUMB", 10)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "DPAD_UP", 11)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "DPAD_RIGHT", 12)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "DPAD_DOWN", 13)
GAMEPAD_BUTTON_MAP = dict_set(GAMEPAD_BUTTON_MAP, "DPAD_LEFT", 14)
