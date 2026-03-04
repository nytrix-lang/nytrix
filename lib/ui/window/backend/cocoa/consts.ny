;; backend/cocoa/consts.ny
;; Reference: NSWindow / NSEvent / CoreFoundation constants used by backend/mod.ny.

;; ── NSWindow style masks ──────────────────────────────────────────────────────
;; NS_TITLED           = 1        NS_CLOSABLE        = 2
;; NS_MINIATURIZABLE   = 4        NS_RESIZABLE       = 8
;; NS_FULLSCREEN       = 0x4000
;; NS_BACKING_BUFFERED = 2        ;; NSBackingStoreBuffered

;; ── NSApplication activation policy ──────────────────────────────────────────
;; NSApplicationActivationPolicyRegular = 0   ;; normal taskbar app

;; ── NSWindowLevel ─────────────────────────────────────────────────────────────
;; NSNormalWindowLevel   = 0   ;; set_floating(false)
;; NSFloatingWindowLevel = 3   ;; set_floating(true)

;; ── NSEvent types ─────────────────────────────────────────────────────────────
;; NSE_KeyDown     = 10   NSE_KeyUp        = 11   NSE_FlagsChanged = 12
;; NSE_LMouseDown  = 1    NSE_LMouseUp     = 2
;; NSE_RMouseDown  = 3    NSE_RMouseUp     = 4
;; NSE_OMouseDown  = 25   NSE_OMouseUp     = 26   ;; other (middle etc.)
;; NSE_MouseMoved  = 5    NSE_ScrollWheel  = 22

;; ── NSEvent modifier flags ────────────────────────────────────────────────────
;; NSM_Shift   = 0x20000    NSM_Control = 0x40000
;; NSM_Option  = 0x80000    NSM_Command = 0x100000
;; NSM_CapsLock= 0x10000

;; ── CFString encoding ─────────────────────────────────────────────────────────
;; kCFStringEncodingUTF8 = 0x08000100

;; ── Key selectors used ────────────────────────────────────────────────────────
;; "sharedApplication"          "setActivationPolicy:"   "finishLaunching"
;; "makeKeyAndOrderFront:"      "setTitle:"              "close"
;; "isZoomed"   "isMiniaturized"  "isVisible"
;; "zoom:"      "miniaturize:"    "deminiaturize:"        "toggleFullScreen:"
;; "setStyleMask:"   "setLevel:"   "setAlphaValue:"  "setIgnoresMouseEvents:"
;; "setFrameOrigin:" "setContentSize:"
;; "nextEventMatchingMask:untilDate:inMode:dequeue:"
;; "sendEvent:"    "type"  "window"   "modifierFlags"   "keyCode"
;; "locationInWindow.x"  "locationInWindow.y"
;; "scrollingDeltaX"     "scrollingDeltaY"

;; ── CGDisplayBounds rect layout (ptr to 4×f64) ────────────────────────────────
;; +0  f64  x (virtual desktop origin X)
;; +8  f64  y (virtual desktop origin Y)  — Y=0 is top on macOS
;; +16 f64  width
;; +24 f64  height

;; ── NSAutoreleasePool ─────────────────────────────────────────────────────────
;; Created per poll_events call: alloc → init, drained at end with drain
