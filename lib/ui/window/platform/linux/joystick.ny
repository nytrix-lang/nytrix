;; Keywords: ui window joystick linux
;; Direct Ny port of GLFW's linux_joystick.c core behavior.

module std.ui.window.platform.linux.joystick (
   init, terminate, poll_joysticks,
   joystick_present, get_joystick_name, get_joystick_guid,
   get_joystick_axes, get_joystick_buttons, get_joystick_hats,
   joystick_is_gamepad, get_gamepad_state, get_gamepad_name,
   set_joystick_callback, update_gamepad_mappings
)

use std.core *
use std.core.error *
use std.core.mem *
use std.os.fs as osfs
use std.os.sys as sys
use std.str as str
use std.ui.window.platform.api as backend_api
use std.ui.window.platform.gamepad_map as gamepad_map

def MAX_JOYSTICKS = 16

def EV_CNT = 32
def KEY_CNT = 768
def ABS_CNT = 64

def BTN_MISC = 256
def ABS_HAT0X = 16
def ABS_HAT3Y = 23

def EV_SYN = 0
def EV_KEY = 1
def EV_ABS = 3

def SYN_REPORT = 0
def SYN_DROPPED = 3

def IN_CREATE = 0x100
def IN_ATTRIB = 0x4
def IN_DELETE = 0x200
def IN_NONBLOCK = 0x800
def IN_CLOEXEC = 0x80000

def O_RDONLY = 0x0
def O_NONBLOCK = 0x800
def O_CLOEXEC = 0x80000

def ENODEV = -19
def EAGAIN = -11

def EVIOCGBIT_EV = 0x80044520
def EVIOCGBIT_KEY = 0x80604521
def EVIOCGBIT_ABS = 0x80084523
def EVIOCGID = 0x80084502
def EVIOCGNAME_256 = 0x81004506
def EVIOCGABS_0 = 0x80184540

def INPUT_ID_SIZE = 8
def INPUT_ABSINFO_SIZE = 24
def INPUT_EVENT_SIZE = 24
def INOTIFY_EVENT_SIZE = 16

def INPUT_ID_BUSTYPE = 0
def INPUT_ID_VENDOR = 2
def INPUT_ID_PRODUCT = 4
def INPUT_ID_VERSION = 6

def ABSINFO_VALUE = 0
def ABSINFO_MINIMUM = 4
def ABSINFO_MAXIMUM = 8

def INPUT_EVENT_TYPE = 16
def INPUT_EVENT_CODE = 18
def INPUT_EVENT_VALUE = 20

def INOTIFY_MASK = 4
def INOTIFY_LEN = 12
def INOTIFY_NAME = 16

def HAT_CENTERED = 0
def HAT_UP = 1
def HAT_RIGHT = 2
def HAT_DOWN = 4
def HAT_LEFT = 8
def HAT_RIGHT_UP = 3
def HAT_RIGHT_DOWN = 6
def HAT_LEFT_UP = 9
def HAT_LEFT_DOWN = 12

mut _initialized = false
mut _inotify_fd = -1
mut _inotify_watch = -1
mut _dropped = false
mut _joysticks = dict()
mut _joystick_callback = 0

if(comptime{ __os_name() == "linux" }){
   #include <sys/inotify.h>
}

fn _is_event_device_name(name){
   if(!name || !is_str(name)){ return false }
   if(!str.startswith(name, "event")){ return false }
   def n = str.str_len(name)
   if(n <= 5){ return false }
   mut i = 5
   while(i < n){
      def c = load8(name, i)
      if(c < 48 || c > 57){ return false }
      i += 1
   }
   true
}

fn _bit_is_set(bits, bit){
   if(!bits || bit < 0){ return false }
   (load8(bits, bit / 8) & (1 << (bit % 8))) != 0
}

fn _find_slot_by_path(path){
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      def js = _get_js(jid)
      if(js && dict_get(js, "connected", false) && dict_get(js, "path", "") == path){
         return jid
      }
      jid += 1
   }
   -1
}

fn _close_fd(fd){
   if(fd >= 0){ unwrap(sys.sys_close(fd)) }
}

fn _free_ptr(p){
   if(p){ free(p) }
}

fn _clear_slot(jid){
   _joysticks = dict_set(_joysticks, jid, 0)
}

fn _disconnect_js(jid){
   if(!_get_js(jid)){ return false }
   _invoke_callback(jid, backend_api.DISCONNECTED)
   _free_js(jid)
}

