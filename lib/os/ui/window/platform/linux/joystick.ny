;; Keywords: platform window backend linux joystick
;; Native Linux joystick backend behavior.
module std.os.ui.window.platform.linux.joystick(init, terminate, poll_joysticks, joystick_present, get_joystick_name, get_joystick_guid, get_joystick_axes, get_joystick_buttons, get_joystick_hats, joystick_is_gamepad, get_gamepad_state, get_gamepad_name, set_joystick_callback, update_gamepad_mappings)
use std.core
use std.core.mem
use std.os.fs as osfs
use std.os.time
use std.core.str as str
use std.os.ui.profile as ui_profile
use std.os.ui.window.platform.api as backend_api
use std.os.ui.window.platform.gamepad_map as gamepad_map
use std.core.common as common

def MAX_JOYSTICKS = 16
def EV_CNT = 32
def KEY_CNT = 768
def ABS_CNT = 64
def BTN_MISC = 256
def BTN_JOYSTICK = 288
def BTN_DIGI = 320
def ABS_HAT0X = 16
def ABS_HAT3Y = 23
def EV_SYN = 0
def EV_KEY = 1
def EV_ABS = 3
def SYN_REPORT = 0
def SYN_DROPPED = 3
def IN_CREATE = 0x100
def IN_ATTRIB = 0x4
def IN_CLOSE_WRITE = 0x8
def IN_DELETE = 0x200
def IN_MOVED_TO = 0x80
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
def EVIOCGKEY_96 = 0x80604518
def EVIOCGID = 0x80084502
def EVIOCGNAME_256 = 0x81004506
def EVIOCGABS_0 = 0x80184540
def JSIOCGAXES = 0x80016a11
def JSIOCGBUTTONS = 0x80016a12
def JSIOCGNAME_128 = 0x80806a13
def INPUT_ID_SIZE = 8
def INPUT_ABSINFO_SIZE = 24
def INPUT_EVENT_SIZE = 24
def INOTIFY_EVENT_SIZE = 16
def JS_EVENT_SIZE = 8
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
def JS_EVENT_BUTTON = 0x01
def JS_EVENT_AXIS = 0x02
def JS_EVENT_INIT = 0x80
def MAX_JS_BUTTONS = 256
def MAX_JS_AXES = 64
def MAX_JS_HATS = 16
def DEVICE_RESCAN_INTERVAL_NS = 250000000
def JOYSTICK_POLL_COALESCE_NS = 1000000
def JOYSTICK_STATE_SYNC_INTERVAL_NS = 8000000
def JOYSTICK_JS_RETRY_INTERVAL_NS = 250000000
mut _js_buttons = []
mut _js_axes = []
mut _js_hats = []
mut _js_hat_axes = []
mut ji = 0
while(ji < MAX_JOYSTICKS){
   _js_buttons = _js_buttons.append(0)
   _js_axes = _js_axes.append(0)
   _js_hats = _js_hats.append(0)
   _js_hat_axes = _js_hat_axes.append(0)
   ji += 1
}

mut _linux_js = dict(8)

fn _ensure_slot_buffers(int: jid): bool {
   if(jid < 0 || jid >= MAX_JOYSTICKS){ return false }
   while(_js_buttons.len <= jid){ _js_buttons = _js_buttons.append(0) }
   while(_js_axes.len <= jid){ _js_axes = _js_axes.append(0) }
   while(_js_hats.len <= jid){ _js_hats = _js_hats.append(0) }
   while(_js_hat_axes.len <= jid){ _js_hat_axes = _js_hat_axes.append(0) }
   if(!_js_buttons.get(jid, 0)){ _js_buttons[jid] = malloc(MAX_JS_BUTTONS) }
   if(!_js_axes.get(jid, 0)){ _js_axes[jid] = malloc(MAX_JS_AXES * 4) }
   if(!_js_hats.get(jid, 0)){ _js_hats[jid] = malloc(MAX_JS_HATS) }
   if(!_js_hat_axes.get(jid, 0)){ _js_hat_axes[jid] = malloc(MAX_JS_HATS * 2 * 4) }
   _js_buttons.get(jid, 0) && _js_axes.get(jid, 0) && _js_hats.get(jid, 0) && _js_hat_axes.get(jid, 0)
}

fn _reset_js_buffers(int: jid): bool {
   if(jid < 0 || jid >= MAX_JOYSTICKS){ return false }
   if(!_ensure_slot_buffers(jid)){ return false }
   def buttons_ptr = _js_buttons.get(jid, 0)
   def axes_ptr = _js_axes.get(jid, 0)
   def hats_ptr = _js_hats.get(jid, 0)
   def hat_axes_ptr = _js_hat_axes.get(jid, 0)
   if(buttons_ptr){ memset(buttons_ptr, 0, MAX_JS_BUTTONS) }
   if(axes_ptr){ memset(axes_ptr, 0, MAX_JS_AXES * 4) }
   if(hats_ptr){ memset(hats_ptr, 0, MAX_JS_HATS) }
   if(hat_axes_ptr){ memset(hat_axes_ptr, 0, MAX_JS_HATS * 2 * 4) }
   true
}

fn _get_js_val(str: key, any: default=0): any { _linux_js.get(key, default) }

fn _set_js_val(str: key, any: val): any {
   _linux_js[key] = val
   val
}

fn _is_debug(): bool {
   ui_profile.debug_enabled() || ui_profile.env_truthy_cached("NY_JOYSTICK_DEBUG")
}

fn _is_trace(): bool { ui_profile.env_truthy_cached("NY_JOYSTICK_TRACE") }

fn _dbg(any: msg): bool {
   if(_is_debug()){ ui_profile.print_text("[linux:joystick] " + to_str(msg)) }
   false
}

fn _trace(any: msg): bool {
   if(_is_trace()){ ui_profile.print_text("[linux:joystick:trace] " + to_str(msg)) }
   false
}

