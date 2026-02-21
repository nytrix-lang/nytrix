;; Keywords: strings timefmt
;; Strings Timefmt module.

module std.math.timefmt (
   _is_leap, _days_in_month, _days_in_year, _pad2, _pad4, format_time
)
use std.core *
use std.math *
use std.str *

fn _is_leap(y){
   "Return true if year y is a leap year."
   if((y % 4) != 0){ return 0 }
   if((y % 100) != 0){ return 1 }
   if((y % 400) != 0){ return 0 }
   return 1
}

fn _days_in_month(y, m){
   "Internal: days in month m for year y."
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
   "Internal: days in year y."
   return 365 + _is_leap(y)
}

fn _pad2(n){
   "Internal: zero-pad integer to 2 digits."
   return pad_start(to_str(n), 2, "0")
}

fn _pad4(n){
   "Internal: zero-pad integer to 4 digits."
   return pad_start(to_str(n), 4, "0")
}

fn format_time(ts){
   "Format unix seconds to YYYY-MM-DD HH:MM:SS (UTC)."
   if(ts < 0){ ts = 0  }
   mut days = ts / 86400
   mut rem = ts - days*86400
   def hour = rem / 3600
   rem = rem - hour*3600
   def minute = rem / 60
   def second = rem - minute*60
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
   return f"{_pad4(year)}-{_pad2(month)}-{_pad2(day)} {_pad2(hour)}:{_pad2(minute)}:{_pad2(second)}"
}

if(comptime{__main()}){
    use std.math.timefmt *
    use std.core.error *

    print("Testing timefmt...")

    def ts = 1673712000
    def formatted = format_time(ts)
    assert((formatted == "2023-01-14 16:00:00"), "format_time")

    print("✓ std.math.timefmt tests passed")

    use std.util.timefmt *
    use std.core *
    use std.math.timefmt *

    print("Testing timefmt...")

    def ts = 1673712000
    def formatted = format_time(ts)
    assert((formatted == "2023-01-14 16:00:00"), "format_time")

    print("✓ std.util.timefmt tests passed")
}
