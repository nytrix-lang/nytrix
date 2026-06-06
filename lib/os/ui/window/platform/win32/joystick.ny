;; Keywords: platform window backend win32 windows joystick os ui input
;; Native Win32 XInput/WinMM joystick backend for Nytrix.
;; References:
;; - std.os.ui.window.platform.win32
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform.win32.joystick(init, terminate, poll_joysticks, joystick_present, get_joystick_name, get_joystick_guid, get_joystick_axes, get_joystick_buttons, get_joystick_hats, joystick_is_gamepad, get_gamepad_state, get_gamepad_name, set_joystick_callback, update_gamepad_mappings)
use std.core
use std.core.mem
use std.os.time
use std.os.ffi as ffi
use std.core.str as str
use std.os.ui.window.platform.api as backend_api
use std.os.ui.window.platform.gamepad_map as gamepad_map

def XINPUT_MAX_CONTROLLERS = 4
def ERROR_SUCCESS = 0
def ERROR_DEVICE_NOT_CONNECTED = 1167
def _XI_PACKET    = 0
def _XI_BUTTONS   = 4
def _XI_LT        = 6
def _XI_RT        = 7
def _XI_LX        = 8
def _XI_LY        = 10
def _XI_RX        = 12
def _XI_RY        = 14
def _XI_STATE_SZ  = 16
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
def _MAX_JOYSTICKS = 16
def _WINMM_BASE_JID = 4
def _WINMM_MAX_SLOTS = _MAX_JOYSTICKS - _WINMM_BASE_JID
def _WINMM_AXIS_COUNT = 6
def _WINMM_MAX_BUTTONS = 32
def _WINMM_HAT_COUNT = 1
def _DINPUT_BASE_JID = 4
def _DINPUT_RESCAN_INTERVAL_NS = 250000000
def _DINPUT_VERSION = 0x0800
def _DINPUT_MAX_OBJECTS = 128
def _DINPUT_MAX_OBJECT_FORMATS = 44
def _DINPUT_GUID_SIZE = 16
def _DINPUT_DIOBJECTDATAFORMAT_SIZE = 24
def _DINPUT_DIDATAFORMAT_SIZE = 32
def _DINPUT_DIJOYSTATE_SIZE = 80
def _DINPUT_DIDEVCAPS_SIZE = 44
def _DINPUT_DIPROPDWORD_SIZE = 20
def _DINPUT_DIPROPRANGE_SIZE = 24
def _DINPUT_DIDF_ABSAXIS = 0x00000001
def _DINPUT_DIDFT_AXIS = 0x00000003
def _DINPUT_DIDFT_BUTTON = 0x0000000c
def _DINPUT_DIDFT_POV = 0x00000010
def _DINPUT_DIDFT_OPTIONAL = 0x80000000
def _DINPUT_DIDFT_ANYINSTANCE = 0x00ffff00
def _DINPUT_DIDOI_ASPECTPOSITION = 0x00000100
def _DINPUT_DIPH_DEVICE = 0
def _DINPUT_DIPH_BYID = 2
def _DINPUT_DIPROP_AXISMODE = 2
def _DINPUT_DIPROP_RANGE = 4
def _DINPUT_DIPROPAXISMODE_ABS = 0
def _DINPUT_DI8DEVCLASS_GAMECTRL = 4
def _DINPUT_DIEDFL_ALLDEVICES = 0
def _DINPUT_DIENUM_CONTINUE = 1
def _DINPUT_DIJOFS_X = 0
def _DINPUT_DIJOFS_Y = 4
def _DINPUT_DIJOFS_Z = 8
def _DINPUT_DIJOFS_RX = 12
def _DINPUT_DIJOFS_RY = 16
def _DINPUT_DIJOFS_RZ = 20
def _DINPUT_DIJOFS_SLIDER0 = 24
def _DINPUT_DIJOFS_POV0 = 32
def _DINPUT_DIJOFS_BUTTON0 = 48
def _DINPUT_TYPE_AXIS = 0
def _DINPUT_TYPE_SLIDER = 1
def _DINPUT_TYPE_BUTTON = 2
def _DINPUT_TYPE_POV = 3
def _RAWINPUTDEVICELIST_SIZE = 16
def _RID_DEVICE_INFO_SIZE = 32
def _RID_DEVICE_INFO_HID_VENDOR = 8
def _RID_DEVICE_INFO_HID_PRODUCT = 12
def _RIM_TYPEHID = 2
def _RIDI_DEVICENAME = 0x20000007
def _RIDI_DEVICEINFO = 0x2000000b
def _RAWINPUT_NAME_MAX = 256
def _WINMM_JOYINFOEX_SIZE = 52
def _WINMM_JOYCAPSW_SIZE = 728
def _WINMM_CAPS_NAME = 4
def _WINMM_CAPS_BUTTONS = 92
def _WINMM_CAPS_FLAGS = 128
def _WINMM_JI_FLAGS = 4
def _WINMM_JI_X = 8
def _WINMM_JI_Y = 12
def _WINMM_JI_Z = 16
def _WINMM_JI_R = 20
def _WINMM_JI_U = 24
def _WINMM_JI_V = 28
def _WINMM_JI_BUTTONS = 32
def _WINMM_JI_POV = 40
def JOYERR_NOERROR = 0
def JOY_RETURNALL = 0xff
def JOYCAPS_HASPOV = 0x10
def JOY_POVCENTERED = 0xffff
def HAT_CENTERED = 0
def HAT_UP = 1
def HAT_RIGHT = 2
def HAT_DOWN = 4
def HAT_LEFT = 8
def HAT_RIGHT_UP = 3
def HAT_RIGHT_DOWN = 6
def HAT_LEFT_UP = 9
def HAT_LEFT_DOWN = 12
def JOYSTICK_POLL_COALESCE_NS = 8000000
def WINMM_RESCAN_INTERVAL_NS = 250000000
mut _initialized = false
mut _joysticks = dict(8)
mut _joystick_callback = 0
mut _last_poll_ticks = 0
mut _last_winmm_scan_ticks = 0
mut _last_dinput_scan_ticks = 0
mut _xi_state_buf = 0
mut _winmm_info_buf = 0
mut _dinput_api = 0
mut _dinput_data_format = 0
mut _dinput_object_formats = 0
mut _dinput_guid_dinput8 = 0
mut _dinput_guid_x = 0
mut _dinput_guid_y = 0
mut _dinput_guid_z = 0
mut _dinput_guid_rx = 0
mut _dinput_guid_ry = 0
mut _dinput_guid_rz = 0
mut _dinput_guid_slider = 0
mut _dinput_guid_pov = 0
mut _dinput_enum_device = 0
mut _dinput_enum_objects = []
mut _dinput_enum_axis_count = 0
mut _dinput_enum_slider_count = 0
mut _dinput_enum_button_count = 0
mut _dinput_enum_pov_count = 0

fn _has_native_support() bool {
   #windows { return true }
   false
}

fn _get_js(int jid) any { _joysticks.get(jid, 0) }

fn _put_js(int jid, any js) any {
   _joysticks = _joysticks.set(jid, js)
   js
}

fn _free_js(int jid) bool {
   def js = _get_js(jid)
   if(!js){ return false }
   if(js.get("kind", "") == "dinput"){
      def dev = js.get("device", 0)
      if(dev){
         _dinput_device_unacquire(dev)
         _dinput_release(dev)
      }
   }
   def ap, bp = js.get("axes_ptr", 0), js.get("buttons_ptr", 0)
   def hp, ip = js.get("hats_ptr", 0), js.get("info_ptr", 0)
   def sp, gp = js.get("state_ptr", 0), js.get("guid_instance_ptr", 0)
   if(ap){ free(ap) }
   if(bp){ free(bp) }
   if(hp){ free(hp) }
   if(ip){ free(ip) }
   if(sp){ free(sp) }
   if(gp){ free(gp) }
   _joysticks = _joysticks.set(jid, 0)
   true
}

