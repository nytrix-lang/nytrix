;; Keywords: os info
;; OS system information (CPU, RAM, GPU).

module std.os.info (
   cpu_name, ram_short, gpu_name, hostname, cpu_logical_count,
   cpu_features_raw, cpu_features, cpu_feature_map, has_cpu_feature,
   has_opencl, system_info
)
use std.core *
use std.core.dict *
use std.str *
use std.os *
use std.os.fs *
use std.os.io *
use std.os.path *

fn _is_windows(){
   "Internal helper."
   eq(__os_name(), "windows")
}

fn _is_macos(){
   "Internal helper."
   eq(__os_name(), "macos")
}

fn _is_linux(){
   "Internal helper."
   eq(__os_name(), "linux")
}

mut _linux_cpuinfo_loaded = false
mut _linux_cpuinfo_cache = ""
mut _cpu_features_raw_loaded = false
mut _cpu_features_raw_cache = ""
mut _cpu_features_norm_loaded = false
mut _cpu_features_norm_cache = ""
mut _cpu_features_loaded = false
mut _cpu_features_cache = 0
mut _cpu_feature_map_loaded = false
mut _cpu_feature_map_cache = 0
mut _hostname_loaded = false
mut _hostname_cache = ""
mut _cpu_name_loaded = false
mut _cpu_name_cache = ""
mut _cpu_logical_count_loaded = false
mut _cpu_logical_count_cache = 1
mut _ram_short_loaded = false
mut _ram_short_cache = ""
mut _gpu_deep_scan_loaded = false
mut _gpu_deep_scan_cache = false
mut _gpu_scan_cards_loaded = false
mut _gpu_scan_cards_cache = 4
mut _gpu_name_loaded = false
mut _gpu_name_cache = ""
mut _system_info_loaded = false
mut _system_info_cache = 0

fn _linux_cpuinfo_text(){
   "Internal helper."
   if(!_is_linux()){ return "" }
   if(_linux_cpuinfo_loaded){ return _linux_cpuinfo_cache }
   _linux_cpuinfo_cache = _read_text(normalize("/proc/cpuinfo"))
   _linux_cpuinfo_loaded = true
   _linux_cpuinfo_cache
}

fn _read_text(path){
   "Internal helper to read a file and return its contents as a string."
   match file_read(path){
      ok(s) -> { return s }
      err(_) -> { return "" }
   }
}

fn _cmd_out(cmd, args){
   "Internal helper: run a command and return stdout (trimmed)."
   def p = spawn(cmd, args)
   if(!p){ return "" }
   shutdown_send(p)
   mut s = recv_all(p)
   close(p)
   if(!is_str(s)){ return "" }
   s = replace_all(s, "\r", "")
   strip(s)
}

fn _first_line(s){
   "Internal helper."
   if(!is_str(s)){ return "" }
   def lines = split(s, "\n")
   mut i = 0
   while(i < len(lines)){
      def ln = strip(get(lines, i, ""))
      if(str_len(ln) > 0){ return ln }
      i += 1
   }
   ""
}

fn _find_value(lines, key){
   "Internal helper."
   mut i = 0
   while(i < len(lines)){
      def ln = strip(get(lines, i, ""))
      if(startswith(ln, key)){
         def idx = find(ln, "=")
         if(idx >= 0){
            return strip(core.slice(ln, idx + 1, str_len(ln), 1))
         }
         return strip(ln)
      }
      i += 1
   }
   ""
}

fn _find_colon_line_value(lines, prefix){
   "Internal helper to find `prefix: value` in a list of lines."
   mut i = 0
   while(i < len(lines)){
      def ln = strip(get(lines, i, ""))
      if(startswith(ln, prefix)){
         return _after_colon(ln)
      }
      i += 1
   }
   ""
}

fn _after_colon(s){
   "Internal helper to extract a value from a 'Key: Value' string."
   if(!is_str(s)){ return "" }
   def n = str_len(s)
   mut i = 0
   while(i < n){
      if(load8(s, i) == 58){ ; ':'
         i += 1
         while(i < n && (load8(s, i) == 32 || load8(s, i) == 9)){ i += 1 }
         def out = malloc(n - i + 1)
         if(!out){ return "" }
         init_str(out, n - i)
         mut k = 0
         while(i + k < n){
            store8(out, load8(s, i + k), k)
            k += 1
         }
         store8(out, 0, n - i)
         return strip(out)
      }
      i += 1
   }
   strip(s)
}

fn _first_number(s){
   "Internal helper to extract the first integer found in a string."
   if(!is_str(s)){ return 0 }
   def n = str_len(s)
   mut i = 0
   while(i < n && (load8(s, i) < 48 || load8(s, i) > 57)){ i += 1 }
   mut v = 0
   while(i < n){
      def c = load8(s, i)
      if(c < 48 || c > 57){ break }
      v = v * 10 + (c - 48)
      i += 1
   }
   v
}

