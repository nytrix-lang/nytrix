#!/usr/bin/env ny

;; Keywords: synth piano keyboard supersaw fx delay reverb chorus flanger bitcrusher ui example
;; Nytrix Synth

use std.core
use std.math
use std.os.sound as snd
use std.os.sound.source.synth as synth
use std.os.ui.window.consts as key
use std.os.ui.render as gfx
use std.os.ui.render.viewer as viewer
use std.os.ui.window as window


def RATE = 48000
def BASE = 440.0
def PAD_SEC = 0.64
def W0 = 1280
def H0 = 760
def FLAGS = key.WINDOW_CENTER | key.WINDOW_FOCUS_ON_SHOW | key.WINDOW_VULKAN

def BG = [0.014, 0.017, 0.026, 1.0]
def DOCK = [0.024, 0.030, 0.048, 1.0]
def CARD = [0.043, 0.052, 0.078, 1.0]
def CARD2 = [0.066, 0.078, 0.112, 1.0]
def LINE = [0.15, 0.17, 0.26, 1.0]
def WHITE = [0.92, 0.94, 1.0, 1.0]
def MUTED = [0.50, 0.54, 0.68, 1.0]
def BLUE = [0.24, 0.84, 1.0, 1.0]
def PURPLE = [0.56, 0.34, 1.0, 1.0]
def GREEN = [0.32, 1.0, 0.50, 1.0]
def RED = [1.0, 0.30, 0.30, 1.0]
def KEY_WHITE = [0.72, 0.76, 0.91, 1.0]
def KEY_BLACK = [0.026, 0.031, 0.046, 1.0]
def KEY_TEXT = [0.07, 0.08, 0.12, 1.0]

def MAP = [
   [key.KEY_A, 0,  "A", "C",  false, 0], [key.KEY_W, 1,  "W", "C#", true,  0],
   [key.KEY_S, 2,  "S", "D",  false, 1], [key.KEY_E, 3,  "E", "D#", true,  1],
   [key.KEY_D, 4,  "D", "E",  false, 2], [key.KEY_F, 5,  "F", "F",  false, 3],
   [key.KEY_T, 6,  "T", "F#", true,  3], [key.KEY_G, 7,  "G", "G",  false, 4],
   [key.KEY_Y, 8,  "Y", "G#", true,  4], [key.KEY_H, 9,  "H", "A",  false, 5],
   [key.KEY_U, 10, "U", "A#", true,  5], [key.KEY_J, 11, "J", "B",  false, 6],
   [key.KEY_K, 12, "K", "C",  false, 7], [key.KEY_O, 13, "O", "C#", true,  7],
   [key.KEY_L, 14, "L", "D",  false, 8], [key.KEY_P, 15, "P", "D#", true,  8],
   [key.KEY_SEMICOLON, 16, ";", "E", false, 9]
]

def PRESETS = ["clean", "wide", "echo", "space", "lofi", "crush", "flange"]

mut AUDIO_OK = false
mut SRC_PAD = 0
mut SRC_HIT = 0

mut OCT = 1
mut held = []
mut inst = []
mut last = "-"
mut patch = "wide"
mut pulse = 0.0
mut mouse_note = -1

mut p_master = 0.74
mut p_drive = 0.045
mut p_comp = 0.24
mut p_hpf = 42.0
mut p_lpf = 13500.0
mut p_delay = 0.025
mut p_time = 0.16
mut p_fb = 0.10
mut p_reverb = 0.035
mut p_chorus = 0.18
mut p_flanger = 0.0
mut p_crush = 0.0
mut p_trem = 0.0
mut p_pan = 0.06
mut p_fx = true

mut font_title = 0
mut font_body = 0
mut font_small = 0
mut font_key = 0

mut mx = 0.0
mut my = 0.0
mut md = false
mut mp = false
mut md_prev = false
mut drag_id = ""

