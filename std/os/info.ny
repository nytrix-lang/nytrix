;; Keywords: os info
;; OS system information (CPU, RAM, GPU).

module std.os.info (
   cpu_name, ram_short, gpu_name
)
use std.core *
use std.str *
use std.os *
use std.os.fs *

fn _read_text(path){
   "Internal helper to read a file and return its contents as a string."
   match file_read(path) {
      ok(s) -> { return s }
      err(_) -> { return "" }
   }
}

fn _after_colon(s){
   "Internal helper to extract a value from a 'Key: Value' string."
   if(!is_str(s)){ return "" }
   def n = str_len(s)
   mut i = 0
   while(i < n){
      if(load8(s, i) == 58){ ; ':'
         i = i + 1
         while(i < n && (load8(s, i) == 32 || load8(s, i) == 9)){ i = i + 1 }
         def out = malloc(n - i + 1)
         if(!out){ return "" }
         init_str(out, n - i)
         mut k = 0
         while(i + k < n){
            store8(out, load8(s, i + k), k)
            k = k + 1
         }
         store8(out, 0, n - i)
         return strip(out)
      }
      i = i + 1
   }
   strip(s)
}

fn _first_number(s){
   "Internal helper to extract the first integer found in a string."
   if(!is_str(s)){ return 0 }
   def n = str_len(s)
   mut i = 0
   while(i < n && (load8(s, i) < 48 || load8(s, i) > 57)){ i = i + 1 }
   mut v = 0
   while(i < n){
      def c = load8(s, i)
      if(c < 48 || c > 57){ break }
      v = v * 10 + (c - 48)
      i = i + 1
   }
   v
}

fn cpu_name(){
   "Returns the CPU model name.
   
   Reads from `/proc/cpuinfo` (Linux only)."
   def cpu = _read_text("/proc/cpuinfo")
   if(str_len(cpu) == 0){ return "unknown" }
   def lines = split(cpu, "\n")
   mut i = 0
   while(i < len(lines)){
      def ln = strip(get(lines, i, ""))
      if(startswith(ln, "model name")){ return _after_colon(ln) }
      if(startswith(ln, "Hardware")){ return _after_colon(ln) }
      i = i + 1
   }
   "unknown"
}

fn ram_short(){
   "Returns a summary string of system RAM usage (e.g., '2048/8192MB').
   
   Reads from `/proc/meminfo` (Linux only)."
   def mem = _read_text("/proc/meminfo")
   if(str_len(mem) == 0){ return "unknown" }
   def lines = split(mem, "\n")
   mut total_kb = 0
   mut avail_kb = 0
   mut i = 0
   while(i < len(lines)){
      def ln = strip(get(lines, i, ""))
      if(startswith(ln, "MemTotal:")){ total_kb = _first_number(ln) }
      elif(startswith(ln, "MemAvailable:")){ avail_kb = _first_number(ln) }
      i = i + 1
   }
   if(total_kb <= 0){ return "unknown" }
   if(avail_kb <= 0){ return to_str(total_kb / 1024) + "MB total" }
   def used_mb = (total_kb - avail_kb) / 1024
   def total_mb = total_kb / 1024
   to_str(used_mb) + "/" + to_str(total_mb) + "MB"
}

fn gpu_name(){
   "Returns the name or primary identifier of the system's GPU.
   
   Scans sysfs for DRM cards and PCI devices (Linux only)."
   ;; 2. Fallback: DRM sysfs (card0-card7)
   mut i = 0
   while(i < 8){
      def base = "/sys/class/drm/card" + to_str(i) + "/device/"
      def vendor = strip(_read_text(base + "vendor"))
      def dev = strip(_read_text(base + "device"))
      if(str_len(vendor) > 0 && str_len(dev) > 0){
         return "pci " + vendor + ":" + dev
      }
      i = i + 1
   }
   ;; 3. Fallback: PCI bus sysfs (scan /sys/bus/pci/devices)
   def pci_path = "/sys/bus/pci/devices"
   if(file_exists(pci_path)){
      def entries = list_dir(pci_path)
      mut j = 0
      while(j < len(entries)){
         def entry = get(entries, j, "")
         def class_file = pci_path + "/" + entry + "/class"
         if(file_exists(class_file)){
            def class_hex = strip(_read_text(class_file))
            ;; Display controller: 0x030000, 3D controller: 0x030200
            if(startswith(class_hex, "0x0300") || startswith(class_hex, "0x0302")){
               def vf = pci_path + "/" + entry + "/vendor"
               def df = pci_path + "/" + entry + "/device"
               if(file_exists(vf) && file_exists(df)){
                  def v = strip(_read_text(vf))
                  def d = strip(_read_text(df))
                  if(str_len(v) > 0 && str_len(d) > 0){
                     return "pci " + v + ":" + d
                  }
               }
            }
         }
         j = j + 1
      }
   }
   "unknown"
}