fn _find_prefixed_line_value(text, prefix){
   "Internal helper to extract the value after ':' from first line matching prefix."
   if(!is_str(text) || !is_str(prefix)){ return "" }
   def n = str_len(text)
   def m = str_len(prefix)
   if(n == 0 || m == 0 || n < m){ return "" }
   mut i = 0
   while(i < n){
      def line_start = i
      mut line_end = i
      while(line_end < n && load8(text, line_end) != 10 && load8(text, line_end) != 13){
         line_end += 1
      }
      mut j = 0
      while(j < m && line_start + j < line_end){
         if(load8(text, line_start + j) != load8(prefix, j)){ break }
         j += 1
      }
      if(j == m){
         mut k = line_start + j
         while(k < line_end){
            if(load8(text, k) == 58){ ;; ':'
               k += 1
               break
            }
            k += 1
         }
         while(k < line_end && (load8(text, k) == 32 || load8(text, k) == 9)){ k += 1 }
         if(k < line_end){ return strip(slice(text, k, line_end, 1)) }
         return ""
      }
      i = line_end + 1
      while(i < n && (load8(text, i) == 10 || load8(text, i) == 13)){ i += 1 }
   }
   ""
}

fn _format_mem_usage_kb(total_kb, free_kb){
   "Internal helper."
   if(total_kb <= 0){ return "" }
   def used_mb = (total_kb - free_kb) / 1024
   def total_mb = total_kb / 1024
   to_str(used_mb) + "/" + to_str(total_mb) + "MB"
}

fn _parse_windows_mem_summary(text){
   "Internal helper."
   if(!is_str(text) || str_len(text) == 0){ return "" }
   def lines = split(text, "\n")
   mut total_kb = 0
   mut free_kb = 0
   mut i = 0
   while(i < len(lines)){
      def ln = strip(get(lines, i, ""))
      if(startswith(ln, "TotalVisibleMemorySize")){ total_kb = _first_number(ln) }
      elif(startswith(ln, "FreePhysicalMemory")){ free_kb = _first_number(ln) }
      i += 1
   }
   _format_mem_usage_kb(total_kb, free_kb)
}

fn cpu_logical_count(){
   "Returns the detected number of logical CPU cores."
   if(_cpu_logical_count_loaded){ return _cpu_logical_count_cache }
   mut out = 0
   if(_is_linux()){
      def nproc_out = _cmd_out("nproc", [])
      def n0 = _first_number(nproc_out)
      if(n0 > 0){ out = n0 }

      if(out <= 0){
         def cpu = _linux_cpuinfo_text()
         if(str_len(cpu) > 0){
            def lines = split(cpu, "\n")
            mut n = 0
            mut i = 0
            while(i < len(lines)){
               def ln = strip(get(lines, i, ""))
               if(startswith(ln, "processor")){ n += 1 }
               i += 1
            }
            if(n > 0){ out = n }
         }
      }
   } elif(_is_macos()){
      def sysctl_out = _cmd_out("sysctl", ["-n", "hw.logicalcpu"])
      def n = _first_number(sysctl_out)
      if(n > 0){ out = n }
      if(out <= 0){
         def ncpu_out = _cmd_out("sysctl", ["-n", "hw.ncpu"])
         def n2 = _first_number(ncpu_out)
         if(n2 > 0){ out = n2 }
      }
   } elif(_is_windows()){
      def p = env("NUMBER_OF_PROCESSORS")
      def n = _first_number(p)
      if(n > 0){ out = n }
      if(out <= 0){
         def wmic_out = _cmd_out("wmic", ["cpu", "get", "NumberOfLogicalProcessors", "/value"])
         def n2 = _first_number(wmic_out)
         if(n2 > 0){ out = n2 }
      }
   }
   if(out <= 0){ out = 1 }
   _cpu_logical_count_cache = out
   _cpu_logical_count_loaded = true
   out
}

fn _linux_cpu_features_raw(){
   "Internal helper."
   def out = _cmd_out("lscpu", [])
   if(str_len(out) > 0){
      def lines = split(out, "\n")
      mut v2 = _find_colon_line_value(lines, "Flags")
      if(str_len(v2) == 0){ v2 = _find_colon_line_value(lines, "Features") }
      if(str_len(v2) > 0){ return strip(v2) }
   }

   def cpu = _linux_cpuinfo_text()
   if(str_len(cpu) > 0){
      def lines2 = split(cpu, "\n")
      mut i = 0
      while(i < len(lines2)){
         def ln = strip(get(lines2, i, ""))
         if(startswith(ln, "flags") || startswith(ln, "Features") || startswith(ln, "isa")){
            def v = _after_colon(ln)
            if(str_len(v) > 0){ return strip(v) }
         }
         i += 1
      }
   }
   ""
}

fn _macos_cpu_features_raw(){
   "Internal helper."
   mut out = _cmd_out("sysctl", ["-n", "machdep.cpu.features"])
   mut out2 = _cmd_out("sysctl", ["-n", "machdep.cpu.leaf7_features"])
   if(str_len(out) > 0 && str_len(out2) > 0){ return strip(out + " " + out2) }
   if(str_len(out) > 0){ return strip(out) }
   if(str_len(out2) > 0){ return strip(out2) }

   ;; Apple Silicon/Generic: collect enabled hw.optional feature flags.
   def all = _cmd_out("sysctl", ["hw.optional"])
   if(str_len(all) == 0){ return "" }
   def lines = split(all, "\n")
   mut feats = list(16)
   mut i = 0
   while(i < len(lines)){
      def ln = strip(get(lines, i, ""))
      if(startswith(ln, "hw.optional.")){
         def idx = find(ln, ":")
         if(idx >= 0){
            def key = strip(slice(ln, 0, idx, 1))
            def val = strip(slice(ln, idx + 1, str_len(ln), 1))
            if(eq(val, "1")){
               ;; Clean up prefix (hw.optional. => "")
               def clean = slice(key, 12, str_len(key), 1)
               feats = append(feats, clean)
            }
         }
      }
      i += 1
   }
   if(len(feats) > 0){ return join(feats, " ") }
   ""
}

