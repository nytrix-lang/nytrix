;; Keywords: ui input keyboard mouse
;; Input helpers for Nytrix.
;;
;; Key code scheme: GLFW-compatible values (same as glfw3.h GLFW_KEY_*).
;;   Printable keys    = ASCII value (A=65, Z=90, 0=48, SPACE=32 …)
;;   Special / fn keys = 256+ (ESC=256, ENTER=257, UP=265, F1=290 …)
;; This matches what backend/mod.ny stores in key_states after normalize_key().
;;
;; Sources consulted: glfw3.h, RGFW.h, backend/mod.ny §7 key maps.

module std.ui.window.input (
   ;; Special keys
   KEY_NULL, KEY_UNKNOWN,
   KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE,
   KEY_INSERT, KEY_DELETE,
   KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
   KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END,
   KEY_CAPS_LOCK, KEY_SCROLL_LOCK, KEY_NUM_LOCK, KEY_PRINT_SCREEN, KEY_PAUSE,
   KEY_SPACE,
   KEY_GRAVE, KEY_MINUS, KEY_EQUAL, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET,
   KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_COMMA, KEY_PERIOD, KEY_SLASH,

   ;; Letter keys A–Z
   KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
   KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
   KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,

   ;; Digit keys 0–9
   KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,

   ;; Function keys F1–F25
   KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
   KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
   KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
   KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_F25,

   ;; Numpad keys
   KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4,
   KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
   KEY_KP_DECIMAL, KEY_KP_DIVIDE, KEY_KP_MULTIPLY,
   KEY_KP_SUBTRACT, KEY_KP_ADD, KEY_KP_ENTER, KEY_KP_EQUAL,

   ;; Modifier key codes (for querying key_states directly)
   KEY_LEFT_SHIFT, KEY_LEFT_CONTROL, KEY_LEFT_ALT, KEY_LEFT_SUPER,
   KEY_RIGHT_SHIFT, KEY_RIGHT_CONTROL, KEY_RIGHT_ALT, KEY_RIGHT_SUPER,
   KEY_MENU,

   ;; Legacy aliases (kept for backward compatibility)
   KEY_SHIFT, KEY_CTRL, KEY_ESC,

   ;; Modifier flags
   MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_SUPER, MOD_META,
   MOD_CAPS_LOCK, MOD_NUM_LOCK,

   ;; Mouse buttons
   MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE,
   MOUSE_BUTTON_4, MOUSE_BUTTON_5,

   ;; Key normalization helpers
   normalize_key, parse_notation, mod_bit_for_key, mods_from_key_states,

   ;; High-level input (active window)
   key_down, key_pressed, key_chord, mod_down,
   mouse_pos, mouse_button_down, mouse_button_pressed
)

use std.core *
use std.str *
use std.ui.window.key as uikey
use std.ui.window.consts *
use std.ui.window as window

;; ── Key constants (GLFW_KEY_* values) ─────────────────────────────────────────

def KEY_NULL    = 0
def KEY_UNKNOWN = -1

;; Printable / ASCII (same value as ASCII code point)
def KEY_SPACE         = 32
def KEY_APOSTROPHE    = 39
def KEY_COMMA         = 44
def KEY_MINUS         = 45
def KEY_PERIOD        = 46
def KEY_SLASH         = 47
def KEY_SEMICOLON     = 59
def KEY_EQUAL         = 61
def KEY_LEFT_BRACKET  = 91
def KEY_BACKSLASH     = 92
def KEY_RIGHT_BRACKET = 93
def KEY_GRAVE         = 96
def KEY_GRAVE_ACCENT  = 96

def KEY_0 = 48  def KEY_1 = 49  def KEY_2 = 50  def KEY_3 = 51  def KEY_4 = 52
def KEY_5 = 53  def KEY_6 = 54  def KEY_7 = 55  def KEY_8 = 56  def KEY_9 = 57

def KEY_A = 65  def KEY_B = 66  def KEY_C = 67  def KEY_D = 68  def KEY_E = 69
def KEY_F = 70  def KEY_G = 71  def KEY_H = 72  def KEY_I = 73  def KEY_J = 74
def KEY_K = 75  def KEY_L = 76  def KEY_M = 77  def KEY_N = 78  def KEY_O = 79
def KEY_P = 80  def KEY_Q = 81  def KEY_R = 82  def KEY_S = 83  def KEY_T = 84
def KEY_U = 85  def KEY_V = 86  def KEY_W = 87  def KEY_X = 88  def KEY_Y = 89
def KEY_Z = 90

