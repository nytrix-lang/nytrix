;; Keywords: ui window cocoa
;; Low-level Cocoa entry helpers for the in-progress native backend.

module std.ui.window.platform.cocoa (
   available, get_backend_name,
   get_class, get_selector, shared_application,
   create_autorelease_pool, drain_autorelease_pool,
   set_activation_policy_regular, finish_launching, activate_ignoring_other_apps, install_app_delegate,
   get_monitors, get_primary_monitor,
   get_monitor_pos, get_monitor_workarea,
   get_monitor_physical_size, get_monitor_content_scale,
   get_monitor_name, get_video_mode, get_video_modes,
   get_gamma_ramp, set_gamma_ramp,
   current_event, next_event, wait_events, send_event, update_windows, run_app, stop_app, request_user_attention, post_event,
   get_window_monitor, set_window_monitor,
   get_key_state, get_mouse_button_state, get_key_name,
   get_cursor_pos, set_cursor_pos,
   create_cursor, create_standard_cursor, destroy_cursor, set_cursor,
   set_window_title, set_window_opacity, set_window_resizable, set_window_icon,
   get_pos, set_pos, get_size, set_size, get_framebuffer_size,
   create_basic_window, destroy_basic_window, poll_window_events,
   show_window, hide_window, iconify_window, restore_window, maximize_window,
   vulkan_supported, vulkan_required_extensions,
   focus_window, set_clipboard, get_clipboard, set_input_mode, create_surface,
   objc_msgSend, objc_msgSend_ptr, objc_msgSend_ptr_ptr, objc_msgSend_ptr_i64,
   get_cocoa_window, get_cocoa_monitor, get_cocoa_view
)

use std.core *
use std.str as str
use std.ui.window.consts *
use std.ui.window.platform.api as backend_api
use std.ui.gfx.vk.bindings as vkb

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

if(comptime{ __os_name() == "macos" }){
   #include <objc/objc.h>
   #include <objc/objc-runtime.h>
   #include <AppKit/AppKit.h>
   #include <ApplicationServices/ApplicationServices.h>
}

fn objc_msgSend(target, op){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend(target, op) }
   0
}
fn objc_msgSend_ptr(target, op, arg=0){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_ptr(target, op, arg) }
   0
}
fn objc_msgSend_ptr_ptr(target, op, a1=0, a2=0){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_ptr_ptr(target, op, a1, a2) }
   0
}
fn objc_msgSend_ptr_ptr_ptr(target, op, a1=0, a2=0, a3=0){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_ptr_ptr_ptr(target, op, a1, a2, a3) }
   0
}
fn objc_msgSend_ptr_u64_ptr_ptr_i64(target, op, a1=0, a2=0, a3=0, a4=0){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_ptr_u64_ptr_ptr_i64(target, op, a1, a2, a3, a4) }
   0
}
fn objc_msgSend_ptr_i64(target, op, a1=0, a2=0){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_ptr_i64(target, op, a1, a2) }
   0
}
fn objc_msgSend_f64(target, op){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_f64(target, op) }
   0.0
}
fn objc_msgSend_arg_f64(target, op, arg){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_arg_f64(target, op, arg) }
   0
}
fn objc_msgSend_ptr_f64(target, op, arg){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_arg_f64(target, op, float(arg)) }
   0
}
fn objc_msgSend_rect_u64_i64_i8(target, op, x, y, w, h, style, backing, do_defer){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_rect_u64_i64_i8(target, op, x, y, w, h, style, backing, do_defer) }
   0
}
fn objc_msgSend_i64(target, op){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_i64(target, op) }
   0
}
fn objc_msgSend_i64_arg(target, op, arg){
   if(comptime{ __os_name() == "macos" }){ return _objc_msgSend_i64_arg(target, op, arg) }
   0
}

fn get_class(name){
   "Looks up an Objective-C class by name."
   if(comptime{ __os_name() == "macos" }){ return objc_getClass(cstr(name)) }
   0
}

fn get_selector(name){
   "Looks up an Objective-C selector by name."
   if(comptime{ __os_name() == "macos" }){ return sel_registerName(cstr(name)) }
   0
}

fn shared_application(){
   "Returns `[NSApplication sharedApplication]` when AppKit is available."
   if(!available()){ return 0 }
   def cls = get_class("NSApplication")
   if(!cls){ return 0 }
   objc_msgSend(cls, get_selector("sharedApplication"))
}

fn create_autorelease_pool(){
   "Creates a local `NSAutoreleasePool` for native Cocoa work."
   if(!available()){ return 0 }
   def cls = get_class("NSAutoreleasePool")
   if(!cls){ return 0 }
   def pool = objc_msgSend(cls, get_selector("alloc"))
   if(!pool){ return 0 }
   objc_msgSend(pool, get_selector("init"))
}

fn drain_autorelease_pool(pool){
   "Drains a previously created `NSAutoreleasePool`."
   if(!available() || !pool){ return false }
   objc_msgSend(pool, get_selector("drain"))
   true
}

fn set_activation_policy_regular(){
   "Sets the app activation policy to regular AppKit-window behavior."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_i64_arg(app, get_selector("setActivationPolicy:"), NSApplicationActivationPolicyRegular) != 0
}

fn _app_delegate_should_terminate(self, sel, sender){
   "NSApplicationDelegate applicationShouldTerminate: — broadcasts QUIT to all windows."
   def keys = dict_keys(_windows)
   mut i = 0
   while(i < len(keys)){
      def w = dict_get(_windows, get(keys, i), 0)
      if(is_dict(w)){ _push_event(w, EVENT_QUIT, 0) }
      i += 1
   }
   1 ;; NSTerminateNow
}

fn _app_delegate_did_finish_launching(self, sel, notification){
   "NSApplicationDelegate applicationDidFinishLaunching: — activates the application."
   def app = shared_application()
   if(app){ objc_msgSend_i64_arg(app, get_selector("activateIgnoringOtherApps:"), 1) }
}

mut _app_delegate_class = 0

fn _create_app_delegate(){
   "Creates and registers a dynamic NytrixAppDelegate NSApplicationDelegate class."
   if(_app_delegate_class){ return _app_delegate_class }
   if(!available()){ return 0 }
   def base = get_class("NSObject")
   def cls = objc_allocateClassPair(base, cstr("NytrixAppDelegate"), 0)
   if(cls){
      class_addMethod(cls, get_selector("applicationShouldTerminate:"), _app_delegate_should_terminate, cstr("l@:@"))
      class_addMethod(cls, get_selector("applicationDidFinishLaunching:"), _app_delegate_did_finish_launching, cstr("v@:@"))
      objc_registerClassPair(cls)
      _app_delegate_class = cls
   }
   cls
}

fn install_app_delegate(){
   "Installs a NytrixAppDelegate as the NSApplication delegate if not already set."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   def existing = objc_msgSend(app, get_selector("delegate"))
   if(existing){ return true }
   def cls = _create_app_delegate()
   if(!cls){ return false }
   def delegate = objc_msgSend(objc_msgSend(cls, get_selector("alloc")), get_selector("init"))
   if(!delegate){ return false }
   objc_msgSend_ptr(app, get_selector("setDelegate:"), delegate)
   true
}

fn finish_launching(){
   "Finishes AppKit startup on the shared application."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   install_app_delegate()
   objc_msgSend(app, get_selector("finishLaunching"))
   true
}

fn activate_ignoring_other_apps(v=true){
   "Activates the current app similarly to GLFW's Cocoa bootstrap."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_i64_arg(app, get_selector("activateIgnoringOtherApps:"), v ? 1 : 0)
   true
}

