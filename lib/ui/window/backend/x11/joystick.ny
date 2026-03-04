;; backend/x11/joystick.ny
;; Reference: Linux /dev/input/js* joystick polling used by backend/mod.ny.
;;
;; Protocol: each read() from /dev/input/jsN returns 8-byte js_event structs:
;;   { u32 time, i16 value, u8 type, u8 number }
;;   O_NONBLOCK (0x800) used so read returns -1 immediately when no events.
;;
;; type bits:
;;   0x80 = JS_EVENT_INIT  — skip (initial state replay)
;;   0x02 = JS_EVENT_AXIS  — axis value in [-32767, 32767], normalized to [-1.0, 1.0]
;;   0x01 = JS_EVENT_BUTTON — value 0/1
;;
;; Module state in backend/mod.ny:
;;   _js_fds  dict  jid → fd (i32)
;;   _js_axes dict  jid → [f32...]
;;   _js_btns dict  jid → [i32...]
;;
;; joystick_present(jid):
;;   opens /dev/input/js{jid} with O_NONBLOCK; caches fd on success.
;;
;; _js_drain_linux(jid):
;;   drains all pending js_event structs, updating _js_axes / _js_btns.
;;   Called inside get_joystick_axes() / get_joystick_buttons().
