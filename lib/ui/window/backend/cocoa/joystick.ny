;; backend/cocoa/joystick.ny
;; Reference: macOS IOHIDManager joystick stub in backend/mod.ny.
;;
;; Status: STUB — IOHIDManager is initialized and opened, device count is
;; accessible via IOHIDManagerCopyDevices + CFSet count, but individual
;; element value reads (IOHIDDeviceGetValue per IOHIDElement) require
;; a scheduled run-loop callback or synchronous value polling that is
;; not yet wired into the Ny event loop.
;;
;; To complete this implementation:
;;   1. IOHIDManagerScheduleWithRunLoop to hook into the main CFRunLoop
;;   2. IOHIDManagerRegisterInputValueCallback for value change events
;;   3. Parse IOHIDValueRef via IOHIDValueGetIntegerValue
;;   4. Separate by IOHIDElementGetUsagePage / IOHIDElementGetUsage:
;;        kHIDPage_GenericDesktop (0x01) + usage 0x30–0x35 = X,Y,Z,Rx,Ry,Rz axes
;;        kHIDPage_Button (0x09) + usage 1–N = digital buttons
;;
;; For now joystick_present() returns true only if IOHIDManagerCopyDevices
;; count >= jid+1. All axis/button reads return empty arrays.