fn _windows_cpu_features_raw(){
   "Internal helper."
   def env_cpu = env("PROCESSOR_IDENTIFIER")
   if(is_str(env_cpu) && str_len(strip(env_cpu)) > 0){ return strip(env_cpu) }
   mut out = _cmd_out("wmic", ["cpu", "get", "Caption,Name", "/value"])
   if(str_len(out) > 0){ return out }
   out = _cmd_out("powershell", ["-NoProfile", "-Command", "(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)"])
   if(str_len(out) > 0){ return _first_line(out) }
   ""
}

fn cpu_features_raw(){
   "Returns a raw CPU feature string from OS-specific sources."
   if(_cpu_features_raw_loaded){ return _cpu_features_raw_cache }
   mut out = ""
   if(_is_linux()){ out = _linux_cpu_features_raw() }
   elif(_is_macos()){ out = _macos_cpu_features_raw() }
   elif(_is_windows()){ out = _windows_cpu_features_raw() }
   if(!is_str(out)){ out = "" }
   _cpu_features_raw_cache = out
   _cpu_features_raw_loaded = true
   out
}

fn _normalize_feature_text(raw){
   "Internal helper."
   if(!is_str(raw)){ return "" }
   mut s = " " + lower(strip(raw)) + " "
   if(str_len(strip(s)) == 0){ return "" }
   s = replace_all(s, "\t", " ")
   s = replace_all(s, "\n", " ")
   s = replace_all(s, "\r", " ")
   s = replace_all(s, ",", " ")
   s = replace_all(s, ";", " ")
   s = replace_all(s, ":", " ")
   s = replace_all(s, "=", " ")
   s = replace_all(s, ".", "_")
   s = replace_all(s, "-", "_")
   s
}

fn _cpu_features_norm_text(){
   "Internal helper."
   if(_cpu_features_norm_loaded){ return _cpu_features_norm_cache }
   _cpu_features_norm_cache = _normalize_feature_text(cpu_features_raw())
   _cpu_features_norm_loaded = true
   _cpu_features_norm_cache
}

fn _feature_has(norm, name){
   "Internal helper."
   if(!is_str(norm) || str_len(norm) == 0){ return false }
   str_contains(norm, " " + name + " ")
}

fn _normalize_feature_name(name){
   "Internal helper to normalize feature names for robust lookup."
   if(!is_str(name)){ return "" }
   mut s = lower(strip(name))
   if(str_len(s) == 0){ return "" }
   s = replace_all(s, ".", "_")
   s = replace_all(s, "-", "_")
   s = replace_all(s, " ", "_")
   while(str_contains(s, "__")){ s = replace_all(s, "__", "_") }
   if(startswith(s, "_")){ s = core.slice(s, 1, str_len(s), 1) }
   if(endswith(s, "_")){ s = core.slice(s, 0, str_len(s) - 1, 1) }
   if(s == "sse41"){ return "sse4_1" }
   if(s == "sse42"){ return "sse4_2" }
   if(s == "sha1" || s == "sha2" || s == "sha_ni"){ return "sha" }
   if(s == "asimd"){ return "neon" }
   if(s == "avx512"){ return "avx512f" }
   s
}

fn cpu_features(){
   "Returns a normalized list of commonly-used CPU feature names."
   if(_cpu_features_loaded){ return list_clone(_cpu_features_cache) }
   def n = _cpu_features_norm_text()
   mut out = list(32)
   if(_feature_has(n, "mmx")){ out = append(out, "mmx") }
   if(_feature_has(n, "sse")){ out = append(out, "sse") }
   if(_feature_has(n, "sse2")){ out = append(out, "sse2") }
   if(_feature_has(n, "sse3")){ out = append(out, "sse3") }
   if(_feature_has(n, "ssse3")){ out = append(out, "ssse3") }
   if(_feature_has(n, "sse4_1")){ out = append(out, "sse4_1") }
   if(_feature_has(n, "sse4_2")){ out = append(out, "sse4_2") }
   if(_feature_has(n, "popcnt")){ out = append(out, "popcnt") }
   if(_feature_has(n, "aes")){ out = append(out, "aes") }
   if(_feature_has(n, "pclmulqdq")){ out = append(out, "pclmulqdq") }
   if(_feature_has(n, "fma")){ out = append(out, "fma") }
   if(_feature_has(n, "avx")){ out = append(out, "avx") }
   if(_feature_has(n, "avx2")){ out = append(out, "avx2") }
   if(_feature_has(n, "avx512f")){ out = append(out, "avx512f") }
   if(_feature_has(n, "bmi1")){ out = append(out, "bmi1") }
   if(_feature_has(n, "bmi2")){ out = append(out, "bmi2") }
   if(_feature_has(n, "sha_ni") || _feature_has(n, "sha1") || _feature_has(n, "sha2")){
      out = append(out, "sha")
   }
   if(_feature_has(n, "neon") || _feature_has(n, "asimd")){ out = append(out, "neon") }
   if(_feature_has(n, "sve")){ out = append(out, "sve") }
   if(_feature_has(n, "crc32")){ out = append(out, "crc32") }
   if(_feature_has(n, "atomics")){ out = append(out, "atomics") }
   if(_feature_has(n, "fp16")){ out = append(out, "fp16") }
   _cpu_features_cache = out
   _cpu_features_loaded = true
   list_clone(out)
}

