;; Keywords: viewer bootstrap startup app runtime os ui render
;; Startup helpers for initializing viewer runtime state and default UI configuration.
;; References:
;; - std.os.ui.render.viewer.app
;; - std.os.ui.render.viewer.runtime
module std.os.ui.render.viewer.bootstrap(
   apply_identity_hints, raw_mouse_enabled, headless_sim_refused, failure_summary,
   open_viewer_window, fit_to_workarea_enabled, live_framebuffer_size, finish_window
)

use std.core
use std.core.common as common
use std.os.ui.render.viewer.app as ui_app
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.viewer.runtime as ui_runtime
use std.os.ui.window
use std.os.ui.window.native as win_native

fn apply_identity_hints(str title_class="NytrixViewer", str title_instance="nytrix-viewer") int {
   "Applies platform window identity hints from env or defaults."
   mut x11_class = common.env_trim("NY_UI_X11_CLASS_NAME")
   mut x11_inst = common.env_trim("NY_UI_X11_INSTANCE_NAME")
   mut wl_app_id = common.env_trim("NY_UI_WAYLAND_APP_ID")
   if(x11_class.len == 0){ x11_class = title_class }
   if(x11_inst.len == 0){ x11_inst = title_instance }
   if(wl_app_id.len == 0){ wl_app_id = x11_inst }
   window.window_hint_string(win_native.X11_CLASS_NAME, x11_class)
   window.window_hint_string(win_native.X11_INSTANCE_NAME, x11_inst)
   window.window_hint_string(win_native.WAYLAND_APP_ID, wl_app_id)
   0
}

fn raw_mouse_enabled() bool {
   "Returns whether raw mouse input was requested by env."
   ui_profile.env_present_cached("NY_UI_RAW_MOUSE") && ui_profile.env_truthy_cached("NY_UI_RAW_MOUSE")
}

fn headless_sim_refused() bool {
   "Returns true when the environment cannot run hidden headless simulation."
   ui_profile.env_present_cached("DISPLAY") ||
   ui_profile.env_present_cached("WAYLAND_DISPLAY") ||
   ui_profile.env_lower_cached("NY_UI_BACKEND") != "none" ||
   !ui_profile.env_truthy_cached("NYTRIX_VK_ALLOW_HEADLESS")
}

fn failure_summary(str backend_name, bool headless_enabled) str {
   "backend=" + backend_name +
   " requested=" + ui_profile.env_lower_cached("NY_UI_BACKEND") +
   " display=" + ui_profile.env_trim_cached("DISPLAY") +
   " wayland=" + ui_profile.env_trim_cached("WAYLAND_DISPLAY") +
   " headless=" + to_str(headless_enabled)
}

fn _windowed_open_flags(bool headless_enabled) int {
   if(headless_enabled){ return window.WINDOW_HIDE }
   window.WINDOW_FOCUS_ON_SHOW | (raw_mouse_enabled() ? window.WINDOW_RAW_MOUSE : 0)
}

fn open_viewer_window(str title, int msaa, bool vsync, bool filter_linear, bool fullscreen, bool headless_enabled) dict {
   "Opens a fullscreen or windowed viewer and returns its handle and size."
   if(fullscreen){
      return {"win": ui_runtime.open_fullscreen(title, msaa, vsync, filter_linear), "w": 0, "h": 0}
   }
   def ext = ui_app.app_window_extent_from_env(headless_enabled)
   def open_w = int(ext.get(0, 1280))
   def open_h = int(ext.get(1, 720))
   {
      "win": ui_runtime.open_windowed(title, open_w, open_h, _windowed_open_flags(headless_enabled), vsync, filter_linear, msaa),
      "w": open_w,
      "h": open_h
   }
}

fn fit_to_workarea_enabled(bool fullscreen, bool headless_enabled) bool {
   "Returns whether the viewer should fit the monitor work area."
   !ui_profile.env_present_cached("CI") &&
   !fullscreen &&
   !headless_enabled &&
   ui_profile.env_truthy_cached("NY_UI_FIT_TO_WORKAREA") &&
   !ui_profile.env_truthy_cached("NY_UI_DISABLE_FIT_TO_WORKAREA")
}

fn live_framebuffer_size(any win, int fallback_w=1280, int fallback_h=720) list {
   "Returns framebuffer size with window size as a fallback."
   def fb = win_native.get_framebuffer_size(window.id(win))
   mut w = float(fb.get(0, 0.0))
   mut h = float(fb.get(1, 0.0))
   if(w <= 0.0 || h <= 0.0){
      def ws = window.size(win)
      w = float(ws.get(0, fallback_w))
      h = float(ws.get(1, fallback_h))
   }
   [w, h]
}

fn finish_window(any win, bool fullscreen, bool headless_enabled) list {
   "Finalizes window visibility, focus, exit key, and framebuffer size."
   window.set_exit_key(win, window.KEY_NULL)
   if(headless_enabled){ window.hide(win) }
   if(fit_to_workarea_enabled(fullscreen, headless_enabled)){ window.fit_to_workarea(win) }
   window.set_should_close(win, false)
   if(!headless_enabled){
      window.show(win)
      window.focus(win)
   }
   live_framebuffer_size(win)
}

#main {
   assert(!fit_to_workarea_enabled(true, false), "fullscreen never workarea-fits")
   assert(failure_summary("vulkan", false).len > 0, "failure summary")
   print("✓ std.os.ui.render.viewer.bootstrap self-test passed")
}
