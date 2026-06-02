;; Keywords: platform window backend cocoa macos joystick
;; Cocoa native window backend for macOS windows, input, monitors, and clipboard.
module std.os.ui.window.platform.cocoa(available, get_backend_name, get_class, get_selector, shared_application, create_autorelease_pool, drain_autorelease_pool, set_activation_policy_regular, finish_launching, activate_ignoring_other_apps, install_app_delegate, get_monitors, get_primary_monitor, get_monitor_pos, get_monitor_workarea, get_monitor_physical_size, get_monitor_content_scale, get_monitor_name, get_video_mode, get_video_modes, get_gamma_ramp, set_gamma_ramp, current_event, next_event, wait_events, send_event, update_windows, run_app, stop_app, request_user_attention, post_event, get_window_monitor, set_window_monitor, get_key_state, get_mouse_button_state, get_key_name, get_cursor_pos, set_cursor_pos, create_cursor, create_standard_cursor, destroy_cursor, set_cursor, set_window_title, get_window_attrib, set_window_opacity, set_window_resizable, set_window_decorated, set_window_floating, set_window_icon, get_pos, set_pos, get_size, set_size, get_framebuffer_size, get_window_content_scale, create_basic_window, destroy_basic_window, poll_window_events, show_window, hide_window, iconify_window, restore_window, maximize_window, vulkan_supported, vulkan_required_extensions, focus_window, set_clipboard, get_clipboard, set_input_mode, create_surface, objc_msgSend, objc_msgSend_ptr, objc_msgSend_ptr_ptr, objc_msgSend_ptr_i64, get_cocoa_window, get_cocoa_monitor, get_cocoa_view)
use std.core
use std.core.mem (cstr)
use std.core.str as str
use std.core.common as common
use std.os.ffi as ffi
use std.os.ui.window.consts
use std.os.ui.window.platform.api as backend_api
use std.os.ui.render.vk.vulkan (vk_create_metal_surface_ext)

fn _handle_from(any: v): any {
   if(is_dict(v)){ return v.get("handle", 0) }
   v
}

def NSApplicationActivationPolicyRegular = 0
def NSEventMaskAny = 0xffffffffffffffff
def NSEventModifierFlagCapsLock = 0x10000
def NSEventModifierFlagShift    = 0x20000
def NSEventModifierFlagControl  = 0x40000
def NSEventModifierFlagOption   = 0x80000
def NSEventModifierFlagCommand  = 0x100000
def NSWindowStyleMaskBorderless = 0
def NSWindowStyleMaskTitled = 1
def NSWindowStyleMaskClosable = 2
def NSWindowStyleMaskMiniaturizable = 4
def NSWindowStyleMaskResizable = 8
def NSWindowStyleMaskUnifiedTitleAndToolbar = 4096
def NSWindowStyleMaskFullScreen = 16384
def NSWindowStyleMaskFullSizeContentView = 32768
def NSNormalWindowLevel = 0
def NSFloatingWindowLevel = 3
def NSMainMenuWindowLevel = 24
def NSBitmapFormatAlphaNonpremultiplied = 2

fn available(): bool { #macos { return true } #else { return false } #endif }

fn get_backend_name(): str { return "cocoa" }

mut _cocoa_frameworks_loaded = false

fn _load_cocoa_frameworks(): bool {
   if(_cocoa_frameworks_loaded){ return true }
   #macos {
      def flags = ffi.RTLD_NOW() | ffi.RTLD_GLOBAL()
      ffi.dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", flags)
      ffi.dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", flags)
      ffi.dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", flags)
   }
   _cocoa_frameworks_loaded = true
   true
}

#macos {
   #link "-framework AppKit"
   #link "-framework ApplicationServices"
   #link "-framework CoreFoundation"
   #link "-framework QuartzCore"
   #link "objc"
   #include <objc/objc.h>
   #include <objc/objc-runtime.h>
   #include <AppKit/AppKit.h>
   #include <ApplicationServices/ApplicationServices.h>
   extern "" {
      fn _objc_msgSend(ptr: target, fnptr: op): ptr as "objc_msgSend"
      fn _objc_msgSend_ptr(ptr: target, fnptr: op, ptr: arg): ptr as "objc_msgSend"
      fn _objc_msgSend_ptr_ptr(ptr: target, fnptr: op, ptr: a1, ptr: a2): ptr as "objc_msgSend"
      fn _objc_msgSend_ptr_ptr_ptr(ptr: target, fnptr: op, ptr: a1, ptr: a2, ptr: a3): ptr as "objc_msgSend"
      fn _objc_msgSend_ptr_u64_ptr_ptr_i64(ptr: target, fnptr: op, u64: a1, ptr: a2, ptr: a3, i64: a4): ptr as "objc_msgSend"
      fn _objc_msgSend_ptr_i64(ptr: target, fnptr: op, i64: a1, i64: a2): ptr as "objc_msgSend"
      fn _objc_msgSend_ptr_i64_arg(ptr: target, fnptr: op, i64: arg): ptr as "objc_msgSend"
      fn _objc_msgSend_sel(ptr: target, fnptr: op, fnptr: arg): ptr as "objc_msgSend"
      fn _objc_msgSend_f64(ptr: target, fnptr: op): f64 as "objc_msgSend"
      fn _objc_msgSend_arg_f64(ptr: target, fnptr: op, f64: arg): ptr as "objc_msgSend"
      fn _objc_msgSend_size(ptr: target, fnptr: op, f64: w, f64: h): ptr as "objc_msgSend"
      fn _objc_msgSend_ptr_point(ptr: target, fnptr: op, ptr: arg, f64: x, f64: y): ptr as "objc_msgSend"
      fn _objc_msgSend_bitmap_init(ptr: target, fnptr: op, ptr: planes, i64: pixels_wide, i64: pixels_high, i64: bits_per_sample, i64: samples_per_pixel, i8: has_alpha, i8: is_planar, ptr: color_space_name, u64: bitmap_format, i64: bytes_per_row, i64: bits_per_pixel): ptr as "objc_msgSend"
      fn _objc_msgSend_rect_u64_i64_i8(ptr: target, fnptr: op, f64: x, f64: y, f64: w, f64: h, u64: style, i64: backing, i8: do_defer): ptr as "objc_msgSend"
      fn _objc_msgSend_i64(ptr: target, fnptr: op): i64 as "objc_msgSend"
      fn _objc_msgSend_i64_arg(ptr: target, fnptr: op, i64: arg): i64 as "objc_msgSend"
      fn _objc_msgSend_i64_sel(ptr: target, fnptr: op, fnptr: arg): i64 as "objc_msgSend"
      fn _ny_objc_getClass(ptr: name): ptr as "objc_getClass"
      fn _ny_sel_registerName(ptr: name): fnptr as "sel_registerName"
      fn _ny_objc_allocateClassPair(ptr: superclass, ptr: name, u64: extra_bytes): ptr as "objc_allocateClassPair"
      fn _ny_class_addMethod(ptr: cls, fnptr: name, fnptr: imp, ptr: types): bool as "class_addMethod"
      fn _ny_objc_registerClassPair(ptr: cls): any as "objc_registerClassPair"
      fn CGAssociateMouseAndMouseCursorPosition(any: connected): any
      fn CGDisplayCopyAllDisplayModes(any: display_id, any: options): any
      fn CGDisplayCopyDisplayMode(any: display_id): any
      fn CGDisplayGammaTableCapacity(any: display_id): int
      fn CGDisplayModeGetHeight(any: mode): int
      fn CGDisplayModeGetRefreshRate(any: mode): int
      fn CGDisplayModeGetWidth(any: mode): int
      fn CGDisplayModeRelease(any: mode): any
      fn CGDisplaySetDisplayMode(any: display_id, any: mode, any: options): int
      fn CGDisplayUnitNumber(any: display_id): int
      fn CGGetActiveDisplayList(u32: max_displays, ptr: ids, ptr: count): int
      fn CGGetDisplayTransferByTable(any: display_id, any: capacity, any: red, any: green, any: blue, any: count): int
      fn CGSetDisplayTransferByTable(any: display_id, any: size, any: red, any: green, any: blue): int
      fn CGWarpMouseCursorPosition(any: x, any: y): any
      fn CFArrayGetCount(any: arr): int
      fn CFArrayGetValueAtIndex(any: arr, int: index): any
      fn CFRelease(any: obj): any
   }
} #else {
   fn _ny_objc_getClass(ptr: _name): ptr { 0 }
   fn _ny_sel_registerName(ptr: _name): fnptr { return fn(){ 0 } }
   fn _ny_objc_allocateClassPair(ptr: _superclass, ptr: _name, u64: _extra_bytes): ptr { 0 }
   fn _ny_class_addMethod(ptr: _cls, fnptr: _name, fnptr: _imp, ptr: _types): bool { false }
   fn _ny_objc_registerClassPair(ptr: _cls): any { 0 }
   fn CGAssociateMouseAndMouseCursorPosition(any: _connected): any { 0 }
   fn CGDisplayCopyAllDisplayModes(any: _display_id, any: _options): any { 0 }
   fn CGDisplayCopyDisplayMode(any: _display_id): any { 0 }
   fn CGDisplayGammaTableCapacity(any: _display_id): int { 0 }
   fn CGDisplayModeGetHeight(any: _mode): int { 0 }
   fn CGDisplayModeGetRefreshRate(any: _mode): int { 0 }
   fn CGDisplayModeGetWidth(any: _mode): int { 0 }
   fn CGDisplayModeRelease(any: _mode): any { 0 }
   fn CGDisplaySetDisplayMode(any: _display_id, any: _mode, any: _options): int { 1 }
   fn CGDisplayUnitNumber(any: _display_id): int { 0 }
   fn CGGetActiveDisplayList(u32: _max_displays, ptr: _ids, ptr: _count): int { 1 }
   fn CGGetDisplayTransferByTable(any: _display_id, any: _capacity, any: _red, any: _green, any: _blue, any: _count): int { 1 }
   fn CGSetDisplayTransferByTable(any: _display_id, any: _size, any: _red, any: _green, any: _blue): int { 1 }
   fn CGWarpMouseCursorPosition(any: _x, any: _y): any { 0 }
   fn CFArrayGetCount(any: _arr): int { 0 }
   fn CFArrayGetValueAtIndex(any: _arr, int: _index): any { 0 }
   fn CFRelease(any: _obj): any { 0 }
} #endif

fn objc_msgSend(any: target, any: op): any {
   #macos { return _objc_msgSend(target, op) }
   0
}

fn objc_msgSend_ptr(any: target, any: op, ptr: arg=0): any {
   #macos { return _objc_msgSend_ptr(target, op, arg) }
   0
}

fn objc_msgSend_ptr_ptr(any: target, any: op, ptr: a1=0, ptr: a2=0): any {
   #macos { return _objc_msgSend_ptr_ptr(target, op, a1, a2) }
   0
}

fn objc_msgSend_ptr_ptr_ptr(any: target, any: op, ptr: a1=0, ptr: a2=0, ptr: a3=0): any {
   #macos { return _objc_msgSend_ptr_ptr_ptr(target, op, a1, a2, a3) }
   0
}

fn objc_msgSend_ptr_u64_ptr_ptr_i64(any: target, any: op, u64: a1=0, ptr: a2=0, ptr: a3=0, i64: a4=0): any {
   #macos { return _objc_msgSend_ptr_u64_ptr_ptr_i64(target, op, a1, a2, a3, a4) }
   0
}

fn objc_msgSend_ptr_i64(any: target, any: op, i64: a1=0, i64: a2=0): any {
   #macos { return _objc_msgSend_ptr_i64(target, op, a1, a2) }
   0
}

fn objc_msgSend_ptr_i64_arg(any: target, any: op, i64: arg=0): any {
   #macos { return _objc_msgSend_ptr_i64_arg(target, op, arg) }
   0
}

fn objc_msgSend_sel(any: target, any: op, any: arg): any {
   #macos { return _objc_msgSend_sel(target, op, arg) }
   0
}

fn objc_msgSend_f64(any: target, any: op): f64 {
   #macos { return _objc_msgSend_f64(target, op) }
   0.0
}

fn objc_msgSend_arg_f64(any: target, any: op, f64: arg): any {
   #macos { return _objc_msgSend_arg_f64(target, op, arg) }
   0
}