fn _invoke_callback(int jid, int event) any {
   def cb = _joystick_callback
   if(cb){ cb(jid, event) }
}

fn _ensure_windows_mappings() bool {
   gamepad_map.init_default_windows_mappings()
   true
}

fn _ensure_init() bool {
   if(_initialized){ return _ensure_windows_mappings() }
   if(!_has_native_support()){ return false }
   _initialized = true
   _ensure_windows_mappings()
}

fn _scratch_xi_state() any {
   if(!_xi_state_buf){ _xi_state_buf = malloc(_XI_STATE_SZ) }
   _xi_state_buf
}

fn _scratch_winmm_info() any {
   if(!_winmm_info_buf){ _winmm_info_buf = malloc(_WINMM_JOYINFOEX_SIZE) }
   _winmm_info_buf
}

fn _dinput_guid(u32 a, u32 b, u32 c, u32 d0, u32 d1, u32 d2, u32 d3, u32 d4, u32 d5, u32 d6, u32 d7) ptr {
   def g = zalloc(_DINPUT_GUID_SIZE)
   if(!g){ return 0 }
   store32(g, a, 0)
   store16(g, b, 4)
   store16(g, c, 6)
   store8(g, d0, 8)
   store8(g, d1, 9)
   store8(g, d2, 10)
   store8(g, d3, 11)
   store8(g, d4, 12)
   store8(g, d5, 13)
   store8(g, d6, 14)
   store8(g, d7, 15)
   g
}

fn _dinput_guid_init() bool {
   if(_dinput_guid_dinput8){ return true }
   _dinput_guid_dinput8 = _dinput_guid(0xbf798031, 0x483a, 0x4da2, 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00)
   _dinput_guid_x = _dinput_guid(0xa36d02e0, 0xc9f3, 0x11cf, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00)
   _dinput_guid_y = _dinput_guid(0xa36d02e1, 0xc9f3, 0x11cf, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00)
   _dinput_guid_z = _dinput_guid(0xa36d02e2, 0xc9f3, 0x11cf, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00)
   _dinput_guid_rx = _dinput_guid(0xa36d02f4, 0xc9f3, 0x11cf, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00)
   _dinput_guid_ry = _dinput_guid(0xa36d02f5, 0xc9f3, 0x11cf, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00)
   _dinput_guid_rz = _dinput_guid(0xa36d02e3, 0xc9f3, 0x11cf, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00)
   _dinput_guid_slider = _dinput_guid(0xa36d02e4, 0xc9f3, 0x11cf, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00)
   _dinput_guid_pov = _dinput_guid(0xa36d02f2, 0xc9f3, 0x11cf, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00)
   _dinput_guid_dinput8 && _dinput_guid_x && _dinput_guid_y && _dinput_guid_z &&
   _dinput_guid_rx && _dinput_guid_ry && _dinput_guid_rz &&
   _dinput_guid_slider && _dinput_guid_pov
}

fn _dinput_store_objfmt(int idx, any guid, int ofs, int typ, int flags) any {
   def p = _dinput_object_formats + idx * _DINPUT_DIOBJECTDATAFORMAT_SIZE
   store64_h(p, guid, 0)
   store32(p, ofs, 8)
   store32(p, typ, 12)
   store32(p, flags, 16)
}

fn _dinput_axis_format_type() int {
   _DINPUT_DIDFT_AXIS | _DINPUT_DIDFT_OPTIONAL | _DINPUT_DIDFT_ANYINSTANCE
}

fn _dinput_button_format_type() int {
   _DINPUT_DIDFT_BUTTON | _DINPUT_DIDFT_OPTIONAL | _DINPUT_DIDFT_ANYINSTANCE
}

fn _dinput_pov_format_type() int {
   _DINPUT_DIDFT_POV | _DINPUT_DIDFT_OPTIONAL | _DINPUT_DIDFT_ANYINSTANCE
}

fn _dinput_build_data_format() bool {
   if(_dinput_data_format){ return true }
   if(!_dinput_guid_init()){ return false }
   _dinput_object_formats = zalloc(_DINPUT_DIOBJECTDATAFORMAT_SIZE * _DINPUT_MAX_OBJECT_FORMATS)
   _dinput_data_format = zalloc(_DINPUT_DIDATAFORMAT_SIZE)
   if(!_dinput_object_formats || !_dinput_data_format){ return false }
   def axis_type = _dinput_axis_format_type()
   _dinput_store_objfmt(0, _dinput_guid_x, _DINPUT_DIJOFS_X, axis_type, _DINPUT_DIDOI_ASPECTPOSITION)
   _dinput_store_objfmt(1, _dinput_guid_y, _DINPUT_DIJOFS_Y, axis_type, _DINPUT_DIDOI_ASPECTPOSITION)
   _dinput_store_objfmt(2, _dinput_guid_z, _DINPUT_DIJOFS_Z, axis_type, _DINPUT_DIDOI_ASPECTPOSITION)
   _dinput_store_objfmt(3, _dinput_guid_rx, _DINPUT_DIJOFS_RX, axis_type, _DINPUT_DIDOI_ASPECTPOSITION)
   _dinput_store_objfmt(4, _dinput_guid_ry, _DINPUT_DIJOFS_RY, axis_type, _DINPUT_DIDOI_ASPECTPOSITION)
   _dinput_store_objfmt(5, _dinput_guid_rz, _DINPUT_DIJOFS_RZ, axis_type, _DINPUT_DIDOI_ASPECTPOSITION)
   _dinput_store_objfmt(6, _dinput_guid_slider, _DINPUT_DIJOFS_SLIDER0, axis_type, _DINPUT_DIDOI_ASPECTPOSITION)
   _dinput_store_objfmt(7, _dinput_guid_slider, _DINPUT_DIJOFS_SLIDER0 + 4, axis_type, _DINPUT_DIDOI_ASPECTPOSITION)
   def pov_type = _dinput_pov_format_type()
   mut i = 0
   while(i < 4){
      _dinput_store_objfmt(8 + i, _dinput_guid_pov, _DINPUT_DIJOFS_POV0 + i * 4, pov_type, 0)
      i += 1
   }
   def button_type = _dinput_button_format_type()
   i = 0
   while(i < 32){
      _dinput_store_objfmt(12 + i, 0, _DINPUT_DIJOFS_BUTTON0 + i, button_type, 0)
      i += 1
   }
   store32(_dinput_data_format, _DINPUT_DIDATAFORMAT_SIZE, 0)
   store32(_dinput_data_format, _DINPUT_DIOBJECTDATAFORMAT_SIZE, 4)
   store32(_dinput_data_format, _DINPUT_DIDF_ABSAXIS, 8)
   store32(_dinput_data_format, _DINPUT_DIJOYSTATE_SIZE, 12)
   store32(_dinput_data_format, _DINPUT_MAX_OBJECT_FORMATS, 16)
   store64_h(_dinput_data_format, _dinput_object_formats, 24)
   true
}

fn _dinput_method(any obj, int index) any {
   if(!obj){ return 0 }
   def vt = load64(obj, 0)
   if(!vt){ return 0 }
   ffi.tag_native(load64(vt, index * 8))
}

fn _dinput_release(any obj) int {
   if(!obj){ return 0 }
   int(ffi.call1(_dinput_method(obj, 2), obj))
}

fn _dinput_device_unacquire(any dev) int {
   if(!dev){ return 0 }
   int(ffi.call1(_dinput_method(dev, 8), dev))
}

fn _dinput_device_acquire(any dev) int {
   if(!dev){ return 1 }
   int(ffi.call1(_dinput_method(dev, 7), dev))
}

