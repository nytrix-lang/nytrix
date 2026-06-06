;; Keywords: clipboard copy paste os
;; Clipboard access utilities with backend/tool fallbacks.
;; References:
;; - std.os
module std.os.clipboard(set_text, get_text, set_clipboard_text, get_clipboard_text)
use std.core
use std.os
use std.os.path as ospath
use std.os.io as pio
use std.os.sys (sys_close_quiet)
use std.os.subprocess as subprocess
use std.core.str as str

fn _detect_tool() str {
   def wd = env("WAYLAND_DISPLAY")
   if(is_str(wd) && wd.len > 0){ if(file_exists("/usr/bin/wl-copy") || file_exists("/usr/local/bin/wl-copy")){ return "wl" } }
   def xd = env("DISPLAY")
   if(is_str(xd) && xd.len > 0){
      if(file_exists("/usr/bin/xclip") || file_exists("/usr/local/bin/xclip")){ return "xclip" }
      if(file_exists("/usr/bin/xsel")  || file_exists("/usr/local/bin/xsel") ){ return "xsel" }
   }
   "none"
}

fn _env_prefix() str {
   mut p = ""
   def wd = env("WAYLAND_DISPLAY")
   if(is_str(wd) && wd.len > 0){ p = p + "WAYLAND_DISPLAY=" + wd + " " }
   def xd = env("DISPLAY")
   if(is_str(xd) && xd.len > 0){ p = p + "DISPLAY=" + xd + " " }
   def xa = env("XAUTHORITY")
   if(is_str(xa) && xa.len > 0){ p = p + "XAUTHORITY=" + xa + " " }
   p
}

fn _tmp_base_dir() str {
   #windows {
      mut d = env("TEMP")
      if(!is_str(d) || str.strip(d).len == 0){ d = env("TMP") }
      if(!is_str(d) || str.strip(d).len == 0){ return "." }
      return d
   } #else {
      return ospath.temp_dir()
   } #endif
}

fn _tmp_file(str tag) str {
   def base = _tmp_base_dir()
   #windows {
      return base + "\\ny_cb_" + tag + "_" + to_str(pid()) + "_" + to_str(ticks()) + ".txt"
   } #else {
      return base + "/ny_cb_" + tag + "_" + to_str(pid()) + "_" + to_str(ticks()) + ".txt"
   } #endif
}

fn _timeout_prefix() str {
   #linux {
      if(file_exists("/usr/bin/timeout")){ return "timeout 1 " }
      if(file_exists("/bin/timeout")){ return "timeout 1 " }
   } #endif
   ""
}

fn _feed_clipboard_writer(str path, list args, any text) bool {
   def p = pio.spawn(path, args)
   if(!p){ return false }
   match pio.send(p, text){
      ok(ignored) -> { ignored }
      err(ignorederr) -> { ignorederr }
   }
   match pio.shutdown_send(p){
      ok(ignored) -> { ignored }
      err(ignorederr) -> { ignorederr }
   }
   sys_close_quiet(p.get("out", -1))
   true
}

fn set_text(any text) bool {
   "Copies text to the system clipboard."
   def tmp = _tmp_file("w")
   def qtmp = "\"" + tmp + "\""
   match file_write(tmp, text){
      ok(ignoredok) -> { ignoredok }
      err(ignorederr) -> { ignorederr }
   }
   defer {
      match file_remove(tmp){
         ok(ignoredok) -> { ignoredok }
         err(ignorederr) -> { ignorederr }
      }
   }
   #linux {
      def tool = _detect_tool()
      if(tool == "wl"    ){ _feed_clipboard_writer("wl-copy", ["wl-copy"], text) }
      elif(tool == "xclip"){
         _feed_clipboard_writer("xclip", ["xclip", "-selection", "clipboard", "-i"], text)
         _feed_clipboard_writer("xclip", ["xclip", "-selection", "primary", "-i"], text)
      }
      elif(tool == "xsel" ){ _feed_clipboard_writer("xsel", ["xsel", "--clipboard", "--input"], text) }
   } #elif macos {
      subprocess.shell("pbcopy < " + qtmp, false, false)
   } #elif windows {
      subprocess.shell("clip < " + qtmp, false, false)
   } #endif
   true
}

fn get_text() str {
   "Retrieves text from the system clipboard."
   mut res = ""
   def tmp = _tmp_file("r")
   def qtmp = "\"" + tmp + "\""
   defer {
      if(file_exists(tmp)){
         match file_remove(tmp){
            ok(ignoredok) -> { ignoredok }
            err(ignorederr) -> { ignorederr }
         }
      }
   }
   #linux {
      def tool = _detect_tool()
      def pfx = _env_prefix()
      def timeout = _timeout_prefix()
      if(tool == "wl"    ){ subprocess.shell(pfx + timeout + "wl-paste > " + qtmp + " 2>/dev/null", false, false) }
      elif(tool == "xclip"){ subprocess.shell(pfx + timeout + "xclip -o -selection clipboard > " + qtmp + " 2>/dev/null", false, false) }
      elif(tool == "xsel" ){ subprocess.shell(pfx + timeout + "xsel --clipboard --output > " + qtmp + " 2>/dev/null", false, false) }
   } #elif macos {
      subprocess.shell("pbpaste > " + qtmp, false, false)
   } #elif windows {
      subprocess.shell("powershell -command \"Get-Clipboard\" > " + qtmp, false, false)
   } #endif
   if(file_exists(tmp)){
      def rd = file_read(tmp)
      if(is_ok(rd)){
         res = unwrap(rd)
         def n = res.len
         if(n >= 2 && load8(res, n - 2) == 13 && load8(res, n - 1) == 10){ res = str.str_slice(res, 0, n - 2) }
         elif(n >= 1 && load8(res, n - 1) == 10){ res = str.str_slice(res, 0, n - 1) }
      }
   }
   res
}

fn set_clipboard_text(any text) bool { "Updates the system clipboard with the provided text string." set_text(text) }

fn get_clipboard_text() str { "Retrieves the current text content from the system clipboard." get_text() }