fn objc_msgSend_ptr_f64(any: target, any: op, any: arg): any {
   #macos { return _objc_msgSend_arg_f64(target, op, float(arg)) }
   0
}

fn objc_msgSend_size(any: target, any: op, any: w, any: h): any {
   #macos { return _objc_msgSend_size(target, op, float(w), float(h)) }
   0
}

fn objc_msgSend_ptr_point(any: target, any: op, ptr: arg=0, any: x=0, any: y=0): any {
   #macos { return _objc_msgSend_ptr_point(target, op, arg, float(x), float(y)) }
   0
}

fn objc_msgSend_bitmap_init(any: target, any: op, ptr: planes, i64: pixels_wide, i64: pixels_high, i64: bits_per_sample, i64: samples_per_pixel, i8: has_alpha, i8: is_planar, ptr: color_space_name, u64: bitmap_format, i64: bytes_per_row, i64: bits_per_pixel): any {
   #macos { return _objc_msgSend_bitmap_init(target, op, planes, pixels_wide, pixels_high, bits_per_sample, samples_per_pixel, has_alpha, is_planar, color_space_name, bitmap_format, bytes_per_row, bits_per_pixel) }
   0
}

fn objc_msgSend_rect_u64_i64_i8(any: target, any: op, any: x, any: y, any: w, any: h, u64: style, i64: backing, i8: do_defer): any {
   #macos { return _objc_msgSend_rect_u64_i64_i8(target, op, x, y, w, h, style, backing, do_defer) }
   0
}

fn objc_msgSend_i64(any: target, any: op): int {
   #macos { return _objc_msgSend_i64(target, op) }
   0
}

fn objc_msgSend_u64(any: target, any: op): int { objc_msgSend_i64(target, op) }

fn objc_msgSend_i64_arg(any: target, any: op, i64: arg): int {
   #macos { return _objc_msgSend_i64_arg(target, op, arg) }
   0
}

fn objc_msgSend_i64_sel(any: target, any: op, any: arg): int {
   #macos { return _objc_msgSend_i64_sel(target, op, arg) }
   0
}

fn get_class(str: name): any {
   "Looks up an Objective-C class by name."
   _load_cocoa_frameworks()
   #macos { return _ny_objc_getClass(cstr(name)) }
   0
}

fn get_selector(str: name): any {
   "Looks up an Objective-C selector by name."
   #macos { return _ny_sel_registerName(cstr(name)) }
   0
}

fn shared_application(): any {
   "Returns `[NSApplication sharedApplication]` when AppKit is available."
   if(!available()){ return 0 }
   def cls = get_class("NSApplication")
   if(!cls){ return 0 }
   objc_msgSend(cls, get_selector("sharedApplication"))
}

fn create_autorelease_pool(): any {
   "Creates a local `NSAutoreleasePool` for native Cocoa work."
   if(!available()){ return 0 }
   def cls = get_class("NSAutoreleasePool")
   if(!cls){ return 0 }
   def pool = objc_msgSend(cls, get_selector("alloc"))
   if(!pool){ return 0 }
   objc_msgSend(pool, get_selector("init"))
}

fn drain_autorelease_pool(any: pool): bool {
   "Drains a previously created `NSAutoreleasePool`."
   if(!available() || !pool){ return false }
   objc_msgSend(pool, get_selector("drain"))
   true
}

fn set_activation_policy_regular(): bool {
   "Sets the app activation policy to regular AppKit-window behavior."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_i64_arg(app, get_selector("setActivationPolicy:"), NSApplicationActivationPolicyRegular) != 0
}

fn _app_delegate_should_terminate(any: self, any: sel, any: sender): int {
   "NSApplicationDelegate applicationShouldTerminate: — broadcasts QUIT to all windows."
   def keys = dict_keys(_windows)
   mut i = 0
   def keys_n = keys.len
   while(i < keys_n){
      def w = _windows.get(keys.get(i), 0)
      if(is_dict(w)){ _push_event(w, 15, 0) }
      i += 1
   }
   1 ;; NSTerminateNow
}

fn _app_delegate_did_finish_launching(any: self, any: sel, any: notification): any {
   "NSApplicationDelegate applicationDidFinishLaunching: — activates the application."
   def app = shared_application()
   if(app){ objc_msgSend_i64_arg(app, get_selector("activateIgnoringOtherApps:"), 1) }
}

mut _app_delegate_class = 0

fn _create_app_delegate(): any {
   "Creates and registers a dynamic NytrixAppDelegate NSApplicationDelegate class."
   if(_app_delegate_class){ return _app_delegate_class }
   if(!available()){ return 0 }
   def base = get_class("NSObject")
   def cls = _ny_objc_allocateClassPair(base, cstr("NytrixAppDelegate"), 0)
   if(cls){
      _ny_class_addMethod(cls, get_selector("applicationShouldTerminate:"), _app_delegate_should_terminate, cstr("l@:@"))
      _ny_class_addMethod(cls, get_selector("applicationDidFinishLaunching:"), _app_delegate_did_finish_launching, cstr("v@:@"))
      _ny_objc_registerClassPair(cls)
      _app_delegate_class = cls
   }
   cls
}

fn install_app_delegate(): bool {
   "Installs a NytrixAppDelegate as the NSApplication delegate if not already set."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   def existing = objc_msgSend(app, get_selector("delegate"))
   if(existing){ return existing != 0 }
   def cls = _create_app_delegate()
   if(!cls){ return false }
   def delegate = objc_msgSend(objc_msgSend(cls, get_selector("alloc")), get_selector("init"))
   if(!delegate){ return false }
   objc_msgSend_ptr(app, get_selector("setDelegate:"), delegate)
   true
}

fn finish_launching(): bool {
   "Finishes AppKit startup on the shared application."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   install_app_delegate()
   objc_msgSend(app, get_selector("finishLaunching"))
   true
}

fn activate_ignoring_other_apps(bool: v=true): bool {
   "Activate the current app for Cocoa window presentation."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_i64_arg(app, get_selector("activateIgnoringOtherApps:"), v ? 1 : 0)
   true
}

fn get_screens(): any {
   "Returns the `NSScreen` array."
   if(!available()){ return 0 }
   def cls = get_class("NSScreen")
   if(!cls){ return 0 }
   objc_msgSend(cls, get_selector("screens"))
}

fn get_screen_count(any: screens=0): int {
   "Returns the number of active Cocoa screens."
   def arr = common.value_or(screens, get_screens())
   if(!arr){ return 0 }
   int(objc_msgSend_i64(arr, get_selector("count")))
}

fn get_screen_at(int: index, any: screens=0): any {
   "Returns the screen object at `index`."
   def arr = common.value_or(screens, get_screens())
   if(!arr || index < 0){ return 0 }
   objc_msgSend_ptr_i64_arg(arr, get_selector("objectAtIndex:"), index)
}

fn get_screen_name(any: screen): str {
   "Returns an `NSScreen` localized name when available."
   if(!available() || !screen){ return "" }
   def name_obj = objc_msgSend(screen, get_selector("localizedName"))
   if(!name_obj){ return "" }
   def utf8 = objc_msgSend(name_obj, get_selector("UTF8String"))
   if(!utf8){ return "" }
   to_str(utf8)
}

fn get_screen_display_id(any: screen): int {
   "Returns the CoreGraphics display id for the given `NSScreen`."
   if(!available() || !screen){ return 0 }
   def key = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr("NSScreenNumber"))
   if(!key){ return 0 }
   def desc = objc_msgSend_ptr(screen, get_selector("deviceDescription"))
   if(!desc){ return 0 }
   def number = objc_msgSend_ptr(desc, get_selector("objectForKey:"), key)
   if(!number){ return 0 }
   int(objc_msgSend_i64(number, get_selector("unsignedIntValue")))
}

fn get_screen_for_display_id(int: display_id, any: screens=0): any {
   "Returns the `NSScreen` matching the given CoreGraphics display id."
   def arr = common.value_or(screens, get_screens())
   if(!arr){ return 0 }
   mut i = 0
   def n = get_screen_count(arr)
   while(i < n){
      def screen = get_screen_at(i, arr)
      if(screen && get_screen_display_id(screen) == int(display_id)){ return screen }
      i += 1
   }
   0
}

fn get_display_unit_number(int: display_id): int {
   "Returns the CoreGraphics unit number for a display id."
   if(!available() || !display_id){ return 0 }
   int(CGDisplayUnitNumber(display_id))
}

fn get_screen_for_unit_number(int: unit_number, any: screens=0): any {
   "Returns the `NSScreen` matching the given CoreGraphics unit number."
   def arr = common.value_or(screens, get_screens())
   if(!arr){ return 0 }
   mut i = 0
   def n = get_screen_count(arr)
   while(i < n){
      def screen = get_screen_at(i, arr)
      if(screen){
         def display_id = get_screen_display_id(screen)
         if(display_id && get_display_unit_number(display_id) == int(unit_number)){ return screen }
      }
      i += 1
   }
   0
}

fn _rect_dict_from_string(str: s): any {
   if(!s || s.len == 0){ return false }
   def s_n = s.len
   mut vals = []
   mut tok = str.Builder(32)
   mut i = 0
   while(i < s_n){
      def c = load8(s, i)
      def numeric = (c >= 48 && c <= 57) || c == 45 || c == 46 || c == 43
      if(numeric){ tok = str.builder_append(tok, str.chr(c)) } elif(tok.get(1, 0) > 0){
         vals = vals.append(str.atof(str.builder_to_str(tok)))
         str.builder_free(tok)
         tok = str.Builder(32)
         if(vals.len >= 4){
            str.builder_free(tok)
            break
         }
      }
      i += 1
   }
   if(tok.get(1, 0) > 0 && vals.len < 4){ vals = vals.append(str.atof(str.builder_to_str(tok))) }
   str.builder_free(tok)
   if(vals.len < 4){ return false }
   return {
      "x": int(vals.get(0, 0.0)),
      "y": int(vals.get(1, 0.0)),
      "width": int(vals.get(2, 0.0)),
      "height": int(vals.get(3, 0.0))
   }
}

fn _screen_rect_for_key(any: screen, str: key_name): any {
   if(!available() || !screen){ return false }
   def key = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(key_name))
   if(!key){ return false }
   def boxed = objc_msgSend_ptr(screen, get_selector("valueForKey:"), key)
   if(!boxed){ return false }
   def desc = objc_msgSend(boxed, get_selector("description"))
   if(!desc){ return false }
   def utf8 = objc_msgSend(desc, get_selector("UTF8String"))
   if(!utf8){ return false }
   _rect_dict_from_string(to_str(utf8))
}

fn get_screen_frame(any: screen): any {
   "Returns the Cocoa screen frame as `{x,y,width,height}` when available."
   _screen_rect_for_key(screen, "frame")
}

fn get_screen_visible_frame(any: screen): any {
   "Returns the Cocoa visible frame/workarea as `{x,y,width,height}` when available."
   _screen_rect_for_key(screen, "visibleFrame")
}

fn get_screen_scale(any: screen): f64 {
   "Returns the AppKit backing scale factor for an `NSScreen`."
   if(!available() || !screen){ return 1.0 }
   def scale = objc_msgSend_f64(screen, get_selector("backingScaleFactor"))
   scale > 0.0 ? scale : 1.0
}

fn get_monitors(): list {
   "Returns a list of dictionaries representing active Cocoa monitors."
   def ids = get_active_display_ids()
   mut out = []
   mut i = 0
   def ids_n = ids.len
   while(i < ids_n){
      def display_id = ids.get(i, 0)
      mut m = dict(8)
      m = m.set("handle", display_id)
      m = m.set("id", display_id)
      m = m.set("name", "Display " + to_str(display_id))
      out = out.append(m)
      i += 1
   }
   out
}

fn get_primary_monitor(): any {
   "Returns the first available Cocoa monitor."
   def ms = get_monitors()
   ms.len > 0 ? ms.get(0) : 0
}

fn get_monitor_name(any: monitor): str {
   "Returns the localized name for a Cocoa monitor handle."
   if(!monitor){ return "Unknown Monitor" }
   def display_id = _handle_from(monitor)
   def screen = get_screen_for_display_id(display_id)
   screen ? get_screen_name(screen) : "Display " + to_str(display_id)
}