fn _dinput_init() bool {
   if(_dinput_api){ return true }
   if(!_has_native_support() || !_dinput_build_data_format()){ return false }
   def out = zalloc(8)
   if(!out){ return false }
   def hr = DirectInput8Create(_ny_GetModuleHandleA(0), _DINPUT_VERSION, _dinput_guid_dinput8, out, 0)
   def api = load64(out, 0)
   free(out)
   if(hr != 0 || !api){ return false }
   _dinput_api = api
   true
}

fn _dinput_shutdown() any {
   if(_dinput_api){
      _dinput_release(_dinput_api)
      _dinput_api = 0
   }
   if(_dinput_data_format){ free(_dinput_data_format) }
   if(_dinput_object_formats){ free(_dinput_object_formats) }
   if(_dinput_guid_dinput8){ free(_dinput_guid_dinput8) }
   if(_dinput_guid_x){ free(_dinput_guid_x) }
   if(_dinput_guid_y){ free(_dinput_guid_y) }
   if(_dinput_guid_z){ free(_dinput_guid_z) }
   if(_dinput_guid_rx){ free(_dinput_guid_rx) }
   if(_dinput_guid_ry){ free(_dinput_guid_ry) }
   if(_dinput_guid_rz){ free(_dinput_guid_rz) }
   if(_dinput_guid_slider){ free(_dinput_guid_slider) }
   if(_dinput_guid_pov){ free(_dinput_guid_pov) }
   _dinput_data_format = 0
   _dinput_object_formats = 0
   _dinput_guid_dinput8 = 0
   _dinput_guid_x = 0
   _dinput_guid_y = 0
   _dinput_guid_z = 0
   _dinput_guid_rx = 0
   _dinput_guid_ry = 0
   _dinput_guid_rz = 0
   _dinput_guid_slider = 0
   _dinput_guid_pov = 0
   _dinput_enum_device = 0
   _dinput_enum_objects = []
}

#windows {
   #link "xinput9_1_0"
   #link "winmm"
   #link "dinput8"
   #include <windows.h>
   #include <xinput.h>
   #include <mmsystem.h>
   extern "" {
      fn _ny_GetModuleHandleA(ptr _name) ptr as "GetModuleHandleA"
      fn XInputGetState(u32 _idx, ptr _state) u32
      fn joyGetNumDevs() u32
      fn joyGetPosEx(u32 _id, ptr _info) u32
      fn joyGetDevCapsW(u32 _id, ptr _caps, u32 _size) u32
      fn DirectInput8Create(ptr _inst, u32 _version, ptr _iid, ptr _out, ptr _outer) i32
      fn GetRawInputDeviceList(ptr _devices, ptr _count, u32 _size) u32
      fn GetRawInputDeviceInfoA(ptr _device, u32 _cmd, ptr _data, ptr _size) u32
   }
} #else {
   "Runs the GetRawInputDeviceInfoA operation."
   fn _ny_GetModuleHandleA(ptr _name) ptr { 0 }
   fn XInputGetState(any _idx, any _state) int {
      "Runs the XInputGetState operation."
      ERROR_DEVICE_NOT_CONNECTED
   }
   fn joyGetNumDevs() u32 {
      "Runs the joyGetNumDevs operation."
      0
   }
   fn joyGetPosEx(u32 _id, ptr _info) u32 {
      "Runs the joyGetPosEx operation."
      1
   }
   fn joyGetDevCapsW(u32 _id, ptr _caps, u32 _size) u32 {
      "Runs the joyGetDevCapsW operation."
      1
   }
   fn DirectInput8Create(ptr _inst, u32 _version, ptr _iid, ptr _out, ptr _outer) i32 {
      "Runs the DirectInput8Create operation."
      1
   }
   fn GetRawInputDeviceList(ptr _devices, ptr _count, u32 _size) u32 {
      "Runs the GetRawInputDeviceList operation."
      0xffffffff
   }
   fn GetRawInputDeviceInfoA(ptr _device, u32 _cmd, ptr _data, ptr _size) u32 {
      "Runs the GetRawInputDeviceInfoA operation."
      0xffffffff
   }
}

fn _signed16(any v) int {
   def x = int(v) & 0xffff
   if(x >= 0x8000){ x - 0x10000 } else { x }
}

fn _signed32(any v) int {
   def x = int(v) & 0xffffffff
   if(x >= 0x80000000){ x - 0x100000000 } else { x }
}

fn _hresult_failed(any hr) bool {
   (int(hr) & 0x80000000) != 0
}

fn _normalize_thumb(any v) f64 {
   def fv = float(v)
   if(fv < 0.0){ return fv / 32768.0 }
   fv / 32767.0
}

fn _normalize_trigger(any v) f64 {
   float(v) / 127.5 - 1.0
}

fn _normalize_winmm_axis(any v) f64 {
   def fv = float(int(v) & 0xffffffff)
   mut out = fv / 32767.5 - 1.0
   if(out < -1.0){ out = -1.0 }
   if(out > 1.0){ out = 1.0 }
   out
}

fn _normalize_dinput_axis(any v) f64 {
   mut out = (float(_signed32(v)) + 0.5) / 32767.5
   if(out < -1.0){ out = -1.0 }
   if(out > 1.0){ out = 1.0 }
   out
}

fn _normalize_dinput_axis_unsigned(any v) f64 {
   mut raw = int(v) & 0xffffffff
   if(raw > 65535){ raw = 65535 }
   mut out = float(raw) / 32767.5 - 1.0
   if(out < -1.0){ out = -1.0 }
   if(out > 1.0){ out = 1.0 }
   out
}

fn _normalize_dinput_axis_for_js(any js, any v) f64 {
   if(js && js.get("dinput_unsigned_axes", false)){
      return _normalize_dinput_axis_unsigned(v)
   }
   _normalize_dinput_axis(v)
}

fn _hex_nibble(any n) str {
   def x = int(n) & 0xf
   if(x < 10){ return chr(48 + x) }
   chr(97 + x - 10)
}

fn _hex_byte(any v) str {
   def x = int(v) & 0xff
   _hex_nibble(x >> 4) + _hex_nibble(x)
}

fn _hex_word_le(any v) str {
   _hex_byte(v) + _hex_byte(int(v) >> 8)
}

fn _utf16_name(any p, int off, int max_chars) str {
   mut out = ""
   mut i = 0
   while(i < max_chars){
      def c = load16(p, off + i * 2)
      if(c == 0){ break }
      if(c >= 32 && c < 127){ out += chr(c) }
      elif(c < 256){ out += chr(c) }
      else { out += "?" }
      i += 1
   }
   out
}

fn _fallback_guid_for_name(any name) str {
   def lname = str.lower(to_str(name))
   if(str.find(lname, "dualsense") != -1 || str.find(lname, "ps5") != -1){
      return "030000004c050000e60c000000000000"
   }
   if(str.find(lname, "dualshock") != -1 || str.find(lname, "ps4") != -1){
      return "030000004c050000cc09000000000000"
   }
   ""
}

fn _build_winmm_guid(any name, any mid, any pid) str {
   def fallback = _fallback_guid_for_name(name)
   if(fallback != ""){ return fallback }
   "03000000" + _hex_word_le(mid) + "0000" + _hex_word_le(pid) + "000000000000"
}

fn _dinput_product_has_pidvid(any product) bool {
   product &&
   load8(product, 10) == 80 && load8(product, 11) == 73 &&
   load8(product, 12) == 68 && load8(product, 13) == 86 &&
   load8(product, 14) == 73 && load8(product, 15) == 68
}

fn _build_dinput_guid(any product, any name) str {
   if(_dinput_product_has_pidvid(product)){
      def data1 = load32(product, 0)
      return "03000000" +
      _hex_byte(data1) + _hex_byte(data1 >> 8) +
      "0000" +
      _hex_byte(data1 >> 16) + _hex_byte(data1 >> 24) +
      "000000000000"
   }
   mut out = "05000000"
   def s = to_str(name)
   mut i = 0
   while(i < 11){
      out += _hex_byte(i < s.len ? load8(s, i) : 0)
      i += 1
   }
   out + "00"
}