fn cpu_feature_map(){
   "Returns a normalized dictionary of commonly-used detected CPU features.

   The returned dict maps normalized feature names to `true` and is optimized
   for fast lookup of common features.
   Examples:
   - `\"sse4.1\"`, `\"sse41\"` and `\"sse4_1\"` normalize to `\"sse4_1\"`
   - `\"sha_ni\"`, `\"sha1\"`, `\"sha2\"` normalize to `\"sha\"`
   - `\"asimd\"` normalizes to `\"neon\"`
   "
   if(_cpu_feature_map_loaded){ return dict_clone(_cpu_feature_map_cache) }
   mut m = dict(64)

   ;; Ensure curated/common aliases are always present and normalized.
   def curated = cpu_features()
   mut j = 0
   while(j < len(curated)){
      def c = _normalize_feature_name(get(curated, j, ""))
      if(str_len(c) > 0){ m = dict_set(m, c, true) }
      j += 1
   }

   _cpu_feature_map_cache = m
   _cpu_feature_map_loaded = true
   dict_clone(m)
}

fn has_cpu_feature(name){
   "Returns true when CPU feature `name` is detected.

   `name` is normalized, so variants like `\"sse4.1\"`, `\"sse41\"`,
   and `\"sse4_1\"` are equivalent.
   "
   def n = _normalize_feature_name(name)
   if(str_len(n) == 0){ return false }
   if(dict_get(cpu_feature_map(), n, false)){ return true }
   _feature_has(_cpu_features_norm_text(), n)
}

fn _gpu_deep_scan_enabled(){
   "Internal helper."
   if(_gpu_deep_scan_loaded){ return _gpu_deep_scan_cache }
   mut out = false
   def raw = env("NYTRIX_GPU_DEEP_SCAN")
   if(is_str(raw)){
      def v = lower(strip(raw))
      if(v == "1" || v == "true" || v == "yes" || v == "on"){ out = true }
   }
   _gpu_deep_scan_cache = out
   _gpu_deep_scan_loaded = true
   out
}

fn _gpu_scan_cards_limit(){
   "Internal helper."
   if(_gpu_scan_cards_loaded){ return _gpu_scan_cards_cache }
   mut n = 4
   def raw = env("NYTRIX_GPU_SCAN_CARDS")
   if(is_str(raw)){
      def s = strip(raw)
      if(str_len(s) > 0){
         def v = atoi(s)
         if(v > 0){
            n = v
            if(n > 8){ n = 8 }
         }
      }
   }
   _gpu_scan_cards_cache = n
   _gpu_scan_cards_loaded = true
   n
}

fn has_opencl(){
   "Returns true when an OpenCL runtime appears available on this host."
   def force = env("NYTRIX_OPENCL_FORCE")
   if(is_str(force)){
      def f = lower(strip(force))
      if(f == "1" || f == "true" || f == "yes" || f == "on"){ return true }
      if(f == "0" || f == "false" || f == "no" || f == "off"){ return false }
   }
   if(_is_windows()){
      if(file_exists("C:\\Windows\\System32\\OpenCL.dll")){ return true }
      if(file_exists("C:\\Windows\\SysWOW64\\OpenCL.dll")){ return true }
      return false
   }
   if(_is_macos()){
      return file_exists(normalize("/System/Library/Frameworks/OpenCL.framework/OpenCL"))
   }
   if(file_exists(normalize("/etc/OpenCL/vendors"))){ return true }
   if(file_exists(normalize("/usr/lib/libOpenCL.so"))){ return true }
   if(file_exists(normalize("/usr/lib64/libOpenCL.so"))){ return true }
   if(file_exists(normalize("/usr/local/lib/libOpenCL.so"))){ return true }
   if(file_exists(normalize("/lib/x86_64-linux-gnu/libOpenCL.so.1"))){ return true }
   if(file_exists(normalize("/usr/lib/x86_64-linux-gnu/libOpenCL.so.1"))){ return true }
   false
}