fn get_monitor_pos(any: monitor): list {
   "Returns the screen position [x, y] of a Cocoa monitor."
   if(!monitor){ return [0, 0] }
   def display_id = _handle_from(monitor)
   def screen = get_screen_for_display_id(display_id)
   def frame = get_screen_frame(screen)
   frame ? [frame.get("x", 0), frame.get("y", 0)] : [0, 0]
}

fn get_monitor_workarea(any: monitor): list {
   "Returns the visible workarea [x, y, w, h] of a Cocoa monitor."
   if(!monitor){ return [0, 0, 0, 0] }
   def display_id = _handle_from(monitor)
   def screen = get_screen_for_display_id(display_id)
   def frame = get_screen_visible_frame(screen)
   frame ? [frame.get("x", 0), frame.get("y", 0), frame.get("width", 0), frame.get("height", 0)] : [0, 0, 0, 0]
}

fn get_monitor_physical_size(any: monitor): list {
   "Returns the approximate physical size [mm_w, mm_h] of a Cocoa monitor."
   if(!monitor){ return [0, 0] }
   def mode = get_video_mode(monitor)
   if(!mode){ return [0, 0] }
   def scale = get_monitor_content_scale(monitor)
   def dpi = 96.0 * float(scale.get(0, 1.0))
   if(dpi <= 0.0){ return [0, 0] }
   [int(float(mode.get("width", 0)) * 25.4 / dpi),
    int(float(mode.get("height", 0)) * 25.4 / dpi)]
}

fn get_monitor_content_scale(any: monitor): list {
   "Returns the backing scale factor(Retina scale) for a Cocoa monitor."
   if(!monitor){ return [1.0, 1.0] }
   def display_id = _handle_from(monitor)
   def screen = get_screen_for_display_id(display_id)
   def s = screen ? get_screen_scale(screen) : 1.0
   [s, s]
}

fn get_video_mode(any: monitor): any {
   "Returns the current video mode for a Cocoa monitor."
   if(!monitor){ return false }
   def display_id = _handle_from(monitor)
   get_current_video_mode(display_id)
}

fn get_video_modes(any: monitor): list {
   "Returns available video modes for a Cocoa monitor."
   if(!monitor){ return [] }
   def display_id = _handle_from(monitor)
   get_video_modes_native(display_id)
}

fn get_gamma_ramp(any: monitor): any {
   "Returns the CoreGraphics gamma ramp for the given monitor."
   if(!available() || !is_dict(monitor)){ return false }
   def display_id = monitor.get("handle", 0)
   if(!display_id){ return false }
   def capacity = int(CGDisplayGammaTableCapacity(display_id))
   if(capacity <= 0){ return false }
   def red_buf   = zalloc(capacity * 4)
   def green_buf = zalloc(capacity * 4)
   def blue_buf  = zalloc(capacity * 4)
   def count_ptr = zalloc(4)
   if(!red_buf || !green_buf || !blue_buf || !count_ptr){ free(red_buf) free(green_buf) free(blue_buf) free(count_ptr) return false }
   def ok = CGGetDisplayTransferByTable(display_id, capacity, red_buf, green_buf, blue_buf, count_ptr)
   def actual = load32(count_ptr, 0)
   free(count_ptr)
   if(ok != 0 || actual <= 0){ free(red_buf) free(green_buf) free(blue_buf) return false }
   mut red, green, blue = [], [], []
   mut i = 0 while(i < actual){
      def scale = 65535.0
      red   = red.append(int(load32_f32(red_buf,   i * 4) * scale + 0.5))
      green = green.append(int(load32_f32(green_buf, i * 4) * scale + 0.5))
      blue  = blue.append(int(load32_f32(blue_buf,  i * 4) * scale + 0.5))
      i += 1
   }
   free(red_buf) free(green_buf) free(blue_buf)
   return {"size": actual, "red": red, "green": green, "blue": blue}
}

fn set_gamma_ramp(any: monitor, any: ramp): bool {
   "Applies a gamma ramp to the given monitor via CoreGraphics."
   if(!available() || !is_dict(monitor) || !is_dict(ramp)){ return false }
   def display_id = monitor.get("handle", 0)
   if(!display_id){ return false }
   def size = ramp.get("size", 0)
   if(size <= 0){ return false }
   def red   = ramp.get("red", [])
   def green = ramp.get("green", [])
   def blue  = ramp.get("blue", [])
   if(red.len < size || green.len < size || blue.len < size){ return false }
   def red_buf   = zalloc(size * 4)
   def green_buf = zalloc(size * 4)
   def blue_buf  = zalloc(size * 4)
   if(!red_buf || !green_buf || !blue_buf){ free(red_buf) free(green_buf) free(blue_buf) return false }
   def scale = 1.0 / 65535.0
   mut i = 0 while(i < size){
      store32_f32(red_buf,   red.get(i) * scale, i * 4)
      store32_f32(green_buf, green.get(i) * scale, i * 4)
      store32_f32(blue_buf,  blue.get(i) * scale, i * 4)
      i += 1
   }
   def ok = CGSetDisplayTransferByTable(display_id, size, red_buf, green_buf, blue_buf) == 0
   free(red_buf) free(green_buf) free(blue_buf)
   ok
}

fn focus_window(any: win): bool { true }

fn set_clipboard(any: win, any: s): bool {
   "Sets the Cocoa system clipboard to string `s`."
   if(!available()){ return false }
   def pool = create_autorelease_pool()
   def pb = objc_msgSend_ptr(get_class("NSPasteboard"), get_selector("generalPasteboard"), 0)
   objc_msgSend_i64(pb, get_selector("clearContents"))
   def str_obj = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(to_str(s)))
   def arr = objc_msgSend_ptr(get_class("NSArray"), get_selector("arrayWithObject:"), str_obj)
   objc_msgSend_ptr(pb, get_selector("writeObjects:"), arr)
   drain_autorelease_pool(pool)
   true
}

fn get_clipboard(any: win): str {
   "Retrieves string contents from the Cocoa system clipboard."
   if(!available()){ return "" }
   def pool = create_autorelease_pool()
   def pb = objc_msgSend_ptr(get_class("NSPasteboard"), get_selector("generalPasteboard"), 0)
   def type = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr("public.utf8-plain-text"))
   def types = objc_msgSend_ptr(get_class("NSArray"), get_selector("arrayWithObject:"), type)
   def best = objc_msgSend_ptr(pb, get_selector("availableTypeFromArray:"), types)
   mut out = ""
   if(best){
      def res = objc_msgSend_ptr(pb, get_selector("stringForType:"), best)
      if(res){
         def utf8 = objc_msgSend_ptr(res, get_selector("UTF8String"), 0)
         if(utf8){ out = to_str(utf8) }
      }
   }
   drain_autorelease_pool(pool)
   out
}

fn set_input_mode(any: win, int: mode, int: value): bool {
   "Sets input modes for a Cocoa window(cursor, sticky keys, etc.)."
   if(!available() || !win || !is_dict(win)){ return false }
   mut next_win = win
   if(mode == CURSOR){
      if(value == CURSOR_NORMAL || value == CURSOR_CAPTURED){
         objc_msgSend_ptr(get_class("NSCursor"), get_selector("unhide"), 0)
         CGAssociateMouseAndMouseCursorPosition(1)
      } elif(value == CURSOR_HIDDEN){
         objc_msgSend_ptr(get_class("NSCursor"), get_selector("hide"), 0)
         CGAssociateMouseAndMouseCursorPosition(1)
      } elif(value == CURSOR_DISABLED){
         objc_msgSend_ptr(get_class("NSCursor"), get_selector("hide"), 0)
         CGAssociateMouseAndMouseCursorPosition(0)
      }
      next_win = next_win.set("cursor_mode", value)
   } elif(mode == RAW_MOUSE_MOTION){
      next_win = next_win.set("raw_mouse_motion", value != 0)
   } elif(mode == STICKY_KEYS){
      next_win = next_win.set("sticky_keys", !!value)
   } elif(mode == STICKY_MOUSE_BUTTONS){
      next_win = next_win.set("sticky_mouse", !!value)
   }
   def hwnd = win.get("handle", 0)
   _windows = _windows.set(hwnd, next_win)
   true
}

fn create_surface(any: instance, any: win, any: allocator, any: surface_out): int {
   "Creates a native Vulkan Metal surface for the Cocoa window."
   if(!available()){ return -1 }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return -1 }
   def layer_cls = get_class("CAMetalLayer")
   if(!layer_cls){ return -1 }
   def layer = objc_msgSend(layer_cls, get_selector("layer"))
   if(!layer){ return -1 }
   def view = objc_msgSend(hwnd, get_selector("contentView"))
   if(!view){ return -1 }
   objc_msgSend_ptr_i64(view, get_selector("setWantsLayer:"), 1)
   objc_msgSend_ptr(view, get_selector("setLayer:"), layer)
   def info = malloc(32)
   if(!info){ return -1 }
   memset(info, 0, 32)
   store32(info, 1000217000, 0)
   store64_h(info, layer, 24)
   def res = vk_create_metal_surface_ext(instance, info, allocator, surface_out)
   free(info)
   res
}

fn get_active_display_ids(): list {
   "Returns the active CoreGraphics display ids."
   if(!available()){ return [] }
   def count_ptr = zalloc(4)
   if(!count_ptr){ return [] }
   if(CGGetActiveDisplayList(0, 0, count_ptr) != 0){
      free(count_ptr)
      return []
   }
   def n = int(load32(count_ptr, 0))
   if(n <= 0){
      free(count_ptr)
      return []
   }
   def ids_buf = zalloc(n * 4)
   if(!ids_buf){
      free(count_ptr)
      return []
   }
   if(CGGetActiveDisplayList(n, ids_buf, count_ptr) != 0){
      free(ids_buf, count_ptr)
      return []
   }
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append(load32(ids_buf, i * 4))
      i += 1
   }
   free(ids_buf, count_ptr)
   out
}

fn _cg_mode_to_dict(any: mode): any {
   "Converts a `CGDisplayModeRef` to a Ny video-mode dictionary."
   if(!mode){ return false }
   return {
      "width": int(CGDisplayModeGetWidth(mode)),
      "height": int(CGDisplayModeGetHeight(mode)),
      "refresh_rate": int(CGDisplayModeGetRefreshRate(mode))
   }
}

fn get_current_video_mode(any: display_id): any {
   "Returns the current video mode for a CoreGraphics display id."
   if(!available() || !display_id){ return false }
   def mode = CGDisplayCopyDisplayMode(display_id)
   if(!mode){ return false }
   def out = _cg_mode_to_dict(mode)
   CGDisplayModeRelease(mode)
   out
}

fn get_video_modes_native(any: display_id): list {
   "Returns the advertised video modes for a CoreGraphics display id."
   if(!available() || !display_id){ return [] }
   def modes = CGDisplayCopyAllDisplayModes(display_id, 0)
   if(!modes){ return [] }
   mut out = []
   mut i = 0
   def n = int(CFArrayGetCount(modes))
   while(i < n){
      def mode = CFArrayGetValueAtIndex(modes, i)
      def item = _cg_mode_to_dict(mode)
      if(item){ out = out.append(item) }
      i += 1
   }
   CFRelease(modes)
   out
}

fn _find_display_mode(any: display_id, int: width, int: height, int: refresh_rate=0): any {
   if(!available() || !display_id || width <= 0 || height <= 0){ return 0 }
   def modes = CGDisplayCopyAllDisplayModes(display_id, 0)
   if(!modes){ return 0 }
   mut found = 0
   mut i = 0
   def n = int(CFArrayGetCount(modes))
   while(i < n){
      def mode = CFArrayGetValueAtIndex(modes, i)
      if(mode &&
         int(CGDisplayModeGetWidth(mode)) == width &&
         int(CGDisplayModeGetHeight(mode)) == height &&
         (refresh_rate <= 0 || int(CGDisplayModeGetRefreshRate(mode)) == refresh_rate)){
         found = mode
         break
      }
      i += 1
   }
   def keep = found ? found : 0
   if(!keep){ CFRelease(modes) }
   keep ? {"modes": modes, "mode": keep} : 0
}

