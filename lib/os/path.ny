;; Keywords: path filepath pathname os
;; Path Manipulation for Nytrix
;; References:
;; - std.os
module std.os.path(sep, has_sep, is_abs, join, normalize, basename, dirname, extname, splitext, resolve_repo_asset, home_dir, temp_dir, config_dir, data_dir, cache_dir)
use std.core
use std.core.str (_substr, Builder, builder_append, builder_free, builder_to_str, cstr_to_str, split, str_replace)
use std.core.common as common
use std.os.prim

fn sep() str {
   "Returns the platform-specific path separator('\\\\' on Windows, '/' otherwise)."
   #windows {
      "\\"
   } #else {
      "/"
   } #endif
}

fn _is_sep(int c) bool { c == 47 || c == 92 }

fn _is_alpha(int c) bool { (c >= 65 && c <= 90) || (c >= 97 && c <= 122) }

fn has_sep(any p) bool {
   "Returns true if path contains '/' or '\\\\'."
   if(!is_str(p)){ return false }
   def n = p.len
   mut i = 0
   while(i < n){
      if(_is_sep(__load8_idx(p, i))){ return true }
      i += 1
   }
   false
}

fn is_abs(any p) bool {
   "Returns true if path `p` is an absolute path for the current platform."
   if(!is_str(p) || p.len == 0){ return false }
   def n = p.len
   def c0 = __load8_idx(p, 0)
   #windows {
      if(n < 2){ return false }
      def c1 = __load8_idx(p, 1)
      c1 == 58 || ((c0 == 92 || c0 == 47) && c1 == c0)
   } #else {
      c0 == 47
   } #endif
}

fn _path_join(any a, any b) str {
   if(!is_str(a) || a.len == 0){
      if(is_str(b)){ return b }
      return ""
   }
   if(!is_str(b) || b.len == 0){ return a }
   if(is_abs(b)){ return b }
   def s = sep()
   def al = a.len
   def bl = b.len
   if(al > 0 && bl > 0){
      def ac, bc = __load8_idx(a, al - 1), __load8_idx(b, 0)
      if(_is_sep(ac) || _is_sep(bc)){ return a + b }
   }
   return a + s + b
}

fn join(any a, any b) str {
   "Joins two path segments, inserting a platform separator if necessary."
   _path_join(a, b)
}

fn _is_drive_prefix(any p) bool {
   if(!is_str(p)){ return false }
   def n = p.len
   if(n < 2){ return false }
   if(!_is_alpha(load8(p, 0))){ return false }
   if(load8(p, 1) != 58){ return false }
   true
}

fn _resolve_under_base(any base, str rel, int max_up=8) str {
   if(!is_str(base) || base.len == 0){ return "" }
   mut cur = _normalize_path(base)
   mut hops = 0
   while(cur.len > 0 && hops <= max_up){
      def cand = _path_join(cur, rel)
      if(_path_exists_local(cand)){ return cand }
      def parent = dirname(cur)
      if(parent == cur || parent == "." || parent.len == 0){ break }
      cur = parent
      hops += 1
   }
   ""
}

fn _cwd_local() str {
   def buf = malloc(4096)
   if(!buf){ return "" }
   defer { free(buf) }
   def clen = __getcwd(buf, 4096)
   if(clen <= 0){ return "" }
   cstr_to_str(buf)
}

fn _path_exists_local(any path) bool {
   if(!is_str(path) || path.len == 0){ return false }
   __access(path, 0) == 0
}

fn _is_non_empty(any v) bool { is_str(v) && v.len > 0 }

fn _env_path(str name) str {
   def v = common.env_trim(name)
   if(_is_non_empty(v)){ return _normalize_path(v) }
   ""
}

fn _first_env_path2(str a, str b) str {
   def va = _env_path(a)
   if(va.len > 0){ return va }
   _env_path(b)
}

fn _first_env_path3(str a, str b, str c) str {
   def va = _env_path(a)
   if(va.len > 0){ return va }
   def vb = _env_path(b)
   if(vb.len > 0){ return vb }
   _env_path(c)
}

fn _home_join(str suffix) str {
   def h = home_dir()
   if(h.len > 0){ return _normalize_path(_path_join(h, suffix)) }
   ""
}

fn _non_empty_or(any primary, str fallback) str {
   if(_is_non_empty(primary)){ return primary }
   fallback
}

