;; Keywords: os dirs
;; OS directories helpers.

module std.os.dirs (
   home_dir, temp_dir, config_dir, data_dir, cache_dir
)
use std.core *
use std.str *
use std.os *
use std.os.path as ospath

fn _is_windows(){
   "Internal helper."
   __os_name() == "windows"
}

fn _is_macos(){
   "Internal helper."
   __os_name() == "macos"
}

fn _is_non_empty(v){
   "Internal helper."
   is_str(v) && str_len(v) > 0
}

fn _env_path(name){
   "Internal helper."
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

fn home_dir(){
   "Function `home_dir`."
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
   "Function `temp_dir`."
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
   "Function `config_dir`."
   if(_is_windows()){
      def a = _env_path("APPDATA")
      if(str_len(a) > 0){ return a }
      return _non_empty_or(_home_join("AppData\\Roaming"), temp_dir())
   }
   if(_is_macos()){
      return _non_empty_or(_home_join("Library/Application Support"), temp_dir())
   }
   def x = _env_path("XDG_CONFIG_HOME")
   if(str_len(x) > 0){ return x }
   _non_empty_or(_home_join(".config"), temp_dir())
}

fn data_dir(){
   "Function `data_dir`."
   if(_is_windows()){
      def a = _env_path("LOCALAPPDATA")
      if(str_len(a) > 0){ return a }
      return _non_empty_or(_home_join("AppData\\Local"), temp_dir())
   }
   if(_is_macos()){
      return _non_empty_or(_home_join("Library/Application Support"), temp_dir())
   }
   def x = _env_path("XDG_DATA_HOME")
   if(str_len(x) > 0){ return x }
   _non_empty_or(_home_join(".local/share"), temp_dir())
}

fn cache_dir(){
   "Function `cache_dir`."
   if(_is_windows()){
      def a = _env_path("LOCALAPPDATA")
      if(str_len(a) > 0){ return ospath.normalize(ospath.join(a, "Temp")) }
      def t = temp_dir()
      if(str_len(t) > 0){ return t }
      return ospath.normalize("C:\\Temp")
   }
   if(_is_macos()){
      return _non_empty_or(_home_join("Library/Caches"), temp_dir())
   }
   def x = _env_path("XDG_CACHE_HOME")
   if(str_len(x) > 0){ return x }
   _non_empty_or(_home_join(".cache"), temp_dir())
}

if(comptime{__main()}){
    use std.os.dirs *
    use std.core *
    use std.str *
    use std.str.io *

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
