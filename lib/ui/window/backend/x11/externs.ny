;; backend/x11/externs.ny
;; Reference: all X11 + Wayland + xkbcommon + Xrandr + POSIX externs
;; used by backend/mod.ny under  comptime{ __os_name() == "linux" }.
;; Not a standalone module — documents what is linked and why.

;; #link "X11"  #link "Xrandr"  #link "Xcursor"  #link "Xext"
;; #link "wayland-client"  #link "xkbcommon"

;; ── X11 display / screen ─────────────────────────────────────────────────────
;; extern fn XOpenDisplay(n: ptr): ptr
;; extern fn XCloseDisplay(d: ptr): i32
;; extern fn XDefaultScreen(d: ptr): i32
;; extern fn XRootWindow(d: ptr, s: i32): u64
;; extern fn XDefaultDepth(d: ptr, s: i32): i32
;; extern fn XDefaultVisual(d: ptr, s: i32): ptr
;; extern fn XDefaultColormap(d: ptr, s: i32): u64
;; extern fn XMatchVisualInfo(d: ptr, s: i32, depth: i32, cls: i32, vi: ptr): i32
;; extern fn XCreateColormap(d: ptr, w: u64, v: ptr, a: i32): u64
;; extern fn XFreeColormap(d: ptr, c: u64): i32

;; ── X11 window lifecycle ─────────────────────────────────────────────────────
;; extern fn XCreateWindow(d: ptr, par: u64, x: i32, y: i32, w: u32, h: u32, bw: u32, depth: i32, cls: u32, vis: ptr, mask: u64, at: ptr): u64
;; extern fn XDestroyWindow(d: ptr, w: u64): i32
;; extern fn XMapWindow(d: ptr, w: u64): i32
;; extern fn XUnmapWindow(d: ptr, w: u64): i32
;; extern fn XRaiseWindow(d: ptr, w: u64): i32
;; extern fn XIconifyWindow(d: ptr, w: u64, s: i32): i32
;; extern fn XMoveWindow(d: ptr, w: u64, x: i32, y: i32): i32
;; extern fn XResizeWindow(d: ptr, w: u64, ww: u32, h: u32): i32

;; ── Atoms / WM / properties ───────────────────────────────────────────────────
;; extern fn XStoreName(d: ptr, w: u64, n: ptr): i32
;; extern fn XInternAtom(d: ptr, n: ptr, e: i32): u64
;; extern fn XSetWMProtocols(d: ptr, w: u64, p: ptr, n: i32): i32
;; extern fn XGetWindowProperty(d: ptr, w: u64, p: u64, off: i64, len: i64, del: i32, rt: u64, at: ptr, af: ptr, ni: ptr, ba: ptr, data: ptr): i32
;; extern fn XChangeProperty(d: ptr, w: u64, p: u64, t: u64, f: i32, m: i32, data: ptr, n: i32): i32
;; extern fn XDeleteProperty(d: ptr, w: u64, p: u64): i32
;; extern fn XFree(p: ptr): i32
;; extern fn XAllocSizeHints(): ptr
;; extern fn XSetWMNormalHints(d: ptr, w: u64, h: ptr)
;; extern fn XAllocClassHint(): ptr
;; extern fn XSetClassHint(d: ptr, w: u64, h: ptr)
;; extern fn Xutf8SetWMProperties(d: ptr, w: u64, wn: ptr, in: ptr, av: ptr, ac: i32, h: ptr, wh: ptr, ch: ptr)

;; ── Events ────────────────────────────────────────────────────────────────────
;; extern fn XPending(d: ptr): i32
;; extern fn XNextEvent(d: ptr, e: ptr): i32
;; extern fn XPeekEvent(d: ptr, e: ptr): i32
;; extern fn XEventsQueued(d: ptr, m: i32): i32
;; extern fn XSendEvent(d: ptr, w: u64, p: i32, m: i64, e: ptr): i32
;; extern fn XFlush(d: ptr): i32
;; extern fn XSync(d: ptr, dis: i32): i32
;; extern fn XFilterEvent(e: ptr, w: u64): i32