fn _hat_state(x_state, y_state){
   if(x_state == 1){
      if(y_state == 1){ return HAT_LEFT_UP }
      if(y_state == 2){ return HAT_LEFT_DOWN }
      return HAT_LEFT
   }
   if(x_state == 2){
      if(y_state == 1){ return HAT_RIGHT_UP }
      if(y_state == 2){ return HAT_RIGHT_DOWN }
      return HAT_RIGHT
   }
   if(y_state == 1){ return HAT_UP }
   if(y_state == 2){ return HAT_DOWN }
   HAT_CENTERED
}

fn _set_hat_buttons(js, hat_index, state){
   def buttons_ptr = dict_get(js, "buttons_ptr", 0)
   if(!buttons_ptr){ return js }
   def raw_button_count = dict_get(js, "raw_button_count", 0)
   def base = raw_button_count + hat_index * 4
   store8(buttons_ptr, band(state, HAT_UP) ? 1 : 0, base + 0)
   store8(buttons_ptr, band(state, HAT_RIGHT) ? 1 : 0, base + 1)
   store8(buttons_ptr, band(state, HAT_DOWN) ? 1 : 0, base + 2)
   store8(buttons_ptr, band(state, HAT_LEFT) ? 1 : 0, base + 3)
   js
}

fn _handle_key_event(js, code, value){
   if(code < BTN_MISC || code >= KEY_CNT){ return js }
   def key_map_ptr = dict_get(js, "key_map_ptr", 0)
   def buttons_ptr = dict_get(js, "buttons_ptr", 0)
   if(!key_map_ptr || !buttons_ptr){ return js }
   def index = load32(key_map_ptr, (code - BTN_MISC) * 4)
   if(index < 0){ return js }
   store8(buttons_ptr, value ? 1 : 0, index)
   js
}

fn _handle_abs_event(js, code, value){
   if(code < 0 || code >= ABS_CNT){ return js }
   def abs_map_ptr = dict_get(js, "abs_map_ptr", 0)
   if(!abs_map_ptr){ return js }
   def index = load32(abs_map_ptr, code * 4)
   if(index < 0){ return js }

   if(code >= ABS_HAT0X && code <= ABS_HAT3Y){
      def hat = (code - ABS_HAT0X) / 2
      def axis = (code - ABS_HAT0X) % 2
      def hat_axes_ptr = dict_get(js, "hat_axes_ptr", 0)
      def hats_ptr = dict_get(js, "hats_ptr", 0)
      if(!hat_axes_ptr || !hats_ptr){ return js }

      mut state_part = 0
      if(value < 0){ state_part = 1 }
      elif(value > 0){ state_part = 2 }
      store32(hat_axes_ptr, state_part, (hat * 2 + axis) * 4)

      def x_state = load32(hat_axes_ptr, (hat * 2 + 0) * 4)
      def y_state = load32(hat_axes_ptr, (hat * 2 + 1) * 4)
      def state = _hat_state(x_state, y_state)
      store8(hats_ptr, state, hat)
      _set_hat_buttons(js, index, state)
      return js
   }

   def axes_ptr = dict_get(js, "axes_ptr", 0)
   def abs_info_ptr = dict_get(js, "abs_info_ptr", 0)
   if(!axes_ptr || !abs_info_ptr){ return js }

   def info_off = code * INPUT_ABSINFO_SIZE
   def minimum = load32(abs_info_ptr, info_off + ABSINFO_MINIMUM)
   def maximum = load32(abs_info_ptr, info_off + ABSINFO_MAXIMUM)

   mut normalized = float(value)
   def range = maximum - minimum
   if(range){
      normalized = (normalized - float(minimum)) / float(range)
      normalized = normalized * 2.0 - 1.0
   }
   store32_f32(axes_ptr, normalized, index * 4)
   js
}

fn _poll_abs_state(js){
   if(!js){ return js }
   def fd = dict_get(js, "fd", -1)
   def abs_map_ptr = dict_get(js, "abs_map_ptr", 0)
   def abs_info_ptr = dict_get(js, "abs_info_ptr", 0)
   if(fd < 0 || !abs_map_ptr || !abs_info_ptr){ return js }

   mut code = 0
   while(code < ABS_CNT){
      if(load32(abs_map_ptr, code * 4) >= 0){
         def info_ptr = abs_info_ptr + code * INPUT_ABSINFO_SIZE
         if(!is_err(sys.sys_ioctl(fd, EVIOCGABS_0 + code, info_ptr))){
         js = _handle_abs_event(js, code, load32(info_ptr, ABSINFO_VALUE))
         }
      }
      code += 1
   }
   js
}