fn _invoke_callback(int: jid, int: event): any {
   def cb = _get_js_val("joystick_callback", 0)
   if(cb){ cb(jid, event) }
}

#linux {
   #include <sys/inotify.h>
} #else {
   fn inotify_init1(any: _flags): int { -1 }
   fn inotify_add_watch(any: _fd, any: _path, any: _mask): int { -1 }
   fn inotify_rm_watch(any: _fd, any: _wd): int { 0 }
} #endif

fn _is_event_device_name(any: name): bool {
   if(!name || !is_str(name)){ return false }
   if(!str.startswith(name, "event")){ return false }
   def n = name.len
   if(n <= 5){ return false }
   mut i = 5
   while(i < n){
      def c = load8(name, i)
      if(c < 48 || c > 57){ return false }
      i += 1
   }
   true
}

fn _bit_is_set(any: bits, int: bit): bool {
   if(!bits || bit < 0){ return false }
   (load8(bits, bit / 8) & (1 << (bit % 8))) != 0
}

fn _looks_like_aux_input(any: name): bool {
   def lname = str.lower(to_str(name))
   if(str.find(lname, "touchpad") != -1){ return true }
   if(str.find(lname, "motion sensor") != -1){ return true }
   if(str.find(lname, "mouse") != -1){ return true }
   if(str.find(lname, "keyboard") != -1){ return true }
   if(str.find(lname, "consumer control") != -1){ return true }
   if(str.find(lname, "headset") != -1){ return true }
   false
}

fn _is_controller_aux_input(any: name): bool {
   def lname = str.lower(to_str(name))
   def controller =
      str.find(lname, "controller") != -1 ||
      str.find(lname, "gamepad") != -1 ||
      str.find(lname, "dualsense") != -1 ||
      str.find(lname, "dualshock") != -1 ||
      str.find(lname, "xbox") != -1
   if(!controller){ return false }
   str.find(lname, "motion sensor") != -1 ||
      str.find(lname, "motion sensors") != -1 ||
      str.find(lname, "touchpad") != -1
}

fn _linux_input_supported(): bool { return osfs.is_dir("/dev/input") }

fn _get_js(int: jid): any {
   if(jid < 0 || jid >= MAX_JOYSTICKS){ return 0 }
   _get_js_val("joysticks", dict(8)).get(jid, 0)
}

fn _put_js(int: jid, any: js): any {
   if(jid >= 0 && jid < MAX_JOYSTICKS){
      mut joysticks = _get_js_val("joysticks", dict(8))
      joysticks[jid] = js
      _set_js_val("joysticks", joysticks)
   }
   js
}

fn _find_free_slot(): int {
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(!_get_js(jid)){ return jid }
      jid += 1
   }
   -1
}

fn _free_js(int: jid): bool {
   def js = _get_js(jid)
   if(!js){ return false }
   _close_fd(js.get("fd", -1))
   _close_fd(js.get("js_fd", -1))
   _reset_js_buffers(jid)
   _free_ptr(js.get("key_map_ptr", 0))
   _free_ptr(js.get("abs_map_ptr", 0))
   _free_ptr(js.get("abs_info_ptr", 0))
   _clear_slot(jid)
   true
}

fn _find_slot_by_path(str: path): int {
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      def js = _get_js(jid)
      if(js && js.get("connected", false) && js.get("path", "") == path){ return jid }
      jid += 1
   }
   -1
}

fn _close_fd(int: fd): any { if(fd >= 0){ __close(fd) } }

fn _free_ptr(any: p): any { if(p){ free(p) } }

fn _free_probe_allocs(any: ev_bits, any: key_bits, any: abs_bits, any: id_ptr, any: name_ptr): any {
   _free_ptr(ev_bits) _free_ptr(key_bits) _free_ptr(abs_bits)
   _free_ptr(id_ptr) _free_ptr(name_ptr)
}

fn _free_map_allocs(any: key_map_ptr, any: abs_map_ptr, any: abs_info_ptr): any { _free_ptr(key_map_ptr) _free_ptr(abs_map_ptr) _free_ptr(abs_info_ptr) }

fn _reject_open_device(
   int: fd, int: js_fd, any: ev_bits, any: key_bits, any: abs_bits, any: id_ptr, any: name_ptr,
   any: key_map_ptr=0, any: abs_map_ptr=0, any: abs_info_ptr=0
): bool {
   _close_fd(fd)
   _close_fd(js_fd)
   _free_probe_allocs(ev_bits, key_bits, abs_bits, id_ptr, name_ptr)
   _free_map_allocs(key_map_ptr, abs_map_ptr, abs_info_ptr)
   false
}

fn _open_fd(str: path, int: flags, int: mode=0): int { __open(path, flags, mode) }

fn _read_fd(int: fd, any: buf, int: n): int { __read_off(fd, buf, n, 0) }

fn _ioctl(int: fd, any: req, any: arg): int { __ioctl(fd, int(req) & 0xffffffff, arg) }

fn _clear_slot(int: jid): any {
   mut joysticks = _get_js_val("joysticks", dict(8))
   joysticks[jid] = 0
   _set_js_val("joysticks", joysticks)
}

fn _disconnect_js(int: jid): bool {
   if(!_get_js(jid)){ return false }
   def js = _get_js(jid)
   _dbg("disconnected jid=" + to_str(jid) +
      " path='" + to_str(js.get("path", "")) + "'" +
   " name='" + to_str(js.get("name", "")) + "'")
   _invoke_callback(jid, backend_api.DISCONNECTED)
   _free_js(jid)
}