fn hostname(){
   "Returns the machine hostname."
   if(_hostname_loaded){ return _hostname_cache }
   mut out = ""
   if(_is_windows()){
      def hn = env("COMPUTERNAME")
      if(is_str(hn) && str_len(strip(hn)) > 0){ out = strip(hn) }
      if(str_len(out) == 0){
         def host_out = _cmd_out("hostname", [])
         if(str_len(host_out) > 0){ out = _first_line(host_out) }
      }
      if(str_len(out) == 0){ out = "windows-host" }
      _hostname_cache = out
      _hostname_loaded = true
      return out
   }
   def env_hn = env("HOSTNAME")
   if(is_str(env_hn) && str_len(strip(env_hn)) > 0){ out = strip(env_hn) }
   if(str_len(out) == 0 && _is_linux()){
      def proc_hn = _read_text(normalize("/proc/sys/kernel/hostname"))
      if(str_len(proc_hn) > 0){
         def h0 = _first_line(proc_hn)
         if(str_len(h0) > 0){ out = h0 }
      }
   }
   if(str_len(out) == 0 && _is_macos()){
      def sys_hn = _cmd_out("sysctl", ["-n", "kern.hostname"])
      if(str_len(sys_hn) > 0){
         def h1 = _first_line(sys_hn)
         if(str_len(h1) > 0){ out = h1 }
      }
   }
   if(str_len(out) == 0){
      def etc_hn = _read_text(normalize("/etc/hostname"))
      if(str_len(etc_hn) > 0){
         def h = _first_line(etc_hn)
         if(str_len(h) > 0){ out = h }
      }
   }
   if(str_len(out) == 0 && !_is_macos()){
      def host_out = _cmd_out("hostname", [])
      if(str_len(host_out) > 0){ out = _first_line(host_out) }
   }
   if(str_len(out) == 0){ out = os() + "-host" }
   _hostname_cache = out
   _hostname_loaded = true
   out
}

fn cpu_name(){
   "Returns the CPU model name.

   Linux uses /proc, macOS uses sysctl, Windows uses wmic/powershell."
   if(_cpu_name_loaded){ return _cpu_name_cache }
   mut out = ""
   if(load8(__os_name(), 0) == 108){ ;; 'l' - linux
      def ls = _cmd_out("lscpu", [])
      if(str_len(ls) > 0){
         def lines = split(ls, "\n")
         def lm = _find_colon_line_value(lines, "Model name")
         if(str_len(lm) > 0){ out = lm }
         if(str_len(out) == 0){
            def la = _find_colon_line_value(lines, "Architecture")
            if(str_len(la) > 0){ out = la }
         }
      }

      if(str_len(out) == 0){
         def cpu = _linux_cpuinfo_text()
         if(str_len(cpu) > 0){
            def lines2 = split(cpu, "\n")
            mut i = 0
            while(i < len(lines2)){
               def ln = strip(get(lines2, i, ""))
               if(startswith(ln, "model name") || startswith(ln, "Hardware")){
                  def v = _after_colon(ln)
                  if(str_len(v) > 0){
                     out = v
                     break
                  }
               }
               i += 1
            }
            if(str_len(out) == 0){
               out = _first_line(cpu)
            }
         }
      }
   } elif(load8(__os_name(), 0) == 109){ ;; 'm' - macos
      mut mac_out = _cmd_out("sysctl", ["-n", "machdep.cpu.brand_string"])
      if(str_len(mac_out) > 0){ out = _first_line(mac_out) }
      if(str_len(out) == 0){
         mac_out = _cmd_out("sysctl", ["-n", "hw.model"])
         if(str_len(mac_out) > 0){ out = _first_line(mac_out) }
      }
   } elif(load8(__os_name(), 0) == 119){ ;; 'w' - windows
      mut win_out = _cmd_out("wmic", ["cpu", "get", "Name", "/value"])
      if(str_len(win_out) > 0){
         def lines = split(win_out, "\n")
         def v = _find_value(lines, "Name")
         if(str_len(v) > 0){ out = v }
      }
      if(str_len(out) == 0){
         win_out = _cmd_out("powershell", ["-NoProfile", "-Command", "(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)"])
         if(str_len(win_out) > 0){ out = _first_line(win_out) }
      }
      if(str_len(out) == 0){
         def env_cpu = env("PROCESSOR_IDENTIFIER")
         if(is_str(env_cpu) && str_len(env_cpu) > 0){ out = env_cpu }
      }
   }
   if(str_len(out) == 0){ out = os() + " cpu" }
   _cpu_name_cache = out
   _cpu_name_loaded = true
   out
}