fn _ascii_cstr(any p, int max_len) str {
   if(!p || max_len <= 0){ return "" }
   mut out = ""
   mut i = 0
   while(i < max_len){
      def c = load8(p, i)
      if(c == 0){ break }
      if(c >= 32 && c < 127){ out += chr(c) }
      elif(c < 256){ out += chr(c) }
      else { out += "?" }
      i += 1
   }
   out
}

fn _rawinput_name_contains_ig(any handle) bool {
   def size_ptr = zalloc(4)
   def name = zalloc(_RAWINPUT_NAME_MAX)
   if(!size_ptr || !name){
      if(size_ptr){ free(size_ptr) }
      if(name){ free(name) }
      return false
   }
   store32(size_ptr, _RAWINPUT_NAME_MAX, 0)
   def rc = int(GetRawInputDeviceInfoA(handle, _RIDI_DEVICENAME, name, size_ptr))
   def ok = rc != 0xffffffff && str.find(_ascii_cstr(name, _RAWINPUT_NAME_MAX), "IG_") != -1
   free(size_ptr)
   free(name)
   ok
}

fn _dinput_supports_xinput(any product) bool {
   if(!product){ return false }
   def product_code = load32(product, 0) & 0xffffffff
   def count_ptr = zalloc(4)
   if(!count_ptr){ return false }
   store32(count_ptr, 0, 0)
   if(GetRawInputDeviceList(0, count_ptr, _RAWINPUTDEVICELIST_SIZE) != 0){
      free(count_ptr)
      return false
   }
   mut count = int(load32(count_ptr, 0))
   if(count <= 0){
      free(count_ptr)
      return false
   }
   def list = zalloc(count * _RAWINPUTDEVICELIST_SIZE)
   if(!list){
      free(count_ptr)
      return false
   }
   def rc = int(GetRawInputDeviceList(list, count_ptr, _RAWINPUTDEVICELIST_SIZE))
   count = int(load32(count_ptr, 0))
   mut result = false
   if(rc != 0xffffffff){
      mut i = 0
      while(i < count && !result){
         def item = list + i * _RAWINPUTDEVICELIST_SIZE
         if(load32(item, 8) == _RIM_TYPEHID){
            def info = zalloc(_RID_DEVICE_INFO_SIZE)
            if(info){
               store32(info, _RID_DEVICE_INFO_SIZE, 0)
               store32(count_ptr, _RID_DEVICE_INFO_SIZE, 0)
               def info_rc = int(GetRawInputDeviceInfoA(load64(item, 0), _RIDI_DEVICEINFO, info, count_ptr))
               if(info_rc != 0xffffffff){
                  def vendor = load32(info, _RID_DEVICE_INFO_HID_VENDOR) & 0xffff
                  def device = load32(info, _RID_DEVICE_INFO_HID_PRODUCT) & 0xffff
                  if(((device << 16) | vendor) == product_code &&
                     _rawinput_name_contains_ig(load64(item, 0))){
                     result = true
                  }
               }
               free(info)
            }
         }
         i += 1
      }
   }
   free(list)
   free(count_ptr)
   result
}

fn _dinput_guid_instance_seen(any guid) bool {
   if(!guid){ return false }
   mut jid = _DINPUT_BASE_JID
   while(jid < _MAX_JOYSTICKS){
      def js = _get_js(jid)
      if(js && js.get("kind", "") == "dinput"){
         def gp = js.get("guid_instance_ptr", 0)
         if(gp && memcmp(gp, guid, _DINPUT_GUID_SIZE) == 0){ return true }
      }
      jid += 1
   }
   false
}

fn _dinput_connected_count() int {
   mut n = 0
   mut jid = _DINPUT_BASE_JID
   while(jid < _MAX_JOYSTICKS){
      def js = _get_js(jid)
      if(js && js.get("kind", "") == "dinput"){ n += 1 }
      jid += 1
   }
   n
}

fn _first_free_dinput_jid() int {
   mut jid = _DINPUT_BASE_JID
   while(jid < _MAX_JOYSTICKS){
      if(!_get_js(jid)){ return jid }
      jid += 1
   }
   -1
}

fn _dinput_object_less(any a, any b) bool {
   if(!a || !b){ return false }
   def at = int(a.get("type", 0))
   def bt = int(b.get("type", 0))
   if(at != bt){ return at < bt }
   int(a.get("offset", 0)) < int(b.get("offset", 0))
}

fn _dinput_sort_objects(list objs) list {
   mut out = objs
   mut i = 0
   while(i < out.len){
      mut j = i + 1
      while(j < out.len){
         if(_dinput_object_less(out[j], out[i])){
            out = swap(out, i, j)
         }
         j += 1
      }
      i += 1
   }
   out
}

fn _dinput_axis_offset(any guid) int {
   def data1 = load32(guid, 0) & 0xffffffff
   if(data1 == 0xa36d02e4){
      return _DINPUT_DIJOFS_SLIDER0 + _dinput_enum_slider_count * 4
   }
   if(data1 == 0xa36d02e0){ return _DINPUT_DIJOFS_X }
   if(data1 == 0xa36d02e1){ return _DINPUT_DIJOFS_Y }
   if(data1 == 0xa36d02e2){ return _DINPUT_DIJOFS_Z }
   if(data1 == 0xa36d02f4){ return _DINPUT_DIJOFS_RX }
   if(data1 == 0xa36d02f5){ return _DINPUT_DIJOFS_RY }
   if(data1 == 0xa36d02e3){ return _DINPUT_DIJOFS_RZ }
   -1
}

fn _dinput_set_axis_range(any dev, int obj_type) bool {
   def dipr = zalloc(_DINPUT_DIPROPRANGE_SIZE)
   if(!dipr){ return false }
   store32(dipr, _DINPUT_DIPROPRANGE_SIZE, 0)
   store32(dipr, 16, 4)
   store32(dipr, obj_type, 8)
   store32(dipr, _DINPUT_DIPH_BYID, 12)
   store32(dipr, -32768, 16)
   store32(dipr, 32767, 20)
   def hr = int(ffi.call3(_dinput_method(dev, 6), dev,
   ffi.tag_native(_DINPUT_DIPROP_RANGE), dipr))
   free(dipr)
   !_hresult_failed(hr)
}

fn _dinput_set_axis_mode_abs(any dev) bool {
   def dipd = zalloc(_DINPUT_DIPROPDWORD_SIZE)
   if(!dipd){ return false }
   store32(dipd, _DINPUT_DIPROPDWORD_SIZE, 0)
   store32(dipd, 16, 4)
   store32(dipd, 0, 8)
   store32(dipd, _DINPUT_DIPH_DEVICE, 12)
   store32(dipd, _DINPUT_DIPROPAXISMODE_ABS, 16)
   def hr = int(ffi.call3(_dinput_method(dev, 6), dev,
   ffi.tag_native(_DINPUT_DIPROP_AXISMODE), dipd))
   free(dipd)
   !_hresult_failed(hr)
}

fn _dinput_append_object(int typ, int offset) any {
   if(_dinput_enum_objects.len >= _DINPUT_MAX_OBJECTS){ return nil }
   _dinput_enum_objects = _dinput_enum_objects.append({"type": typ, "offset": offset})
}

fn _dinput_add_default_axes(int axis_count) any {
   def offsets = [
      _DINPUT_DIJOFS_X,
      _DINPUT_DIJOFS_Y,
      _DINPUT_DIJOFS_Z,
      _DINPUT_DIJOFS_RX,
      _DINPUT_DIJOFS_RY,
      _DINPUT_DIJOFS_RZ
   ]
   mut i = 0
   while(i < axis_count && i < offsets.len){
      _dinput_append_object(_DINPUT_TYPE_AXIS, offsets[i])
      _dinput_enum_axis_count += 1
      i += 1
   }
}

