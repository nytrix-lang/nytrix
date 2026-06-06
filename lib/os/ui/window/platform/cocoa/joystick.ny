;; Keywords: platform window backend cocoa macos joystick os ui input
;; Native macOS IOKit/HID joystick backend for Nytrix.
;; References:
;; - std.os.ui.window.platform.cocoa
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform.cocoa.joystick(init, terminate, poll_joysticks, joystick_present, get_joystick_name, get_joystick_guid, get_joystick_axes, get_joystick_buttons, joystick_is_gamepad, get_gamepad_state, get_gamepad_name, set_joystick_callback, update_gamepad_mappings)
use std.core
use std.core.mem
use std.core.str as str
use std.os.ui.window.platform.api as backend_api
use std.os.ui.window.platform.gamepad_map as gamepad_map

def MAX_JOYSTICKS = 16
def MAX_ELEMENTS  = 128
def kHIDPage_GenericDesktop = 1
def kHIDPage_Button         = 9
def kHIDUsage_GD_X          = 0x30
def kHIDUsage_GD_Y          = 0x31
def kHIDUsage_GD_Z          = 0x32
def kHIDUsage_GD_Rx         = 0x33
def kHIDUsage_GD_Ry         = 0x34
def kHIDUsage_GD_Rz         = 0x35
def kHIDUsage_GD_Slider     = 0x36
def kHIDUsage_GD_Hatswitch  = 0x39
def kHIDUsage_GD_Joystick            = 4
def kHIDUsage_GD_GamePad             = 5
def kHIDUsage_GD_MultiAxisController = 8
def kCFAllocatorDefault  = 0
def kCFNumberSInt32Type  = 3
def kIOHIDOptionsTypeNone = 0
def kCFStringEncodingUTF8 = 0x08000100
def kCFRunLoopRunTimedOut = 3
def kIOReturnSuccess = 0
def kCFTypeArrayCallBacks    = 0
def kCFTypeDictionaryKeyCallBacks   = 0
def kCFTypeDictionaryValueCallBacks = 0
#macos {
   #link "-framework CoreFoundation"
   #link "-framework IOKit"
   #include <IOKit/IOKitLib.h>
   #include <IOKit/hid/IOHIDLib.h>
   #include <CoreFoundation/CoreFoundation.h>
   extern "" {
      fn CFArrayCreate(any alloc, any values, int count, any callbacks) any
      fn CFArrayGetCount(any arr) int
      fn CFArrayGetValueAtIndex(any arr, int idx) any
      fn CFDictionaryCreateMutable(any alloc, int cap, any keys, any values) any
      fn CFDictionarySetValue(any dict, any key, any value) any
      fn CFNumberCreate(any alloc, int typ, any value) any
      fn CFNumberGetValue(any num, int typ, any out) int
      fn CFRelease(any obj) any
      fn CFRunLoopGetCurrent() any
      fn CFRunLoopRunInMode(any mode, f64 seconds, int return_after_source) int
      fn CFStringCreateWithCString(any alloc, any s, int enc) any
      fn CFStringGetCString(any s, any buf, int cap, int enc) int
      fn IOHIDDeviceCopyMatchingElements(any device, any matching, int options) any
      fn IOHIDDeviceGetProperty(any device, any key) any
      fn IOHIDDeviceGetValue(any device, any element, any out) int
      fn IOHIDElementGetLogicalMax(any elem) int
      fn IOHIDElementGetLogicalMin(any elem) int
      fn IOHIDElementGetUsage(any elem) int
      fn IOHIDElementGetUsagePage(any elem) int
      fn IOHIDManagerClose(any mgr, int options) int
      fn IOHIDManagerCreate(any alloc, int options) any
      fn IOHIDManagerOpen(any mgr, int options) int
      fn IOHIDManagerRegisterDeviceMatchingCallback(any mgr, any cb, any ctx) any
      fn IOHIDManagerRegisterDeviceRemovalCallback(any mgr, any cb, any ctx) any
      fn IOHIDManagerScheduleWithRunLoop(any mgr, any loop, any mode) any
      fn IOHIDManagerSetDeviceMatchingMultiple(any mgr, any arr) any
      fn IOHIDManagerUnscheduleFromRunLoop(any mgr, any loop, any mode) any
      fn IOHIDValueGetIntegerValue(any value) int
   }
} #else {
   "Runs the IOHIDValueGetIntegerValue operation."
   fn CFArrayCreate(any _alloc, any _values, int _count, any _callbacks) any {
      "Runs the CFArrayCreate operation."
      0
   }
   fn CFArrayGetCount(any _arr) int {
      "Runs the CFArrayGetCount operation."
      0
   }
   fn CFArrayGetValueAtIndex(any _arr, int _idx) any {
      "Runs the CFArrayGetValueAtIndex operation."
      0
   }
   fn CFDictionaryCreateMutable(any _alloc, int _cap, any _keys, any _values) any {
      "Runs the CFDictionaryCreateMutable operation."
      0
   }
   fn CFDictionarySetValue(any _dict, any _key, any _value) any {
      "Runs the CFDictionarySetValue operation."
      0
   }
   fn CFNumberCreate(any _alloc, int _typ, any _value) any {
      "Runs the CFNumberCreate operation."
      0
   }
   fn CFNumberGetValue(any _num, int _typ, any _out) int {
      "Runs the CFNumberGetValue operation."
      0
   }
   fn CFRelease(any _obj) any {
      "Runs the CFRelease operation."
      0
   }
   fn CFRunLoopGetCurrent() any {
      "Runs the CFRunLoopGetCurrent operation."
      0
   }
   fn CFRunLoopRunInMode(any _mode, f64 _seconds, int _return_after_source) int {
      "Runs the CFRunLoopRunInMode operation."
      kCFRunLoopRunTimedOut
   }
   fn CFStringCreateWithCString(any _alloc, any _s, int _enc) any {
      "Runs the CFStringCreateWithCString operation."
      0
   }
   fn CFStringGetCString(any _s, any _buf, int _cap, int _enc) int {
      "Runs the CFStringGetCString operation."
      0
   }
   fn IOHIDDeviceCopyMatchingElements(any _device, any _matching, int _options) any {
      "Runs the IOHIDDeviceCopyMatchingElements operation."
      0
   }
   fn IOHIDDeviceGetProperty(any _device, any _key) any {
      "Runs the IOHIDDeviceGetProperty operation."
      0
   }
   fn IOHIDDeviceGetValue(any _device, any _element, any _out) int {
      "Runs the IOHIDDeviceGetValue operation."
      -1
   }
   fn IOHIDElementGetLogicalMax(any _elem) int {
      "Runs the IOHIDElementGetLogicalMax operation."
      0
   }
   fn IOHIDElementGetLogicalMin(any _elem) int {
      "Runs the IOHIDElementGetLogicalMin operation."
      0
   }
   fn IOHIDElementGetUsage(any _elem) int {
      "Runs the IOHIDElementGetUsage operation."
      0
   }
   fn IOHIDElementGetUsagePage(any _elem) int {
      "Runs the IOHIDElementGetUsagePage operation."
      0
   }
   fn IOHIDManagerClose(any _mgr, int _options) int {
      "Runs the IOHIDManagerClose operation."
      0
   }
   fn IOHIDManagerCreate(any _alloc, int _options) any {
      "Runs the IOHIDManagerCreate operation."
      0
   }
   fn IOHIDManagerOpen(any _mgr, int _options) int {
      "Runs the IOHIDManagerOpen operation."
      -1
   }
   fn IOHIDManagerRegisterDeviceMatchingCallback(any _mgr, any _cb, any _ctx) any {
      "Runs the IOHIDManagerRegisterDeviceMatchingCallback operation."
      0
   }
   fn IOHIDManagerRegisterDeviceRemovalCallback(any _mgr, any _cb, any _ctx) any {
      "Runs the IOHIDManagerRegisterDeviceRemovalCallback operation."
      0
   }
   fn IOHIDManagerScheduleWithRunLoop(any _mgr, any _loop, any _mode) any {
      "Runs the IOHIDManagerScheduleWithRunLoop operation."
      0
   }
   fn IOHIDManagerSetDeviceMatchingMultiple(any _mgr, any _arr) any {
      "Runs the IOHIDManagerSetDeviceMatchingMultiple operation."
      0
   }
   fn IOHIDManagerUnscheduleFromRunLoop(any _mgr, any _loop, any _mode) any {
      "Runs the IOHIDManagerUnscheduleFromRunLoop operation."
      0
   }
   fn IOHIDValueGetIntegerValue(any _value) int {
      "Runs the IOHIDValueGetIntegerValue operation."
      0
   }
}

