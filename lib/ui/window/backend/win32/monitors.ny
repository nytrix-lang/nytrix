;; backend/win32/monitors.ny
;; Reference: Win32 monitor enumeration used by backend/mod.ny _collect_monitors_win().
;;
;; Uses EnumDisplayDevicesW + EnumDisplaySettingsW (no callback needed).
;;
;; DISPLAY_DEVICEW layout (840 bytes, cb field at +0):
;;   +0   u32    cb (must be set to 840 before call)
;;   +4   wchar  DeviceName[32]  (primary adapter e.g. "\\.\DISPLAY1")
;;   +68  wchar  DeviceString[128]
;;   +132 u32    StateFlags  (bit 0 = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
;;   ...
;;
;; DEVMODEW layout (220 bytes, dmSize at +0):
;;   +0   u16    dmSize (must be 220)
;;   +44  i32    dmPosition.x      (virtual desktop X)
;;   +48  i32    dmPosition.y      (virtual desktop Y)
;;   +52  u32    dmPelsWidth       (pixel width)
;;   +56  u32    dmPelsHeight      (pixel height)
;;   +72  u32    dmDisplayFrequency
;;
;; EnumDisplaySettingsW mode = 0xFFFFFFFF → ENUM_CURRENT_SETTINGS (current mode).
;;
;; Result dict shape stored in _monitors[]:
;;   { "name": str, "x": i32, "y": i32, "w": u32, "h": u32 }