fn _dir_writable(str dir) bool {
   if(!_is_non_empty(dir)){ return false }
   def probe = _path_join(dir, ".nytrix_cache_probe_" + to_str(__getpid()))
   def fd = __open(probe, 577, 420)
   if(fd < 0){ return false }
   def written = __write_off(fd, "ok", 2, 0)
   __close(fd)
   __unlink(probe)
   written >= 0
}

fn _user_dir(str win_env, str win_suffix, str mac_suffix, str xdg_env, str unix_suffix) str {
   #windows {
      def win_path = _env_path(win_env)
      if(win_path.len > 0){ return win_path }
      return _non_empty_or(_home_join(win_suffix), temp_dir())
   } #elif macos {
      return _non_empty_or(_home_join(mac_suffix), temp_dir())
   } #else {
      def xdg_path = _env_path(xdg_env)
      if(xdg_path.len > 0){ return xdg_path }
      return _non_empty_or(_home_join(unix_suffix), temp_dir())
   } #endif
}

fn home_dir() str {
   "Returns the path to the current user's home directory, using OS-specific environment variables."
   #windows {
      def h = _env_path("USERPROFILE")
      if(h.len > 0){ return h }
      def d, p = common.env_trim("HOMEDRIVE"), common.env_trim("HOMEPATH")
      if(_is_non_empty(d) && _is_non_empty(p)){ return _normalize_path(d + p) }
      return _normalize_path("C:\\")
   } #else {
      def h = _env_path("HOME")
      if(h.len > 0){ return h }
      def pwd = _env_path("PWD")
      if(pwd.len > 0){ return pwd }
      return _normalize_path(".")
   } #endif
}

fn temp_dir() str {
   "Returns the path to the system's temporary directory."
   #windows {
      def t = _first_env_path2("TEMP", "TMP")
      if(t.len > 0){ return t }
      return _normalize_path("C:\\Temp")
   } #else {
      def t = _first_env_path3("TMPDIR", "TMP", "TEMP")
      if(t.len > 0){ return t }
      return _normalize_path("/tmp")
   } #endif
}

fn config_dir() str {
   "Returns the path to the current user's configuration directory(e.g., ~/.config on Linux)."
   _user_dir(
      "APPDATA",
      "AppData\\Roaming",
      "Library/Application Support",
      "XDG_CONFIG_HOME",
      ".config"
   )
}

fn data_dir() str {
   "Returns the path to the current user's persistent data directory(e.g., ~/.local/share on Linux)."
   _user_dir(
      "LOCALAPPDATA",
      "AppData\\Local",
      "Library/Application Support",
      "XDG_DATA_HOME",
      ".local/share"
   )
}

fn cache_dir() str {
   "Returns the path to the current user's cache directory(e.g., ~/.cache on Linux)."
   def override = _env_path("NYTRIX_CACHE_DIR")
   if(override.len > 0 && _dir_writable(override)){ return override }
   def repo_root = _repo_root_env()
   def cwd = _cwd_local()
   if(repo_root.len > 0){
      def repo_cache = _normalize_path(_path_join(repo_root, "build" + sep() + "cache"))
      if(_dir_writable(repo_cache)){ return repo_cache }
   }
   if(cwd.len > 0){
      def cwd_cache = _normalize_path(_path_join(cwd, "build" + sep() + "cache"))
      if(_dir_writable(cwd_cache)){ return cwd_cache }
   }
   #windows {
      def a = _env_path("LOCALAPPDATA")
      if(a.len > 0){
         def win_cache = _normalize_path(_path_join(a, "Temp"))
         if(_dir_writable(win_cache)){ return win_cache }
      }
      def t = temp_dir()
      if(t.len > 0){ return t }
      return _normalize_path("C:\\Temp")
   } #elif macos {
      def mac_cache = _home_join("Library/Caches")
      if(_dir_writable(mac_cache)){ return mac_cache }
      return temp_dir()
   } #else {
      def x = _env_path("XDG_CACHE_HOME")
      if(x.len > 0 && _dir_writable(x)){ return x }
      def home_cache = _home_join(".cache")
      if(_dir_writable(home_cache)){ return home_cache }
      return temp_dir()
   } #endif
}