fn _guid16(v){
   def lo = v & 0xff
   def hi = (v >> 8) & 0xff
   str.to_hex(lo, 2) + str.to_hex(hi, 2)
}

fn _name_byte(name, index){
   def n = str.str_len(name)
   if(index >= 0 && index < n){ return load8(name, index) }
   0
}

fn _open_device(path){
   if(_find_slot_by_path(path) >= 0){ return false }
   def jid = _find_free_slot()
   if(jid < 0){ return false }

   match sys.sys_open(path, bor(bor(O_RDONLY, O_NONBLOCK), O_CLOEXEC), 0){
      err(_) -> { return false }
      ok(fd) -> {
         def ev_bits = calloc(1, (EV_CNT + 7) / 8)
         def key_bits = calloc(1, (KEY_CNT + 7) / 8)
         def abs_bits = calloc(1, (ABS_CNT + 7) / 8)
         def id_ptr = calloc(1, INPUT_ID_SIZE)
         def name_ptr = calloc(1, 256)
         if(!ev_bits || !key_bits || !abs_bits || !id_ptr || !name_ptr){
         _close_fd(fd)
         _free_ptr(ev_bits) _free_ptr(key_bits) _free_ptr(abs_bits)
         _free_ptr(id_ptr) _free_ptr(name_ptr)
         return false
         }

         def ok_ev = !is_err(sys.sys_ioctl(fd, EVIOCGBIT_EV, ev_bits))
         def ok_key = !is_err(sys.sys_ioctl(fd, EVIOCGBIT_KEY, key_bits))
         def ok_abs = !is_err(sys.sys_ioctl(fd, EVIOCGBIT_ABS, abs_bits))
         def ok_id = !is_err(sys.sys_ioctl(fd, EVIOCGID, id_ptr))
         if(!(ok_ev && ok_key && ok_abs && ok_id)){
         _close_fd(fd)
         _free_ptr(ev_bits) _free_ptr(key_bits) _free_ptr(abs_bits)
         _free_ptr(id_ptr) _free_ptr(name_ptr)
         return false
         }

         if(!_bit_is_set(ev_bits, EV_ABS)){
         _close_fd(fd)
         _free_ptr(ev_bits) _free_ptr(key_bits) _free_ptr(abs_bits)
         _free_ptr(id_ptr) _free_ptr(name_ptr)
         return false
         }

         memset(name_ptr, 0, 256)
         if(is_err(sys.sys_ioctl(fd, EVIOCGNAME_256, name_ptr))){
         store8(name_ptr, 85, 0)
         store8(name_ptr, 110, 1)
         store8(name_ptr, 107, 2)
         store8(name_ptr, 110, 3)
         store8(name_ptr, 111, 4)
         store8(name_ptr, 119, 5)
         store8(name_ptr, 110, 6)
         store8(name_ptr, 0, 7)
         }

         def name = str.cstr_to_str(name_ptr)
         def guid = _build_guid(id_ptr, name)

         def key_map_ptr = malloc((KEY_CNT - BTN_MISC) * 4)
         def abs_map_ptr = malloc(ABS_CNT * 4)
         def abs_info_ptr = calloc(ABS_CNT, INPUT_ABSINFO_SIZE)
         if(!key_map_ptr || !abs_map_ptr || !abs_info_ptr){
         _close_fd(fd)
         _free_ptr(ev_bits) _free_ptr(key_bits) _free_ptr(abs_bits)
         _free_ptr(id_ptr) _free_ptr(name_ptr)
         _free_ptr(key_map_ptr) _free_ptr(abs_map_ptr) _free_ptr(abs_info_ptr)
         return false
         }

         mut i = 0
         while(i < KEY_CNT - BTN_MISC){
         store32(key_map_ptr, -1, i * 4)
         i += 1
         }
         i = 0
         while(i < ABS_CNT){
         store32(abs_map_ptr, -1, i * 4)
         i += 1
         }

         mut raw_button_count = 0
         mut axis_count = 0
         mut hat_count = 0

         mut code = BTN_MISC
         while(code < KEY_CNT){
         if(_bit_is_set(key_bits, code)){
               store32(key_map_ptr, raw_button_count, (code - BTN_MISC) * 4)
               raw_button_count += 1
         }
         code += 1
         }

         code = 0
         while(code < ABS_CNT){
         if(_bit_is_set(abs_bits, code)){
               if(code >= ABS_HAT0X && code <= ABS_HAT3Y){
                  store32(abs_map_ptr, hat_count, code * 4)
                  hat_count += 1
                  code += 1
               } else {
                  def info_ptr = abs_info_ptr + code * INPUT_ABSINFO_SIZE
                  if(!is_err(sys.sys_ioctl(fd, EVIOCGABS_0 + code, info_ptr))){
                     store32(abs_map_ptr, axis_count, code * 4)
                     axis_count += 1
                  }
               }
         }
         code += 1
         }

         def button_count = raw_button_count + hat_count * 4
         def axes_ptr = axis_count > 0 ? calloc(axis_count, 4) : 0
         def buttons_ptr = button_count > 0 ? calloc(button_count, 1) : 0
         def hats_ptr = hat_count > 0 ? calloc(hat_count, 1) : 0
         def hat_axes_ptr = hat_count > 0 ? calloc(hat_count * 2, 4) : 0

         mut js = dict()
         js = dict_set(js, "connected", true)
         js = dict_set(js, "fd", fd)
         js = dict_set(js, "path", path)
         js = dict_set(js, "name", name)
         js = dict_set(js, "guid", guid)
         js = dict_set(js, "axis_count", axis_count)
         js = dict_set(js, "raw_button_count", raw_button_count)
         js = dict_set(js, "button_count", button_count)
         js = dict_set(js, "hat_count", hat_count)
         js = dict_set(js, "axes_ptr", axes_ptr)
         js = dict_set(js, "buttons_ptr", buttons_ptr)
         js = dict_set(js, "hats_ptr", hats_ptr)
         js = dict_set(js, "hat_axes_ptr", hat_axes_ptr)
         js = dict_set(js, "key_map_ptr", key_map_ptr)
         js = dict_set(js, "abs_map_ptr", abs_map_ptr)
         js = dict_set(js, "abs_info_ptr", abs_info_ptr)

         js = _poll_abs_state(js)
         _put_js(jid, js)

         _free_ptr(ev_bits) _free_ptr(key_bits) _free_ptr(abs_bits)
         _free_ptr(id_ptr) _free_ptr(name_ptr)
         _invoke_callback(jid, backend_api.CONNECTED)
         true
      }
   }
}