fn _set_display_mode(any: display_id, int: width, int: height, int: refresh_rate=0): any {
   if(!available() || !display_id || width <= 0 || height <= 0){ return 0 }
   def mode_match = _find_display_mode(display_id, width, height, refresh_rate)
   if(!mode_match){ return 0 }
   def modes = mode_match.get("modes", 0)
   def mode = mode_match.get("mode", 0)
   def previous = CGDisplayCopyDisplayMode(display_id)
   def ok = mode && CGDisplaySetDisplayMode(display_id, mode, 0) == 0
   CFRelease(modes)
   if(ok){ return previous }
   if(previous){ CGDisplayModeRelease(previous) }
   0
}

fn _restore_display_mode(any: win): any {
   if(!available() || !is_dict(win)){ return win }
   def previous = win.get("previous_mode", 0)
   def display_id = win.get("monitor_display_id", 0)
   if(previous && display_id){
      CGDisplaySetDisplayMode(display_id, previous, 0)
      CGDisplayModeRelease(previous)
      win = win.set("previous_mode", 0)
   }
   win
}

fn create_menu(str: title=""): any {
   "Creates and initializes an `NSMenu`."
   if(!available()){ return 0 }
   def cls = get_class("NSMenu")
   if(!cls){ return 0 }
   mut menu = objc_msgSend(cls, get_selector("alloc"))
   if(!menu){ return 0 }
   menu = objc_msgSend(menu, get_selector("init"))
   if(title && is_str(title) && title.len > 0){ objc_msgSend_ptr(menu, get_selector("setTitle:"), objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(title))) }
   menu
}

fn create_menu_item(str: title, any: action=0, str: key_equivalent=""): any {
   "Creates and initializes an `NSMenuItem`."
   if(!available()){ return 0 }
   def cls = get_class("NSMenuItem")
   if(!cls){ return 0 }
   def item = objc_msgSend(cls, get_selector("alloc"))
   if(!item){ return 0 }
   def title_obj = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(to_str(title)))
   def key_obj = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(to_str(key_equivalent)))
   objc_msgSend_ptr_ptr_ptr(item, get_selector("initWithTitle:action:keyEquivalent:"), title_obj, action, key_obj)
}

fn add_menu_item(any: menu, any: item): bool {
   "Adds an item to an `NSMenu`."
   if(!available() || !menu || !item){ return false }
   objc_msgSend_ptr(menu, get_selector("addItem:"), item)
   true
}

fn set_submenu(any: menu, any: submenu, any: item): bool {
   "Assigns a submenu to a parent menu item."
   if(!available() || !menu || !submenu || !item){ return false }
   objc_msgSend_ptr_ptr(menu, get_selector("setSubmenu:forItem:"), submenu, item)
   true
}

fn set_main_menu(any: menu): bool {
   "Sets the shared application's main menu."
   if(!available() || !menu){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_ptr(app, get_selector("setMainMenu:"), menu)
   true
}

fn install_default_menu_bar(str: app_name="Nytrix"): bool {
   "Installs a minimal Cocoa menu bar with a Quit item."
   if(!available()){ return false }
   def main_menu = create_menu()
   if(!main_menu){ return false }
   def app_item = create_menu_item("", 0, "")
   if(!app_item){ return false }
   add_menu_item(main_menu, app_item)
   def app_menu = create_menu(app_name)
   if(!app_menu){ return false }
   def quit_title = "Quit " + to_str(app_name)
   def quit_item = create_menu_item(quit_title, get_selector("terminate:"), "q")
   if(!quit_item){ return false }
   add_menu_item(app_menu, quit_item)
   if(!set_submenu(main_menu, app_menu, app_item)){ return false }
   set_main_menu(main_menu)
}

fn next_event(any: timeout=0): any {
   "Pops the next event from the AppKit queue if available."
   if(!available()){ return 0 }
   def app = shared_application()
   if(!app || !is_ptr(app)){ return 0 }
   def date = (timeout > 0) ? objc_msgSend_ptr_f64(get_class("NSDate"), get_selector("dateWithTimeIntervalSinceNow:"), float(timeout)) :
   objc_msgSend(get_class("NSDate"), get_selector("distantPast"))
   def ev = objc_msgSend_ptr_u64_ptr_ptr_i64(app, get_selector("nextEventMatchingMask:untilDate:inMode:dequeue:"),
   NSEventMaskAny, date, objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr("kCFRunLoopDefaultMode")), 1)
   if(ev){ return _translate_event(ev) }
   0
}

fn wait_events(int: timeout_ms=-1): any {
   "Blocks until an event arrives or the timeout expires."
   def timeout = (timeout_ms < 0) ? 1000000.0 : float(timeout_ms) / 1000.0
   next_event(timeout)
}

fn current_event(): any {
   "Returns the current `NSEvent*` from the shared application."
   if(!available()){ return 0 }
   def app = shared_application()
   if(!app || !is_ptr(app)){ return 0 }
   objc_msgSend(app, get_selector("currentEvent"))
}