mut _initialized = false
mut _hid_manager  = 0
mut _run_loop     = 0
mut _run_loop_mode = 0
mut _joysticks    = dict(8)
mut _joystick_callback = 0
mut _key_kIOHIDProductKey       = 0
mut _key_kIOHIDVendorIDKey      = 0
mut _key_kIOHIDProductIDKey     = 0
mut _key_kIOHIDVersionNumberKey = 0
mut _key_kIOHIDManufacturerKey  = 0

fn _has_native_support() bool {
   #macos { return true }
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
   def ap, bp = js.get("axes_ptr", 0), js.get("buttons_ptr", 0)
   def ep = js.get("elements_ptr", 0)
   if(ap){ free(ap) }
   if(bp){ free(bp) }
   if(ep){ free(ep) }
   _joysticks = _joysticks.set(jid, 0)
   true
}

fn _find_free_slot() int {
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(!_get_js(jid)){ return jid }
      jid += 1
   }
   -1
}

fn _find_slot_by_device(any device) int {
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      def js = _get_js(jid)
      if(js && js.get("device", 0) == device){ return jid }
      jid += 1
   }
   -1
}

fn _hex_nibble_c(any n) str {
   def x = int(n) & 0xf
   if(x < 10){ return chr(48 + x) }
   chr(97 + x - 10)
}