;; Special keys (GLFW_KEY_* 256+)
def KEY_ESCAPE      = 256
def KEY_ENTER       = 257
def KEY_TAB         = 258
def KEY_BACKSPACE   = 259
def KEY_INSERT      = 260
def KEY_DELETE      = 261
def KEY_RIGHT       = 262
def KEY_LEFT        = 263
def KEY_DOWN        = 264
def KEY_UP          = 265
def KEY_PAGE_UP     = 266
def KEY_PAGE_DOWN   = 267
def KEY_HOME        = 268
def KEY_END         = 269
def KEY_CAPS_LOCK   = 280
def KEY_SCROLL_LOCK = 281
def KEY_NUM_LOCK    = 282
def KEY_PRINT_SCREEN= 283
def KEY_PAUSE       = 284

def KEY_F1  = 290   def KEY_F2  = 291   def KEY_F3  = 292   def KEY_F4  = 293
def KEY_F5  = 294   def KEY_F6  = 295   def KEY_F7  = 296   def KEY_F8  = 297
def KEY_F9  = 298   def KEY_F10 = 299   def KEY_F11 = 300   def KEY_F12 = 301
def KEY_F13 = 302   def KEY_F14 = 303   def KEY_F15 = 304   def KEY_F16 = 305
def KEY_F17 = 306   def KEY_F18 = 307   def KEY_F19 = 308   def KEY_F20 = 309
def KEY_F21 = 310   def KEY_F22 = 311   def KEY_F23 = 312   def KEY_F24 = 313
def KEY_F25 = 314

def KEY_KP_0        = 320   def KEY_KP_1 = 321   def KEY_KP_2 = 322
def KEY_KP_3        = 323   def KEY_KP_4 = 324   def KEY_KP_5 = 325
def KEY_KP_6        = 326   def KEY_KP_7 = 327   def KEY_KP_8 = 328
def KEY_KP_9        = 329   def KEY_KP_DECIMAL  = 330
def KEY_KP_DIVIDE   = 331   def KEY_KP_MULTIPLY = 332
def KEY_KP_SUBTRACT = 333   def KEY_KP_ADD      = 334
def KEY_KP_ENTER    = 335   def KEY_KP_EQUAL    = 336

def KEY_LEFT_SHIFT    = 340   def KEY_LEFT_CONTROL = 341
def KEY_LEFT_ALT      = 342   def KEY_LEFT_SUPER   = 343
def KEY_RIGHT_SHIFT   = 344   def KEY_RIGHT_CONTROL= 345
def KEY_RIGHT_ALT     = 346   def KEY_RIGHT_SUPER  = 347
def KEY_MENU          = 348

;; Legacy aliases (backward compat)
def KEY_ESC   = 256
def KEY_SHIFT = 340
def KEY_CTRL  = 341

;; ── Mouse buttons ─────────────────────────────────────────────────────────────
def MOUSE_LEFT     = 0
def MOUSE_RIGHT    = 1
def MOUSE_MIDDLE   = 2
def MOUSE_BUTTON_4 = 3
def MOUSE_BUTTON_5 = 4

;; ── Helpers ───────────────────────────────────────────────────────────────────
fn normalize_key(key){  "Normalize any native key code to a Nytrix/GLFW code."  uikey.normalize_key(key) }
fn mod_bit_for_key(key){ uikey.mod_bit_for_key(key) }
fn mods_from_key_states(ks){ uikey.mods_from_key_states(ks) }
fn parse_notation(notation){ uikey.parse_notation(notation) }

;; ── High-level (proxies to the active window) ─────────────────────────────────
fn _aw(){ window.last() }

fn key_down(key){
   "True if key is held in the active window. Accepts GLFW key codes or KEY_* constants."
   def w = _aw()  if(!w){ return false }  window.key_down(w, key)
}
fn key_pressed(key){
   "True if key was pressed this frame."
   def w = _aw()  if(!w){ return false }  window.key_pressed(w, key)
}
fn key_chord(notation){
   "True if chord notation string (e.g. 'C-x') was triggered this frame."
   def w = _aw()  if(!w){ return false }
   def seq = parse_notation(notation)  if(len(seq) != 1){ return false }
   def pair = get(seq, 0)
   if(!window.key_pressed(w, get(pair, 0))){ return false }
   def mod = get(pair, 1)
   if(mod != 0 && (window.get_modifiers(w) & mod) != mod){ return false }
   true
}
fn mod_down(mod){  def w = _aw()  if(!w){ return false }  window.mod_down(w, mod) }
fn mouse_pos(){    def w = _aw()  if(!w){ return [0, 0] }  window.mouse_pos(w) }
fn mouse_button_down(btn){    def w = _aw()  if(!w){ return false }  window.mouse_down(w, btn) }
fn mouse_button_pressed(btn){ def w = _aw()  if(!w){ return false }  window.mouse_pressed(w, btn) }