fn _abs_from_base(any base, any rel) str {
   if(!is_str(rel)){ return "" }
   if(rel.len == 0){ return "" }
   if(is_abs(rel)){ return _normalize_path(rel) }
   if(!is_str(base) || base.len == 0){ return _normalize_path(rel) }
   _normalize_path(_path_join(base, rel))
}

comptime template _env_root_getter(name, env_key, doc){
   fn ${name}() str {
      doc
      def raw = common.env_trim(env_key)
      if(raw.len == 0){ return "" }
      _normalize_path(raw)
   }
}

comptime emit _env_root_getter(_repo_root_env,
   "NYTRIX_ROOT",
"Internal: prefers an explicit launcher-provided repo root when available.")
comptime emit _env_root_getter(_share_root_env,
   "NYTRIX_SHARE_ROOT",
"Internal: optional override for installed share root(e.g. `/usr/share/nytrix`).")
comptime emit _env_root_getter(_asset_root_env,
   "NYTRIX_ASSET_ROOT",
"Internal: optional override for direct asset root(e.g. `/usr/share/nytrix/etc/assets`).")

fn _prefix_eq(any s, any prefix) bool {
   if(!is_str(s) || !is_str(prefix)){ return false }
   def sn, pn = s.len, prefix.len
   if(pn == 0 || pn > sn){ return false }
   mut i = 0
   while(i < pn){
      if(load8(s, i) != load8(prefix, i)){ return false }
      i += 1
   }
   true
}

fn _asset_rel_suffix(any rel) str {
   if(!is_str(rel) || rel.len == 0){ return "" }
   def norm = _normalize_path(rel)
   def prefix = "etc" + sep() + "assets" + sep()
   if(!_prefix_eq(norm, prefix)){ return "" }
   _substr(norm, prefix.len, norm.len)
}

fn _search_root_for_rel(any root, str rel) str {
   if(!is_str(root) || root.len == 0){ return "" }
   def cand = _normalize_path(_path_join(root, rel))
   if(_path_exists_local(cand)){ return cand }
   ""
}

fn _search_asset_root_for_rel(any root, str rel) str {
   if(!is_str(root) || root.len == 0){ return "" }
   def direct = _search_root_for_rel(root, rel)
   if(direct.len > 0){ return direct }
   def suffix = _asset_rel_suffix(rel)
   if(suffix.len == 0){ return "" }
   _search_root_for_rel(root, suffix)
}

fn _search_share_or_asset_root(any root, str rel) str {
   def hit = _search_root_for_rel(root, rel)
   if(hit.len > 0){ return hit }
   _search_asset_root_for_rel(_path_join(root, "etc" + sep() + "assets"), rel)
}

fn _resolve_share_or_asset_under_base(any base, str rel, int max_up=8) str {
   def hit = _resolve_under_base(base, rel, max_up)
   if(hit.len > 0){ return _normalize_path(hit) }
   def suffix = _asset_rel_suffix(rel)
   def asset_rel = "etc" + sep() + "assets" + sep() + (suffix.len > 0 ? suffix : rel)
   _resolve_under_base(base, asset_rel, max_up)
}

fn _search_installed_roots(str rel) str {
   def roots = ["/usr/share/nytrix", "/usr/local/share/nytrix", "/opt/nytrix/share/nytrix"]
   mut i = 0
   while(i < roots.len){ def hit = _search_share_or_asset_root(roots.get(i, ""), rel) if(hit.len > 0){ return hit } i += 1 }
   def legacy = _search_root_for_rel("/opt/nytrix/share", rel)
   legacy.len > 0 ? legacy : _search_share_or_asset_root("/opt/homebrew/share/nytrix", rel)
}