fn send_event(any: event): bool {
   "Forwards an AppKit event to `NSApplication`."
   if(!available() || !event){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_ptr(app, get_selector("sendEvent:"), event)
   true
}

fn update_windows(): bool {
   "Runs `-[NSApplication updateWindows]`."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend(app, get_selector("updateWindows"))
   true
}

fn run_app(): bool {
   "Runs the shared AppKit application event loop."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend(app, get_selector("run"))
   true
}

fn stop_app(any: sender=0): bool {
   "Stops the shared AppKit application event loop."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_ptr(app, get_selector("stop:"), sender)
   true
}

fn request_user_attention(int: kind=10): int {
   "Requests user attention via AppKit."
   if(!available()){ return 0 }
   def app = shared_application()
   if(!app){ return 0 }
   int(objc_msgSend_i64_arg(app, get_selector("requestUserAttention:"), kind))
}

fn post_event(any: event, bool: at_start=true): bool {
   "Posts an `NSEvent*` back into the AppKit queue."
   if(!available() || !event){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_ptr_i64(app, get_selector("postEvent:atStart:"), event, at_start ? 1 : 0)
   true
}

mut _windows = dict(8)
mut _pending_events = []
mut _known_display_ids = []
mut _displays_initialized = false

fn _push_event(any: win, int: typ, any: data=0): any {
   if(!win){ return 0 }
   _pending_events = _pending_events.append([typ, win, win.get("handle", 0), data])
}

def _COCOA_KEY_MAP = [[0x00, backend_api.KEY_A], [0x01, backend_api.KEY_S], [0x02, backend_api.KEY_D], [0x03, backend_api.KEY_F], [0x04, backend_api.KEY_H], [0x05, backend_api.KEY_G], [0x06, backend_api.KEY_Z], [0x07, backend_api.KEY_X], [0x08, backend_api.KEY_C], [0x09, backend_api.KEY_V], [0x0A, backend_api.KEY_WORLD_1], [0x0B, backend_api.KEY_B], [0x0C, backend_api.KEY_Q], [0x0D, backend_api.KEY_W], [0x0E, backend_api.KEY_E], [0x0F, backend_api.KEY_R], [0x10, backend_api.KEY_Y], [0x11, backend_api.KEY_T], [0x12, backend_api.KEY_1], [0x13, backend_api.KEY_2], [0x14, backend_api.KEY_3], [0x15, backend_api.KEY_4], [0x16, backend_api.KEY_6], [0x17, backend_api.KEY_5], [0x18, backend_api.KEY_EQUAL], [0x19, backend_api.KEY_9], [0x1A, backend_api.KEY_7], [0x1B, backend_api.KEY_MINUS], [0x1C, backend_api.KEY_8], [0x1D, backend_api.KEY_0], [0x1E, backend_api.KEY_RIGHT_BRACKET], [0x1F, backend_api.KEY_O], [0x20, backend_api.KEY_U], [0x21, backend_api.KEY_LEFT_BRACKET], [0x22, backend_api.KEY_I], [0x23, backend_api.KEY_P], [0x24, backend_api.KEY_ENTER], [0x25, backend_api.KEY_L], [0x26, backend_api.KEY_J], [0x27, backend_api.KEY_APOSTROPHE], [0x28, backend_api.KEY_K], [0x29, backend_api.KEY_SEMICOLON], [0x2A, backend_api.KEY_BACKSLASH], [0x2B, backend_api.KEY_COMMA], [0x2C, backend_api.KEY_SLASH], [0x2D, backend_api.KEY_N], [0x2E, backend_api.KEY_M], [0x2F, backend_api.KEY_PERIOD], [0x30, backend_api.KEY_TAB], [0x31, backend_api.KEY_SPACE], [0x32, backend_api.KEY_GRAVE_ACCENT], [0x33, backend_api.KEY_BACKSPACE], [0x35, backend_api.KEY_ESCAPE], [0x36, backend_api.KEY_RIGHT_SUPER], [0x37, backend_api.KEY_LEFT_SUPER], [0x38, backend_api.KEY_LEFT_SHIFT], [0x39, backend_api.KEY_CAPS_LOCK], [0x3A, backend_api.KEY_LEFT_ALT], [0x3B, backend_api.KEY_LEFT_CONTROL], [0x3C, backend_api.KEY_RIGHT_SHIFT], [0x3D, backend_api.KEY_RIGHT_ALT], [0x3E, backend_api.KEY_RIGHT_CONTROL], [0x3F, backend_api.KEY_LEFT_SUPER], [0x40, backend_api.KEY_F17], [0x41, backend_api.KEY_KP_DECIMAL], [0x43, backend_api.KEY_KP_MULTIPLY], [0x45, backend_api.KEY_KP_ADD], [0x47, backend_api.KEY_NUM_LOCK], [0x4B, backend_api.KEY_KP_DIVIDE], [0x4C, backend_api.KEY_KP_ENTER], [0x4E, backend_api.KEY_KP_SUBTRACT], [0x4F, backend_api.KEY_F18], [0x50, backend_api.KEY_F19], [0x51, backend_api.KEY_KP_EQUAL], [0x52, backend_api.KEY_KP_0], [0x53, backend_api.KEY_KP_1], [0x54, backend_api.KEY_KP_2], [0x55, backend_api.KEY_KP_3], [0x56, backend_api.KEY_KP_4], [0x57, backend_api.KEY_KP_5], [0x58, backend_api.KEY_KP_6], [0x59, backend_api.KEY_KP_7], [0x5A, backend_api.KEY_F20], [0x5B, backend_api.KEY_KP_8], [0x5C, backend_api.KEY_KP_9], [0x60, backend_api.KEY_F5], [0x61, backend_api.KEY_F6], [0x62, backend_api.KEY_F7], [0x63, backend_api.KEY_F3], [0x64, backend_api.KEY_F8], [0x65, backend_api.KEY_F9], [0x67, backend_api.KEY_F11], [0x69, backend_api.KEY_PRINT_SCREEN], [0x6A, backend_api.KEY_F16], [0x6B, backend_api.KEY_F14], [0x6D, backend_api.KEY_F10], [0x6E, backend_api.KEY_MENU], [0x6F, backend_api.KEY_F12], [0x71, backend_api.KEY_F15], [0x72, backend_api.KEY_INSERT], [0x73, backend_api.KEY_HOME], [0x74, backend_api.KEY_PAGE_UP], [0x75, backend_api.KEY_DELETE], [0x76, backend_api.KEY_F4], [0x77, backend_api.KEY_END], [0x78, backend_api.KEY_F2], [0x79, backend_api.KEY_PAGE_DOWN], [0x7A, backend_api.KEY_F1], [0x7B, backend_api.KEY_LEFT], [0x7C, backend_api.KEY_RIGHT], [0x7D, backend_api.KEY_DOWN], [0x7E, backend_api.KEY_UP]]
mut _cocoa_key_dict = 0

fn _cocoa_key_dict_get(): dict {
   if(is_dict(_cocoa_key_dict)){ return _cocoa_key_dict }
   mut d, i = dict(128), 0
   def key_map_n = _COCOA_KEY_MAP.len
   while(i < key_map_n){
      def pair = _COCOA_KEY_MAP.get(i)
      d = d.set(pair.get(0), pair.get(1))
      i += 1
   }
   _cocoa_key_dict = d
   d
}

fn _cocoa_translate_keycode(any: code): int {
   def key = _cocoa_key_dict_get().get(int(code), 0)
   if(key){ return key }
   if(code >= 0 && code <= 9){ return backend_api.KEY_0 + int(code) }
   0
}

fn _mods_from_flags(any: flags): int {
   mut mods = 0
   if(band(flags, NSEventModifierFlagShift) != 0){ mods = bor(mods, backend_api.MOD_SHIFT) }
   if(band(flags, NSEventModifierFlagControl) != 0){ mods = bor(mods, backend_api.MOD_CONTROL) }
   if(band(flags, NSEventModifierFlagOption) != 0){ mods = bor(mods, backend_api.MOD_ALT) }
   if(band(flags, NSEventModifierFlagCommand) != 0){ mods = bor(mods, backend_api.MOD_SUPER) }
   if(band(flags, NSEventModifierFlagCapsLock) != 0){ mods = bor(mods, backend_api.MOD_CAPS_LOCK) }
   mods
}

fn _push_utf8_char_events(any: win, any: utf8, int: mods=0): any {
   if(!utf8){ return 0 }
   mut i = 0
   while(true){
      def b0 = load8(utf8, i)
      if(b0 == 0){ break }
      mut cp = -1
      mut next = i + 1
      if(b0 < 0x80){ cp = b0 } elif(band(b0, 0xe0) == 0xc0 && load8(utf8, i + 1) != 0){
         cp = bor(bshl(band(b0, 0x1f), 6), band(load8(utf8, i + 1), 0x3f))
         next = i + 2
      } elif(band(b0, 0xf0) == 0xe0 && load8(utf8, i + 1) != 0 && load8(utf8, i + 2) != 0){
         cp = bor(bshl(band(b0, 0x0f), 12),
         bor(bshl(band(load8(utf8, i + 1), 0x3f), 6), band(load8(utf8, i + 2), 0x3f)))
         next = i + 3
      } elif(band(b0, 0xf8) == 0xf0 && load8(utf8, i + 1) != 0 && load8(utf8, i + 2) != 0 && load8(utf8, i + 3) != 0){
         cp = bor(bshl(band(b0, 0x07), 18),
            bor(bshl(band(load8(utf8, i + 1), 0x3f), 12),
         bor(bshl(band(load8(utf8, i + 2), 0x3f), 6), band(load8(utf8, i + 3), 0x3f))))
         next = i + 4
      }
      if(cp > 0){
         mut char_data = dict(8)
         char_data = char_data.set("char", cp)
         char_data = char_data.set("mod", mods)
         char_data = char_data.set("mods", mods)
         _push_event(win, EVENT_KEY_CHAR, char_data)
      }
      i = next
   }
}

fn _translate_event(any: event): any {
   "Translates a native Cocoa `NSEvent` into a Nytrix event dict."
   if(!available() || !event){ return 0 }
   def type = int(objc_msgSend_i64(event, get_selector("type")))
   def hwnd = objc_msgSend_ptr(event, get_selector("window"))
   def win = _windows.get(hwnd, 0)
   if(!win){ return 0 }
   match type {
      1, 3, 25 -> { ;; NSEventTypeLeftMouseDown, RightMouseDown, OtherMouseDown
         def btn = (type == 1) ? 0 : (type == 3) ? 1 : int(objc_msgSend_i64(event, get_selector("buttonNumber")))
         mut next_win = win.set(f"mouse_button_{btn}", true)
         _windows = _windows.set(hwnd, next_win)
         return [EVENT_MOUSE_BUTTON_PRESSED, next_win, 0, btn]
      }
      2, 4, 26 -> { ;; NSEventTypeLeftMouseUp, RightMouseUp, OtherMouseUp
         def btn = (type == 2) ? 0 : (type == 4) ? 1 : int(objc_msgSend_i64(event, get_selector("buttonNumber")))
         mut next_win = win.set(f"mouse_button_{btn}", false)
         _windows = _windows.set(hwnd, next_win)
         return [EVENT_MOUSE_BUTTON_RELEASED, next_win, 0, btn]
      }
      5, 6, 7, 27 -> { ;; NSEventTypeMouseMoved, LeftMouseDragged, RightMouseDragged, OtherMouseDragged
         def dx, dy = objc_msgSend_f64(event, get_selector("deltaX")), objc_msgSend_f64(event, get_selector("deltaY"))
         mut mx, my = float(win.get("mouse_x", 0)) + dx, float(win.get("mouse_y", 0)) + dy
         mut next_win = win.set("mouse_x", int(mx))
         next_win = next_win.set("mouse_y", int(my))
         _windows = _windows.set(hwnd, next_win)
         return [EVENT_MOUSE_POS_CHANGED, next_win, 0, [mx, my]]
      }
      10 -> { ;; NSEventTypeKeyDown
         def code = objc_msgSend_i64(event, get_selector("keyCode"))
         def key = _cocoa_translate_keycode(int(code))
         def flags = objc_msgSend_i64(event, get_selector("modifierFlags"))
         def mods = _mods_from_flags(flags)
         def is_repeat = objc_msgSend_i64(event, get_selector("isARepeat")) != 0
         mut next_win = win.set(f"key_{key}", true)
         _windows = _windows.set(hwnd, next_win)
         def data = {
            "key": key,
            "scancode": int(code),
            "action": is_repeat ? backend_api.ACTION_REPEAT : backend_api.ACTION_PRESS,
            "mod": mods,
            "mods": mods
         }
         def nsstr = objc_msgSend_ptr(event, get_selector("characters"))
         if(nsstr){
            def utf8 = objc_msgSend_ptr(nsstr, get_selector("UTF8String"))
            if(utf8){ _push_utf8_char_events(next_win, utf8, mods) }
         }
         return [EVENT_KEY_PRESSED, next_win, 0, data]
      }
      11 -> { ;; NSEventTypeKeyUp
         def code = objc_msgSend_i64(event, get_selector("keyCode"))
         def key = _cocoa_translate_keycode(int(code))
         def flags = objc_msgSend_i64(event, get_selector("modifierFlags"))
         def mods = _mods_from_flags(flags)
         mut next_win = win.set(f"key_{key}", false)
         _windows = _windows.set(hwnd, next_win)
         def data = {
            "key": key,
            "scancode": int(code),
            "action": backend_api.ACTION_RELEASE,
            "mod": mods,
            "mods": mods
         }
         return [EVENT_KEY_RELEASED, next_win, 0, data]
      }
      12 -> { ;; NSEventTypeFlagsChanged — modifier key press/release
         def code = objc_msgSend_i64(event, get_selector("keyCode"))
         def key = _cocoa_translate_keycode(int(code))
         if(!key){ return 0 }
         def flags = objc_msgSend_i64(event, get_selector("modifierFlags"))
         mut modifier_flag = 0
         if(key == backend_api.KEY_LEFT_SHIFT || key == backend_api.KEY_RIGHT_SHIFT){ modifier_flag = NSEventModifierFlagShift }
         elif(key == backend_api.KEY_LEFT_CONTROL || key == backend_api.KEY_RIGHT_CONTROL){ modifier_flag = NSEventModifierFlagControl }
         elif(key == backend_api.KEY_LEFT_ALT || key == backend_api.KEY_RIGHT_ALT){ modifier_flag = NSEventModifierFlagOption }
         elif(key == backend_api.KEY_LEFT_SUPER || key == backend_api.KEY_RIGHT_SUPER){ modifier_flag = NSEventModifierFlagCommand }
         elif(key == backend_api.KEY_CAPS_LOCK){ modifier_flag = NSEventModifierFlagCapsLock }
         def pressed = modifier_flag ? (band(flags, modifier_flag) != 0) : false
         def data = {
            "key": key,
            "scancode": int(code),
            "action": pressed ? backend_api.ACTION_PRESS : backend_api.ACTION_RELEASE,
            "mod": _mods_from_flags(flags),
            "mods": _mods_from_flags(flags)
         }
         def kind = pressed ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED
         mut next_win = _windows.get(hwnd, win)
         next_win = next_win.set(f"key_{key}", pressed)
         _windows = _windows.set(hwnd, next_win)
         return [kind, next_win, 0, data]
      }
      22 -> { ;; NSEventTypeScrollWheel
         def dx = objc_msgSend_f64(event, get_selector("scrollingDeltaX"))
         def dy = objc_msgSend_f64(event, get_selector("scrollingDeltaY"))
         def precise = objc_msgSend_i64(event, get_selector("hasPreciseScrollingDeltas"))
         mut sdx, sdy = dx, dy
         if(!precise){ sdx, sdy = dx * 10.0, dy * 10.0 }
         return [EVENT_MOUSE_SCROLL, win, 0, [sdx, sdy]]
      }
      _ -> { return 0 }
   }
}

fn _find_window_by_handle(any: hwnd): any {
   "Looks up a tracked window by its native NSWindow handle."
   if(!hwnd){ return 0 }
   _windows.get(hwnd, 0)
}

fn _window_delegate_should_close(any: self, any: sel, any: win_obj): bool {
   "NSWindowDelegate windowShouldClose: callback."
   def win = _find_window_by_handle(win_obj)
   if(win){ _push_event(win, 15, 0) }
   true
}

fn _window_delegate_did_resize(any: self, any: sel, any: notification): any {
   "NSWindowDelegate windowDidResize: callback."
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(!win){ return 0 }
   def size = get_size(win)
   _push_event(win, EVENT_WINDOW_RESIZED, size)
}

fn _window_delegate_did_miniaturize(any: self, any: sel, any: notification): any {
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_event(win, EVENT_WINDOW_MINIMIZED, 0) }
}

fn _window_delegate_did_deminiaturize(any: self, any: sel, any: notification): any {
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_event(win, EVENT_WINDOW_RESTORED, 0) }
}

fn _window_delegate_did_become_key(any: self, any: sel, any: notification): any {
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_event(win, EVENT_FOCUS_IN, 0) }
}

fn _window_delegate_did_resign_key(any: self, any: sel, any: notification): any {
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_event(win, EVENT_FOCUS_OUT, 0) }
}

fn _window_delegate_did_change_backing(any: self, any: sel, any: notification): any {
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){
      def scale = get_window_content_scale(win)
      _push_event(win, EVENT_SCALE_UPDATED, scale)
   }
}

fn _window_delegate_did_move(any: self, any: sel, any: notification): any {
   "NSWindowDelegate windowDidMove: callback — updates win[x/y] and emits EVENT_WINDOW_MOVED."
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(!win){ return 0 }
   def rect = _get_nswindow_rect(hwnd, "frame")
   def nx = rect.get(0)
   def ny = rect.get(1)
   mut next_win = win.set("x", nx)
   next_win = next_win.set("y", ny)
   _windows = _windows.set(hwnd, next_win)
   _push_event(next_win, EVENT_WINDOW_MOVED, [nx, ny])
}

fn _ime_insert_text(any: self, any: sel, any: string_obj, any: replacement_range): any {
   "NSTextInputClient insertText:replacementRange: — pushes EVENT_KEY_CHAR for each codepoint."
   if(!string_obj){ return 0 }
   def cs = objc_msgSend_ptr(string_obj, get_selector("UTF8String"))
   if(!cs){ return 0 }
   def hwnd = _last_key_window()
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_utf8_char_events(win, cs) }
}

fn _last_key_window(): any {
   "Returns the NSWindow that currently has keyboard focus."
   def app = shared_application()
   if(!app){ return 0 }
   objc_msgSend_ptr(app, get_selector("keyWindow"))
}

fn _drag_enter(any: self, any: sel, any: sender): int {
   "NSDraggingDestination draggingEntered: — accept copy operation."
   1
}

fn _drag_updated(any: self, any: sel, any: sender): int {
   "NSDraggingDestination draggingUpdated: — keep accepting copy."
   1
}

