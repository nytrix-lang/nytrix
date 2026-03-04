;; backend/x11/keymap.ny
;; Reference: Linux X11 scancode → Nytrix key code table.
;; The actual _init_keys() for linux is compiled inline in backend/mod.ny §7.
;; Key codes here use the X11 hardware keycode (scancode) as the input.

;; Scancodes are hardware-level (kernel evdev codes + 8 on X11).
;; The mapping targets GLFW-compatible Nytrix key values:
;;   printable ASCII → their ASCII value (A=65, Z=90, 0=48 … 9=57)
;;   special keys    → GLFW-style extended codes (ESC=256, ENTER=257 …)
;;   modifiers       → 340–348 (LSHIFT, LCTRL, LALT, LSUPER … RSUPER, MENU)

;; Scancode → Nytrix  (format: scancode : nytrix_key ;; label)
;;   9  : 256   ;; ESC
;;  10  : 49    ;; 1
;;  11  : 50    ;; 2
;;  12  : 51    ;; 3   ...  19 : 48  ;; 0
;;  20  : 45    ;; -    21  : 61  ;; =
;;  22  : 259   ;; BACKSPACE
;;  23  : 258   ;; TAB
;;  24–35 : QWERTYUIOP[]   (81,87,69,82,84,89,85,73,79,80,91,93)
;;  36  : 257   ;; ENTER
;;  37  : 341   ;; CTRL_L
;;  38–46 : ASDFGHJKL      (65,83,68,70,71,72,74,75,76)
;;  47  : 59    ;; ;    48  : 39  ;; '    49  : 96  ;; `
;;  50  : 340   ;; SHIFT_L
;;  51  : 92    ;; \
;;  52–61 : ZXCVBNM,./     (90,88,67,86,66,78,77,44,46,47)
;;  62  : 344   ;; SHIFT_R
;;  64  : 342   ;; ALT_L    65 : 32 ;; SPACE    66 : 280 ;; CAPS
;;  67–76 : F1–F10          (290–299)
;;  77  : 282   ;; NUM_LOCK    78 : 281 ;; SCROLL_LOCK
;;  95  : 300   ;; F11         96 : 301 ;; F12
;; 105  : 345   ;; CTRL_R    108 : 346 ;; ALT_R
;; 110  : 268   ;; HOME      111 : 265 ;; UP
;; 112  : 266   ;; PAGE_UP   113 : 263 ;; LEFT
;; 114  : 262   ;; RIGHT     115 : 269 ;; END
;; 116  : 264   ;; DOWN      117 : 267 ;; PAGE_DOWN
;; 118  : 260   ;; INSERT    119 : 261 ;; DELETE
;; 133  : 343   ;; SUPER_L   134 : 347 ;; SUPER_R   135 : 348 ;; MENU
