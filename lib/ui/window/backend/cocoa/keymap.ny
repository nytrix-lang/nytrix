;; backend/cocoa/keymap.ny
;; Reference: macOS hardware key codes (kVK_*) → Nytrix key code mapping.
;; The actual _init_keys() for macos is compiled inline in backend/mod.ny §7.
;;
;; macOS key codes are hardware-level (independent of keyboard layout).
;; Source: Carbon/HIToolbox/Events.h  kVK_* constants.
;;
;; kVK → Nytrix  (format: kVK : nytrix ;; label)
;;  0x00 : 65  ;; A          0x01 : 83  ;; S    0x02 : 68  ;; D
;;  0x03 : 70  ;; F          0x04 : 72  ;; H    0x05 : 71  ;; G
;;  0x06 : 90  ;; Z          0x07 : 88  ;; X    0x08 : 67  ;; C
;;  0x09 : 86  ;; V          0x0B : 66  ;; B    0x0C : 81  ;; Q
;;  0x0D : 87  ;; W          0x0E : 69  ;; E    0x0F : 82  ;; R
;;  0x10 : 89  ;; Y          0x11 : 84  ;; T    0x1F : 79  ;; O
;;  0x20 : 85  ;; U          0x22 : 73  ;; I    0x23 : 80  ;; P
;;  0x25 : 76  ;; L          0x26 : 74  ;; J    0x28 : 75  ;; K
;;  0x2D : 78  ;; N          0x2E : 77  ;; M
;; Number row:
;;  0x1D:48 0x12:49 0x13:50 0x14:51 0x15:52 0x17:53
;;  0x16:54 0x1A:55 0x1C:56 0x19:57
;; Punctuation:
;;  0x27:39(')  0x2A:92(\)  0x2B:44(,)  0x18:61(=)  0x32:96(`)
;;  0x21:91([)  0x1B:45(-)  0x2F:46(.)  0x1E:93(])  0x29:59(;)
;;  0x2C:47(/)  0x31:32( )
;; Control:
;;  0x35:256(ESC)  0x24:257(RETURN)  0x30:258(TAB)  0x33:259(BACKSPACE/DELETE)
;;  0x72:260(INS)  0x75:261(DEL)    0x7C:262(→)    0x7B:263(←)
;;  0x7D:264(↓)    0x7E:265(↑)      0x79:266(PgDn)  0x74:267(PgUp)
;;  0x73:268(Home) 0x77:269(End)    0x39:280(CapsLk)
;; Function keys:
;;  0x7A:290(F1) 0x78:291(F2) 0x63:292(F3) 0x76:293(F4)
;;  0x60:294(F5) 0x61:295(F6) 0x62:296(F7) 0x64:297(F8)
;;  0x65:298(F9) 0x6D:299(F10) 0x67:300(F11) 0x6F:301(F12)
;; Modifiers (reported via FlagsChanged NSEvent):
;;  0x38:340(LShift) 0x3C:344(RShift)
;;  0x3B:341(LCtrl)  0x3E:345(RCtrl)
;;  0x3A:342(LAlt)   0x3D:346(RAlt)
;;  0x37:343(LCmd)   0x36:347(RCmd)
