;; backend/win32/consts.ny
;; Reference: Win32 style / message / VK / XInput / DWM constants used by backend/mod.ny.

;; ── Window styles ─────────────────────────────────────────────────────────────
;; WS_OVERLAPPEDWINDOW = 0x00CF0000  ;; CAPTION|SYSMENU|THICKFRAME|MIN|MAX
;; WS_POPUP            = 0x80000000
;; WS_EX_LAYERED       = 0x00080000  ;; SetLayeredWindowAttributes (opacity)
;; WS_EX_TRANSPARENT   = 0x00000020  ;; click-through / mouse passthrough
;; WS_EX_TOPMOST       = 0x00000008  ;; always-on-top / floating

;; ── ShowWindow commands ───────────────────────────────────────────────────────
;; SW_SHOWNORMAL    = 1   SW_SHOWMINIMIZED = 2
;; SW_SHOWMAXIMIZED = 3   SW_RESTORE       = 9

;; ── GetWindowLongPtrW indices ─────────────────────────────────────────────────
;; GWL_STYLE   = -16   GWL_EXSTYLE = -20

;; ── SetWindowPos Z-order ──────────────────────────────────────────────────────
;; HWND_TOPMOST  = -1    HWND_NOTOPMOST = -2

;; ── Window messages ───────────────────────────────────────────────────────────
;; WM_CLOSE      = 0x0010   WM_SIZE      = 0x0005   WM_MOVE      = 0x0003
;; WM_SETFOCUS   = 0x0007   WM_KILLFOCUS = 0x0008
;; WM_KEYDOWN    = 0x0100   WM_KEYUP     = 0x0101
;; WM_SYSKEYDOWN = 0x0104   WM_SYSKEYUP  = 0x0105
;; WM_LBUTTONDOWN= 0x0201   WM_LBUTTONUP = 0x0202
;; WM_RBUTTONDOWN= 0x0204   WM_RBUTTONUP = 0x0205
;; WM_MBUTTONDOWN= 0x0207   WM_MBUTTONUP = 0x0208
;; WM_MOUSEWHEEL = 0x020A   WM_MOUSEHWHEEL = 0x020E
;; WM_MOUSEMOVE  = 0x0200   WM_SETICON   = 0x0080
;; PM_REMOVE     = 0x0001

;; ── Virtual key codes ─────────────────────────────────────────────────────────
;; VK_LSHIFT=0xA0  VK_RSHIFT=0xA1   VK_LCONTROL=0xA2  VK_RCONTROL=0xA3
;; VK_LMENU=0xA4   VK_RMENU=0xA5    VK_LWIN=0x5B      VK_RWIN=0x5C
;; VK_APPS=0x5D    VK_CAPITAL=0x14  VK_NUMLOCK=0x90   VK_SCROLL=0x91
;; VK_ESCAPE=0x1B  VK_RETURN=0x0D   VK_BACK=0x08      VK_TAB=0x09
;; VK_SPACE=0x20   VK_INSERT=0x2D   VK_DELETE=0x2E
;; VK_HOME=0x24    VK_END=0x23      VK_PRIOR=0x21     VK_NEXT=0x22
;; VK_LEFT=0x25    VK_RIGHT=0x27    VK_UP=0x26        VK_DOWN=0x28
;; VK_F1=0x70 … VK_F12=0x7B
;; Letters A-Z: VK = ASCII uppercase (same as Nytrix key code)
;; Digits  0-9: VK = 0x30–0x39 → Nytrix 48–57
;; OEM keys:  VK_OEM_MINUS=0xBD VK_OEM_PLUS=0xBB VK_OEM_4=0xDB VK_OEM_6=0xDD
;;            VK_OEM_5=0xDC     VK_OEM_1=0xBA     VK_OEM_7=0xDE VK_OEM_3=0xC0
;;            VK_OEM_COMMA=0xBC VK_OEM_PERIOD=0xBE VK_OEM_2=0xBF

;; ── Clipboard ─────────────────────────────────────────────────────────────────
;; CF_UNICODETEXT = 13   GMEM_MOVEABLE = 0x0002

;; ── StretchDIBits ROP ─────────────────────────────────────────────────────────
;; SRCCOPY = 0x00CC0020

;; ── XInput XINPUT_GAMEPAD button masks ────────────────────────────────────────
;; XINPUT_GAMEPAD_DPAD_UP      = 0x0001   DPAD_DOWN  = 0x0002
;; XINPUT_GAMEPAD_DPAD_LEFT    = 0x0004   DPAD_RIGHT = 0x0008
;; XINPUT_GAMEPAD_START        = 0x0010   BACK       = 0x0020
;; XINPUT_GAMEPAD_LEFT_THUMB   = 0x0040   RIGHT_THUMB= 0x0080
;; XINPUT_GAMEPAD_LEFT_SHOULDER= 0x0100   RIGHT      = 0x0200
;; XINPUT_GAMEPAD_A            = 0x1000   B          = 0x2000
;; XINPUT_GAMEPAD_X            = 0x4000   Y          = 0x8000
;;
;; XINPUT_STATE layout (16 bytes):
;;   +0  u32  dwPacketNumber
;;   +4  XINPUT_GAMEPAD gamepad:
;;     +0  u16  wButtons     +2  u8  bLeftTrigger   +3  u8  bRightTrigger
;;     +4  i16  sThumbLX     +6  i16 sThumbLY
;;     +8  i16  sThumbRX     +10 i16 sThumbRY
;; Axes output: [LX, LY, RX, RY, LTrig, RTrig] normalized to [-1.0, 1.0]
;; Buttons output: [A,B,X,Y,LB,RB,BACK,START,LTHUMB,RTHUMB,DU,DR,DD,DL]
