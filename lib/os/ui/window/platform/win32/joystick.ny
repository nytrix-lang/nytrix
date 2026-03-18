;; Keywords: platform window backend win32 windows joystick
;; Native Win32 XInput joystick backend for Nytrix.
module std.os.ui.window.platform.win32.joystick(init, terminate, poll_joysticks, joystick_present, get_joystick_name, get_joystick_guid, get_joystick_axes, get_joystick_buttons, joystick_is_gamepad, get_gamepad_state, get_gamepad_name, set_joystick_callback, update_gamepad_mappings)
use std.core
use std.core.mem
use std.os.ui.window.platform.api as backend_api
use std.os.ui.window.platform.gamepad_map as gamepad_map

def XINPUT_MAX_CONTROLLERS = 4
def ERROR_SUCCESS = 0
def ERROR_DEVICE_NOT_CONNECTED = 1167
def _XI_PACKET    = 0 ;; u32
def _XI_BUTTONS   = 8 ;; u16
def _XI_LT        = 10 ;; u8
def _XI_RT        = 11 ;; u8
def _XI_LX        = 12 ;; i16
def _XI_LY        = 14 ;; i16
def _XI_RX        = 16 ;; i16
def _XI_RY        = 18 ;; i16
def _XI_STATE_SZ  = 20
def XINPUT_A           = 0x1000
def XINPUT_B           = 0x2000
def XINPUT_X           = 0x4000
def XINPUT_Y           = 0x8000
def XINPUT_LB          = 0x0100
def XINPUT_RB          = 0x0200
def XINPUT_BACK        = 0x0020
def XINPUT_START       = 0x0010
def XINPUT_GUIDE       = 0x0400
def XINPUT_LSTICK      = 0x0040
def XINPUT_RSTICK      = 0x0080
def XINPUT_DPAD_UP     = 0x0001
def XINPUT_DPAD_DOWN   = 0x0002
def XINPUT_DPAD_LEFT   = 0x0004
def XINPUT_DPAD_RIGHT  = 0x0008
def _AXIS_COUNT   = 6
def _BUTTON_COUNT = 15
mut _initialized = false
mut _joysticks = dict(8)
mut _joystick_callback = 0

fn _has_native_support(): bool {
   #windows { return true }
   false
}

fn _get_js(int: jid): any { _joysticks.get(jid, 0) }

fn _put_js(int: jid, any: js): any {
   _joysticks = _joysticks.set(jid, js)
   js
}

fn _free_js(int: jid): bool {
   def js = _get_js(jid)
   if(!js){ return false }
   def ap, bp = js.get("axes_ptr", 0), js.get("buttons_ptr", 0)
   if(ap){ free(ap) }
   if(bp){ free(bp) }
   _joysticks = _joysticks.set(jid, 0)
   true
}

fn _invoke_callback(int: jid, int: event): any {
   def cb = _joystick_callback
   if(cb){ cb(jid, event) }
}

#windows {
   #include <xinput.h>
} #else {
   fn XInputGetState(any: _idx, any: _state): int { ERROR_DEVICE_NOT_CONNECTED }
}

fn _normalize_thumb(any: v): f64 {
   def fv = float(v)
   if(fv < 0.0){ return fv / 32768.0 }
   fv / 32767.0
}

fn _normalize_trigger(any: v): f64 {
   float(v) / 127.5 - 1.0
}

fn _alloc_js(int: jid): any {
   def axes_ptr    = malloc(_AXIS_COUNT * 4)
   def buttons_ptr = malloc(_BUTTON_COUNT)
   if(!axes_ptr || !buttons_ptr){
      if(axes_ptr){ free(axes_ptr) }
      if(buttons_ptr){ free(buttons_ptr) }
      return 0
   }
   memset(axes_ptr, 0, _AXIS_COUNT * 4)
   memset(buttons_ptr, 0, _BUTTON_COUNT)
   return {
      "connected": true,
      "jid": jid,
      "name": f"XInput Controller {jid}",
      "guid": f"78696e707574{jid}0000000000000000000000000000000",
      "axes_ptr": axes_ptr,
      "buttons_ptr": buttons_ptr,
      "axis_count": _AXIS_COUNT,
      "button_count": _BUTTON_COUNT
   }
}