fn midi_freq(int m) f64 { 440.0 * pow(2.0, (m - 69) / 12.0) }
fn midi_for(int semi) int { 48 + OCT * 12 + semi }
fn note_name(int midi, str name) str { name + to_str(midi / 12 - 1) }
fn pct(f64 v) str { to_str(int(clamp01(v) * 100.0 + 0.5)) + "%" }
fn hz(f64 v) str { int(v) >= 1000 ? (to_str(int(v / 1000.0 + 0.5)) + "k") : to_str(int(v)) }
fn ms(f64 v) str { to_str(int(v * 1000.0 + 0.5)) + "ms" }

fn build_sources() {
   SRC_PAD = synth.make_supersaw_source(BASE, PAD_SEC, RATE, 0.44, 7, 0.0025, 0.56, true)
   SRC_HIT = synth.make_soft_pluck_source(BASE, 0.040, RATE, 0.20, 0.12)
   return
}

fn apply_fx() {
   snd.set_master_volume(p_master)
   snd.set_fx_enabled(p_fx)
   snd.set_fx_param("drive", p_drive)
   snd.set_fx_param("comp", p_comp)
   snd.set_fx_param("hpf", p_hpf)
   snd.set_fx_param("lpf", p_lpf)
   snd.set_fx_param("tone", clamp01((p_lpf - 1000.0) / 21000.0))
   snd.set_fx_param("delay", p_delay)
   snd.set_fx_param("delay_time", p_time)
   snd.set_fx_param("feedback", p_fb)
   snd.set_fx_param("reverb", p_reverb)
   snd.set_fx_param("chorus", p_chorus)
   snd.set_fx_param("flanger", p_flanger)
   snd.set_fx_param("crush", p_crush)
   snd.set_fx_param("trem", p_trem)
   snd.set_fx_param("autopan", p_pan)
   return
}

fn set_preset(str name) {
   patch = name
   case name {
      "clean" -> {
         p_drive = 0.025
         p_comp = 0.18
         p_hpf = 30.0
         p_lpf = 17000.0
         p_delay = 0.0
         p_time = 0.16
         p_fb = 0.06
         p_reverb = 0.0
         p_chorus = 0.03
         p_flanger = 0.0
         p_crush = 0.0
         p_trem = 0.0
         p_pan = 0.0
      }
      "wide" -> {
         p_drive = 0.045
         p_comp = 0.24
         p_hpf = 42.0
         p_lpf = 13500.0
         p_delay = 0.025
         p_time = 0.16
         p_fb = 0.10
         p_reverb = 0.035
         p_chorus = 0.18
         p_flanger = 0.0
         p_crush = 0.0
         p_trem = 0.0
         p_pan = 0.12
      }
      "echo" -> {
         p_drive = 0.040
         p_comp = 0.24
         p_hpf = 55.0
         p_lpf = 11500.0
         p_delay = 0.22
         p_time = 0.22
         p_fb = 0.24
         p_reverb = 0.025
         p_chorus = 0.08
         p_flanger = 0.0
         p_crush = 0.0
         p_trem = 0.0
         p_pan = 0.06
      }
      "space" -> {
         p_drive = 0.030
         p_comp = 0.22
         p_hpf = 65.0
         p_lpf = 9800.0
         p_delay = 0.11
         p_time = 0.30
         p_fb = 0.20
         p_reverb = 0.22
         p_chorus = 0.14
         p_flanger = 0.0
         p_crush = 0.0
         p_trem = 0.0
         p_pan = 0.12
      }
      "lofi" -> {
         p_drive = 0.14
         p_comp = 0.34
         p_hpf = 95.0
         p_lpf = 6200.0
         p_delay = 0.035
         p_time = 0.14
         p_fb = 0.12
         p_reverb = 0.035
         p_chorus = 0.06
         p_flanger = 0.0
         p_crush = 0.36
         p_trem = 0.08
         p_pan = 0.0
      }
      "crush" -> {
         p_drive = 0.20
         p_comp = 0.40
         p_hpf = 120.0
         p_lpf = 5200.0
         p_delay = 0.0
         p_time = 0.12
         p_fb = 0.0
         p_reverb = 0.0
         p_chorus = 0.01
         p_flanger = 0.0
         p_crush = 0.48
         p_trem = 0.0
         p_pan = 0.0
      }
      "flange" -> {
         p_drive = 0.055
         p_comp = 0.26
         p_hpf = 48.0
         p_lpf = 11500.0
         p_delay = 0.035
         p_time = 0.11
         p_fb = 0.10
         p_reverb = 0.02
         p_chorus = 0.04
         p_flanger = 0.32
         p_crush = 0.0
         p_trem = 0.0
         p_pan = 0.10
      }
      _ -> nil
   }
   apply_fx()
   return
}