fn ram_short(){
   "Returns a summary string of system RAM usage (e.g., '2048/8192MB').

   Linux uses /proc ; macOS uses sysctl/vm_stat; Windows uses wmic/powershell."
   if(_ram_short_loaded){ return _ram_short_cache }
   if(_is_linux()){
      def mem = _read_text(normalize("/proc/meminfo"))
      if(str_len(mem) > 0){
         def total_v = _find_prefixed_line_value(mem, "MemTotal")
         def avail_v = _find_prefixed_line_value(mem, "MemAvailable")
         def total_kb = _first_number(total_v)
         def avail_kb = _first_number(avail_v)
         if(total_kb > 0){
            if(avail_kb <= 0){
               _ram_short_cache = to_str(total_kb / 1024) + "MB total"
               _ram_short_loaded = true
               return _ram_short_cache
            }
            _ram_short_cache = _format_mem_usage_kb(total_kb, avail_kb)
            _ram_short_loaded = true
            return _ram_short_cache
         }
      }
   } elif(_is_macos()){
      def total = _cmd_out("sysctl", ["-n", "hw.memsize"])
      mut total_b = _first_number(total)
      if(total_b <= 0){
         def t2 = _cmd_out("sysctl", ["-n", "hw.physmem"])
         total_b = _first_number(t2)
      }
      def vm = _cmd_out("vm_stat", [])
      if(total_b > 0 && str_len(vm) > 0){
         def lines = split(vm, "\n")
         mut page_sz = 4096
         mut free_p = 0
         mut spec_p = 0
         mut i = 0
         while(i < len(lines)){
            def ln = strip(get(lines, i, ""))
            if(startswith(ln, "Mach Virtual Memory Statistics")){
               page_sz = _first_number(ln)
               if(page_sz <= 0){ page_sz = 4096 }
            } elif(startswith(ln, "Pages free:")){
               free_p = _first_number(ln)
            } elif(startswith(ln, "Pages speculative:")){
               spec_p = _first_number(ln)
            }
            i += 1
         }
         def free_b = (free_p + spec_p) * page_sz
         mut used_b = total_b - free_b
         if(used_b < 0){ used_b = 0 }
         def used_mb = used_b / (1024 * 1024)
         def total_mb = total_b / (1024 * 1024)
         _ram_short_cache = to_str(used_mb) + "/" + to_str(total_mb) + "MB"
         _ram_short_loaded = true
         return _ram_short_cache
      }
      if(total_b > 0){
         def total_mb = total_b / (1024 * 1024)
         _ram_short_cache = to_str(total_mb) + "MB total"
         _ram_short_loaded = true
         return _ram_short_cache
      }
   } elif(_is_windows()){
      mut out = _cmd_out("wmic", ["OS", "get", "TotalVisibleMemorySize,FreePhysicalMemory", "/value"])
      def wmic_mem = _parse_windows_mem_summary(out)
      if(str_len(wmic_mem) > 0){
         _ram_short_cache = wmic_mem
         _ram_short_loaded = true
         return _ram_short_cache
      }
      out = _cmd_out("powershell", ["-NoProfile", "-Command", "(Get-CimInstance Win32_OperatingSystem | Select-Object -First 1 TotalVisibleMemorySize,FreePhysicalMemory | Format-List)"])
      def ps_mem = _parse_windows_mem_summary(out)
      if(str_len(ps_mem) > 0){
         _ram_short_cache = ps_mem
         _ram_short_loaded = true
         return _ram_short_cache
      }
   }
   _ram_short_cache = os() + " ram"
   _ram_short_loaded = true
   _ram_short_cache
}

fn gpu_name(){
   "Returns the name or primary identifier of the system's GPU.

   Linux uses sysfs ; macOS uses system_profiler; Windows uses wmic/powershell."
   if(_gpu_name_loaded){ return _gpu_name_cache }
   if(_is_macos()){
      def out = _cmd_out("system_profiler", ["SPDisplaysDataType", "-detailLevel", "mini"])
      if(str_len(out) > 0){
         def lines = split(out, "\n")
         mut i = 0
         while(i < len(lines)){
            def ln = strip(get(lines, i, ""))
            if(startswith(ln, "Chipset Model:")){
               _gpu_name_cache = _after_colon(ln)
               _gpu_name_loaded = true
               return _gpu_name_cache
            }
            if(startswith(ln, "Model:")){
               _gpu_name_cache = _after_colon(ln)
               _gpu_name_loaded = true
               return _gpu_name_cache
            }
            i += 1
         }
         _gpu_name_cache = _first_line(out)
         _gpu_name_loaded = true
         return _gpu_name_cache
      }
      _gpu_name_cache = os() + " gpu"
      _gpu_name_loaded = true
      return _gpu_name_cache
   }
   if(_is_windows()){
      mut out = _cmd_out("wmic", ["path", "win32_VideoController", "get", "Name", "/value"])
      if(str_len(out) > 0){
         def lines = split(out, "\n")
         def v = _find_value(lines, "Name")
         if(str_len(v) > 0){
            _gpu_name_cache = v
            _gpu_name_loaded = true
            return _gpu_name_cache
         }
      }
      out = _cmd_out("powershell", ["-NoProfile", "-Command", "(Get-CimInstance Win32_VideoController | Select-Object -First 1 -ExpandProperty Name)"])
      if(str_len(out) > 0){
         _gpu_name_cache = _first_line(out)
         _gpu_name_loaded = true
         return _gpu_name_cache
      }
      _gpu_name_cache = os() + " gpu"
      _gpu_name_loaded = true
      return _gpu_name_cache
   }
   if(load8(__os_name(), 0) != 108){
      _gpu_name_cache = __os_name() + " gpu"
      _gpu_name_loaded = true
      return _gpu_name_cache
   }
   ;; 2. Fallback: DRM sysfs (card0-card7)
   def max_cards = _gpu_scan_cards_limit()
   mut i = 0
   while(i < max_cards){
      def base = normalize("/sys/class/drm/card" + to_str(i) + "/device/")
      def vendor = strip(_read_text(base + "vendor"))
      def dev = strip(_read_text(base + "device"))
      if(str_len(vendor) > 0 && str_len(dev) > 0){
         _gpu_name_cache = "pci " + vendor + ":" + dev
         _gpu_name_loaded = true
         return _gpu_name_cache
      }
      i += 1
   }
   if(!_gpu_deep_scan_enabled()){
      _gpu_name_cache = os() + " gpu"
      _gpu_name_loaded = true
      return _gpu_name_cache
   }
   ;; 3. Fallback: PCI bus sysfs (scan /sys/bus/pci/devices)
   def pci_path = normalize("/sys/bus/pci/devices")
   if(file_exists(pci_path)){
      def entries = list_dir(pci_path)
      mut j = 0
      while(j < len(entries)){
         def entry = get(entries, j, "")
         def class_file = normalize(pci_path + "/" + entry + "/class")
         if(file_exists(class_file)){
            def class_hex = strip(_read_text(class_file))
            ;; Display controller: 0x030000, 3D controller: 0x030200
            if(startswith(class_hex, "0x0300") || startswith(class_hex, "0x0302")){
               def vf = normalize(pci_path + "/" + entry + "/vendor")
               def df = normalize(pci_path + "/" + entry + "/device")
               if(file_exists(vf) && file_exists(df)){
                  def v = strip(_read_text(vf))
                  def d = strip(_read_text(df))
                  if(str_len(v) > 0 && str_len(d) > 0){
                     _gpu_name_cache = "pci " + v + ":" + d
                     _gpu_name_loaded = true
                     return _gpu_name_cache
                  }
               }
            }
         }
         j += 1
      }
   }
   _gpu_name_cache = os() + " gpu"
   _gpu_name_loaded = true
   _gpu_name_cache
}