;; ── Input / cursor ────────────────────────────────────────────────────────────
;; extern fn XSetInputFocus(d: ptr, w: u64, r: i32, t: u64): i32
;; extern fn XDefineCursor(d: ptr, w: u64, c: u64): i32
;; extern fn XUndefineCursor(d: ptr, w: u64): i32
;; extern fn XCreateFontCursor(d: ptr, sh: u32): u64
;; extern fn XFreeCursor(d: ptr, c: u64): i32
;; extern fn XQueryPointer(d: ptr, w: u64, rr: ptr, cr: ptr, rx: ptr, ry: ptr, wx: ptr, wy: ptr, mr: ptr): i32
;; extern fn XWarpPointer(d: ptr, sw: u64, dw: u64, sx: i32, sy: i32, sw2: u32, sh2: u32, dx: i32, dy: i32): i32

;; ── Clipboard ─────────────────────────────────────────────────────────────────
;; extern fn XSetSelectionOwner(d: ptr, s: u64, o: u64, t: u64): i32

;; ── Xkb ───────────────────────────────────────────────────────────────────────
;; extern fn XkbGetState(d: ptr, dev: u32, out: ptr): i32

;; ── Xrandr ────────────────────────────────────────────────────────────────────
;; extern fn XRRGetScreenResourcesCurrent(d: ptr, w: u64): ptr
;; extern fn XRRFreeScreenResources(r: ptr)
;; extern fn XRRGetOutputInfo(d: ptr, r: ptr, o: u64): ptr
;; extern fn XRRFreeOutputInfo(i: ptr)
;; extern fn XRRGetCrtcInfo(d: ptr, r: ptr, c: u64): ptr
;; extern fn XRRFreeCrtcInfo(i: ptr)

;; ── POSIX / joystick ─────────────────────────────────────────────────────────
;; extern fn poll(fds: ptr, n: u64, t: i32): i32
;; extern fn ConnectionNumber(d: ptr): i32      ;; XConnectionNumber
;; extern fn open(path: ptr, flags: i32): i32
;; extern fn read(fd: i32, buf: ptr, n: u64): i64
;; extern fn close(fd: i32): i32
;; extern fn memfd_create(n: ptr, flags: u32): i32
;; extern fn ftruncate(fd: i32, size: i64): i32
;; extern fn mmap(addr: ptr, len: u64, prot: i32, flags: i32, fd: i32, off: i64): ptr
;; extern fn munmap(addr: ptr, len: u64): i32

;; ── Wayland core ─────────────────────────────────────────────────────────────
;; extern fn wl_display_connect(n: ptr): ptr
;; extern fn wl_display_disconnect(d: ptr)
;; extern fn wl_display_get_fd(d: ptr): i32
;; extern fn wl_display_dispatch(d: ptr): i32
;; extern fn wl_display_dispatch_pending(d: ptr): i32
;; extern fn wl_display_roundtrip(d: ptr): i32
;; extern fn wl_display_flush(d: ptr): i32
;; extern fn wl_display_get_registry(d: ptr): ptr
;; extern fn wl_registry_add_listener(r: ptr, l: ptr, data: ptr): i32
;; extern fn wl_registry_bind(r: ptr, name: u32, iface: ptr, ver: u32): ptr

;; ── Wayland compositor / surface ─────────────────────────────────────────────
;; extern fn wl_compositor_create_surface(c: ptr): ptr
;; extern fn wl_surface_destroy(s: ptr)
;; extern fn wl_surface_commit(s: ptr)
;; extern fn wl_surface_attach(s: ptr, b: ptr, x: i32, y: i32)
;; extern fn wl_surface_damage_buffer(s: ptr, x: i32, y: i32, w: i32, h: i32)