fn get_screens(){
   "Returns the `NSScreen` array."
   if(!available()){ return 0 }
   def cls = get_class("NSScreen")
   if(!cls){ return 0 }
   objc_msgSend(cls, get_selector("screens"))
}

fn get_screen_count(screens=0){
   "Returns the number of active Cocoa screens."
   def arr = screens ? screens : get_screens()
   if(!arr){ return 0 }
   int(objc_msgSend_i64(arr, get_selector("count")))
}

fn get_screen_at(index, screens=0){
   "Returns the screen object at `index`."
   def arr = screens ? screens : get_screens()
   if(!arr || index < 0){ return 0 }
   objc_msgSend_i64_arg(arr, get_selector("objectAtIndex:"), index)
}

fn get_screen_name(screen){
   "Returns an `NSScreen` localized name when available."
   if(!available() || !screen){ return "" }
   def name_obj = objc_msgSend(screen, get_selector("localizedName"))
   if(!name_obj){ return "" }
   def utf8 = objc_msgSend(name_obj, get_selector("UTF8String"))
   if(!utf8){ return "" }
   to_str(utf8)
}

fn get_screen_display_id(screen){
   "Returns the CoreGraphics display id for the given `NSScreen`."
   if(!available() || !screen){ return 0 }
   def key = objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr("NSScreenNumber"))
   if(!key){ return 0 }
   def number = objc_msgSend_ptr(screen, get_selector("valueForKey:"), key)
   if(!number){ return 0 }
   int(objc_msgSend_i64(number, get_selector("unsignedIntValue")))
}