fn system_info(){
   "Returns a dictionary with common host information.

   GPU/accelerator/parallel policy fields reflect current runtime configuration.
   In CLI usage, compiler flags are the primary source (for example `--gpu`,
   `--accel-target`, `--parallel`)."
   if(_system_info_loaded){ return dict_clone(_system_info_cache) }
   mut d = dict(16)
   d = dict_set(d, "os", os())
   d = dict_set(d, "arch", arch())
   d = dict_set(d, "platform", os() + "/" + arch())
   d = dict_set(d, "hostname", hostname())
   d = dict_set(d, "cpu", cpu_name())
   mut logical = cpu_logical_count()
   if(logical <= 0){ logical = 1 }
   d = dict_set(d, "logical_cpus", logical)
   d = dict_set(d, "cpu_features_raw", cpu_features_raw())
   d = dict_set(d, "cpu_features", cpu_features())
   d = dict_set(d, "cpu_feature_map", cpu_feature_map())
   d = dict_set(d, "ram", ram_short())
   d = dict_set(d, "gpu", gpu_name())
   d = dict_set(d, "opencl", has_opencl())
   d = dict_set(d, "gpu_mode", gpu_mode())
   d = dict_set(d, "gpu_backend", gpu_backend())
   d = dict_set(d, "gpu_offload", gpu_offload())
   d = dict_set(d, "gpu_min_work", gpu_min_work())
   d = dict_set(d, "gpu_async", gpu_async())
   d = dict_set(d, "gpu_fast_math", gpu_fast_math())
   d = dict_set(d, "gpu_available", gpu_available())
   def gst = gpu_offload_status(0)
   d = dict_set(d, "gpu_selected_backend", dict_get(gst, "selected_backend", gpu_backend()))
   d = dict_set(d, "gpu_offload_policy_selected", dict_get(gst, "policy_selected", false))
   d = dict_set(d, "gpu_offload_active", dict_get(gst, "active", false))
   d = dict_set(d, "gpu_offload_reason", dict_get(gst, "reason", ""))
   d = dict_set(d, "gpu_offload_active_reason", dict_get(gst, "active_reason", ""))
   d = dict_set(d, "accel_target", accel_target())
   d = dict_set(d, "accel_targets", accel_targets())
   d = dict_set(d, "accel_binary_kind", accel_binary_kind())
   d = dict_set(d, "accel_binary_ext", accel_binary_ext())
   def ast = accel_target_status()
   d = dict_set(d, "accel_status", ast)
   d = dict_set(d, "accel_available", dict_get(ast, "available", false))
   d = dict_set(d, "parallel_mode", parallel_mode())
   d = dict_set(d, "parallel_threads", parallel_threads())
   d = dict_set(d, "parallel_min_work", parallel_min_work())
   def pst = parallel_status(0)
   d = dict_set(d, "parallel_effective_threads", dict_get(pst, "effective_threads", parallel_threads()))
   d = dict_set(d, "parallel_policy_selected", dict_get(pst, "selected", false))
   d = dict_set(d, "parallel_reason", dict_get(pst, "reason", ""))
   _system_info_cache = d
   _system_info_loaded = true
   dict_clone(d)
}