fn _hex_byte_c(any v) str {
   def x = int(v) & 0xff
   _hex_nibble_c(x >> 4) + _hex_nibble_c(x)
}

fn _build_guid(any vid, any pid, any ver) str {
   def bus = 3
   _hex_byte_c(bus & 0xff) + _hex_byte_c((bus >> 8) & 0xff) + "0000" +
   _hex_byte_c(vid & 0xff) + _hex_byte_c((vid >> 8) & 0xff) + "0000" +
   _hex_byte_c(pid & 0xff) + _hex_byte_c((pid >> 8) & 0xff) + "0000" +
   _hex_byte_c(ver & 0xff) + _hex_byte_c((ver >> 8) & 0xff) + "0000"
}

fn _invoke_callback(int jid, int event) any {
   def cb = _joystick_callback
   if(cb){ cb(jid, event) }
}

fn _cf_str(str s) any { CFStringCreateWithCString(0, cstr(s), kCFStringEncodingUTF8) }

fn _cf_int(any v) any {
   def p = malloc(8)
   if(!p){ return 0 }
   store64_h(p, v, 0)
   def n = CFNumberCreate(0, kCFNumberSInt32Type, p)
   free(p)
   n
}

fn _cf_str_to_str(any cf) str {
   if(!cf){ return "" }
   def buf = malloc(512)
   if(!buf){ return "" }
   CFStringGetCString(cf, buf, 512, kCFStringEncodingUTF8)
   def s = to_str(buf)
   free(buf)
   s
}

fn _init_keys() any {
   if(_key_kIOHIDProductKey){ return nil }
   _key_kIOHIDProductKey       = _cf_str("Product")
   _key_kIOHIDVendorIDKey      = _cf_str("VendorID")
   _key_kIOHIDProductIDKey     = _cf_str("ProductID")
   _key_kIOHIDVersionNumberKey = _cf_str("VersionNumber")
   _key_kIOHIDManufacturerKey  = _cf_str("Manufacturer")
}

fn _matching_dict_for_usage(int usage_page, int usage) any {
   def dict = CFDictionaryCreateMutable(0, 0, 0, 0)
   if(!dict){ return 0 }
   def page_num = _cf_int(usage_page)
   def usage_num = _cf_int(usage)
   def page_key = _cf_str("DeviceUsagePage")
   def usage_key = _cf_str("DeviceUsage")
   CFDictionarySetValue(dict, page_key, page_num)
   CFDictionarySetValue(dict, usage_key, usage_num)
   CFRelease(page_num)
   CFRelease(usage_num)
   CFRelease(page_key)
   CFRelease(usage_key)
   dict
}