fn get_screen_for_display_id(display_id, screens=0){
   "Returns the `NSScreen` matching the given CoreGraphics display id."
   def arr = screens ? screens : get_screens()
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

fn get_display_unit_number(display_id){
   "Returns the CoreGraphics unit number for a display id."
   if(!available() || !display_id){ return 0 }
   int(CGDisplayUnitNumber(display_id))
}

fn get_screen_for_unit_number(unit_number, screens=0){
   "Returns the `NSScreen` matching the given CoreGraphics unit number."
   def arr = screens ? screens : get_screens()
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

fn _rect_dict_from_string(s){
   if(!s || len(s) == 0){ return false }
   mut vals = []
   mut tok = ""
   mut i = 0
   while(i < len(s)){
      def c = load8(s, i)
      def numeric = (c >= 48 && c <= 57) || c == 45 || c == 46 || c == 43
      if(numeric){
         tok = tok + chr(c)
      } elif(len(tok) > 0){
         vals = append(vals, str.atof(tok))
         tok = ""
         if(len(vals) >= 4){ break }
      }
      i += 1
   }
   if(len(tok) > 0 && len(vals) < 4){
      vals = append(vals, str.atof(tok))
   }
   if(len(vals) < 4){ return false }
   mut out = dict()
   out = dict_set(out, "x", int(get(vals, 0, 0.0)))
   out = dict_set(out, "y", int(get(vals, 1, 0.0)))
   out = dict_set(out, "width", int(get(vals, 2, 0.0)))
   out = dict_set(out, "height", int(get(vals, 3, 0.0)))
   out
}

fn _screen_rect_for_key(screen, key_name){
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

fn get_screen_frame(screen){
   "Returns the Cocoa screen frame as `{x,y,width,height}` when available."
   _screen_rect_for_key(screen, "frame")
}

fn get_screen_visible_frame(screen){
   "Returns the Cocoa visible frame/workarea as `{x,y,width,height}` when available."
   _screen_rect_for_key(screen, "visibleFrame")
}

fn get_screen_scale(screen){
   "Returns the AppKit backing scale factor for an `NSScreen`."
   if(!available() || !screen){ return 1.0 }
   def scale = objc_msgSend_f64(screen, get_selector("backingScaleFactor"))
   scale > 0.0 ? scale : 1.0
}

fn get_monitors(){
   "Returns a list of dictionaries representing active Cocoa monitors."
   def ids = get_active_display_ids()
   mut out = []
   mut i = 0
   while(i < len(ids)){
      def display_id = get(ids, i, 0)
      mut m = dict()
      m = dict_set(m, "handle", display_id)
      m = dict_set(m, "id", display_id)
      m = dict_set(m, "name", get_monitor_name(m))
      out = append(out, m)
      i += 1
   }
   out
}

fn get_primary_monitor(){
   "Returns the first available Cocoa monitor."
   def ms = get_monitors()
   len(ms) > 0 ? get(ms, 0) : 0
}

fn get_monitor_name(monitor){
   "Returns the localized name for a Cocoa monitor handle."
   if(!monitor){ return "Unknown Monitor" }
   def display_id = is_dict(monitor) ? dict_get(monitor, "handle", 0) : monitor
   def screen = get_screen_for_display_id(display_id)
   screen ? get_screen_name(screen) : "Unknown Monitor"
}

fn get_monitor_pos(monitor){
   "Returns the screen position [x, y] of a Cocoa monitor."
   if(!monitor){ return [0, 0] }
   def display_id = is_dict(monitor) ? dict_get(monitor, "handle", 0) : monitor
   def screen = get_screen_for_display_id(display_id)
   def frame = get_screen_frame(screen)
   frame ? [dict_get(frame, "x", 0), dict_get(frame, "y", 0)] : [0, 0]
}

fn get_monitor_workarea(monitor){
   "Returns the visible workarea [x, y, w, h] of a Cocoa monitor."
   if(!monitor){ return [0, 0, 0, 0] }
   def display_id = is_dict(monitor) ? dict_get(monitor, "handle", 0) : monitor
   def screen = get_screen_for_display_id(display_id)
   def frame = get_screen_visible_frame(screen)
   frame ? [dict_get(frame, "x", 0), dict_get(frame, "y", 0), dict_get(frame, "width", 0), dict_get(frame, "height", 0)] : [0, 0, 0, 0]
}

fn get_monitor_physical_size(monitor){
   "Returns the approximate physical size [mm_w, mm_h] of a Cocoa monitor."
   [0, 0] ;; CGDisplayScreenSize could be used here
}

fn get_monitor_content_scale(monitor){
   "Returns the backing scale factor (Retina scale) for a Cocoa monitor."
   if(!monitor){ return [1.0, 1.0] }
   def display_id = is_dict(monitor) ? dict_get(monitor, "handle", 0) : monitor
   def screen = get_screen_for_display_id(display_id)
   def s = screen ? get_screen_scale(screen) : 1.0
   [s, s]
}

fn get_video_mode(monitor){
   "Returns the current video mode for a Cocoa monitor."
   if(!monitor){ return false }
   def display_id = is_dict(monitor) ? dict_get(monitor, "handle", 0) : monitor
   get_current_video_mode(display_id)
}

fn get_video_modes(monitor){
   "Returns available video modes for a Cocoa monitor."
   if(!monitor){ return [] }
   def display_id = is_dict(monitor) ? dict_get(monitor, "handle", 0) : monitor
   get_video_modes_native(display_id)
}

fn get_gamma_ramp(monitor){
   "Returns the CoreGraphics gamma ramp for the given monitor."
   if(!available() || !is_dict(monitor)){ return false }
   def display_id = dict_get(monitor, "handle", 0)
   if(!display_id){ return false }
   def capacity = int(CGDisplayGammaTableCapacity(display_id))
   if(capacity <= 0){ return false }
   def red_buf   = calloc(capacity, 4)
   def green_buf = calloc(capacity, 4)
   def blue_buf  = calloc(capacity, 4)
   def count_ptr = calloc(1, 4)
   if(!red_buf || !green_buf || !blue_buf || !count_ptr){
      free(red_buf) free(green_buf) free(blue_buf) free(count_ptr) return false
   }
   def ok = CGGetDisplayTransferByTable(display_id, capacity, red_buf, green_buf, blue_buf, count_ptr)
   def actual = load32(count_ptr, 0)
   free(count_ptr)
   if(ok != 0 || actual <= 0){
      free(red_buf) free(green_buf) free(blue_buf) return false
   }
   mut red = [] mut green = [] mut blue = []
   mut i = 0 while(i < actual){
      def scale = 65535.0
      red   = append(red,   int(load32_f32(red_buf,   i * 4) * scale + 0.5))
      green = append(green, int(load32_f32(green_buf, i * 4) * scale + 0.5))
      blue  = append(blue,  int(load32_f32(blue_buf,  i * 4) * scale + 0.5))
      i += 1
   }
   free(red_buf) free(green_buf) free(blue_buf)
   mut res = dict()
   res = dict_set(res, "size", actual)
   res = dict_set(res, "red", red)
   res = dict_set(res, "green", green)
   res = dict_set(res, "blue", blue)
   res
}

fn set_gamma_ramp(monitor, ramp){
   "Applies a gamma ramp to the given monitor via CoreGraphics."
   if(!available() || !is_dict(monitor) || !is_dict(ramp)){ return false }
   def display_id = dict_get(monitor, "handle", 0)
   if(!display_id){ return false }
   def size = dict_get(ramp, "size", 0)
   if(size <= 0){ return false }
   def red   = dict_get(ramp, "red", [])
   def green = dict_get(ramp, "green", [])
   def blue  = dict_get(ramp, "blue", [])
   if(len(red) < size || len(green) < size || len(blue) < size){ return false }
   def red_buf   = calloc(size, 4)
   def green_buf = calloc(size, 4)
   def blue_buf  = calloc(size, 4)
   if(!red_buf || !green_buf || !blue_buf){
      free(red_buf) free(green_buf) free(blue_buf) return false
   }
   def scale = 1.0 / 65535.0
   mut i = 0 while(i < size){
      store32_f32(red_buf,   get(red,   i) * scale, i * 4)
      store32_f32(green_buf, get(green, i) * scale, i * 4)
      store32_f32(blue_buf,  get(blue,  i) * scale, i * 4)
      i += 1
   }
   def ok = CGSetDisplayTransferByTable(display_id, size, red_buf, green_buf, blue_buf) == 0
   free(red_buf) free(green_buf) free(blue_buf)
   ok
}

fn focus_window(win){ true }
fn set_clipboard(win, s){
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

fn get_clipboard(win){
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
fn set_input_mode(win, mode, value){
   "Sets input modes for a Cocoa window (cursor, sticky keys, etc.)."
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
      next_win = dict_set(next_win, "cursor_mode", value)
   } elif(mode == RAW_MOUSE_MOTION){
      next_win = dict_set(next_win, "raw_mouse_motion", value != 0)
   } elif(mode == STICKY_KEYS){
      next_win = dict_set(next_win, "sticky_keys", !!value)
   } elif(mode == STICKY_MOUSE_BUTTONS){
      next_win = dict_set(next_win, "sticky_mouse", !!value)
   }

   def hwnd = dict_get(win, "handle", 0)
   _windows = dict_set(_windows, hwnd, next_win)
   true
}
fn create_surface(instance, win, allocator, surface){
   "Creates a native Vulkan Metal surface for the Cocoa window."
   if(!available()){ return -1 }

   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return -1 }

   def layer_cls = get_class("CAMetalLayer")
   if(!layer_cls){ return -1 }
   def layer = objc_msgSend(layer_cls, get_selector("layer"))
   if(!layer){ return -1 }

   def view = objc_msgSend(hwnd, get_selector("contentView"))
   if(!view){ return -1 }

   objc_msgSend_ptr_i64(view, get_selector("setWantsLayer:"), 1)
   objc_msgSend_ptr(view, get_selector("setLayer:"), layer)

   def info = vkb.VkMetalSurfaceCreateInfoEXT(layer)
   def res = vkb.vk_create_metal_surface_ext(instance, info, allocator, surface)
   free(info)
   res
}

fn get_active_display_ids(){
   "Returns the active CoreGraphics display ids."
   if(!available()){ return [] }
   def count_ptr = calloc(1, 4)
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
   def ids_buf = calloc(n, 4)
   if(!ids_buf){
      free(count_ptr)
      return []
   }
   if(CGGetActiveDisplayList(n, ids_buf, count_ptr) != 0){
      free(ids_buf)
      free(count_ptr)
      return []
   }
   mut out = []
   mut i = 0
   while(i < n){
      out = append(out, load32(ids_buf, i * 4))
      i += 1
   }
   free(ids_buf)
   free(count_ptr)
   out
}

fn _cg_mode_to_dict(mode){
   "Converts a `CGDisplayModeRef` to a Ny video-mode dictionary."
   if(!mode){ return false }
   mut out = dict()
   out = dict_set(out, "width", int(CGDisplayModeGetWidth(mode)))
   out = dict_set(out, "height", int(CGDisplayModeGetHeight(mode)))
   out = dict_set(out, "refresh_rate", int(CGDisplayModeGetRefreshRate(mode)))
   out
}

fn get_current_video_mode(display_id){
   "Returns the current video mode for a CoreGraphics display id."
   if(!available() || !display_id){ return false }
   def mode = CGDisplayCopyDisplayMode(display_id)
   if(!mode){ return false }
   def out = _cg_mode_to_dict(mode)
   CGDisplayModeRelease(mode)
   out
}

fn get_video_modes_native(display_id){
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
      if(item){ out = append(out, item) }
      i += 1
   }
   CFRelease(modes)
   out
}

fn create_menu(title=""){
   "Creates and initializes an `NSMenu`."
   if(!available()){ return 0 }
   def cls = get_class("NSMenu")
   if(!cls){ return 0 }
   mut menu = objc_msgSend(cls, get_selector("alloc"))
   if(!menu){ return 0 }
   menu = objc_msgSend(menu, get_selector("init"))
   if(title && is_str(title) && len(title) > 0){
      objc_msgSend_ptr(menu, get_selector("setTitle:"), objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(title)))
   }
   menu
}

fn create_menu_item(title, action=0, key_equivalent=""){
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

fn add_menu_item(menu, item){
   "Adds an item to an `NSMenu`."
   if(!available() || !menu || !item){ return false }
   objc_msgSend_ptr(menu, get_selector("addItem:"), item)
   true
}

fn set_submenu(menu, submenu, item){
   "Assigns a submenu to a parent menu item."
   if(!available() || !menu || !submenu || !item){ return false }
   objc_msgSend_ptr_ptr(menu, get_selector("setSubmenu:forItem:"), submenu, item)
   true
}

fn set_main_menu(menu){
   "Sets the shared application's main menu."
   if(!available() || !menu){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_ptr(app, get_selector("setMainMenu:"), menu)
   true
}

fn install_default_menu_bar(app_name="Nytrix"){
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

fn _default_run_loop_mode(){
   "Returns an NSString for the default AppKit run loop mode."
   objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr("kCFRunLoopDefaultMode"))
}

fn _distant_future_date(){
   "Returns `[NSDate distantFuture]`."
   def cls = get_class("NSDate")
   if(!cls){ return 0 }
   objc_msgSend(cls, get_selector("distantFuture"))
}

fn next_event(timeout=0){
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

fn wait_events(timeout_ms=-1){
   "Blocks until an event arrives or the timeout expires."
   def timeout = (timeout_ms < 0) ? 1000000.0 : float(timeout_ms) / 1000.0
   next_event(timeout)
}

fn current_event(){
   "Returns the current `NSEvent*` from the shared application."
   if(!available()){ return 0 }
   def app = shared_application()
   if(!app || !is_ptr(app)){ return 0 }
   objc_msgSend(app, get_selector("currentEvent"))
}

fn send_event(event){
   "Forwards an AppKit event to `NSApplication`."
   if(!available() || !event){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_ptr(app, get_selector("sendEvent:"), event)
   true
}

fn update_windows(){
   "Runs `-[NSApplication updateWindows]`."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend(app, get_selector("updateWindows"))
   true
}

fn run_app(){
   "Runs the shared AppKit application event loop."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend(app, get_selector("run"))
   true
}

fn stop_app(sender=0){
   "Stops the shared AppKit application event loop."
   if(!available()){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_ptr(app, get_selector("stop:"), sender)
   true
}

fn request_user_attention(kind=10){
   "Requests user attention via AppKit."
   if(!available()){ return 0 }
   def app = shared_application()
   if(!app){ return 0 }
   int(objc_msgSend_i64_arg(app, get_selector("requestUserAttention:"), kind))
}

fn post_event(event, at_start=true){
   "Posts an `NSEvent*` back into the AppKit queue."
   if(!available() || !event){ return false }
   def app = shared_application()
   if(!app){ return false }
   objc_msgSend_ptr_i64(app, get_selector("postEvent:atStart:"), event, at_start ? 1 : 0)
   true
}

mut _windows = dict()
mut _pending_events = []
mut _known_display_ids = []
mut _displays_initialized = false

fn _push_event(win, typ, data=0){
   if(!win){ return }
   _pending_events = append(_pending_events, [typ, win, dict_get(win, "handle", 0), data])
}

def _COCOA_KEY_MAP = [
   [0x00, backend_api.KEY_A], [0x01, backend_api.KEY_S], [0x02, backend_api.KEY_D],
   [0x03, backend_api.KEY_F], [0x04, backend_api.KEY_H], [0x05, backend_api.KEY_G],
   [0x06, backend_api.KEY_Z], [0x07, backend_api.KEY_X], [0x08, backend_api.KEY_C],
   [0x09, backend_api.KEY_V], [0x0B, backend_api.KEY_B], [0x0C, backend_api.KEY_Q],
   [0x0D, backend_api.KEY_W], [0x0E, backend_api.KEY_E], [0x0F, backend_api.KEY_R],
   [0x10, backend_api.KEY_Y], [0x11, backend_api.KEY_T], [0x12, backend_api.KEY_1],
   [0x13, backend_api.KEY_2], [0x14, backend_api.KEY_3], [0x15, backend_api.KEY_4],
   [0x16, backend_api.KEY_6], [0x17, backend_api.KEY_5], [0x18, backend_api.KEY_EQUAL],
   [0x19, backend_api.KEY_9], [0x1A, backend_api.KEY_7], [0x1B, backend_api.KEY_MINUS],
   [0x1C, backend_api.KEY_8], [0x1D, backend_api.KEY_0], [0x1E, backend_api.KEY_RIGHT_BRACKET],
   [0x1F, backend_api.KEY_O], [0x20, backend_api.KEY_U], [0x21, backend_api.KEY_LEFT_BRACKET],
   [0x22, backend_api.KEY_I], [0x23, backend_api.KEY_P], [0x24, backend_api.KEY_ENTER],
   [0x25, backend_api.KEY_L], [0x26, backend_api.KEY_J], [0x27, backend_api.KEY_APOSTROPHE],
   [0x28, backend_api.KEY_K], [0x29, backend_api.KEY_SEMICOLON], [0x2A, backend_api.KEY_BACKSLASH],
   [0x2B, backend_api.KEY_COMMA], [0x2C, backend_api.KEY_SLASH], [0x2D, backend_api.KEY_N],
   [0x2E, backend_api.KEY_M], [0x2F, backend_api.KEY_PERIOD], [0x30, backend_api.KEY_TAB],
   [0x31, backend_api.KEY_SPACE], [0x32, backend_api.KEY_GRAVE_ACCENT],
   [0x33, backend_api.KEY_BACKSPACE], [0x35, backend_api.KEY_ESCAPE],
   [0x36, backend_api.KEY_RIGHT_SUPER], [0x37, backend_api.KEY_LEFT_SUPER], [0x38, backend_api.KEY_LEFT_SHIFT],
   [0x39, backend_api.KEY_CAPS_LOCK], [0x3A, backend_api.KEY_LEFT_ALT],
   [0x3B, backend_api.KEY_LEFT_CONTROL], [0x3C, backend_api.KEY_RIGHT_SHIFT],
   [0x3D, backend_api.KEY_RIGHT_ALT], [0x3E, backend_api.KEY_RIGHT_CONTROL],
   [0x3F, backend_api.KEY_LEFT_SUPER],
   [0x60, backend_api.KEY_F5], [0x61, backend_api.KEY_F6], [0x62, backend_api.KEY_F7],
   [0x63, backend_api.KEY_F3], [0x64, backend_api.KEY_F8], [0x65, backend_api.KEY_F9],
   [0x67, backend_api.KEY_F11], [0x6D, backend_api.KEY_F10], [0x6F, backend_api.KEY_F12],
   [0x73, backend_api.KEY_HOME], [0x74, backend_api.KEY_PAGE_UP],
   [0x75, backend_api.KEY_DELETE], [0x76, backend_api.KEY_F4], [0x77, backend_api.KEY_END],
   [0x78, backend_api.KEY_F2], [0x79, backend_api.KEY_PAGE_DOWN], [0x7A, backend_api.KEY_F1],
   [0x7B, backend_api.KEY_LEFT], [0x7C, backend_api.KEY_RIGHT],
   [0x7D, backend_api.KEY_DOWN], [0x7E, backend_api.KEY_UP]
]

mut _cocoa_key_dict = 0
fn _cocoa_key_dict_get(){
   if(_cocoa_key_dict){ return _cocoa_key_dict }
   mut d = dict(128)
   mut i = 0
   while(i < len(_COCOA_KEY_MAP)){
      def pair = get(_COCOA_KEY_MAP, i)
      d = dict_set(d, get(pair, 0), get(pair, 1))
      i += 1
   }
   _cocoa_key_dict = d
   d
}

fn _cocoa_translate_keycode(code){
   def key = dict_get(_cocoa_key_dict_get(), int(code), 0)
   if(key){ return key }
   if(code >= 0 && code <= 9){ return backend_api.KEY_0 + int(code) }
   0
}

fn _mods_from_flags(flags){
   mut mods = 0
   if(band(flags, NSEventModifierFlagShift) != 0){ mods = bor(mods, backend_api.MOD_SHIFT) }
   if(band(flags, NSEventModifierFlagControl) != 0){ mods = bor(mods, backend_api.MOD_CONTROL) }
   if(band(flags, NSEventModifierFlagOption) != 0){ mods = bor(mods, backend_api.MOD_ALT) }
   if(band(flags, NSEventModifierFlagCommand) != 0){ mods = bor(mods, backend_api.MOD_SUPER) }
   if(band(flags, NSEventModifierFlagCapsLock) != 0){ mods = bor(mods, backend_api.MOD_CAPS_LOCK) }
   mods
}

fn _push_utf8_char_events(win, utf8){
   if(!utf8){ return }
   mut i = 0
   while(true){
      def b0 = load8(utf8, i)
      if(b0 == 0){ break }
      mut cp = -1
      mut next = i + 1
      if(b0 < 0x80){
         cp = b0
      } elif(band(b0, 0xe0) == 0xc0 && load8(utf8, i + 1) != 0){
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
         mut char_data = dict()
         char_data = dict_set(char_data, "char", cp)
         char_data = dict_set(char_data, "mod", 0)
         _push_event(win, EVENT_KEY_CHAR, char_data)
      }
      i = next
   }
}

fn _translate_event(event){
   "Translates a native Cocoa `NSEvent` into a Nytrix event dict."
   if(!available() || !event){ return 0 }
   def type = int(objc_msgSend_i64(event, get_selector("type")))
   def hwnd = objc_msgSend_ptr(event, get_selector("window"))
   def win = dict_get(_windows, hwnd, 0)
   if(!win){ return 0 }

   match type {
      1, 3, 25 -> { ;; NSEventTypeLeftMouseDown, RightMouseDown, OtherMouseDown
         def btn = (type == 1) ? 0 : (type == 3) ? 1 : int(objc_msgSend_i64(event, get_selector("buttonNumber")))
         mut next_win = dict_set(win, f"mouse_button_{btn}", true)
         _windows = dict_set(_windows, hwnd, next_win)
         return [EVENT_MOUSE_BUTTON_PRESSED, next_win, 0, btn]
      }
      2, 4, 26 -> { ;; NSEventTypeLeftMouseUp, RightMouseUp, OtherMouseUp
         def btn = (type == 2) ? 0 : (type == 4) ? 1 : int(objc_msgSend_i64(event, get_selector("buttonNumber")))
         mut next_win = dict_set(win, f"mouse_button_{btn}", false)
         _windows = dict_set(_windows, hwnd, next_win)
         return [EVENT_MOUSE_BUTTON_RELEASED, next_win, 0, btn]
      }
      5, 6, 7, 27 -> { ;; NSEventTypeMouseMoved, LeftMouseDragged, RightMouseDragged, OtherMouseDragged
         def dx = objc_msgSend_f64(event, get_selector("deltaX"))
         def dy = objc_msgSend_f64(event, get_selector("deltaY"))
         mut mx = float(dict_get(win, "mouse_x", 0)) + dx
         mut my = float(dict_get(win, "mouse_y", 0)) + dy
         mut next_win = dict_set(win, "mouse_x", int(mx))
         next_win = dict_set(next_win, "mouse_y", int(my))
         _windows = dict_set(_windows, hwnd, next_win)
         return [EVENT_MOUSE_POS_CHANGED, next_win, 0, [mx, my]]
      }
      10 -> { ;; NSEventTypeKeyDown
         def code = objc_msgSend_i64(event, get_selector("keyCode"))
         def key = _cocoa_translate_keycode(int(code))
         def flags = objc_msgSend_i64(event, get_selector("modifierFlags"))
         def mods = _mods_from_flags(flags)
         def is_repeat = objc_msgSend_i64(event, get_selector("isARepeat")) != 0
         mut next_win = dict_set(win, f"key_{key}", true)
         _windows = dict_set(_windows, hwnd, next_win)
         mut data = dict()
         data = dict_set(data, "key", key)
         data = dict_set(data, "scancode", int(code))
         data = dict_set(data, "action", is_repeat ? backend_api.ACTION_REPEAT : backend_api.ACTION_PRESS)
         data = dict_set(data, "mods", mods)
         ;; Emit character events for printable text input
         def nsstr = objc_msgSend_ptr(event, get_selector("characters"))
         if(nsstr){
         def utf8 = objc_msgSend_ptr(nsstr, get_selector("UTF8String"))
         if(utf8){ _push_utf8_char_events(next_win, utf8) }
         }
         return [EVENT_KEY_PRESSED, next_win, 0, data]
      }
      11 -> { ;; NSEventTypeKeyUp
         def code = objc_msgSend_i64(event, get_selector("keyCode"))
         def key = _cocoa_translate_keycode(int(code))
         def flags = objc_msgSend_i64(event, get_selector("modifierFlags"))
         def mods = _mods_from_flags(flags)
         mut next_win = dict_set(win, f"key_{key}", false)
         _windows = dict_set(_windows, hwnd, next_win)
         mut data = dict()
         data = dict_set(data, "key", key)
         data = dict_set(data, "scancode", int(code))
         data = dict_set(data, "action", backend_api.ACTION_RELEASE)
         data = dict_set(data, "mods", mods)
         return [EVENT_KEY_RELEASED, next_win, 0, data]
      }
      12 -> { ;; NSEventTypeFlagsChanged — modifier key press/release
         def code = objc_msgSend_i64(event, get_selector("keyCode"))
         def key = _cocoa_translate_keycode(int(code))
         if(!key){ return 0 }
         def flags = objc_msgSend_i64(event, get_selector("modifierFlags"))
         ;; Determine pressed/released based on which modifier flag maps to this keyCode
         mut modifier_flag = 0
         if(key == backend_api.KEY_LEFT_SHIFT || key == backend_api.KEY_RIGHT_SHIFT){ modifier_flag = NSEventModifierFlagShift }
         elif(key == backend_api.KEY_LEFT_CONTROL || key == backend_api.KEY_RIGHT_CONTROL){ modifier_flag = NSEventModifierFlagControl }
         elif(key == backend_api.KEY_LEFT_ALT || key == backend_api.KEY_RIGHT_ALT){ modifier_flag = NSEventModifierFlagOption }
         elif(key == backend_api.KEY_LEFT_SUPER || key == backend_api.KEY_RIGHT_SUPER){ modifier_flag = NSEventModifierFlagCommand }
         elif(key == backend_api.KEY_CAPS_LOCK){ modifier_flag = NSEventModifierFlagCapsLock }
         def pressed = modifier_flag ? (band(flags, modifier_flag) != 0) : false
         mut data = dict()
         data = dict_set(data, "key", key)
         data = dict_set(data, "scancode", int(code))
         data = dict_set(data, "action", pressed ? backend_api.ACTION_PRESS : backend_api.ACTION_RELEASE)
         data = dict_set(data, "mods", _mods_from_flags(flags))
         def kind = pressed ? EVENT_KEY_PRESSED : EVENT_KEY_RELEASED
         mut next_win = dict_get(_windows, hwnd, win)
         next_win = dict_set(next_win, f"key_{key}", pressed)
         _windows = dict_set(_windows, hwnd, next_win)
         return [kind, next_win, 0, data]
      }
      22 -> { ;; NSEventTypeScrollWheel
         def dx = objc_msgSend_f64(event, get_selector("scrollingDeltaX"))
         def dy = objc_msgSend_f64(event, get_selector("scrollingDeltaY"))
         def precise = objc_msgSend_i64(event, get_selector("hasPreciseScrollingDeltas"))
         mut sdx = dx
         mut sdy = dy
         if(!precise){
         sdx = dx * 10.0
         sdy = dy * 10.0
         }
         return [EVENT_MOUSE_SCROLL, win, 0, [sdx, sdy]]
      }
      _ -> { return 0 }
   }
}
fn _find_window_by_handle(hwnd){
   "Looks up a tracked window by its native NSWindow handle."
   if(!hwnd){ return 0 }
   dict_get(_windows, hwnd, 0)
}

fn _window_delegate_should_close(self, sel, win_obj){
   "NSWindowDelegate windowShouldClose: callback."
   def win = _find_window_by_handle(win_obj)
   if(win){
      _push_event(win, EVENT_QUIT, 0)
   }
   true
}

fn _window_delegate_did_resize(self, sel, notification){
   "NSWindowDelegate windowDidResize: callback."
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(!win){ return }
   def size = get_size(win)
   _push_event(win, EVENT_WINDOW_RESIZED, size)
}

fn _window_delegate_did_miniaturize(self, sel, notification){
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_event(win, EVENT_WINDOW_MINIMIZED, 0) }
}

fn _window_delegate_did_deminiaturize(self, sel, notification){
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_event(win, EVENT_WINDOW_RESTORED, 0) }
}

fn _window_delegate_did_become_key(self, sel, notification){
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_event(win, EVENT_FOCUS_IN, 0) }
}

fn _window_delegate_did_resign_key(self, sel, notification){
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_event(win, EVENT_FOCUS_OUT, 0) }
}

fn _window_delegate_did_change_backing(self, sel, notification){
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(win){
      def scale = get_window_content_scale(win)
      _push_event(win, EVENT_SCALE_UPDATED, scale)
   }
}

fn _window_delegate_did_move(self, sel, notification){
   "NSWindowDelegate windowDidMove: callback — updates win[x/y] and emits EVENT_WINDOW_MOVED."
   def hwnd = objc_msgSend_ptr(notification, get_selector("object"))
   def win = _find_window_by_handle(hwnd)
   if(!win){ return }
   def rect = _get_nswindow_rect(hwnd, "frame")
   def nx = get(rect, 0)
   def ny = get(rect, 1)
   mut next_win = dict_set(win, "x", nx)
   next_win = dict_set(next_win, "y", ny)
   _windows = dict_set(_windows, hwnd, next_win)
   _push_event(next_win, EVENT_WINDOW_MOVED, [nx, ny])
}

fn _ime_insert_text(self, sel, string_obj, replacement_range){
   "NSTextInputClient insertText:replacementRange: — pushes EVENT_KEY_CHAR for each codepoint."
   if(!string_obj){ return }
   def cs = objc_msgSend_ptr(string_obj, get_selector("UTF8String"))
   if(!cs){ return }
   def hwnd = _last_key_window()
   def win = _find_window_by_handle(hwnd)
   if(win){ _push_utf8_char_events(win, cs) }
}

fn _last_key_window(){
   "Returns the NSWindow that currently has keyboard focus."
   def app = shared_application()
   if(!app){ return 0 }
   objc_msgSend_ptr(app, get_selector("keyWindow"))
}

fn _drag_enter(self, sel, sender){
   "NSDraggingDestination draggingEntered: — accept copy operation."
   1
}

fn _drag_updated(self, sel, sender){
   "NSDraggingDestination draggingUpdated: — keep accepting copy."
   1
}

fn _drag_performed(self, sel, sender){
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
         def item = _objc_msgSend_ptr_i64(items, get_selector("objectAtIndex:"), 0, idx)
         if(item){
         def cs = objc_msgSend_ptr(item, get_selector("UTF8String"))
         if(cs){ paths = append(paths, to_str(cs)) }
         }
         idx += 1
      }
   }
   def hwnd = _last_key_window()
   def win = _find_window_by_handle(hwnd)
   if(win && len(paths) > 0){
      mut data = dict()
      data = dict_set(data, "paths", paths)
      _push_event(win, EVENT_DATA_DROP, data)
   }
   len(paths) > 0
}

