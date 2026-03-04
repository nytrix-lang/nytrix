;; Keywords: ui window joystick cocoa macos iokit hid
;; Native macOS IOKit/HID joystick backend for Nytrix.

module std.ui.window.platform.cocoa.joystick (
   init, terminate, poll_joysticks,
   joystick_present, get_joystick_name, get_joystick_guid,
   get_joystick_axes, get_joystick_buttons,
   joystick_is_gamepad, get_gamepad_state, get_gamepad_name,
   set_joystick_callback, update_gamepad_mappings
)

use std.core *
use std.core.mem *
use std.str as str
use std.ui.window.platform.api as backend_api
use std.ui.window.platform.gamepad_map as gamepad_map

def MAX_JOYSTICKS = 16
def MAX_ELEMENTS  = 128

;; IOHIDElement usage pages
def kHIDPage_GenericDesktop = 1
def kHIDPage_Button         = 9

;; GenericDesktop axis usages
def kHIDUsage_GD_X          = 0x30
def kHIDUsage_GD_Y          = 0x31
def kHIDUsage_GD_Z          = 0x32
def kHIDUsage_GD_Rx         = 0x33
def kHIDUsage_GD_Ry         = 0x34
def kHIDUsage_GD_Rz         = 0x35
def kHIDUsage_GD_Slider     = 0x36
def kHIDUsage_GD_Hatswitch  = 0x39

;; Device usages for matching
def kHIDUsage_GD_Joystick            = 4
def kHIDUsage_GD_GamePad             = 5
def kHIDUsage_GD_MultiAxisController = 8

def kCFAllocatorDefault  = 0
def kCFNumberSInt32Type  = 3
def kIOHIDOptionsTypeNone = 0
def kCFStringEncodingUTF8 = 0x08000100
def kCFRunLoopRunTimedOut = 3
def kIOReturnSuccess = 0

;; CFArray const callbacks pointer (use NULL for default)
def kCFTypeArrayCallBacks    = 0
def kCFTypeDictionaryKeyCallBacks   = 0
def kCFTypeDictionaryValueCallBacks = 0

if(comptime{ __os_name() == "macos" }){
   #include <IOKit/IOKitLib.h>
   #include <CoreFoundation/CoreFoundation.h>
}

mut _initialized = false
mut _hid_manager  = 0
mut _run_loop     = 0
mut _run_loop_mode = 0
mut _joysticks    = dict()
mut _joystick_callback = 0
mut _key_kIOHIDProductKey       = 0
mut _key_kIOHIDVendorIDKey      = 0
mut _key_kIOHIDProductIDKey     = 0
mut _key_kIOHIDVersionNumberKey = 0
mut _key_kIOHIDManufacturerKey  = 0

fn _has_native_support(){
   comptime{ __os_name() == "macos" }
}

fn _cf_str(s){
   CFStringCreateWithCString(0, cstr(s), kCFStringEncodingUTF8)
}

fn _cf_int(v){
   def p = malloc(8)
   store64_h(p, v, 0)
   def n = CFNumberCreate(0, kCFNumberSInt32Type, p)
   free(p)
   n
}

fn _cf_str_to_str(cf){
   if(!cf){ return "" }
   def buf = malloc(512)
   CFStringGetCString(cf, buf, 512, kCFStringEncodingUTF8)
   def s = to_str(buf)
   free(buf)
   s
}

fn _init_keys(){
   if(_key_kIOHIDProductKey){ return }
   _key_kIOHIDProductKey       = _cf_str("Product")
   _key_kIOHIDVendorIDKey      = _cf_str("VendorID")
   _key_kIOHIDProductIDKey     = _cf_str("ProductID")
   _key_kIOHIDVersionNumberKey = _cf_str("VersionNumber")
   _key_kIOHIDManufacturerKey  = _cf_str("Manufacturer")
}

fn _matching_dict_for_usage(usage_page, usage){
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

fn _get_js(jid){
   dict_get(_joysticks, jid, 0)
}

fn _put_js(jid, js){
   _joysticks = dict_set(_joysticks, jid, js)
}

fn _find_free_slot(){
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(!_get_js(jid)){ return jid }
      jid += 1
   }
   -1
}