fn _cf_num_to_int(any cf_num) int {
   if(!cf_num){ return 0 }
   def p = malloc(8)
   if(!p){ return 0 }
   CFNumberGetValue(cf_num, kCFNumberSInt32Type, p)
   def v = load32(p, 0)
   free(p)
   v
}

fn _device_connected(any ctx, any result, any sender, any device) any {
   if(!device){ return nil }
   def jid = _find_free_slot()
   if(jid < 0){ return nil }
   _init_keys()
   def prod_cf  = IOHIDDeviceGetProperty(device, _key_kIOHIDProductKey)
   def vid_cf   = IOHIDDeviceGetProperty(device, _key_kIOHIDVendorIDKey)
   def pid_cf   = IOHIDDeviceGetProperty(device, _key_kIOHIDProductIDKey)
   def ver_cf   = IOHIDDeviceGetProperty(device, _key_kIOHIDVersionNumberKey)
   def name = prod_cf ? _cf_str_to_str(prod_cf) : "Unknown Gamepad"
   def vid  = _cf_num_to_int(vid_cf)
   def pid  = _cf_num_to_int(pid_cf)
   def ver  = _cf_num_to_int(ver_cf)
   def guid = _build_guid(vid, pid, ver)
   def elems = IOHIDDeviceCopyMatchingElements(device, 0, 0)
   if(!elems){ return nil }
   def count = CFArrayGetCount(elems)
   def elem_arr = malloc(count * 24)
   if(!elem_arr){ CFRelease(elems) return nil }
   memset(elem_arr, 0, count * 24)
   mut axis_count   = 0
   mut button_count = 0
   mut hat_count    = 0
   mut i = 0
   while(i < count){
      def elem = CFArrayGetValueAtIndex(elems, i)
      def page  = IOHIDElementGetUsagePage(elem)
      def usage = IOHIDElementGetUsage(elem)
      if(page == kHIDPage_GenericDesktop && usage >= kHIDUsage_GD_X && usage <= kHIDUsage_GD_Rz){ axis_count += 1 } elif(page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Slider){
         axis_count += 1
      } elif(page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Hatswitch){
         hat_count += 1
      } elif(page == kHIDPage_Button){
         button_count += 1
      }
      i += 1
   }
   def total_buttons = button_count + hat_count * 4
   def axes_ptr    = malloc(axis_count * 4 + 4)
   if(!axes_ptr){
      free(elem_arr)
      CFRelease(elems)
      return nil
   }
   def buttons_ptr = malloc(total_buttons + 1)
   if(!buttons_ptr){
      free(axes_ptr, elem_arr)
      CFRelease(elems)
      return nil
   }
   memset(axes_ptr, 0, axis_count * 4 + 4)
   memset(buttons_ptr, 0, total_buttons + 1)
   mut axis_idx   = 0
   mut button_idx = 0
   mut hat_idx    = 0
   i = 0
   while(i < count){
      def elem  = CFArrayGetValueAtIndex(elems, i)
      def page  = IOHIDElementGetUsagePage(elem)
      def usage = IOHIDElementGetUsage(elem)
      def lmin  = IOHIDElementGetLogicalMin(elem)
      def lmax  = IOHIDElementGetLogicalMax(elem)
      def is_axis = (page == kHIDPage_GenericDesktop &&
      ((usage >= kHIDUsage_GD_X && usage <= kHIDUsage_GD_Rz) || usage == kHIDUsage_GD_Slider))
      def is_hat  = (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Hatswitch)
      def is_btn  = (page == kHIDPage_Button)
      if(is_axis){
         def off = axis_idx * 24
         store64_h(elem_arr, elem,    off)
         store32(elem_arr, 1,           off + 8)
         store32(elem_arr, axis_idx,    off + 12)
         store64_h(elem_arr, lmin,    off + 16)
         store32(elem_arr, int(lmax),   off + 16)
         store32(elem_arr, int(lmin),   off + 12)
         store32(elem_arr, int(lmax),   off + 16)
         axis_idx += 1
      } elif(is_hat){
         def hat_off = (axis_count + button_count + hat_idx) * 24
         store64_h(elem_arr, elem,    hat_off)
         store32(elem_arr, 3,           hat_off + 8)
         store32(elem_arr, hat_idx,     hat_off + 12)
         store32(elem_arr, int(lmin),   hat_off + 16)
         store32(elem_arr, int(lmax),   hat_off + 20)
         hat_idx += 1
      } elif(is_btn){
         def off = (axis_count + button_idx) * 24
         store64_h(elem_arr, elem,    off)
         store32(elem_arr, 2,           off + 8)
         store32(elem_arr, button_idx,  off + 12)
         button_idx += 1
      }
      i += 1
   }
   CFRelease(elems)
   def js = {
      "device": device,
      "connected": true,
      "name": name,
      "guid": guid,
      "platform": "Mac OS X",
      "axes_ptr": axes_ptr,
      "buttons_ptr": buttons_ptr,
      "axis_count": axis_count,
      "button_count": total_buttons,
      "raw_button_count": button_count,
      "hat_count": hat_count,
      "elements_ptr": elem_arr,
      "element_count": axis_count + button_count + hat_count
   }
   _put_js(jid, js)
   _invoke_callback(jid, backend_api.CONNECTED)
}