mut _delegate_class = 0

fn create_window_delegate(){
   "Creates and registers a dynamic `NytrixWindowDelegate` class."
   if(_delegate_class){ return _delegate_class }
   def base = get_class("NSObject")
   def cls = objc_allocateClassPair(base, cstr("NytrixWindowDelegate"), 0)
   if(cls){
      class_addMethod(cls, get_selector("windowShouldClose:"), _window_delegate_should_close, cstr("c@:@"))
      class_addMethod(cls, get_selector("windowDidResize:"), _window_delegate_did_resize, cstr("v@:@"))
      class_addMethod(cls, get_selector("windowDidMiniaturize:"), _window_delegate_did_miniaturize, cstr("v@:@"))
      class_addMethod(cls, get_selector("windowDidDeminiaturize:"), _window_delegate_did_deminiaturize, cstr("v@:@"))
      class_addMethod(cls, get_selector("windowDidBecomeKey:"), _window_delegate_did_become_key, cstr("v@:@"))
      class_addMethod(cls, get_selector("windowDidResignKey:"), _window_delegate_did_resign_key, cstr("v@:@"))
      class_addMethod(cls, get_selector("windowDidChangeBackingProperties:"), _window_delegate_did_change_backing, cstr("v@:@"))
      class_addMethod(cls, get_selector("windowDidMove:"), _window_delegate_did_move, cstr("v@:@"))
      class_addMethod(cls, get_selector("insertText:replacementRange:"), _ime_insert_text, cstr("v@:@{NSRange=QQ}"))
      class_addMethod(cls, get_selector("draggingEntered:"), _drag_enter, cstr("Q@:@"))
      class_addMethod(cls, get_selector("draggingUpdated:"), _drag_updated, cstr("Q@:@"))
      class_addMethod(cls, get_selector("performDragOperation:"), _drag_performed, cstr("c@:@"))
      objc_registerClassPair(cls)
      _delegate_class = cls
   }
   cls
}

