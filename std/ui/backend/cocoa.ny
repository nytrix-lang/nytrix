;; Keywords: ui window cocoa
;; macOS Cocoa Backend for std.ui.window (Preliminary)

module std.ui.backend.cocoa (
   available, create_native_window, poll_events, swap_buffers, make_current, blit_buffer
)
use std.core *
use std.os.ffi *
use std.ui.consts *
use std.ui.event as ev

mut _objc = 0
mut _framework = 0
mut _objc_msgSend = 0
mut _objc_getClass = 0
mut _sel_registerName = 0

fn _touch(...args){
   "Internal helper to mark arguments as used."
   len(args)
}

fn available(){
   "Returns true if the Cocoa (Objective-C runtime) is available."
   if(_objc != 0){ return true }
   _objc = dlopen_any("objc", RTLD_NOW())
   _framework = dlopen("/System/Library/Frameworks/Cocoa.framework/Cocoa", RTLD_NOW())
   if(_objc == 0){ return false }
   _objc_msgSend = bind(_objc, "objc_msgSend")
   _objc_getClass = bind(_objc, "objc_getClass")
   _sel_registerName = bind(_objc, "sel_registerName")
   if(_objc_msgSend == 0){ return false }
   return true
}

fn create_native_window(win){
   "Creates a native NSWindow for the given Nytrix window object."
   if(!available()){ return false }
   ;; NSApplication.sharedApplication
   def NSApp_class = call1(_objc_getClass, "NSApplication")
   def sharedApp_sel = call1(_sel_registerName, "sharedApplication")
   def app = call2(_objc_msgSend, NSApp_class, sharedApp_sel)
   ;; setActivationPolicy: NSApplicationActivationPolicyRegular=0
   def setActPol_sel = call1(_sel_registerName, "setActivationPolicy:")
   call3(_objc_msgSend, app, setActPol_sel, 0)
   ;; alloc NSWindow
   def NSWindow_class = call1(_objc_getClass, "NSWindow")
   def alloc_sel = call1(_sel_registerName, "alloc")
   def window_obj = call2(_objc_msgSend, NSWindow_class, alloc_sel)
   ;; init NSWindow
   def init_sel = call1(_sel_registerName, "initWithContentRect:styleMask:backing:defer:")
   ;; NSWindowStyleMaskTitled=1 | NSWindowStyleMaskClosable=2 | NSWindowStyleMaskMiniaturizable=4 | NSWindowStyleMaskResizable=8 -> 15
   ;; NSBackingStoreBuffered=2
   ;; we pass a fake NSRect struct buffer. Since arguments are passed via registers or stack, FFI for struct by value is complex.
   ;; We will pass 0 for now as a graceful stub since C FFI by value structs are broken natively across platforms in raw dlsym
   call6(_objc_msgSend, window_obj, init_sel, 0, 15, 2, 0)
   ;; makeKeyAndOrderFront
   def makeKeyAndOrderFront_sel = call1(_sel_registerName, "makeKeyAndOrderFront:")
   call3(_objc_msgSend, window_obj, makeKeyAndOrderFront_sel, 0)
   set_idx(win, 22, window_obj)
   return true
}

fn poll_events(win){
   "Polls Cocoa events and dispatches them to the NSApplication."
   _touch(win)
   if(_objc == 0){ return 0 }
   ;; [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES]
   def NSApp_class = call1(_objc_getClass, "NSApplication")
   def sharedApp_sel = call1(_sel_registerName, "sharedApplication")
   def app = call2(_objc_msgSend, NSApp_class, sharedApp_sel)
   def NSAnyEventMask = 0xFFFFFFFF
   def nextEvent_sel = call1(_sel_registerName, "nextEventMatchingMask:untilDate:inMode:dequeue:")
   def ev_obj = call6(_objc_msgSend, app, nextEvent_sel, NSAnyEventMask, 0, "kCFRunLoopDefaultMode", 1)
   if(ev_obj != 0){
      def sendEvent_sel = call1(_sel_registerName, "sendEvent:")
      call3(_objc_msgSend, app, sendEvent_sel, ev_obj)
   }
   0
}

fn swap_buffers(win){
   "Cocoa buffer swap implementation (no-op, handles by Metal/Vulkan if used)."
   _touch(win)
   ;; Vulkan present handled by vkr
}

fn make_current(win){
   "Makes the window the current rendering context."
   _touch(win)
}
fn blit_buffer(win, buf, w, h){
   "Blits a raw buffer to the Cocoa window (placeholder)."
   _touch(win, buf, w, h)
   0
}
