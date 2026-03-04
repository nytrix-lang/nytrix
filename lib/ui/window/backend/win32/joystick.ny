;; backend/win32/joystick.ny
;; Reference: XInput gamepad backend used by backend/mod.ny §18.
;;
;; XInputGetState(idx, XINPUT_STATE*) → ERROR_SUCCESS (0) if connected.
;; Called with indices 0–3 (max 4 simultaneous controllers).
;;
;; _js_drain_windows(jid):
;;   reads XINPUT_STATE, parses wButtons bitmask + thumb/trigger raw values,
;;   normalizes:
;;     thumbstick axes: i16 / 32767.0  → [-1.0, 1.0]
;;     trigger axes:    u8  / 255.0    → [ 0.0, 1.0]
;;   outputs 6 axes:  [LX, LY, RX, RY, LTrigger, RTrigger]
;;   outputs 14 buttons in GLFW order: A B X Y LB RB BACK START LT RT DU DR DD DL
;;
;; Module state in backend/mod.ny:
;;   _js_axes dict  jid → [f32×6]
;;   _js_btns dict  jid → [i32×14]