fn _device_removed(any ctx, any result, any sender, any device) any {
   def jid = _find_slot_by_device(device)
   if(jid < 0){ return nil }
   _invoke_callback(jid, backend_api.DISCONNECTED)
   _free_js(jid)
}

fn _hat_buttons(any value, any lmin, any lmax) int {
   def range = lmax - lmin
   if(range == 0){ return 0 }
   def v = int(value) - int(lmin)
   if(v < 0 || v > 8){ return 0 }
   def up    = (v == 0 || v == 1 || v == 7) ? 1 : 0
   def right = (v == 1 || v == 2 || v == 3) ? 1 : 0
   def down  = (v == 3 || v == 4 || v == 5) ? 1 : 0
   def left  = (v == 5 || v == 6 || v == 7) ? 1 : 0
   up | (right << 1) | (down << 2) | (left << 3)
}

fn _poll_js(int jid) any {
   def js = _get_js(jid)
   if(!js){ return nil }
   def device = js.get("device", 0)
   if(!device){ return nil }
   def elem_arr   = js.get("elements_ptr", 0)
   def axis_count = js.get("axis_count", 0)
   def btn_count  = js.get("raw_button_count", 0)
   def hat_count  = js.get("hat_count", 0)
   def axes_ptr   = js.get("axes_ptr", 0)
   def btns_ptr   = js.get("buttons_ptr", 0)
   if(!elem_arr || !axes_ptr || !btns_ptr){ return nil }
   def total = axis_count + btn_count + hat_count
   def val_ptr = malloc(8)
   if(!val_ptr){ return nil }
   mut i = 0
   while(i < total){
      def off  = i * 24
      def elem = load64_h(elem_arr, off)
      def etype = load32(elem_arr, off + 8)
      def eidx  = load32(elem_arr, off + 12)
      def lmin  = load32(elem_arr, off + 16)
      def lmax  = load32(elem_arr, off + 20)
      if(!elem){ i += 1 continue }
      def ret = IOHIDDeviceGetValue(device, elem, val_ptr)
      if(ret == kIOReturnSuccess){
         def value_ref = load64_h(val_ptr, 0)
         def raw = value_ref ? IOHIDValueGetIntegerValue(value_ref) : 0
         if(etype == 1){
            def range = lmax - lmin
            def f = range > 0 ? (float(raw - lmin) / float(range)) * 2.0 - 1.0 : 0.0
            store32_f32(axes_ptr, f, eidx * 4)
         } elif(etype == 2){
            store8(btns_ptr, raw ? 1 : 0, eidx)
         } elif(etype == 3){
            def bits = _hat_buttons(raw, lmin, lmax)
            def base = btn_count + eidx * 4
            store8(btns_ptr, band(bits, 1) ? 1 : 0, base + 0)
            store8(btns_ptr, band(bits, 2) ? 1 : 0, base + 1)
            store8(btns_ptr, band(bits, 4) ? 1 : 0, base + 2)
            store8(btns_ptr, band(bits, 8) ? 1 : 0, base + 3)
         }
      }
      i += 1
   }
   free(val_ptr)
}