fn play_note(int midi) any {
   if !AUDIO_OK || !SRC_PAD { return 0 }
   def ratio = midi_freq(midi) / BASE
   def h = snd.play(SRC_PAD, ratio, 0.68, true, 0.0)
   if SRC_HIT { def _ = snd.play(SRC_HIT, ratio, 0.10, false, 0.0) }
   h
}

fn stop_note(any h) {
   if h { snd.stop(h) }
   return
}

fn trigger_idx(int i) {
   if i < 0 || i >= MAP.len || held[i] { return }
   def m = MAP[i]
   def midi = midi_for(m[1])
   held[i] = true
   inst[i] = play_note(midi)
   last = note_name(midi, to_str(m[3]))
   pulse = 1.0
   return
}

fn release_idx(int i) {
   if i < 0 || i >= MAP.len || !held[i] { return }
   held[i] = false
   stop_note(inst[i])
   inst[i] = 0
   return
}

fn init_state() {
   mut i = 0
   while i < MAP.len { held = held.append(false) inst = inst.append(0) i += 1 }
   return
}

fn panic_notes() {
   snd.stop_all()
   mut i = 0
   while i < inst.len { inst[i] = 0 held[i] = false i += 1 }
   mouse_note = -1
   return
}

fn active_count() int {
   mut n = 0
   mut i = 0
   while i < held.len { if held[i] { n += 1 } i += 1 }
   n
}

fn update_pointer(any win) {
   def mv = viewer.mouse_view(win, W0, H0)
   mx = float(mv.get(2, 0.0))
   my = float(mv.get(3, 0.0))
   md = window.mouse_down(win, 0)
   mp = md && !md_prev
   return
}

fn finish_pointer() {
   md_prev = md
   if !md { drag_id = "" }
   return
}

fn update_keys(any win) {
   if window.key_pressed(win, key.KEY_Z) && OCT > -1 { panic_notes() OCT -= 1 }
   if window.key_pressed(win, key.KEY_X) && OCT < 4 { panic_notes() OCT += 1 }
   if window.key_pressed(win, key.KEY_SPACE) { panic_notes() }
   if window.key_pressed(win, key.KEY_1) { set_preset("clean") }
   if window.key_pressed(win, key.KEY_2) { set_preset("wide") }
   if window.key_pressed(win, key.KEY_3) { set_preset("echo") }
   if window.key_pressed(win, key.KEY_4) { set_preset("space") }
   if window.key_pressed(win, key.KEY_5) { set_preset("lofi") }
   if window.key_pressed(win, key.KEY_6) { set_preset("crush") }
   if window.key_pressed(win, key.KEY_7) { set_preset("flange") }

   mut i = 0
   while i < MAP.len {
      def m = MAP[i]
      def down = window.key_down(win, m[0])
      if down && !held[i] { trigger_idx(i) } elif !down && held[i] && mouse_note != i { release_idx(i) }
      i += 1
   }
   return
}

fn text(int f, str s, f64 x, f64 y, any color) { gfx.draw_text(f, s, x, y, color) return }
fn hit(f64 x, f64 y, f64 w, f64 h) bool { viewer.hit(x, y, w, h, mx, my) }

fn button(str id, str label, f64 x, f64 y, f64 w, f64 h, bool on=false) bool {
   def hov = hit(x, y, w, h)
   def bg = on ? [0.105, 0.078, 0.190, 1.0] : (hov ? [0.078, 0.092, 0.135, 1.0] : CARD2)
   def fg = on ? WHITE : (hov ? BLUE : MUTED)
   gfx.draw_rect_rounded(x, y, w, h, 11.0, bg, 16)
   if on { gfx.draw_rect_rounded(x + 8.0, y + h - 6.0, w - 16.0, 3.0, 2.0, PURPLE, 8) }
   text(font_small, label, x + 12.0, y + 9.0, fg)
   mp && hov
}

