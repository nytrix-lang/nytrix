;; backend/x11/consts.ny
;; Reference: all X11 / EWMH / Motif / Wayland / Xkb constants used by backend/mod.ny.
;; Kept as documentation — the actual defs live inline in mod.ny §4.

;; ── X11 window class / CW mask ────────────────────────────────────────────────
;; X_InputOutput = 1        X_AllocNone = 0      X_TrueColor = 4
;; X_CWBackPixel = 2        X_CWBorderPixel = 8
;; X_CWEventMask = 2048     X_CWColormap = 8192
;; X_PropModeReplace = 0
;; XA_ATOM = 4              XA_CARDINAL = 6

;; ── X11 event types ───────────────────────────────────────────────────────────
;; XEV_KeyPress = 2          XEV_KeyRelease = 3
;; XEV_ButtonPress = 4       XEV_ButtonRelease = 5
;; XEV_MotionNotify = 6      XEV_EnterNotify = 7     XEV_LeaveNotify = 8
;; XEV_FocusIn = 9           XEV_FocusOut = 10
;; XEV_Expose = 12           XEV_ConfigureNotify = 22  XEV_ClientMessage = 33

;; ── X11 event masks ───────────────────────────────────────────────────────────
;; XM_KeyPress = 1           XM_KeyRelease = 2
;; XM_ButtonPress = 4        XM_ButtonRelease = 8
;; XM_PointerMotion = 64     XM_EnterWindow = 16     XM_LeaveWindow = 32
;; XM_Exposure = 32768       XM_StructureNotify = 131072
;; XM_FocusChange = 2097152  XM_PropertyChange = 4194304
;; XM_SubstructureRedirect = 1048576   XM_SubstructureNotify = 524288
;; X_QueuedAfterReading = 1

;; ── X11 cursor shapes (X11/cursorfont.h) ─────────────────────────────────────
;; XC_ARROW = 2   XC_IBEAM = 152   XC_CROSSHAIR = 34
;; XC_HAND = 60   XC_FLEUR = 52

;; ── ICCCM XSizeHints flags ────────────────────────────────────────────────────
;; XSH_PMinSize = 16    XSH_PMaxSize = 32
;; XSH_PWinGravity = 512   XSH_StaticGravity = 10

;; ── EWMH _NET_WM_STATE actions ───────────────────────────────────────────────
;; EWMH_REMOVE = 0   EWMH_ADD = 1   EWMH_TOGGLE = 2

;; ── Motif WM hints ────────────────────────────────────────────────────────────
;; MWM_HINTS_DECORATIONS = 2
;; MWM_DECOR_ALL = 1    MWM_DECOR_NONE = 0

;; ── EWMH atom strings ─────────────────────────────────────────────────────────
;; A_WM_DEL     = "WM_DELETE_WINDOW"
;; A_NET_STATE  = "_NET_WM_STATE"
;; A_FULLSCREEN = "_NET_WM_STATE_FULLSCREEN"
;; A_ABOVE      = "_NET_WM_STATE_ABOVE"
;; A_MAX_V      = "_NET_WM_STATE_MAXIMIZED_VERT"
;; A_MAX_H      = "_NET_WM_STATE_MAXIMIZED_HORZ"
;; A_HIDDEN     = "_NET_WM_STATE_HIDDEN"
;; A_ATTENTION  = "_NET_WM_STATE_DEMANDS_ATTENTION"
;; A_BYPASS     = "_NET_WM_BYPASS_COMPOSITOR"
;; A_OPACITY    = "_NET_WM_WINDOW_OPACITY"
;; A_NET_NAME   = "_NET_WM_NAME"
;; A_NET_ICON   = "_NET_WM_ICON"
;; A_UTF8       = "UTF8_STRING"
;; A_MOTIF      = "_MOTIF_WM_HINTS"
;; A_CLIPBOARD  = "CLIPBOARD"

;; ── Xkb ───────────────────────────────────────────────────────────────────────
;; XKB_CORE_KBD = 256   ;; XkbUseCoreKbd = 0x0100

;; ── Wayland protocol versions ─────────────────────────────────────────────────
;; WL_COMPOSITOR_VER = 4   WL_SHM_VER = 1
;; XDG_WM_BASE_VER   = 2   ZXDG_DECOR_VER = 1
;; WL_SEAT_VER       = 5   WL_OUTPUT_VER  = 2
;; ZXDG_DECORATION_MODE_SERVER_SIDE = 2