fn _drag_performed(any: self, any: sel, any: sender): bool {
   "NSDraggingDestination performDragOperation: — reads pasteboard, pushes EVENT_DROP."
   if(!sender){ return false }
   def pb = objc_msgSend_ptr(sender, get_selector("draggingPasteboard"))
   if(!pb){ return false }
   def fn_type = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr("NSFilenamesPboardType"))
   def items = objc_msgSend_ptr(pb, get_selector("propertyListForType:"), fn_type)
   mut paths = []
   if(items){
      def count = objc_msgSend_i64(items, get_selector("count"))
      mut idx = 0
      while(idx < count){
         def item = objc_msgSend_ptr_i64_arg(items, get_selector("objectAtIndex:"), idx)
         if(item){
            def cs = objc_msgSend_ptr(item, get_selector("UTF8String"))
            if(cs){ paths = paths.append(to_str(cs)) }
         }
         idx += 1
      }
   }
   def hwnd = _last_key_window()
   def win = _find_window_by_handle(hwnd)
   if(win && paths.len > 0){
      mut data = dict(8)
      data = data.set("paths", paths)
      _push_event(win, EVENT_DATA_DROP, data)
   }
   paths.len > 0
}

mut _delegate_class = 0

fn create_window_delegate(): any {
   "Creates and registers a dynamic `NytrixWindowDelegate` class."
   if(_delegate_class){ return _delegate_class }
   def base = get_class("NSObject")
   def cls = _ny_objc_allocateClassPair(base, cstr("NytrixWindowDelegate"), 0)
   if(cls){
      _ny_class_addMethod(cls, get_selector("windowShouldClose:"), _window_delegate_should_close, cstr("c@:@"))
      _ny_class_addMethod(cls, get_selector("windowDidResize:"), _window_delegate_did_resize, cstr("v@:@"))
      _ny_class_addMethod(cls, get_selector("windowDidMiniaturize:"), _window_delegate_did_miniaturize, cstr("v@:@"))
      _ny_class_addMethod(cls, get_selector("windowDidDeminiaturize:"), _window_delegate_did_deminiaturize, cstr("v@:@"))
      _ny_class_addMethod(cls, get_selector("windowDidBecomeKey:"), _window_delegate_did_become_key, cstr("v@:@"))
      _ny_class_addMethod(cls, get_selector("windowDidResignKey:"), _window_delegate_did_resign_key, cstr("v@:@"))
      _ny_class_addMethod(cls, get_selector("windowDidChangeBackingProperties:"), _window_delegate_did_change_backing, cstr("v@:@"))
      _ny_class_addMethod(cls, get_selector("windowDidMove:"), _window_delegate_did_move, cstr("v@:@"))
      _ny_class_addMethod(cls, get_selector("insertText:replacementRange:"), _ime_insert_text, cstr("v@:@{NSRange=QQ}"))
      _ny_class_addMethod(cls, get_selector("draggingEntered:"), _drag_enter, cstr("Q@:@"))
      _ny_class_addMethod(cls, get_selector("draggingUpdated:"), _drag_updated, cstr("Q@:@"))
      _ny_class_addMethod(cls, get_selector("performDragOperation:"), _drag_performed, cstr("c@:@"))
      _ny_objc_registerClassPair(cls)
      _delegate_class = cls
   }
   cls
}

fn get_window_content_scale(any: win): f64 {
   "Returns the backing scale factor for a Cocoa window."
   if(!available() || !win || !is_dict(win)){ return 1.0 }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return 1.0 }
   float(objc_msgSend_f64(hwnd, get_selector("backingScaleFactor")))
}

fn get_window_monitor(any: win): any {
   "Returns the monitor dictionary that the Cocoa window is currently on."
   if(!available() || !win || !is_dict(win)){ return 0 }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return 0 }
   def screen = objc_msgSend_ptr(hwnd, get_selector("screen"))
   if(!screen){ return 0 }
   def display_id = get_screen_display_id(screen)
   if(!display_id){ return 0 }
   def monitors = get_monitors()
   mut i = 0
   def monitors_n = monitors.len
   while(i < monitors_n){
      def m = monitors.get(i)
      if(m.get("handle", 0) == display_id){ return m }
      i += 1
   }
   0
}

fn _cocoa_window_style(bool: decorated, bool: resizable, bool: fullscreen=false, bool: transparent=false): int {
   mut style = NSWindowStyleMaskBorderless
   if(!fullscreen && decorated){
      style = bor(style, bor(NSWindowStyleMaskTitled,
         bor(NSWindowStyleMaskClosable, NSWindowStyleMaskMiniaturizable)))
   }
   if(!fullscreen && resizable){ style = bor(style, NSWindowStyleMaskResizable) }
   if(transparent){ style = bor(style, NSWindowStyleMaskFullSizeContentView) }
   style
}

fn _cocoa_window_style_from_flags(int: flags): int {
   _cocoa_window_style(!band(flags, WINDOW_NO_BORDER),
      !band(flags, WINDOW_NO_RESIZE),
      !!band(flags, WINDOW_FULLSCREEN),
      !!band(flags, WINDOW_TRANSPARENT))
}

fn _cocoa_set_frame(any: hwnd, any: x, any: y, any: w, any: h): bool {
   if(!available() || !hwnd){ return false }
   objc_msgSend_rect_u64_i64_i8(hwnd, get_selector("setFrame:display:"),
      float(x), float(y), float(w), float(h), 1, 1, 0)
   true
}

fn _cocoa_apply_style(any: hwnd, int: style): bool {
   if(!available() || !hwnd){ return false }
   objc_msgSend_i64_arg(hwnd, get_selector("setStyleMask:"), style)
   def view = objc_msgSend(hwnd, get_selector("contentView"))
   if(view){ objc_msgSend_ptr(hwnd, get_selector("makeFirstResponder:"), view) }
   true
}

fn _icon_image_width(any: image): int {
   if(!is_dict(image)){ return 0 }
   int(image.get("width", image.get("w", 0)))
}

fn _icon_image_height(any: image): int {
   if(!is_dict(image)){ return 0 }
   int(image.get("height", image.get("h", 0)))
}

fn _icon_image_pixels(any: image): any {
   if(!is_dict(image)){ return 0 }
   image.get("pixels_ptr",
      image.get("pixels",
   image.get("data", 0)))
}

fn _icon_pixel_source_len(any: pixels): int {
   if(is_str(pixels) || is_bytes(pixels) || is_list(pixels) || is_tuple(pixels)){ return pixels.len }
   if(is_ptr(pixels)){ return -1 }
   0
}

fn _icon_pixel_byte(any: pixels, int: index): int {
   if(is_ptr(pixels) || is_str(pixels) || is_bytes(pixels)){ return load8(pixels, index) }
   if(is_list(pixels) || is_tuple(pixels)){ return pixels.get(index, 0) }
   0
}

fn _abs_int(int: x): int { x < 0 ? 0 - x : x }

fn _choose_icon_image(any: images, int: wanted_w=128, int: wanted_h=128): any {
   if(is_dict(images)){ return images }
   if(!is_list(images) && !is_tuple(images)){ return false }
   mut best = false
   mut best_score = 1 << 30
   mut i = 0
   def images_n = images.len
   while(i < images_n){
      def image = images.get(i, 0)
      def iw = _icon_image_width(image)
      def ih = _icon_image_height(image)
      if(iw > 0 && ih > 0){
         def score = _abs_int(iw - wanted_w) * 1000 + _abs_int(ih - wanted_h)
         if(!best || score < best_score){
            best = image
            best_score = score
         }
      }
      i += 1
   }
   best
}

fn _copy_rgba_pixels(any: dst, any: pixels, int: bytes): bool {
   if(!dst || !pixels || bytes <= 0){ return false }
   if(is_ptr(pixels) || is_str(pixels) || is_bytes(pixels)){
      memcpy(dst, pixels, bytes)
      return true
   }
   if(!is_list(pixels) && !is_tuple(pixels)){ return false }
   mut i = 0
   while(i < bytes){
      store8(dst, _icon_pixel_byte(pixels, i), i)
      i += 1
   }
   true
}

fn _cocoa_nsimage_from_rgba(any: image): any {
   if(!available() || !is_dict(image)){ return 0 }
   def width = _icon_image_width(image)
   def height = _icon_image_height(image)
   def pixels = _icon_image_pixels(image)
   def bytes = width * height * 4
   def have = _icon_pixel_source_len(pixels)
   if(width <= 0 || height <= 0 || !pixels || (have >= 0 && have < bytes)){ return 0 }
   def rep_cls = get_class("NSBitmapImageRep")
   def img_cls = get_class("NSImage")
   if(!rep_cls || !img_cls){ return 0 }
   def color_space = objc_msgSend_ptr(get_class("NSString"),
      get_selector("stringWithUTF8String:"), cstr("NSCalibratedRGBColorSpace"))
   def rep = objc_msgSend_bitmap_init(objc_msgSend(rep_cls, get_selector("alloc")),
      get_selector("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:"),
      0, width, height, 8, 4, 1, 0, color_space,
      NSBitmapFormatAlphaNonpremultiplied, width * 4, 32)
   if(!rep){ return 0 }
   def data = objc_msgSend(rep, get_selector("bitmapData"))
   if(!data || !_copy_rgba_pixels(data, pixels, bytes)){
      objc_msgSend(rep, get_selector("release"))
      return 0
   }
   def native = objc_msgSend_size(objc_msgSend(img_cls, get_selector("alloc")),
      get_selector("initWithSize:"), width, height)
   if(!native){
      objc_msgSend(rep, get_selector("release"))
      return 0
   }
   objc_msgSend_ptr(native, get_selector("addRepresentation:"), rep)
   objc_msgSend(rep, get_selector("release"))
   native
}

fn set_window_monitor(any: win, any: monitor, int: xpos, int: ypos, int: width, int: height, int: refresh_rate=0): any {
   "Moves a Cocoa window into borderless monitor mode or restores windowed placement."
   if(!available() || !win || !is_dict(win)){ return win }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return win }
   def pool = create_autorelease_pool()
   if(monitor){
      def display_id = _handle_from(monitor)
      def screen = get_screen_for_display_id(display_id)
      def frame = get_screen_frame(screen)
      def mode = get_video_mode(monitor)
      def fallback_w = frame ? frame.get("width", win.get("w", 1)) : win.get("w", 1)
      def fallback_h = frame ? frame.get("height", win.get("h", 1)) : win.get("h", 1)
      def target_w = width > 0 ? width : (mode ? mode.get("width", fallback_w) : fallback_w)
      def target_h = height > 0 ? height : (mode ? mode.get("height", fallback_h) : fallback_h)
      if(!win.get("monitor", 0)){
         def pos = get_pos(win)
         def size = get_size(win)
         win = win.set("windowed_x", pos.get(0, win.get("x", xpos)))
         win = win.set("windowed_y", pos.get(1, win.get("y", ypos)))
         win = win.set("windowed_w", size.get(0, win.get("w", width)))
         win = win.set("windowed_h", size.get(1, win.get("h", height)))
         win = win.set("windowed_style", int(objc_msgSend_i64(hwnd, get_selector("styleMask"))))
         win = win.set("windowed_level", int(objc_msgSend_i64(hwnd, get_selector("level"))))
      }
      if(display_id && target_w > 0 && target_h > 0 && !win.get("previous_mode", 0)){
         def previous = _set_display_mode(display_id, target_w, target_h, refresh_rate)
         if(previous){ win = win.set("previous_mode", previous) }
      }
      _cocoa_apply_style(hwnd, NSWindowStyleMaskBorderless)
      objc_msgSend_i64_arg(hwnd, get_selector("setLevel:"), NSMainMenuWindowLevel + 1)
      objc_msgSend_i64_arg(hwnd, get_selector("setHasShadow:"), 0)
      def fx = frame ? frame.get("x", xpos) : xpos
      def fy = frame ? frame.get("y", ypos) : ypos
      def fw = frame ? frame.get("width", target_w) : target_w
      def fh = frame ? frame.get("height", target_h) : target_h
      _cocoa_set_frame(hwnd, fx, fy, fw, fh)
      objc_msgSend_ptr(hwnd, get_selector("makeKeyAndOrderFront:"), 0)
      win = win.set("monitor", monitor)
      win = win.set("monitor_display_id", display_id)
      win = win.set("fullscreen", true)
      win = win.set("x", fx).set("y", fy).set("w", fw).set("h", fh)
      win = win.set("flags", bor(int(win.get("flags", 0)), WINDOW_FULLSCREEN))
   } else {
      win = _restore_display_mode(win)
      def decorated = !band(int(win.get("flags", 0)), WINDOW_NO_BORDER)
      def resizable = !band(int(win.get("flags", 0)), WINDOW_NO_RESIZE)
      def transparent = !!band(int(win.get("flags", 0)), WINDOW_TRANSPARENT)
      def style = win.get("windowed_style", _cocoa_window_style(decorated, resizable, false, transparent))
      _cocoa_apply_style(hwnd, style)
      objc_msgSend_i64_arg(hwnd, get_selector("setHasShadow:"), 1)
      objc_msgSend_i64_arg(hwnd, get_selector("setLevel:"), win.get("windowed_level", band(int(win.get("flags", 0)), WINDOW_FLOATING) ? NSFloatingWindowLevel : NSNormalWindowLevel))
      def rx = xpos >= 0 ? xpos : win.get("windowed_x", win.get("x", 0))
      def ry = ypos >= 0 ? ypos : win.get("windowed_y", win.get("y", 0))
      def rw = width > 0 ? width : win.get("windowed_w", win.get("w", 1))
      def rh = height > 0 ? height : win.get("windowed_h", win.get("h", 1))
      _cocoa_set_frame(hwnd, rx, ry, rw, rh)
      win = win.set("monitor", 0)
      win = win.set("monitor_display_id", 0)
      win = win.set("fullscreen", false)
      win = win.set("x", rx).set("y", ry).set("w", rw).set("h", rh)
      win = win.set("flags", band(int(win.get("flags", 0)), bnot(WINDOW_FULLSCREEN)))
   }
   _windows = _windows.set(hwnd, win)
   drain_autorelease_pool(pool)
   win
}