;; ── xdg-shell ─────────────────────────────────────────────────────────────────
;; extern fn xdg_wm_base_add_listener(b: ptr, l: ptr, d: ptr): i32
;; extern fn xdg_wm_base_get_xdg_surface(b: ptr, s: ptr): ptr
;; extern fn xdg_wm_base_pong(b: ptr, serial: u32)
;; extern fn xdg_surface_add_listener(s: ptr, l: ptr, d: ptr): i32
;; extern fn xdg_surface_get_toplevel(s: ptr): ptr
;; extern fn xdg_surface_ack_configure(s: ptr, serial: u32)
;; extern fn xdg_surface_destroy(s: ptr)
;; extern fn xdg_toplevel_set_title(t: ptr, ttl: ptr)
;; extern fn xdg_toplevel_set_app_id(t: ptr, id: ptr)
;; extern fn xdg_toplevel_set_fullscreen(t: ptr, out: ptr)
;; extern fn xdg_toplevel_unset_fullscreen(t: ptr)
;; extern fn xdg_toplevel_set_maximized(t: ptr)
;; extern fn xdg_toplevel_unset_maximized(t: ptr)
;; extern fn xdg_toplevel_set_minimized(t: ptr)
;; extern fn xdg_toplevel_set_min_size(t: ptr, w: i32, h: i32)
;; extern fn xdg_toplevel_set_max_size(t: ptr, w: i32, h: i32)
;; extern fn xdg_toplevel_destroy(t: ptr)

;; ── zxdg-decoration-v1 ────────────────────────────────────────────────────────
;; extern fn zxdg_decoration_manager_v1_get_toplevel_decoration(m: ptr, t: ptr): ptr
;; extern fn zxdg_toplevel_decoration_v1_set_mode(d: ptr, mode: u32)
;; extern fn zxdg_toplevel_decoration_v1_destroy(d: ptr)

;; ── wl_seat / pointer / keyboard ──────────────────────────────────────────────
;; extern fn wl_seat_get_pointer(s: ptr): ptr
;; extern fn wl_seat_get_keyboard(s: ptr): ptr
;; extern fn wl_pointer_add_listener(p: ptr, l: ptr, d: ptr): i32
;; extern fn wl_keyboard_add_listener(k: ptr, l: ptr, d: ptr): i32
;; extern fn wl_pointer_set_cursor(p: ptr, serial: u32, s: ptr, hx: i32, hy: i32)

;; ── wl_shm (software pixel blit) ─────────────────────────────────────────────
;; extern fn wl_shm_create_pool(s: ptr, fd: i32, size: i32): ptr
;; extern fn wl_shm_pool_create_buffer(p: ptr, off: i32, w: i32, h: i32, stride: i32, fmt: u32): ptr
;; extern fn wl_shm_pool_destroy(p: ptr)
;; extern fn wl_buffer_destroy(b: ptr)
;; extern fn wl_output_add_listener(o: ptr, l: ptr, d: ptr): i32

;; ── xkbcommon ─────────────────────────────────────────────────────────────────
;; extern fn xkb_context_new(flags: u32): ptr
;; extern fn xkb_context_unref(c: ptr)
;; extern fn xkb_keymap_new_from_string(c: ptr, s: ptr, fmt: u32, flags: u32): ptr
;; extern fn xkb_keymap_unref(k: ptr)
;; extern fn xkb_state_new(k: ptr): ptr
;; extern fn xkb_state_unref(s: ptr)
;; extern fn xkb_state_key_get_syms(s: ptr, key: u32, out: ptr): i32
;; extern fn xkb_state_update_mask(s: ptr, dm: u32, lm: u32, lk: u32, dg: u32, lg: u32, lkg: u32): u32
;; extern fn xkb_state_mod_name_is_active(s: ptr, n: ptr, comp: u32): i32
