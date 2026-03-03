;; Keywords: os clipboard
;; Clipboard access for Nytrix.

module std.os.clipboard (
   set_text, get_text,
   set_clipboard_text, get_clipboard_text
)

use std.core *
use std.os *
use std.ui.window as uiw

;; Tool detection: not cached — called at use time so env vars are available.
;; Priority: wl (Wayland) → xclip (X11) → xsel (X11 fallback)

fn _detect_tool(){
   "Returns best available clipboard tool name, preferring xclip for reliability."
   ;; Try xclip first — it works on both X11 and XWayland, no daemon needed
   if(file_exists("/usr/bin/xclip") || file_exists("/usr/local/bin/xclip")){ return "xclip" }
   ;; wl-copy for pure Wayland
   def wd = env("WAYLAND_DISPLAY")
   if(is_str(wd) && str_len(wd) > 0){
      if(file_exists("/usr/bin/wl-copy") || file_exists("/usr/local/bin/wl-copy")){ return "wl" }
   }
   if(file_exists("/usr/bin/xsel")  || file_exists("/usr/local/bin/xsel") ){ return "xsel" }
   "none"
}

fn _env_prefix(){
   "Build env var prefix string so system() child has display vars."
   mut p = ""
   def wd = env("WAYLAND_DISPLAY")
   if(is_str(wd) && str_len(wd) > 0){ p = p + "WAYLAND_DISPLAY=" + wd + " " }
   def xd = env("DISPLAY")
   if(is_str(xd) && str_len(xd) > 0){ p = p + "DISPLAY=" + xd + " " }
   def xa = env("XAUTHORITY")
   if(is_str(xa) && str_len(xa) > 0){ p = p + "XAUTHORITY=" + xa + " " }
   p
}

fn set_text(text){
   "Copies text to the system clipboard."
   ;; FAST PATH: Try active GLFW window first
   def win = uiw.last()
   if(win){
      uiw.set_clipboard(win, text)
      ;; We still also write to terminal/system clipboards for maximum compatibility
   }

   def tmp = "/build/cache/ny_cb_w.txt"
   mut _ = file_write(tmp, text)
   def tool = _detect_tool()
   def pfx = _env_prefix()
   if(eq(os(), "linux")){
      if(tool == "wl"    ){ system(pfx + "wl-copy < " + tmp + " 2>/dev/null") }
      elif(tool == "xclip"){
         system(pfx + "xclip -selection clipboard -i " + tmp + " 2>/dev/null")
         system(pfx + "xclip -selection primary -i " + tmp + " 2>/dev/null")
      }
      elif(tool == "xsel" ){ system(pfx + "xsel --clipboard --input < " + tmp + " 2>/dev/null") }
   } elif(eq(os(), "macos")){
      system("pbcopy < " + tmp)
   } elif(eq(os(), "windows")){
      system("clip < " + tmp)
   }
   _ = file_remove(tmp)
}

fn get_text(){
   "Retrieves text from the system clipboard."
   ;; FAST PATH: Try active GLFW window first
   def win = uiw.last()
   if(win){
      def res = uiw.get_clipboard(win)
      if(str_len(res) > 0){ return res }
   }

   mut res = ""
   def tmp = "/build/cache/ny_cb_r.txt"
   def tool = _detect_tool()
   def pfx = _env_prefix()
   if(eq(os(), "linux")){
      if(tool == "wl"    ){ system(pfx + "wl-paste > " + tmp + " 2>/dev/null") }
      elif(tool == "xclip"){ system(pfx + "xclip -o -selection clipboard > " + tmp + " 2>/dev/null") }
      elif(tool == "xsel" ){ system(pfx + "xsel --clipboard --output > " + tmp + " 2>/dev/null") }
   } elif(eq(os(), "macos")){
      system("pbpaste > " + tmp)
   } elif(eq(os(), "windows")){
      system("powershell -command \"Get-Clipboard\" > " + tmp)
   }
   if(file_exists(tmp)){
      def rd = file_read(tmp)
      if(is_ok(rd)){
         res = unwrap(rd)
         def n = str_len(res)
         if(n >= 2 && load8(res, n - 2) == 13 && load8(res, n - 1) == 10){ res = str_slice(res, 0, n - 2) }
         elif(n >= 1 && load8(res, n - 1) == 10){ res = str_slice(res, 0, n - 1) }
      }
      mut _ = file_remove(tmp)
   }
   res
}

fn set_clipboard_text(text){ set_text(text) }
fn get_clipboard_text(){ get_text() }