fn _hat_state(int: x_state, int: y_state): int {
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

fn _signed32(any: v): int {
   def x = int(v) & 0xffffffff
   if(x >= 0x80000000){ x - 0x100000000 } else { x }
}

fn _signed16(any: v): int {
   def x = int(v) & 0xffff
   if(x >= 0x8000){ x - 0x10000 } else { x }
}

fn _same_input_name(any: a, any: b): bool { str.lower(str.strip(to_str(a))) == str.lower(str.strip(to_str(b))) }

fn _normalize_js_axis(any: value): f64 {
   def fv = float(value)
   if(fv <= -32767.0){ return -1.0 }
   if(fv >= 32767.0){ return 1.0 }
   fv / 32767.0
}

fn _open_js_fd_for_name(any: name): list {
   if(!_linux_input_supported()){ return [-1, 0, 0] }
   def names = osfs.list_dir("/dev/input")
   mut i = 0
   def names_n = names.len
   while(i < names_n){
      def entry = to_str(names.get(i))
      if(str.startswith(entry, "js")){
         def fd = _open_fd("/dev/input/" + entry, bor(bor(O_RDONLY, O_NONBLOCK), O_CLOEXEC), 0)
         if(fd >= 0){
            def name_ptr = malloc(128)
            if(!name_ptr){ _close_fd(fd) i += 1 continue }
            def axes_ptr = malloc(1)
            if(!axes_ptr){ _close_fd(fd) free(name_ptr) i += 1 continue }
            def buttons_ptr = malloc(1)
            if(!buttons_ptr){ _close_fd(fd) free(name_ptr) free(axes_ptr) i += 1 continue }
            memset(name_ptr, 0, 128)
            memset(axes_ptr, 0, 1)
            memset(buttons_ptr, 0, 1)
            def ok_name = _ioctl(fd, JSIOCGNAME_128, name_ptr) >= 0
            def ok_axes = _ioctl(fd, JSIOCGAXES, axes_ptr) >= 0
            def ok_buttons = _ioctl(fd, JSIOCGBUTTONS, buttons_ptr) >= 0
            if(ok_name && ok_axes && ok_buttons){
               def js_name = str.cstr_to_str(name_ptr)
               def js_axes = load8(axes_ptr, 0)
               def js_buttons = load8(buttons_ptr, 0)
               if(js_buttons > 0 && _same_input_name(js_name, name)){
                  free(name_ptr, axes_ptr, buttons_ptr)
                  return [fd, js_axes, js_buttons]
               }
            }
            _close_fd(fd)
            _free_ptr(name_ptr)
            _free_ptr(axes_ptr)
            _free_ptr(buttons_ptr)
         }
      }
      i += 1
   }
   [-1, 0, 0]
}

fn _maybe_attach_js_fd(any: js): any {
   if(!js){ return js }
   if(!js.get("use_js", false)){ return js }
   if(js.get("js_fd", -1) >= 0){ return js }
   def name = js.get("name", "")
   if(!name || name.len == 0){ return js }
   def now = ticks()
   def last_try = js.get("last_js_open_ticks", 0)
   if(last_try > 0 && (now - last_try) < JOYSTICK_JS_RETRY_INTERVAL_NS){ return js }
   js["last_js_open_ticks"] = now
   def js_dev = _open_js_fd_for_name(name)
   def js_fd = js_dev.get(0, -1)
   if(js_fd >= 0){
      js["js_fd"] = js_fd
      js["js_axes_count"] = js_dev.get(1, 0)
      js["js_button_count"] = js_dev.get(2, 0)
      _dbg("paired js name='" + to_str(name) + "'" +
         " js_fd=" + to_str(js_fd) +
         " js_axes=" + to_str(js_dev.get(1, 0)) +
      " js_buttons=" + to_str(js_dev.get(2, 0)))
   }
   js
}

fn _handle_key_event(int: jid, any: js, int: code, any: value): any {
   if(code < BTN_MISC || code >= KEY_CNT){ return js }
   def key_map_ptr = js.get("key_map_ptr", 0)
   def buttons_ptr = _js_buttons.get(jid, 0)
   if(!key_map_ptr || !buttons_ptr){ return js }
   def index = load32(key_map_ptr, (code - BTN_MISC) * 4)
   if(index < 0 || index >= MAX_JS_BUTTONS){ return js }
   store8(buttons_ptr, value ? 1 : 0, index)
   _trace("key jid=" + to_str(jid) + " code=" + to_str(code) + " index=" + to_str(index) + " value=" + to_str(value ? 1 : 0))
   js
}

fn _handle_abs_event(int: jid, any: js, int: code, any: value): any {
   if(code < 0 || code >= ABS_CNT){ return js }
   def abs_map_ptr = js.get("abs_map_ptr", 0)
   if(!abs_map_ptr){ return js }
   def index = load32(abs_map_ptr, code * 4)
   if(index < 0){
      _trace("abs-unmapped jid=" + to_str(jid) + " code=" + to_str(code) + " value=" + to_str(value))
      return js
   }
   if(code >= ABS_HAT0X && code <= ABS_HAT3Y){
      def hat = (code - ABS_HAT0X) / 2
      def axis = (code - ABS_HAT0X) % 2
      def hat_axes_ptr = _js_hat_axes.get(jid, 0)
      def hats_ptr = _js_hats.get(jid, 0)
      if(!hat_axes_ptr || !hats_ptr || hat < 0 || hat >= MAX_JS_HATS){ return js }
      mut state_part = 0
      if(value < 0){ state_part = 1 }
      elif(value > 0){ state_part = 2 }
      store32(hat_axes_ptr, state_part, (hat * 2 + axis) * 4)
      def x_state, y_state = load32(hat_axes_ptr, (hat * 2 + 0) * 4), load32(hat_axes_ptr, (hat * 2 + 1) * 4)
      def state = _hat_state(x_state, y_state)
      store8(hats_ptr, state, hat)
      _trace("hat jid=" + to_str(jid) + " code=" + to_str(code) + " hat=" + to_str(hat) + " axis=" + to_str(axis) + " value=" + to_str(value) + " state=" + to_str(state))
      return js
   }
   def axes_ptr = _js_axes.get(jid, 0)
   def abs_info_ptr = js.get("abs_info_ptr", 0)
   if(!axes_ptr || !abs_info_ptr){
      _trace("abs-skip-ptr jid=" + to_str(jid) + " code=" + to_str(code) + " index=" + to_str(index) + " axes_ptr=" + to_str(axes_ptr) + " abs_info_ptr=" + to_str(abs_info_ptr))
      return js
   }
   if(index >= MAX_JS_AXES){
      _trace("abs-skip-index jid=" + to_str(jid) + " code=" + to_str(code) + " index=" + to_str(index))
      return js
   }
   def info_off = code * INPUT_ABSINFO_SIZE
   def minimum = _signed32(load32(abs_info_ptr, info_off + ABSINFO_MINIMUM))
   def maximum = _signed32(load32(abs_info_ptr, info_off + ABSINFO_MAXIMUM))
   def svalue = _signed32(value)
   mut normalized = float(svalue)
   def range = maximum - minimum
   if(range){
      normalized = (normalized - float(minimum)) / float(range)
      normalized = normalized * 2.0 - 1.0
   }
   if(normalized < -1.0){ normalized = -1.0 }
   elif(normalized > 1.0){ normalized = 1.0 }
   store32_f32(axes_ptr, normalized, index * 4)
   _trace("abs jid=" + to_str(jid) + " code=" + to_str(code) + " index=" + to_str(index) + " value=" + to_str(svalue) + " min=" + to_str(minimum) + " max=" + to_str(maximum) + " norm=" + to_str(normalized))
   js
}

fn _poll_abs_state(any: js): any {
   if(!js){ return js }
   def fd = js.get("fd", -1)
   def abs_map_ptr = js.get("abs_map_ptr", 0)
   def abs_info_ptr = js.get("abs_info_ptr", 0)
   def jid = js.get("jid", -1)
   if(fd < 0 || !abs_map_ptr || !abs_info_ptr){ return js }
   mut code = 0
   while(code < ABS_CNT){
      def mapped_index = load32(abs_map_ptr, code * 4)
      if(mapped_index < MAX_JS_AXES){
         def info_ptr = abs_info_ptr + code * INPUT_ABSINFO_SIZE
         def rc = _ioctl(fd, EVIOCGABS_0 + code, info_ptr)
         _trace("sync-abs jid=" + to_str(jid) + " code=" + to_str(code) + " map=" + to_str(mapped_index) + " rc=" + to_str(rc))
         if(rc >= 0){ js = _handle_abs_event(jid, js, code, _signed32(load32(info_ptr, ABSINFO_VALUE))) }
      }
      code += 1
   }
   js
}

fn _poll_key_state(any: js): any {
   if(!js){ return js }
   def fd = js.get("fd", -1)
   def key_map_ptr = js.get("key_map_ptr", 0)
   def jid = js.get("jid", -1)
   def buttons_ptr = jid >= 0 ? _js_buttons.get(jid, 0) : 0
   if(fd < 0 || !key_map_ptr || !buttons_ptr){ return js }
   def key_state = malloc((KEY_CNT + 7) / 8)
   if(!key_state){ return js }
   memset(key_state, 0, (KEY_CNT + 7) / 8)
   if(_ioctl(fd, EVIOCGKEY_96, key_state) >= 0){
      mut code = BTN_MISC
      while(code < KEY_CNT){
         def index = load32(key_map_ptr, (code - BTN_MISC) * 4)
         if(index >= 0 && index < MAX_JS_BUTTONS){ store8(buttons_ptr, _bit_is_set(key_state, code) ? 1 : 0, index) }
         code += 1
      }
   }
   free(key_state)
   js
}

fn _poll_js_slot(any: js): any {
   if(!js){ return js }
   if(!js.get("use_js", false)){ return js }
   def js_fd = js.get("js_fd", -1)
   def jid = js.get("jid", -1)
   def axes_ptr = jid >= 0 ? _js_axes.get(jid, 0) : 0
   def buttons_ptr = jid >= 0 ? _js_buttons.get(jid, 0) : 0
   def hats_ptr = jid >= 0 ? _js_hats.get(jid, 0) : 0
   def hat_axes_ptr = jid >= 0 ? _js_hat_axes.get(jid, 0) : 0
   if(js_fd < 0 || jid < 0 || !axes_ptr || !buttons_ptr){ return js }
   def event_ptr = malloc(JS_EVENT_SIZE)
   if(!event_ptr){ return js }
   mut event_count = 0
   while(true){
      def nread = _read_fd(js_fd, event_ptr, JS_EVENT_SIZE)
      if(nread < 0){
         if(nread == ENODEV){
            _close_fd(js_fd)
            js["js_fd"] = -1
         }
         break
      }
      if(nread < JS_EVENT_SIZE){ break }
      def typ = load8(event_ptr, 6) & band(bnot(JS_EVENT_INIT), 0xff)
      def num = load8(event_ptr, 7)
      def value = _signed16(load16(event_ptr, 4))
      event_count += 1
      if(typ == JS_EVENT_BUTTON){
         if(num >= 0 && num < js.get("raw_button_count", 0)){ store8(buttons_ptr, value != 0 ? 1 : 0, num) }
         _trace("js-button jid=" + to_str(jid) + " num=" + to_str(num) + " value=" + to_str(value))
      } elif(typ == JS_EVENT_AXIS){
         def axis_count = js.get("axis_count", 0)
         def hat_count = js.get("hat_count", 0)
         if(num >= 0 && num < axis_count){
            def norm = _normalize_js_axis(value)
            store32_f32(axes_ptr, norm, num * 4)
            _trace("js-axis jid=" + to_str(jid) + " num=" + to_str(num) + " value=" + to_str(value) + " norm=" + to_str(norm))
         } elif(num >= axis_count && num < axis_count + hat_count * 2 && hats_ptr && hat_axes_ptr){
            def rel = num - axis_count
            def hat = rel / 2
            def axis = rel % 2
            mut state_part = 0
            if(value < 0){ state_part = 1 }
            elif(value > 0){ state_part = 2 }
            store32(hat_axes_ptr, state_part, (hat * 2 + axis) * 4)
            def x_state, y_state = load32(hat_axes_ptr, (hat * 2 + 0) * 4), load32(hat_axes_ptr, (hat * 2 + 1) * 4)
            def state = _hat_state(x_state, y_state)
            store8(hats_ptr, state, hat)
            _trace("js-hat jid=" + to_str(jid) + " num=" + to_str(num) + " value=" + to_str(value) + " state=" + to_str(state))
         }
      }
   }
   if(event_count > 0 && _is_debug()){
      _dbg("js events jid=" + to_str(jid) +
         " count=" + to_str(event_count) +
         " axes=" + to_str(js.get("axis_count", 0)) +
      " raw_buttons=" + to_str(js.get("raw_button_count", 0)))
   }
   free(event_ptr)
   js
}

fn _hex_nibble(any: n): str {
   def x = int(n) & 0xf
   if(x < 10){ return chr(48 + x) }
   chr(97 + x - 10)
}

fn _hex_byte(any: v): str {
   def x = int(v) & 0xff
   _hex_nibble(x >> 4) + _hex_nibble(x)
}

fn _guid_bytes16(any: data_ptr, int: off): str {
   if(!data_ptr){ return "0000" }
   _hex_byte(load8(data_ptr, off + 0)) + _hex_byte(load8(data_ptr, off + 1))
}

fn _zero_guid_fallback(any: name): str {
   def lname = str.lower(to_str(name))
   if(str.find(lname, "dualsense") != -1 || str.find(lname, "ps5 controller") != -1){ return "050000004c050000e60c000000810000" }
   if(str.find(lname, "xbox one") != -1 || str.find(lname, "xbox wireless") != -1){ return "050000005e040000e002000003090000" }
   if(str.find(lname, "xbox 360") != -1 || str.find(lname, "x360") != -1){ return "030000005e0400001907000000010000" }
   ""
}

fn _build_linux_guid(any: id_ptr, any: device_name): str {
   if(!id_ptr){ return "00000000000000000000000000000000" }
   if(load8(id_ptr, 0) == 0 && load8(id_ptr, 1) == 0 &&
      load8(id_ptr, 2) == 0 && load8(id_ptr, 3) == 0 &&
      load8(id_ptr, 4) == 0 && load8(id_ptr, 5) == 0 &&
      load8(id_ptr, 6) == 0 && load8(id_ptr, 7) == 0){
      def fallback = _zero_guid_fallback(device_name)
      if(fallback != ""){ return fallback }
   }
   def bus = _guid_bytes16(id_ptr, INPUT_ID_BUSTYPE)
   def vendor = _guid_bytes16(id_ptr, INPUT_ID_VENDOR)
   def product = _guid_bytes16(id_ptr, INPUT_ID_PRODUCT)
   def version = _guid_bytes16(id_ptr, INPUT_ID_VERSION)
   def guid = bus + "0000" + vendor + "0000" + product + "0000" + version + "0000"
   guid
}

fn _open_device(str: path): bool {
   if(_find_slot_by_path(path) >= 0){ return false }
   def jid = _find_free_slot()
   if(jid < 0){ return false }
   _dbg("probing path='" + to_str(path) + "'")
   def fd = _open_fd(path, bor(bor(O_RDONLY, O_NONBLOCK), O_CLOEXEC), 0)
   if(fd < 0){
      _dbg("reject path='" + to_str(path) + "' open errno=" + to_str(fd))
      return false
   }
   def ev_bits = malloc((EV_CNT + 7) / 8)
   def key_bits = malloc((KEY_CNT + 7) / 8)
   def abs_bits = malloc((ABS_CNT + 7) / 8)
   def id_ptr = malloc(INPUT_ID_SIZE)
   def name_ptr = malloc(256)
   if(!ev_bits || !key_bits || !abs_bits || !id_ptr || !name_ptr){ return _reject_open_device(fd, -1, ev_bits, key_bits, abs_bits, id_ptr, name_ptr) }
   memset(ev_bits, 0, (EV_CNT + 7) / 8)
   memset(key_bits, 0, (KEY_CNT + 7) / 8)
   memset(abs_bits, 0, (ABS_CNT + 7) / 8)
   memset(id_ptr, 0, INPUT_ID_SIZE)
   memset(name_ptr, 0, 256)
   if(_ioctl(fd, EVIOCGBIT_EV, ev_bits) < 0){
      _dbg("reject path='" + to_str(path) + "' ioctl ev=false")
      return _reject_open_device(fd, -1, ev_bits, key_bits, abs_bits, id_ptr, name_ptr)
   }
   _ioctl(fd, EVIOCGBIT_KEY, key_bits)
   _ioctl(fd, EVIOCGBIT_ABS, abs_bits)
   _ioctl(fd, EVIOCGID, id_ptr)
   if(!_bit_is_set(ev_bits, EV_ABS) && !_bit_is_set(ev_bits, EV_KEY)){
      _dbg("reject path='" + to_str(path) + "' missing EV_ABS and EV_KEY")
      return _reject_open_device(fd, -1, ev_bits, key_bits, abs_bits, id_ptr, name_ptr)
   }
   memset(name_ptr, 0, 256)
   def name = (_ioctl(fd, EVIOCGNAME_256, name_ptr) < 0) ? "Unknown" : str.cstr_to_str(name_ptr)
   def controller_aux = _is_controller_aux_input(name)
   mut guid = _build_linux_guid(id_ptr, name)
   def fallback_guid = _zero_guid_fallback(name)
   if(fallback_guid != "" && (guid == "00000000000000000000000000000000" || guid == "0000000000000000" || guid.len < 32)){ guid = fallback_guid }
   def key_map_bytes = (KEY_CNT - BTN_MISC) * 4
   def abs_map_bytes = ABS_CNT * 4
   def key_map_ptr = malloc(key_map_bytes)
   def abs_map_ptr = malloc(abs_map_bytes)
   def abs_info_ptr = malloc(ABS_CNT * INPUT_ABSINFO_SIZE)
   if(!key_map_ptr || !abs_map_ptr || !abs_info_ptr){
      _dbg("reject path='" + to_str(path) + "' alloc failure")
      return _reject_open_device(fd, -1, ev_bits, key_bits, abs_bits, id_ptr, name_ptr, key_map_ptr, abs_map_ptr, abs_info_ptr)
   }
   memset(key_map_ptr, 255, key_map_bytes)
   memset(abs_map_ptr, 255, abs_map_bytes)
   mut raw_button_count = 0
   mut axis_count = 0
   mut hat_count = 0
   mut code = BTN_MISC
   while(code < KEY_CNT){
      if(_bit_is_set(key_bits, code) &&
         ((code >= BTN_JOYSTICK && code < BTN_DIGI) || controller_aux)){
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
            if(_ioctl(fd, EVIOCGABS_0 + code, info_ptr) >= 0){
               store32(abs_map_ptr, axis_count, code * 4)
               _trace("map-axis path='" + to_str(path) + "' code=" + to_str(code) + " index=" + to_str(axis_count))
               axis_count += 1
            }
         }
      }
      code += 1
   }
   def button_count = raw_button_count + hat_count * 4
   if(axis_count <= 0 && button_count <= 0 && hat_count <= 0){
      _dbg("reject path='" + to_str(path) + "' no usable axes/buttons/hats raw_buttons=" + to_str(raw_button_count) +
      " axis_count=" + to_str(axis_count) + " hat_count=" + to_str(hat_count))
      return _reject_open_device(fd, -1, ev_bits, key_bits, abs_bits, id_ptr, name_ptr, key_map_ptr, abs_map_ptr, abs_info_ptr)
   }
   if(!_reset_js_buffers(jid)){
      _dbg("reject path='" + to_str(path) + "' joystick buffer alloc failure")
      return _reject_open_device(fd, -1, ev_bits, key_bits, abs_bits, id_ptr, name_ptr, key_map_ptr, abs_map_ptr, abs_info_ptr)
   }
   def axes_ptr = _js_axes.get(jid, 0)
   def buttons_ptr = _js_buttons.get(jid, 0)
   def hats_ptr = _js_hats.get(jid, 0)
   def use_js = axis_count <= 0 || button_count <= 0
   def js_dev = use_js ? _open_js_fd_for_name(name) : [-1, 0, 0]
   def js_fd = js_dev.get(0, -1)
   mut js = {
      "jid": jid,
      "connected": true,
      "fd": fd,
      "js_fd": js_fd,
      "js_axes_count": js_dev.get(1, 0),
      "js_button_count": js_dev.get(2, 0),
      "path": path,
      "name": name,
      "guid": guid,
      "platform": "Linux",
      "axis_count": axis_count,
      "raw_button_count": raw_button_count,
      "button_count": button_count,
      "hat_count": hat_count,
      "controller_aux": controller_aux,
      "use_js": use_js,
      "axes_ptr": axes_ptr,
      "buttons_ptr": buttons_ptr,
      "hats_ptr": hats_ptr,
      "key_map_ptr": key_map_ptr,
      "abs_map_ptr": abs_map_ptr,
      "abs_info_ptr": abs_info_ptr,
      "last_state_sync_ticks": 0,
      "last_js_open_ticks": js_fd >= 0 ? ticks() : 0
   }
   def mapped = gamepad_map.joystick_is_gamepad(js)
   if(!mapped && !controller_aux && raw_button_count <= 0 && hat_count <= 0){
      _dbg("reject path='" + to_str(path) + "' no joystick/gamepad buttons")
      return _reject_open_device(fd, js_fd, ev_bits, key_bits, abs_bits, id_ptr, name_ptr, key_map_ptr, abs_map_ptr, abs_info_ptr)
   }
   if(!mapped && !controller_aux && _looks_like_aux_input(name)){
      _dbg("reject path='" + to_str(path) + "' auxiliary input name='" + to_str(name) + "'")
      return _reject_open_device(fd, js_fd, ev_bits, key_bits, abs_bits, id_ptr, name_ptr, key_map_ptr, abs_map_ptr, abs_info_ptr)
   }
   js = _poll_key_state(js)
   js = _poll_abs_state(js)
   js = _poll_js_slot(js)
   js["last_state_sync_ticks"] = ticks()
   _put_js(jid, js)
   _dbg("connected jid=" + to_str(jid) +
      " path='" + to_str(path) + "'" +
      " name='" + to_str(name) + "'" +
      " guid='" + to_str(guid) + "'" +
      " axes=" + to_str(axis_count) +
      " buttons=" + to_str(button_count) +
      " hats=" + to_str(hat_count) +
      " js_fd=" + to_str(js_dev.get(0, -1)) +
      " js_axes=" + to_str(js_dev.get(1, 0)) +
   " js_buttons=" + to_str(js_dev.get(2, 0)))
   _free_probe_allocs(ev_bits, key_bits, abs_bits, id_ptr, name_ptr)
   _invoke_callback(jid, backend_api.CONNECTED)
   true
}