fn slider(str id, str label, f64 x, f64 y, f64 w, f64 value, f64 lo, f64 hi, str readout, any color) f64 {
   def track_y = y + 24.0
   def hov = hit(x, track_y - 10.0, w, 28.0)
   if mp && hov { drag_id = id }
   mut out = value
   if drag_id == id && md { out = lo + clamp((mx - x) / max(1.0, w), 0.0, 1.0) * (hi - lo) }
   def t = clamp((out - lo) / max(0.000001, hi - lo), 0.0, 1.0)
   text(font_small, label, x, y, WHITE)
   text(font_small, readout, x + w - 54.0, y, MUTED)
   gfx.draw_rect_rounded(x, track_y, w, 8.0, 4.0, [0.030, 0.036, 0.055, 1.0], 8)
   gfx.draw_rect_rounded(x, track_y, max(4.0, w * t), 8.0, 4.0, color, 8)
   gfx.draw_circle(x + w * t, track_y + 4.0, hov || drag_id == id ? 7.0 : 5.5, color, 24)
   out
}

fn draw_header(f64 sw) {
   gfx.draw_rect_rounded(28.0, 24.0, sw - 56.0, 126.0, 22.0, CARD, 22)
   text(font_title, "Nytrix Synth", 54.0, 48.0, WHITE)
   text(font_body, "click keys/sliders · 1-7 presets · Z/X octave · SPACE panic", 56.0, 94.0, MUTED)
   text(font_small, "A S D F G H J K L ;  /  W E T Y U O P", 56.0, 124.0, MUTED)
   def status = AUDIO_OK ? ("audio=" + snd.get_backend_name()) : "audio=none"
   def c = AUDIO_OK ? GREEN : RED
   def x = sw - 420.0
   def _ = button("fx_toggle", p_fx ? "FX ON" : "FX OFF", x, 54.0, 86.0, 32.0, p_fx)
   if _ { p_fx = !p_fx apply_fx() }
   text(font_small, status, x + 104.0, 62.0, c)
   text(font_small, "oct " + to_str(OCT) + "  on " + to_str(active_count()) + "  last " + last, x + 104.0, 96.0, WHITE)
   if pulse > 0.0 { gfx.draw_circle(sw - 54.0, 87.0, 16.0 + pulse * 32.0, [0.45, 0.22, 1.0, pulse * 0.28], 64) }
   gfx.draw_circle(sw - 54.0, 87.0, 16.0, active_count() > 0 ? GREEN : LINE, 48)
   return
}

fn draw_panel(f64 sw, f64 sh) {
   def x = 28.0
   def y = 162.0
   def w = sw - 56.0
   def h = 188.0
   gfx.draw_rect_rounded(x, y, w, h, 22.0, CARD, 22)
   text(font_body, "Patch", x + 24.0, y + 28.0, WHITE)

   mut bx = x + 24.0
   mut i = 0
   while i < PRESETS.len {
      def name = PRESETS[i]
      if button("preset_" + name, to_str(i + 1) + " " + name, bx, y + 52.0, 94.0, 34.0, patch == name) { set_preset(name) }
      bx += 102.0
      i += 1
   }

   def sx = x + 24.0
   def sy = y + 102.0
   def col = (w - 72.0) / 4.0
   p_master = slider("master", "Master", sx, sy, col - 20.0, p_master, 0.0, 1.0, pct(p_master), GREEN)
   p_drive = slider("drive", "Drive", sx + col, sy, col - 20.0, p_drive, 0.0, 1.0, pct(p_drive), PURPLE)
   p_comp = slider("comp", "Comp", sx + col * 2.0, sy, col - 20.0, p_comp, 0.0, 1.0, pct(p_comp), BLUE)
   p_crush = slider("crush", "Bitcrush", sx + col * 3.0, sy, col - 20.0, p_crush, 0.0, 1.0, pct(p_crush), RED)

   p_hpf = slider("hpf", "High Pass", sx, sy + 54.0, col - 20.0, p_hpf, 10.0, 600.0, hz(p_hpf), BLUE)
   p_lpf = slider("lpf", "Low Pass", sx + col, sy + 54.0, col - 20.0, p_lpf, 1000.0, 22000.0, hz(p_lpf), BLUE)
   p_chorus = slider("chorus", "Chorus", sx + col * 2.0, sy + 54.0, col - 20.0, p_chorus, 0.0, 1.0, pct(p_chorus), PURPLE)
   p_flanger = slider("flanger", "Flanger", sx + col * 3.0, sy + 54.0, col - 20.0, p_flanger, 0.0, 1.0, pct(p_flanger), PURPLE)
   return
}