fn _find_slot_by_device(dev){
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      def js = _get_js(jid)
      if(js && dict_get(js, "device", 0) == dev){ return jid }
      jid += 1
   }
   -1
}

fn _invoke_callback(jid, event){
   if(_joystick_callback){ _joystick_callback(jid, event) }
}

fn _free_js(jid){
   def js = _get_js(jid)
   if(!js){ return }
   def ap = dict_get(js, "axes_ptr", 0)
   def bp = dict_get(js, "buttons_ptr", 0)
   def ep = dict_get(js, "elements_ptr", 0)
   if(ap){ free(ap) }
   if(bp){ free(bp) }
   if(ep){ free(ep) }
   ;; Release CF objects in elements array (element pointers are owned by the device, not us)
   _put_js(jid, 0)
}

fn _cf_num_to_int(cf_num){
   if(!cf_num){ return 0 }
   def p = malloc(8)
   CFNumberGetValue(cf_num, kCFNumberSInt32Type, p)
   def v = load32(p, 0)
   free(p)
   v
}

fn _build_guid(vendor, product, version){
   ;; SDL-style GUID: bus(2) + pad(2) + vendor(2) + pad(2) + product(2) + pad(2) + version(2) + pad(2)
   ;; For HID on macOS we use bus=0x03 (USB HID)
   def bus = 3
   def b0  = (bus & 0xff)
   def b1  = (bus >> 8) & 0xff
   def v0  = vendor & 0xff
   def v1  = (vendor >> 8) & 0xff
   def p0  = product & 0xff
   def p1  = (product >> 8) & 0xff
   def r0  = version & 0xff
   def r1  = (version >> 8) & 0xff
   def hex = "0123456789abcdef"
   fn byte_hex(b){
      str.chr(load8(hex, (b >> 4) & 0xf)) + str.chr(load8(hex, b & 0xf))
   }
   byte_hex(b0) + byte_hex(b1) + "0000" +
   byte_hex(v0) + byte_hex(v1) + "0000" +
   byte_hex(p0) + byte_hex(p1) + "0000" +
   byte_hex(r0) + byte_hex(r1) + "0000"
}