fn get_window_content_scale(win){
   "Returns the backing scale factor for a Cocoa window."
   if(!available() || !win || !is_dict(win)){ return 1.0 }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return 1.0 }
   float(objc_msgSend_f64(hwnd, get_selector("backingScaleFactor")))
}

fn get_window_monitor(win){
   "Returns the monitor dictionary that the Cocoa window is currently on."
   if(!available() || !win || !is_dict(win)){ return 0 }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return 0 }
   def screen = objc_msgSend_ptr(hwnd, get_selector("screen"))
   if(!screen){ return 0 }

   def display_id = get_screen_display_id(screen)
   if(!display_id){ return 0 }

   def monitors = get_monitors()
   mut i = 0
   while(i < len(monitors)){
      def m = get(monitors, i)
      if(dict_get(m, "handle", 0) == display_id){ return m }
      i += 1
   }
   0
}

fn set_window_monitor(win, monitor, xpos, ypos, width, height, refresh_rate=0){
   "Stub for Cocoa window-monitor association."
   win
}

fn get_key_name(win, key, scancode){
   "Returns the keyboard-layout key name via the _COCOA_KEY_MAP reverse lookup."
   if(key >= 32 && key <= 126){ return str.chr(key) }
   ;; Try to find the scancode in the key map to return a friendly name
   mut i = 0
   while(i < len(_COCOA_KEY_MAP)){
      def entry = get(_COCOA_KEY_MAP, i)
      if(entry && get(entry, 1) == key){
         def sc = get(entry, 0)
         ;; Return the single char for common printable keys, else empty
         if(sc >= 0 && key >= 32 && key < 127){ return str.chr(key) }
      }
      i += 1
   }
   ""
}