fn _mark_scan_now(): any { _set_js_val("last_scan_ticks", ticks()) }

fn _scan_devices(): int {
   if(!_linux_input_supported()){ return 0 }
   def names = osfs.list_dir("/dev/input")
   mut i = 0
   def names_n = names.len
   while(i < names_n){
      def name = names.get(i)
      if(_is_event_device_name(name)){ _open_device("/dev/input/" + to_str(name)) }
      i += 1
   }
   _mark_scan_now()
   0
}

fn _maybe_rescan_devices(bool: force=false): int {
   if(!_linux_input_supported()){ return 0 }
   def now = ticks()
   def last = _get_js_val("last_scan_ticks", 0)
   if(force || last <= 0 || (now - last) >= DEVICE_RESCAN_INTERVAL_NS){ _scan_devices() }
   0
}

fn _detect_connections(): int {
   def inotify_fd = _get_js_val("inotify_fd", -1)
   if(inotify_fd < 0){ return _maybe_rescan_devices() }
   def buffer = malloc(16384)
   if(!buffer){ return _maybe_rescan_devices() }
   mut saw_candidate = false
   def size = _read_fd(inotify_fd, buffer, 16384)
   if(size == EAGAIN){
      free(buffer)
      return 0
   }
   if(size == 0){
      free(buffer)
      return 0
   }
   if(size < 0){
      free(buffer)
      return _maybe_rescan_devices()
   }
   mut offset = 0
   while(offset + INOTIFY_EVENT_SIZE <= size){
      def event_ptr = buffer + offset
      def mask = load32(event_ptr, INOTIFY_MASK)
      def name_len = load32(event_ptr, INOTIFY_LEN)
      def name = str.cstr_to_str(event_ptr + INOTIFY_NAME)
      if(_is_event_device_name(name)){
         def path = "/dev/input/" + name
         if(mask & bor(bor(IN_CREATE, IN_ATTRIB), bor(IN_CLOSE_WRITE, IN_MOVED_TO))){
            saw_candidate = true
            _dbg("inotify create/attrib/move path='" + to_str(path) + "'")
            _open_device(path)
         }
         elif(mask & IN_DELETE){
            def jid = _find_slot_by_path(path)
            if(jid >= 0){ _disconnect_js(jid) }
         }
      }
      offset += INOTIFY_EVENT_SIZE + name_len
   }
   free(buffer)
   _maybe_rescan_devices(saw_candidate)
   0
}

