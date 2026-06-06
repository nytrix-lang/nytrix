;; Keywords: platform window backend contract os ui input
;; Window-platform backend contract shared by native window implementations.
;; References:
;; - std.os.ui.window.platform
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform.contract(WindowBackendContract, CAP_CREATE, CAP_DESTROY, CAP_POLL, CAP_TITLE, CAP_SIZE, CAP_SWAP, CAP_SURFACE, CAP_CURSOR, CAP_CLIPBOARD, CAP_JOYSTICK, REQUIRED_WINDOW, REQUIRED_RENDER_WINDOW, REQUIRED_INPUT, make, get_backend, get_mask, get_required, has, missing_mask, valid, cap_name)
use std.core

layout WindowBackendContract pack(4){
   i32: backend,
   i32: mask,
   i32: required
}

def CAP_CREATE = 1
def CAP_DESTROY = 2
def CAP_POLL = 4
def CAP_TITLE = 8
def CAP_SIZE = 16
def CAP_SWAP = 32
def CAP_SURFACE = 64
def CAP_CURSOR = 128
def CAP_CLIPBOARD = 256
def CAP_JOYSTICK = 512
def REQUIRED_WINDOW = CAP_CREATE | CAP_DESTROY | CAP_POLL | CAP_TITLE | CAP_SIZE
def REQUIRED_RENDER_WINDOW = REQUIRED_WINDOW | CAP_SWAP | CAP_SURFACE
def REQUIRED_INPUT = CAP_POLL | CAP_CURSOR | CAP_CLIPBOARD | CAP_JOYSTICK

comptime table WindowCapName {
   CAP_CREATE -> "create"
   CAP_DESTROY -> "destroy"
   CAP_POLL -> "poll"
   CAP_TITLE -> "title"
   CAP_SIZE -> "size"
   CAP_SWAP -> "swap"
   CAP_SURFACE -> "surface"
   CAP_CURSOR -> "cursor"
   CAP_CLIPBOARD -> "clipboard"
   CAP_JOYSTICK -> "joystick"
}

fn make(i32 backend, i32 mask, i32 required=REQUIRED_WINDOW) ptr {
   "Builds make."
   def out = malloc(__layout_size("WindowBackendContract"))
   store_layout(out, "WindowBackendContract", backend, mask, required)
   return out
}

fn get_backend(ptr c) i32 { return load_layout(c, "WindowBackendContract", "backend") }

fn get_mask(ptr c) i32 { return load_layout(c, "WindowBackendContract", "mask") }

fn get_required(ptr c) i32 { return load_layout(c, "WindowBackendContract", "required") }

fn has(ptr c, i32 cap) bool {
   "Runs the has operation."
   return(get_mask(c) & cap) == cap
}

fn missing_mask(ptr c) i32 {
   "Runs the missing mask operation."
   def mask = get_mask(c)
   def required = get_required(c)
   return required - (required & mask)
}

fn valid(ptr c) bool {
   "Runs the valid operation."
   return missing_mask(c) == 0
}

fn cap_name(i32 cap) str {
   "Runs the cap name operation."
   comptime match WindowCapName(cap, "unknown")
}
