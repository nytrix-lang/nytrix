;; Keywords: os time
;; Os Time module.

module std.os.time (
   time, sleep, msleep, ticks,
   _is_leap, _days_in_month, _days_in_year, _pad2, _pad4, format_time
)
use std.core *
use std.os.sys *
use std.text *

fn _is_leap(y){
   "Return true when `y` is a leap year."
   if((y % 4) != 0){ return 0 }
   if((y % 100) != 0){ return 1 }
   if((y % 400) != 0){ return 0 }
   1
}

fn _days_in_month(y, m){
   "Return days in month `m` for year `y`."
   match m {
      1 -> { return 31 }
      2 -> { return 28 + _is_leap(y) }
      3 -> { return 31 }
      4 -> { return 30 }
      5 -> { return 31 }
      6 -> { return 30 }
      7 -> { return 31 }
      8 -> { return 31 }
      9 -> { return 30 }
      10 -> { return 31 }
      11 -> { return 30 }
      12 -> { return 31 }
      _ -> { return 30 }
   }
}

fn _days_in_year(y){
   "Return days in year `y`."
   365 + _is_leap(y)
}

fn _pad2(n){
   "Zero-pad integer `n` to two digits."
   pad_start(to_str(n), 2, "0")
}

fn _pad4(n){
   "Zero-pad integer `n` to four digits."
   pad_start(to_str(n), 4, "0")
}

fn format_time(ts){
   "Format Unix seconds as `YYYY-MM-DD HH:MM:SS` in UTC."
   if(ts < 0){ ts = 0 }
   mut days = ts / 86400
   mut rem = ts - days * 86400
   def hour = rem / 3600
   rem = rem - hour * 3600
   def minute = rem / 60
   def second = rem - minute * 60

   mut year = 1970
   while(1){
      def diy = _days_in_year(year)
      if(days < diy){ break }
      days = days - diy
      year += 1
   }

   mut month = 1
   while(1){
      def dim = _days_in_month(year, month)
      if(days < dim){ break }
      days = days - dim
      month += 1
   }
   def day = days + 1
   f"{_pad4(year)}-{_pad2(month)}-{_pad2(day)} {_pad2(hour)}:{_pad2(minute)}:{_pad2(second)}"
}

fn time(){
   "Returns the current Unix timestamp (seconds since epoch) using `clock_gettime(CLOCK_REALTIME)`."
   def ts = malloc(16)
   def r = __clock_gettime(0, ts)
   if(r != 0){ free(ts) return 0 }
   def raw_sec = load64(ts)
   def res = from_int(raw_sec)
   free(ts)
   return res
}

fn msleep(ms){
   "Suspends execution of the current thread for `ms` milliseconds."
   def ts = malloc(16)
   store64(ts, to_int(ms / 1000), 0)
   store64(ts, to_int((ms % 1000) * 1000000), 8)
   __nanosleep(ts)
   free(ts)
}

fn sleep(s){
   "Suspends execution of the current thread for `s` seconds."
   msleep(s * 1000)
}

fn ticks(){
   "Returns a high-resolution monotonic tick count in nanoseconds. Useful for precise timing and benchmarking."
   def ts = malloc(16)
   def r = __clock_gettime(1, ts)
   if(r != 0){ free(ts) return 0 }
   def raw_sec = load64(ts, 0)
   def raw_nsec = load64(ts, 8)
   def sec = from_int(raw_sec)
   def nsec = from_int(raw_nsec)
   def res = sec * 1000000000 + nsec
   free(ts)
   return res
}

if(comptime{__main()}){
    use std.os.time *
    use std.core.error *

    print("Testing std.os.time...")

    def t1 = time()
    assert(t1 > 0, "time > 0")

    def start = ticks()
    msleep(120)
    def end = ticks()

    if(start > 0 && end > 0){
     if(end < start){
      print("Warning: ticks went backwards")
     }
    } else {
     print("Warning: ticks unavailable")
    }

    ticks()

    def fmt = format_time(0)
    assert((fmt == "1970-01-01 00:00:00"), "epoch")

    def fmt2 = format_time(1672531200)
    assert((fmt2 == "2023-01-01 00:00:00"), "2023")

    print("✓ std.os.time tests passed")
}