fn _poll_slot(int: jid): bool {
   mut js = _get_js(jid)
   if(!js || !js.get("connected", false)){ return false }
   js = _maybe_attach_js_fd(js)
   def fd = js.get("fd", -1)
   if(fd < 0){
      _disconnect_js(jid)
      return false
   }
   def event_ptr = malloc(INPUT_EVENT_SIZE)
   if(!event_ptr){ return true }
   mut event_count = 0
   while(true){
      def nread = _read_fd(fd, event_ptr, INPUT_EVENT_SIZE)
      if(nread < 0){
         if(nread == ENODEV){
            free(event_ptr)
            _disconnect_js(jid)
            return false
         }
         break
      }
      if(nread < INPUT_EVENT_SIZE){ break }
      def typ = load16(event_ptr, INPUT_EVENT_TYPE)
      def code = load16(event_ptr, INPUT_EVENT_CODE)
      def value = _signed32(load32(event_ptr, INPUT_EVENT_VALUE))
      event_count += 1
      if(typ == EV_SYN){
         if(code == SYN_DROPPED){ _set_js_val("dropped", true) } elif(code == SYN_REPORT){
            _set_js_val("dropped", false)
            js = _poll_abs_state(js)
         }
      }
      if(_get_js_val("dropped", false)){ continue }
      if(typ == EV_KEY){ js = _handle_key_event(jid, js, code, value) } elif(typ == EV_ABS){ js = _handle_abs_event(jid, js, code, value) }
   }
   def now = ticks()
   def last_sync = js.get("last_state_sync_ticks", 0)
   if(last_sync <= 0 || (now - last_sync) >= JOYSTICK_STATE_SYNC_INTERVAL_NS){
      js = _poll_key_state(js)
      js = _poll_abs_state(js)
      js["last_state_sync_ticks"] = now
   }
   js = _poll_js_slot(js)
   if(event_count > 0 && _is_debug()){
      _dbg("evdev events jid=" + to_str(jid) +
         " count=" + to_str(event_count) +
      " path='" + to_str(js.get("path", "")) + "'")
   }
   free(event_ptr)
   _put_js(jid, js)
   true
}