fn set_window_icon(win, images){
   "Stub for Cocoa window icon."
   false
}

fn get_key_state(win, key){
   "Returns the Cocoa key state from the local dictionary."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(win, f"key_{key}", 0)
}

fn get_mouse_button_state(win, btn){
   "Returns the Cocoa mouse button state from the local dictionary."
   if(!win || !is_dict(win)){ return 0 }
   dict_get(win, f"mouse_button_{btn}", 0)
}

fn get_cursor_pos(win){
   "Returns the current cursor position relative to the Cocoa window."
   if(!available() || !win || !is_dict(win)){ return [0.0, 0.0] }
   [float(dict_get(win, "mouse_x", 0)), float(dict_get(win, "mouse_y", 0))]
}

fn set_cursor_pos(win, x, y){
   "Warps the cursor to (x, y) in window-local coordinates using CGWarpMouseCursorPosition."
   if(!available() || !win || !is_dict(win)){ return false }
   ;; Convert window-local (x, y) to screen coordinates using tracked origin
   def win_x = float(dict_get(win, "x", 0))
   def win_y = float(dict_get(win, "y", 0))
   def win_h = float(dict_get(win, "h", 600))
   ;; Cocoa: origin bottom-left; CG: origin top-left; stored y is bottom of window in screen coords
   ;; screen_top_y = primary_screen_height - (win_y + win_h - y_in_win)
   def screen_y = win_y + win_h - float(y)
   CGWarpMouseCursorPosition(win_x + float(x), screen_y)
   true
}