fn draw_fx_panel(f64 sw, f64 sh) {
   def x = 28.0
   def y = 368.0
   def w = sw - 56.0
   def h = 146.0
   def col = (w - 72.0) / 4.0
   gfx.draw_rect_rounded(x, y, w, h, 22.0, CARD, 22)
   text(font_body, "Delay / Space / Motion", x + 24.0, y + 28.0, WHITE)
   def sx = x + 24.0
   def sy = y + 58.0
   p_delay = slider("delay", "Delay", sx, sy, col - 20.0, p_delay, 0.0, 1.0, pct(p_delay), GREEN)
   p_time = slider("time", "Time", sx + col, sy, col - 20.0, p_time, 0.025, 1.0, ms(p_time), GREEN)
   p_fb = slider("fb", "Feedback", sx + col * 2.0, sy, col - 20.0, p_fb, 0.0, 0.9, pct(p_fb), GREEN)
   p_reverb = slider("reverb", "Reverb", sx + col * 3.0, sy, col - 20.0, p_reverb, 0.0, 1.0, pct(p_reverb), BLUE)

   p_trem = slider("trem", "Tremolo", sx, sy + 54.0, col - 20.0, p_trem, 0.0, 1.0, pct(p_trem), PURPLE)
   p_pan = slider("pan", "AutoPan", sx + col, sy + 54.0, col - 20.0, p_pan, 0.0, 1.0, pct(p_pan), PURPLE)
   if button("panic", "PANIC", sx + col * 3.0, sy + 44.0, 112.0, 34.0, false) { panic_notes() }
   apply_fx()
   return
}

fn draw_key(f64 x, f64 y, f64 w, f64 h, bool black, bool on, str k, str name) {
   def fill = on ? (black ? PURPLE : BLUE) : (black ? KEY_BLACK : KEY_WHITE)
   def fg = (black || on) ? WHITE : KEY_TEXT
   gfx.draw_rect_rounded(x, y, w, h, black ? 9.0 : 14.0, fill, 18)
   if on { gfx.draw_rect_rounded(x + 8.0, y + 8.0, w - 16.0, 5.0, 3.0, black ? [0.72, 0.56, 1.0, 1.0] : [0.74, 0.96, 1.0, 1.0], 8) }
   elif black { gfx.draw_rect_rounded(x + 7.0, y + 7.0, w - 14.0, 4.0, 3.0, [0.060, 0.070, 0.100, 1.0], 8) }
   text(font_key, k, x + 14.0, y + 16.0, fg)
   text(font_body, name, x + 14.0, y + h - 40.0, fg)
   return
}

fn key_hit_idx(f64 key_x, f64 key_y, f64 key_w, f64 key_h, f64 black_w, f64 black_h) int {
   mut i = 0
   while i < MAP.len {
      def m = MAP[i]
      if m[4] {
         def x = key_x + (float(m[5]) + 1.0) * key_w - black_w * 0.5 - 3.0
         if hit(x, key_y, black_w, black_h) { return i }
      }
      i += 1
   }
   i = 0
   while i < MAP.len {
      def m = MAP[i]
      if !m[4] {
         def x = key_x + float(m[5]) * key_w
         if hit(x, key_y, key_w - 6.0, key_h) { return i }
      }
      i += 1
   }
   -1
}