fn _ensure_init(): bool {
   def initialized = _get_js_val("initialized", false)
   if(initialized || !_linux_input_supported()){ return initialized }
   _set_js_val("initialized", true)
   gamepad_map.init_default_linux_mappings()
   def inotify_fd = inotify_init1(bor(IN_NONBLOCK, IN_CLOEXEC))
   _set_js_val("inotify_fd", inotify_fd)
   if(inotify_fd >= 0){
      def watch = inotify_add_watch(inotify_fd, cstr("/dev/input"),
      bor(bor(bor(IN_CREATE, IN_ATTRIB), bor(IN_CLOSE_WRITE, IN_MOVED_TO)), IN_DELETE))
      _set_js_val("inotify_watch", watch)
   }
   _scan_devices()
   true
}

fn init(): bool {
   "Initializes native Linux joystick handling."
   _ensure_init()
}

fn terminate(): bool {
   "Shuts down native Linux joystick handling."
   if(!_get_js_val("initialized", false)){ return true }
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(_get_js(jid)){ _free_js(jid) }
      jid += 1
   }
   def inotify_fd = _get_js_val("inotify_fd", -1)
   def inotify_watch = _get_js_val("inotify_watch", -1)
   if(inotify_fd >= 0){
      if(inotify_watch >= 0){ inotify_rm_watch(inotify_fd, inotify_watch) }
      _close_fd(inotify_fd)
   }
   _set_js_val("joysticks", dict(8))
   _set_js_val("inotify_fd", -1)
   _set_js_val("inotify_watch", -1)
   _set_js_val("dropped", false)
   _set_js_val("last_poll_ticks", 0)
   _set_js_val("initialized", false)
   true
}