fn create_cursor(image, xhot=0, yhot=0){
   "Stub for Cocoa custom cursor creation."
   0
}

fn create_standard_cursor(shape){
   "Creates an AppKit-backed standard cursor object."
   if(!available()){ return 0 }
   mut sel = ""
   match shape {
      ARROW_CURSOR -> { sel = "arrowCursor" }
      IBEAM_CURSOR -> { sel = "IBeamCursor" }
      CROSSHAIR_CURSOR -> { sel = "crosshairCursor" }
      POINTING_HAND_CURSOR -> { sel = "pointingHandCursor" }
      RESIZE_EW_CURSOR -> { sel = "resizeLeftRightCursor" }
      RESIZE_NS_CURSOR -> { sel = "resizeUpDownCursor" }
      RESIZE_NWSE_CURSOR -> { sel = "arrowCursor" }
      RESIZE_NESW_CURSOR -> { sel = "arrowCursor" }
      RESIZE_ALL_CURSOR -> { sel = "arrowCursor" }
      NOT_ALLOWED_CURSOR -> { sel = "operationNotAllowedCursor" }
      _ -> { sel = "arrowCursor" }
   }
   def cls = get_class("NSCursor")
   def handle = objc_msgSend_ptr(cls, get_selector(sel), 0)
   if(!handle){ return 0 }
   mut c = dict()
   c = dict_set(c, "handle", handle)
   c
}

fn destroy_cursor(cursor){
   "Cocoa standard cursors are owned by the system."
   true
}

fn set_cursor(win, cursor){
   "Applies an AppKit cursor."
   if(!available()){ return false }
   def handle = is_dict(cursor) ? dict_get(cursor, "handle", 0) : cursor
   if(handle){
      objc_msgSend_ptr(handle, get_selector("set"), 0)
      true
   } else { false }
}

fn show_window(win){
   "Ny direct port of GLFW's `_glfwShowWindowCocoa`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   objc_msgSend_ptr(hwnd, get_selector("makeKeyAndOrderFront:"), 0)
   drain_autorelease_pool(pool)
   true
}

fn hide_window(win){
   "Ny direct port of GLFW's `_glfwHideWindowCocoa`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   objc_msgSend_ptr(hwnd, get_selector("orderOut:"), 0)
   drain_autorelease_pool(pool)
   true
}

fn iconify_window(win){
   "Ny direct port of GLFW's `_glfwIconifyWindowCocoa`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   objc_msgSend_ptr(hwnd, get_selector("miniaturize:"), 0)
   drain_autorelease_pool(pool)
   true
}

fn maximize_window(win){
   "Ny direct port of GLFW's `_glfwMaximizeWindowCocoa`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   if(objc_msgSend_i64(hwnd, get_selector("isZoomed")) == 0){
      objc_msgSend_ptr(hwnd, get_selector("zoom:"), 0)
   }
   drain_autorelease_pool(pool)
   true
}

fn restore_window(win){
   "Ny direct port of GLFW's `_glfwRestoreWindowCocoa`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   if(objc_msgSend_i64(hwnd, get_selector("isMiniaturized")) != 0){
      objc_msgSend_ptr(hwnd, get_selector("deminiaturize:"), 0)
   } elif(objc_msgSend_i64(hwnd, get_selector("isZoomed")) != 0){
      objc_msgSend_ptr(hwnd, get_selector("zoom:"), 0)
   }
   drain_autorelease_pool(pool)
   true
}

fn set_title(win, title){
   "Updates the Cocoa native window title."
   if(!available() || !win || !is_dict(win) || !title){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def pool = create_autorelease_pool()
   objc_msgSend_ptr(hwnd, get_selector("setTitle:"),
      objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(to_str(title))))
   drain_autorelease_pool(pool)
   true
}