fn _device_connected(ctx, result, sender, device){
   if(!device){ return }
   def jid = _find_free_slot()
   if(jid < 0){ return }
   _init_keys()

   ;; Get device name and IDs
   def prod_cf  = IOHIDDeviceGetProperty(device, _key_kIOHIDProductKey)
   def vid_cf   = IOHIDDeviceGetProperty(device, _key_kIOHIDVendorIDKey)
   def pid_cf   = IOHIDDeviceGetProperty(device, _key_kIOHIDProductIDKey)
   def ver_cf   = IOHIDDeviceGetProperty(device, _key_kIOHIDVersionNumberKey)
   def name = prod_cf ? _cf_str_to_str(prod_cf) : "Unknown Gamepad"
   def vid  = _cf_num_to_int(vid_cf)
   def pid  = _cf_num_to_int(pid_cf)
   def ver  = _cf_num_to_int(ver_cf)
   def guid = _build_guid(vid, pid, ver)

   ;; Enumerate elements (axes + buttons)
   def elems = IOHIDDeviceCopyMatchingElements(device, 0, 0)
   if(!elems){ return }
   def count = CFArrayGetCount(elems)

   ;; Allocate storage: element_ptr array, axes floats, buttons bytes
   def elem_arr = calloc(count, 24) ;; [ptr, page, usage, lmin, lmax] per element (24 bytes)
   if(!elem_arr){ CFRelease(elems) return }

   mut axis_count   = 0
   mut button_count = 0
   mut hat_count    = 0

   ;; First pass: count
   mut i = 0
   while(i < count){
      def elem = CFArrayGetValueAtIndex(elems, i)
      def page  = IOHIDElementGetUsagePage(elem)
      def usage = IOHIDElementGetUsage(elem)
      if(page == kHIDPage_GenericDesktop && usage >= kHIDUsage_GD_X && usage <= kHIDUsage_GD_Rz){
         axis_count += 1
      } elif(page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Slider){
         axis_count += 1
      } elif(page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Hatswitch){
         hat_count += 1
      } elif(page == kHIDPage_Button){
         button_count += 1
      }
      i += 1
   }

   def total_buttons = button_count + hat_count * 4
   def axes_ptr    = calloc(axis_count, 4)
   def buttons_ptr = calloc(total_buttons + 1, 1)

   ;; Second pass: store element metadata
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
         store64_h(elem_arr, elem,    off) ;; ptr
         store32(elem_arr, 1,           off + 8) ;; type=axis
         store32(elem_arr, axis_idx,    off + 12) ;; index
         store64_h(elem_arr, lmin,    off + 16) ;; lmin
         ;; store lmax in separate slot — pack lmin/lmax as i32
         store32(elem_arr, int(lmax),   off + 16) ;; overwrite — store i32 lmin, lmax
         store32(elem_arr, int(lmin),   off + 12)
         store32(elem_arr, int(lmax),   off + 16)
         axis_idx += 1
      } elif(is_hat){
         ;; Store hat element at a special slot
         def hat_off = (axis_count + button_count + hat_idx) * 24
         store64_h(elem_arr, elem,    hat_off)
         store32(elem_arr, 3,           hat_off + 8) ;; type=hat
         store32(elem_arr, hat_idx,     hat_off + 12)
         store32(elem_arr, int(lmin),   hat_off + 16)
         store32(elem_arr, int(lmax),   hat_off + 20)
         hat_idx += 1
      } elif(is_btn){
         def off = (axis_count + button_idx) * 24
         store64_h(elem_arr, elem,    off)
         store32(elem_arr, 2,           off + 8) ;; type=button
         store32(elem_arr, button_idx,  off + 12)
         button_idx += 1
      }
      i += 1
   }
   CFRelease(elems)

   mut js = dict()
   js = dict_set(js, "device", device)
   js = dict_set(js, "connected", true)
   js = dict_set(js, "name", name)
   js = dict_set(js, "guid", guid)
   js = dict_set(js, "axes_ptr", axes_ptr)
   js = dict_set(js, "buttons_ptr", buttons_ptr)
   js = dict_set(js, "axis_count", axis_count)
   js = dict_set(js, "button_count", total_buttons)
   js = dict_set(js, "raw_button_count", button_count)
   js = dict_set(js, "hat_count", hat_count)
   js = dict_set(js, "elements_ptr", elem_arr)
   js = dict_set(js, "element_count", axis_count + button_count + hat_count)
   _put_js(jid, js)
   _invoke_callback(jid, backend_api.CONNECTED)
}

fn _device_removed(ctx, result, sender, device){
   def jid = _find_slot_by_device(device)
   if(jid < 0){ return }
   _invoke_callback(jid, backend_api.DISCONNECTED)
   _free_js(jid)
}

fn _hat_buttons(value, lmin, lmax){
   ;; Map hat switch value to 4 direction bits: up=bit0 right=bit1 down=bit2 left=bit3
   def range = lmax - lmin
   if(range == 0){ return 0 }
   ;; 8-position hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=center
   def v = int(value) - int(lmin)
   if(v < 0 || v > 8){ return 0 }
   def up    = (v == 0 || v == 1 || v == 7) ? 1 : 0
   def right = (v == 1 || v == 2 || v == 3) ? 1 : 0
   def down  = (v == 3 || v == 4 || v == 5) ? 1 : 0
   def left  = (v == 5 || v == 6 || v == 7) ? 1 : 0
   up | (right << 1) | (down << 2) | (left << 3)
}