fn _scan_devices(){
   if(!_has_native_support()){ return 0 }
   def names = osfs.list_dir("/dev/input")
   mut i = 0
   while(i < len(names)){
      def name = get(names, i)
      if(_is_event_device_name(name)){
         _open_device("/dev/input/" + to_str(name))
      }
      i += 1
   }
   0
}

fn _detect_connections(){
   if(_inotify_fd < 0){ return 0 }
   def buffer = malloc(16384)
   if(!buffer){ return 0 }

   match sys.sys_read(_inotify_fd, buffer, 16384){
      err(_) -> {
         free(buffer)
         return 0
      }
      ok(size) -> {
         mut offset = 0
         while(offset + INOTIFY_EVENT_SIZE <= size){
         def event_ptr = buffer + offset
         def mask = load32(event_ptr, INOTIFY_MASK)
         def name_len = load32(event_ptr, INOTIFY_LEN)
         def name = str.cstr_to_str(event_ptr + INOTIFY_NAME)

         if(_is_event_device_name(name)){
               def path = "/dev/input/" + name
               if(mask & bor(IN_CREATE, IN_ATTRIB)){ _open_device(path) }
               elif(mask & IN_DELETE){
                  def jid = _find_slot_by_path(path)
                  if(jid >= 0){ _disconnect_js(jid) }
               }
         }

         offset += INOTIFY_EVENT_SIZE + name_len
         }
      }
   }
   free(buffer)
   0
}