fn get_window_attrib(win, attrib){
   "Returns the value of the specified window attribute."
   if(!available() || !win || !is_dict(win)){ return 0 }
   def hwnd = dict_get(win, "handle", 0)
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

fn set_window_opacity(win, opacity){
   "Sets the whole-window alpha value for a Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   objc_msgSend_arg_f64(hwnd, get_selector("setAlphaValue:"), float(opacity))
   true
}

fn set_window_resizable(win, enabled){
   "Toggles the Cocoa window's resizable style mask bit."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   mut style = int(objc_msgSend_i64(hwnd, get_selector("styleMask")))
   if(enabled){ style = bor(style, NSWindowStyleMaskResizable) }
   else { style = band(style, bnot(NSWindowStyleMaskResizable)) }
   objc_msgSend_i64_arg(hwnd, get_selector("setStyleMask:"), style)
   true
}

fn set_window_decorated(win, enabled){
   "Toggles window decorations for a Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   mut style = int(objc_msgSend_i64(hwnd, get_selector("styleMask")))
   if(enabled){ style = bor(style, NSWindowStyleMaskTitled) }
   else { style = band(style, bnot(NSWindowStyleMaskTitled)) }
   objc_msgSend_i64_arg(hwnd, get_selector("setStyleMask:"), style)
   true
}

fn set_window_floating(win, enabled){
   "Toggles the always-on-top state of a Cocoa window."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   ;; NSNormalWindowLevel = 0, NSStatusWindowLevel = 25
   def level = enabled ? 25 : 0
   objc_msgSend_i64_arg(hwnd, get_selector("setLevel:"), level)
   true
}

fn _get_nswindow_rect(win, key_name){
   if(!available() || !win){ return [0, 0, 0, 0] }
   def hwnd = is_dict(win) ? dict_get(win, "handle", 0) : win
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
   [dict_get(res, "x", 0), dict_get(res, "y", 0), dict_get(res, "width", 0), dict_get(res, "height", 0)]
}

fn get_pos(win){
   "Returns the bottom-left Cocoa screen coordinates [x, y] of a window's client area."
   def rect = _get_nswindow_rect(win, "contentLayoutRect")
   [get(rect, 0), get(rect, 1)]
}

fn set_pos(win, x, y){
   "Moves a Cocoa window so its client area starts at screen coordinates [x, y]."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   ;; Cocoa uses bottom-left origin. For now we just use absolute screen coords.
   def current = _get_nswindow_rect(win, "frame")
   def w = get(current, 2)
   def h = get(current, 3)
   objc_msgSend_rect_u64_i64_i8(hwnd, get_selector("setFrame:display:"), float(x), float(y), float(w), float(h), 1, 1, 0)
   true
}

fn get_size(win){
   "Returns the pixel size [width, height] of a Cocoa window's client area."
   def rect = _get_nswindow_rect(win, "contentLayoutRect")
   [get(rect, 2), get(rect, 3)]
}

fn set_size(win, w, h){
   "Resizes a Cocoa window so its client area has width `w` and height `h`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   def current = _get_nswindow_rect(win, "frame")
   def x = get(current, 0)
   def y = get(current, 1)
   objc_msgSend_rect_u64_i64_i8(hwnd, get_selector("setFrame:display:"), float(x), float(y), float(w), float(h), 1, 1, 0)
   true
}

fn get_framebuffer_size(win){
   "Returns the backing pixel size of the Cocoa window's content view."
   def size = get_size(win)
   def scale = get_screen_scale(get_screen_at(0)) ;; Placeholder
   [int(float(get(size, 0)) * scale), int(float(get(size, 1)) * scale)]
}

fn create_basic_window(title, width, height, x=0, y=0, flags=0){
   "Creates a native `NSWindow` via AppKit, mirroring GLFW's `cocoa_window.m` path."
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

   mut style = 0
   if(band(flags, 1 << 0)){ style = bor(style, 0) } ;; Placeholder for real styles
   style = 1 | 2 | 4 | 8 ;; Titled, Closable, Miniaturizable, Resizable

   ;; [[NSWindow alloc] initWithContentRect:rect styleMask:style backing:NSBackingStoreBuffered defer:NO]
   def hwnd = objc_msgSend_rect_u64_i64_i8(win_obj, get_selector("initWithContentRect:styleMask:backing:defer:"),
      float(x), float(y), float(width), float(height), style, 2, 0)

   if(hwnd){
      objc_msgSend_ptr(hwnd, get_selector("setTitle:"),
         objc_msgSend_ptr(get_class("NSString"), get_selector("stringWithUTF8String:"), cstr(to_str(title))))

      def delegate_cls = create_window_delegate()
      def delegate = objc_msgSend(objc_msgSend(delegate_cls, get_selector("alloc")), get_selector("init"))
      objc_msgSend_ptr(hwnd, get_selector("setDelegate:"), delegate)

      objc_msgSend_ptr_i64(hwnd, get_selector("setAcceptsMouseMovedEvents:"), 1)
      objc_msgSend(hwnd, get_selector("makeKeyAndOrderFront:"))
   }

   drain_autorelease_pool(pool)
   mut win = dict()
   win = dict_set(win, "handle", hwnd)
   win = dict_set(win, "title", to_str(title))
   win = dict_set(win, "x", x)
   win = dict_set(win, "y", y)
   win = dict_set(win, "w", width)
   win = dict_set(win, "h", height)
   _windows = dict_set(_windows, hwnd, win)
   win
}

fn destroy_basic_window(win){
   "Closes and releases a native `NSWindow`."
   if(!available() || !win || !is_dict(win)){ return false }
   def hwnd = dict_get(win, "handle", 0)
   if(!hwnd){ return false }
   _windows = dict_set(_windows, hwnd, 0)
   objc_msgSend(hwnd, get_selector("close"))
   true
}

fn set_window_icon(win, images){
   "Sets the Cocoa window/app icon (not yet implemented)."
   false
}

fn _check_monitor_changes(){
   "Diffs current display IDs against known list; emits CONNECTED/DISCONNECTED to all windows."
   def ids = get_active_display_ids()
   if(!_displays_initialized){
      _known_display_ids = ids
      _displays_initialized = true
      return
   }
   mut cur_set = dict()
   mut i = 0
   while(i < len(ids)){
      cur_set = dict_set(cur_set, get(ids, i), 1)
      i += 1
   }
   mut old_set = dict()
   i = 0
   while(i < len(_known_display_ids)){
      old_set = dict_set(old_set, get(_known_display_ids, i), 1)
      i += 1
   }
   ;; New displays
   i = 0
   while(i < len(ids)){
      def did = get(ids, i)
      if(!dict_has(old_set, did)){
         mut data = dict()
         data = dict_set(data, "display_id", did)
         def keys = dict_keys(_windows)
         mut j = 0
         while(j < len(keys)){
         def w = dict_get(_windows, get(keys, j), 0)
         if(is_dict(w)){ _push_event(w, EVENT_MONITOR_CONNECTED, data) }
         j += 1
         }
      }
      i += 1
   }
   ;; Removed displays
   i = 0
   while(i < len(_known_display_ids)){
      def did = get(_known_display_ids, i)
      if(!dict_has(cur_set, did)){
         mut data = dict()
         data = dict_set(data, "display_id", did)
         def keys = dict_keys(_windows)
         mut j = 0
         while(j < len(keys)){
         def w = dict_get(_windows, get(keys, j), 0)
         if(is_dict(w)){ _push_event(w, EVENT_MONITOR_DISCONNECTED, data) }
         j += 1
         }
      }
      i += 1
   }
   _known_display_ids = ids
}

fn poll_window_events(win, max_events=64){
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
         def ev_win = get(translated, 1, 0)
         if(ev_win){ _push_event(ev_win, get(translated, 0, 0), get(translated, 3, 0)) }
      }
      drained += 1
   }
   def handle = dict_get(win, "handle", 0)
   mut out = []
   mut remaining = []
   mut i = 0
   while(i < len(_pending_events)){
      def ev = get(_pending_events, i)
      def ev_handle = get(ev, 2, 0)
      def ev_win = get(ev, 1, 0)
      def matched = ev_handle == handle || (is_dict(ev_win) && dict_get(ev_win, "handle", 0) == handle)
      if(matched && len(out) < max_events){
         out = append(out, ev)
      } else {
         remaining = append(remaining, ev)
      }
      i += 1
   }
   _pending_events = remaining
   def next_win = dict_get(_windows, handle, win)
   [next_win, out]
}

fn vulkan_supported(){
   "Returns true if the Cocoa backend supports Vulkan (requires MoltenVK)."
   true
}

fn vulkan_required_extensions(){
   "Returns the Vulkan instance extensions required for a macOS surface."
   if(!_cocoa_vk_ext_ptrs){
      _cocoa_vk_ext_surface = cstr("VK_KHR_surface")
      _cocoa_vk_ext_metal = cstr("VK_EXT_metal_surface")
      def arr = malloc(16)
      store64_h(arr, _cocoa_vk_ext_surface, 0)
      store64_h(arr, _cocoa_vk_ext_metal, 8)
      _cocoa_vk_ext_ptrs = [2, arr]
   }
   _cocoa_vk_ext_ptrs
}

mut _cocoa_vk_ext_ptrs = 0
mut _cocoa_vk_ext_surface = 0
mut _cocoa_vk_ext_metal = 0

fn get_cocoa_window(win){
   "Returns the native NSWindow pointer for the given window dict."
   if(is_dict(win)){ dict_get(win, "handle", 0) } else { win }
}

fn get_cocoa_monitor(mon){
   "Returns the CGDirectDisplayID for the given monitor dict."
   if(is_dict(mon)){ dict_get(mon, "handle", 0) } else { 0 }
}

fn get_cocoa_view(win){
   "Returns the NSView (content view) for the given window."
   def hwnd = is_dict(win) ? dict_get(win, "handle", 0) : win
   if(!hwnd){ return 0 }
   objc_msgSend_ptr(hwnd, get_selector("contentView"))
}
