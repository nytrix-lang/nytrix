;; backend/x11/wayland.ny
;; Reference: Wayland-specific behaviors implemented in backend/mod.ny §9.
;;
;; Runtime detection: wl_display_connect(0) is tried first in _linux_init().
;; If it succeeds, _use_wayland = true and the X11 path is not initialized.
;;
;; Registry bind flow:
;;   _linux_init() calls wl_display_roundtrip() twice.
;;   A C shim calls _wl_bind(name, iface_str, ver) for each global advertised.
;;   _wl_bind() calls wl_registry_bind() and stores:
;;     wl_compositor → _wl_comp
;;     wl_shm        → _wl_shm
;;     xdg_wm_base   → _wl_xdg_base
;;     zxdg_decoration_manager_v1 → _wl_decor_mgr
;;     wl_seat       → _wl_seat  (then _wl_setup_seat())
;;     wl_output     → appended to _wl_outputs []
;;
;; Window creation (_wl_create_window):
;;   wl_compositor_create_surface → xdg_wm_base_get_xdg_surface →
;;   xdg_surface_get_toplevel → set title / app_id →
;;   optional zxdg server-side decoration request →
;;   wl_surface_commit + roundtrip
;;
;; Event loop (_wl_poll / _wl_wait):
;;   wl_display_flush + poll(fd, POLLIN, timeout) +
;;   wl_display_dispatch_pending
;;   Keyboard events arrive via wl_keyboard listener C shim →
;;   _wl_set_keymap / _wl_update_mods / _wl_key_event
;;
;; Software blit (blit_buffer on Wayland):
;;   memfd_create → ftruncate → mmap → memcpy pixels →
;;   wl_shm_create_pool → wl_shm_pool_create_buffer →
;;   wl_surface_attach + wl_surface_damage_buffer + wl_surface_commit →
;;   munmap + close + wl_buffer_destroy
;;   Format: WL_SHM_FORMAT_ARGB8888 = 0
;;
;; Window state queries on Wayland:
;;   is_maximized / is_minimized tracked via "maximized"/"minimized" keys
;;   in the window state dict, updated from xdg_toplevel configure events.
;;
;; Limitations vs X11:
;;   set_cursor_pos — not implemented (requires zwp_locked_pointer_v1)
;;   set_mouse_passthrough — no standard Wayland protocol
;;   request_attention — no standard protocol (compositor-specific)
;;   get_pos — position tracked from windowDidMove-equivalent configure events