fn _poll_slot(jid){
   mut js = _get_js(jid)
   if(!js || !dict_get(js, "connected", false)){ return false }

   def fd = dict_get(js, "fd", -1)
   if(fd < 0){
      _disconnect_js(jid)
      return false
   }

   def event_ptr = malloc(INPUT_EVENT_SIZE)
   if(!event_ptr){ return true }

   while(true){
      match sys.sys_read(fd, event_ptr, INPUT_EVENT_SIZE){
         err(errcode) -> {
         if(errcode == ENODEV){
               free(event_ptr)
               _disconnect_js(jid)
               return false
         }
         break
         }
         ok(nread) -> {
         if(nread < INPUT_EVENT_SIZE){ break }
         def typ = load16(event_ptr, INPUT_EVENT_TYPE)
         def code = load16(event_ptr, INPUT_EVENT_CODE)
         def value = load32(event_ptr, INPUT_EVENT_VALUE)

         if(typ == EV_SYN){
               if(code == SYN_DROPPED){
                  _dropped = true
               } elif(code == SYN_REPORT){
                  _dropped = false
                  js = _poll_abs_state(js)
               }
         }

         if(_dropped){ continue }

         if(typ == EV_KEY){
               js = _handle_key_event(js, code, value)
         } elif(typ == EV_ABS){
               js = _handle_abs_event(js, code, value)
         }
         }
      }
   }

   free(event_ptr)
   _put_js(jid, js)
   true
}

fn _ensure_init(){
   if(_initialized || !_has_native_support()){ return _initialized }
   _initialized = true

   _inotify_fd = inotify_init1(bor(IN_NONBLOCK, IN_CLOEXEC))
   if(_inotify_fd >= 0){
      _inotify_watch = inotify_add_watch(_inotify_fd, cstr("/dev/input"),
         bor(bor(IN_CREATE, IN_ATTRIB), IN_DELETE))
   }

   _scan_devices()
   true
}

fn init(){
   "Initializes native Linux joystick handling."
   gamepad_map.init_default_linux_mappings()
   _ensure_init()
}

fn terminate(){
   "Shuts down native Linux joystick handling."
   if(!_initialized){ return true }

   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(_get_js(jid)){ _free_js(jid) }
      jid += 1
   }

   if(_inotify_fd >= 0){
      if(_inotify_watch >= 0){ inotify_rm_watch(_inotify_fd, _inotify_watch) }
      _close_fd(_inotify_fd)
   }

   _joysticks = dict()
   _inotify_fd = -1
   _inotify_watch = -1
   _dropped = false
   _initialized = false
   true
}

fn poll_joysticks(){
   "Polls connection changes and queued joystick events."
   if(!_ensure_init()){ return false }
   _detect_connections()
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(_get_js(jid)){ _poll_slot(jid) }
      jid += 1
   }
   true
}

fn joystick_present(jid){
   if(!_ensure_init() || jid < 0 || jid >= MAX_JOYSTICKS){ return false }
   poll_joysticks()
   def js = _get_js(jid)
   !!dict_get(js, "connected", false)
}

fn get_joystick_name(jid){
   if(!joystick_present(jid)){ return "Unknown" }
   dict_get(_get_js(jid), "name", "Unknown")
}

fn get_joystick_guid(jid){
   if(!joystick_present(jid)){ return "00000000000000000000000000000000" }
   dict_get(_get_js(jid), "guid", "00000000000000000000000000000000")
}

fn get_joystick_axes(jid, count_ptr){
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, dict_get(js, "axis_count", 0), 0) }
   dict_get(js, "axes_ptr", 0)
}

fn get_joystick_buttons(jid, count_ptr){
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, dict_get(js, "button_count", 0), 0) }
   dict_get(js, "buttons_ptr", 0)
}

fn get_joystick_hats(jid, count_ptr){
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, dict_get(js, "hat_count", 0), 0) }
   dict_get(js, "hats_ptr", 0)
}

fn joystick_is_gamepad(jid){
   if(!joystick_present(jid)){ return false }
   gamepad_map.joystick_is_gamepad(_get_js(jid))
}

fn get_gamepad_state(jid, state_ptr){
   if(!joystick_present(jid)){ if(state_ptr){ memset(state_ptr, 0, 64) } return 0 }
   gamepad_map.get_gamepad_state(_get_js(jid), state_ptr)
}

fn get_gamepad_name(jid){
   if(!joystick_present(jid)){ return "Unknown" }
   def name = gamepad_map.get_gamepad_name(_get_js(jid))
   if(name && str.str_len(name) > 0){ return name }
   get_joystick_name(jid)
}

fn set_joystick_callback(cb){
   def prev = _joystick_callback
   _joystick_callback = cb
   prev
}

fn update_gamepad_mappings(_s){
   gamepad_map.update_mappings(_s, "Linux")
}
