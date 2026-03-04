;; backend/win32/externs.ny
;; Reference: all Win32 API externs used by backend/mod.ny §3 / §10–§18.
;; Linked: user32 gdi32 dwmapi xinput shell32

;; ── Module / class ────────────────────────────────────────────────────────────
;; extern fn GetModuleHandleW(n: ptr): ptr
;; extern fn GetModuleHandleA(n: ptr): ptr
;; extern fn RegisterClassExW(wc: ptr): u16       ;; WNDCLASSEXW* → atom

;; ── Window lifecycle ──────────────────────────────────────────────────────────
;; extern fn CreateWindowExW(ex: u32, cls: ptr, ttl: ptr, sty: u32, x: i32, y: i32, w: i32, h: i32, par: ptr, men: ptr, ins: ptr, p2: ptr): ptr
;; extern fn DestroyWindow(h: ptr): i32
;; extern fn ShowWindow(h: ptr, cmd: i32): i32
;; extern fn UpdateWindow(h: ptr): i32
;; extern fn GetClientRect(h: ptr, rc: ptr): i32  ;; RECT*
;; extern fn GetWindowRect(h: ptr, rc: ptr): i32  ;; RECT*
;; extern fn SetWindowPos(h: ptr, af: ptr, x: i32, y: i32, w: i32, h2: i32, fl: u32): i32
;; extern fn MoveWindow(h: ptr, x: i32, y: i32, w: i32, h2: i32, r: i32): i32
;; extern fn IsIconic(h: ptr): i32
;; extern fn IsZoomed(h: ptr): i32
;; extern fn IsWindowVisible(h: ptr): i32
;; extern fn GetWindowLongPtrW(h: ptr, i: i32): i64
;; extern fn SetWindowLongPtrW(h: ptr, i: i32, v: i64): i64
;; extern fn SetWindowTextW(h: ptr, t: ptr): i32
;; extern fn SetForegroundWindow(h: ptr): i32
;; extern fn SetLayeredWindowAttributes(h: ptr, k: u32, a: u8, fl: u32): i32
;; extern fn FlashWindowEx(fwi: ptr): i32         ;; FLASHWINFO*
;; extern fn SendMessageW(h: ptr, msg: u32, wp: u64, lp: i64): i64

;; ── Message loop ──────────────────────────────────────────────────────────────
;; extern fn PeekMessageW(msg: ptr, h: ptr, mn: u32, mx: u32, rm: u32): i32  ;; MSG*
;; extern fn TranslateMessage(msg: ptr): i32
;; extern fn DispatchMessageW(msg: ptr): i64
;; extern fn MsgWaitForMultipleObjectsEx(n: u32, h: ptr, ms: u32, mask: u32, fl: u32): u32

;; ── Input / cursor ────────────────────────────────────────────────────────────
;; extern fn GetCursorPos(pt: ptr): i32           ;; POINT*
;; extern fn SetCursorPos(x: i32, y: i32): i32
;; extern fn ScreenToClient(h: ptr, pt: ptr): i32
;; extern fn ClientToScreen(h: ptr, pt: ptr): i32
;; extern fn ShowCursor(show: i32): i32
;; extern fn ClipCursor(rc: ptr): i32             ;; RECT* or NULL
;; extern fn GetKeyState(vk: i32): i16            ;; bit 15 = down, bit 0 = toggled

;; ── Clipboard ─────────────────────────────────────────────────────────────────
;; extern fn OpenClipboard(h: ptr): i32
;; extern fn CloseClipboard(): i32
;; extern fn EmptyClipboard(): i32
;; extern fn GetClipboardData(fmt: u32): ptr
;; extern fn SetClipboardData(fmt: u32, d: ptr): ptr
;; extern fn IsClipboardFormatAvailable(fmt: u32): i32
;; extern fn GlobalAlloc(fl: u32, sz: u64): ptr
;; extern fn GlobalLock(m: ptr): ptr
;; extern fn GlobalUnlock(m: ptr): i32

;; ── GDI (software blit) ───────────────────────────────────────────────────────
;; extern fn GetDC(h: ptr): ptr
;; extern fn ReleaseDC(h: ptr, dc: ptr): i32
;; extern fn StretchDIBits(dc: ptr, dx: i32, dy: i32, dw: i32, dh: i32, sx: i32, sy: i32, sw: i32, sh: i32, bits: ptr, bmi: ptr, u: u32, rop: u32): i32

;; ── Icon ──────────────────────────────────────────────────────────────────────
;; extern fn CreateIconFromResourceEx(b: ptr, sz: u32, ico: i32, ver: u32, w: i32, h: i32, fl: u32): ptr

;; ── XInput (gamepad) ──────────────────────────────────────────────────────────
;; extern fn XInputGetState(idx: u32, st: ptr): u32   ;; XINPUT_STATE*

;; ── String conversion ─────────────────────────────────────────────────────────
;; extern fn MultiByteToWideChar(cp: u32, fl: u32, src: ptr, sl: i32, dst: ptr, dl: i32): i32
;; extern fn WideCharToMultiByte(cp: u32, fl: u32, src: ptr, sl: i32, dst: ptr, dl: i32, df: ptr, du: ptr): i32

;; ── Monitor enumeration ───────────────────────────────────────────────────────
;; extern fn EnumDisplayDevicesW(dev: ptr, idx: u32, dd: ptr, fl: u32): i32
;; extern fn EnumDisplaySettingsW(dev: ptr, mode: u32, dm: ptr): i32