fn init() bool {
   "Initializes init."
   if(_initialized){ return _initialized }
   if(!_has_native_support()){ return false }
   gamepad_map.init_default_macos_mappings()
   def rl = CFRunLoopGetCurrent()
   def rl_mode = CFStringCreateWithCString(0, cstr("kCFRunLoopDefaultMode"), kCFStringEncodingUTF8)
   _run_loop = rl
   _run_loop_mode = rl_mode
   def mgr = IOHIDManagerCreate(0, kIOHIDOptionsTypeNone)
   if(!mgr){ return false }
   def m1 = _matching_dict_for_usage(kHIDPage_GenericDesktop, kHIDUsage_GD_Joystick)
   def m2 = _matching_dict_for_usage(kHIDPage_GenericDesktop, kHIDUsage_GD_GamePad)
   def m3 = _matching_dict_for_usage(kHIDPage_GenericDesktop, kHIDUsage_GD_MultiAxisController)
   def ptrs = malloc(24)
   if(!ptrs){
      if(m1){ CFRelease(m1) }
      if(m2){ CFRelease(m2) }
      if(m3){ CFRelease(m3) }
      return false
   }
   store64_h(ptrs, m1, 0)
   store64_h(ptrs, m2, 8)
   store64_h(ptrs, m3, 16)
   def arr = CFArrayCreate(0, ptrs, 3, 0)
   free(ptrs)
   if(m1){ CFRelease(m1) }
   if(m2){ CFRelease(m2) }
   if(m3){ CFRelease(m3) }
   IOHIDManagerSetDeviceMatchingMultiple(mgr, arr)
   if(arr){ CFRelease(arr) }
   IOHIDManagerScheduleWithRunLoop(mgr, rl, rl_mode)
   IOHIDManagerRegisterDeviceMatchingCallback(mgr, _device_connected, 0)
   IOHIDManagerRegisterDeviceRemovalCallback(mgr, _device_removed, 0)
   if(IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) != kIOReturnSuccess){
      CFRelease(mgr)
      return false
   }
   CFRunLoopRunInMode(rl_mode, 0.0, 0)
   _hid_manager = mgr
   _initialized = true
   true
}

fn terminate() bool {
   "Runs the terminate operation."
   if(!_initialized){ return true }
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(_get_js(jid)){
         _invoke_callback(jid, backend_api.DISCONNECTED)
         _free_js(jid)
      }
      jid += 1
   }
   if(_hid_manager){
      if(_run_loop && _run_loop_mode){ IOHIDManagerUnscheduleFromRunLoop(_hid_manager, _run_loop, _run_loop_mode) }
      IOHIDManagerClose(_hid_manager, kIOHIDOptionsTypeNone)
      CFRelease(_hid_manager)
      _hid_manager = 0
   }
   if(_run_loop_mode){ CFRelease(_run_loop_mode) _run_loop_mode = 0 }
   _initialized = false
   true
}

fn poll_joysticks() bool {
   "Polls poll joysticks."
   if(!_has_native_support()){ return false }
   if(!_initialized){ return false }
   if(_run_loop_mode){ CFRunLoopRunInMode(_run_loop_mode, 0.0, 0) }
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(_get_js(jid)){ _poll_js(jid) }
      jid += 1
   }
   true
}

fn joystick_present(int jid) bool {
   "Runs the joystick present operation."
   if(!_has_native_support() || jid < 0 || jid >= MAX_JOYSTICKS){ return false }
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

fn joystick_is_gamepad(int jid) bool {
   "Runs the joystick is gamepad operation."
   if(!joystick_present(jid)){ return false }
   gamepad_map.joystick_is_gamepad(_get_js(jid))
}

fn get_gamepad_state(int jid, any state_ptr) bool {
   "Returns get gamepad state."
   if(!joystick_present(jid)){ if(state_ptr){ memset(state_ptr, 0, 64) } return false }
   gamepad_map.get_gamepad_state(_get_js(jid), state_ptr)
}

fn get_gamepad_name(int jid) str {
   "Returns get gamepad name."
   if(!joystick_present(jid)){ return "Unknown" }
   def name = gamepad_map.get_gamepad_name(_get_js(jid))
   if(name && name.len > 0){ return name }
   get_joystick_name(jid)
}

fn set_joystick_callback(any cb) any {
   "Sets set joystick callback."
   def prev = _joystick_callback
   _joystick_callback = cb
   prev
}

fn update_gamepad_mappings(str s) int { gamepad_map.update_mappings(s, "Mac OS X") }
