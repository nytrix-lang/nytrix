#!/usr/bin/env ny

;; Keywords: ui input keyboard gamepad controller example
;; Unified input visualizer. Tab switches keyboard/gamepad; active inputs can auto-follow.
use std.core
use std.math (max)
use std.os (ticks)
use std.os.ui.window.consts
use std.os.ui.render
use std.os.ui.render.viewer.runtime as ui_runtime
use std.os.ui.render.viewer
use std.os.ui.render.dump as ui_dump
use std.os.ui.render.viewer.gamepad
use std.os.ui.render.viewer.input
use std.os.ui.render.viewer.keyboard
use std.os.ui.render.viewer.widgets
use std.os.ui.window

def START_W = 1040
def START_H = 620
def START_FLAGS = consts.WINDOW_CENTER | consts.WINDOW_FOCUS_ON_SHOW
def win = render.init_window(START_W, START_H, "Nytrix Input", START_FLAGS, false, true, 1)

if(!win){ panic("window init failed") }
window.set_exit_key(win, consts.KEY_NULL)
def fonts = viewer.FONT_CANDIDATES
def font_title = render.font_load_first(fonts, 28)
def font_body = render.font_load_first(fonts, 17)
def font_key = render.font_load_first(fonts, 13)
viewer.set_font(font_body)
def auto_dump_delay = ui_dump.auto_dump_delay_frames(8)
def timeout_limit = ui_runtime.timeout_ns(0)
mut fps_state = ui_runtime.fps_begin()
mut active = "keyboard"
mut last_key = "-"
mut last_code = consts.KEY_NULL
mut last_key_timer = 0.0
mut last_pad_sig = ""
mut pad_state_cache = 0
mut pad_state_jid = -1
mut pad_state_changed = false
mut next_pad_sample_ticks = 0
mut pad_info_cache = dict(0)
mut pad_info_sig = ""
mut frame_count = 0
mut auto_dump_done = false
def PAD_SAMPLE_ACTIVE_NS = 4000000
def PAD_SAMPLE_IDLE_NS = 12000000

fn release_cached_pad() int {
   if(pad_state_cache){ gamepad.release(pad_state_cache) }
   pad_state_cache = 0
   pad_state_jid = -1
   0
}

fn sampled_pad_state(int jid, bool active_view) any {
   if(jid < 0){
      release_cached_pad()
      pad_state_changed = true
      return 0
   }
   def now = ticks()
   if(pad_state_cache && pad_state_jid == jid && now < next_pad_sample_ticks){
      pad_state_changed = false
      return pad_state_cache
   }
   release_cached_pad()
   pad_state_cache = gamepad.snapshot(jid)
   pad_state_jid = jid
   next_pad_sample_ticks = now + (active_view ? PAD_SAMPLE_ACTIVE_NS : PAD_SAMPLE_IDLE_NS)
   pad_state_changed = true
   pad_state_cache
}

fn cached_pad_info(any pad_state, list rows, int jid, str sig) dict {
   if(!pad_state){ return dict(0) }
   def key = sig + "|rows=" + to_str(rows.len) + "|jid=" + to_str(jid)
   if(key == pad_info_sig && is_dict(pad_info_cache)){ return pad_info_cache }
   pad_info_cache = input.pad_info(pad_state, rows, jid)
   pad_info_sig = key
   pad_info_cache
}

while(!render.window_should_close(win)){
   gamepad.refresh(false)
   if(!render.begin_frame_clear(widgets.C_BG)){ continue }
   def dt = render.get_delta_time()
   fps_state = ui_runtime.fps_tick(fps_state, dt)
   def fps = ui_runtime.fps_current(fps_state, dt)
   def fb = render.framebuffer_size_f64(START_W, START_H)
   def sw = max(1.0, float(fb.get(0, START_W)))
   def sh = max(1.0, float(fb.get(1, START_H)))
   render.set_ortho_2d(0.0, sw, 0.0, sh)
   def rows = gamepad.rows()
   def jid = gamepad.best_jid()
   def pad_state = sampled_pad_state(jid, active == "gamepad")
   if(pad_state){
      def sig = (pad_state_changed || last_pad_sig == "") ? gamepad.signature(pad_state) : last_pad_sig
      if(last_pad_sig == ""){ last_pad_sig = sig }
      elif(input.pad_active(pad_state)){ active = "gamepad" }
      last_pad_sig = sig
   } else { last_pad_sig = "" }
   def tab = window.key_pressed(win, consts.KEY_TAB)
   def pressed = tab ? 0 : keyboard.scan_pressed(win)
   if(tab){
      active = active == "gamepad" ? "keyboard" : "gamepad"
      last_code = consts.KEY_TAB
      last_key = "TAB"
      last_key_timer = 1.0
   } elif(pressed){
      active = "keyboard"
      last_code = int(pressed.get("code", consts.KEY_NULL))
      last_key = to_str(pressed.get("label", "-"))
      last_key_timer = 1.0
   }
   last_key_timer = max(0.0, last_key_timer - dt)
   def pad = input.pad(sw, sh)
   if(active == "gamepad"){
      input.draw_gamepad(font_title, font_body, pad_state, rows, jid, fps, sw, sh, pad, cached_pad_info(pad_state, rows, jid, last_pad_sig))
   } else {
      input.draw_keyboard(win, font_title, font_body, font_key, last_key, last_code, last_key_timer, fps, sw, sh, pad)
   }
   ui_dump.auto_dump_pre_frame(auto_dump_done, frame_count, auto_dump_delay)
   render.end_frame()
   frame_count += 1
   auto_dump_done = ui_dump.auto_dump_post_frame(win, auto_dump_done, frame_count, auto_dump_delay, "build/cache/fb/ui/input.png")
   ui_runtime.close_on_timeout(win, int(fps_state.get("start", 0)), timeout_limit)
}

release_cached_pad()
ui_runtime.fps_finish("input", fps_state)
render.close_window()