fn _update_js_state(int: jid, any: xi_state): any {
   def js = _get_js(jid)
   if(!js){ return nil }
   def ap, bp = js.get("axes_ptr", 0), js.get("buttons_ptr", 0)
   if(!ap || !bp){ return nil }
   def wbtns = load16(xi_state, _XI_BUTTONS)
   def lt    = load8(xi_state, _XI_LT)
   def rt    = load8(xi_state, _XI_RT)
   def lx    = load16(xi_state, _XI_LX)
   def ly    = load16(xi_state, _XI_LY)
   def rx    = load16(xi_state, _XI_RX)
   def ry    = load16(xi_state, _XI_RY)
   store32_f32(ap, _normalize_thumb(lx),   0)
   store32_f32(ap, _normalize_thumb(-ly),  4)
   store32_f32(ap, _normalize_thumb(rx),   8)
   store32_f32(ap, _normalize_thumb(-ry),  12)
   store32_f32(ap, _normalize_trigger(lt), 16)
   store32_f32(ap, _normalize_trigger(rt), 20)
   store8(bp, (wbtns & XINPUT_A)          ? 1 : 0, 0)
   store8(bp, (wbtns & XINPUT_B)          ? 1 : 0, 1)
   store8(bp, (wbtns & XINPUT_X)          ? 1 : 0, 2)
   store8(bp, (wbtns & XINPUT_Y)          ? 1 : 0, 3)
   store8(bp, (wbtns & XINPUT_LB)         ? 1 : 0, 4)
   store8(bp, (wbtns & XINPUT_RB)         ? 1 : 0, 5)
   store8(bp, (wbtns & XINPUT_BACK)       ? 1 : 0, 6)
   store8(bp, (wbtns & XINPUT_START)      ? 1 : 0, 7)
   store8(bp, (wbtns & XINPUT_GUIDE)      ? 1 : 0, 8)
   store8(bp, (wbtns & XINPUT_LSTICK)     ? 1 : 0, 9)
   store8(bp, (wbtns & XINPUT_RSTICK)     ? 1 : 0, 10)
   store8(bp, (wbtns & XINPUT_DPAD_UP)    ? 1 : 0, 11)
   store8(bp, (wbtns & XINPUT_DPAD_RIGHT) ? 1 : 0, 12)
   store8(bp, (wbtns & XINPUT_DPAD_DOWN)  ? 1 : 0, 13)
   store8(bp, (wbtns & XINPUT_DPAD_LEFT)  ? 1 : 0, 14)
}

fn init(): bool {
   if(_initialized){ return _initialized }
   if(!_has_native_support()){ return false }
   gamepad_map.init_default_windows_mappings()
   _initialized = true
   true
}

fn terminate(): bool {
   if(!_initialized){ return true }
   mut jid = 0
   while(jid < XINPUT_MAX_CONTROLLERS){
      if(_get_js(jid)){
         _invoke_callback(jid, backend_api.DISCONNECTED)
         _free_js(jid)
      }
      jid += 1
   }
   _initialized = false
   true
}

fn poll_joysticks(): bool {
   if(!_has_native_support()){ return false }
   def xi_state = malloc(_XI_STATE_SZ)
   if(!xi_state){ return false }
   mut jid = 0
   while(jid < XINPUT_MAX_CONTROLLERS){
      memset(xi_state, 0, _XI_STATE_SZ)
      def res = XInputGetState(int(jid), xi_state)
      def was_connected = !!_get_js(jid)
      if(res == ERROR_SUCCESS){
         if(!was_connected){
            def js = _alloc_js(jid)
            if(js){
               _put_js(jid, js)
               _invoke_callback(jid, backend_api.CONNECTED)
            }
         }
         _update_js_state(jid, xi_state)
      } else {
         if(was_connected){
            _invoke_callback(jid, backend_api.DISCONNECTED)
            _free_js(jid)
         }
      }
      jid += 1
   }
   free(xi_state)
   true
}

fn joystick_present(int: jid): bool {
   if(!_has_native_support() || jid < 0 || jid >= XINPUT_MAX_CONTROLLERS){ return false }
   poll_joysticks()
   def js = _get_js(jid)
   !!js.get("connected", false)
}

fn get_joystick_name(int: jid): str {
   if(!joystick_present(jid)){ return "Unknown" }
   _get_js(jid).get("name", "Unknown")
}

fn get_joystick_guid(int: jid): str {
   if(!joystick_present(jid)){ return "00000000000000000000000000000000" }
   _get_js(jid).get("guid", "00000000000000000000000000000000")
}

fn get_joystick_axes(int: jid, any: count_ptr): any {
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, js.get("axis_count", 0), 0) }
   js.get("axes_ptr", 0)
}

fn get_joystick_buttons(int: jid, any: count_ptr): any {
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, js.get("button_count", 0), 0) }
   js.get("buttons_ptr", 0)
}

fn joystick_is_gamepad(int: jid): bool { joystick_present(jid) }

fn get_gamepad_state(int: jid, any: state_ptr): bool {
   if(!joystick_present(jid)){
      if(state_ptr){ memset(state_ptr, 0, 64) }
      return false
   }
   if(!state_ptr){ return false }
   def js = _get_js(jid)
   def ap = js.get("axes_ptr", 0)
   def bp = js.get("buttons_ptr", 0)
   if(!ap || !bp){ return false }
   memset(state_ptr, 0, 64)
   memcpy(state_ptr, bp, _BUTTON_COUNT)
   memcpy(state_ptr + 16, ap, _AXIS_COUNT * 4)
   true
}

fn get_gamepad_name(int: jid): str {
   if(!joystick_present(jid)){ return "Unknown" }
   get_joystick_name(jid)
}

fn set_joystick_callback(any: cb): any {
   def prev = _joystick_callback
   _joystick_callback = cb
   prev
}

fn update_gamepad_mappings(str: s): int { gamepad_map.update_mappings(s, "Windows") }
