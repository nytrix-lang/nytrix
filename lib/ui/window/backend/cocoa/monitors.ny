;; backend/cocoa/monitors.ny
;; Reference: macOS monitor enumeration using CoreGraphics CGGetActiveDisplayList.
;;
;; CGGetActiveDisplayList(maxDisplays, activeDisplays, displayCount):
;;   call with maxDisplays=32, activeDisplays=NULL first to get count,
;;   then again with allocated u32[] to fill CGDirectDisplayID values.
;;
;; CGDisplayBounds(displayID, CGRect*):
;;   writes CGRect = { CGFloat x, CGFloat y, CGFloat width, CGFloat height }
;;   at the provided pointer (4 × f64 = 32 bytes).
;;   Note: Y=0 is top of the primary display on macOS virtual desktop.
;;         Secondary displays may have negative Y coordinates.
;;
;; Result dict shape stored in _monitors[]:
;;   { "name": "Display N", "x": i32, "y": i32, "w": i32, "h": i32 }
;;
;; _cocoa_init() calls CGGetActiveDisplayList to populate _monitors
;; on first create_window. No Xrandr equivalent event exists; call
;; _cocoa_init() again on NSApplicationDidChangeScreenParameters.