fn get_key_name(any: win, int: key, int: scancode): str {
   "Returns the keyboard-layout key name via the _COCOA_KEY_MAP reverse lookup."
   if(key >= 32 && key <= 126){ return str.chr(key) }
   mut i = 0
   def key_map_n = _COCOA_KEY_MAP.len
   while(i < key_map_n){
      def entry = _COCOA_KEY_MAP.get(i)
      if(entry && entry.get(1) == key){
         def sc = entry.get(0)
         if(sc >= 0 && key >= 32 && key < 127){ return str.chr(key) }
      }
      i += 1
   }
   ""
}

fn set_window_icon(any: win, any: images): bool {
   "Applies a best-effort AppKit application icon from Ny RGBA image dictionaries."
   if(!available()){ return false }
   def pool = create_autorelease_pool()
   def app = shared_application()
   if(!app){
      drain_autorelease_pool(pool)
      return false
   }
   mut ok = true
   if(!images || ((is_list(images) || is_tuple(images)) && images.len == 0)){
      objc_msgSend_ptr(app, get_selector("setApplicationIconImage:"), 0)
   } else {
      def image = _choose_icon_image(images, 128, 128)
      def native = _cocoa_nsimage_from_rgba(image)
      if(native){
         objc_msgSend_ptr(app, get_selector("setApplicationIconImage:"), native)
         objc_msgSend(native, get_selector("release"))
      } else {
         ok = false
      }
   }
   if(ok && is_dict(win)){
      def hwnd = win.get("handle", 0)
      if(hwnd){ _windows = _windows.set(hwnd, win.set("icon_images", images)) }
   }
   drain_autorelease_pool(pool)
   ok
}

fn get_key_state(any: win, int: key): int {
   "Returns the Cocoa key state from the local dictionary."
   if(!win || !is_dict(win)){ return 0 }
   win.get(f"key_{key}", 0)
}

fn get_mouse_button_state(any: win, int: btn): int {
   "Returns the Cocoa mouse button state from the local dictionary."
   if(!win || !is_dict(win)){ return 0 }
   win.get(f"mouse_button_{btn}", 0)
}

fn get_cursor_pos(any: win): list {
   "Returns the current cursor position relative to the Cocoa window."
   if(!available() || !win || !is_dict(win)){ return [0.0, 0.0] }
   [float(win.get("mouse_x", 0)), float(win.get("mouse_y", 0))]
}

fn set_cursor_pos(any: win, any: x, any: y): bool {
   "Warps the cursor to(x, y) in window-local coordinates using CGWarpMouseCursorPosition."
   if(!available() || !win || !is_dict(win)){ return false }
   def win_x, win_y = float(win.get("x", 0)), float(win.get("y", 0))
   def win_h = float(win.get("h", 600))
   def screen_y = win_y + win_h - float(y)
   CGWarpMouseCursorPosition(win_x + float(x), screen_y)
   true
}

fn create_cursor(any: image, int: xhot=0, int: yhot=0): any {
   "Creates an AppKit NSCursor from a Ny RGBA8 image dictionary."
   if(!available()){ return 0 }
   def pool = create_autorelease_pool()
   def native = _cocoa_nsimage_from_rgba(image)
   if(!native){
      drain_autorelease_pool(pool)
      return 0
   }
   def cls = get_class("NSCursor")
   mut handle = 0
   if(cls){
      handle = objc_msgSend_ptr_point(objc_msgSend(cls, get_selector("alloc")),
         get_selector("initWithImage:hotSpot:"), native, xhot, yhot)
   }
   objc_msgSend(native, get_selector("release"))
   drain_autorelease_pool(pool)
   if(!handle){ return 0 }
   mut c = dict(8)
   c = c.set("handle", handle)
   c = c.set("shared", false)
   c = c.set("image", image)
   c = c.set("xhot", xhot)
   c = c.set("yhot", yhot)
   c
}

fn create_standard_cursor(int: shape): any {
   "Creates an AppKit-backed standard cursor object."
   if(!available()){ return 0 }
   def cls = get_class("NSCursor")
   if(!cls){ return 0 }
   mut private_sel = ""
   match shape {
      RESIZE_EW_CURSOR -> { private_sel = "_windowResizeEastWestCursor" }
      RESIZE_NS_CURSOR -> { private_sel = "_windowResizeNorthSouthCursor" }
      RESIZE_NWSE_CURSOR -> { private_sel = "_windowResizeNorthWestSouthEastCursor" }
      RESIZE_NESW_CURSOR -> { private_sel = "_windowResizeNorthEastSouthWestCursor" }
      _ -> { private_sel = "" }
   }
   mut handle = 0
   if(private_sel != ""){
      def priv = get_selector(private_sel)
      if(priv && objc_msgSend_i64_sel(cls, get_selector("respondsToSelector:"), priv) != 0){
         handle = objc_msgSend_sel(cls, get_selector("performSelector:"), priv)
      }
   }
   mut sel = ""
   if(!handle){
      match shape {
         ARROW_CURSOR -> { sel = "arrowCursor" }
         IBEAM_CURSOR -> { sel = "IBeamCursor" }
         CROSSHAIR_CURSOR -> { sel = "crosshairCursor" }
         POINTING_HAND_CURSOR -> { sel = "pointingHandCursor" }
         RESIZE_EW_CURSOR -> { sel = "resizeLeftRightCursor" }
         RESIZE_NS_CURSOR -> { sel = "resizeUpDownCursor" }
         RESIZE_NWSE_CURSOR -> { sel = "closedHandCursor" }
         RESIZE_NESW_CURSOR -> { sel = "closedHandCursor" }
         RESIZE_ALL_CURSOR -> { sel = "openHandCursor" }
         NOT_ALLOWED_CURSOR -> { sel = "operationNotAllowedCursor" }
         _ -> { sel = "arrowCursor" }
      }
      handle = objc_msgSend_ptr(cls, get_selector(sel), 0)
   }
   if(!handle){ return 0 }
   mut c = dict(8)
   c = c.set("handle", handle)
   c = c.set("shared", true)
   c = c.set("shape", shape)
   c
}

fn destroy_cursor(any: cursor): bool {
   "Destroys a Cocoa cursor object when it owns native resources."
   if(!cursor || !is_dict(cursor)){ return true }
   if(!cursor.get("shared", false)){
      def handle = cursor.get("handle", 0)
      if(handle){ objc_msgSend(handle, get_selector("release")) }
   }
   true
}

fn set_cursor(any: win, any: cursor): bool {
   "Applies an AppKit cursor."
   if(!available()){ return false }
   def handle = _handle_from(cursor)
   if(handle){
      objc_msgSend_ptr(handle, get_selector("set"), 0)
      true
   } else { false }
}

fn show_window(any: win): bool {
   "Show a native Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   _windows = _windows.set(hwnd, 0)
   objc_msgSend_ptr(hwnd, get_selector("makeKeyAndOrderFront:"), 0)
   def next_win = win.set("visible", true)
   _windows = _windows.set(hwnd, next_win)
   drain_autorelease_pool(pool)
   true
}

fn hide_window(any: win): bool {
   "Hide a native Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   objc_msgSend_ptr(hwnd, get_selector("orderOut:"), 0)
   _windows = _windows.set(hwnd, win.set("visible", false))
   drain_autorelease_pool(pool)
   true
}

fn iconify_window(any: win): bool {
   "Iconify a native Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   objc_msgSend_ptr(hwnd, get_selector("miniaturize:"), 0)
   drain_autorelease_pool(pool)
   true
}

fn maximize_window(any: win): bool {
   "Maximize a native Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   if(objc_msgSend_i64(hwnd, get_selector("isZoomed")) == 0){ objc_msgSend_ptr(hwnd, get_selector("zoom:"), 0) }
   drain_autorelease_pool(pool)
   true
}

fn restore_window(any: win): bool {
   "Restore a native Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   if(objc_msgSend_i64(hwnd, get_selector("isMiniaturized")) != 0){ objc_msgSend_ptr(hwnd, get_selector("deminiaturize:"), 0) } elif(objc_msgSend_i64(hwnd, get_selector("isZoomed")) != 0){ objc_msgSend_ptr(hwnd, get_selector("zoom:"), 0) }
   drain_autorelease_pool(pool)
   true
}

fn set_title(any: win, str: title): bool {
   "Updates the Cocoa native window title."
   if(!available() || !win || !is_dict(win) || !title){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   objc_msgSend_ptr(hwnd, get_selector("setTitle:"),
   objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(to_str(title))))
   drain_autorelease_pool(pool)
   true
}

fn set_window_title(any: win, str: title): bool { set_title(win, title) }

fn get_window_attrib(any: win, int: attrib): int {
   "Returns the value of the specified window attribute."
   if(!available() || !win || !is_dict(win)){ return 0 }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return 0 }
   match attrib {
      FOCUSED -> { return objc_msgSend_i64(hwnd, get_selector("isKeyWindow")) != 0 ? 1 : 0 }
      ICONIFIED -> { return objc_msgSend_i64(hwnd, get_selector("isMiniaturized")) != 0 ? 1 : 0 }
      MAXIMIZED -> { return objc_msgSend_i64(hwnd, get_selector("isZoomed")) != 0 ? 1 : 0 }
      VISIBLE -> { return objc_msgSend_i64(hwnd, get_selector("isVisible")) != 0 ? 1 : 0 }
      RESIZABLE -> {
         def style = objc_msgSend_u64(hwnd, get_selector("styleMask"))
         return band(style, NSWindowStyleMaskResizable) != 0 ? 1 : 0
      }
      DECORATED -> {
         def style = objc_msgSend_u64(hwnd, get_selector("styleMask"))
         return band(style, NSWindowStyleMaskTitled) != 0 ? 1 : 0
      }
      FLOATING -> {
         return objc_msgSend_i64(hwnd, get_selector("level")) > 0 ? 1 : 0
      }
      _ -> { return 0 }
   }
}

fn set_window_opacity(any: win, f64: opacity): bool {
   "Sets the whole-window alpha value for a Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   objc_msgSend_arg_f64(hwnd, get_selector("setAlphaValue:"), float(opacity))
   true
}

fn set_window_resizable(any: win, bool: enabled): bool {
   "Toggles the Cocoa window's resizable style mask bit."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   mut style = int(objc_msgSend_i64(hwnd, get_selector("styleMask")))
   if(enabled){ style = bor(style, NSWindowStyleMaskResizable) }
   else { style = band(style, bnot(NSWindowStyleMaskResizable)) }
   objc_msgSend_i64_arg(hwnd, get_selector("setStyleMask:"), style)
   _windows = _windows.set(hwnd, win.set("resizable", enabled))
   true
}