fn _poll_js(jid){
   def js = _get_js(jid)
   if(!js){ return }
   def device = dict_get(js, "device", 0)
   if(!device){ return }
   def elem_arr   = dict_get(js, "elements_ptr", 0)
   def axis_count = dict_get(js, "axis_count", 0)
   def btn_count  = dict_get(js, "raw_button_count", 0)
   def hat_count  = dict_get(js, "hat_count", 0)
   def axes_ptr   = dict_get(js, "axes_ptr", 0)
   def btns_ptr   = dict_get(js, "buttons_ptr", 0)
   if(!elem_arr || !axes_ptr || !btns_ptr){ return }

   def total = axis_count + btn_count + hat_count
   def val_ptr = malloc(8)
   if(!val_ptr){ return }

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
         ;; axis: normalize lmin..lmax to -1..1
         def range = lmax - lmin
         def f = range > 0 ? (float(raw - lmin) / float(range)) * 2.0 - 1.0 : 0.0
         store32_f32(axes_ptr, f, eidx * 4)
         } elif(etype == 2){
         ;; button: 0 or 1
         store8(btns_ptr, raw ? 1 : 0, eidx)
         } elif(etype == 3){
         ;; hat switch
         def bits = _hat_buttons(raw, lmin, lmax)
         def base = btn_count + eidx * 4
         store8(btns_ptr, band(bits, 1) ? 1 : 0, base + 0) ;; up
         store8(btns_ptr, band(bits, 2) ? 1 : 0, base + 1) ;; right
         store8(btns_ptr, band(bits, 4) ? 1 : 0, base + 2) ;; down
         store8(btns_ptr, band(bits, 8) ? 1 : 0, base + 3) ;; left
         }
      }
      i += 1
   }
   free(val_ptr)
}

fn init(){
   if(_initialized){ return true }
   if(!_has_native_support()){ return false }
   gamepad_map.init_default_macos_mappings()

   def rl = CFRunLoopGetCurrent()
   def rl_mode = CFStringCreateWithCString(0, cstr("kCFRunLoopDefaultMode"), kCFStringEncodingUTF8)
   _run_loop = rl
   _run_loop_mode = rl_mode

   def mgr = IOHIDManagerCreate(0, kIOHIDOptionsTypeNone)
   if(!mgr){ return false }

   ;; Build matching array for Joystick + GamePad + MultiAxisController
   def m1 = _matching_dict_for_usage(kHIDPage_GenericDesktop, kHIDUsage_GD_Joystick)
   def m2 = _matching_dict_for_usage(kHIDPage_GenericDesktop, kHIDUsage_GD_GamePad)
   def m3 = _matching_dict_for_usage(kHIDPage_GenericDesktop, kHIDUsage_GD_MultiAxisController)
   def ptrs = malloc(24)
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

   ;; Drain the run loop briefly to detect already-connected devices
   CFRunLoopRunInMode(rl_mode, 0.0, 0)

   _hid_manager = mgr
   _initialized = true
   true
}

fn terminate(){
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
      if(_run_loop && _run_loop_mode){
         IOHIDManagerUnscheduleFromRunLoop(_hid_manager, _run_loop, _run_loop_mode)
      }
      IOHIDManagerClose(_hid_manager, kIOHIDOptionsTypeNone)
      CFRelease(_hid_manager)
      _hid_manager = 0
   }
   if(_run_loop_mode){ CFRelease(_run_loop_mode) _run_loop_mode = 0 }
   _initialized = false
   true
}

fn poll_joysticks(){
   if(!_has_native_support()){ return false }
   if(!_initialized){ return false }
   ;; Process pending run loop events (connect/disconnect)
   if(_run_loop_mode){
      CFRunLoopRunInMode(_run_loop_mode, 0.0, 0)
   }
   ;; Poll current values for all connected joysticks
   mut jid = 0
   while(jid < MAX_JOYSTICKS){
      if(_get_js(jid)){ _poll_js(jid) }
      jid += 1
   }
   true
}

fn joystick_present(jid){
   if(!_has_native_support() || jid < 0 || jid >= MAX_JOYSTICKS){ return false }
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

fn update_gamepad_mappings(s){
   gamepad_map.update_mappings(s, "Mac OS X")
}