fn poll_joysticks(): bool {
   "Polls connection changes and queued joystick events."
   if(!_ensure_init()){ return false }
   def now = ticks()
   def last_poll = _get_js_val("last_poll_ticks", 0)
   if(last_poll > 0 && (now - last_poll) < JOYSTICK_POLL_COALESCE_NS){ return true }
   _set_js_val("last_poll_ticks", now)
   _detect_connections()
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(_get_js(jid)){ _poll_slot(jid) }
      jid += 1
   }
   true
}

fn joystick_present(int: jid): bool {
   if(!_ensure_init() || jid < 0 || jid >= MAX_JOYSTICKS){ return false }
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
   _poll_slot(jid)
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, js.get("axis_count", 0), 0) }
   _js_axes.get(jid, 0)
}

fn get_joystick_buttons(int: jid, any: count_ptr): any {
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   _poll_slot(jid)
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, js.get("button_count", 0), 0) }
   _js_buttons.get(jid, 0)
}

fn get_joystick_hats(int: jid, any: count_ptr): any {
   if(count_ptr){ store32(count_ptr, 0, 0) }
   if(!joystick_present(jid)){ return 0 }
   _poll_slot(jid)
   def js = _get_js(jid)
   if(count_ptr){ store32(count_ptr, js.get("hat_count", 0), 0) }
   _js_hats.get(jid, 0)
}

