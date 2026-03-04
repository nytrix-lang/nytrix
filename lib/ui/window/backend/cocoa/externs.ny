;; backend/cocoa/externs.ny
;; Reference: macOS Cocoa + CoreGraphics + IOKit externs used by backend/mod.ny §4.
;; All Cocoa calls go through objc_msgSend — no AppKit headers needed at link time.
;; Linked: objc AppKit CoreFoundation CoreGraphics IOKit

;; ── ObjC runtime ─────────────────────────────────────────────────────────────
;; extern fn objc_getClass(n: ptr): ptr
;; extern fn sel_registerName(n: ptr): ptr
;; extern fn objc_msgSend(r: ptr, s: ptr): ptr
;;   — also called with extra args; Ny uses typed wrappers:
;; extern fn objc_msgSend_void(r: ptr, s: ptr)
;; extern fn objc_msgSend_void_ptr(r: ptr, s: ptr, a: ptr)
;; extern fn objc_msgSend_void_int(r: ptr, s: ptr, a: i64)
;; extern fn objc_msgSend_void_bool(r: ptr, s: ptr, a: i32)
;; extern fn objc_msgSend_rect_int(r: ptr, s: ptr, x: f64, y: f64, w: f64, h: f64, a: u64): ptr
;;   — used for initWithContentRect:styleMask:backing:defer:
;; extern fn objc_msgSend_ret_i64(r: ptr, s: ptr): i64
;; extern fn objc_msgSend_ret_bool(r: ptr, s: ptr): i32
;; extern fn objc_msgSend_ret_f64(r: ptr, s: ptr): f64

;; ── CoreFoundation ────────────────────────────────────────────────────────────
;; extern fn CFStringCreateWithCString(al: ptr, s: ptr, enc: u32): ptr
;; extern fn CFStringGetCString(s: ptr, b: ptr, max: i64, enc: u32): i32
;; extern fn CFRelease(o: ptr)
;; extern fn CFRunLoopRunInMode(mode: ptr, sec: f64, once: i32): i32
;;   kCFStringEncodingUTF8 = 0x08000100

;; ── CoreGraphics ──────────────────────────────────────────────────────────────
;; extern fn CGGetActiveDisplayList(max: u32, ids: ptr, cnt: ptr): i32
;;   ids = u32[] (CGDirectDisplayID array)
;;   CGDisplayBounds returns CGRect { x f64, y f64, w f64, h f64 } at rect ptr
;; extern fn CGDisplayBounds(did: u32, rect: ptr)   ;; writes 4×f64
;; extern fn CGWarpMouseCursorPosition(x: f64, y: f64): i32
;; extern fn CGDisplayHideCursor(did: u32): i32
;; extern fn CGDisplayShowCursor(did: u32): i32
;; extern fn CGMainDisplayID(): u32

;; ── IOKit HID (joystick — stub) ───────────────────────────────────────────────
;; extern fn IOHIDManagerCreate(al: ptr, opts: u32): ptr
;; extern fn IOHIDManagerSetDeviceMatching(m: ptr, match: ptr)
;; extern fn IOHIDManagerOpen(m: ptr, opts: u32): i32
;; extern fn IOHIDManagerCopyDevices(m: ptr): ptr   ;; returns CFSetRef
