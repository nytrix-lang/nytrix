;; Keywords: os clipboard
;; Clipboard access for Nytrix.

module std.os.clipboard (
   set_text, get_text,
   set_clipboard_text, get_clipboard_text
)

use std.core *
use std.os *

fn set_text(text){
   "Copies the provided `text` to the system clipboard. Uses platform-specific tools: xsel/wl-copy/xclip on Linux, pbcopy on macOS, and clip on Windows."
   if(eq(os(), "linux")){
      mut tool = ""
      if(posix_spawn(["which", "xsel"])){ tool = "xsel" }
      elif(posix_spawn(["which", "wl-copy"])){ tool = "wl-copy" }
      elif(posix_spawn(["which", "xclip"])){ tool = "xclip" }

      if(tool == "xsel"){ system(cat("echo -n '", text, "' | xsel -ib")) }
      elif(tool == "wl-copy"){ system(cat("echo -n '", text, "' | wl-copy")) }
      elif(tool == "xclip"){ system(cat("echo -n '", text, "' | xclip -selection clipboard")) }
   } elif(eq(os(), "macos")){
      system(cat("echo -n '", text, "' | pbcopy"))
   } elif(eq(os(), "windows")){
      system(cat("echo | set /p=\"", text, "\" | clip"))
   }
}

fn get_text(){
   "Retrieves the current text content from the system clipboard. Uses platform-specific tools: xsel/wl-paste/xclip on Linux, pbpaste on macOS, and Powershell on Windows."
   mut res = ""
   def tmp = "/build/cache/ny_clipboard_tmp.txt"
   if(eq(os(), "linux")){
      mut tool = ""
      if(posix_spawn(["which", "xsel"])){ tool = "xsel" }
      elif(posix_spawn(["which", "wl-paste"])){ tool = "wl-paste" }
      elif(posix_spawn(["which", "xclip"])){ tool = "xclip" }

      if(tool == "xsel"){ system("xsel -ob > " + tmp) }
      elif(tool == "wl-paste"){ system("wl-paste > " + tmp) }
      elif(tool == "xclip"){ system("xclip -o -selection clipboard > " + tmp) }
   } elif(eq(os(), "macos")){
      system("pbpaste > " + tmp)
   } elif(eq(os(), "windows")){
      system("powershell -command \"Get-Clipboard\" > " + tmp)
   }
   if(sys_file_exists(tmp)){
      def res_err = sys_file_read(tmp)
      if(is_ok(res_err)){
         res = unwrap(res_err)
         ; Optionally strip trailing newline if added by tools like powershell
         def n = str_len(res)
         if(n > 0 && str_slice(res, n - 1, n) == "\n"){
            res = str_slice(res, 0, n - 1)
         }
         if(n > 1 && str_slice(res, n - 2, n) == "\r\n"){
            res = str_slice(res, 0, n - 2)
         }
      }
      sys_file_remove(tmp)
   }
   res
}

fn set_clipboard_text(text){
   "Convenience alias for `set_text`."
   set_text(text)
}

fn get_clipboard_text(){
   "Convenience alias for `get_text`."
   get_text()
}