fn _raw_button(any: buttons_ptr, int: button_count, int: idx): int {
   (buttons_ptr && idx >= 0 && idx < button_count) ? load8(buttons_ptr, idx) : 0
}

fn _raw_axis(any: axes_ptr, int: axis_count, int: idx): f64 {
   (axes_ptr && idx >= 0 && idx < axis_count) ? load32_f32(axes_ptr, idx * 4) : 0.0
}

fn _store_standard_button(any: state_ptr, int: dst, any: buttons_ptr, int: button_count, int: src): any {
   store8(state_ptr, _raw_button(buttons_ptr, button_count, src), dst)
}

fn _fill_raw_gamepad_state(int: jid, any: state_ptr): bool {
   if(!state_ptr || jid < 0 || jid >= MAX_JOYSTICKS){ return false }
   memset(state_ptr, 0, 64)
   def js = _get_js(jid)
   if(!js){ return false }
   def axes_ptr = _js_axes.get(jid, 0)
   def buttons_ptr = _js_buttons.get(jid, 0)
   def hats_ptr = _js_hats.get(jid, 0)
   def axis_count = int(js.get("axis_count", 0))
   def button_count = int(js.get("button_count", 0))
   def hat_count = int(js.get("hat_count", 0))
   _store_standard_button(state_ptr, 0, buttons_ptr, button_count, 0)
   _store_standard_button(state_ptr, 1, buttons_ptr, button_count, 1)
   _store_standard_button(state_ptr, 2, buttons_ptr, button_count, 3)
   _store_standard_button(state_ptr, 3, buttons_ptr, button_count, 2)
   _store_standard_button(state_ptr, 4, buttons_ptr, button_count, 4)
   _store_standard_button(state_ptr, 5, buttons_ptr, button_count, 5)
   _store_standard_button(state_ptr, 6, buttons_ptr, button_count, 8)
   _store_standard_button(state_ptr, 7, buttons_ptr, button_count, 9)
   _store_standard_button(state_ptr, 8, buttons_ptr, button_count, 10)
   _store_standard_button(state_ptr, 9, buttons_ptr, button_count, 11)
   _store_standard_button(state_ptr, 10, buttons_ptr, button_count, 12)
   if(hats_ptr && hat_count > 0){
      def hat = load8(hats_ptr, 0)
      store8(state_ptr, (hat & HAT_UP) != 0 ? 1 : load8(state_ptr, 11), 11)
      store8(state_ptr, (hat & HAT_RIGHT) != 0 ? 1 : load8(state_ptr, 12), 12)
      store8(state_ptr, (hat & HAT_DOWN) != 0 ? 1 : load8(state_ptr, 13), 13)
      store8(state_ptr, (hat & HAT_LEFT) != 0 ? 1 : load8(state_ptr, 14), 14)
   }
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 0), 16)
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 1), 20)
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 3), 24)
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 4), 28)
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 2), 32)
   store32_f32(state_ptr, _raw_axis(axes_ptr, axis_count, 5), 36)
   true
}

fn joystick_is_gamepad(int: jid): bool {
   if(!joystick_present(jid)){ return false }
   def js = _get_js(jid)
   if(js.get("controller_aux", false)){ return false }
   if(gamepad_map.joystick_is_gamepad(js)){ return true }
   int(js.get("axis_count", 0)) >= 2 && int(js.get("button_count", 0)) >= 4
}

fn get_gamepad_state(int: jid, any: state_ptr): bool {
   if(!joystick_present(jid)){ if(state_ptr){ memset(state_ptr, 0, 64) } return false }
   _poll_slot(jid)
   _fill_raw_gamepad_state(jid, state_ptr)
}

fn get_gamepad_name(int: jid): str {
   if(!joystick_present(jid)){ return "Unknown" }
   def name = gamepad_map.get_gamepad_name(_get_js(jid))
   if(name && name.len > 0){ return name }
   get_joystick_name(jid)
}

fn set_joystick_callback(any: cb): any {
   def prev = _get_js_val("joystick_callback", 0)
   _set_js_val("joystick_callback", cb)
   prev
}

fn update_gamepad_mappings(str: s): int { gamepad_map.update_mappings(s, "Linux") }