if(comptime{__main()}){
    use std.os.info *
    use std.os *
    use std.str *
    use std.core *
    use std.core.error *
    use std.core.dict *

    print("Testing std.os.info...")

    def hn = hostname()
    assert(is_str(hn) && str_len(strip(hn)) > 0, "hostname")

    def si = system_info()
    assert(is_dict(si), "system_info dict")
    assert(eq(dict_get(si, "os", ""), os()), "system_info os")
    assert(eq(dict_get(si, "arch", ""), arch()), "system_info arch")
    mut platform_v = dict_get(si, "platform", "")
    if(!is_str(platform_v) || str_len(strip(platform_v)) == 0){
       platform_v = dict_get(si, "os", "") + "/" + dict_get(si, "arch", "")
    }
    assert(str_len(strip(platform_v)) > 0, "system_info platform")
    assert(str_len(strip(dict_get(si, "hostname", ""))) > 0, "system_info hostname")
    assert(str_len(strip(dict_get(si, "cpu", ""))) > 0, "system_info cpu")
    mut logical = dict_get(si, "logical_cpus", 0)
    if(logical < 1){ logical = cpu_logical_count() }
    if(logical < 1){ logical = 1 }
    assert(logical >= 1, "system_info logical_cpus")
    assert(is_str(cpu_features_raw()), "cpu_features_raw api")
    assert(is_list(cpu_features()), "cpu_features api")
    assert(is_dict(cpu_feature_map()), "cpu_feature_map api")
    assert((has_cpu_feature("sse4.1") == has_cpu_feature("sse4_1")), "feature alias sse4.1")
    assert((has_cpu_feature("sse41") == has_cpu_feature("sse4_1")), "feature alias sse41")
    assert((has_cpu_feature("sha_ni") == has_cpu_feature("sha")), "feature alias sha_ni")
    assert(str_len(strip(dict_get(si, "ram", ""))) > 0, "system_info ram")
    assert(str_len(strip(dict_get(si, "gpu", ""))) > 0, "system_info gpu")
    assert(is_str(dict_get(si, "cpu_features_raw", "")), "system_info cpu_features_raw")
    def sif = dict_get(si, "cpu_features", list(1))
    assert(is_list(sif), "system_info cpu_features list")
    def sfm = dict_get(si, "cpu_feature_map", 0)
    assert(is_dict(sfm), "system_info cpu_feature_map dict")
    assert((dict_get(sfm, "sha", false) == has_cpu_feature("sha")), "system_info cpu_feature_map mirrors api")
    mut i = 0
    mut _feature_probe_checks = 0
    while(i < len(sif) && _feature_probe_checks < 3){
       def feat = strip(get(sif, i, ""))
       if(str_len(feat) > 0){
          assert(dict_get(sfm, feat, false), "system_info cpu_feature_map contains sampled feature")
          assert(has_cpu_feature(feat), "has_cpu_feature(sampled)")
          _feature_probe_checks += 1
       }
       i += 1
    }
    if(len(sif) > 0){
       assert(_feature_probe_checks > 0, "system_info cpu_features sample processed")
    }
    def cl = dict_get(si, "opencl", false)
    assert((cl == true || cl == false), "system_info opencl bool")
    assert(str_len(strip(dict_get(si, "gpu_mode", ""))) > 0, "system_info gpu_mode")
    assert(str_len(strip(dict_get(si, "gpu_backend", ""))) > 0, "system_info gpu_backend")
    assert(str_len(strip(dict_get(si, "gpu_offload", ""))) > 0, "system_info gpu_offload")
    assert(dict_get(si, "gpu_min_work", -1) >= 0, "system_info gpu_min_work")
    def gasync = dict_get(si, "gpu_async", false)
    assert((gasync == true || gasync == false), "system_info gpu_async")
    def gfast = dict_get(si, "gpu_fast_math", false)
    assert((gfast == true || gfast == false), "system_info gpu_fast_math")
    def gav = dict_get(si, "gpu_available", false)
    assert((gav == true || gav == false), "system_info gpu_available")
    def at = dict_get(si, "accel_target", "")
    assert((eq(at, "none") || eq(at, "nvptx") || eq(at, "amdgpu") || eq(at, "spirv") || eq(at, "hsaco")), "system_info accel_target")
    def atg = dict_get(si, "accel_targets", list(1))
    assert(is_list(atg), "system_info accel_targets list")
    assert(len(atg) >= 4, "system_info accel_targets size")
    def abk = dict_get(si, "accel_binary_kind", "")
    assert((eq(abk, "none") || eq(abk, "ptx") || eq(abk, "o") || eq(abk, "spv") || eq(abk, "hsaco")), "system_info accel_binary_kind")
    def abe = dict_get(si, "accel_binary_ext", "")
    assert((eq(abe, "") || eq(abe, ".ptx") || eq(abe, ".o") || eq(abe, ".spv") || eq(abe, ".hsaco")), "system_info accel_binary_ext")
    def ast = dict_get(si, "accel_status", 0)
    assert(is_dict(ast), "system_info accel_status dict")
    def aav = dict_get(si, "accel_available", false)
    assert((aav == true || aav == false), "system_info accel_available")
    assert(str_len(strip(dict_get(si, "gpu_selected_backend", ""))) > 0, "system_info gpu_selected_backend")
    def gpol = dict_get(si, "gpu_offload_policy_selected", false)
    assert((gpol == true || gpol == false), "system_info gpu_offload_policy_selected")
    def gact = dict_get(si, "gpu_offload_active", false)
    assert((gact == true || gact == false), "system_info gpu_offload_active")
    assert(str_len(strip(dict_get(si, "gpu_offload_reason", ""))) > 0, "system_info gpu_offload_reason")
    assert(str_len(strip(dict_get(si, "gpu_offload_active_reason", ""))) > 0, "system_info gpu_offload_active_reason")
    assert(str_len(strip(dict_get(si, "parallel_mode", ""))) > 0, "system_info parallel_mode")
    assert(dict_get(si, "parallel_threads", -1) >= 0, "system_info parallel_threads")
    assert(dict_get(si, "parallel_min_work", -1) >= 0, "system_info parallel_min_work")
    assert(dict_get(si, "parallel_effective_threads", 0) >= 1, "system_info parallel_effective_threads")
    def psel = dict_get(si, "parallel_policy_selected", false)
    assert((psel == true || psel == false), "system_info parallel_policy_selected")
    assert(str_len(strip(dict_get(si, "parallel_reason", ""))) > 0, "system_info parallel_reason")

    print("✓ std.os.info tests passed")
}