fn set_window_decorated(any: win, bool: enabled): bool {
   "Toggles window decorations for a Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   mut style = int(objc_msgSend_i64(hwnd, get_selector("styleMask")))
   if(enabled){
      style = bor(style, bor(NSWindowStyleMaskTitled, NSWindowStyleMaskClosable))
      style = band(style, bnot(NSWindowStyleMaskBorderless))
   } else {
      style = bor(style, NSWindowStyleMaskBorderless)
      style = band(style, bnot(bor(NSWindowStyleMaskTitled, NSWindowStyleMaskClosable)))
   }
   objc_msgSend_i64_arg(hwnd, get_selector("setStyleMask:"), style)
   def view = objc_msgSend(hwnd, get_selector("contentView"))
   if(view){ objc_msgSend_ptr(hwnd, get_selector("makeFirstResponder:"), view) }
   _windows = _windows.set(hwnd, win.set("decorated", enabled))
   true
}

fn set_window_floating(any: win, bool: enabled): bool {
   "Toggles the always-on-top state of a Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def level = enabled ? NSFloatingWindowLevel : NSNormalWindowLevel
   objc_msgSend_i64_arg(hwnd, get_selector("setLevel:"), level)
   _windows = _windows.set(hwnd, win.set("floating", enabled))
   true
}

fn _get_nswindow_rect(any: win, str: key_name): list {
   if(!available() || !win){ return [0, 0, 0, 0] }
   def hwnd = _handle_from(win)
   if(!hwnd){ return [0, 0, 0, 0] }
   def pool = create_autorelease_pool()
   def key = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(key_name))
   def boxed = objc_msgSend_ptr(hwnd, get_selector("valueForKey:"), key)
   if(!boxed){
      drain_autorelease_pool(pool)
      return [0, 0, 0, 0]
   }
   def desc = objc_msgSend(boxed, get_selector("description"))
   def utf8 = objc_msgSend(desc, get_selector("UTF8String"))
   def res = _rect_dict_from_string(to_str(utf8))
   drain_autorelease_pool(pool)
   if(!res){ return [0, 0, 0, 0] }
   [res.get("x", 0), res.get("y", 0), res.get("width", 0), res.get("height", 0)]
}

fn get_pos(any: win): list {
   "Returns the bottom-left Cocoa screen coordinates [x, y] of a window's client area."
   def rect = _get_nswindow_rect(win, "contentLayoutRect")
   def x = rect.get(0)
   def y = rect.get(1)
   if(x == 0 && y == 0 && is_dict(win)){ return [win.get("x", 0), win.get("y", 0)] }
   [x, y]
}

fn set_pos(any: win, int: x, int: y): bool {
   "Moves a Cocoa window so its client area starts at screen coordinates [x, y]."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def current = _get_nswindow_rect(win, "frame")
   def w = current.get(2)
   def h = current.get(3)
   objc_msgSend_rect_u64_i64_i8(hwnd, get_selector("setFrame:display:"), float(x), float(y), float(w), float(h), 1, 1, 0)
   true
}

fn get_size(any: win): list {
   "Returns the pixel size [width, height] of a Cocoa window's client area."
   def rect = _get_nswindow_rect(win, "contentLayoutRect")
   def w = rect.get(2)
   def h = rect.get(3)
   if((w <= 0 || h <= 0) && is_dict(win)){ return [win.get("w", 0), win.get("h", 0)] }
   [w, h]
}

fn set_size(any: win, int: w, int: h): bool {
   "Resizes a Cocoa window so its client area has width `w` and height `h`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   def current = _get_nswindow_rect(win, "frame")
   def x = current.get(0)
   def y = current.get(1)
   objc_msgSend_rect_u64_i64_i8(hwnd, get_selector("setFrame:display:"), float(x), float(y), float(w), float(h), 1, 1, 0)
   true
}

fn get_framebuffer_size(any: win): list {
   "Returns the backing pixel size of the Cocoa window's content view."
   def size = get_size(win)
   def scale = get_window_content_scale(win)
   [int(float(size.get(0)) * scale), int(float(size.get(1)) * scale)]
}

fn create_basic_window(str: title, int: width, int: height, int: x=0, int: y=0, int: flags=0): any {
   "Create a native `NSWindow` through AppKit."
   if(!available()){ return 0 }
   def pool = create_autorelease_pool()
   def cls = get_class("NSWindow")
   if(!cls){
      drain_autorelease_pool(pool)
      return 0
   }
   def win_obj = objc_msgSend(cls, get_selector("alloc"))
   if(!win_obj){
      drain_autorelease_pool(pool)
      return 0
   }
   def style = _cocoa_window_style_from_flags(flags)
   def hwnd = objc_msgSend_rect_u64_i64_i8(win_obj, get_selector("initWithContentRect:styleMask:backing:defer:"),
   float(x), float(y), float(width), float(height), style, 2, 0)
   mut delegate = 0
   if(hwnd){
      objc_msgSend_ptr(hwnd, get_selector("setTitle:"),
      objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(to_str(title))))
      def delegate_cls = create_window_delegate()
      delegate = objc_msgSend(objc_msgSend(delegate_cls, get_selector("alloc")), get_selector("init"))
      objc_msgSend_ptr(hwnd, get_selector("setDelegate:"), delegate)
      objc_msgSend_ptr_i64(hwnd, get_selector("setAcceptsMouseMovedEvents:"), 1)
      if(band(flags, WINDOW_TRANSPARENT)){ objc_msgSend_i64_arg(hwnd, get_selector("setOpaque:"), 0) }
      if(band(flags, WINDOW_FLOATING)){ objc_msgSend_i64_arg(hwnd, get_selector("setLevel:"), NSFloatingWindowLevel) }
      if(!band(flags, WINDOW_HIDE)){ objc_msgSend_ptr(hwnd, get_selector("makeKeyAndOrderFront:"), 0) }
      if(band(flags, WINDOW_MINIMIZE)){ objc_msgSend_ptr(hwnd, get_selector("miniaturize:"), 0) }
      if(band(flags, WINDOW_MAXIMIZE)){ objc_msgSend_ptr(hwnd, get_selector("zoom:"), 0) }
   }
   drain_autorelease_pool(pool)
   def win = {
      "handle": hwnd,
      "title": to_str(title),
      "x": x,
      "y": y,
      "w": width,
      "h": height,
      "flags": flags,
      "decorated": !band(flags, WINDOW_NO_BORDER),
      "resizable": !band(flags, WINDOW_NO_RESIZE),
      "floating": !!band(flags, WINDOW_FLOATING),
      "fullscreen": !!band(flags, WINDOW_FULLSCREEN),
      "visible": !band(flags, WINDOW_HIDE),
      "delegate": delegate
   }
   _windows = _windows.set(hwnd, win)
   win
}

fn destroy_basic_window(any: win): bool {
   "Closes and releases a native `NSWindow`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = win.get("handle", 0)
   if(!hwnd){ return false }
   win = _restore_display_mode(win)
   _windows = _windows.set(hwnd, 0)
   objc_msgSend_ptr(hwnd, get_selector("setDelegate:"), 0)
   objc_msgSend_ptr(hwnd, get_selector("orderOut:"), 0)
   objc_msgSend(hwnd, get_selector("close"))
   def delegate = win.get("delegate", 0)
   if(delegate){ objc_msgSend(delegate, get_selector("release")) }
   true
}

fn _check_monitor_changes(): any {
   "Diffs current display IDs against known list; emits CONNECTED/DISCONNECTED to all windows."
   def ids = get_active_display_ids()
   if(!_displays_initialized){
      _known_display_ids = ids
      _displays_initialized = true
      return 0
   }
   mut cur_set = dict(8)
   mut i = 0
   def ids_n = ids.len
   while(i < ids_n){
      cur_set = cur_set.set(ids.get(i), 1)
      i += 1
   }
   mut old_set = dict(8)
   i = 0
   def known_display_ids_n = _known_display_ids.len
   while(i < known_display_ids_n){
      old_set = old_set.set(_known_display_ids.get(i), 1)
      i += 1
   }
   i = 0
   while(i < ids_n){
      def did = ids.get(i)
      if(!old_set.contains(did)){
         mut data = dict(8)
         data = data.set("display_id", did)
         def keys = dict_keys(_windows)
         mut j = 0
         def keys_n = keys.len
         while(j < keys_n){
            def w = _windows.get(keys.get(j), 0)
            if(is_dict(w)){ _push_event(w, EVENT_MONITOR_CONNECTED, data) }
            j += 1
         }
      }
      i += 1
   }
   i = 0
   while(i < known_display_ids_n){
      def did = _known_display_ids.get(i)
      if(!cur_set.contains(did)){
         mut data = dict(8)
         data = data.set("display_id", did)
         def keys = dict_keys(_windows)
         mut j = 0
         def keys_n = keys.len
         while(j < keys_n){
            def w = _windows.get(keys.get(j), 0)
            if(is_dict(w)){ _push_event(w, EVENT_MONITOR_DISCONNECTED, data) }
            j += 1
         }
      }
      i += 1
   }
   _known_display_ids = ids
}

fn poll_window_events(any: win, int: max_events=64): list {
   "Drains Cocoa AppKit events for the given window and returns translated Nytrix events."
   if(!available()){ return [win, []] }
   _check_monitor_changes()
   def app = shared_application()
   if(!app){ return [win, []] }
   def mode_str = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr("kCFRunLoopDefaultMode"))
   def past_date = objc_msgSend(get_class("NSDate"), get_selector("distantPast"))
   mut drained = 0
   while(drained < max_events){
      def ev = objc_msgSend_ptr_u64_ptr_ptr_i64(app, get_selector("nextEventMatchingMask:untilDate:inMode:dequeue:"),
      NSEventMaskAny, past_date, mode_str, 1)
      if(!ev){ break }
      def translated = _translate_event(ev)
      if(translated && is_list(translated)){
         def ev_win = translated.get(1, 0)
         if(ev_win){ _push_event(ev_win, translated.get(0, 0), translated.get(3, 0)) }
      }
      drained += 1
   }
   def handle = win.get("handle", 0)
   mut out = []
   mut remaining = []
   mut i = 0
   def pending_events_n = _pending_events.len
   while(i < pending_events_n){
      def ev = _pending_events.get(i)
      def ev_handle = ev.get(2, 0)
      def ev_win = ev.get(1, 0)
      def matched = ev_handle == handle || (is_dict(ev_win) && ev_win.get("handle", 0) == handle)
      if(matched && out.len < max_events){ out = out.append(ev) } else { remaining = remaining.append(ev) }
      i += 1
   }
   _pending_events = remaining
   def next_win = _windows.get(handle, win)
   [next_win, out]
}

fn vulkan_supported(): bool {
   "Returns true if the Cocoa backend supports Vulkan(requires MoltenVK)."
   true
}

fn vulkan_required_extensions(): list {
   "Returns the Vulkan instance extensions required for a macOS surface."
   if(!_cocoa_vk_ext_ptrs){
      _cocoa_vk_ext_surface = cstr("VK_KHR_surface")
      _cocoa_vk_ext_metal = cstr("VK_EXT_metal_surface")
      def arr = malloc(16)
      if(!arr){ return [0, 0] }
      store64_h(arr, _cocoa_vk_ext_surface, 0)
      store64_h(arr, _cocoa_vk_ext_metal, 8)
      _cocoa_vk_ext_ptrs = [2, arr]
   }
   _cocoa_vk_ext_ptrs
}

mut _cocoa_vk_ext_ptrs = 0
mut _cocoa_vk_ext_surface = 0
mut _cocoa_vk_ext_metal = 0

fn get_cocoa_window(any: win): any {
   "Returns the native NSWindow pointer for the given window dict."
   if(is_dict(win)){ win.get("handle", 0) } else { win }
}

fn get_cocoa_monitor(any: mon): any {
   "Returns the CGDirectDisplayID for the given monitor dict."
   if(is_dict(mon)){ mon.get("handle", 0) } else { 0 }
}

fn get_cocoa_view(any: win): any {
   "Returns the NSView(content view) for the given window."
   def hwnd = _handle_from(win)
   if(!hwnd){ return 0 }
   objc_msgSend_ptr(hwnd, get_selector("contentView"))
}