fn update_mouse_piano(f64 key_x, f64 key_y, f64 key_w, f64 key_h, f64 black_w, f64 black_h) {
   if !md && mouse_note >= 0 { release_idx(mouse_note) mouse_note = -1 }
   if mp {
      def idx = key_hit_idx(key_x, key_y, key_w, key_h, black_w, black_h)
      if idx >= 0 { mouse_note = idx trigger_idx(idx) }
   }
   return
}

fn draw_keyboard(f64 sw, f64 sh) {
   def key_x = 42.0
   def dock_h = min(285.0, max(225.0, sh * 0.31))
   def key_y = sh - dock_h - 22.0
   def key_h = dock_h - 44.0
   def key_w = (sw - key_x * 2.0) / 10.0
   def black_w = key_w * 0.56
   def black_h = key_h * 0.58

   gfx.draw_rect_rounded(key_x - 18.0, key_y - 18.0, sw - key_x * 2.0 + 36.0, key_h + 36.0, 24.0, DOCK, 24)
   text(font_small, "PIANO", key_x - 4.0, key_y - 42.0, MUTED)

   update_mouse_piano(key_x, key_y, key_w, key_h, black_w, black_h)

   mut i = 0
   while i < MAP.len {
      def m = MAP[i]
      if !m[4] {
         def x = key_x + float(m[5]) * key_w
         draw_key(x, key_y, key_w - 6.0, key_h, false, held[i], to_str(m[2]), to_str(m[3]))
      }
      i += 1
   }

   i = 0
   while i < MAP.len {
      def m = MAP[i]
      if m[4] {
         def x = key_x + (float(m[5]) + 1.0) * key_w - black_w * 0.5 - 3.0
         draw_key(x, key_y, black_w, black_h, true, held[i], to_str(m[2]), to_str(m[3]))
      }
      i += 1
   }
   return
}

fn draw_ui(f64 sw, f64 sh) {
   gfx.set_ortho_2d(0.0, sw, 0.0, sh)
   gfx.draw_rect(0.0, 0.0, sw, sh, BG)
   draw_header(sw)
   draw_panel(sw, sh)
   draw_fx_panel(sw, sh)
   draw_keyboard(sw, sh)
   return
}

fn load_fonts() {
   mut fonts = []
   if is_list(viewer.FONT_CANDIDATES) { fonts = viewer.FONT_CANDIDATES }
   font_title = gfx.font_load_first(fonts, 30)
   font_body = gfx.font_load_first(fonts, 18)
   font_small = gfx.font_load_first(fonts, 14)
   font_key = gfx.font_load_first(fonts, 16)
   if font_title { gfx.font_allow_color_fallback(font_title, true) }
   if font_body { gfx.font_allow_color_fallback(font_body, true) viewer.set_font(font_body) }
   if font_small { gfx.font_allow_color_fallback(font_small, true) }
   if font_key { gfx.font_allow_color_fallback(font_key, true) }
   return
}

gfx.apply_backend_env()
gfx.apply_backend_argv()

def win = gfx.init_window(W0, H0, "Nytrix Lush Synth", FLAGS, false, true, 1)
if !win { panic("window init failed") }
window.set_exit_key(win, key.KEY_ESCAPE)
load_fonts()

AUDIO_OK = snd.init(true)
build_sources()
init_state()
set_preset("wide")

print("audio backend=" + snd.get_backend_name() + " ok=" + to_str(AUDIO_OK))

while !gfx.window_should_close(win) {
   if !gfx.begin_frame_clear(BG) { continue }
   update_pointer(win)
   def dt = gfx.get_delta_time()
   if pulse > 0.0 { pulse = max(0.0, pulse - dt * 2.8) }
   update_keys(win)
   def fb = gfx.framebuffer_size_f64(W0, H0)
   draw_ui(max(1.0, float(fb.get(0, W0))), max(1.0, float(fb.get(1, H0))))
   finish_pointer()
   gfx.end_frame()
}

panic_notes()
snd.shutdown()
gfx.close_window()