fn _normalize_path(any p) str {
   if(!is_str(p)){ return "" }
   if(p.len == 0){ return "" }
   def sepch = sep()
   mut s = p
   #windows {
      s = str_replace(s, "/", "\\")
   } #else {
      s = str_replace(s, "\\", "/")
   } #endif
   def n = s.len
   mut prefix = ""
   mut abs = false
   mut rest = s
   #windows {
      if(n >= 2 && _is_sep(load8(s, 0)) && _is_sep(load8(s, 1))){
         prefix = "\\\\"
         abs = true
         rest = _substr(s, 2, n)
      } elif(_is_drive_prefix(s)){
         prefix = _substr(s, 0, 2)
         rest = _substr(s, 2, n)
         if(rest.len > 0 && _is_sep(load8(rest, 0))){
            abs = true
            rest = _substr(rest, 1, rest.len)
         }
      } elif(n > 0 && _is_sep(load8(s, 0))){
         abs = true
         rest = _substr(s, 1, n)
      }
   } #else {
      if(_is_sep(load8(s, 0))){
         abs = true
         rest = _substr(s, 1, n)
      }
   } #endif
   def raw_parts = split(rest, sepch)
   mut parts = list(0)
   mut i = 0
   while(i < raw_parts.len){
      def p_comp = raw_parts.get(i, "")
      if(p_comp.len == 0 || p_comp == "."){
         i += 1
         continue
      }
      if(p_comp == ".."){
         if(parts.len > 0){
            def last = parts.get(parts.len - 1, "")
            if(last != ".."){
               parts.pop()
               i += 1
               continue
            }
         }
         if(!abs){ parts = parts.append(p_comp) }
         i += 1
         continue
      }
      parts = parts.append(p_comp)
      i += 1
   }
   mut out = ""
   if(prefix.len > 0){
      if(abs && _is_drive_prefix(prefix)){ out = prefix + sepch } else { out = prefix }
   } else if(abs){
      out = sepch
   }
   mut idx = 0
   def part_count = parts.len
   mut b = Builder(max(16, out.len + part_count * 8 + 8))
   if(out.len > 0){ b = builder_append(b, out) }
   mut has_out = out.len > 0
   mut last_is_sep = false
   if(has_out){ last_is_sep = _is_sep(load8(out, out.len - 1)) }
   while(idx < part_count){
      def part = parts.get(idx, "")
      if(has_out && !last_is_sep){ b = builder_append(b, sepch) }
      b = builder_append(b, part)
      has_out = true
      last_is_sep = false
      idx += 1
   }
   out = builder_to_str(b)
   builder_free(b)
   if(out.len == 0 && part_count == 0){
      if(abs){ return sepch }
      if(prefix.len > 0){ return prefix }
      if(n == 0){ return "" }
      return "."
   }
   return out
}

fn normalize(any p) str { _normalize_path(p) }

fn basename(any p) str {
   "Returns the final component of a path(the file or directory name)."
   if(!is_str(p)){ return "" }
   def npath = _normalize_path(p)
   def n = npath.len
   if(n == 0){ return "" }
   def s = sep()
   if(npath == s){ return s }
   mut end = n - 1
   while(end >= 0 && _is_sep(load8(npath, end))){ end -= 1 }
   if(end < 0){ return sep() }
   mut start = end
   while(start >= 0 && !_is_sep(load8(npath, start))){ start -= 1 }
   return _substr(npath, start + 1, end + 1)
}

fn resolve_repo_asset(any rel) str {
   "Resolves a repo-relative asset path when the runtime cwd is not the repository root."
   if(!is_str(rel)){ return "" }
   if(rel.len == 0){ return "" }
   if(is_abs(rel) && _path_exists_local(rel)){ return _normalize_path(rel) }
   def share_root = _share_root_env()
   if(share_root.len > 0){
      def share_hit = _search_share_or_asset_root(share_root, rel)
      if(share_hit.len > 0){ return share_hit }
   }
   def asset_root = _asset_root_env()
   if(asset_root.len > 0){
      def asset_hit = _search_asset_root_for_rel(asset_root, rel)
      if(asset_hit.len > 0){ return asset_hit }
   }
   def repo_root = _repo_root_env()
   if(repo_root.len > 0){
      def repo_hit = _search_share_or_asset_root(repo_root, rel)
      if(repo_hit.len > 0){ return repo_hit }
   }
   def cwd = _cwd_local()
   if(_path_exists_local(rel)){ return _abs_from_base(cwd, rel) }
   def from_cwd = _resolve_share_or_asset_under_base(cwd, rel, 10)
   if(from_cwd.len > 0){ return _normalize_path(from_cwd) }
   def exe0 = argv(0)
   def exe_dir = dirname(exe0)
   def from_exe = _resolve_share_or_asset_under_base(exe_dir, rel, 10)
   if(from_exe.len > 0){ return _normalize_path(from_exe) }
   def exe_share = _normalize_path(_path_join(exe_dir, ".." + sep() + "share" + sep() + "nytrix"))
   def from_exe_share = _search_share_or_asset_root(exe_share, rel)
   if(from_exe_share.len > 0){ return from_exe_share }
   def exe_share_parent = _normalize_path(_path_join(exe_dir, ".." + sep() + ".." + sep() + "share" + sep() + "nytrix"))
   def from_exe_share_parent = _search_share_or_asset_root(exe_share_parent, rel)
   if(from_exe_share_parent.len > 0){ return from_exe_share_parent }
   def installed = _search_installed_roots(rel)
   if(installed.len > 0){ return installed }
   #windows {
      def program_data = common.env_trim("PROGRAMDATA")
      if(program_data.len > 0){
         def win_share = _search_share_or_asset_root(_path_join(program_data, "nytrix"), rel)
         if(win_share.len > 0){ return win_share }
      }
   } #endif
   rel
}