fn _dinput_object_cb(ptr doi, ptr user) i32 {
   if(!doi || !_dinput_enum_device){ return _DINPUT_DIENUM_CONTINUE }
   def typ = load32(doi, 24) & 0xff
   def raw_type = load32(doi, 24)
   if((typ & _DINPUT_DIDFT_AXIS) != 0){
      def guid = doi + 4
      def offset = _dinput_axis_offset(guid)
      if(offset < 0){ return _DINPUT_DIENUM_CONTINUE }
      if(!_dinput_set_axis_range(_dinput_enum_device, raw_type)){ return _DINPUT_DIENUM_CONTINUE }
      mut obj_type = _DINPUT_TYPE_AXIS
      if(offset == _DINPUT_DIJOFS_SLIDER0 || offset == _DINPUT_DIJOFS_SLIDER0 + 4){
         obj_type = _DINPUT_TYPE_SLIDER
      }
      if(obj_type == _DINPUT_TYPE_AXIS){ _dinput_enum_axis_count += 1 }
      else { _dinput_enum_slider_count += 1 }
      _dinput_append_object(obj_type, offset)
   } elif((typ & _DINPUT_DIDFT_BUTTON) != 0){
      _dinput_append_object(_DINPUT_TYPE_BUTTON, _DINPUT_DIJOFS_BUTTON0 + _dinput_enum_button_count)
      _dinput_enum_button_count += 1
   } elif((typ & _DINPUT_DIDFT_POV) != 0){
      _dinput_append_object(_DINPUT_TYPE_POV, _DINPUT_DIJOFS_POV0 + _dinput_enum_pov_count * 4)
      _dinput_enum_pov_count += 1
   }
   _DINPUT_DIENUM_CONTINUE
}

fn _alloc_dinput_js(int jid, any dev, any di, any objects) any {
   def axis_count = _dinput_enum_axis_count + _dinput_enum_slider_count
   def button_count = _dinput_enum_button_count
   def hat_count = _dinput_enum_pov_count
   if(axis_count <= 0 && button_count <= 0 && hat_count <= 0){ return 0 }
   def axis_bytes = axis_count > 0 ? axis_count * 4 : 4
   def button_bytes = button_count > 0 ? button_count : 1
   def hat_bytes = hat_count > 0 ? hat_count : 1
   def axes_ptr = malloc(axis_bytes)
   def buttons_ptr = malloc(button_bytes)
   def hats_ptr = malloc(hat_bytes)
   def state_ptr = zalloc(_DINPUT_DIJOYSTATE_SIZE)
   def guid_ptr = malloc(_DINPUT_GUID_SIZE)
   if(!axes_ptr || !buttons_ptr || !hats_ptr || !state_ptr || !guid_ptr){
      if(axes_ptr){ free(axes_ptr) }
      if(buttons_ptr){ free(buttons_ptr) }
      if(hats_ptr){ free(hats_ptr) }
      if(state_ptr){ free(state_ptr) }
      if(guid_ptr){ free(guid_ptr) }
      return 0
   }
   memset(axes_ptr, 0, axis_bytes)
   memset(buttons_ptr, 0, button_bytes)
   memset(hats_ptr, 0, hat_bytes)
   memcpy(guid_ptr, di + 4, _DINPUT_GUID_SIZE)
   def name_raw = _utf16_name(di, 40, 260)
   def name = name_raw.len > 0 ? name_raw : "DirectInput Controller"
   {
      "connected": true,
      "jid": jid,
      "name": name,
      "guid": _build_dinput_guid(di + 20, name),
      "axes_ptr": axes_ptr,
      "buttons_ptr": buttons_ptr,
      "hats_ptr": hats_ptr,
      "state_ptr": state_ptr,
      "guid_instance_ptr": guid_ptr,
      "axis_count": axis_count,
      "button_count": button_count,
      "hat_count": hat_count,
      "kind": "dinput",
      "platform": "Windows",
      "device": dev,
      "objects": objects,
      "object_count": objects.len
   }
}

fn _dinput_device_cb(ptr di, ptr user) i32 {
   if(!di || !_dinput_api){ return _DINPUT_DIENUM_CONTINUE }
   if(_dinput_guid_instance_seen(di + 4)){ return _DINPUT_DIENUM_CONTINUE }
   if(_dinput_supports_xinput(di + 20)){ return _DINPUT_DIENUM_CONTINUE }
   def jid = _first_free_dinput_jid()
   if(jid < 0){ return _DINPUT_DIENUM_CONTINUE }
   def out = zalloc(8)
   if(!out){ return _DINPUT_DIENUM_CONTINUE }
   def hr = int(ffi.call4(_dinput_method(_dinput_api, 3), _dinput_api, di + 4, out, 0))
   def dev = load64(out, 0)
   free(out)
   if(_hresult_failed(hr) || !dev){ return _DINPUT_DIENUM_CONTINUE }
   if(_hresult_failed(ffi.call2(_dinput_method(dev, 11), dev, _dinput_data_format))){
      _dinput_release(dev)
      return _DINPUT_DIENUM_CONTINUE
   }
   def caps = zalloc(_DINPUT_DIDEVCAPS_SIZE)
   if(!caps){
      _dinput_release(dev)
      return _DINPUT_DIENUM_CONTINUE
   }
   store32(caps, _DINPUT_DIDEVCAPS_SIZE, 0)
   if(_hresult_failed(ffi.call2(_dinput_method(dev, 3), dev, caps))){
      free(caps)
      _dinput_release(dev)
      return _DINPUT_DIENUM_CONTINUE
   }
   if(!_dinput_set_axis_mode_abs(dev)){
      free(caps)
      _dinput_release(dev)
      return _DINPUT_DIENUM_CONTINUE
   }
   _dinput_enum_device = dev
   _dinput_enum_objects = []
   _dinput_enum_axis_count = 0
   _dinput_enum_slider_count = 0
   _dinput_enum_button_count = 0
   _dinput_enum_pov_count = 0
   def flags = _DINPUT_DIDFT_AXIS | _DINPUT_DIDFT_BUTTON | _DINPUT_DIDFT_POV
   def enum_hr = int(ffi.call4(_dinput_method(dev, 4), dev, ffi.tag_native(_dinput_object_cb), 0, flags))
   def caps_axes = int(load32(caps, 12))
   _dinput_enum_device = 0
   free(caps)
   if(_hresult_failed(enum_hr)){
      _dinput_release(dev)
      return _DINPUT_DIENUM_CONTINUE
   }
   if(_dinput_enum_axis_count + _dinput_enum_slider_count == 0){
      _dinput_add_default_axes(caps_axes)
   }
   def objects = _dinput_sort_objects(_dinput_enum_objects)
   def js = _alloc_dinput_js(jid, dev, di, objects)
   if(!js){
      _dinput_release(dev)
      return _DINPUT_DIENUM_CONTINUE
   }
   _put_js(jid, js)
   _invoke_callback(jid, backend_api.CONNECTED)
   _DINPUT_DIENUM_CONTINUE
}

fn _dinput_scan() bool {
   if(!_dinput_init()){ return false }
   def hr = int(ffi.call5(_dinput_method(_dinput_api, 4), _dinput_api,
         _DINPUT_DI8DEVCLASS_GAMECTRL, ffi.tag_native(_dinput_device_cb), 0,
   _DINPUT_DIEDFL_ALLDEVICES))
   !_hresult_failed(hr)
}

