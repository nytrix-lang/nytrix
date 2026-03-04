;; backend/x11/monitors.ny
;; Reference: Xrandr monitor enumeration used by backend/mod.ny _x11_poll_monitors().
;;
;; XRRGetScreenResourcesCurrent layout (offsets from XRRScreenResources*):
;;   +24  i32   nOutput
;;   +32  u64*  outputs  (array of RROutput XIDs)
;;
;; XRRGetOutputInfo layout (offsets from XRROutputInfo*):
;;   +0   u64*  name pointer (char*)
;;   +48  u16   connection  (0 = RR_Connected)
;;   +56  u64   crtc        (RRCrtc XID, 0 if none)
;;
;; XRRGetCrtcInfo layout (offsets from XRRCrtcInfo*):
;;   +16  i32   x
;;   +20  i32   y
;;   +24  u32   width
;;   +28  u32   height
;;
;; Result dict shape stored in _monitors[]:
;;   { "name": str, "x": i32, "y": i32, "w": u32, "h": u32 }
;;
;; Runtime detection: _x11_poll_monitors() is called once during _linux_init()
;; when falling back to X11, and again whenever an XRandR RRNotify event arrives.