fn dirname(any p) str {
   "Returns the directory component of a path."
   if(!is_str(p)){ return "." }
   def npath = _normalize_path(p)
   def n = npath.len
   if(n == 0){ return "." }
   def s = sep()
   if(npath == s){ return s }
   #windows {
      if(_is_drive_prefix(npath) && n == 3 && _is_sep(load8(npath, 2))){ return npath }
   } #endif
   mut end = n
   while(end > 1 && _is_sep(load8(npath, end - 1))){ end -= 1 }
   mut j = end - 1
   while(j >= 0 && !_is_sep(load8(npath, j))){ j -= 1 }
   if(j < 0){ return "." }
   if(j == 0){ return s }
   #windows {
      if(j == 2 && _is_drive_prefix(npath)){ return _substr(npath, 0, 3) }
   } #endif
   return _substr(npath, 0, j)
}

fn extname(any p) str {
   "Returns the file extension, including the dot(e.g. '.txt')."
   if(!is_str(p)){ return "" }
   def b, n = basename(p), b.len
   if(n == 0){ return "" }
   def dot = common.last_index_byte(b, 46)
   if(dot <= 0 || dot == n - 1){ return "" }
   _substr(b, dot, n)
}

fn splitext(any p) list {
   "Splits path into [root, ext]."
   if(!is_str(p)){ return ["", ""] }
   def ext = extname(p)
   if(ext.len == 0){ return [p, ""] }
   def root_len = p.len - ext.len
   if(root_len < 0){ return [p, ""] }
   [_substr(p, 0, root_len), ext]
}

#main {
   def s = sep()
   assert(s == "/" || s == "\\", "path separator")
   assert(has_sep("a/b"), "path slash sep")
   assert(has_sep("a\\b"), "path backslash sep")
   assert(!has_sep("leaf"), "path no sep")
   assert(join("alpha", "beta") == "alpha" + s + "beta", "path join")
   assert(normalize("alpha/./beta/../gamma") == "alpha" + s + "gamma", "path normalize")
   def nested = "alpha" + s + "beta" + s + "file.tar.gz"
   assert(basename(nested) == "file.tar.gz", "path basename")
   assert(dirname(nested) == "alpha" + s + "beta", "path dirname")
   assert(extname(nested) == ".gz", "path extname")
   assert(splitext(nested) == ["alpha" + s + "beta" + s + "file.tar", ".gz"], "path splitext")
   assert(extname(".bashrc") == "", "path hidden ext")
   assert(splitext("README") == ["README", ""], "path splitext no ext")
   assert(is_str(home_dir()) && home_dir().len > 0, "path home_dir")
   assert(is_str(temp_dir()) && temp_dir().len > 0, "path temp_dir")
   def resolved = resolve_repo_asset("lib/os/path.ny")
   assert(is_abs(resolved), "path resolve repo asset")
   assert(basename(resolved) == "path.ny", "path resolve basename")
   assert(_search_asset_root_for_rel("etc/assets", "fonts/jetbrains.ttf").len > 0, "path resolve asset root suffix")
   assert(_search_asset_root_for_rel("etc/assets", "etc/assets/fonts/jetbrains.ttf").len > 0, "path resolve asset root repo rel")
   assert(_search_share_or_asset_root(".", "fonts/jetbrains.ttf").len > 0, "path resolve share asset root")
   assert(_resolve_share_or_asset_under_base(".", "fonts/jetbrains.ttf", 1).len > 0, "path resolve cwd asset root")
   print("✓ std.os.path self-test passed")
}