fn _dinput_pov_to_hat(any v) int {
   def p = int(v) & 0xffff
   if(p > 36000){ return HAT_CENTERED }
   def idx = p / 4500
   if(idx == 0){ return HAT_UP }
   if(idx == 1){ return HAT_RIGHT_UP }
   if(idx == 2){ return HAT_RIGHT }
   if(idx == 3){ return HAT_RIGHT_DOWN }
   if(idx == 4){ return HAT_DOWN }
   if(idx == 5){ return HAT_LEFT_DOWN }
   if(idx == 6){ return HAT_LEFT }
   if(idx == 7){ return HAT_LEFT_UP }
   HAT_CENTERED
}

fn _dinput_device_state(any dev, any state) int {
   ffi.call1(_dinput_method(dev, 25), dev)
   int(ffi.call3(_dinput_method(dev, 9), dev, _DINPUT_DIJOYSTATE_SIZE, state))
}

fn _dinput_detect_unsigned_axes(any js, any state, any objects) any {
   if(!js || !state || !objects || js.get("dinput_axis_mode_known", false)){ return nil }
   mut axes = 0
   mut negative = 0
   mut centerish = 0
   mut i = 0
   while(i < objects.len){
      def obj = objects[i]
      def typ = int(obj.get("type", 0))
      if(typ == _DINPUT_TYPE_AXIS || typ == _DINPUT_TYPE_SLIDER){
         def raw = int(load32(state, int(obj.get("offset", 0)))) & 0xffffffff
         axes += 1
         if(raw > 65535 && _signed32(raw) < -1024){ negative += 1 }
         if(raw >= 20000 && raw <= 45000){ centerish += 1 }
      }
      i += 1
   }
   if(axes <= 0){ return nil }
   if(negative > 0){
      js["dinput_axis_mode_known"] = true
      js["dinput_unsigned_axes"] = false
   } elif(centerish >= 2){
      js["dinput_axis_mode_known"] = true
      js["dinput_unsigned_axes"] = true
   }
}

fn _update_dinput_js_state(int jid) any {
   def js = _get_js(jid)
   if(!js || js.get("kind", "") != "dinput"){ return nil }
   def dev = js.get("device", 0)
   def state = js.get("state_ptr", 0)
   if(!dev || !state){ return nil }
   memset(state, 0, _DINPUT_DIJOYSTATE_SIZE)
   mut hr = _dinput_device_state(dev, state)
   if(_hresult_failed(hr)){
      _dinput_device_acquire(dev)
      hr = _dinput_device_state(dev, state)
   }
   if(_hresult_failed(hr)){
      _invoke_callback(jid, backend_api.DISCONNECTED)
      _free_js(jid)
      return nil
   }
   def objects = js.get("objects", [])
   _dinput_detect_unsigned_axes(js, state, objects)
   def ap = js.get("axes_ptr", 0)
   def bp = js.get("buttons_ptr", 0)
   def hp = js.get("hats_ptr", 0)
   mut ai = 0
   mut bi = 0
   mut pi = 0
   mut i = 0
   while(i < objects.len){
      def obj = objects[i]
      def typ = int(obj.get("type", 0))
      def offset = int(obj.get("offset", 0))
      if(typ == _DINPUT_TYPE_AXIS || typ == _DINPUT_TYPE_SLIDER){
         if(ap && ai < int(js.get("axis_count", 0))){
            store32_f32(ap, _normalize_dinput_axis_for_js(js, load32(state, offset)), ai * 4)
         }
         ai += 1
      } elif(typ == _DINPUT_TYPE_BUTTON){
         if(bp && bi < int(js.get("button_count", 0))){
            store8(bp, (load8(state, offset) & 0x80) != 0 ? 1 : 0, bi)
         }
         bi += 1
      } elif(typ == _DINPUT_TYPE_POV){
         def hat = _dinput_pov_to_hat(load32(state, offset))
         if(hp && pi < int(js.get("hat_count", 0))){
            store8(hp, hat, pi)
         }
         pi += 1
      }
      i += 1
   }
}

fn _pov_to_hat(any pov) int {
   def p = int(pov) & 0xffffffff
   if(p == JOY_POVCENTERED || p < 0){ return HAT_CENTERED }
   def dir = ((p + 2250) / 4500) % 8
   if(dir == 0){ return HAT_UP }
   if(dir == 1){ return HAT_RIGHT_UP }
   if(dir == 2){ return HAT_RIGHT }
   if(dir == 3){ return HAT_RIGHT_DOWN }
   if(dir == 4){ return HAT_DOWN }
   if(dir == 5){ return HAT_LEFT_DOWN }
   if(dir == 6){ return HAT_LEFT }
   HAT_LEFT_UP
}

fn _read_winmm_state(int native_jid, any info) bool {
   if(!info){ return false }
   memset(info, 0, _WINMM_JOYINFOEX_SIZE)
   store32(info, _WINMM_JOYINFOEX_SIZE, 0)
   store32(info, JOY_RETURNALL, _WINMM_JI_FLAGS)
   joyGetPosEx(native_jid, info) == JOYERR_NOERROR
}

fn _winmm_stable_axis(any js, any ap, int idx, f64 value) f64 {
   if(idx < 4){ return value }
   def samples = int(js.get("winmm_samples", 0))
   if(samples < 20 && value > -0.01 && value < 0.01 &&
      _raw_axis(ap, _WINMM_AXIS_COUNT, idx) <= -0.90){
      return -1.0
   }
   value
}

fn _alloc_js(int jid) any {
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
      "button_count": _BUTTON_COUNT,
      "hat_count": 0,
      "kind": "xinput",
      "platform": "Windows"
   }
}

fn _fill_winmm_buffers_from_info(any js, any info) any {
   if(!js || !info){ return nil }
   def ap = js.get("axes_ptr", 0)
   def bp = js.get("buttons_ptr", 0)
   def hp = js.get("hats_ptr", 0)
   if(!ap || !bp){ return nil }
   store32_f32(ap, _normalize_winmm_axis(load32(info, _WINMM_JI_X)), 0)
   store32_f32(ap, _normalize_winmm_axis(load32(info, _WINMM_JI_Y)), 4)
   store32_f32(ap, _normalize_winmm_axis(load32(info, _WINMM_JI_Z)), 8)
   store32_f32(ap, _normalize_winmm_axis(load32(info, _WINMM_JI_R)), 12)
   store32_f32(ap, _winmm_stable_axis(js, ap, 4, _normalize_winmm_axis(load32(info, _WINMM_JI_U))), 16)
   store32_f32(ap, _winmm_stable_axis(js, ap, 5, _normalize_winmm_axis(load32(info, _WINMM_JI_V))), 20)
   js["winmm_samples"] = int(js.get("winmm_samples", 0)) + 1
   def wbtns = load32(info, _WINMM_JI_BUTTONS)
   mut i = 0
   def button_count = js.get("button_count", 0)
   while(i < _WINMM_MAX_BUTTONS){
      store8(bp, (i < button_count && band(wbtns, 1 << i)) ? 1 : 0, i)
      i += 1
   }
   if(hp && js.get("hat_count", 0) > 0){
      store8(hp, _pov_to_hat(load32(info, _WINMM_JI_POV)), 0)
   }
}

