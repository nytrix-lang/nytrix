;; backend/win32/keymap.ny
;; Reference: Win32 VK code → Nytrix key code mapping used by backend/mod.ny §7.
;;
;; Strategy: VK codes for letters (0x41–0x5A) map directly to ASCII uppercase (same value).
;; Digit VKs (0x30–0x39) map to ASCII 48–57.
;; OEM punctuation VKs map to their printable ASCII characters.
;; Special VKs map to GLFW-compatible Nytrix extended codes.
;;
;; Modifier VKs (left/right separate):
;;   0xA0 LSHIFT→340  0xA1 RSHIFT→344
;;   0xA2 LCTRL→341   0xA3 RCTRL→345
;;   0xA4 LALT→342    0xA5 RALT→346
;;   0x5B LWIN→343    0x5C RWIN→347   0x5D APPS→348
;;
;; Current modifier state read via GetKeyState() per-frame in _win_mods():
;;   bit 15 = currently down, bit 0 = toggled (CapsLock/NumLock)
;;   Checks pairs: GetKeyState(0xA0)|GetKeyState(0xA1) for shift etc.
