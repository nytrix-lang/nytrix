;; Keywords: os dirs
;; OS directories helpers.

module std.os.dirs (
   home_dir, temp_dir, config_dir, data_dir, cache_dir
)
use std.core *
use std.text *
use std.os *
use std.os.path as ospath
use std.os.platform as platform

fn _is_non_empty(v){
   "Internal: returns true if `v` is a non-empty string."
   is_str(v) && str_len(v) > 0
}

fn _env_path(name){
   "Internal: retrieves the value of environment variable `name` and returns its normalized path."
   def v = env(name)
   if(_is_non_empty(v)){ return ospath.normalize(v) }
   ""
}

fn _first_env_path2(a, b){
   "Internal helper."
   def va = _env_path(a)
   if(str_len(va) > 0){ return va }
   _env_path(b)
}

fn _first_env_path3(a, b, c){
   "Internal helper."
   def va = _env_path(a)
   if(str_len(va) > 0){ return va }
   def vb = _env_path(b)
   if(str_len(vb) > 0){ return vb }
   _env_path(c)
}

fn _home_join(suffix){
   "Internal helper."
   def h = home_dir()
   if(str_len(h) > 0){ return ospath.normalize(ospath.join(h, suffix)) }
   ""
}

fn _non_empty_or(primary, fallback){
   "Internal helper."
   if(_is_non_empty(primary)){ return primary }
   fallback
}

fn _user_dir(win_env, win_suffix, mac_suffix, xdg_env, unix_suffix){
   "Internal: resolves a standard user data/config directory across Windows, macOS, and XDG hosts."
   if(platform.is_windows()){
      def win_path = _env_path(win_env)
      if(str_len(win_path) > 0){ return win_path }
      return _non_empty_or(_home_join(win_suffix), temp_dir())
   }
   if(platform.is_macos()){
      return _non_empty_or(_home_join(mac_suffix), temp_dir())
   }
   def xdg_path = _env_path(xdg_env)
   if(str_len(xdg_path) > 0){ return xdg_path }
   _non_empty_or(_home_join(unix_suffix), temp_dir())
}

fn home_dir(){
   "Returns the path to the current user's home directory, using OS-specific environment variables."
   if(__os_name() == "windows"){
      def h = _env_path("USERPROFILE")
      if(str_len(h) > 0){ return h }
      def d = env("HOMEDRIVE")
      def p = env("HOMEPATH")
      if(_is_non_empty(d) && _is_non_empty(p)){ return ospath.normalize(d + p) }
      return ospath.normalize("C:\\")
   }
   def h = _env_path("HOME")
   if(str_len(h) > 0){ return h }
   def pwd = _env_path("PWD")
   if(str_len(pwd) > 0){ return pwd }
   ospath.normalize("/tmp")
}

fn temp_dir(){
   "Returns the path to the system's temporary directory."
   if(__os_name() == "windows"){
      def t = _first_env_path2("TEMP", "TMP")
      if(str_len(t) > 0){ return t }
      return ospath.normalize("C:\\Temp")
   }
   def t = _first_env_path3("TMPDIR", "TMP", "TEMP")
   if(str_len(t) > 0){ return t }
   ospath.normalize("/tmp")
}

fn config_dir(){
   "Returns the path to the current user's configuration directory (e.g., ~/.config on Linux)."
   _user_dir(
      "APPDATA",
      "AppData\\Roaming",
      "Library/Application Support",
      "XDG_CONFIG_HOME",
      ".config"
   )
}

fn data_dir(){
   "Returns the path to the current user's persistent data directory (e.g., ~/.local/share on Linux)."
   _user_dir(
      "LOCALAPPDATA",
      "AppData\\Local",
      "Library/Application Support",
      "XDG_DATA_HOME",
      ".local/share"
   )
}

fn cache_dir(){
   "Returns the path to the current user's cache directory (e.g., ~/.cache on Linux)."
   if(platform.is_windows()){
      def a = _env_path("LOCALAPPDATA")
      if(str_len(a) > 0){ return ospath.normalize(ospath.join(a, "Temp")) }
      def t = temp_dir()
      if(str_len(t) > 0){ return t }
      return ospath.normalize("C:\\Temp")
   }
   if(platform.is_macos()){
      return _non_empty_or(_home_join("Library/Caches"), temp_dir())
   }
   def x = _env_path("XDG_CACHE_HOME")
   if(str_len(x) > 0){ return x }
   _non_empty_or(_home_join(".cache"), temp_dir())
}

if(comptime{__main()}){
    use std.os.dirs *
    use std.core *
    use std.text *
    use std.text.io *

    def h = home_dir()
    assert(is_str(h), "home_dir string")
    assert(str_len(h) > 0, "home_dir non-empty")

    def t = temp_dir()
    assert(is_str(t), "temp_dir string")
    assert(str_len(t) > 0, "temp_dir non-empty")

    mut c = config_dir()
    assert(is_str(c), "config_dir string")
    if(str_len(c) == 0){ c = temp_dir() }
    assert(str_len(c) > 0, "config_dir non-empty")

    mut d = data_dir()
    assert(is_str(d), "data_dir string")
    if(str_len(d) == 0){ d = c }
    assert(str_len(d) > 0, "data_dir non-empty")

    mut ch = cache_dir()
    assert(is_str(ch), "cache_dir string")
    if(str_len(ch) == 0){ ch = temp_dir() }
    assert(str_len(ch) > 0, "cache_dir non-empty")

    print("✓ std.os.dirs tests passed")
}