fn _alloc_winmm_js(int jid, int native_jid, any info) any {
   def caps = malloc(_WINMM_JOYCAPSW_SIZE)
   def axes_ptr = malloc(_WINMM_AXIS_COUNT * 4)
   def buttons_ptr = malloc(_WINMM_MAX_BUTTONS)
   def hats_ptr = malloc(_WINMM_HAT_COUNT)
   def info_ptr = malloc(_WINMM_JOYINFOEX_SIZE)
   if(!caps || !axes_ptr || !buttons_ptr || !hats_ptr || !info_ptr){
      if(caps){ free(caps) }
      if(axes_ptr){ free(axes_ptr) }
      if(buttons_ptr){ free(buttons_ptr) }
      if(hats_ptr){ free(hats_ptr) }
      if(info_ptr){ free(info_ptr) }
      return 0
   }
   memset(caps, 0, _WINMM_JOYCAPSW_SIZE)
   def caps_ok = joyGetDevCapsW(native_jid, caps, _WINMM_JOYCAPSW_SIZE) == JOYERR_NOERROR
   def name_raw = caps_ok ? _utf16_name(caps, _WINMM_CAPS_NAME, 32) : ""
   def name = name_raw.len > 0 ? name_raw : f"WinMM Controller {native_jid}"
   def mid = caps_ok ? load16(caps, 0) : 0
   def pid = caps_ok ? load16(caps, 2) : native_jid
   def caps_buttons = caps_ok ? int(load32(caps, _WINMM_CAPS_BUTTONS)) : _WINMM_MAX_BUTTONS
   mut button_count = caps_buttons
   if(button_count <= 0){ button_count = _WINMM_MAX_BUTTONS }
   if(button_count > _WINMM_MAX_BUTTONS){ button_count = _WINMM_MAX_BUTTONS }
   def has_pov = caps_ok && band(load32(caps, _WINMM_CAPS_FLAGS), JOYCAPS_HASPOV)
   memset(axes_ptr, 0, _WINMM_AXIS_COUNT * 4)
   memset(buttons_ptr, 0, _WINMM_MAX_BUTTONS)
   memset(hats_ptr, 0, _WINMM_HAT_COUNT)
   store32_f32(axes_ptr, -1.0, 16)
   store32_f32(axes_ptr, -1.0, 20)
   memcpy(info_ptr, info, _WINMM_JOYINFOEX_SIZE)
   def js = {
      "connected": true,
      "jid": jid,
      "native_jid": native_jid,
      "name": name,
      "guid": _build_winmm_guid(name, mid, pid),
      "axes_ptr": axes_ptr,
      "buttons_ptr": buttons_ptr,
      "hats_ptr": hats_ptr,
      "info_ptr": info_ptr,
      "axis_count": _WINMM_AXIS_COUNT,
      "button_count": button_count,
      "hat_count": has_pov ? _WINMM_HAT_COUNT : 0,
      "winmm_samples": 0,
      "kind": "winmm",
      "platform": "Windows"
   }
   _fill_winmm_buffers_from_info(js, info)
   free(caps)
   js
}

fn _update_js_state(int jid, any xi_state) any {
   def js = _get_js(jid)
   if(!js){ return nil }
   def ap, bp = js.get("axes_ptr", 0), js.get("buttons_ptr", 0)
   if(!ap || !bp){ return nil }
   def wbtns = load16(xi_state, _XI_BUTTONS)
   def lt    = load8(xi_state, _XI_LT)
   def rt    = load8(xi_state, _XI_RT)
   def lx    = _signed16(load16(xi_state, _XI_LX))
   def ly    = _signed16(load16(xi_state, _XI_LY))
   def rx    = _signed16(load16(xi_state, _XI_RX))
   def ry    = _signed16(load16(xi_state, _XI_RY))
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

fn _update_winmm_js_state(int jid) any {
   def js = _get_js(jid)
   if(!js){ return nil }
   def info = js.get("info_ptr", 0)
   if(!_read_winmm_state(js.get("native_jid", jid), info)){ return nil }
   _fill_winmm_buffers_from_info(js, info)
}

fn init() bool {
   "Initializes init."
   _ensure_init()
}

fn terminate() bool {
   "Runs the terminate operation."
   if(!_initialized){ return true }
   mut jid = 0
   while(jid < _MAX_JOYSTICKS){
      if(_get_js(jid)){
         _invoke_callback(jid, backend_api.DISCONNECTED)
         _free_js(jid)
      }
      jid += 1
   }
   if(_xi_state_buf){ free(_xi_state_buf) }
   if(_winmm_info_buf){ free(_winmm_info_buf) }
   _dinput_shutdown()
   _xi_state_buf = 0
   _winmm_info_buf = 0
   _last_poll_ticks = 0
   _last_winmm_scan_ticks = 0
   _last_dinput_scan_ticks = 0
   _initialized = false
   true
}

fn poll_joysticks() bool {
   "Polls poll joysticks."
   if(!_ensure_init()){ return false }
   def now = ticks()
   if(_last_poll_ticks > 0 && (now - _last_poll_ticks) < JOYSTICK_POLL_COALESCE_NS){ return true }
   _last_poll_ticks = now
   def xi_state = _scratch_xi_state()
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
   def scan_dinput = _last_dinput_scan_ticks <= 0 || (now - _last_dinput_scan_ticks) >= _DINPUT_RESCAN_INTERVAL_NS
   if(scan_dinput){
      _last_dinput_scan_ticks = now
      _dinput_scan()
   }
   jid = _DINPUT_BASE_JID
   while(jid < _MAX_JOYSTICKS){
      def js = _get_js(jid)
      if(js && js.get("kind", "") == "dinput"){
         _update_dinput_js_state(jid)
      }
      jid += 1
   }
   if(_dinput_connected_count() > 0){ return true }
   def info = _scratch_winmm_info()
   if(!info){ return true }
   def scan_winmm = _last_winmm_scan_ticks <= 0 || (now - _last_winmm_scan_ticks) >= WINMM_RESCAN_INTERVAL_NS
   if(scan_winmm){ _last_winmm_scan_ticks = now }
   def winmm_count = int(joyGetNumDevs())
   mut native_jid = 0
   while(native_jid < winmm_count && native_jid < _WINMM_MAX_SLOTS){
      def public_jid = _WINMM_BASE_JID + native_jid
      def was_connected = !!_get_js(public_jid)
      if(was_connected || scan_winmm){
         def present = _read_winmm_state(native_jid, info)
         if(present){
            if(!was_connected){
               def js = _alloc_winmm_js(public_jid, native_jid, info)
               if(js){
                  _put_js(public_jid, js)
                  _invoke_callback(public_jid, backend_api.CONNECTED)
               }
            }
            _update_winmm_js_state(public_jid)
         } else {
            if(was_connected){
               _invoke_callback(public_jid, backend_api.DISCONNECTED)
               _free_js(public_jid)
            }
         }
      }
      native_jid += 1
   }
   true
}

fn joystick_present(int jid) bool {
   "Runs the joystick present operation."
   if(!_ensure_init() || jid < 0 || jid >= _MAX_JOYSTICKS){ return false }
   poll_joysticks()
   def js = _get_js(jid)
   !!js.get("connected", false)
}

fn get_joystick_name(int jid) str {
   "Returns get joystick name."
   if(!joystick_present(jid)){ return "Unknown" }
   _get_js(jid).get("name", "Unknown")
}

fn get_joystick_guid(int jid) str {
   "Returns get joystick guid."
   if(!joystick_present(jid)){ return "00000000000000000000000000000000" }
   _get_js(jid).get("guid", "00000000000000000000000000000000")
}

fn get_joystick_axes(int jid, any count_ptr) any {
   "Returns get joystick axes."
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, js.get("axis_count", 0), 0) }
   js.get("axes_ptr", 0)
}

fn get_joystick_buttons(int jid, any count_ptr) any {
   "Returns get joystick buttons."
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, js.get("button_count", 0), 0) }
   js.get("buttons_ptr", 0)
}

fn get_joystick_hats(int jid, any count_ptr) any {
   "Returns get joystick hats."
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, js.get("hat_count", 0), 0) }
   js.get("hats_ptr", 0)
}

fn joystick_is_gamepad(int jid) bool {
   "Runs the joystick is gamepad operation."
   if(!joystick_present(jid)){ return false }
   def js = _get_js(jid)
   if(js.get("kind", "") == "xinput"){ return true }
   gamepad_map.joystick_is_gamepad(js)
}

fn _raw_button(any buttons_ptr, int button_count, int idx) int {
   (buttons_ptr && idx >= 0 && idx < button_count) ? load8(buttons_ptr, idx) : 0
}

fn _raw_axis(any axes_ptr, int axis_count, int idx) f64 {
   (axes_ptr && idx >= 0 && idx < axis_count) ? load32_f32(axes_ptr, idx * 4) : 0.0
}

fn _store_standard_button(any state_ptr, int dst, any buttons_ptr, int button_count, int src) any {
   store8(state_ptr, _raw_button(buttons_ptr, button_count, src), dst)
}

fn _winmm_uses_button_triggers(any js) bool {
   if(!js){ return false }
   def buttons_ptr = js.get("buttons_ptr", 0)
   def button_count = int(js.get("button_count", 0))
   def lt_down = _raw_button(buttons_ptr, button_count, 6) != 0
   def rt_down = _raw_button(buttons_ptr, button_count, 7) != 0 ||
   _raw_button(buttons_ptr, button_count, 8) != 0
   if(lt_down || rt_down){ js["winmm_button_triggers"] = true }
   !!js.get("winmm_button_triggers", false)
}

fn _store_winmm_standard_axes(any js, any state_ptr) bool {
   if(!js || !state_ptr){ return false }
   def axes_ptr = js.get("axes_ptr", 0)
   def axis_count = int(js.get("axis_count", 0))
   if(!axes_ptr){ return false }
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 0), 16)
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 1), 20)
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 2), 24)
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 3), 28)
   if(_winmm_uses_button_triggers(js)){
      def buttons_ptr = js.get("buttons_ptr", 0)
      def button_count = int(js.get("button_count", 0))
      store32_f32(state_ptr, _raw_button(buttons_ptr, button_count, 6) ? 1.0 : -1.0, 32)
      store32_f32(state_ptr,
         (_raw_button(buttons_ptr, button_count, 7) ||
         _raw_button(buttons_ptr, button_count, 8)) ? 1.0 : -1.0,
      36)
   } else {
      store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 5), 32)
      store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 4), 36)
   }
   true
}

fn _fill_winmm_gamepad_state(any js, any state_ptr) bool {
   if(!js || !state_ptr){ return false }
   memset(state_ptr, 0, 64)
   def buttons_ptr = js.get("buttons_ptr", 0)
   def hats_ptr = js.get("hats_ptr", 0)
   def button_count = int(js.get("button_count", 0))
   def hat_count = int(js.get("hat_count", 0))
   def button_triggers = _winmm_uses_button_triggers(js)
   _store_standard_button(state_ptr, 0, buttons_ptr, button_count, 0)
   _store_standard_button(state_ptr, 1, buttons_ptr, button_count, 1)
   _store_standard_button(state_ptr, 2, buttons_ptr, button_count, 3)
   _store_standard_button(state_ptr, 3, buttons_ptr, button_count, 2)
   _store_standard_button(state_ptr, 4, buttons_ptr, button_count, 4)
   _store_standard_button(state_ptr, 5, buttons_ptr, button_count, 5)
   if(!button_triggers){ _store_standard_button(state_ptr, 6, buttons_ptr, button_count, 8) }
   _store_standard_button(state_ptr, 7, buttons_ptr, button_count, 9)
   _store_standard_button(state_ptr, 8, buttons_ptr, button_count, 12)
   _store_standard_button(state_ptr, 9, buttons_ptr, button_count, 10)
   _store_standard_button(state_ptr, 10, buttons_ptr, button_count, 11)
   if(hats_ptr && hat_count > 0){
      def hat = load8(hats_ptr, 0)
      store8(state_ptr, (hat & HAT_UP) != 0 ? 1 : load8(state_ptr, 11), 11)
      store8(state_ptr, (hat & HAT_RIGHT) != 0 ? 1 : load8(state_ptr, 12), 12)
      store8(state_ptr, (hat & HAT_DOWN) != 0 ? 1 : load8(state_ptr, 13), 13)
      store8(state_ptr, (hat & HAT_LEFT) != 0 ? 1 : load8(state_ptr, 14), 14)
   }
   _store_winmm_standard_axes(js, state_ptr)
   true
}

fn _apply_standard_hat_buttons(any js, any state_ptr) any {
   if(!js || !state_ptr){ return nil }
   def hats_ptr = js.get("hats_ptr", 0)
   def hat_count = int(js.get("hat_count", 0))
   if(!hats_ptr || hat_count <= 0){ return nil }
   def hat = load8(hats_ptr, 0)
   store8(state_ptr, (hat & HAT_UP) != 0 ? 1 : 0, 11)
   store8(state_ptr, (hat & HAT_RIGHT) != 0 ? 1 : 0, 12)
   store8(state_ptr, (hat & HAT_DOWN) != 0 ? 1 : 0, 13)
   store8(state_ptr, (hat & HAT_LEFT) != 0 ? 1 : 0, 14)
}

fn get_gamepad_state(int jid, any state_ptr) bool {
   "Returns get gamepad state."
   if(!joystick_present(jid)){
      if(state_ptr){ memset(state_ptr, 0, 64) }
      return false
   }
   if(!state_ptr){ return false }
   def js = _get_js(jid)
   if(js.get("kind", "") == "winmm"){
      return _fill_winmm_gamepad_state(js, state_ptr)
   }
   if(js.get("kind", "") != "xinput"){
      def ok = gamepad_map.get_gamepad_state(js, state_ptr)
      if(ok){
         _apply_standard_hat_buttons(js, state_ptr)
      }
      return ok
   }
   def ap = js.get("axes_ptr", 0)
   def bp = js.get("buttons_ptr", 0)
   if(!ap || !bp){ return false }
   memset(state_ptr, 0, 64)
   memcpy(state_ptr, bp, _BUTTON_COUNT)
   memcpy(state_ptr + 16, ap, _AXIS_COUNT * 4)
   true
}

fn get_gamepad_name(int jid) str {
   "Returns get gamepad name."
   if(!joystick_present(jid)){ return "Unknown" }
   def js = _get_js(jid)
   if(js.get("kind", "") != "xinput"){
      def mapped = gamepad_map.get_gamepad_name(js)
      if(mapped && mapped.len > 0){ return mapped }
   }
   get_joystick_name(jid)
}

fn debug_joystick_objects(int jid) list {
   "Runs the debug joystick objects operation."
   poll_joysticks()
   def js = _get_js(jid)
   if(!js){ return [] }
   def objects = js.get("objects", [])
   mut out = []
   mut i = 0
   while(i < objects.len){
      def obj = objects[i]
      out = out.append({
            "slot": i,
            "type": int(obj.get("type", 0)),
            "offset": int(obj.get("offset", 0))
      })
      i += 1
   }
   out
}

fn debug_joystick_state(int jid) list {
   "Runs the debug joystick state operation."
   poll_joysticks()
   def js = _get_js(jid)
   if(!js || js.get("kind", "") != "dinput"){ return [] }
   def state = js.get("state_ptr", 0)
   def objects = js.get("objects", [])
   if(!state){ return [] }
   mut out = []
   mut i = 0
   while(i < objects.len){
      def obj = objects[i]
      def typ = int(obj.get("type", 0))
      def offset = int(obj.get("offset", 0))
      mut value = 0
      if(typ == _DINPUT_TYPE_AXIS || typ == _DINPUT_TYPE_SLIDER){
         value = load32(state, offset)
      } elif(typ == _DINPUT_TYPE_BUTTON){
         value = (load8(state, offset) & 0x80) != 0 ? 1 : 0
      } elif(typ == _DINPUT_TYPE_POV){
         value = load32(state, offset)
      }
      out = out.append({
            "slot": i,
            "type": typ,
            "offset": offset,
            "value": value
      })
      i += 1
   }
   out
}

fn set_joystick_callback(any cb) any {
   "Sets set joystick callback."
   def prev = _joystick_callback
   _joystick_callback = cb
   prev
}

fn update_gamepad_mappings(str s) int { gamepad_map.update_mappings(s, "Windows") }
